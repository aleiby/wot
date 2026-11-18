#!/usr/bin/env python3
"""Specs for verify_release.py: every refusal a bad publish set must hit.

Several cases RE-SIGN a mutated object with the fixture's temp keys, so the
refusal is proven to come from the semantic check and not merely from a broken
signature.
"""
from __future__ import annotations

import contextlib
import dataclasses
import io
import json
import os
import shutil
import sys
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import release_test_fixtures as fixtures  # noqa: E402
import verify_release  # noqa: E402

# Stands in for Install-WoT-Reborn.desktop and for its rebuild at the build
# commit: the fixture commit is not in any repository, so these specs patch
# the rebuild (test_steamos_installer_asset.py runs the real one).
STEAMOS_BYTES = b"[Desktop Entry]\nName=Install WoT Reborn\n# spec stand-in\n"


def replace_zip_member(archive_path: Path, member: str, data: bytes) -> None:
    replacement_path = archive_path.with_suffix(archive_path.suffix + ".replacement")
    with zipfile.ZipFile(archive_path) as source, zipfile.ZipFile(
            replacement_path, "w") as replacement:
        for info in source.infolist():
            contents = source.read(info)
            if info.filename.rstrip("/") == member:
                contents = data
            replacement.writestr(info, contents)
    os.replace(replacement_path, archive_path)


class VerifyReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dmg_inspection_patcher = mock.patch.object(
            verify_release.mac_dmg, "verify_dmg")
        cls.dmg_inspection = cls.dmg_inspection_patcher.start()
        cls.steamos_patcher = mock.patch.object(
            verify_release.steamos_installer, "build_at_commit",
            return_value=STEAMOS_BYTES)
        cls.steamos_rebuild = cls.steamos_patcher.start()
        # Whether the build commit carries the installer: yes, unless a spec says no.
        cls.steamos_source_patcher = mock.patch.object(
            verify_release.steamos_installer, "has_source", return_value=True)
        cls.steamos_source = cls.steamos_source_patcher.start()
        cls.fixture = fixtures.ReleaseFixture()
        cls.pristine = cls.fixture.root / "pristine"
        cls.fixture.sign(cls.pristine)
        cls.counter = 0

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()
        cls.dmg_inspection_patcher.stop()
        cls.steamos_patcher.stop()
        cls.steamos_source_patcher.stop()

    def setUp(self):
        self.steamos_rebuild.reset_mock(side_effect=True)
        self.steamos_rebuild.return_value = STEAMOS_BYTES
        self.steamos_source.reset_mock(side_effect=True)
        self.steamos_source.return_value = True

    @staticmethod
    def add_steamos_installer(directory, data=STEAMOS_BYTES):
        (directory / formats.STEAMOS_INSTALLER_ASSET).write_bytes(data)

    def copy(self):
        type(self).counter += 1
        return fixtures.copy_set(self.pristine, self.fixture.root / f"case-{self.counter}")

    def test_advisory_funnel_does_not_add_refusals_or_false_proofs(self):
        for receipts in ({}, {"Win64": self.fixture.root / "absent.json"},
                         {"Linux": self.fixture.root / "absent.json"}):
            with self.subTest(receipts=receipts), contextlib.redirect_stderr(io.StringIO()) as errors:
                failures = []
                gate = verify_release._verify_update_sequence_gate(
                    1, self.fixture.releases, receipts, False, failures)
                self.assertEqual(failures, [])
                self.assertEqual(gate, {"mode": "unproven", "platforms": {}})
                self.assertIn("WARNING", errors.getvalue())
                self.assertIn("Win64", errors.getvalue())
                self.assertIn("Mac", errors.getvalue())

    def test_advisory_funnel_contains_nested_schema_exceptions(self):
        for exception in (KeyError("blobs"), TypeError("bad nesting"),
                          ValueError("bad value")):
            with self.subTest(exception=type(exception).__name__), \
                    mock.patch.object(
                        verify_release.formats, "validate_funnel_receipt",
                        side_effect=exception), \
                    contextlib.redirect_stderr(io.StringIO()) as errors:
                failures = []
                gate = verify_release._verify_update_sequence_gate(
                    1, self.fixture.releases, self.fixture.funnel_receipts,
                    False, failures)
                self.assertEqual(failures, [])
                self.assertEqual(gate, {"mode": "unproven", "platforms": {}})
                self.assertIn("no validated funnel proof", errors.getvalue())

    def test_unknown_funnel_platform_mapping_warns(self):
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            paths = verify_release.parse_platform_paths(
                ["Linux=receipt.json", "Win64=win.json"], "funnel-receipt")
        self.assertEqual(paths, {"Win64": Path("win.json")})
        self.assertIn("Linux", errors.getvalue())
        self.assertIn("WARNING", errors.getvalue())

    def verify(self, directory, **options):
        if "releases" not in options:
            options["releases"] = {"Mac": self.fixture.releases["Mac"]}
        return verify_release.verify_publish_set(
            directory, trust=self.fixture.trust, compiled=self.fixture.compiled, **options)

    def assertFails(self, directory, fragment, **options):
        with self.assertRaises(verify_release.VerifyError) as caught:
            self.verify(directory, **options)
        joined = "\n".join(caught.exception.failures)
        self.assertIn(fragment, joined)
        return caught.exception.failures

    def resign_announcement(self, directory, platform, mutate):
        name = formats.announcement_asset(platform)
        document = json.loads((directory / name).read_bytes())
        mutate(document)
        raw = json.dumps(document, sort_keys=True, separators=(",", ":")).encode()
        (directory / name).write_bytes(raw)
        (directory / (name + ".sig")).write_bytes(crypto.sign_framed(
            self.fixture.openssl, self.fixture.announcement_key,
            crypto.CONTEXT_ANNOUNCEMENT_V1, raw, self.fixture.announcement_public))

    def resign_manifest(self, directory, platform, mutate):
        name = formats.manifest_asset(platform)
        document = json.loads((directory / name).read_bytes())
        mutate(document)
        raw = json.dumps(document, sort_keys=True, separators=(",", ":")).encode()
        (directory / name).write_bytes(raw)
        (directory / (name + ".sig")).write_bytes(crypto.sign_framed(
            self.fixture.openssl, self.fixture.release_key,
            crypto.CONTEXT_RELEASE_MANIFEST_V2, raw, self.fixture.release_public))

        def point(announcement):
            announcement["manifest"]["sha256"] = formats.sha256_hex(raw)
            announcement["manifest"]["size"] = len(raw)
        self.resign_announcement(directory, platform, point)

    def test_pristine_set_passes(self):
        verified = self.verify(self.pristine, releases=dict(self.fixture.releases))
        self.assertEqual((verified.tag, verified.release_sequence, verified.ring_signer),
                         ("v0.1.0", 0, "offline-root-a"))
        self.assertEqual(sorted(verified.platforms), ["Mac", "Win64"])

    def test_unnotarized_shipping_set_requires_override(self):
        mac = fixtures.write_guarded_release(
            self.fixture.root / "verify-unnotarized-mac", "Mac",
            notarized=False)
        releases = {"Win64": self.fixture.releases["Win64"], "Mac": mac}
        funnels = dict(self.fixture.funnel_receipts)
        funnels["Mac"] = fixtures.write_funnel_receipt(
            self.fixture.root / "verify-unnotarized-funnel.json", mac)
        directory = self.fixture.root / "verify-unnotarized-set"
        self.fixture.sign(
            directory, release_jsons=releases, funnel_receipts=funnels,
            allow_unnotarized=True)
        self.assertFails(
            directory, "--allow-unnotarized", releases=releases)
        verified = self.verify(
            directory, releases=releases, allow_unnotarized=True)
        self.assertFalse(verified.platforms["Mac"].dmg_notarized)

    def test_signature_tampering(self):
        directory = self.copy()
        name = formats.announcement_asset("Win64")
        raw = bytearray((directory / name).read_bytes())
        raw[-2] = ord("1") if raw[-2] != ord("1") else ord("2")
        (directory / name).write_bytes(bytes(raw))
        self.assertFails(directory, "does not verify under the embedded announcement key")
        directory = self.copy()
        (directory / (formats.manifest_asset("Mac") + ".sig")).write_bytes(b"\0" * 63)
        self.assertFails(directory, "exactly 64")
        directory = self.copy()
        ring = directory / (formats.KEY_RING_ASSET + ".sig")
        ring.write_bytes(bytes(64))
        self.assertFails(directory, "not signed by any embedded offline root")

    def test_manifest_signed_by_the_wrong_role(self):
        directory = self.copy()
        name = formats.manifest_asset("Win64")
        (directory / (name + ".sig")).write_bytes(crypto.sign_framed(
            self.fixture.openssl, self.fixture.announcement_key,
            crypto.CONTEXT_RELEASE_MANIFEST_V2, (directory / name).read_bytes(),
            self.fixture.announcement_public))
        self.assertFails(directory, "does not verify under release key")

    def test_latest_urls_are_refused_in_signed_references(self):
        directory = self.copy()

        def latest(document):
            document["key_ring"]["url"] = ("https://github.com/aleiby/wot/releases/latest/"
                                           "download/" + formats.KEY_RING_ASSET)
        self.resign_announcement(directory, "Mac", latest)
        self.assertFails(directory, "not the immutable per-tag download URL")

    def test_sequence_epoch_and_gate_rules(self):
        directory = self.copy()
        self.resign_announcement(directory, "Win64",
                                 lambda document: document.update(key_ring_epoch=2))
        self.assertFails(directory, "key_ring_epoch 2 != ring epoch 1")
        directory = self.copy()
        self.resign_announcement(directory, "Mac",
                                 lambda document: document.update(minimum_gate_protocol=9))
        self.assertFails(directory, "StayBlocked")
        directory = self.copy()
        for platform in formats.PLATFORMS:
            self.resign_announcement(directory, platform,
                                     lambda document: document.update(release_sequence=3))
        self.assertFails(directory, "release manifest sequence does not match")

    def test_platforms_must_agree_and_both_be_present(self):
        directory = self.copy()
        for suffix in ("", ".sig"):
            (directory / (formats.announcement_asset("Mac") + suffix)).unlink()
        self.assertFails(directory, "missing asset WoT-Mac-announcement-v1.json")
        directory = self.copy()
        self.resign_manifest(directory, "Mac",
                             lambda document: document.update(human_version="0.2.0"))
        self.assertFails(directory, "platforms disagree on manifest human_version")

    def test_signed_set_refuses_equal_empty_manifest_and_archive_versions(self):
        releases = {
            platform: fixtures.write_guarded_release(
                self.fixture.root / f"empty-version-{platform}", platform,
                human_version="")
            for platform in formats.PLATFORMS
        }
        funnels = {
            platform: fixtures.write_funnel_receipt(
                self.fixture.root / f"empty-version-{platform}-funnel.json",
                release)
            for platform, release in releases.items()
        }
        directory = self.fixture.root / "empty-version-set"
        with mock.patch.object(
                formats, "validate_human_version", return_value=""):
            self.fixture.sign(
                directory, release_jsons=releases,
                funnel_receipts=funnels, human_version="")
        self.assertFails(
            directory, "human_version: must start with a digit",
            releases=releases)

    def test_archive_and_tree_integrity(self):
        directory = self.copy()
        archive = directory / formats.update_archive_asset("Win64", 0, fixtures.COMMIT)
        data = bytearray(archive.read_bytes())
        data[40] ^= 0xFF
        archive.write_bytes(bytes(data))
        self.assertFails(directory, "sha256/size differ")
        directory = self.copy()
        self.resign_manifest(directory, "Win64", lambda document: document[
            "main_executable"].update(name="WoT/Binaries/Win64/Other.exe"))
        self.assertFails(directory, "does not match a file in the closed tree")
        directory = self.copy()
        self.resign_manifest(directory, "Mac", lambda document: document[
            "installed_tree_root"].update(sha256="0" * 64))
        self.assertFails(directory, "rehashed tree root differs")

    def test_publish_set_contents_are_closed(self):
        directory = self.copy()
        (directory / "notes.txt").write_bytes(b"x")
        self.assertFails(directory, "unexpected file notes.txt")
        directory = self.copy()
        (directory / formats.RELEASE_NOTES_ASSET).write_bytes(
            b'{"format":"wot-release-notes-v1","notes":[]}')
        self.assertFails(
            directory, f"unexpected file {formats.RELEASE_NOTES_ASSET}")
        directory = self.copy()
        install = directory / f"WoT-Win64-guarded-shipping-{fixtures.COMMIT}.zip"
        with zipfile.ZipFile(install, "a") as archive:
            archive.writestr("WoT Reborn/Game/extra.dll", b"not signed")
        self.assertFails(directory, "'extra.dll' is not in the signed tree")

    def test_a_published_set_requires_and_validates_its_release_notes(self):
        notes = formats.RELEASE_NOTES_ASSET
        directory = self.copy()
        # Sequence 0 publishes an empty cumulative asset (merge_release_notes).
        (directory / notes).write_bytes(formats.merge_release_notes(None, 0, ""))
        self.add_steamos_installer(directory)
        verified = self.verify(directory, published=True)
        self.assertEqual(verified.release_notes["entry_sequences"], [])
        self.assertNotIn(notes, verified.assets)
        self.assertEqual(len(verified.assets), len(self.verify(self.pristine).assets))
        self.assertEqual(verified.summary()["release_notes"]["entry_sequences"], [])
        self.assertNotIn("release_notes", self.verify(self.pristine).summary())
        # A signing set stays closed: the notes asset is refused there.
        self.assertFails(directory, f"unexpected file {notes}")
        # Published mode still refuses anything else.
        (directory / "stray.txt").write_bytes(b"x")
        self.assertFails(directory, "unexpected file stray.txt", published=True)

        directory = self.copy()
        self.add_steamos_installer(directory)
        self.assertFails(directory, f"a published release carries {notes}, but the "
                                    "downloaded set has none", published=True)
        (directory / notes).write_bytes(b'{"format":"wrong","notes":[]}')
        self.assertFails(directory, ".format is not", published=True)
        (directory / notes).write_bytes(formats.merge_release_notes(None, 2, "Later."))
        self.assertFails(directory, "entries for later releases [2]", published=True)

    def test_published_player_downloads_are_optional_identical_copies(self):
        directory = self.copy()
        (directory / formats.RELEASE_NOTES_ASSET).write_bytes(
            formats.merge_release_notes(None, 0, ""))
        sources = {
            formats.PLAYER_DOWNLOAD_ASSETS["Win64"]:
                formats.guarded_install_zip_asset(
                    "Win64", fixtures.COMMIT, formats.SHIPPING_CONFIGURATION),
            formats.PLAYER_DOWNLOAD_ASSETS["Mac"]:
                formats.guarded_install_dmg_asset(
                    fixtures.COMMIT, formats.SHIPPING_CONFIGURATION),
        }
        for fixed_name, source_name in sources.items():
            shutil.copyfile(directory / source_name, directory / fixed_name)
        self.add_steamos_installer(directory)

        verified = self.verify(directory, published=True)
        expected = {
            fixed_name: {
                "source": source_name,
                "sha256": formats.sha256_file(directory / source_name)[0],
                "size": (directory / source_name).stat().st_size,
            }
            for fixed_name, source_name in sources.items()
        }
        self.assertEqual(verified.player_downloads, expected)
        self.assertEqual(verified.summary()["player_downloads"], expected)
        self.assertTrue(set(sources).isdisjoint(verified.assets))
        self.assertFails(
            directory, f"unexpected file {formats.PLAYER_DOWNLOAD_ASSETS['Mac']}")

    def test_published_player_download_must_match_its_source(self):
        directory = self.copy()
        (directory / formats.RELEASE_NOTES_ASSET).write_bytes(
            formats.merge_release_notes(None, 0, ""))
        fixed_name = formats.PLAYER_DOWNLOAD_ASSETS["Win64"]
        source_name = formats.guarded_install_zip_asset(
            "Win64", fixtures.COMMIT, formats.SHIPPING_CONFIGURATION)
        (directory / fixed_name).write_bytes(
            (directory / source_name).read_bytes() + b"different")
        self.add_steamos_installer(directory)
        self.assertFails(
            directory, f"{fixed_name}: differs from {source_name}", published=True)

    def test_published_player_download_requires_its_source_in_the_set(self):
        directory = self.copy()
        (directory / formats.RELEASE_NOTES_ASSET).write_bytes(
            formats.merge_release_notes(None, 0, ""))
        fixed_name = formats.PLAYER_DOWNLOAD_ASSETS["Win64"]
        source_name = formats.guarded_install_zip_asset(
            "Win64", fixtures.COMMIT, formats.SHIPPING_CONFIGURATION)
        shutil.copyfile(directory / source_name, directory / fixed_name)
        (directory / source_name).unlink()
        self.add_steamos_installer(directory)
        self.assertFails(
            directory, f"{fixed_name}: has no commit-named source in the set",
            published=True)

    def test_published_notes_must_carry_this_releases_entry(self):
        path = self.fixture.root / "published-notes.json"
        path.write_bytes(formats.merge_release_notes(
            formats.merge_release_notes(None, 1, "First update."), 2, "Second update."))
        failures = []
        result = verify_release._check_published_release_notes(path, 2, failures)
        self.assertEqual((failures, result["entry_sequences"]), ([], [1, 2]))
        failures = []
        verify_release._check_published_release_notes(path, 3, failures)
        self.assertEqual(failures, [
            f"{formats.RELEASE_NOTES_ASSET} has no entry for this release's sequence 3"])

    def test_cli_published_flag_accepts_the_notes_asset(self):
        directory = self.copy()
        (directory / formats.RELEASE_NOTES_ASSET).write_bytes(
            formats.merge_release_notes(None, 0, ""))
        self.add_steamos_installer(directory)
        arguments = ["--set", str(directory), "--release",
                     f"Mac={self.fixture.releases['Mac']}", *self.fixture.trust_arguments()]
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(verify_release.main(arguments), 1)
        self.assertIn(f"unexpected file {formats.RELEASE_NOTES_ASSET}", errors.getvalue())
        self.assertIn(f"unexpected file {formats.STEAMOS_INSTALLER_ASSET}",
                      errors.getvalue())
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(verify_release.main(arguments + ["--published"]), 0)
        self.assertIn(f"{formats.RELEASE_NOTES_ASSET} is valid", out.getvalue())
        self.assertIn(f"{formats.STEAMOS_INSTALLER_ASSET} matches a rebuild at build "
                      f"commit {fixtures.COMMIT}", out.getvalue())
        self.assertIn("verify_release: PASS", out.getvalue())
        self.steamos_rebuild.assert_called_with(formats.REPOSITORY_ROOT, fixtures.COMMIT)
        elsewhere = self.fixture.root / "another-clone"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(verify_release.main(
                arguments + ["--published", "--repository-root", str(elsewhere)]), 0)
        self.steamos_rebuild.assert_called_with(elsewhere, fixtures.COMMIT)

    def test_an_unpublished_set_refuses_the_steamos_installer(self):
        directory = self.copy()
        self.add_steamos_installer(directory)
        self.assertFails(directory, f"unexpected file {formats.STEAMOS_INSTALLER_ASSET}")
        # The signing set (sign_release's own re-verification) is closed too.
        with self.assertRaises(verify_release.VerifyError) as caught:
            verify_release._verify_signing_output(
                directory, trust=self.fixture.trust, compiled=self.fixture.compiled,
                releases={"Mac": self.fixture.releases["Mac"]}, funnel_receipts={},
                allow_staging_sequence=False, allow_test=False)
        self.assertIn(f"unexpected file {formats.STEAMOS_INSTALLER_ASSET}",
                      "\n".join(caught.exception.failures))
        self.steamos_rebuild.assert_not_called()
        self.steamos_source.assert_not_called()
        self.assertNotIn(formats.STEAMOS_INSTALLER_ASSET,
                         {path.name for path in self.pristine.iterdir()})

    def test_a_release_built_before_the_installer_must_not_carry_it(self):
        self.steamos_source.return_value = False
        directory = self.copy()
        (directory / formats.RELEASE_NOTES_ASSET).write_bytes(
            formats.merge_release_notes(None, 0, ""))
        repository = self.fixture.root / "repository"
        # Older releases stay verifiable: no asset, no failure, nothing rebuilt.
        verified = self.verify(directory, published=True, repository_root=repository)
        self.steamos_source.assert_called_once_with(repository, fixtures.COMMIT)
        self.steamos_rebuild.assert_not_called()
        self.assertIsNone(verified.steamos_installer)
        self.assertNotIn("steamos_installer", verified.summary())
        # Present at such a commit, it is an unexpected file.
        self.add_steamos_installer(directory)
        failures = self.assertFails(
            directory, f"unexpected file {formats.STEAMOS_INSTALLER_ASSET} in the publish "
                       f"set: build commit {fixtures.COMMIT} predates the SteamOS installer",
            published=True, repository_root=repository)
        self.assertEqual(len(failures), 1, failures)
        self.steamos_rebuild.assert_not_called()

    def test_an_unknown_build_commit_is_a_failure_not_an_older_release(self):
        self.steamos_source.side_effect = verify_release.steamos_installer \
            .SteamOSInstallerError(f"git ls-tree failed: Not a valid object name "
                                   f"{fixtures.COMMIT}")
        directory = self.copy()
        (directory / formats.RELEASE_NOTES_ASSET).write_bytes(
            formats.merge_release_notes(None, 0, ""))
        for present in (False, True):
            with self.subTest(present=present):
                if present:
                    self.add_steamos_installer(directory)
                self.assertFails(
                    directory, f"{formats.STEAMOS_INSTALLER_ASSET}: cannot tell whether "
                               f"build commit {fixtures.COMMIT}", published=True)
        self.steamos_rebuild.assert_not_called()

    def test_a_published_set_requires_the_steamos_installer(self):
        directory = self.copy()
        (directory / formats.RELEASE_NOTES_ASSET).write_bytes(
            formats.merge_release_notes(None, 0, ""))
        failures = self.assertFails(
            directory, f"a published release carries {formats.STEAMOS_INSTALLER_ASSET}, "
                       "but the downloaded set has none", published=True)
        self.assertEqual(len(failures), 1, failures)
        # Required because the build commit carries the installer, checked in git.
        self.steamos_source.assert_called_once_with(formats.REPOSITORY_ROOT, fixtures.COMMIT)
        self.steamos_rebuild.assert_not_called()

    def test_a_published_steamos_installer_must_match_a_rebuild_at_the_build_commit(self):
        directory = self.copy()
        (directory / formats.RELEASE_NOTES_ASSET).write_bytes(
            formats.merge_release_notes(None, 0, ""))
        self.add_steamos_installer(directory)
        repository = self.fixture.root / "repository"
        verified = self.verify(directory, published=True, repository_root=repository)
        self.steamos_source.assert_called_once_with(repository, fixtures.COMMIT)
        self.steamos_rebuild.assert_called_once_with(repository, fixtures.COMMIT)
        expected = {"build_commit": fixtures.COMMIT,
                    "sha256": formats.sha256_hex(STEAMOS_BYTES),
                    "size": len(STEAMOS_BYTES)}
        self.assertEqual(verified.steamos_installer, expected)
        self.assertEqual(verified.summary()["steamos_installer"], expected)
        self.assertNotIn(formats.STEAMOS_INSTALLER_ASSET, verified.assets)
        self.assertEqual(sorted(verified.assets), sorted(self.verify(self.pristine).assets))
        self.assertNotIn("steamos_installer", self.verify(self.pristine).summary())

        # One changed byte fails, and so does a longer or shorter copy.
        for published in (STEAMOS_BYTES.replace(b"stand-in", b"stand-im"),
                          STEAMOS_BYTES + b"#\n", STEAMOS_BYTES[:-1]):
            with self.subTest(published=published):
                self.add_steamos_installer(directory, published)
                self.assertFails(
                    directory, f"{formats.STEAMOS_INSTALLER_ASSET}: differs from a rebuild "
                               f"at build commit {fixtures.COMMIT}", published=True)

        # A rebuild that cannot run is a failure, never a pass.
        self.add_steamos_installer(directory)
        self.steamos_rebuild.side_effect = verify_release.steamos_installer \
            .SteamOSInstallerError(f"git ls-tree failed: Not a valid object name "
                                   f"{fixtures.COMMIT}")
        self.assertFails(
            directory, f"{formats.STEAMOS_INSTALLER_ASSET}: cannot rebuild it at build "
                       f"commit {fixtures.COMMIT}", published=True)

    def test_mac_dmg_and_sidecar_are_required_and_hash_bound(self):
        dmg_name = formats.guarded_install_dmg_asset(
            fixtures.COMMIT, formats.SHIPPING_CONFIGURATION)
        self.dmg_inspection.reset_mock()
        verified = self.verify(self.pristine)
        self.assertEqual(verified.platforms["Mac"].dmg, dmg_name)
        self.assertTrue(verified.platforms["Mac"].dmg_inspection)
        if sys.platform != "darwin":
            self.dmg_inspection.assert_not_called()
        self.dmg_inspection.reset_mock()
        inspected = self.verify(self.pristine, inspect_mac_dmg=True)
        mac_release = formats.load_guarded_release(
            self.fixture.releases["Mac"], "Mac")
        self.dmg_inspection.assert_called_once_with(
            self.pristine / dmg_name,
            inspected.platforms["Mac"].tree_root,
            mac_release.dmg["background_sha256"], notarized=True)
        mac_manifest = json.loads(
            (self.pristine / formats.manifest_asset("Mac")).read_bytes())
        self.assertTrue(mac_manifest["archive"]["name"].endswith(".zip"))
        self.assertNotEqual(mac_manifest["archive"]["name"], dmg_name)

        directory = self.copy()
        (directory / dmg_name).write_bytes((directory / dmg_name).read_bytes() + b"x")
        self.assertFails(directory, "differs from its verification sidecar")
        directory = self.copy()
        (directory / (dmg_name + ".verify.json")).unlink()
        self.assertFails(directory, "not a regular verification sidecar")

        directory = self.copy()
        dmg = directory / dmg_name
        dmg.write_bytes(dmg.read_bytes() + b"joint replacement")
        sidecar_path = directory / (dmg_name + ".verify.json")
        sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
        sidecar["sha256"], sidecar["size"] = formats.sha256_file(dmg)
        sidecar_path.write_bytes(formats.canonical_bytes(
            sidecar, "jointly replaced DMG sidecar"))
        self.assertFails(directory, "bound Mac release.json")

    def test_mac_release_binding_is_required_at_sequence_zero_and_override(self):
        self.assertFails(
            self.pristine, "requires --release Mac=", releases={})
        directory = self.fixture.root / "sequence-one-missing-mac-binding"
        self.fixture.sign(directory, release_sequence=1, tag="v0.1.1-missing-mac")
        earlier = self.verify(self.pristine).platforms
        previous = {platform: row.announcement for platform, row in earlier.items()}
        self.assertFails(
            directory, "requires --release Mac=", releases={},
            previous=previous, allow_staging_sequence=True)

    def test_manual_install_zip_refuses_appledouble(self):
        directory = self.copy()
        install = directory / formats.guarded_install_zip_asset(
            "Mac", fixtures.COMMIT, formats.SHIPPING_CONFIGURATION)
        with zipfile.ZipFile(install, "a") as archive:
            archive.writestr("WoT.app/Contents/Resources/._Global.pak", b"metadata")
        self.assertFails(directory, "AppleDouble metadata is not allowed")

    def test_refuses_missing_malformed_mismatched_or_empty_release_identity(self):
        identity_path = fixtures.release_identity_relative("Win64")
        cases = {
            "missing release identity": None,
            "malformed release identity": b"{",
            "release identity sequence": fixtures.release_identity_bytes(4),
            "empty release commit": fixtures.release_identity_bytes(0, ""),
            "release identity commit": fixtures.release_identity_bytes(0, "fedcba987"),
        }
        for index, (fragment, identity) in enumerate(cases.items()):
            with self.subTest(fragment=fragment):
                files = fixtures.platform_files("Win64")
                if identity is None:
                    files.pop(identity_path)
                else:
                    _data, mode = files[identity_path]
                    files[identity_path] = (identity, mode)
                win = fixtures.write_guarded_release(
                    self.fixture.root / f"verify-bad-identity-{index}", "Win64",
                    files=files)
                funnel = fixtures.write_funnel_receipt(
                    self.fixture.root / f"verify-bad-identity-{index}-funnel.json",
                    win)
                output = self.fixture.root / f"verify-bad-identity-set-{index}"
                accepted = formats.ReleaseIdentity(0, fixtures.COMMIT)
                with mock.patch.object(
                        formats, "read_release_identity_from_zip",
                        return_value=accepted):
                    self.fixture.sign(
                        output,
                        release_jsons={"Win64": win,
                                       "Mac": self.fixture.releases["Mac"]},
                        include_install_zips=False,
                        funnel_receipts={
                            "Win64": funnel,
                            "Mac": self.fixture.funnel_receipts["Mac"]})
                self.assertFails(output, fragment)

    def test_install_zip_identity_is_checked_independently_of_update_archives(self):
        directory = self.copy()
        install = directory / formats.guarded_install_zip_asset(
            "Win64", fixtures.COMMIT, formats.SHIPPING_CONFIGURATION)
        member = (formats.GUARDED_TREE_ROOTS["Win64"] + "/"
                  + fixtures.release_identity_relative("Win64"))
        replace_zip_member(install, member, fixtures.release_identity_bytes(4))
        self.assertFails(directory, "release identity sequence")
        self.assertFails(directory, "release identity sequence",
                         allow_staging_sequence=True)
        self.assertFails(directory, "release identity sequence", allow_test=True)

    def _unstamped_set(self, sequence):
        """A signed set at a sequence whose trees carry no release-version.json."""
        releases = {}
        for platform in formats.PLATFORMS:
            files = fixtures.platform_files(platform, release_sequence=sequence)
            files.pop(fixtures.release_version_relative(platform))
            releases[platform] = fixtures.write_guarded_release(
                self.fixture.root / f"unstamped-{sequence}-{platform}", platform,
                files=files, release_sequence=sequence)
        funnels = {
            platform: fixtures.write_funnel_receipt(
                self.fixture.root / f"unstamped-{sequence}-funnel" / platform
                / "funnel-receipt.json", release, include_update_cells=True)
            for platform, release in releases.items()}
        output = self.fixture.root / f"unstamped-set-{sequence}"
        with mock.patch.object(formats, "read_release_version_from_zip",
                               return_value="0.1.0"):
            self.fixture.sign(output, release_sequence=sequence,
                              tag=f"v0.1.{sequence}-unstamped",
                              release_jsons=releases, funnel_receipts=funnels)
        return output, releases, funnels

    def test_sets_signed_before_the_stamp_verify_without_it(self):
        # v0.1.0 (sequence 0) and v0.1.1 (sequence 1) carry no stamp: the
        # cache gate authenticates the previous set, and verify re-reads it.
        for sequence in (0, 1):
            with self.subTest(sequence=sequence):
                output, releases, funnels = self._unstamped_set(sequence)
                verify_release._verify_signing_output(
                    output, trust=self.fixture.trust,
                    compiled=self.fixture.compiled, releases=releases,
                    funnel_receipts=funnels, allow_staging_sequence=False,
                    allow_test=False, inspect_mac_dmg=False)
                for platform in formats.PLATFORMS:
                    verify_release.authenticate_platform(
                        output, platform, trust=self.fixture.trust,
                        compiled=self.fixture.compiled)

    def test_refuses_a_missing_release_version_from_sequence_two(self):
        self.assertEqual(formats.RELEASE_VERSION_FIRST_SEQUENCE, 2)
        output, releases, funnels = self._unstamped_set(2)
        failures = self.assertFails(
            output, "missing release version", releases=releases,
            funnel_receipts=funnels, allow_staging_sequence=True)
        joined = "\n".join(failures)
        # Both the signed update archive and the manual-install zip refuse it.
        for platform in formats.PLATFORMS:
            self.assertRegex(joined, rf"{platform}: \S*-update-r2-\S*\.zip "
                             "release version: missing release version")
            install = formats.guarded_install_zip_asset(
                platform, fixtures.COMMIT, formats.SHIPPING_CONFIGURATION)
            self.assertIn(f"{platform}: {install} release version: missing "
                          "release version", joined)
        with self.assertRaises(verify_release.VerifyError) as caught:
            verify_release.authenticate_platform(
                output, "Win64", trust=self.fixture.trust,
                compiled=self.fixture.compiled)
        self.assertIn("missing release version",
                      "\n".join(caught.exception.failures))

    def test_refuses_malformed_or_mismatched_release_version(self):
        version_path = fixtures.release_version_relative("Win64")
        cases = {
            "malformed release version": b"{",
            "release version '0.2.0'": fixtures.release_version_bytes("0.2.0"),
        }
        for index, (fragment, version) in enumerate(cases.items()):
            with self.subTest(fragment=fragment):
                files = fixtures.platform_files("Win64")
                if version is None:
                    files.pop(version_path)
                else:
                    _data, mode = files[version_path]
                    files[version_path] = (version, mode)
                win = fixtures.write_guarded_release(
                    self.fixture.root / f"verify-bad-version-{index}",
                    "Win64", files=files)
                funnel = fixtures.write_funnel_receipt(
                    self.fixture.root / f"verify-bad-version-{index}-funnel.json",
                    win)
                output = self.fixture.root / f"verify-bad-version-set-{index}"
                with mock.patch.object(
                        formats, "read_release_version_from_zip",
                        return_value="0.1.0"):
                    self.fixture.sign(
                        output,
                        release_jsons={"Win64": win,
                                       "Mac": self.fixture.releases["Mac"]},
                        include_install_zips=False,
                        funnel_receipts={
                            "Win64": funnel,
                            "Mac": self.fixture.funnel_receipts["Mac"]})
                self.assertFails(output, fragment)

    def test_install_zip_version_is_checked_independently_of_update_archives(self):
        directory = self.copy()
        install = directory / formats.guarded_install_zip_asset(
            "Win64", fixtures.COMMIT, formats.SHIPPING_CONFIGURATION)
        member = (formats.GUARDED_TREE_ROOTS["Win64"] + "/"
                  + fixtures.release_version_relative("Win64"))
        replace_zip_member(
            install, member, fixtures.release_version_bytes("0.2.0"))
        self.assertFails(directory, "release version '0.2.0'")

    def test_release_json_provenance(self):
        directory = self.copy()
        other = fixtures.write_guarded_release(
            self.fixture.root / f"provenance-{self.counter}", "Win64",
            files={**fixtures.WIN64_FILES,
                   "InternalTools/WoTUpdater.exe": (b"different", 0o100777)})
        self.assertFails(directory, "guarded release tree differs from the signed update tree",
                         releases={"Win64": other})

    def test_release_json_binds_the_full_tree_without_manual_install_zips(self):
        directory = self.fixture.root / "tree-bound-sequence-one"
        self.fixture.sign(directory, release_sequence=1, tag="v0.1.1-tree",
                          include_install_zips=False)
        releases = {
            platform: self.fixture.root / f"sequence-1-{platform}" / "release.json"
            for platform in formats.PLATFORMS}
        funnels = {
            platform: self.fixture.root / "sequence-1-funnel" / platform
            / "funnel-receipt.json"
            for platform in formats.PLATFORMS}

        files = fixtures.platform_files("Win64", release_sequence=1)
        pak = "WoT/Content/Paks/pakchunk0-Windows.pak"
        _contents, mode = files[pak]
        files[pak] = (b"different non-executable content", mode)
        releases["Win64"] = fixtures.write_guarded_release(
            self.fixture.root / "tree-bound-alternate-win64", "Win64", files=files,
            release_sequence=1)
        funnels["Win64"] = fixtures.write_funnel_receipt(
            self.fixture.root / "tree-bound-alternate-win64-funnel.json",
            releases["Win64"], include_update_cells=True)

        self.assertFails(directory, "guarded release tree differs from the signed "
                         "update tree", releases=releases, funnel_receipts=funnels)

    def test_pristine_set_attests_shipping_everywhere(self):
        verified = self.verify(self.pristine)
        self.assertEqual({result.configuration for result in verified.platforms.values()},
                         {"Shipping"})
        self.assertEqual(verified.summary()["platforms"]["Mac"]["configuration"],
                         "Shipping")

    def test_a_test_configuration_is_flagged_unless_allowed(self):
        directory = self.copy()
        for platform in formats.PLATFORMS:
            self.resign_manifest(directory, platform, lambda document: document[
                "target_attestation"].update(configuration="Test"))
        failures = self.assertFails(directory, "FLAGGED Test build")
        # The attested Test claim also contradicts the Shipping-named image.
        self.assertIn("is not the Test game image", "\n".join(failures))
        directory = self.copy()
        self.resign_manifest(directory, "Mac", lambda document: document[
            "target_attestation"].update(configuration="Development"))
        self.assertFails(directory, "is not a guarded configuration")

    def test_a_deliberate_test_set_verifies_only_with_allow_test(self):
        releases = {platform: fixtures.write_guarded_release(
            self.fixture.root / f"allow-test-{platform}", platform, configuration="Test")
            for platform in formats.PLATFORMS}
        directory = self.fixture.root / "allow-test-set"
        self.fixture.sign(directory, release_jsons=releases, funnel_receipts={},
                          allow_test=True)
        self.assertFails(directory, "FLAGGED Test build")
        verified = self.verify(directory, allow_test=True, releases=releases)
        self.assertEqual(verified.platforms["Win64"].configuration, "Test")
        arguments = ["--set", str(directory), *self.fixture.trust_arguments()]
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(verify_release.main(arguments), 1)
        self.assertIn("FLAGGED Test build", errors.getvalue())
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(verify_release.main(arguments + [
                "--allow-test", "--release", f"Mac={releases['Mac']}"]), 0)

    def test_install_zip_name_must_match_the_attested_configuration(self):
        directory = self.copy()
        shipping = directory / f"WoT-Win64-guarded-shipping-{fixtures.COMMIT}.zip"
        shipping.rename(directory / f"WoT-Win64-guarded-{fixtures.COMMIT}.zip")
        self.assertFails(directory, "install zip name does not match the 'Shipping'")

    def test_release_json_configuration_must_match_the_attestation(self):
        directory = self.copy()
        test_release = fixtures.write_guarded_release(
            self.fixture.root / f"config-{self.counter}", "Win64", configuration="Test")
        self.assertFails(directory, "configuration 'Test' differs from the manifest "
                         "attestation 'Shipping'", releases={"Win64": test_release})

    def test_test_markers_are_refused(self):
        directory = self.copy()
        self.resign_manifest(directory, "Win64",
                             lambda document: document.update(human_version="test-key"))
        self.resign_manifest(directory, "Mac",
                             lambda document: document.update(human_version="test-key"))
        self.assertFails(directory, "test marker 'test-key'")

    def test_positive_sequence_gate_and_monotonic_checks(self):
        directory = self.fixture.root / "sequence-four-set"
        self.fixture.sign(directory, release_sequence=4, tag="v0.1.4")
        releases = {
            platform: self.fixture.root / f"sequence-4-{platform}" / "release.json"
            for platform in formats.PLATFORMS}
        funnels = {
            platform: self.fixture.root / "sequence-4-funnel" / platform
            / "funnel-receipt.json"
            for platform in formats.PLATFORMS}
        earlier = self.verify(self.pristine).platforms
        previous = {platform: result.announcement for platform, result in earlier.items()}
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            unproven = self.verify(directory, releases=releases, previous=previous,
                                   funnel_receipts={"Win64": funnels["Win64"]})
        self.assertEqual(unproven.sequence_gate["mode"], "unproven")
        self.assertEqual(set(unproven.sequence_gate["platforms"]), {"Win64"})
        self.assertIn("WARNING: Mac", errors.getvalue())
        self.assertFails(
            directory, "authenticated --previous announcements for exactly",
            releases=releases, funnel_receipts=funnels)
        for platform in formats.PLATFORMS:
            with self.subTest(previous_only=platform):
                self.assertFails(
                    directory, "authenticated --previous announcements for exactly",
                    releases=releases, funnel_receipts=funnels,
                    previous={platform: previous[platform]})
        verified = self.verify(directory, releases=releases, funnel_receipts=funnels,
                               previous=previous)
        self.assertEqual(verified.sequence_gate["mode"], "task7-update-funnel")
        overridden = self.verify(directory, allow_staging_sequence=True,
                                 previous=previous,
                                 releases={"Mac": releases["Mac"]})
        self.assertEqual(overridden.sequence_gate["mode"],
                         "unproven")
        # 4 after 0 is fine; 0 after 4 is a rollback; 4 after 4 is a duplicate.
        self.verify(directory, releases=releases, funnel_receipts=funnels,
                    previous=previous)
        later = self.verify(directory, releases=releases,
                            funnel_receipts=funnels, previous=previous).platforms
        newer = {platform: result.announcement for platform, result in later.items()}
        self.assertFails(self.pristine, "(rollback)", previous=newer)
        self.assertFails(directory, "already published", previous=newer,
                         releases=releases, funnel_receipts=funnels)
        higher = {platform: dataclasses.replace(announcement, release_sequence=5)
                  for platform, announcement in newer.items()}
        self.assertFails(directory, "(rollback)", previous=higher,
                         releases=releases, funnel_receipts=funnels)
        previous_directory = self.fixture.root / "sequence-four-previous"
        previous_directory.mkdir()
        for platform in formats.PLATFORMS:
            for suffix in ("", formats.SIGNATURE_SUFFIX):
                name = formats.announcement_asset(platform) + suffix
                shutil.copyfile(directory / name, previous_directory / name)
        arguments = ["--set", str(directory), "--previous", str(previous_directory),
                     *self.fixture.trust_arguments()]
        for platform in formats.PLATFORMS:
            arguments += ["--release", f"{platform}={releases[platform]}",
                          "--funnel-receipt", f"{platform}={funnels[platform]}"]
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(verify_release.main(arguments), 1)
        self.assertIn("already published", errors.getvalue())
        # Equal zero is the portable-era beacon and is allowed.
        self.verify(self.pristine, previous=previous)

    def test_replayed_sequence_receipt_and_test_twin_are_refused(self):
        sequence_two_set = self.fixture.root / "replay-sequence-two-set"
        self.fixture.sign(sequence_two_set, release_sequence=2, tag="v0.1.2")
        previous = {
            platform: result.announcement
            for platform, result in self.verify(self.pristine).platforms.items()}
        sequence_one = {
            platform: fixtures.write_guarded_release(
                self.fixture.root / f"verify-replay-one-{platform}", platform,
                release_sequence=1)
            for platform in formats.PLATFORMS}
        replayed_releases = {}
        replayed_receipts = {}
        for platform in formats.PLATFORMS:
            old = json.loads(sequence_one[platform].read_text(encoding="utf-8"))
            replayed_releases[platform] = fixtures.write_guarded_release(
                self.fixture.root / f"verify-replay-two-{platform}", platform,
                release_sequence=2, test_twin=old["test_twin"])
            replayed_receipts[platform] = fixtures.write_funnel_receipt(
                self.fixture.root / f"verify-replay-one-{platform}-funnel.json",
                sequence_one[platform], include_update_cells=True)
        self.assertFails(
            sequence_two_set, "test_twin release_sequence",
            releases=replayed_releases, funnel_receipts=replayed_receipts,
            previous=previous)

    def resign_lineage(self, directory, platform, mutate, *, key=None, public=None,
                       context=crypto.CONTEXT_CACHE_LINEAGE_V1):
        name = formats.cache_lineage_asset(platform)
        document = json.loads((directory / name).read_bytes())
        mutate(document)
        raw = json.dumps(document, sort_keys=True, separators=(",", ":")).encode()
        (directory / name).write_bytes(raw)
        (directory / (name + ".sig")).write_bytes(crypto.sign_framed(
            self.fixture.openssl, key or self.fixture.release_key, context, raw,
            public or self.fixture.release_public))

    def test_cache_lineage_is_signed_and_bound_to_the_manifest(self):
        verified = self.verify(self.pristine, openssl=self.fixture.openssl)
        for platform in formats.PLATFORMS:
            lineage = verified.platforms[platform].cache_lineage
            self.assertEqual((lineage["platform"], lineage["release_sequence"],
                              lineage["revision"]), (platform, 0, 1))
            for suffix in ("", ".sig"):
                self.assertIn(formats.cache_lineage_asset(platform) + suffix,
                              verified.assets)
        name = formats.cache_lineage_asset("Win64")
        directory = self.copy()
        raw = bytearray((directory / name).read_bytes())
        raw[-3] = ord("1") if raw[-3] != ord("1") else ord("2")
        (directory / name).write_bytes(bytes(raw))
        self.assertFails(directory, "does not verify under release key")
        directory = self.copy()
        self.resign_lineage(directory, "Win64", lambda document: None,
                            key=self.fixture.announcement_key,
                            public=self.fixture.announcement_public)
        self.assertFails(directory, f"{name}: the signature does not verify")
        directory = self.copy()
        self.resign_lineage(directory, "Win64", lambda document: None,
                            context=crypto.CONTEXT_RELEASE_MANIFEST_V2)
        self.assertFails(directory, f"{name}: the signature does not verify")
        directory = self.copy()
        self.resign_lineage(directory, "Mac",
                            lambda document: document.update(release_sequence=5))
        self.assertFails(directory, "release_sequence 5 differs from the signed "
                         "manifest's 0")
        directory = self.copy()
        self.resign_lineage(directory, "Mac",
                            lambda document: document.update(human_version="0.1.0"))
        self.assertFails(directory, "unknown=['human_version']")
        directory = self.copy()
        self.resign_lineage(directory, "Win64",
                            lambda document: document.update(platform="Mac"))
        self.assertFails(directory, f"{name}.platform is not Win64")
        directory = self.copy()
        self.resign_lineage(directory, "Win64",
                            lambda document: document.update(revision=2))
        self.assertFails(directory, "not the content label of revision 2")

    def test_cache_lineage_is_optional_at_sequence_zero_and_required_after(self):
        directory = self.copy()
        for suffix in ("", ".sig"):
            (directory / (formats.cache_lineage_asset("Win64") + suffix)).unlink()
        verified = self.verify(directory)
        self.assertIsNone(verified.platforms["Win64"].cache_lineage)
        self.assertIsNotNone(verified.platforms["Mac"].cache_lineage)
        directory = self.copy()
        (directory / formats.cache_lineage_asset("Mac")).unlink()
        self.assertFails(directory, "missing asset WoT-Mac-cache-lineage-v1.json")

        sequence_one = self.fixture.root / "lineage-sequence-one"
        self.fixture.sign(sequence_one, release_sequence=1, tag="v0.1.1-lineage")
        releases = {"Mac": self.fixture.root / "sequence-1-Mac" / "release.json"}
        previous = {platform: result.announcement
                    for platform, result in self.verify(self.pristine).platforms.items()}
        with contextlib.redirect_stderr(io.StringIO()):
            verified = self.verify(sequence_one, releases=releases, previous=previous)
            self.assertEqual(verified.platforms["Win64"].cache_lineage["release_sequence"],
                             1)
            for suffix in ("", ".sig"):
                (sequence_one / (formats.cache_lineage_asset("Win64") + suffix)).unlink()
            self.assertFails(sequence_one, "missing asset WoT-Win64-cache-lineage-v1.json: "
                             "a release at sequence 1 or later",
                             releases=releases, previous=previous)

    def test_verified_cache_lineage_loader_needs_only_one_platform_chain(self):
        directory = self.fixture.root / "lineage-win64-only"
        directory.mkdir()
        manifest = json.loads((self.pristine / formats.manifest_asset("Win64")).read_bytes())
        names = [formats.KEY_RING_ASSET, manifest["archive"]["name"]]
        for name in (formats.KEY_RING_ASSET, formats.announcement_asset("Win64"),
                     formats.manifest_asset("Win64"),
                     formats.cache_lineage_asset("Win64")):
            names += [name, name + ".sig"]
        for name in set(names):
            shutil.copyfile(self.pristine / name, directory / name)
        lineage = verify_release.load_verified_cache_lineage(
            directory, "Win64", trust=self.fixture.trust, compiled=self.fixture.compiled,
            openssl=self.fixture.openssl)
        self.assertEqual(lineage, formats.parse_cache_lineage(
            (directory / formats.cache_lineage_asset("Win64")).read_bytes(), "Win64"))
        with self.assertRaises(verify_release.VerifyError) as caught:
            verify_release.load_verified_cache_lineage(
                directory, "Mac", trust=self.fixture.trust,
                compiled=self.fixture.compiled)
        self.assertIn("missing asset WoT-Mac-announcement-v1.json",
                      "\n".join(caught.exception.failures))
        (directory / (formats.cache_lineage_asset("Win64") + ".sig")).write_bytes(
            bytes(64))
        with self.assertRaises(verify_release.VerifyError) as caught:
            verify_release.load_verified_cache_lineage(
                directory, "Win64", trust=self.fixture.trust,
                compiled=self.fixture.compiled)
        self.assertIn("does not verify", "\n".join(caught.exception.failures))

    def test_previous_announcements_must_authenticate(self):
        directory = self.copy()
        previous = self.fixture.root / f"previous-{self.counter}"
        previous.mkdir()
        for platform in formats.PLATFORMS:
            for suffix in ("", ".sig"):
                name = formats.announcement_asset(platform) + suffix
                shutil.copyfile(directory / name, previous / name)
        loaded = verify_release.load_previous_announcements(
            previous, self.fixture.trust, self.fixture.compiled)
        self.assertEqual(sorted(loaded), ["Mac", "Win64"])
        (previous / (formats.announcement_asset("Mac") + ".sig")).write_bytes(bytes(64))
        with self.assertRaises(verify_release.VerifyError):
            verify_release.load_previous_announcements(
                previous, self.fixture.trust, self.fixture.compiled)

    def test_command_line_exit_codes(self):
        directory = self.copy()
        (directory / "stray.bin").write_bytes(b"x")
        arguments = ["--set", str(directory), *self.fixture.trust_arguments()]
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(verify_release.main(arguments), 1)
        self.assertIn("unexpected file stray.bin", errors.getvalue())
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(verify_release.main(
                ["--set", str(self.pristine), "--release",
                 f"Mac={self.fixture.releases['Mac']}",
                 *self.fixture.trust_arguments()]), 0)
        self.assertIn("verify_release: PASS", output.getvalue())
        if sys.platform != "darwin":
            self.assertIn("bound only to the unsigned release.json",
                          output.getvalue())
            self.assertIn("contents were not inspected", output.getvalue())


class ProductionTrustTests(unittest.TestCase):
    def test_a_test_signed_set_never_verifies_against_production_trust(self):
        fixture = fixtures.ReleaseFixture()
        try:
            directory = fixture.root / "set"
            fixture.sign(directory)
            with self.assertRaises(verify_release.VerifyError) as caught:
                verify_release.verify_publish_set(
                    directory, trust=formats.load_trust(),
                    compiled=formats.load_compiled_constants())
            self.assertIn("not signed by any embedded offline root",
                          "\n".join(caught.exception.failures))
        finally:
            fixture.cleanup()


if __name__ == "__main__":
    unittest.main()
