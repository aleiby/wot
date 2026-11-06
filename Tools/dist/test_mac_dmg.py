#!/usr/bin/env python3
"""Specs for the guarded Mac first-install DMG builder and verifier."""
from __future__ import annotations

import os
import json
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dmg_layout  # noqa: E402
import mac_dmg  # noqa: E402
import release_formats as formats  # noqa: E402
import release_test_fixtures as fixtures  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"


def _copy_directory_contents(source: Path, destination: Path) -> None:
    for child in destination.iterdir():
        if child.is_symlink() or child.is_file():
            child.unlink()
        else:
            shutil.rmtree(child)
    for child in source.iterdir():
        target = destination / child.name
        if child.is_symlink():
            os.symlink(os.readlink(str(child)), str(target))
        elif child.is_dir():
            shutil.copytree(child, target, symlinks=True)
        else:
            shutil.copyfile(child, target)


class FakeMacCommands:
    def __init__(self, guarded_zip: Path, fail_codesign: bool = False,
                 attach_returncode: int = 0, extra_volume: bool = False,
                 detach_failures: int = 0):
        self.guarded_zip = guarded_zip
        self.fail_codesign = fail_codesign
        self.calls = []
        self.image_source = None
        self.created_source_names = None
        self.attach_returncode = attach_returncode
        self.extra_volume = extra_volume
        self.detach_failures = detach_failures
        self.mountpoint = None
        self.mount_readonly = False

    def __call__(self, command):
        command = list(command)
        self.calls.append(command)
        if command[0] == "ditto" and "-c" not in command:
            shutil.copytree(command[-2], command[-1], symlinks=True,
                            dirs_exist_ok=True)
        elif command[0] == "tiffutil":
            Path(command[-1]).write_bytes(b"combined retina background")
        elif command[:2] == ["hdiutil", "create"]:
            self.image_source = Path(command[command.index("-srcfolder") + 1])
            self.created_source_names = {
                entry.name for entry in os.scandir(self.image_source)}
            for name in (".fseventsd", ".Trashes", ".Spotlight-V100"):
                (self.image_source / name).mkdir()
            (self.image_source / ".DS_Store").write_bytes(b"created metadata")
            Path(command[-1]).write_bytes(b"mock UDRW image")
        elif command[:2] == ["hdiutil", "convert"]:
            Path(command[command.index("-o") + 1]).write_bytes(b"mock UDZO image")
        elif command[:2] == ["hdiutil", "attach"]:
            entities = [
                {"content-hint": "Apple_HFS",
                 "dev-entry": "/dev/disk42s1",
                 "potentially-mountable": True,
                 "unmapped-content-hint":
                     "48465300-0000-11AA-AA11-00306543ECAC",
                 "volume-kind": "hfs"},
                {"content-hint": "GUID_partition_scheme",
                 "dev-entry": "/dev/disk42",
                 "potentially-mountable": False,
                 "unmapped-content-hint": "GUID_partition_scheme"},
            ]
            if self.extra_volume:
                entities.append({"dev-entry": "/dev/disk42s2",
                                 "potentially-mountable": True})
            plist = plistlib.dumps({"system-entities": entities}).decode("utf-8")
            return subprocess.CompletedProcess(
                command, self.attach_returncode, plist,
                "partial attach" if self.attach_returncode else "")
        elif command[:2] == ["diskutil", "mount"]:
            mountpoint = Path(command[command.index("-mountPoint") + 1])
            self.mountpoint = mountpoint
            self.mount_readonly = "readOnly" in command
            _copy_directory_contents(self.image_source, mountpoint)
        elif command[0] == "codesign" and self.fail_codesign:
            return subprocess.CompletedProcess(command, 1, "", "bad signature")
        elif command[:3] == ["xcrun", "notarytool", "submit"]:
            return subprocess.CompletedProcess(
                command, 0, json.dumps({"id": "submission-1",
                                        "status": "Accepted"}), "")
        elif command[0] == "spctl":
            return subprocess.CompletedProcess(
                command, 0, "", f"{command[-1]}: accepted\n"
                "source=Notarized Developer ID\n")
        elif command[:3] == ["diskutil", "info", "-plist"]:
            plist = plistlib.dumps({"MountPoint": str(self.mountpoint),
                                    "VolumeName": "WoT Reborn"}).decode("utf-8")
            return subprocess.CompletedProcess(command, 0, plist, "")
        elif command[:2] == ["hdiutil", "detach"]:
            if self.detach_failures:
                self.detach_failures -= 1
                return subprocess.CompletedProcess(command, 1, "", "busy")
            if self.mountpoint is not None and self.mountpoint.exists():
                if not self.mount_readonly:
                    _copy_directory_contents(self.mountpoint, self.image_source)
                for child in list(self.mountpoint.iterdir()):
                    if child.is_symlink() or child.is_file():
                        child.unlink()
                    else:
                        shutil.rmtree(child)
        elif command[0] == "ditto" and "-c" in command:
            shutil.copyfile(self.guarded_zip, command[-1])
        return subprocess.CompletedProcess(command, 0, "", "")


