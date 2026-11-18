#!/usr/bin/env python3
"""Unit tests for deterministic, loopback-only update funnel fixtures."""
from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import release_test_fixtures as fixtures  # noqa: E402
import update_funnel_fixtures as updates  # noqa: E402


class UpdateFunnelFixtureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.openssl = fixtures.openssl_or_skip()
        self.release_json = fixtures.write_guarded_release(
            self.root / "base-release", "Win64", release_sequence=7,
            configuration=formats.TEST_CONFIGURATION,
        )
        guarded = formats.load_guarded_release(self.release_json, "Win64")
        extracted = self.root / "extracted"
        with zipfile.ZipFile(guarded.zip_path) as archive:
            archive.extractall(extracted)
        self.base_tree = extracted / formats.GUARDED_ZIP_ROOTS["Win64"] / "Game"

    def test_fixture_signers_are_the_native_test_identities(self):
        # A guarded Test build trusts only these public halves; fresh keys
        # fail the update-apply cell with SIGNATURE-INVALID.
        key_dir = self.root / "keys"
        key_dir.mkdir()
        _paths, public = updates._make_test_keys(key_dir, self.openssl)
        self.assertEqual({role: value.hex() for role, value in public.items()}, {
            "announcement": updates.ANNOUNCEMENT_PUBLIC_HEX,
            "offline_root": updates.ROOT_PUBLIC_HEX,
            "release": updates.RELEASE_PUBLIC_HEX,
        })

    def test_builds_good_and_bad_n_plus_one_from_verified_base(self):
        receipt_path = updates.build_update_fixtures(
            self.release_json, self.base_tree, self.root / "output",
            openssl=self.openssl,
        )
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))

        self.assertEqual(receipt["format"], updates.DERIVATION_FORMAT)
        self.assertEqual(receipt["platform"], "Win64")
        self.assertEqual(receipt["base"]["release_sequence"], 7)
        self.assertEqual(receipt["next_release"], {
            "release_commit": fixtures.COMMIT,
            "release_sequence": 8,
        })
        self.assertEqual(receipt["test_public_keys"], {
            "announcement": updates.ANNOUNCEMENT_PUBLIC_HEX,
            "offline_root": updates.ROOT_PUBLIC_HEX,
            "release": updates.RELEASE_PUBLIC_HEX,
        })

        good = receipt["variants"]["good"]
        bad = receipt["variants"]["bad"]
        self.assertNotEqual(good["tree_root"], bad["tree_root"])
        self.assertEqual(
            good["main_executable"]["sha256"],
            bad["main_executable"]["sha256"],
        )
        self.assertEqual(good["mutations"], [
            {
                "path": updates.RELEASE_IDENTITY_RELATIVE,
                "operation": "rewrite-release-identity",
            },
            {
                "path": updates.RELEASE_VERSION_RELATIVE,
                "operation": "rewrite-release-version",
            }
        ])
        self.assertEqual(bad["mutations"][2], {
            "path": "wot-test-fail-after-started",
            "operation": "fail-after-started",
        })

        for variant in (good, bad):
            directory = receipt_path.parent / variant["directory"]
            archive = directory / variant["archive"]["name"]
            staged = formats.stage_archive(archive, "Win64")
            self.assertEqual(staged.tree_root, variant["tree_root"])
            manifest_raw = (directory / variant["manifest"]["name"]).read_bytes()
            announcement_raw = (
                directory / variant["announcement"]["name"]
            ).read_bytes()
            announcement = formats.parse_announcement(
                announcement_raw, "Win64",
                formats.load_compiled_constants().announcement_domain,
            )
            manifest = formats.parse_manifest(manifest_raw, announcement)
            formats.validate_human_version(
                manifest.document["human_version"],
                "fixture manifest human_version", allow_empty=False)
            self.assertEqual(
                formats.read_release_version_from_zip(
                    archive, "Win64", staged.root),
                manifest.document["human_version"])
            entries = {entry.path: entry for entry in staged.entries}
            self.assertEqual(
                manifest.main_executable.sha256,
                entries[manifest.main_executable.name].sha256,
            )
            self.assertTrue(crypto.verify_framed(
                bytes.fromhex(receipt["test_public_keys"]["announcement"]),
                crypto.CONTEXT_ANNOUNCEMENT_V1, announcement_raw,
                (directory / (variant["announcement"]["name"] + ".sig")).read_bytes(),
            ))
            self.assertTrue(crypto.verify_framed(
                bytes.fromhex(receipt["test_public_keys"]["release"]),
                crypto.CONTEXT_RELEASE_MANIFEST_V2, manifest_raw,
                (directory / (variant["manifest"]["name"] + ".sig")).read_bytes(),
            ))

        private_like = [path for path in receipt_path.parent.rglob("*")
                        if path.suffix.casefold() in {".pem", ".key"}]
        self.assertEqual(private_like, [])

    def test_refuses_an_extracted_tree_that_differs_from_guarded_release(self):
        main = self.base_tree / "WoT" / "Binaries" / "Win64" / "WoT-Win64-Test.exe"
        main.write_bytes(b"changed")
        with self.assertRaisesRegex(updates.FixtureError, "extracted release tree"):
            updates.build_update_fixtures(
                self.release_json, self.base_tree, self.root / "output",
                openssl=self.openssl,
            )

    def test_derivation_receipt_binds_its_artifacts_by_hash_and_size(self):
        receipt_path = updates.build_update_fixtures(
            self.release_json, self.base_tree, self.root / "output",
            openssl=self.openssl,
        )
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        for variant in receipt["variants"].values():
            directory = receipt_path.parent / variant["directory"]
            for key in ("announcement", "key_ring", "manifest", "archive"):
                reference = variant[key]
                path = directory / reference["name"]
                raw = path.read_bytes()
                self.assertEqual(reference["size"], len(raw))
                self.assertEqual(reference["sha256"], hashlib.sha256(raw).hexdigest())


    @unittest.skipUnless(sys.platform == "darwin", "Mac fixture derivation uses ditto")
    def test_builds_mac_good_and_resigned_bad_n_plus_one(self):
        release_json = fixtures.write_guarded_release(
            self.root / "mac-base-release", "Mac", release_sequence=7,
            configuration=formats.TEST_CONFIGURATION)
        guarded = formats.load_guarded_release(release_json, "Mac")
        extracted = self.root / "mac-extracted"
        subprocess.run(
            ["ditto", "-x", "-k", str(guarded.zip_path), str(extracted)],
            check=True, capture_output=True, text=True)
        base_tree = extracted / formats.GUARDED_ZIP_ROOTS["Mac"]
        resigned = []

        def record_resign(app, _codesign="codesign"):
            resigned.append({
                "source": app.parent.name,
                "bad_marker": (app / updates.bad_build_marker("Mac")).is_file(),
            })
            return {
                "artifact": "source-before-ditto",
                "command": "codesign --verify --deep --strict",
                "verified": True,
            }

        archived_verification = {
            "artifact": "extracted-post-ditto",
            "command": "codesign --verify --deep --strict",
            "verified": True,
        }
        with mock.patch.object(
                updates, "_resign_mac_app", side_effect=record_resign), \
                mock.patch.object(
                    updates, "_verify_archived_mac_app",
                    return_value=archived_verification):
            receipt_path = updates.build_update_fixtures(
                release_json, base_tree, self.root / "mac-output",
                platform="Mac", openssl=self.openssl)

        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        self.assertEqual(receipt["platform"], "Mac")
        self.assertEqual(receipt["base"]["release_sequence"], 7)
        self.assertEqual(receipt["next_release"]["release_sequence"], 8)
        good = receipt["variants"]["good"]
        bad = receipt["variants"]["bad"]
        self.assertNotEqual(good["tree_root"], bad["tree_root"])
        self.assertEqual(good["signature_verification"], archived_verification)
        self.assertEqual(bad["signature_verification"], archived_verification)
        self.assertEqual(resigned, [
            {"source": "good-source", "bad_marker": False},
            {"source": "bad-source", "bad_marker": True},
        ])
        self.assertEqual(bad["mutations"][1:], [{
            "path": "Contents/UE/WoT/Build/WoTRelease/release-version.json",
            "operation": "rewrite-release-version",
        }, {
            "path": "Contents/Resources/wot-test-fail-after-started",
            "operation": "fail-after-started",
        }])
        for variant in (good, bad):
            archive = (receipt_path.parent / variant["directory"]
                       / variant["archive"]["name"])
            staged = formats.stage_archive(archive, "Mac")
            self.assertEqual(staged.tree_root, variant["tree_root"])

    def test_mac_resign_verifies_the_signed_bundle(self):
        app = self.root / "Sign.app"
        app.mkdir()
        completed = mock.Mock(returncode=0, stderr="", stdout="")
        with mock.patch.object(
                updates.subprocess, "run", return_value=completed) as run:
            result = updates._resign_mac_app(
                app)
        self.assertTrue(result["verified"])
        self.assertEqual([call.args[0][1:4] for call in run.call_args_list], [
            ["--force", "--deep", "--sign"],
            ["--verify", "--deep", "--strict"],
        ])


    @unittest.skipUnless(sys.platform == "darwin", "real codesign")
    def test_mac_bad_marker_location_survives_real_codesign(self):
        # The resign above is mocked; this one is not. A live funnel run
        # stopped on "unsealed contents present in the bundle root".
        def bundle(name: str) -> Path:
            app = self.root / name / "WoT.app"
            (app / "Contents" / "MacOS").mkdir(parents=True)
            (app / "Contents" / "Resources").mkdir()
            executable = app / "Contents" / "MacOS" / "WoT"
            # Bytes only: copy2 would also try to copy the system file flags.
            shutil.copyfile("/usr/bin/true", executable)
            executable.chmod(0o755)
            (app / "Contents" / "Info.plist").write_text(
                '<?xml version="1.0" encoding="UTF-8"?>\n'
                '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
                '<plist version="1.0"><dict>'
                '<key>CFBundleExecutable</key><string>WoT</string>'
                '<key>CFBundleIdentifier</key><string>com.example.wot-fixture</string>'
                '</dict></plist>\n', encoding="utf-8")
            return app

        sealed = bundle("sealed")
        (sealed / updates.bad_build_marker("Mac")).write_bytes(b"marker\n")
        self.assertTrue(updates._resign_mac_app(sealed)["verified"])

        loose = bundle("loose")
        (loose / "wot-test-fail-after-started").write_bytes(b"marker\n")
        with self.assertRaisesRegex(updates.FixtureError, "bundle root"):
            updates._resign_mac_app(loose)

    @unittest.skipUnless(sys.platform == "darwin", "POSIX-host Mac fixture contract")
    def test_mac_extracted_tree_verification_compares_projected_modes(self):
        release_json = fixtures.write_guarded_release(
            self.root / "mac-mode-release", "Mac", release_sequence=7,
            configuration=formats.TEST_CONFIGURATION)
        guarded = formats.load_guarded_release(release_json, "Mac")
        projected = formats.project_guarded_archive_tree(guarded.zip_path, "Mac")
        base_tree = self.root / "mode-WoT.app"
        for relative, (raw, mode) in fixtures.platform_files(
                "Mac", formats.TEST_CONFIGURATION, 7).items():
            path = base_tree / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
            path.chmod(mode & 0o777)
        for path in base_tree.rglob("*"):
            if path.is_dir():
                path.chmod(0o755)
        updates._verify_extracted_tree(base_tree, projected, "Mac")
        main = base_tree / "Contents" / "MacOS" / "WoT-Mac-Test"
        main.chmod(0o700)
        with self.assertRaisesRegex(updates.FixtureError, "changed"):
            updates._verify_extracted_tree(base_tree, projected, "Mac")


if __name__ == "__main__":
    unittest.main()
