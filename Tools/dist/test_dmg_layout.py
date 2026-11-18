#!/usr/bin/env python3
"""Specs for the vendored, headless Finder layout writer."""
from __future__ import annotations

import importlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


def _layout_module():
    try:
        return importlib.import_module("dmg_layout")
    except ModuleNotFoundError:
        raise AssertionError("dmg_layout module is missing")


class DmgLayoutTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_tampered_wheel_is_rejected_before_import(self):
        layout = _layout_module()
        vendor = self.root / "vendor"
        shutil.copytree(layout.VENDOR_DIR, vendor)
        wheel = vendor / "dmgbuild-1.6.5-py3-none-any.whl"
        wheel.write_bytes(wheel.read_bytes() + b"tampered")
        with mock.patch.object(layout, "VENDOR_DIR", vendor):
            with self.assertRaisesRegex(layout.LayoutError, "sha256"):
                layout.load_vendored_dmgbuild()

    def test_write_layout_creates_assets_and_finder_records(self):
        layout = _layout_module()
        layout.load_vendored_dmgbuild()
        from ds_store import DSStore
        from mac_alias import Alias

        mountpoint = self.root / "mount"
        mountpoint.mkdir()
        background = self.root / "source-background.tiff"
        background.write_bytes(b"background bytes")
        volume_icon = self.root / "source-icon.icns"
        volume_icon.write_bytes(b"icon bytes")
        calls = []

        def runner(command):
            calls.append(list(command))
            return subprocess.CompletedProcess(command, 0, "", "")

        aliased = []

        def alias_bytes(target):
            aliased.append(Path(target))
            return b"alias bytes"

        layout.write_layout(mountpoint, background=background,
                            volume_icon=volume_icon, runner=runner,
                            alias_bytes=alias_bytes)
        self.assertEqual(aliased, [mountpoint / ".background" / "background.tiff"])

        self.assertEqual({entry.name for entry in os.scandir(mountpoint)}, {
            ".DS_Store", ".background", ".VolumeIcon.icns",
        })
        self.assertEqual(
            {entry.name for entry in os.scandir(mountpoint / ".background")},
            {"background.tiff"})
        self.assertEqual((mountpoint / ".background" / "background.tiff").read_bytes(),
                         b"background bytes")
        self.assertEqual((mountpoint / ".VolumeIcon.icns").read_bytes(), b"icon bytes")
        self.assertEqual(calls, [["SetFile", "-a", "C", str(mountpoint)]])
        with DSStore.open(str(mountpoint / ".DS_Store"), "r") as store:
            self.assertEqual(store[layout.APP_NAME]["Iloc"],
                             layout.ICON_POSITIONS[layout.APP_NAME])
            self.assertEqual(store["Applications"]["Iloc"],
                             layout.ICON_POSITIONS["Applications"])
            self.assertEqual(store["."]["vSrn"], (b"long", 1))
            self.assertEqual(store["."]["icvl"], (b"type", b"icnv"))
            bounds = store["."]["bwsp"]
            self.assertEqual(bounds["WindowBounds"],
                             "{{100, 100}, {660, 400}}")
            self.assertFalse(bounds["ShowToolbar"])
            self.assertFalse(bounds["ShowSidebar"])
            self.assertFalse(bounds["ShowStatusBar"])
            self.assertFalse(bounds["ShowPathbar"])
            icon_view = store["."]["icvp"]
            self.assertEqual(icon_view["iconSize"], 128.0)
            self.assertEqual(icon_view["textSize"], 13.0)
            self.assertEqual(icon_view["arrangeBy"], "none")
            self.assertTrue(icon_view["labelOnBottom"])
            self.assertEqual(icon_view["backgroundType"], 2)

    def test_missing_setfile_falls_back_to_finder_info_xattr(self):
        layout = _layout_module()
        layout.load_vendored_dmgbuild()
        from mac_alias import Alias

        mountpoint = self.root / "mount"
        mountpoint.mkdir()
        background = self.root / "background.tiff"
        background.write_bytes(b"background")
        volume_icon = self.root / "icon.icns"
        volume_icon.write_bytes(b"icon")
        calls = []

        def runner(command):
            command = list(command)
            calls.append(command)
            if command[0] == "SetFile":
                raise FileNotFoundError("SetFile")
            return subprocess.CompletedProcess(command, 0, "", "")

        with mock.patch.object(
                Alias, "for_file",
                return_value=SimpleNamespace(to_bytes=lambda: b"alias bytes")):
            layout.write_layout(mountpoint, background=background,
                                volume_icon=volume_icon, runner=runner)
        self.assertEqual(calls[0], ["SetFile", "-a", "C", str(mountpoint)])
        self.assertEqual(calls[1][0:3], ["xattr", "-wx", "com.apple.FinderInfo"])
        self.assertEqual(calls[1][-1], str(mountpoint))
        finder_info = bytes.fromhex(calls[1][3])
        self.assertEqual(len(finder_info), 32)
        self.assertEqual(finder_info[8:10], b"\x04\x00")

    @unittest.skipUnless(sys.platform == "darwin", "alias resolution is macOS-only")
    def test_background_alias_targets_the_copied_background(self):
        layout = _layout_module()
        layout.load_vendored_dmgbuild()
        from ds_store import DSStore
        from mac_alias import Alias

        # The real alias needs 32-bit catalog IDs, i.e. an HFS+ volume like the
        # one the builder writes into; an APFS temp directory cannot hold it.
        image = self.root / "alias-test.dmg"
        mountpoint = self.root / "mount"
        mountpoint.mkdir()
        subprocess.run(["hdiutil", "create", "-size", "4m", "-fs", "HFS+",
                        "-volname", "WoTAliasTest", str(image)],
                       check=True, capture_output=True)
        subprocess.run(["hdiutil", "attach", "-nobrowse", "-noautoopen",
                        "-mountpoint", str(mountpoint), str(image)],
                       check=True, capture_output=True)
        try:
            background = self.root / "background.tiff"
            background.write_bytes(b"background")
            volume_icon = self.root / "icon.icns"
            volume_icon.write_bytes(b"icon")

            def runner(command):
                return subprocess.CompletedProcess(command, 0, "", "")

            layout.write_layout(mountpoint, background=background,
                                volume_icon=volume_icon, runner=runner)
            with DSStore.open(str(mountpoint / ".DS_Store"), "r") as store:
                alias = Alias.from_bytes(store["."]["icvp"]["backgroundImageAlias"])
            expected = mountpoint / ".background" / "background.tiff"
            self.assertEqual(os.fsdecode(alias.target.filename), "background.tiff")
            self.assertEqual(os.fsdecode(alias.volume.name), "WoTAliasTest")
            self.assertEqual(alias.target.cnid, os.stat(str(expected)).st_ino)
        finally:
            subprocess.run(["hdiutil", "detach", str(mountpoint)],
                           capture_output=True)


if __name__ == "__main__":
    unittest.main()