class MacDmgUnitTests(unittest.TestCase):
    def setUp(self):
        local_config = mock.patch.object(
            mac_dmg, "local_value", return_value=None)
        local_config.start()
        self.addCleanup(local_config.stop)
        # The fake mount is an APFS temp directory, whose IDs do not fit the
        # HFS+ alias record (test_dmg_layout covers the real alias on HFS+).
        alias = mock.patch.object(dmg_layout, "_background_alias_bytes",
                                  return_value=b"alias bytes")
        alias.start()
        self.addCleanup(alias.stop)
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.app = self.root / "WoT.app"
        (self.app / "Contents" / "MacOS").mkdir(parents=True)
        resources = self.app / "Contents" / "Resources"
        resources.mkdir()
        executable = self.app / "Contents" / "MacOS" / "WoT-Mac-Shipping"
        executable.write_bytes(b"tiny mach-o fixture")
        executable.chmod(0o755)
        self.icon = resources / "WoT.icns"
        self.icon.write_bytes(b"fixture icon")
        info = plistlib.dumps({"CFBundleIconFile": "WoT"})
        (self.app / "Contents" / "Info.plist").write_bytes(info)
        self.zip = self.root / "guarded.zip"
        files = {
            "Contents/MacOS/WoT-Mac-Shipping":
                (b"tiny mach-o fixture", 0o100755),
            "Contents/Info.plist": (info, 0o100644),
            "Contents/Resources/WoT.icns": (b"fixture icon", 0o100644),
        }
        fixtures.write_guarded_zip(self.zip, "Mac", files=files)
        self.alias_patch = None
        if sys.platform != "darwin":
            dmg_layout.load_vendored_dmgbuild()
            from mac_alias import Alias
            self.alias_patch = mock.patch.object(
                Alias, "for_file",
                return_value=SimpleNamespace(to_bytes=lambda: b"alias bytes"))
            self.alias_patch.start()

    def tearDown(self):
        if self.alias_patch is not None:
            self.alias_patch.stop()
        self.temporary.cleanup()

    def _tree_root(self):
        return formats.project_guarded_archive_tree(self.zip, "Mac").tree_root

    def _valid_mount(self):
        mount = self.root / "mount"
        mount.mkdir()
        shutil.copytree(self.app, mount / dmg_layout.APP_NAME)
        os.symlink("/Applications", str(mount / "Applications"))
        (mount / ".background").mkdir()
        background = mount / ".background" / "background.tiff"
        background.write_bytes(b"verified background")
        (mount / ".VolumeIcon.icns").write_bytes(self.icon.read_bytes())
        (mount / ".DS_Store").write_bytes(b"finder records")
        return mount, formats.sha256_file(background)[0]

    def _verify_attached(self, mount, background_sha256, commands=None):
        try:
            mac_dmg.verify_attached_volume(
                mount, self._tree_root(), background_sha256,
                runner=commands or FakeMacCommands(self.zip), temporary=self.root)
        except TypeError as exc:
            self.fail(f"verify_attached_volume lacks the v2 interface: {exc}")

    def test_builder_uses_root_app_polished_layout_udzo_and_v2_sidecar(self):
        output = self.root / "WoT-Mac-guarded-shipping-abc123456.dmg"
        commands = FakeMacCommands(self.zip)
        document = mac_dmg.build_dmg(self.app, self.zip, output, runner=commands)
        self.assertEqual(document, mac_dmg.verify_sidecar(
            output, output.with_name(output.name + ".verify.json")))
        self.assertEqual(commands.created_source_names,
                         {dmg_layout.APP_NAME, "Applications"})
        self.assertEqual(document["format"], "wot-mac-dmg-v2")
        self.assertFalse(document["notarized"])
        self.assertEqual(document["background_sha256"],
                         formats.sha256_hex(b"combined retina background"))
        create = next(call for call in commands.calls
                      if call[:2] == ["hdiutil", "create"])
        self.assertEqual(create[create.index("-format") + 1], "UDRW")
        self.assertEqual(create[create.index("-fs") + 1], "HFS+")
        self.assertEqual(create[create.index("-volname") + 1], "WoT Reborn")
        convert = next(call for call in commands.calls
                       if call[:2] == ["hdiutil", "convert"])
        self.assertEqual(convert[convert.index("-format") + 1], "UDZO")
        self.assertTrue(any(call[0] == "tiffutil" for call in commands.calls))
        self.assertTrue(any(call[0] == "SetFile" for call in commands.calls))
        attach = next(call for call in commands.calls
                      if call[:2] == ["hdiutil", "attach"] and "-readonly" in call)
        self.assertIn("-nomount", attach)
        mounts = [call for call in commands.calls if call[:2] == ["diskutil", "mount"]]
        self.assertTrue(any("readOnly" not in call for call in mounts))
        self.assertTrue(any("readOnly" in call for call in mounts))

    def test_developer_id_notarizes_staples_and_round_trips_sidecar(self):
        output = self.root / "notarized.dmg"
        commands = FakeMacCommands(self.zip)

        def local_value(key, required=False):
            return {"mac_signing_identity": "Developer ID Application: Example",
                    "mac_notary_profile": "profile-placeholder"}.get(key)

        with mock.patch.object(mac_dmg, "local_value", side_effect=local_value):
            document = mac_dmg.build_dmg(
                self.app, self.zip, output, runner=commands)
        self.assertTrue(document["notarized"])
        self.assertTrue(mac_dmg.load_sidecar(
            output.with_name(output.name + ".verify.json"))["notarized"])
        sign_index = next(index for index, call in enumerate(commands.calls)
                          if call[:4] == ["codesign", "--force", "--timestamp",
                                          "--sign"])
        submit_index = next(index for index, call in enumerate(commands.calls)
                            if call[:3] == ["xcrun", "notarytool", "submit"])
        staple_index = next(index for index, call in enumerate(commands.calls)
                            if call[:3] == ["xcrun", "stapler", "staple"])
        self.assertLess(sign_index, submit_index)
        self.assertLess(submit_index, staple_index)

    def test_remove_created_metadata_leaves_only_source_allowlist(self):
        mount = self.root / "cleanup"
        (mount / dmg_layout.APP_NAME).mkdir(parents=True)
        os.symlink("/Applications", str(mount / "Applications"))
        (mount / ".DS_Store").write_bytes(b"stale")
        for name in (".fseventsd", ".Trashes", ".Spotlight-V100"):
            (mount / name).mkdir()
            (mount / name / "payload").write_bytes(b"metadata")
        mac_dmg._remove_created_metadata(mount)
        self.assertEqual({entry.name for entry in os.scandir(mount)},
                         {dmg_layout.APP_NAME, "Applications"})

    def test_attached_layout_accepts_exact_allowlist(self):
        mount, digest = self._valid_mount()
        self._verify_attached(mount, digest)

    def test_attached_layout_rejects_wrong_applications_alias_and_extra_root(self):
        mount, digest = self._valid_mount()
        (mount / "Applications").unlink()
        os.symlink("/Wrong", str(mount / "Applications"))
        with self.assertRaisesRegex(mac_dmg.DmgError, "Applications"):
            self._verify_attached(mount, digest)
        (mount / "Applications").unlink()
        os.symlink("/Applications", str(mount / "Applications"))
        (mount / "extra").write_bytes(b"x")
        with self.assertRaisesRegex(mac_dmg.DmgError, "unexpected.*extra"):
            self._verify_attached(mount, digest)

    def test_attached_layout_rejects_second_background_file(self):
        mount, digest = self._valid_mount()
        (mount / ".background" / "extra").write_bytes(b"x")
        with self.assertRaisesRegex(mac_dmg.DmgError, "background.*only"):
            self._verify_attached(mount, digest)

    def test_attached_layout_rejects_volume_icon_different_from_app_icon(self):
        mount, digest = self._valid_mount()
        (mount / ".VolumeIcon.icns").write_bytes(b"different")
        with self.assertRaisesRegex(mac_dmg.DmgError, "volume icon.*app icon"):
            self._verify_attached(mount, digest)

    def test_attached_layout_rejects_background_hash_different_from_sidecar(self):
        mount, _digest = self._valid_mount()
        with self.assertRaisesRegex(mac_dmg.DmgError, "background.*sidecar"):
            self._verify_attached(mount, "0" * 64)

    def test_multiple_app_icons_use_info_plist_selection(self):
        alternate = self.app / "Contents" / "Resources" / "Other.icns"
        alternate.write_bytes(b"other")
        self.assertEqual(mac_dmg._find_app_icon(self.app), self.icon)

    def test_codesign_failure_still_detaches_and_does_not_publish_image(self):
        output = self.root / "failed.dmg"
        commands = FakeMacCommands(self.zip, fail_codesign=True)
        with self.assertRaisesRegex(mac_dmg.DmgError, "codesign"):
            mac_dmg.build_dmg(self.app, self.zip, output, runner=commands)
        self.assertFalse(output.exists())
        self.assertTrue(any(call[:2] == ["hdiutil", "detach"]
                            for call in commands.calls))

    def test_partial_attach_is_detached_and_reports_both_failures(self):
        commands = FakeMacCommands(
            self.zip, attach_returncode=1, detach_failures=2)
        with self.assertRaisesRegex(
                mac_dmg.DmgError, "attach failed.*cleanup also failed.*forced"):
            mac_dmg.verify_dmg(self.root / "bad.dmg", "0" * 64, "1" * 64,
                               runner=commands)
        detach = [call for call in commands.calls
                  if call[:2] == ["hdiutil", "detach"]]
        self.assertEqual(detach, [
            ["hdiutil", "detach", "/dev/disk42"],
            ["hdiutil", "detach", "-force", "/dev/disk42"],
        ])

    def test_multi_volume_attach_is_refused_before_mount_and_detached(self):
        commands = FakeMacCommands(self.zip, extra_volume=True)
        with self.assertRaisesRegex(mac_dmg.DmgError, "builder-produced HFS\\+"):
            mac_dmg.verify_dmg(self.root / "multi.dmg", "0" * 64, "1" * 64,
                               runner=commands)
        self.assertFalse(any(call[:2] == ["diskutil", "mount"]
                             for call in commands.calls))
        self.assertIn(["hdiutil", "detach", "/dev/disk42"], commands.calls)

    def test_detach_retries_with_force(self):
        commands = FakeMacCommands(self.zip, detach_failures=1)
        commands.extra_volume = True
        with self.assertRaisesRegex(mac_dmg.DmgError, "builder-produced HFS\\+"):
            mac_dmg.verify_dmg(self.root / "busy.dmg", "0" * 64, "1" * 64,
                               runner=commands)
        self.assertIn(["hdiutil", "detach", "-force", "/dev/disk42"],
                      commands.calls)

    def test_real_hfs_plus_nomount_plist_has_the_only_accepted_topology(self):
        result = subprocess.CompletedProcess(
            ["hdiutil", "attach"], 0,
            (FIXTURES / "hfs-plus-attach-nomount.plist").read_text(
                encoding="utf-8"), "")
        whole, volume, error = mac_dmg._attach_topology(result)
        self.assertIsNone(error)
        self.assertEqual((whole, volume), ("/dev/disk6", "/dev/disk6s1"))

    def test_apfs_shaped_nomount_plist_is_refused_fail_closed(self):
        entities = [
            {"content-hint": "GUID_partition_scheme",
             "dev-entry": "/dev/disk8", "potentially-mountable": False,
             "unmapped-content-hint": "GUID_partition_scheme"},
            {"content-hint": "Apple_APFS", "dev-entry": "/dev/disk8s1",
             "potentially-mountable": False,
             "unmapped-content-hint":
                 "7C3457EF-0000-11AA-AA11-00306543ECAC"},
            {"content-hint": "Apple_APFS", "dev-entry": "/dev/disk9",
             "potentially-mountable": False, "unmapped-content-hint": "Apple_APFS"},
            {"content-hint": "Apple_APFS", "dev-entry": "/dev/disk9s1",
             "potentially-mountable": True, "volume-kind": "apfs"},
        ]
        result = subprocess.CompletedProcess(
            ["hdiutil", "attach"], 0,
            plistlib.dumps({"system-entities": entities}).decode("utf-8"), "")
        whole, volume, error = mac_dmg._attach_topology(result)
        self.assertEqual(whole, "/dev/disk8")
        self.assertIsNone(volume)
        self.assertRegex(str(error), "only builder-produced HFS\\+ images")

    def test_detach_failure_after_successful_inspection_is_reported(self):
        source, background_digest = self._valid_mount()
        commands = FakeMacCommands(self.zip, detach_failures=2)
        commands.image_source = source
        with self.assertRaisesRegex(
                mac_dmg.DmgError, "DMG detach failed.*forced detach failed"):
            mac_dmg.verify_dmg(self.root / "busy.dmg", self._tree_root(),
                               background_digest, runner=commands)

    def test_notarized_verification_validates_ticket_and_gatekeeper(self):
        source, background_digest = self._valid_mount()
        commands = FakeMacCommands(self.zip)
        commands.image_source = source
        with mock.patch.object(mac_dmg.sys, "platform", "darwin"):
            mac_dmg.verify_dmg(
                self.root / "notarized.dmg", self._tree_root(),
                background_digest, notarized=True, runner=commands)
        self.assertEqual(commands.calls[0], [
            "xcrun", "stapler", "validate", str(self.root / "notarized.dmg")])
        self.assertTrue(any(call[0] == "spctl" for call in commands.calls))


