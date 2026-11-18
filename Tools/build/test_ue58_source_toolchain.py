#!/usr/bin/env python3
"""Specs for the pinned source-engine verifier.

Every case is a way the wrong engine could be used to build a release binary and
nobody would notice: an installed build, a moved tag, a shallow clone, a changed
Setup dependency, a drifted source file the patch is about to touch, a different
compiler. Each of those produces a binary whose provenance cannot be established
after the fact, which for a mandatory-update system is the whole game.

The fixtures are synthetic trees, so this suite runs anywhere in milliseconds and
does not need the real 20 GB checkout.
"""
from __future__ import annotations

import hashlib
import json
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import provision_ue58_source as provisioner
import verify_ue58_source as verifier

PINNED_COMMIT = "7deeb413d3dc1fc034f48d1aacc0861301829d32"
OTHER_COMMIT = "1111111111111111111111111111111111111111"
PATCHED_SOURCE = "Engine/Source/Runtime/Engine/Private/UnrealEngine.cpp"
DEPENDENCY_MANIFEST = "Engine/Build/Commit.gitdeps.xml"


class _SyntheticEngine:
    """A tree shaped like a source engine, with a real git repo inside it."""

    def __init__(self, root: Path):
        self.root = root
        (root / "Engine" / "Build").mkdir(parents=True)
        (root / PATCHED_SOURCE).parent.mkdir(parents=True)
        (root / DEPENDENCY_MANIFEST).write_text("<DependencyManifest/>\n")
        (root / PATCHED_SOURCE).write_text("int UnrealEngine() { return 0; }\n")

        def git(*arguments):
            subprocess.run(["git", "-C", str(root), *arguments], check=True,
                           capture_output=True)

        git("init", "-q")
        git("config", "user.email", "spec@example.invalid")
        git("config", "user.name", "spec")
        git("add", "-A")
        git("commit", "-q", "-m", "synthetic engine")
        self.commit = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"], check=True,
            capture_output=True, text=True).stdout.strip()
        git("tag", "5.8.0-release")

    def manifest(self, **overrides) -> dict:
        document = {
            "format": verifier.MANIFEST_FORMAT,
            "engine": {
                "repository": "git@github.com:EpicGames/UnrealEngine.git",
                "tag": "5.8.0-release",
                "commit": self.commit,
            },
            "dependencies": {
                DEPENDENCY_MANIFEST: verifier.sha256_file(self.root / DEPENDENCY_MANIFEST),
            },
            "pristine_sources": {
                PATCHED_SOURCE: verifier.sha256_file(self.root / PATCHED_SOURCE),
            },
            "toolchains": {
                "Mac": {"apple_clang": "clang-1700", "macos_sdk": "26.2"},
                "Win64": {"msvc_cl": "19.44.35214", "msvc_toolset": "14.44.35207",
                          "visual_studio": "17.14.36401.2",
                          "windows_sdk": "10.0.22621.0"},
            },
        }
        document.update(overrides)
        return document


class SourceToolchainVerifierTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.engine = _SyntheticEngine(self.root / "engine")
        self.manifest_path = self.root / "source-toolchain.json"
        self.patch_manifest_path = self.root / "manifest.json"
        self.write_manifest(self.engine.manifest())
        # This suite runs on Linux/macOS CI alike, so the host identity is stubbed
        # and only the COMPARISON is under test here.
        self._platform = mock.patch.object(verifier.platform, "system",
                                           return_value="Darwin")
        self._platform.start()
        self._toolchain = mock.patch.object(
            verifier, "detect_toolchain",
            return_value={"apple_clang": "clang-1700", "macos_sdk": "26.2"})
        self._toolchain.start()

    def tearDown(self):
        self._toolchain.stop()
        self._platform.stop()
        self._tmp.cleanup()

    def write_manifest(self, document) -> None:
        self.manifest_path.write_text(json.dumps(document, indent=2, sort_keys=True))

    def verify(self, require_pristine=True, *, patch_manifest=None,
               require_patched=False):
        return verifier.verify_source_engine(self.engine.root, self.manifest_path,
                                             require_pristine, patch_manifest,
                                             require_patched)

    def assert_rejects(self, pattern):
        with self.assertRaisesRegex(verifier.VerificationFailed, pattern):
            self.verify()

    def test_windows_sdk_uses_ubt_main_version_not_highest_installed(self):
        include = self.root / "kits" / "Include"
        (include / "10.0.22621.0").mkdir(parents=True)
        (include / "10.0.26100.0").mkdir()
        policy = (self.engine.root / "Engine" / "Config" / "Windows" /
                  "Windows_SDK.json")
        policy.parent.mkdir(parents=True)
        policy.write_text(json.dumps({"MainVersion": "10.0.22621.0"}))

        self.assertEqual(
            verifier._windows_sdk_selected_by_ubt(self.engine.root, include),
            "10.0.22621.0")

    def test_windows_sdk_falls_back_to_highest_when_ubt_policy_is_absent(self):
        include = self.root / "kits" / "Include"
        (include / "10.0.22621.0").mkdir(parents=True)
        (include / "10.0.26100.0").mkdir()

        self.assertEqual(
            verifier._windows_sdk_selected_by_ubt(self.engine.root, include),
            "10.0.26100.0")

    def test_windows_sdk_refuses_an_uninstalled_ubt_main_version(self):
        include = self.root / "kits" / "Include"
        (include / "10.0.26100.0").mkdir(parents=True)
        policy = (self.engine.root / "Engine" / "Config" / "Windows" /
                  "Windows_SDK.json")
        policy.parent.mkdir(parents=True)
        policy.write_text(json.dumps({"MainVersion": "10.0.22621.0"}))

        with self.assertRaisesRegex(
                verifier.VerificationFailed, "not installed"):
            verifier._windows_sdk_selected_by_ubt(self.engine.root, include)

    # --- the happy path --------------------------------------------------------

    def test_a_pinned_pristine_source_tree_verifies(self):
        receipt = self.verify()
        self.assertEqual(receipt.commit, self.engine.commit)
        self.assertEqual(receipt.tag, "5.8.0-release")
        self.assertIn("not-an-installed-build", receipt.checks)
        self.assertIn("commit-matches-pin", receipt.checks)
        self.assertIn("toolchain-matches-pin(Mac)", receipt.checks)
        self.assertEqual(receipt.as_dict()["format"], "wot-ue58-source-receipt-v1")

    def test_public_manifest_without_patch_verifies_matching_patched_hashes(self):
        patched = b"int UnrealEngine() { return 58; }\n"
        self.patch_manifest_path.write_text(json.dumps({
            "format": verifier.PATCH_MANIFEST_FORMAT,
            "source_manifest": self.manifest_path.name,
            "patch_sha256": "0" * 64,
            "files": [{
                "path": PATCHED_SOURCE,
                "created": False,
                "pristine_sha256": verifier.sha256_file(
                    self.engine.root / PATCHED_SOURCE),
                "patched_sha256": hashlib.sha256(patched).hexdigest(),
            }],
        }))
        (self.engine.root / PATCHED_SOURCE).write_bytes(patched)
        self.assertFalse((self.root / "guarded-bootstrap.patch").exists())

        receipt = self.verify(
            patch_manifest=self.patch_manifest_path, require_patched=True)

        self.assertIn("patch-manifest-patched(1)", receipt.checks)

    def test_public_manifest_without_patch_rejects_a_hash_mismatch_clearly(self):
        pristine = verifier.sha256_file(self.engine.root / PATCHED_SOURCE)
        self.patch_manifest_path.write_text(json.dumps({
            "format": verifier.PATCH_MANIFEST_FORMAT,
            "source_manifest": self.manifest_path.name,
            "files": [{
                "path": PATCHED_SOURCE,
                "created": False,
                "pristine_sha256": pristine,
                "patched_sha256": "f" * 64,
            }],
        }))
        (self.engine.root / PATCHED_SOURCE).write_text("mismatched engine bytes\n")

        with self.assertRaisesRegex(
                verifier.VerificationFailed,
                r"has drifted .* expected .* pristine or .* patched"):
            self.verify(
                patch_manifest=self.patch_manifest_path, require_patched=True)

    # --- installed builds ------------------------------------------------------

    def test_an_installed_build_is_refused(self):
        (self.engine.root / "Engine" / "Build" / "InstalledBuild.txt").write_text("")
        self.assert_rejects("INSTALLED build")

    def test_a_developer_baseline_root_is_refused_by_name(self):
        with mock.patch.object(verifier, "installed_engine_root",
                               return_value=self.engine.root):
            self.assert_rejects("pre-plan developer baseline")

    # --- commit provenance -----------------------------------------------------

    def test_a_missing_commit_pin_is_refused(self):
        document = self.engine.manifest()
        document["engine"]["commit"] = ""
        self.write_manifest(document)
        self.assert_rejects("full 40-hex engine commit")

    def test_an_abbreviated_commit_pin_is_refused(self):
        document = self.engine.manifest()
        document["engine"]["commit"] = self.engine.commit[:12]
        self.write_manifest(document)
        self.assert_rejects("full 40-hex engine commit")

    def test_a_tag_only_pin_is_refused(self):
        document = self.engine.manifest()
        document["engine"]["commit"] = "5.8.0-release"
        self.write_manifest(document)
        self.assert_rejects("full 40-hex engine commit")

    def test_a_different_checked_out_commit_is_refused(self):
        document = self.engine.manifest()
        document["engine"]["commit"] = OTHER_COMMIT
        self.write_manifest(document)
        self.assert_rejects("expected the pinned")

    def test_a_moved_tag_does_not_override_the_commit_pin(self):
        # The tag now points somewhere else. The COMMIT is authoritative, and this
        # is the check that would notice a tag being moved under us.
        (self.engine.root / "extra.txt").write_text("later work\n")
        subprocess.run(["git", "-C", str(self.engine.root), "add", "-A"], check=True,
                       capture_output=True)
        subprocess.run(["git", "-C", str(self.engine.root), "commit", "-q", "-m",
                        "moved"], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(self.engine.root), "tag", "-f",
                        "5.8.0-release"], check=True, capture_output=True)
        self.assert_rejects("expected the pinned")

    def test_a_non_git_tree_is_refused(self):
        # Two Windows-lane hazards, both hit for real here:
        #   1. `rm -rf` does not exist, so shelling out raised FileNotFoundError
        #      before the assertion under test ever ran.
        #   2. git marks its loose objects READ-ONLY, and Windows honours that,
        #      so a plain rmtree then fails PermissionError. Add write back
        #      (preserving the other bits, which keeps POSIX traversal intact)
        #      before removing.
        git_dir = self.engine.root / ".git"
        for entry in git_dir.rglob("*"):
            entry.chmod(entry.stat().st_mode | stat.S_IWRITE)
        shutil.rmtree(git_dir)
        self.assert_rejects("not a git checkout")

    def test_a_shallow_clone_is_refused(self):
        (self.engine.root / ".git" / "shallow").write_text(self.engine.commit + "\n")
        self.assert_rejects("SHALLOW clone")

    # --- dependencies and source drift -----------------------------------------

    def test_an_unpinned_dependency_manifest_is_refused(self):
        document = self.engine.manifest()
        document["dependencies"] = {}
        self.write_manifest(document)
        self.assert_rejects("pins no Setup dependency manifest")

    def test_a_changed_dependency_manifest_is_refused(self):
        (self.engine.root / DEPENDENCY_MANIFEST).write_text("<DependencyManifest x/>\n")
        self.assert_rejects("not the pinned set")

    def test_a_missing_dependency_manifest_is_refused(self):
        (self.engine.root / DEPENDENCY_MANIFEST).unlink()
        self.assert_rejects("is missing")

    def test_a_drifted_patch_input_is_refused(self):
        (self.engine.root / PATCHED_SOURCE).write_text("int LaunchEngineLoop(){return 1;}\n")
        self.assert_rejects("has drifted")

    def test_unpinned_patch_inputs_are_refused(self):
        document = self.engine.manifest()
        document["pristine_sources"] = {}
        self.write_manifest(document)
        self.assert_rejects("pins no pristine source hashes")

    def test_source_drift_is_only_checked_when_required(self):
        # Without --require-source the cheap provenance checks still run, so a
        # drifted tree is caught later rather than never.
        (self.engine.root / PATCHED_SOURCE).write_text("drifted\n")
        receipt = self.verify(require_pristine=False)
        self.assertIn("commit-matches-pin", receipt.checks)
        self.assertNotIn("patch-inputs-pristine(1)", receipt.checks)
        self.assert_rejects("has drifted")

    # --- toolchain identity ----------------------------------------------------

    def test_an_absent_toolchain_identity_is_refused(self):
        document = self.engine.manifest()
        document["toolchains"] = {"Win64": {"msvc_cl": "19.44.35214"}}
        self.write_manifest(document)
        self.assert_rejects("records no toolchain identity for Mac")

    def test_an_empty_pinned_toolchain_field_is_refused(self):
        document = self.engine.manifest()
        document["toolchains"]["Mac"]["apple_clang"] = ""
        self.write_manifest(document)
        self.assert_rejects("empty fields")

    def test_a_different_compiler_is_refused(self):
        with mock.patch.object(verifier, "detect_toolchain", return_value={
                "apple_clang": "clang-1600", "macos_sdk": "26.2"}):
            self.assert_rejects("expected the pinned")

    def test_a_host_that_reports_no_compiler_is_refused(self):
        with mock.patch.object(verifier, "detect_toolchain", return_value={
                "apple_clang": "", "macos_sdk": "26.2"}):
            self.assert_rejects("reports no apple_clang")

    def test_a_mismatched_platform_is_refused(self):
        with mock.patch.object(verifier.platform, "system", return_value="Linux"):
            self.assert_rejects("unsupported host platform")

    # --- manifest hygiene ------------------------------------------------------

    def test_a_foreign_manifest_format_is_refused(self):
        document = self.engine.manifest()
        document["format"] = "wot-ue58-source-toolchain-v2"
        self.write_manifest(document)
        self.assert_rejects("manifest format")

    def test_an_unreadable_manifest_is_refused(self):
        self.manifest_path.write_text("{not json")
        self.assert_rejects("cannot read the manifest")

    def test_an_absent_engine_path_is_refused(self):
        with self.assertRaisesRegex(verifier.VerificationFailed, "does not exist"):
            verifier.verify_source_engine(self.root / "absent", self.manifest_path, True)

    def test_main_exits_non_zero_on_rejection_and_writes_a_receipt_on_success(self):
        receipt_path = self.root / "receipt.json"
        self.assertEqual(verifier.main([
            "--engine", str(self.engine.root), "--manifest", str(self.manifest_path),
            "--require-source", "--receipt", str(receipt_path)]), 0)
        written = json.loads(receipt_path.read_text())
        self.assertEqual(written["commit"], self.engine.commit)

        (self.engine.root / "Engine" / "Build" / "InstalledBuild.txt").write_text("")
        self.assertEqual(verifier.main([
            "--engine", str(self.engine.root), "--manifest", str(self.manifest_path),
            "--require-source"]), 1)