@unittest.skipUnless(sys.platform == "darwin", "real hdiutil/codesign test is macOS-only")
class MacDmgIntegrationTests(unittest.TestCase):
    def test_real_builder_attaches_signed_app_and_preserves_tree_root(self):
        with tempfile.TemporaryDirectory(prefix="wot-real-dmg-test-") as temporary:
            root = Path(temporary)
            app = root / "WoT.app"
            macos = app / "Contents" / "MacOS"
            resources = app / "Contents" / "Resources"
            macos.mkdir(parents=True)
            resources.mkdir()
            executable = macos / "WoT-Mac-Shipping"
            executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            executable.chmod(0o755)
            (resources / "WoT.icns").write_bytes(b"synthetic icon")
            (app / "Contents" / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleExecutable": "WoT-Mac-Shipping",
                "CFBundleIconFile": "WoT",
                "CFBundleIdentifier": "com.aleiby.wot.dmgtest",
                "CFBundleName": "WoT",
                "CFBundlePackageType": "APPL",
            }))
            subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(app)],
                           check=True, capture_output=True, text=True)
            guarded_zip = root / "guarded.zip"
            subprocess.run(["ditto", "-c", "-k", "--norsrc", "--noextattr",
                            "--keepParent", str(app), str(guarded_zip)],
                           check=True, capture_output=True, text=True)
            output = root / "WoT-Mac-guarded-shipping-abc123456.dmg"
            sidecar = mac_dmg.build_dmg(app, guarded_zip, output)
            self.assertEqual(sidecar["installed_tree_root"],
                             formats.project_guarded_archive_tree(
                                 guarded_zip, "Mac").tree_root)
            mac_dmg.verify_dmg(output, sidecar["installed_tree_root"],
                               sidecar["background_sha256"])


if __name__ == "__main__":
    unittest.main()