class CheckedInManifestTests(unittest.TestCase):
    """The real manifest must stay complete, whatever else changes."""

    MANIFEST = (Path(__file__).resolve().parents[2] / "EnginePatches" / "UE-5.8"
                / "source-toolchain.json")

    def test_the_checked_in_manifest_pins_everything_that_matters(self):
        if not self.MANIFEST.is_file():
            self.skipTest("the source-toolchain manifest has not been generated yet")
        document = json.loads(self.MANIFEST.read_text())
        self.assertEqual(document["format"], verifier.MANIFEST_FORMAT)
        self.assertRegex(document["engine"]["commit"], r"^[0-9a-f]{40}$")
        self.assertTrue(document["engine"]["tag"])
        self.assertTrue(document["dependencies"],
                        "Setup dependencies must be pinned")
        # The reduced patch has four existing inputs: the two world-start hook
        # files plus the two Launch platform files whose stock process entry is
        # guarded out for targets that own their own (WoTGuardedEngineTests).
        # The patch's fifth file, WoTWorldMaterialization.h, is created and
        # therefore has no pristine bytes to pin in the source-toolchain
        # artifact.
        expected = {
            "Engine/Source/Runtime/Engine/Private/UnrealEngine.cpp",
            "Engine/Source/Runtime/Engine/Private/World.cpp",
            "Engine/Source/Runtime/Launch/Private/Mac/LaunchMac.cpp",
            "Engine/Source/Runtime/Launch/Private/Windows/LaunchWindows.cpp",
        }
        self.assertEqual(set(provisioner.PATCHED_SOURCES), expected)
        self.assertEqual(set(document["pristine_sources"]), expected)
        for digest in document["pristine_sources"].values():
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
        self.assertIn("Mac", document["toolchains"])
        self.assertIn("Win64", document["toolchains"])
        self.assertNotIn("install_roots", document)
        self.assertNotIn("rejected_install_roots", document)
        self.assertNotIn("xcode_path", document["toolchains"]["Mac"])
        self.assertNotIn("vs_install_path", document["toolchains"]["Win64"])


if __name__ == "__main__":
    unittest.main()
