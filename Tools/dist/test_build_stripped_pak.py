#!/usr/bin/env python3
"""Stdlib unit tests for the stripped-pak orchestrator's seed integrity.

Pins the iso-directory denylist, entitlement invalidation, and the release
commit contract shared by the seeded cook and final package manifest.
"""
from __future__ import annotations

import sys
import copy
import subprocess
import shutil
import tempfile
import unittest
import plistlib
import hashlib
import json
import os
from types import SimpleNamespace
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_stripped_pak as bs
from test_guarded_build_root import fake_pe  # noqa: E402


def write_fake_mac_zip(path, tag=b"synthetic-archive", extra=()):
    """A real zip shaped like ditto's Mac asset (tests read its members)."""
    import zipfile
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("WoT.app/Contents/Info.plist", tag)
        entries = extra.items() if isinstance(extra, dict) else (
            (name, b"") for name in extra)
        for name, contents in entries:
            archive.writestr(name, contents)


def write_fake_mac_dmg(_app, guarded_zip, output):
    """A bound cross-host DMG fixture; real attach coverage is test_mac_dmg."""
    import release_formats
    output = Path(output)
    output.write_bytes(b"synthetic DMG " + output.name.encode("utf-8"))
    digest, size = release_formats.sha256_file(output)
    document = {
        "background_sha256": release_formats.sha256_hex(
            b"synthetic DMG background"),
        "format": bs.mac_dmg.DMG_SIDECAR_FORMAT,
        "installed_tree_root": release_formats.project_guarded_archive_tree(
            Path(guarded_zip), "Mac").tree_root,
        "name": output.name,
        "notarized": False,
        "sha256": digest,
        "size": size,
        "volume_name": bs.mac_dmg.VOLUME_NAME,
    }
    output.with_name(output.name + ".verify.json").write_bytes(
        release_formats.canonical_bytes(document, "test DMG sidecar"))
    return document
build_root_cache = bs.build_root_cache

# Every stage spec runs as if no EOS secrets file existed: the operator's real
# %USERPROFILE%\.wot-secrets file must neither leak into a fixture nor change
# a stage's cache decision.
_EOS_ISOLATION = None


def setUpModule():
    global _EOS_ISOLATION
    missing = Path(tempfile.gettempdir()) / "wot-eos-spec-absent" / "none.json"
    _EOS_ISOLATION = mock.patch.dict(
        os.environ, {bs.eos_credentials.ENV_FILE: str(missing)})
    _EOS_ISOLATION.start()


def tearDownModule():
    if _EOS_ISOLATION is not None:
        _EOS_ISOLATION.stop()


class EOSCredentialStagingTests(unittest.TestCase):
    """Per-package injection into the ISOLATED config, immediately before the
    Test cook/stage and the Shipping stage."""

    FAKE = {
        "ProductId": "FAKEproduct0000000000000000000000",
        "SandboxId": "FAKEsandbox0000000000000000000000",
        "DeploymentIdTest": "FAKEdeployTEST000000000000000000",
        "DeploymentIdPublic": "FAKEdeployPUBLIC0000000000000000",
        "ClientId": "FAKEclient00000000000000000000000",
        "ClientSecret": "FAKE+secret/value=000000000000000",
    }

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.iso = self.root / "iso"
        (self.iso / "Config").mkdir(parents=True)
        self.config = self.iso / "Config" / "DefaultEngine.ini"
        self.config.write_text("[OnlineSubsystem]\nDefaultPlatformService=Null\n",
                               encoding="utf-8")
        self.secrets = self.root / "eos-credentials.json"

    def tearDown(self):
        self.temp.cleanup()

    def env(self):
        return mock.patch.dict(os.environ, {
            bs.eos_credentials.ENV_FILE: str(self.secrets)})

    def test_absent_file_disables_eos_and_writes_nothing(self):
        with self.env():
            record = bs._inject_eos_credentials(self.iso, "Test")
        self.assertEqual(record["state"], "absent")
        self.assertEqual(record["internet_play"], "disabled")
        self.assertNotIn("Artifacts", self.config.read_text(encoding="utf-8"))

    def test_test_then_shipping_swap_the_single_artifact(self):
        self.secrets.write_text(json.dumps(self.FAKE), encoding="utf-8")
        with self.env():
            test = bs._inject_eos_credentials(self.iso, "Test")
            text = self.config.read_text(encoding="utf-8")
            self.assertIn('ArtifactName="WoTTest"', text)
            self.assertIn(self.FAKE["DeploymentIdTest"], text)
            self.assertNotIn(self.FAKE["DeploymentIdPublic"], text)
            shipping = bs._inject_eos_credentials(self.iso, "Shipping")
            text = self.config.read_text(encoding="utf-8")
            self.assertIn('ArtifactName="WoT"', text)
            self.assertIn(self.FAKE["DeploymentIdPublic"], text)
            self.assertNotIn(self.FAKE["DeploymentIdTest"], text)
            self.assertEqual(text.count("+Artifacts="), 1)
            preview = bs._eos_credentials_preview("Shipping")
        for record in (test, shipping, preview):
            serialized = json.dumps(record)
            for value in self.FAKE.values():
                self.assertNotIn(value, serialized)
        self.assertEqual(bs.eos_credentials.stage_identity(shipping),
                         bs.eos_credentials.stage_identity(preview))
        self.assertNotEqual(bs.eos_credentials.stage_identity(test),
                            bs.eos_credentials.stage_identity(shipping))

    def test_invalid_file_disables_without_failing_the_build(self):
        self.secrets.write_text('{"ProductId": "only one key"}', encoding="utf-8")
        with self.env():
            record = bs._inject_eos_credentials(self.iso, "Shipping")
        self.assertEqual(record["state"], "invalid")
        self.assertEqual(record["internet_play"], "disabled")
        self.assertNotIn("Artifacts", self.config.read_text(encoding="utf-8"))

    def test_missing_isolated_config_only_matters_when_credentials_exist(self):
        self.config.unlink()
        with self.env():
            self.assertEqual(
                bs._inject_eos_credentials(self.iso, "Test")["state"], "absent")
            self.secrets.write_text(json.dumps(self.FAKE), encoding="utf-8")
            with self.assertRaisesRegex(bs.BuildFailure, "config is missing"):
                bs._inject_eos_credentials(self.iso, "Test")

EXPECTED_NEUTRAL_ROOTS = (
    "/Game/WoT/Materials/M_WoT_World",
    "/Game/WoT/Materials/M_WoT_WorldMasked",
    "/Game/WoT/Materials/M_WoT_WorldWave",
    "/Game/WoT/Materials/M_WoT_WorldWaveMasked",
    "/Game/WoT/Materials/M_WoT_WorldWaveTranslucent",
    "/Game/WoT/Materials/M_WoT_WorldWaveModulated",
    "/Game/WoT/Materials/MI_WoT_MoverWaveRuntime",
    "/Game/WoT/Materials/MI_WoT_MoverWaveRuntimeMasked",
    "/Game/WoT/Materials/MI_WoT_MoverWaveRuntimeTranslucent",
    "/Game/WoT/Materials/MI_WoT_MoverWaveRuntimeModulated",
    "/Game/WoT/Materials/M_WoT_MeshTranslucent",
    "/Game/WoT/Materials/M_WoT_CoronaAdd",
    "/Game/WoT/Materials/M_WoT_CoronaMod",
    "/Game/WoT/Materials/M_WoT_Master",
    "/Game/WoT/Materials/M_WoT_MeshLit",
    "/Game/WoT/Materials/M_WoT_MeshLit2S",
    "/Game/WoT/Materials/M_WoT_MeshLitMasked",
    "/Game/WoT/Materials/M_WoT_MeshLitT",
    "/Game/WoT/Materials/M_WoT_MoverLit",
    "/Game/WoT/Materials/M_WoT_MoverLitMasked",
    "/Game/WoT/Materials/M_WoT_MeshModulate",
    "/Game/WoT/Materials/M_WoT_MeshFBAddGlow",
    "/Game/WoT/Materials/M_WoT_ParticleAdd",
    "/Game/WoT/Materials/M_WoT_ParticleMod",
    "/Game/WoT/Materials/M_WoT_EntryWorld",
    "/Game/WoT/Materials/M_WoT_EntryWorld_TwoSided",
    "/Game/WoT/Materials/M_WoT_EntryMasked",
    "/Game/WoT/Materials/M_WoT_EntryMasked_TwoSided",
    "/Game/WoT/Materials/M_WoT_EntryTranslucent",
    "/Game/WoT/Materials/M_WoT_EntryTranslucent_TwoSided",
    "/Game/WoT/Materials/M_WoT_EntryModulated",
    "/Game/WoT/Materials/M_WoT_EntryModulated_TwoSided",
    "/Game/WoT/Materials/M_WoT_EntryTranslucent_SceneBlend",
    "/Game/WoT/Materials/Instances/MI_GammaBoundary",
    "/Game/WoT/Materials/M_WoT_VolumeFog",
    "/Game/WoT/Materials/M_WoT_Decal",
    "/Game/WoT/Materials/M_WoT_EntryTranslucent_Mirror",
    "/Game/WoT/Materials/M_WoT_SkyWindow",
    "/Game/WoT/Materials/M_WoT_EntrySkyModulate",
    "/Game/WoT/Materials/M_WoT_EntrySkyAdd",
)
EXPECTED_NEUTRAL_HARD_DEPENDENCIES = (
    "/Game/WoT/Materials/MPC_WoTDynamicLights",
    "/Game/WoT/Materials/MPC_WoTLightWave",
    "/Game/WoT/Materials/MPC_WoTDisplay",
    "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral",
    "/Game/WoT/Materials/M_WoT_GammaBoundary",
)
EXPECTED_NEUTRAL_PACKAGES = (
    *EXPECTED_NEUTRAL_ROOTS, *EXPECTED_NEUTRAL_HARD_DEPENDENCIES)


class EntryWeatherNeutralSupportTests(unittest.TestCase):
    def test_entry_weather_and_gamma_boundary_are_in_the_closed_root_allowlist(self):
        self.assertEqual(bs.NEUTRAL_SUPPORT_ROOT_PACKAGES,
                         EXPECTED_NEUTRAL_ROOTS)
        self.assertEqual(len(bs.NEUTRAL_SUPPORT_ROOT_PACKAGES), 40)
        self.assertEqual(len(bs.NEUTRAL_SUPPORT_PACKAGES), 45)

FIXTURES = Path(__file__).resolve().parent / "fixtures"


class ValidateIsoTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self._old_repo = bs.REPO
        bs.REPO = self.repo

    def tearDown(self):
        bs.REPO = self._old_repo
        self._tmp.cleanup()

    def test_every_seeded_directory_is_forbidden(self):
        for name in bs.GUARDED_SEED_COPY_SET:
            if name == "WoT.uproject":
                continue
            with self.assertRaises(bs.BuildFailure, msg=name):
                bs._validate_iso(self.repo / name / "example-iso")

    def test_the_plugins_directory_is_forbidden(self):
        # The seed copies the code plugins out of Plugins/.
        for iso in (self.repo / "Plugins" / "example-iso",
                    self.repo / "Plugins" / "WoTOnboarding" / "example-iso"):
            with self.assertRaises(bs.BuildFailure, msg=str(iso)):
                bs._validate_iso(iso)

    def test_default_style_iso_is_accepted(self):
        iso = bs._validate_iso(self.repo / ".stripped-pak")
        self.assertEqual(iso, (self.repo / ".stripped-pak").resolve())


class SeedEntitlementsValidityTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.repo = root / "repo"
        self.iso = root / "repo" / ".stripped-pak"
        self._old_repo = bs.REPO
        bs.REPO = self.repo
        self.release_commit = "abcdef123"
        self._git_commit_patch = mock.patch.object(
            bs, "_git_commit", side_effect=lambda _root: self.release_commit)
        self._git_commit_patch.start()
        for name in bs.SEED_COPY_SET:
            if name == "WoT.uproject":
                (self.repo / name).parent.mkdir(parents=True, exist_ok=True)
                (self.repo / name).write_text("{}")
            else:
                (self.repo / name).mkdir(parents=True, exist_ok=True)
                (self.iso / name).mkdir(parents=True, exist_ok=True)
        (self.iso / "WoT.uproject").write_text("{}")
        resources = self.repo / "Build" / "Mac" / "Resources"
        resources.mkdir(parents=True, exist_ok=True)
        self.entitlements = resources / "Sandbox.Server.entitlements"
        self.entitlements.write_text("<dict/>")
        version_path = (
            self.iso / "Build" / "WoTRelease" / "release-version.json")
        version_path.parent.mkdir(parents=True, exist_ok=True)
        version_path.write_bytes(
            b'{"format":"wot-release-version-v1",'
            b'"human_version":"","version":1}')
        self.write_stamp()

    def write_stamp(self, **overrides):
        stamp = {
            "copy_set": list(bs.SEED_COPY_SET),
            "code_plugins": bs._seed_code_plugins(self.repo),
            "import_generated_outputs": [
                path.as_posix() for path in bs.IMPORT_GENERATED_OUTPUTS],
            "editor_module_mtime": bs._editor_module_mtime(self.repo),
            "entitlements_sha256": bs._entitlements_digests(self.repo),
            "release_commit": self.release_commit,
            "release_sequence": 0,
        }
        stamp.update(overrides)
        bs._write_stamp(self.iso, "seed", stamp)

    def add_plugin(self, root: Path, name: str = "WoTOnboarding") -> Path:
        plugin = root / "Plugins" / name
        plugin.mkdir(parents=True, exist_ok=True)
        (plugin / f"{name}.uplugin").write_text("{}")
        return plugin

    def tearDown(self):
        self._git_commit_patch.stop()
        bs.REPO = self._old_repo
        self._tmp.cleanup()

    def test_matching_seed_is_cached(self):
        self.assertTrue(bs._seed_valid(self.iso))

    def test_seed_from_before_code_plugins_is_stale(self):
        stamp = bs._read_stamp(self.iso, "seed")
        del stamp["code_plugins"]
        bs._write_stamp(self.iso, "seed", stamp)
        self.assertFalse(bs._seed_valid(self.iso))

    def test_seeded_plugin_must_match_the_repo_and_the_iso(self):
        self.add_plugin(self.repo)
        # The repo gained the plugin after this seed.
        self.assertFalse(bs._seed_valid(self.iso))
        self.write_stamp()
        # The stamp names it, but the ISO copy is gone.
        self.assertFalse(bs._seed_valid(self.iso))
        self.add_plugin(self.iso)
        self.assertTrue(bs._seed_valid(self.iso))

    def test_plugin_editor_module_rebuild_invalidates_seed(self):
        binaries = self.add_plugin(self.repo) / "Binaries" / "Mac"
        binaries.mkdir(parents=True)
        module = binaries / "libUnrealEditor-WoTOnboarding.dylib"
        module.write_bytes(b"old")
        os.utime(module, (1_000_000, 1_000_000))
        self.add_plugin(self.iso)
        self.write_stamp()
        self.assertTrue(bs._seed_valid(self.iso))
        os.utime(module, (2_000_000, 2_000_000))
        self.assertFalse(bs._seed_valid(self.iso))

    def test_repo_entitlements_edit_invalidates_seed(self):
        self.entitlements.write_text(
            "<dict><key>com.apple.security.app-sandbox</key><true/></dict>")
        self.assertFalse(bs._seed_valid(self.iso))

    def test_moved_head_invalidates_seed(self):
        self.release_commit = "abcdef124"
        self.assertFalse(bs._seed_valid(self.iso))


class SeedReleaseCommitTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name) / "repo"
        self.iso = self.repo / ".stripped-pak"
        self._old_repo = bs.REPO
        bs.REPO = self.repo
        (self.repo / "WoT.uproject").parent.mkdir(parents=True)
        (self.repo / "WoT.uproject").write_text("{}")
        for name in bs.SEED_COPY_SET:
            if name != "WoT.uproject":
                (self.repo / name).mkdir(parents=True)
        (self.repo / "ThirdParty").mkdir()
        (self.repo / "EnginePatches").mkdir()
        (self.repo / "Config" / "DefaultGame.ini").write_text(
            "[/Script/EngineSettings.GeneralProjectSettings]\n")
        self._git_commit_patch = mock.patch.object(
            bs, "_git_commit", return_value="abcdef123")
        self._git_commit_patch.start()
        self.source_snapshot = {
            "format": bs.SOURCE_SNAPSHOT_FORMAT,
            "git_commit": "abcdef123",
            "paths": list(bs.SOURCE_SNAPSHOT_PATHS),
            "tracked_diff_size": 0,
            "tracked_diff_sha256": "0" * 64,
            "untracked": [],
            "sha256": "1" * 64,
        }
        self._source_snapshot_patch = mock.patch.object(
            bs, "_source_snapshot",
            side_effect=lambda _root: dict(self.source_snapshot))
        self._source_snapshot_patch.start()
        self.write_neutral_support_authority()
        (self.repo / ".gitignore").write_text(
            "Tools/local.json\nTools/test/net/nodes.json\n"
            "**/__pycache__/\n*.log\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        subprocess.run(
            ["git", "-C", str(self.repo), "config", "user.email",
             "seed@example.invalid"], check=True)
        subprocess.run(
            ["git", "-C", str(self.repo), "config", "user.name", "Seed Test"],
            check=True)
        subprocess.run(
            ["git", "-C", str(self.repo), "add", "-A"], check=True)
        subprocess.run(
            ["git", "-C", str(self.repo), "commit", "-qm", "fixture"],
            check=True)

    def tearDown(self):
        self._source_snapshot_patch.stop()
        self._git_commit_patch.stop()
        bs.REPO = self._old_repo
        self._tmp.cleanup()

    def write_neutral_support_authority(self) -> list[dict]:
        packages = list(EXPECTED_NEUTRAL_PACKAGES)
        rows = []
        for index, package in enumerate(packages):
            relative = "Content" + package.removeprefix("/Game") + ".uasset"
            data = f"neutral-support-{index}".encode("ascii")
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            rows.append({
                "package": package,
                "relative_path": relative,
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
            })
        authority = {
            "format": "wot-neutral-support-asset-authority-v1",
            "roots": packages[:len(EXPECTED_NEUTRAL_ROOTS)],
            "hard_dependencies": packages[len(EXPECTED_NEUTRAL_ROOTS):],
            "assets": rows,
        }
        path = self.repo / "Tools" / "dist" / \
            "neutral-support-assets-v1.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(authority, indent=2, sort_keys=True) + "\n")
        return rows

    def test_seed_stamps_commit_and_human_version_into_project_and_stamp(self):
        self.assertEqual(bs.stage_seed(
            self.iso, release_sequence=12, human_version="0.1.2"), "ran")
        self.assertTrue(
            (self.iso / "Config" / "DefaultGame.ini").read_text().endswith(
                "\n[WoTNetVersion]\nReleaseCommit=abcdef123\n"))
        self.assertEqual(
            bs._read_stamp(self.iso, "seed")["release_commit"], "abcdef123")
        self.assertEqual(
            bs._read_stamp(self.iso, "seed")["release_sequence"], 12)
        self.assertEqual(
            bs._read_stamp(self.iso, "seed")["human_version"], "0.1.2")
        self.assertEqual(
            (self.iso / "Build" / "WoTRelease" / "release-identity.json")
            .read_bytes(),
            b'{"format":"wot-release-identity-v1",'
            b'"release_commit":"abcdef123","release_sequence":12,"version":1}')
        self.assertEqual(
            (self.iso / "Build" / "WoTRelease" / "release-version.json")
            .read_bytes(),
            b'{"format":"wot-release-version-v1",'
            b'"human_version":"0.1.2","version":1}')

    def test_release_sequence_change_invalidates_the_seed_and_downstream_steps(self):
        self.assertEqual(bs.stage_seed(self.iso, release_sequence=5), "ran")
        bs._write_stamp(self.iso, "import", {"success_marker": "complete"})
        self.assertEqual(bs.stage_seed(self.iso, release_sequence=5), "cached")
        self.assertEqual(bs.stage_seed(self.iso, release_sequence=6), "ran")
        self.assertIsNone(bs._read_stamp(self.iso, "import"))

    def test_legacy_empty_seed_without_release_version_is_reseeded(self):
        self.assertEqual(bs.stage_seed(self.iso), "ran")
        stamp = bs._read_stamp(self.iso, "seed")
        del stamp["human_version"]
        bs._write_stamp(self.iso, "seed", stamp)
        version_path = (
            self.iso / "Build" / "WoTRelease" / "release-version.json")
        version_path.unlink()
        bs._write_stamp(self.iso, "import", {"success_marker": "complete"})
        self.assertEqual(bs.stage_seed(self.iso, human_version=""), "ran")
        self.assertIsNone(bs._read_stamp(self.iso, "import"))
        self.assertEqual(
            version_path.read_bytes(),
            b'{"format":"wot-release-version-v1",'
            b'"human_version":"","version":1}')

    def test_stale_release_version_is_reseeded(self):
        self.assertEqual(
            bs.stage_seed(self.iso, human_version="0.1.2"), "ran")
        version_path = (
            self.iso / "Build" / "WoTRelease" / "release-version.json")
        version_path.write_bytes(
            b'{"format":"wot-release-version-v1",'
            b'"human_version":"0.2.0","version":1}')
        bs._write_stamp(self.iso, "import", {"success_marker": "complete"})
        self.assertEqual(
            bs.stage_seed(self.iso, human_version="0.1.2"), "ran")
        self.assertIsNone(bs._read_stamp(self.iso, "import"))
        self.assertEqual(
            version_path.read_bytes(),
            b'{"format":"wot-release-version-v1",'
            b'"human_version":"0.1.2","version":1}')

    def test_correct_release_version_is_cached(self):
        self.assertEqual(
            bs.stage_seed(self.iso, human_version="0.1.2"), "ran")
        self.assertEqual(
            bs.stage_seed(self.iso, human_version="0.1.2"), "cached")

    def write_repo_plugin(self) -> Path:
        plugin = self.repo / "Plugins" / "WoTOnboarding"
        files = {
            "WoTOnboarding.uplugin": b'{"FileVersion": 3}',
            "Source/WoTOnboarding/Private/A.cpp": b"int a;\n",
            "Binaries/Mac/libUnrealEditor-WoTOnboarding.dylib": b"dylib",
            "Binaries/Mac/UnrealEditor.modules": b'{"BuildId": "x"}',
            "Intermediate/Build/Mac/A.o": b"stale object",
        }
        for relative, data in files.items():
            path = plugin / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return plugin

    def test_seed_copies_the_code_plugin_without_its_intermediate(self):
        self.write_repo_plugin()
        self.assertEqual(bs.stage_seed(self.iso), "ran")
        seeded = self.iso / "Plugins" / "WoTOnboarding"
        self.assertEqual(
            (seeded / "WoTOnboarding.uplugin").read_bytes(),
            b'{"FileVersion": 3}')
        self.assertEqual(
            (seeded / "Source/WoTOnboarding/Private/A.cpp").read_bytes(),
            b"int a;\n")
        self.assertEqual(
            (seeded / "Binaries/Mac/libUnrealEditor-WoTOnboarding.dylib")
            .read_bytes(), b"dylib")
        self.assertFalse((seeded / "Intermediate").exists())
        self.assertEqual(
            bs._read_stamp(self.iso, "seed")["code_plugins"],
            ["WoTOnboarding"])
        self.assertEqual(bs.stage_seed(self.iso), "cached")

    def test_reseed_replaces_the_whole_iso_plugin_copy(self):
        plugin = self.write_repo_plugin()
        self.assertEqual(bs.stage_seed(self.iso, release_sequence=1), "ran")
        seeded = self.iso / "Plugins" / "WoTOnboarding"
        # An ISO build's objects and a source file the repo since deleted.
        (seeded / "Intermediate" / "Build").mkdir(parents=True)
        (seeded / "Intermediate" / "Build" / "B.o").write_bytes(b"iso object")
        (seeded / "Source" / "WoTOnboarding" / "Gone.cpp").write_bytes(b"x")
        (plugin / "Source/WoTOnboarding/Private/A.cpp").write_bytes(b"int b;\n")
        self.assertEqual(bs.stage_seed(self.iso, release_sequence=2), "ran")
        self.assertFalse((seeded / "Intermediate").exists())
        self.assertFalse((seeded / "Source" / "WoTOnboarding" / "Gone.cpp")
                         .exists())
        self.assertEqual(
            (seeded / "Source/WoTOnboarding/Private/A.cpp").read_bytes(),
            b"int b;\n")

    def test_a_tree_without_the_plugin_seeds_none_and_drops_an_old_copy(self):
        self.write_repo_plugin()
        self.assertEqual(bs.stage_seed(self.iso, release_sequence=1), "ran")
        shutil.rmtree(self.repo / "Plugins" / "WoTOnboarding")
        # The repo lost the plugin, so the old seed no longer matches it.
        self.assertEqual(bs.stage_seed(self.iso, release_sequence=1), "ran")
        self.assertFalse((self.iso / "Plugins" / "WoTOnboarding").exists())
        self.assertEqual(bs._read_stamp(self.iso, "seed")["code_plugins"], [])

    def test_seed_refuses_a_symlinked_iso_plugins_directory(self):
        self.write_repo_plugin()
        outside = self.repo.parent / "outside"
        (outside / "WoTOnboarding").mkdir(parents=True)
        (outside / "WoTOnboarding" / "keep.txt").write_text("keep")
        self.iso.mkdir(parents=True)
        (self.iso / "Plugins").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(bs.BuildFailure, "crosses a symlink"):
            bs.stage_seed(self.iso)
        self.assertTrue((outside / "WoTOnboarding" / "keep.txt").is_file())

    def test_human_version_change_invalidates_seed_and_downstream_steps(self):
        self.assertEqual(bs.stage_seed(self.iso), "ran")
        bs._write_stamp(self.iso, "import", {"success_marker": "complete"})
        self.assertEqual(
            bs.stage_seed(self.iso, human_version="0.1.2"), "ran")
        self.assertIsNone(bs._read_stamp(self.iso, "import"))

    def test_release_sequence_argument_accepts_only_non_negative_int64(self):
        parsed = bs.parser().parse_args([
            "--cache", str(self.repo / "cache"), "--release-sequence", "5"])
        self.assertEqual(parsed.release_sequence, 5)
        for value in ("-1", str(2 ** 63)):
            with self.subTest(value=value), self.assertRaises(SystemExit):
                bs.parser().parse_args([
                    "--cache", str(self.repo / "cache"),
                    "--release-sequence", value])

    def test_human_version_argument_defaults_empty_and_validates_at_parse_time(self):
        parsed = bs.parser().parse_args([
            "--cache", str(self.repo / "cache")])
        self.assertEqual(parsed.human_version, "")
        parsed = bs.parser().parse_args([
            "--cache", str(self.repo / "cache"),
            "--human-version", "0.1.2"])
        self.assertEqual(parsed.human_version, "0.1.2")
        for value in ("v0.1.2", "1" + "a" * 32):
            with self.subTest(value=value), self.assertRaises(SystemExit):
                bs.parser().parse_args([
                    "--cache", str(self.repo / "cache"),
                    "--human-version", value])

    def test_guarded_summary_reports_requested_human_version(self):
        args = SimpleNamespace(
            guarded=True, configuration="Test", platform="Win64",
            cache=self.repo / "cache", release_sequence=12,
            human_version="0.1.2")
        containers = {"container_count": 0, "total_bytes": 0}
        with mock.patch.object(bs, "_container_summary", return_value=containers), \
                mock.patch.object(bs, "_cache_summary", return_value={}):
            path = bs.write_summary(args, self.iso, {})
        self.assertEqual(
            json.loads(path.read_text(encoding="utf-8"))["human_version"],
            "0.1.2")

    def test_ignored_machine_files_never_enter_or_invalidate_guarded_seed(self):
        local = self.repo / "Tools" / "local.json"
        nodes = self.repo / "Tools" / "test" / "net" / "nodes.json"
        cache = self.repo / "Tools" / "dist" / "__pycache__" / "private.pyc"
        log = self.repo / "Tools" / "dist" / "logs" / "private.log"
        for path, contents in (
                (local, '{"source_engine_root_mac":"/private/one"}'),
                (nodes, '{"host":"private"}'),
                (cache, "cached"), (log, "private log")):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")

        self.assertEqual(bs.stage_seed(self.iso, guarded=True), "ran")

        for relative in (
                "Tools/local.json", "Tools/test/net/nodes.json",
                "Tools/dist/__pycache__/private.pyc",
                "Tools/dist/logs/private.log"):
            self.assertFalse((self.iso / relative).exists(), relative)
        local.write_text(
            '{"source_engine_root_mac":"/private/two"}', encoding="utf-8")
        self.assertEqual(bs.stage_seed(self.iso, guarded=True), "cached")

    def test_guarded_seed_rejects_missing_neutral_support_authority(self):
        (self.repo / "Tools" / "dist" /
         "neutral-support-assets-v1.json").unlink()
        with self.assertRaisesRegex(bs.BuildFailure, "neutral support authority"):
            bs.stage_seed(self.iso, guarded=True)

    def test_guarded_seed_binds_exact_neutral_support_authority(self):
        self.write_neutral_support_authority()

        self.assertEqual(bs.stage_seed(self.iso, guarded=True), "ran")

        stamp = bs._read_stamp(self.iso, "seed")
        self.assertIn("neutral_support_authority", stamp)
        receipt = stamp["neutral_support_authority"]
        self.assertEqual(receipt["format"],
                         "wot-neutral-support-asset-receipt-v1")
        self.assertEqual(receipt["asset_count"], 45)
        self.assertEqual(receipt["roots"], list(EXPECTED_NEUTRAL_ROOTS))
        self.assertEqual(receipt["hard_dependencies"],
                         list(EXPECTED_NEUTRAL_HARD_DEPENDENCIES))

    def test_guarded_seed_rejects_tampered_neutral_support_asset(self):
        rows = self.write_neutral_support_authority()
        (self.repo / rows[3]["relative_path"]).write_bytes(
            b"x" * rows[3]["size"])

        with self.assertRaisesRegex(bs.BuildFailure, "sha256"):
            bs.stage_seed(self.iso, guarded=True)

    def test_guarded_seed_rejects_duplicate_neutral_support_row(self):
        rows = self.write_neutral_support_authority()
        path = self.repo / "Tools" / "dist" / \
            "neutral-support-assets-v1.json"
        authority = json.loads(path.read_text())
        authority["assets"][-1] = dict(rows[0])
        path.write_text(json.dumps(authority, indent=2, sort_keys=True) + "\n")

        with self.assertRaisesRegex(bs.BuildFailure, "exact assets"):
            bs.stage_seed(self.iso, guarded=True)

    def test_guarded_seed_rejects_reordered_neutral_support_roots(self):
        self.write_neutral_support_authority()
        path = self.repo / "Tools" / "dist" / \
            "neutral-support-assets-v1.json"
        authority = json.loads(path.read_text())
        authority["roots"][0], authority["roots"][1] = \
            authority["roots"][1], authority["roots"][0]
        path.write_text(json.dumps(authority, indent=2, sort_keys=True) + "\n")

        with self.assertRaisesRegex(bs.BuildFailure, "ordered roots"):
            bs.stage_seed(self.iso, guarded=True)

    def test_neutral_support_authority_digest_ignores_crlf_checkout(self):
        # Regression: a Windows autocrlf checkout recorded a CRLF digest
        # the first-run funnel (canonical LF pretty JSON) always refused.
        self.write_neutral_support_authority()
        path = self.repo / "Tools" / "dist" / "neutral-support-assets-v1.json"
        lf = path.read_bytes().replace(b"\r\n", b"\n")
        path.write_bytes(lf)
        lf_receipt = bs._neutral_support_asset_receipt(self.repo)
        path.write_bytes(lf.replace(b"\n", b"\r\n"))
        crlf_receipt = bs._neutral_support_asset_receipt(self.repo)
        self.assertEqual(crlf_receipt, lf_receipt)
        self.assertEqual(lf_receipt["authority_sha256"],
                         hashlib.sha256(lf).hexdigest())

    def test_neutral_support_authority_must_be_canonical_json(self):
        self.write_neutral_support_authority()
        path = self.repo / "Tools" / "dist" / "neutral-support-assets-v1.json"
        authority = json.loads(path.read_text())
        path.write_text(json.dumps(authority, sort_keys=True) + "\n")
        with self.assertRaisesRegex(bs.BuildFailure, "canonical"):
            bs._neutral_support_asset_receipt(self.repo)

    def test_neutral_support_authority_accepts_isolated_hardlinks(self):
        rows = self.write_neutral_support_authority()
        isolated = self.repo / "isolated"
        authority_relative = Path(
            "Tools/dist/neutral-support-assets-v1.json")
        authority = isolated / authority_relative
        authority.parent.mkdir(parents=True, exist_ok=True)
        authority.write_bytes((self.repo / authority_relative).read_bytes())
        for row in rows:
            source = self.repo / row["relative_path"]
            target = isolated / row["relative_path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            os.link(source, target)

        self.assertEqual(
            bs._neutral_support_asset_receipt(isolated),
            bs._neutral_support_asset_receipt(self.repo))

    def test_seed_omits_checked_in_bootstrap_map_for_import_generation(self):
        bootstrap = self.repo / bs.IMPORT_GENERATED_OUTPUTS[0]
        bootstrap.parent.mkdir(parents=True, exist_ok=True)
        bootstrap.write_bytes(b"checked-in generated map")
        self.assertEqual(bs.stage_seed(self.iso, guarded=True), "ran")
        self.assertTrue(bootstrap.is_file())
        self.assertFalse((self.iso / bs.IMPORT_GENERATED_OUTPUTS[0]).exists())

    def test_guarded_seed_removes_broad_always_cook_roots_only_in_iso(self):
        config = self.repo / "Config" / "DefaultGame.ini"
        original = (
            "[/Script/UnrealEd.ProjectPackagingSettings]\n"
            "+DirectoriesToAlwaysCook=(Path=\"/Game/WoT/Maps/LightTables\")\n"
            "+DirectoriesToAlwaysCook=(Path=\"/Game/WoT/Data\")\n"
            "+DirectoriesToAlwaysCook=(Path=\"/Game/WoT\")\n"
        )
        config.write_text(original)

        self.assertEqual(bs.stage_seed(self.iso, guarded=True), "ran")

        isolated = (self.iso / "Config" / "DefaultGame.ini").read_text()
        self.assertNotIn("+DirectoriesToAlwaysCook", isolated)
        self.assertEqual(
            isolated.count("!DirectoriesToAlwaysCook=ClearArray"), 1)
        self.assertEqual(config.read_text(), original)

    def test_legacy_seed_preserves_checked_in_bootstrap_map(self):
        bootstrap = self.repo / bs.IMPORT_GENERATED_OUTPUTS[0]
        bootstrap.parent.mkdir(parents=True, exist_ok=True)
        bootstrap.write_bytes(b"checked-in generated map")

        self.assertEqual(bs.stage_seed(self.iso), "ran")

        self.assertEqual(
            (self.iso / bs.IMPORT_GENERATED_OUTPUTS[0]).read_bytes(),
            b"checked-in generated map")
        self.assertNotIn(
            "import_generated_outputs", bs._read_stamp(self.iso, "seed"))

    def test_seed_refuses_repo_network_version_section(self):
        (self.repo / "Config" / "DefaultGame.ini").write_text(
            "[WoTNetVersion]\nReleaseCommit=must-not-live-here\n")
        with self.assertRaises(bs.BuildFailure):
            bs.stage_seed(self.iso)

    def test_guarded_seed_adds_compile_inputs_without_invalidating_import(self):
        self.assertEqual(bs.stage_seed(self.iso, guarded=True), "ran")
        shutil_target = self.iso / "ThirdParty"
        import_marker = self.iso / "import-preserved.txt"
        import_marker.write_text("expensive generated content")
        shutil_target.rmdir()
        stamp = bs._read_stamp(self.iso, "seed")
        stamp["copy_set"] = list(bs.SEED_COPY_SET)
        bs._write_json(bs._stamp_path(self.iso, "seed"), stamp)
        bs._write_stamp(self.iso, "import", {"success_marker": "ok"})
        bs._write_stamp(self.iso, "marker", {"config": "ok"})
        bs._write_stamp(self.iso, "cook", {"command_exit_code": 0})
        makefile = (self.iso / "Intermediate" / "Build" / "Mac" / "arm64" /
                    "WoT" / "Test" / "Makefile.bin")
        makefile.parent.mkdir(parents=True)
        makefile.write_bytes(b"cached before ThirdParty existed")
        vendor = self.repo / "ThirdParty" / "vendor.h"
        vendor.write_text("required build input")

        self.assertEqual(bs.stage_seed(self.iso, guarded=True), "ran")

        self.assertEqual(import_marker.read_text(), "expensive generated content")
        self.assertEqual((self.iso / "ThirdParty" / "vendor.h").read_text(),
                         "required build input")
        self.assertIsNotNone(bs._read_stamp(self.iso, "import"))
        self.assertIsNotNone(bs._read_stamp(self.iso, "marker"))
        self.assertIsNone(bs._read_stamp(self.iso, "cook"))
        self.assertFalse(makefile.exists())

    def test_pre_snapshot_seed_is_reseeded_instead_of_upgraded(self):
        self.assertEqual(bs.stage_seed(self.iso), "ran")
        bs._write_stamp(self.iso, "import", {"success_marker": "legacy"})

        self.assertEqual(bs.stage_seed(self.iso, guarded=True), "ran")

        self.assertIsNone(bs._read_stamp(self.iso, "import"))
        stamp = bs._read_stamp(self.iso, "seed")
        self.assertEqual(
            stamp["source_snapshot_method"],
            "file-copy-with-before-and-after-snapshot-v1")
        self.assertEqual(stamp["source_snapshot"], self.source_snapshot)


class SourceSnapshotTests(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory()
        self.repo = Path(self._temporary.name) / "repo"
        (self.repo / "Source").mkdir(parents=True)
        (self.repo / "Tools").mkdir()
        (self.repo / "Source" / "tracked.cpp").write_text("tracked v1\n")
        (self.repo / "Tools" / "tracked.py").write_text("tracked v1\n")
        subprocess.run(
            ["git", "init", "-q", str(self.repo)], check=True)
        subprocess.run(
            ["git", "-C", str(self.repo), "config", "user.email",
             "snapshot@example.invalid"], check=True)
        subprocess.run(
            ["git", "-C", str(self.repo), "config", "user.name",
             "Snapshot Test"], check=True)
        subprocess.run(
            ["git", "-C", str(self.repo), "add", "Source", "Tools"],
            check=True)
        subprocess.run(
            ["git", "-C", str(self.repo), "commit", "-qm", "fixture"],
            check=True)
        self.addCleanup(self._temporary.cleanup)

    def test_snapshot_binds_tracked_diff_and_untracked_inventory(self):
        clean = bs._source_snapshot(self.repo)
        (self.repo / "Source" / "tracked.cpp").write_text("tracked v2\n")
        untracked = self.repo / "Tools" / "new-tool.py"
        untracked.write_text("print('one')\n")

        dirty = bs._source_snapshot(self.repo)

        self.assertNotEqual(clean["sha256"], dirty["sha256"])
        self.assertGreater(dirty["tracked_diff_size"], 0)
        self.assertEqual(
            dirty["untracked"],
            [{
                "path": "Tools/new-tool.py",
                "type": "file",
                "size": untracked.stat().st_size,
                "sha256": bs._sha256_file(untracked),
            }])
        untracked.write_text("print('two')\n")
        self.assertNotEqual(
            dirty["sha256"], bs._source_snapshot(self.repo)["sha256"])

    def test_snapshot_binds_the_seeded_code_plugins(self):
        self.assertIn("Plugins/WoTOnboarding", bs.SOURCE_SNAPSHOT_PATHS)
        clean = bs._source_snapshot(self.repo)
        self.assertEqual(clean["paths"], list(bs.SOURCE_SNAPSHOT_PATHS))
        plugin = self.repo / "Plugins" / "WoTOnboarding"
        (plugin / "Source").mkdir(parents=True)
        source = plugin / "Source" / "New.cpp"
        source.write_text("int a;\n")
        # UBT outputs are ignored machine state, never source.
        (self.repo / ".gitignore").write_text("Binaries/\nIntermediate/\n")
        (plugin / "Binaries").mkdir()
        (plugin / "Binaries" / "x.dylib").write_bytes(b"built")
        dirty = bs._source_snapshot(self.repo)
        self.assertNotEqual(clean["sha256"], dirty["sha256"])
        self.assertEqual(
            [row["path"] for row in dirty["untracked"]],
            ["Plugins/WoTOnboarding/Source/New.cpp"])


class ReleasePackagingTests(unittest.TestCase):
    def test_asset_name_binds_platform_and_commit(self):
        self.assertEqual(bs._release_asset_name("Mac", "abc1234"),
                         "WoT-Mac-stripped-abc1234.zip")

    def test_manifest_binds_hash_audit_and_provenance(self):
        audit_stamp = {"verdict": "pass",
                       "cache_sha256": "c" * 64,
                       "container_count": 10}
        manifest = bs._release_manifest(
            asset_name="WoT-Mac-stripped-abc1234.zip",
            sha256="a" * 64, size=123, platform="Mac", commit="abc1234",
            audit_stamp=audit_stamp)
        self.assertEqual(manifest["format"], "wot-stripped-release-v1")
        self.assertEqual(manifest["asset"], "WoT-Mac-stripped-abc1234.zip")
        self.assertEqual(manifest["sha256"], "a" * 64)
        self.assertEqual(manifest["size"], 123)
        self.assertEqual(manifest["platform"], "Mac")
        self.assertEqual(manifest["git_commit"], "abc1234")
        self.assertEqual(manifest["audit"]["verdict"], "pass")
        self.assertEqual(manifest["audit"]["cache_sha256"], "c" * 64)

    def test_release_v1_synthetic_manifest_bytes_are_frozen(self):
        """Frozen migration oracle: release-manifest-v1 canonical bytes.

        release-manifest-v2 must be able to prove what v1 emitted, so the
        canonical serialization is pinned byte-for-byte.  `generated_utc` is
        the one non-deterministic field and is overwritten before comparison.
        """
        value = bs._release_manifest(
            asset_name="WoT-Mac-stripped-abc123456.zip",
            sha256="a" * 64, size=123, platform="Mac", commit="abc123456",
            audit_stamp={"verdict": "pass", "cache_sha256": "c" * 64,
                         "container_count": 4})
        value["generated_utc"] = "2026-07-24T00:00:00+00:00"
        self.assertEqual(bs._canonical_release_bytes(value),
                         (FIXTURES / "release-v1-oracle.json").read_bytes())

    def test_manifest_refuses_non_pass_audit(self):
        with self.assertRaises(bs.BuildFailure):
            bs._release_manifest(
                asset_name="x.zip", sha256="a" * 64, size=1, platform="Mac",
                commit="abc1234", audit_stamp={"verdict": "not-run"})

    def test_package_step_is_last_and_gated_on_audit(self):
        test_chain = [step for step in bs.STEPS if step not in bs.SHIPPING_STEPS]
        self.assertEqual(test_chain[-1], "package")
        self.assertEqual(bs.PREREQUISITES["package"], ("audit",))
        # The release branch hangs off the Test cook and ends on a package
        # step that also binds the Test package (its funnel twin).
        self.assertEqual(bs.STEPS[-1], "shipping-package")
        self.assertEqual(bs.PREREQUISITES["shipping-stage"], ("cook",))
        self.assertEqual(bs.PREREQUISITES["shipping-package"],
                         ("shipping-audit", "package"))

    def test_invalidation_follows_dependencies_not_step_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            iso = Path(temporary)
            for step in bs.STEPS:
                bs._write_stamp(iso, step, {})
            bs._invalidate_from(iso, "package")
            remaining = {step for step in bs.STEPS
                         if bs._read_stamp(iso, step) is not None}
            # Re-packaging Test never throws away the Shipping build/stage.
            self.assertEqual(remaining, set(bs.STEPS) - {
                "package", "shipping-package"})
            bs._invalidate_from(iso, "cook")
            remaining = {step for step in bs.STEPS
                         if bs._read_stamp(iso, step) is not None}
            self.assertEqual(remaining, {"seed", "import", "marker"})
        self.assertEqual(bs._dependent_steps("seed"), bs.STEPS)
        self.assertEqual(bs._dependent_steps("audit"),
                         ("audit", "package", "shipping-package"))

    def test_package_refuses_commit_different_from_cooked_seed(self):
        with tempfile.TemporaryDirectory() as temporary:
            iso = Path(temporary)
            bs._write_stamp(iso, "seed", {"release_commit": "abcdef123"})
            args = mock.Mock(platform="Mac")
            with mock.patch.object(bs, "_git_commit", return_value="abcdef124"):
                with self.assertRaises(bs.BuildFailure):
                    bs.stage_package(args, iso)


class GuardedModeTests(unittest.TestCase):
    def setUp(self):
        dmg_builder = mock.patch.object(
            bs.mac_dmg, "build_dmg", side_effect=write_fake_mac_dmg)
        dmg_builder.start()
        self.addCleanup(dmg_builder.stop)
        self._tmp = tempfile.TemporaryDirectory()
        # Resolved: macOS temp dirs live under /var, a symlink to /private/var.
        self.root = Path(self._tmp.name).resolve()
        self.addCleanup(self._tmp.cleanup)
        self.repo = self.root / "repo"
        self._old_repo = bs.REPO
        bs.REPO = self.repo
        self.addCleanup(setattr, bs, "REPO", self._old_repo)
        self.write_neutral_support_assets(self.repo)
        self._pinned = mock.patch.dict(
            bs.PINNED_SOURCE_ENGINES, {"Mac": self.root / "engine"})
        self._pinned.start()
        self.addCleanup(self._pinned.stop)

    def write_neutral_support_assets(self, root: Path) -> dict:
        packages = list(EXPECTED_NEUTRAL_PACKAGES)
        rows = []
        for index, package in enumerate(packages):
            relative = "Content" + package.removeprefix("/Game") + ".uasset"
            data = f"guarded-neutral-support-{index}".encode("ascii")
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            rows.append({
                "package": package,
                "relative_path": relative,
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
            })
        authority = {
            "format": "wot-neutral-support-asset-authority-v1",
            "roots": packages[:len(EXPECTED_NEUTRAL_ROOTS)],
            "hard_dependencies": packages[len(EXPECTED_NEUTRAL_ROOTS):],
            "assets": rows,
        }
        path = root / "Tools" / "dist" / \
            "neutral-support-assets-v1.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(authority, indent=2, sort_keys=True) + "\n")
        return bs._neutral_support_asset_receipt(root)

    def provision_neutral_support_lifecycle(
            self, iso: Path, *, imported: bool = True) -> dict:
        receipt = self.write_neutral_support_assets(iso)
        bs._write_stamp(iso, "seed", {
            "neutral_support_authority": receipt,
        })
        if imported:
            bs._write_stamp(iso, "import", {
                "neutral_support_authority": receipt,
            })
        return receipt

    def args(self, **overrides):
        values = {
            "guarded": True,
            "configuration": None,
            "platform": "Mac",
            "engine": None,
        }
        values.update(overrides)
        return SimpleNamespace(**values)

    @staticmethod
    def neutral_support_package_option() -> str:
        return "-PACKAGE=" + "+".join(EXPECTED_NEUTRAL_ROOTS)

    @staticmethod
    def guarded_cooked_paths() -> list[str]:
        return [
            "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin",
            "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap",
            "../../../WoT/Content/WoT/Maps/WoTFrontEnd.umap",
            *[
                "../../../WoT/Plugins/WoTNeutralShells/Content/"
                f"WoTMapShell{slot:02d}.umap"
                for slot in range(1, 20)
            ],
            *[
                "../../../WoT/Content" + package.removeprefix("/Game")
                + ".uasset"
                for package in bs.NEUTRAL_SUPPORT_PACKAGES
            ],
        ]

    def receipt_stamp(self, *, maps=None):
        iso = self.root / "iso"
        neutral_support_authority = self.provision_neutral_support_lifecycle(
            iso)
        engine_identity = {
            "commit": "e" * 40,
            "tag": "5.8.0-release",
            "engine": "/pinned/source",
            "toolchain": {"apple_clang": "17"},
            "checks": ["commit-matches-pin"],
        }
        rows = {}
        for target in bs.GUARDED_RECEIPT_TARGETS:
            engine_path = self.root / f"{target}-engine.json"
            compiled_path = self.root / f"{target}-compiled.json"
            engine = {
                "format": "wot-ue58-source-receipt-v1",
                "platform": "Mac",
                **engine_identity,
            }
            definitions = ({
                "WOT_GUARDED_BOOTSTRAP": "1",
                "WOT_BOOTSTRAP_TEST_IDENTITIES": "1",
                "WOT_BOOTSTRAP_TEST_ENDPOINTS": "1",
                **bs.GUARDED_GAME_PROFILE_DEFINITIONS,
            } if target == "WoT" else {
                "WOT_GUARDED_BOOTSTRAP": "0",
                "WOT_BOOTSTRAP_TEST_IDENTITIES": "1",
            })
            checks = (["guard-definitions-present",
                       "test-only-definitions-present"] if target == "WoT"
                      else ["unguarded-test-program-target",
                            "guard-definitions-absent",
                            "test-only-definitions-present"])
            compiled = {
                "format": "wot-guarded-build-receipt-v1",
                "target": target,
                "platform": "Mac",
                "configuration": "Test",
                "definitions": definitions,
                "checks": checks,
                "engine": engine_identity,
            }
            bs._write_json(engine_path, engine)
            bs._write_json(compiled_path, compiled)
            rows[target] = {
                "engine": str(engine_path),
                "compiled": str(compiled_path),
                "engine_sha256": bs._sha256_file(engine_path),
                "compiled_sha256": bs._sha256_file(compiled_path),
            }
        tool = self.root / "UnrealPak"
        tool.write_bytes(b"synthetic-unrealpak")
        container = self.root / "iso" / "staged" / "Paks" / "test.utoc"
        container.parent.mkdir(parents=True, exist_ok=True)
        container.write_bytes(b"synthetic-container")
        cooked_paths = [
            "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin",
            "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap",
            "../../../WoT/Content/WoT/Maps/WoTFrontEnd.umap",
            *[
                "../../../WoT/Plugins/WoTNeutralShells/Content/"
                f"WoTMapShell{slot:02d}.umap"
                for slot in range(1, 20)
            ],
            *[
                "../../../WoT/Content" + package.removeprefix("/Game")
                + ".uasset" for package in EXPECTED_NEUTRAL_PACKAGES
            ],
        ]
        validation = bs._validate_guarded_cooked_paths(cooked_paths)
        inventory = {
            "format": bs.COOKED_CONTAINER_RECEIPT_FORMAT,
            "platform": "Mac",
            "configuration": "Test",
            "tool": {
                "path": str(tool),
                "size": tool.stat().st_size,
                "sha256": bs._sha256_file(tool),
            },
            "containers": [{
                "path": "Paks/test.utoc",
                "size": container.stat().st_size,
                "sha256": bs._sha256_file(container),
                "entry_count": len(cooked_paths),
            }],
            "paths": cooked_paths,
            "path_count": len(cooked_paths),
            "validation": validation,
        }
        inventory_path = self.root / "iso" / "receipts" / \
            "cooked-container-inventory.json"
        bs._write_json(inventory_path, inventory)
        return {
            "mode": "guarded",
            "configuration": "Test",
            "platform": "Mac",
            "engine": str(self.root / "engine"),
            "log": str(self.root / "iso" / "logs" / "cook.log"),
            "maps": list(maps if maps is not None
                         else bs._cook_maps(guarded=True)),
            "neutral_support_authority": neutral_support_authority,
            "target_receipts": rows,
            "cooked_container_inventory": {
                "path": str(inventory_path),
                "sha256": bs._sha256_file(inventory_path),
                "path_count": len(cooked_paths),
                "validation": validation,
            },
        }

    def neutral_shell_generation(self, iso: Path) -> Path:
        iso.mkdir(parents=True, exist_ok=True)
        project = iso / "WoT.uproject"
        if not project.exists():
            project.write_text(json.dumps({
                "FileVersion": 3,
                "Plugins": [],
            }), encoding="utf-8")
        output = iso / "NeutralMapShells"
        output.mkdir(parents=True)
        rows = []
        census = hashlib.sha256()
        for slot in range(1, 20):
            name = f"WoTMapShell{slot:02d}"
            data = f"neutral-shell-{slot:02d}".encode("ascii")
            path = output / f"{name}.umap"
            path.write_bytes(data)
            row = {
                "bytes": len(data),
                "package": f"/WoTNeutralShells/{name}",
                "relative_path": f"{name}.umap",
                "sha256": hashlib.sha256(data).hexdigest(),
                "slot": slot,
            }
            rows.append(row)
            census.update(json.dumps(
                row, sort_keys=True, separators=(",", ":"),
                ensure_ascii=True).encode("ascii"))
        receipt = {
            "format": "neutral-map-shells-output-identity-receipt-v2",
            "package_census_sha256": census.hexdigest(),
            "package_count": 19,
            "packages": rows,
            "root_began_absent": True,
            "root_identity": str(output.resolve()),
        }
        receipt_path = iso / bs.NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT
        receipt_path.parent.mkdir(parents=True)
        receipt_path.write_bytes(json.dumps(
            receipt, sort_keys=True, separators=(",", ":"),
            ensure_ascii=True).encode("ascii") + b"\n")
        return receipt_path

    @staticmethod
    def seed_production_descriptor(iso: Path, staged_app: Path) -> None:
        """Required checked-in NonUFS files staged where a Mac package carries
        them (the package step refuses a game without the descriptor/identity)."""
        data = b'{"format": "cache-v3-production-descriptor-v1"}\n'
        relative = Path("Build", "WoTRelease", "cache-v3-production-descriptor.json")
        (iso / relative).parent.mkdir(parents=True, exist_ok=True)
        (iso / relative).write_bytes(data)
        builder = iso / "Source" / "WoTRetailReader" / "Private" / "CacheBuilder.cpp"
        builder.parent.mkdir(parents=True, exist_ok=True)
        builder.write_text(
            "constexpr TCHAR WoTCacheExpectedProductionDescriptorSha256[] =\n"
            f'\tTEXT("{hashlib.sha256(data).hexdigest()}");\n', encoding="utf-8")
        staged = staged_app / "Contents" / "UE" / "WoT" / relative
        staged.parent.mkdir(parents=True, exist_ok=True)
        staged.write_bytes(data)
        identity_relative = Path(
            "Build", "WoTRelease", "release-identity.json")
        identity = (b'{"format":"wot-release-identity-v1",'
                    b'"release_commit":"abc123456",'
                    b'"release_sequence":0,"version":1}')
        (iso / identity_relative).write_bytes(identity)
        (staged_app / "Contents" / "UE" / "WoT" /
         identity_relative).write_bytes(identity)
        version_relative = Path(
            "Build", "WoTRelease", "release-version.json")
        version = (b'{"format":"wot-release-version-v1",'
                   b'"human_version":"","version":1}')
        (iso / version_relative).write_bytes(version)
        (staged_app / "Contents" / "UE" / "WoT" /
         version_relative).write_bytes(version)

    def guarded_evidence(self):
        iso = self.root / "iso"
        game = iso / "staged" / "Mac" / "WoT.app" / \
            "Contents" / "MacOS" / "WoT-Mac-Test"
        updater = iso / "staged" / "Mac" / "WoT.app" / \
            "Contents" / "Helpers" / "WoTUpdater"
        game.parent.mkdir(parents=True)
        updater.parent.mkdir(parents=True)
        game.write_bytes(b"game-final")
        updater.write_bytes(b"updater-final")
        self.seed_production_descriptor(iso, iso / "staged" / "Mac" / "WoT.app")
        with (game.parents[1] / "Info.plist").open("wb") as handle:
            plistlib.dump({"CFBundleExecutable": game.name}, handle)
        cook_stamp = self.receipt_stamp()
        cook_stamp["product_receipt"] = bs._write_guarded_product_receipt(
            iso, "Mac", iso / "staged" / "Mac",
            cook_stamp["target_receipts"])
        engine = self.root / "engine"
        pinned_zen = engine / "Engine" / "Binaries" / "Mac" / "zenserver"
        pinned_zen.parent.mkdir(parents=True)
        pinned_zen.write_bytes(b"pinned zen")
        editor = pinned_zen.with_name("UnrealEditor-Cmd")
        editor.write_bytes(b"editor")
        runuat = engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.sh"
        runuat.parent.mkdir(parents=True)
        runuat.write_bytes(b"runuat")
        (iso / "WoT.uproject").write_text("{}", encoding="utf-8")
        zen_path = iso / "receipts" / "zen-lifetime.json"
        owned_data_dir = iso / "Saved" / "GuardedZenSponsor-123"
        sponsor_command = [
            str(pinned_zen), "--port", "18558",
            "--allow-port-probing=false", "--http-forceloopback",
            "--data-dir", str(owned_data_dir), "--owner-pid", "123",
            "--detach=false", "--no-sentry", "--quiet",
        ]
        command_rows = []
        for stage, kind in (
                ("import", "editor"),
                ("finalize-stripped-fractal-catalog", "editor"),
                ("generate-neutral-map-shells", "editor"),
                ("cook", "cook")):
            log = iso / "logs" / f"{stage}.log"
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(
                "Zen server ready at http://127.0.0.1:18558/\n",
                encoding="utf-8")
            script_name = {
                "import": "import_all.py",
                "finalize-stripped-fractal-catalog":
                    "finalize_stripped_fractal_catalog.py",
                "generate-neutral-map-shells":
                    "generate_neutral_map_shells.py",
            }.get(stage)
            script_argument = None
            if kind == "editor":
                script_argument = "-script=" + str(
                    iso / "Tools" / "import" / script_name).replace(
                        "\\", "/")
                if stage != "finalize-stripped-fractal-catalog":
                    script_argument += " --fixture=1"
            command = ([
                str(editor), str(iso / "WoT.uproject"),
                "-run=pythonscript",
                script_argument,
                "-ddc=(ZenLocal)", bs.ZEN_NO_AUTO_LAUNCH,
            ] if kind == "editor" else [
                str(runuat), "BuildCookRun",
                f"-project={iso / 'WoT.uproject'}", "-platform=Mac",
                "-clientconfig=Test", "-build", "-cook", "-stage", "-pak",
                f"-stagingdirectory={iso / 'staged'}",
                "-map=" + "+".join(cook_stamp["maps"]),
                "-AdditionalCookerOptions=-nullrhi -skipzenstore "
                "-ddc=(ZenLocal) " + bs.ZEN_NO_AUTO_LAUNCH + " "
                + self.neutral_support_package_option(),
            ])
            command_rows.append({
                "stage": stage, "kind": kind, "command": command,
                "log": str(log), "log_sha256": bs._sha256_file(log),
                "exit_code": 0,
                "stripped_ddc_environment_overrides":
                    ["UE-LocalDataCachePath"],
            })
        zen_receipt = {
            "format": bs.ZEN_RECEIPT_FORMAT,
            "mechanism": "pipeline-sponsored-zen-v3",
            "unreal_isolation_switches": [
                "-ddc=(ZenLocal)", bs.ZEN_NO_AUTO_LAUNCH],
            "endpoint": bs.ZEN_SPONSOR_ENDPOINT,
            "owner": {"kind": "pipeline-process", "pid": 123},
            "sponsor": {
                "pid": 321,
                "command": sponsor_command,
                "log": "/iso/logs/zen-sponsor.log",
            },
            "version": {
                "path": str(pinned_zen),
                "size": pinned_zen.stat().st_size,
                "sha256": bs._sha256_file(pinned_zen),
            },
            "before": [],
            "endpoint_preflight": {
                "host": "127.0.0.1", "port": 18558,
                "exclusive_bind_succeeded": True,
                "checked_utc": "2026-07-29T00:00:00+00:00",
            },
            "health": {
                "url": bs.ZEN_SPONSOR_ENDPOINT + "/health/ready",
                "status_code": 200,
                "body_sha256": "3" * 64,
                "checked_utc": "2026-07-29T00:00:00+00:00",
            },
            "processes_after_lifecycle": [{
                "pid": 321,
                "command": " ".join(sponsor_command),
            }],
            "monitor": {
                "interval_seconds": 0.1, "sample_count": 2,
                "started_utc": "2026-07-29T00:00:00+00:00",
                "finished_utc": "2026-07-29T00:01:00+00:00",
                "violation": None,
            },
            "unreal_commands": command_rows,
            "environment_policy": "strip-all-ue-datacache-overrides-v1",
            "stripped_ddc_environment_overrides":
                ["UE-LocalDataCachePath"],
            "after": [],
            "cleanup": {
                "action": "stop-pipeline-sponsor",
                "target_pid": 321,
                "signal": "terminate",
                "forced": False,
                "wait_return_code": 0,
                "verified_stopped": True,
                "data_directory_removed": True,
            },
            "lifecycle_status": "success",
        }
        bs._write_json(zen_path, zen_receipt)
        cook_stamp["zen_lifetime"] = {
            "path": str(zen_path),
            "sha256": bs._sha256_file(zen_path),
            "receipt": zen_receipt,
        }
        staged_tree = bs._write_staged_tree_receipt(iso)
        audit_stamp = {
            "verdict": "pass",
            "cache": str(self.root / "cache"),
            "cache_sha256": "c" * 64,
            "container_count": 5,
            "staged_tree": staged_tree,
        }
        source_snapshot = {
            "format": bs.SOURCE_SNAPSHOT_FORMAT,
            "git_commit": "abc123456",
            "paths": list(bs.SOURCE_SNAPSHOT_PATHS),
            "tracked_diff_size": 0,
            "tracked_diff_sha256": "3" * 64,
            "untracked": [],
            "sha256": "4" * 64,
        }
        return {
            "iso": iso,
            "game": game,
            "updater": updater,
            "cook": cook_stamp,
            "audit": audit_stamp,
            "source": source_snapshot,
        }

    def test_guarded_refuses_development_and_launcher_engine(self):
        with self.assertRaisesRegex(bs.BuildFailure, "Development is refused"):
            bs._resolve_build_mode(self.args(configuration="Development"))

        source = self.root / "UE-5.8-source"
        marker = source / "Engine" / "Build" / "InstalledBuild.txt"
        marker.parent.mkdir(parents=True)
        marker.write_text("launcher")
        with mock.patch.dict(bs.PINNED_SOURCE_ENGINES, {"Mac": source}):
            with self.assertRaisesRegex(bs.BuildFailure, "launcher/installed"):
                bs._resolve_build_mode(
                    self.args(configuration="Test", engine=source))

    def test_guarded_game_disables_profile_writing_crash_helpers(self):
        repo = Path(__file__).resolve().parents[2]
        target = (repo / "Source" / "WoT.Target.cs").read_text(
            encoding="utf-8")
        for name, value in bs.GUARDED_GAME_PROFILE_DEFINITIONS.items():
            self.assertIn(
                f'GlobalDefinitions.Add("{name}={value}");', target)
            self.assertEqual(bs.SHIPPING_GAME_DEFINITIONS[name], value)
        config = (repo / "Config" / "DefaultGame.ini").read_text(
            encoding="utf-8")
        self.assertIn("IncludeCrashReporter=False", config)

    def test_guarded_win64_cxx_invocations_are_serial_and_local_only(self):
        exact_ubt = [
            "-MaxParallelActions=1", "-NoXGE", "-NoFASTBuild",
            "-NoSNDBS", "-NoUBA", "-UBADisableRemote",
        ]
        args = self.args(platform="Win64", configuration="Test")
        iso = self.root / "iso"
        engine = self.root / "engine"
        runuat = engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.bat"
        runuat.parent.mkdir(parents=True)
        runuat.write_bytes(b"@exit /b 0\r\n")
        # runuat_path resolves the host launcher; give the Mac one too.
        (runuat.parent / "RunUAT.sh").write_text("#!/bin/sh\n")
        guarded_build = bs._guarded_build_command(
            args, iso, engine, "WoT", iso / "engine.json")
        self.assertEqual(guarded_build[-1], "-SerialLocalBuild")

        cook = bs._cook_command(args, iso, engine)
        self.assertIn("-NoXGE", cook)
        self.assertIn("-UbtArgs=" + " ".join(exact_ubt), cook)

        engine_modules = (engine / "Engine" / "Binaries" / "Win64"
                          / "UnrealEditor.modules")
        repo_modules = (self.repo / "Binaries" / "Win64"
                        / "UnrealEditor.modules")
        engine_modules.parent.mkdir(parents=True)
        repo_modules.parent.mkdir(parents=True)
        engine_modules.write_text('{"BuildId":"engine"}')
        repo_modules.write_text('{"BuildId":"repo"}')
        with mock.patch.object(
                bs, "_engine_root_and_editor",
                return_value=(engine, engine / "UnrealEditor-Cmd.exe")), \
                mock.patch.object(bs, "_run_logged", return_value=0) as run:
            bs._ensure_repo_editor_module(args)
        self.assertEqual(list(run.call_args.args[1][-len(exact_ubt):]),
                         exact_ubt)

        unguarded = self.args(
            guarded=False, platform="Win64", configuration="Development")
        with mock.patch.object(
                bs, "_engine_root_and_editor",
                return_value=(engine, engine / "UnrealEditor-Cmd.exe")), \
                mock.patch.object(bs, "_run_logged", return_value=0) as run:
            bs._ensure_repo_editor_module(unguarded)
        self.assertFalse(any(
            token in exact_ubt for token in run.call_args.args[1]))

    def test_guarded_win64_cook_command_validator_requires_exact_serial_local_ubt(self):
        args = self.args(platform="Win64", configuration="Test")
        iso = self.root / "iso"
        engine = self.root / "engine"
        runuat = engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.bat"
        runuat.parent.mkdir(parents=True)
        runuat.write_bytes(b"@exit /b 0\r\n")
        # runuat_path resolves the host launcher; give the Mac one too.
        (runuat.parent / "RunUAT.sh").write_text("#!/bin/sh\n")
        canonical = bs._cook_command(args, iso, engine)
        bs._validate_guarded_unreal_command(canonical, "cook", "Win64")
        ubt = next(token for token in canonical
                   if token.startswith("-UbtArgs="))
        no_ubt = [token for token in canonical if token != ubt]
        hostile = (
            no_ubt,
            [token.replace("-MaxParallelActions=1", "-MaxParallelActions=2")
             if token == ubt else token for token in canonical],
            [*canonical, ubt],
            [token + " -AllowRemoteExecutor" if token == ubt else token
             for token in canonical],
            [*canonical, "-MaxParallelActions=8"],
            [*canonical, "-XGE"],
            [token for token in canonical if token != "-NoXGE"],
            [*canonical, "-NoXGE"],
        )
        for command in hostile:
            with self.subTest(command=command), self.assertRaises(
                    bs.BuildFailure):
                bs._validate_guarded_unreal_command(
                    command, "cook", "Win64")

    @unittest.skipUnless(os.name == "nt", "PowerShell wrapper is Win64-only")
    def test_guarded_win64_wrapper_binds_serial_local_switch_exactly(self):
        powershell = shutil.which("powershell.exe")
        if powershell is None:
            self.skipTest("Windows PowerShell is unavailable")
        engine = self.root / "fake-engine"
        build = engine / "Engine" / "Build" / "BatchFiles" / "Build.bat"
        ubt = (engine / "Engine" / "Binaries" / "DotNET" /
               "UnrealBuildTool" / "UnrealBuildTool.dll")
        build.parent.mkdir(parents=True)
        ubt.parent.mkdir(parents=True)
        build.write_bytes(b"@echo FAKE_BUILD_ARGS:%*\r\n@exit /b 0\r\n")
        ubt.write_bytes(b"fake")
        project = self.root / "fake-project" / "WoT.uproject"
        project.parent.mkdir()
        project.write_text("{}", encoding="utf-8")
        receipt = self.root / "receipt.json"
        # bs.REPO points at the synthetic fixture, so use the real checkout.
        wrapper = Path(__file__).resolve().parents[1] / "build" / \
            "build_guarded_ue58.ps1"
        quoted = lambda value: str(value).replace("'", "''")
        prefix = (
            "function global:python { $global:LASTEXITCODE=0 }; "
            f"& '{quoted(wrapper)}' -Engine '{quoted(engine)}' "
            f"-Project '{quoted(project)}' -Target WoTEditor "
            f"-Platform Win64 -Configuration Development "
            f"-Receipt '{quoted(receipt)}'")
        enabled = subprocess.run(
            [powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy",
             "Bypass", "-Command",
             prefix + " -SerialLocalBuild"],
            capture_output=True, text=True)
        self.assertEqual(enabled.returncode, 0, enabled.stderr)
        for token in bs.WIN64_SERIAL_LOCAL_UBT_ARGUMENTS:
            self.assertEqual(enabled.stdout.count(token), 1)
        omitted = subprocess.run(
            [powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy",
             "Bypass", "-Command", prefix],
            capture_output=True, text=True)
        self.assertEqual(omitted.returncode, 0, omitted.stderr)
        self.assertFalse(any(token in omitted.stdout
                             for token in bs.WIN64_SERIAL_LOCAL_UBT_ARGUMENTS))
        rejected = subprocess.run(
            [powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy",
             "Bypass", "-Command",
             prefix.replace("-Platform Win64", "-Platform Mac")
             + " -SerialLocalBuild"],
            capture_output=True, text=True)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("supported only for Win64",
                      rejected.stdout + rejected.stderr)
        unknown_receipt = self.root / "unknown-option-receipt.json"
        unknown_prefix = prefix.replace(
            quoted(receipt), quoted(unknown_receipt))
        unknown = subprocess.run(
            [powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy",
             "Bypass", "-Command",
             unknown_prefix
             + " -SerialLocalBuild -UnknownUbtArgs '-MaxParallelActions=8'"],
            capture_output=True, text=True)
        self.assertNotEqual(unknown.returncode, 0)
        self.assertIn("UnknownUbtArgs", unknown.stdout + unknown.stderr)
        self.assertNotIn("FAKE_BUILD_ARGS", unknown.stdout + unknown.stderr)
        self.assertFalse(unknown_receipt.exists())

    @unittest.skipUnless(os.name == "nt", "PowerShell wrapper is Win64-only")
    def test_guarded_wrapper_rejects_non_win64_serial_before_any_mutation(self):
        powershell = shutil.which("powershell.exe")
        if powershell is None:
            self.skipTest("Windows PowerShell is unavailable")
        engine = self.root / "fake-engine"
        build = engine / "Engine" / "Build" / "BatchFiles" / "Build.bat"
        ubt = (engine / "Engine" / "Binaries" / "DotNET" /
               "UnrealBuildTool" / "UnrealBuildTool.dll")
        build.parent.mkdir(parents=True)
        ubt.parent.mkdir(parents=True)
        build.write_bytes(b"@echo FAKE_BUILD_ARGS:%*\r\n@exit /b 0\r\n")
        ubt.write_bytes(b"fake")
        project = self.root / "fake-project" / "WoT.uproject"
        project.parent.mkdir()
        project.write_text("{}", encoding="utf-8")
        intermediate = (project.parent / "Intermediate" / "Build" / "Mac" /
                        "nested" / "WoTEditor")
        intermediate.mkdir(parents=True)
        intermediate_sentinel = intermediate / "keep.txt"
        intermediate_sentinel.write_text("keep", encoding="utf-8")
        binary_sentinel = (project.parent / "Binaries" / "Mac" /
                           "WoTEditor-clean-sentinel.bin")
        binary_sentinel.parent.mkdir(parents=True)
        binary_sentinel.write_bytes(b"keep")
        receipt = self.root / "fresh-receipt-dir" / "receipt.json"
        wrapper = Path(__file__).resolve().parents[1] / "build" / \
            "build_guarded_ue58.ps1"
        quoted = lambda value: str(value).replace("'", "''")
        command = (
            "function global:python { Write-Output 'VERIFIER_CALLED'; "
            "$global:LASTEXITCODE=0 }; "
            f"& '{quoted(wrapper)}' -Engine '{quoted(engine)}' "
            f"-Project '{quoted(project)}' -Target WoTEditor "
            f"-Platform Mac -Configuration Development -Clean "
            f"-Receipt '{quoted(receipt)}' -SerialLocalBuild")
        rejected = subprocess.run(
            [powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy",
             "Bypass", "-Command", command],
            capture_output=True, text=True)
        output = rejected.stdout + rejected.stderr
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("supported only for Win64", output)
        self.assertNotIn("VERIFIER_CALLED", output)
        self.assertNotIn("FAKE_BUILD_ARGS", output)
        self.assertFalse(receipt.parent.exists())
        self.assertTrue(intermediate_sentinel.is_file())
        self.assertTrue(binary_sentinel.is_file())

    def test_import_retry_clears_only_exact_no_replace_outputs(self):
        iso = self.root / "iso"
        for relative in bs.IMPORT_GENERATED_OUTPUTS:
            output = iso / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(b"stale generated output")
        unrelated = iso / "Saved" / "ImportReceipts" / "keep.json"
        unrelated.write_bytes(b"unrelated evidence")
        neutral_output = iso / bs.NEUTRAL_SHELL_GENERATED_ROOT
        neutral_output.mkdir(parents=True)
        (neutral_output / "partial.umap").write_bytes(b"partial")
        plugin = iso / bs.NEUTRAL_SHELL_PLUGIN_ROOT
        plugin.mkdir(parents=True)
        (plugin / "stale.uplugin").write_bytes(b"stale")
        for receipt in (
                bs.NEUTRAL_SHELL_DENIED_READ_RECEIPT,
                bs.NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT):
            path = iso / receipt
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"stale receipt")

        bs._prepare_import_generated_outputs(iso)

        self.assertTrue(unrelated.is_file())
        for relative in bs.IMPORT_GENERATED_OUTPUTS:
            self.assertFalse((iso / relative).exists())
        self.assertFalse(neutral_output.exists())
        self.assertFalse(plugin.exists())
        self.assertFalse(
            (iso / bs.NEUTRAL_SHELL_DENIED_READ_RECEIPT).exists())
        self.assertFalse(
            (iso / bs.NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT).exists())

    def test_guarded_clean_cook_is_limited_to_disposable_outputs(self):
        iso = self.root / "iso"
        cooked = iso / "Saved" / "Cooked" / "Mac"
        staged = iso / "staged"
        receipt = iso / "receipts" / "keep.json"
        other_platform = iso / "Saved" / "Cooked" / "Windows" / "keep.bin"
        for path in (cooked / "old.bin", staged / "old.bin", receipt,
                     other_platform):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")

        bs._prepare_guarded_clean_cook(iso, "Mac")

        self.assertFalse(cooked.exists())
        self.assertFalse(staged.exists())
        self.assertTrue(receipt.is_file())
        self.assertTrue(other_platform.is_file())

    def test_fresh_fractal_finalize_uses_a_separate_commandlet(self):
        iso = self.root / "iso"
        editor = self.root / "UnrealEditor-Cmd"
        command = bs._fresh_fractal_finalize_command(iso, editor)
        self.assertEqual(command[0], str(editor))
        self.assertEqual(command[1], str(iso / "WoT.uproject"))
        self.assertIn("-run=pythonscript", command)
        script_argument = next(
            value for value in command if value.startswith("-script="))
        # Forward slashes are the commandlet-boundary contract: UE re-parses
        # the quoted -script value with escape processing, so a Windows
        # backslash path with an accidental \n/\t sequence silently truncates
        # the argument list (a real Win64 guarded failure).
        self.assertEqual(
            script_argument,
            "-script=" + str(
                iso / "Tools/import/finalize_stripped_fractal_catalog.py"
            ).replace("\\", "/"))

    def test_editor_process_affinity_reaches_every_guarded_editor_launch(self):
        iso = self.root / "iso"
        editor = self.root / "UnrealEditor-Cmd.exe"
        args = self.args(
            configuration="Test", platform="Win64",
            editor_process_affinity=8,
            conv_dir=self.root / "converted",
            exports_dir=self.root / "exports",
            retail_root=self.root / "retail",
            mesh_source="prepared")
        expected = "-processaffinity=8"

        self.assertEqual(
            bs._import_cache_identity(args)["editor_process_affinity"], 8)
        self.assertIn(expected, bs._import_command(args, iso, editor))
        finalize_command = bs._fresh_fractal_finalize_command(
            iso, editor, editor_process_affinity=8, guarded=True)
        self.assertIn(expected, finalize_command)
        with mock.patch.object(bs, "REPO", self.root / "repo"):
            shell_command = bs._neutral_shell_generation_command(
                iso, editor, editor_process_affinity=8, guarded=True)
        self.assertIn(expected, shell_command)
        for command in (
                bs._import_command(args, iso, editor),
                finalize_command, shell_command):
            self.assertIn("-ddc=(ZenLocal)", command)
            self.assertIn(bs.ZEN_NO_AUTO_LAUNCH, command)

        engine = self.root / "engine"
        runuat = engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.bat"
        runuat.parent.mkdir(parents=True)
        runuat.write_text("@echo off\n")
        # runuat_path resolves the host launcher; give the Mac one too.
        (runuat.parent / "RunUAT.sh").write_text("#!/bin/sh\n")
        cooker_options = next(
            value for value in bs._cook_command(args, iso, engine)
            if value.startswith("-AdditionalCookerOptions="))
        self.assertIn(expected, cooker_options.split("=", 1)[1].split())
        self.assertIn("-ddc=(ZenLocal)", cooker_options)
        self.assertIn(bs.ZEN_NO_AUTO_LAUNCH, cooker_options)

    def test_guarded_import_skips_carrier_and_dependent_pickup_authoring(self):
        iso = self.root / "iso"
        editor = self.root / "UnrealEditor-Cmd.exe"
        guarded = self.args(
            configuration="Test", platform="Win64",
            editor_process_affinity=None,
            conv_dir=self.root / "converted",
            exports_dir=self.root / "exports",
            retail_root=self.root / "retail",
            mesh_source="prepared")
        development = self.args(
            guarded=False, configuration="Development", platform="Win64",
            editor_process_affinity=None,
            conv_dir=self.root / "converted",
            exports_dir=self.root / "exports",
            retail_root=self.root / "retail",
            mesh_source="prepared")

        guarded_script = next(
            value for value in bs._import_command(guarded, iso, editor)
            if value.startswith("-script="))
        development_script = next(
            value for value in bs._import_command(development, iso, editor)
            if value.startswith("-script="))
        self.assertIn("--skip=meshes,pickups", guarded_script)
        self.assertNotIn("--skip=", development_script)
        self.assertEqual(
            bs._import_cache_identity(guarded)["guarded_mesh_carrier_policy"],
            "skip-meshes-and-pickups-v1")
        self.assertEqual(
            bs._import_cache_identity(development)["guarded_mesh_carrier_policy"],
            "development-import-v1")

    def test_guarded_unreal_log_refuses_preexisting_ddc_or_zen_contact(self):
        hostile = (
            "LogDerivedDataCache: /fixture/preexisting-ddc: Maintenance finished",
            "Zen server ready at http://127.0.0.1:8558/",
        )
        for text in hostile:
            with self.subTest(text=text), self.assertRaisesRegex(
                    bs.BuildFailure, "preexisting"):
                bs._validate_guarded_unreal_log(text, "import")
        bs._validate_guarded_unreal_log(
            "Zen server ready at http://127.0.0.1:18558/", "import")

    def test_guarded_unreal_environment_strips_every_ddc_override(self):
        source = {
            "PATH": "preserve",
            "UE-LocalDataCachePath": r"/fixture/preexisting-ddc",
            "ue-shareddatacachepath": r"\\foreign\ddc",
            "UE-ZenLocalDataCacheReadOnly": "true",
            "UE-ZenSharedDataCacheHost": "127.0.0.1:8558",
            "UE-CloudDataCacheAccessToken": "secret",
        }
        cleaned, removed = bs._guarded_unreal_environment(source)
        self.assertEqual(cleaned, {"PATH": "preserve"})
        self.assertEqual(removed, sorted(source.keys() - {"PATH"}))
        self.assertEqual(source["UE-LocalDataCachePath"], r"/fixture/preexisting-ddc")

    def test_guarded_run_logged_passes_only_sanitized_ddc_environment(self):
        log = self.root / "iso" / "logs" / "import.log"
        command = [
            str(self.root / "UnrealEditor-Cmd.exe"), "WoT.uproject",
            "-ddc=(ZenLocal)", bs.ZEN_NO_AUTO_LAUNCH]

        def run(_command, **kwargs):
            self.assertNotIn("UE-LocalDataCachePath", kwargs["env"])
            self.assertEqual(kwargs["env"]["PATH"], "preserve")
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text("owned Zen 127.0.0.1:18558\n", encoding="utf-8")
            return SimpleNamespace(returncode=0)

        rows = []
        with mock.patch.object(bs, "_ACTIVE_GUARDED_ZEN", True), \
                mock.patch.object(bs, "_ACTIVE_GUARDED_ZEN_COMMANDS", rows), \
                mock.patch.object(bs.subprocess, "run", side_effect=run):
            self.assertEqual(bs._run_logged(
                "import", command, log, env={
                    "PATH": "preserve",
                    "UE-LocalDataCachePath": r"/fixture/preexisting-ddc"}), 0)
        self.assertEqual(rows[0]["stripped_ddc_environment_overrides"],
                         ["UE-LocalDataCachePath"])

    def test_guarded_unreal_lifecycle_is_healthy_before_action_and_preserves_foreign(self):
        iso = self.root / "iso"
        engine = self.root / "engine"
        zen = engine / "Engine" / "Binaries" / "Mac" / "zenserver"
        zen.parent.mkdir(parents=True)
        zen.write_bytes(b"pinned zen")
        process = mock.Mock(pid=321, returncode=0)
        process.poll.return_value = None
        state = {"started": False, "cleaned": False}
        process.wait.side_effect = lambda **_kwargs: state.update(
            cleaned=True) or 0
        foreign = {"pid": 55, "command": "/foreign/zenserver --port 8558"}
        owner_pid = bs.os.getpid()
        sponsor_command = bs._canonical_zen_sponsor_command(
            str(zen), iso / "Saved" / f"GuardedZenSponsor-{owner_pid}",
            owner_pid)
        sponsor = {"pid": 321, "command": " ".join(sponsor_command)}

        def scan():
            if not state["started"] or state["cleaned"]:
                return [foreign]
            return [foreign, sponsor]

        events = []

        def health(*_args):
            state["started"] = True
            events.append("health")
            return {
                "url": "http://127.0.0.1:18558/health/ready",
                "status_code": 200, "body_sha256": "3" * 64,
                "checked_utc": "2026-08-28T00:00:00+00:00",
            }

        def action():
            events.append("action")
            self.assertTrue(bs._ACTIVE_GUARDED_ZEN)
            bs.time.sleep(0.12)
            return "ran"

        with mock.patch.object(bs, "_zen_processes", side_effect=scan), \
                mock.patch.object(bs, "_prove_zen_endpoint_unoccupied",
                                  return_value={
                                      "host": "127.0.0.1", "port": 18558,
                                      "exclusive_bind_succeeded": True,
                                      "checked_utc": "2026-08-28T00:00:00+00:00"}), \
                mock.patch.object(bs, "_wait_for_zen_health",
                                  side_effect=health), \
                mock.patch.object(bs.subprocess, "Popen",
                                  return_value=process) as popen:
            result, row = bs._run_guarded_unreal_lifecycle(
                iso, engine, "Mac", action)

        self.assertEqual(result, "ran")
        self.assertEqual(events, ["health", "action"])
        self.assertEqual(
            row["receipt"]["mechanism"], "pipeline-sponsored-zen-v3")
        self.assertEqual(row["receipt"]["before"], [foreign])
        self.assertEqual(row["receipt"]["after"], [foreign])
        self.assertGreater(row["receipt"]["monitor"]["sample_count"], 0)
        popen.assert_called_once()
        process.terminate.assert_called_once()

    def test_guarded_unreal_command_refuses_alternate_or_duplicate_ddc(self):
        editor = [
            "UnrealEditor-Cmd", "WoT.uproject",
            "-ddc=(ZenLocal)", bs.ZEN_NO_AUTO_LAUNCH]
        cook = [
            "RunUAT", "-AdditionalCookerOptions=-skipzenstore "
            "-ddc=(ZenLocal) " + bs.ZEN_NO_AUTO_LAUNCH + " "
            + self.neutral_support_package_option()]
        self.assertEqual(
            bs._validate_guarded_unreal_command(editor, "editor"), "editor")
        self.assertEqual(
            bs._validate_guarded_unreal_command(cook, "cook"), "cook")
        hostile = (
            ([*editor, "-ddc=Shared"], "editor"),
            ([*editor, "-NoZenAutoLaunch=127.0.0.1:8558"], "editor"),
            (["UnrealEditor-Cmd", "WoT.uproject", "-ddc=NoShared",
              bs.ZEN_NO_AUTO_LAUNCH], "editor"),
            (["RunUAT", "-AdditionalCookerOptions=-skipzenstore "
              "-ddc=(ZenLocal) -ddc=Shared " + bs.ZEN_NO_AUTO_LAUNCH],
             "cook"),
            ([*cook, "-ddc=Shared"], "cook"),
            ([*cook, "-NoZenAutoLaunch=127.0.0.1:8558"], "cook"),
        )
        for command, kind in hostile:
            with self.subTest(command=command), self.assertRaises(
                    bs.BuildFailure):
                bs._validate_guarded_unreal_command(command, kind)

    def test_main_wraps_all_guarded_unreal_stages_in_one_lifecycle(self):
        iso = self.root / "iso"
        bs._write_stamp(iso, "seed", {"seeded": True})
        events = []
        zen_row = {
            "path": str(iso / "receipts" / "zen-lifetime.json"),
            "sha256": "a" * 64,
            "receipt": {"mechanism": "pipeline-sponsored-zen-v3"},
        }

        def stage(name):
            def run(*_args, **_kwargs):
                events.append(name)
                bs._write_stamp(iso, name, {"ran": True})
                return "ran"
            return run

        def lifecycle(_iso, _engine, _platform, action):
            events.append("lifecycle-start")
            result = action()
            events.append("lifecycle-stop")
            return result, zen_row

        with mock.patch.object(bs, "_resolve_build_mode"), \
                mock.patch.object(bs, "_validate_iso", return_value=iso), \
                mock.patch.object(bs, "_require_reviewed_innoextract"), \
                mock.patch.object(bs, "_ensure_repo_editor_module"), \
                mock.patch.object(bs, "_engine_root_and_editor",
                                  return_value=(self.root / "engine",
                                                self.root / "editor")), \
                mock.patch.object(bs, "stage_import", side_effect=stage("import")), \
                mock.patch.object(bs, "stage_marker", side_effect=stage("marker")), \
                mock.patch.object(bs, "stage_cook", side_effect=stage("cook")), \
                mock.patch.object(bs, "_run_guarded_unreal_lifecycle",
                                  side_effect=lifecycle), \
                mock.patch.object(bs, "write_summary",
                                  return_value=iso / "summary.json"):
            exit_code = bs.main([
                "--iso", str(iso), "--cache", str(self.root / "cache"),
                "--guarded", "--configuration", "Test",
                "--only-step", "import,marker,cook"])

        self.assertEqual(exit_code, 0)
        self.assertEqual(events, [
            "lifecycle-start", "import", "marker", "cook",
            "lifecycle-stop"])
        self.assertEqual(
            bs._read_stamp(iso, "import")["zen_lifetime"], zen_row)
        self.assertEqual(
            bs._read_stamp(iso, "cook")["zen_lifetime"], zen_row)

    def test_main_refuses_partial_guarded_unreal_lifecycle_before_mutation(self):
        iso = self.root / "iso"
        with mock.patch.object(bs, "_resolve_build_mode"), \
                mock.patch.object(bs, "_validate_iso", return_value=iso), \
                mock.patch.object(bs, "stage_cook") as cook, \
                mock.patch.object(bs, "_run_guarded_unreal_lifecycle") as life:
            exit_code = bs.main([
                "--iso", str(iso), "--cache", str(self.root / "cache"),
                "--guarded", "--configuration", "Test",
                "--only-step", "cook"])
        self.assertEqual(exit_code, 1)
        cook.assert_not_called()
        life.assert_not_called()

    def test_editor_process_affinity_is_guarded_win64_only(self):
        with self.assertRaisesRegex(
                bs.BuildFailure, "guarded Win64"):
            bs._resolve_build_mode(self.args(
                guarded=False, configuration=None, platform="Win64",
                editor_process_affinity=8))
        with self.assertRaisesRegex(
                bs.BuildFailure, "guarded Win64"):
            bs._resolve_build_mode(self.args(
                platform="Mac", editor_process_affinity=8))

    def test_stage_import_forwards_affinity_on_full_and_repair_paths(self):
        args = self.args(
            configuration="Test", platform="Win64",
            editor_process_affinity=8,
            conv_dir=self.root / "converted",
            exports_dir=self.root / "exports",
            retail_root=self.root / "retail",
            mesh_source="prepared")
        engine = self.root / "engine"
        editor = engine / "UnrealEditor-Cmd.exe"
        neutral_support = bs._neutral_support_asset_receipt(self.repo)

        full_iso = self.root / "full"

        def successful_import(_step, _command, log_path, **_kwargs):
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text(
                bs.IMPORT_SUCCESS_MARKERS[0], encoding="utf-8")
            return 0

        with (mock.patch.object(bs, "_read_stamp", return_value=None),
              mock.patch.object(bs, "_invalidate_from"),
              mock.patch.object(bs, "_prepare_import_generated_outputs"),
              mock.patch.object(
                  bs, "_neutral_support_lifecycle_receipt",
                  return_value=neutral_support),
              mock.patch.object(
                  bs, "_engine_root_and_editor",
                  return_value=(engine, editor)),
              mock.patch.object(bs, "_run_logged", side_effect=successful_import),
              mock.patch.object(
                  bs, "_run_fresh_fractal_finalize", return_value={}) as full_finalize,
              mock.patch.object(
                  bs, "_run_neutral_shell_generation", return_value={}) as full_shells,
              mock.patch.object(bs, "_write_stamp") as full_write):
            self.assertEqual(bs.stage_import(args, full_iso), "ran")
        self.assertNotIn(
            "authenticated_alternate_bind", full_write.call_args.args[2])
        self.assertEqual(full_finalize.call_args.args[-1], 8)
        self.assertEqual(full_shells.call_args.args[-1], 8)

        repair_iso = self.root / "repair"
        import_log = repair_iso / "logs" / "import.log"
        import_log.parent.mkdir(parents=True)
        import_log.write_text("prior success", encoding="utf-8")
        stamp = {
            **bs._import_cache_identity(args),
            "command_exit_code": 0,
            "success_marker": bs.IMPORT_SUCCESS_MARKERS[0],
            "neutral_support_authority": neutral_support,
        }
        with (mock.patch.object(bs, "_read_stamp", return_value=stamp),
              mock.patch.object(bs, "_invalidate_from"),
              mock.patch.object(bs, "_neutral_shell_stamp_valid", return_value=False),
              mock.patch.object(
                  bs, "_neutral_support_lifecycle_receipt",
                  return_value=neutral_support),
              mock.patch.object(
                  bs, "_engine_root_and_editor",
                  return_value=(engine, editor)),
              mock.patch.object(
                  bs, "_run_fresh_fractal_finalize", return_value={}) as repair_finalize,
              mock.patch.object(
                  bs, "_run_neutral_shell_generation", return_value={}) as repair_shells,
              mock.patch.object(bs, "_write_stamp") as repair_write):
            self.assertEqual(bs.stage_import(args, repair_iso), "ran")
        self.assertNotIn(
            "authenticated_alternate_bind", repair_write.call_args.args[2])
        self.assertEqual(repair_finalize.call_args.args[-1], 8)
        self.assertEqual(repair_shells.call_args.args[-1], 8)

    def test_guarded_stage_import_rejects_nonzero_even_with_success_marker(self):
        args = self.args(
            configuration="Test", platform="Win64",
            editor_process_affinity=8,
            conv_dir=self.root / "converted",
            exports_dir=self.root / "exports",
            retail_root=self.root / "retail",
            mesh_source="prepared")
        iso = self.root / "iso"
        engine = self.root / "engine"
        editor = engine / "UnrealEditor-Cmd.exe"
        neutral_support = bs._neutral_support_asset_receipt(self.repo)

        def nonzero_import(_step, _command, log_path, **_kwargs):
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text(
                bs.IMPORT_SUCCESS_MARKERS[0], encoding="utf-8")
            return 1

        with (mock.patch.object(bs, "_read_stamp", return_value=None),
              mock.patch.object(bs, "_invalidate_from"),
              mock.patch.object(bs, "_prepare_import_generated_outputs"),
              mock.patch.object(
                  bs, "_neutral_support_lifecycle_receipt",
                  return_value=neutral_support),
              mock.patch.object(
                  bs, "_engine_root_and_editor",
                  return_value=(engine, editor)),
              mock.patch.object(bs, "_run_logged", side_effect=nonzero_import),
              mock.patch.object(bs, "_run_fresh_fractal_finalize") as finalize,
              mock.patch.object(bs, "_run_neutral_shell_generation") as shells):
            with self.assertRaisesRegex(
                    bs.BuildFailure, "import command exited 1"):
                bs.stage_import(args, iso)

        finalize.assert_not_called()
        shells.assert_not_called()

    def test_guarded_stage_import_never_reuses_nonzero_cached_command(self):
        args = self.args(
            configuration="Test", platform="Win64",
            editor_process_affinity=8,
            conv_dir=self.root / "converted",
            exports_dir=self.root / "exports",
            retail_root=self.root / "retail",
            mesh_source="prepared")
        iso = self.root / "iso"
        log = iso / "logs" / "import.log"
        fractal_log = iso / "logs" / \
            "finalize-stripped-fractal-catalog.log"
        receipt = iso / bs.FRACTAL_FINALIZE_RECEIPT
        log.parent.mkdir(parents=True)
        log.write_text(bs.IMPORT_SUCCESS_MARKERS[0])
        fractal_log.write_text("clean")
        bs._write_json(receipt, {"valid": True})
        neutral_support = bs._neutral_support_asset_receipt(self.repo)
        stamp = {
            **bs._import_cache_identity(args),
            "command_exit_code": 1,
            "success_marker": bs.IMPORT_SUCCESS_MARKERS[0],
            "neutral_support_authority": neutral_support,
            "fresh_fractal_finalize_success_marker":
                bs.PYTHON_COMMANDLET_SUCCESS,
            "fresh_fractal_finalize_script_marker":
                bs.FRACTAL_FINALIZE_SUCCESS,
            "fresh_fractal_finalize_receipt_sha256":
                bs._sha256_file(receipt),
        }

        def rerun(_step, _command, log_path, **_kwargs):
            log_path.write_text(bs.IMPORT_SUCCESS_MARKERS[0])
            return 0

        with (mock.patch.object(bs, "_read_stamp", return_value=stamp),
              mock.patch.object(bs, "_invalidate_from"),
              mock.patch.object(bs, "_prepare_import_generated_outputs"),
              mock.patch.object(
                  bs, "_neutral_support_lifecycle_receipt",
                  return_value=neutral_support),
              mock.patch.object(bs, "_neutral_shell_stamp_valid", return_value=True),
              mock.patch.object(
                  bs, "_engine_root_and_editor",
                  return_value=(self.root / "engine", self.root / "editor")),
              mock.patch.object(bs, "_run_logged", side_effect=rerun) as run,
              mock.patch.object(bs, "_run_fresh_fractal_finalize", return_value={}),
              mock.patch.object(bs, "_run_neutral_shell_generation", return_value={}),
              mock.patch.object(bs, "_write_stamp")):
            self.assertEqual(bs.stage_import(args, iso), "ran")
        run.assert_called_once()

    def test_neutral_shell_generation_uses_one_commandlet_script_argument(self):
        iso = self.root / "iso"
        editor = self.root / "UnrealEditor-Cmd"
        with mock.patch.object(bs, "REPO", self.root / "repo"):
            command = bs._neutral_shell_generation_command(iso, editor)
        self.assertEqual(command[:3], [
            str(editor), str(iso / "WoT.uproject"), "-run=pythonscript"])
        self.assertEqual(command[-5:], [
            "-unattended", "-nullrhi", "-nosplash", "-stdout", "-nosteam"])
        script_arguments = [
            value for value in command if value.startswith("-script=")]
        self.assertEqual(len(script_arguments), 1)
        script_argument = script_arguments[0]
        # Same forward-slash contract as the fractal finalize command: the
        # spec path's specs\neutral-... is exactly the \n sequence that UE's
        # escape-processing QuotedString truncated on Windows.
        slashed = lambda path: str(path).replace("\\", "/")
        self.assertIn(
            "--spec=" + slashed(
                iso / "Tools/import/specs/neutral-map-shells-v1.json"),
            script_argument)
        self.assertIn(
            "--output-root=" + slashed(iso / "NeutralMapShells"),
            script_argument)
        self.assertEqual(script_argument.count("--deny-read-root="), 4)
        self.assertIn(
            "--require-disjoint-from=" + slashed(iso / "Content"),
            script_argument)
        self.assertNotIn("\\", script_argument)
        self.assertTrue(script_argument.endswith(
            "--require-empty-output --no-replace"))

    def test_plugin_synthesis_moves_and_verifies_all_receipted_shells(self):
        iso = self.root / "iso"
        self.neutral_shell_generation(iso)

        result = bs._synthesize_neutral_shell_plugin(iso)

        content = iso / bs.NEUTRAL_SHELL_PLUGIN_ROOT / "Content"
        self.assertFalse((iso / bs.NEUTRAL_SHELL_GENERATED_ROOT).exists())
        self.assertEqual(result["package_count"], 19)
        self.assertEqual(len(list(content.glob("*.umap"))), 19)
        self.assertEqual(
            (iso / bs.NEUTRAL_SHELL_PLUGIN_ROOT /
             "WoTNeutralShells.uplugin").read_bytes(),
            bs._neutral_shell_plugin_descriptor_bytes())
        project = json.loads((iso / "WoT.uproject").read_text(
            encoding="utf-8"))
        self.assertEqual(
            [entry for entry in project["Plugins"]
             if entry.get("Name") == "WoTNeutralShells"],
            [{"Enabled": True, "Name": "WoTNeutralShells"}])

    def test_plugin_synthesis_enables_existing_neutral_shell_reference(self):
        iso = self.root / "iso"
        self.neutral_shell_generation(iso)
        (iso / "WoT.uproject").write_text(json.dumps({
            "FileVersion": 3,
            "Plugins": [
                {"Name": "Other", "Enabled": True},
                {"Name": "WoTNeutralShells", "Enabled": False},
            ],
        }), encoding="utf-8")

        bs._synthesize_neutral_shell_plugin(iso)

        plugins = json.loads((iso / "WoT.uproject").read_text(
            encoding="utf-8"))["Plugins"]
        self.assertEqual(plugins.count(
            {"Name": "WoTNeutralShells", "Enabled": True}), 1)

    def test_plugin_synthesis_rejects_duplicate_project_references(self):
        iso = self.root / "iso"
        self.neutral_shell_generation(iso)
        (iso / "WoT.uproject").write_text(json.dumps({
            "FileVersion": 3,
            "Plugins": [
                {"Name": "WoTNeutralShells", "Enabled": True},
                {"Name": "WoTNeutralShells", "Enabled": False},
            ],
        }), encoding="utf-8")

        with self.assertRaisesRegex(bs.BuildFailure, "duplicate"):
            bs._synthesize_neutral_shell_plugin(iso)

    def test_retry_cleanup_clears_interrupted_generator_staging(self):
        # The generator authors temporary maps under
        # Content/WoTNeutralShellStaging and hard-refuses a pre-existing
        # staging shell, so residue from a killed run would make every retry
        # fail forever unless cleanup removes it.
        iso = self.root / "iso"
        staging = iso / bs.NEUTRAL_SHELL_STAGING_ROOT
        staging.mkdir(parents=True)
        (staging / "WoTMapShell03.umap").write_bytes(b"interrupted")
        bs._prepare_neutral_shell_outputs(iso)
        self.assertFalse(staging.exists())

    def test_cleanup_refuses_to_delete_through_a_symlinked_ancestor(self):
        # iso/Plugins -> external makes the leaf look like a real directory;
        # an unguarded rmtree would delete the external tree.
        iso = self.root / "iso"
        iso.mkdir(parents=True, exist_ok=True)
        external = self.root / "external"
        (external / "WoTNeutralShells").mkdir(parents=True)
        canary = external / "WoTNeutralShells" / "precious.txt"
        canary.write_bytes(b"must survive")
        (iso / "Plugins").symlink_to(external, target_is_directory=True)
        with self.assertRaisesRegex(bs.BuildFailure, "symlink"):
            bs._prepare_neutral_shell_outputs(iso)
        self.assertTrue(canary.is_file())

    def test_plugin_synthesis_rejects_byte_mismatch(self):
        iso = self.root / "iso"
        self.neutral_shell_generation(iso)
        shell = (iso / bs.NEUTRAL_SHELL_GENERATED_ROOT /
                 "WoTMapShell07.umap")
        # Mutate WITHOUT changing length, so this exercises the SHA-256
        # re-hash specifically: a size-only check would pass a shorter string
        # and the post-move byte verification would go untested.
        original = shell.read_bytes()
        mutated = bytes(b ^ 0xFF for b in original[:1]) + original[1:]
        shell.write_bytes(mutated)
        self.assertEqual(len(mutated), len(original))
        self.assertNotEqual(mutated, original)
        with self.assertRaisesRegex(bs.BuildFailure, "changed|mismatch"):
            bs._synthesize_neutral_shell_plugin(iso)

    def test_plugin_synthesis_rejects_missing_file(self):
        iso = self.root / "iso"
        self.neutral_shell_generation(iso)
        (iso / bs.NEUTRAL_SHELL_GENERATED_ROOT /
         "WoTMapShell12.umap").unlink()
        with self.assertRaisesRegex(bs.BuildFailure, "missing|census"):
            bs._synthesize_neutral_shell_plugin(iso)

    def test_plugin_synthesis_rejects_extra_file(self):
        iso = self.root / "iso"
        self.neutral_shell_generation(iso)
        (iso / bs.NEUTRAL_SHELL_GENERATED_ROOT /
         "unexpected.bin").write_bytes(b"unexpected")
        with self.assertRaisesRegex(bs.BuildFailure, "unexpected|census"):
            bs._synthesize_neutral_shell_plugin(iso)

    def test_neutral_shell_plugin_descriptor_bytes_are_deterministic(self):
        expected = (
            b'{\n'
            b'  "CanContainContent": true,\n'
            b'  "EnabledByDefault": true,\n'
            b'  "FileVersion": 3,\n'
            b'  "FriendlyName": "WoT Neutral Shells",\n'
            b'  "Version": 1,\n'
            b'  "VersionName": "1.0"\n'
            b'}\n'
        )
        self.assertEqual(bs._neutral_shell_plugin_descriptor_bytes(), expected)
        self.assertEqual(
            bs._neutral_shell_plugin_descriptor_bytes(),
            bs._neutral_shell_plugin_descriptor_bytes())

    def test_neutral_shell_leg_uses_marker_and_cache_requires_its_log(self):
        iso = self.root / "iso"
        log_path = iso / "logs" / "generate-neutral-map-shells.log"

        def fake_run(_step, _command, emitted_log, **_kwargs):
            self.assertEqual(emitted_log, log_path)
            self.neutral_shell_generation(iso)
            denied_path = iso / bs.NEUTRAL_SHELL_DENIED_READ_RECEIPT
            denied_path.write_text(json.dumps({
                "format": "neutral-map-shells-denied-read-open-receipt-v2",
                "violations": [],
            }), encoding="utf-8")
            emitted_log.parent.mkdir(parents=True)
            emitted_log.write_text(
                f"noise\n{bs.PYTHON_COMMANDLET_SUCCESS}\n",
                encoding="utf-8")
            return 23

        with mock.patch.object(bs, "_run_logged", side_effect=fake_run):
            stamp = bs._run_neutral_shell_generation(
                iso, self.root / "UnrealEditor-Cmd", log_path)

        self.assertEqual(stamp["neutral_shells_command_exit_code"], 23)
        self.assertTrue(bs._neutral_shell_stamp_valid(iso, stamp))
        log_path.unlink()
        self.assertFalse(bs._neutral_shell_stamp_valid(iso, stamp))

    def test_guarded_cook_is_test_and_adds_bootstrap_and_all_shell_maps(self):
        args = self.args(configuration="Test")
        engine = self.root / "engine"
        runuat = engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.sh"
        runuat.parent.mkdir(parents=True)
        runuat.write_text("#!/bin/sh\n")
        runuat.chmod(0o755)
        # runuat_path resolves the platform launcher; give the fake engine
        # the Windows one too so the fixture works on both lanes.
        (runuat.parent / "RunUAT.bat").write_text("@echo off\n")
        command = bs._cook_command(args, self.root / "iso", engine)
        self.assertIn("-clientconfig=Test", command)
        self.assertNotIn("-clientconfig=Development", command)
        self.assertIn("-build", command)
        self.assertNotIn("-clean", command)
        self.assertIn("-unattended", command)
        self.assertNotIn("-nullrhi", command)
        self.assertTrue(any(
            value.startswith(
                "-AdditionalCookerOptions=-nullrhi -skipzenstore "
                "-ddc=(ZenLocal) "
                "-NoZenAutoLaunch=127.0.0.1:18558 ")
            for value in command))
        map_argument = next(value for value in command if value.startswith("-map="))
        cooked_maps = map_argument.split("=", 1)[1].split("+")
        self.assertEqual(cooked_maps, [
            bs.BOOTSTRAP_MAP,
            "/Game/WoT/Maps/WoTFrontEnd",
            *[f"/WoTNeutralShells/WoTMapShell{slot:02d}"
              for slot in range(1, 20)],
        ])
        self.assertEqual(
            [path for path in cooked_maps
             if path.startswith("/WoTNeutralShells/WoTMapShell")],
            [f"/WoTNeutralShells/WoTMapShell{slot:02d}"
             for slot in range(1, 20)])
        self.assertFalse(any(
            path == f"/Game/WoT/Maps/{name}" for name in bs.MP_MAPS
            for path in cooked_maps))
        self.assertEqual(len(bs.MP_MAPS), 19)
        self.assertNotIn(bs.BOOTSTRAP_MAP, bs._cook_maps(guarded=False))
        self.assertFalse(any(
            path.startswith("/WoTNeutralShells/")
            for path in bs._cook_maps(guarded=False)))
        self.assertEqual(bs._cook_maps(guarded=False), (
            *(f"/Game/WoT/Maps/{name}" for name in bs.MP_MAPS),
            "/Game/WoT/Maps/WoTFrontEnd",
        ))

    def test_guarded_cook_requests_exact_ordered_neutral_support_roots(self):
        args = self.args(configuration="Test")
        engine = self.root / "engine"
        runuat = engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.sh"
        runuat.parent.mkdir(parents=True)
        runuat.write_text("#!/bin/sh\n")
        runuat.chmod(0o755)
        (runuat.parent / "RunUAT.bat").write_text("@echo off\n")

        command = bs._cook_command(args, self.root / "iso", engine)
        cooker_options = next(
            value for value in command
            if value.startswith("-AdditionalCookerOptions="))

        self.assertEqual(
            cooker_options,
            "-AdditionalCookerOptions=-nullrhi -skipzenstore "
            "-ddc=(ZenLocal) -NoZenAutoLaunch=127.0.0.1:18558 "
            "-PACKAGE=" + "+".join(EXPECTED_NEUTRAL_ROOTS))

    def test_guarded_cook_proof_rejects_missing_neutral_support_roots(self):
        command = [
            "RunUAT", "-AdditionalCookerOptions=-nullrhi -skipzenstore "
            "-ddc=(ZenLocal) -NoZenAutoLaunch=127.0.0.1:18558",
        ]

        with self.assertRaisesRegex(bs.BuildFailure, "neutral support roots"):
            bs._validate_guarded_zen_cook_command(command)

    def test_guarded_cook_proof_rejects_reordered_neutral_support_roots(self):
        command = [
            "RunUAT", "-AdditionalCookerOptions=-nullrhi -skipzenstore "
            "-ddc=(ZenLocal) -NoZenAutoLaunch=127.0.0.1:18558 "
            "-PACKAGE=" + "+".join((
                EXPECTED_NEUTRAL_ROOTS[1], EXPECTED_NEUTRAL_ROOTS[0],
                *EXPECTED_NEUTRAL_ROOTS[2:])),
        ]

        with self.assertRaisesRegex(bs.BuildFailure, "neutral support roots"):
            bs._validate_guarded_zen_cook_command(command)

    def test_guarded_inventory_accepts_exact_maps_and_neutral_support(self):
        paths = [
            "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin",
            "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap",
            "../../../WoT/Content/WoT/Maps/WoTFrontEnd.umap",
            *[
                "../../../WoT/Plugins/WoTNeutralShells/Content/"
                f"WoTMapShell{slot:02d}.umap"
                for slot in range(1, 20)
            ],
            *[
                "../../../WoT/Content" + package.removeprefix("/Game")
                + ".uasset" for package in EXPECTED_NEUTRAL_PACKAGES
            ],
        ]

        try:
            validation = bs._validate_guarded_cooked_paths(paths)
        except bs.BuildFailure as exc:
            self.fail(f"exact neutral support closure was rejected: {exc}")

        self.assertEqual(validation, {
            "project_content_count": 66,
            "retail_derived_count": 0,
            "neutral_map_count": 21,
            "neutral_support_package_count": 45,
            "neutral_shell_plugin_descriptor_count": 1,
            "engine_plugin_descriptor_count": 0,
        })

    def test_guarded_inventory_rejects_missing_neutral_support(self):
        paths = [
            "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin",
            "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap",
            "../../../WoT/Content/WoT/Maps/WoTFrontEnd.umap",
            *[
                "../../../WoT/Plugins/WoTNeutralShells/Content/"
                f"WoTMapShell{slot:02d}.umap"
                for slot in range(1, 20)
            ],
            "../../../WoT/Content/WoT/Materials/M_WoT_World.uasset",
        ]

        with self.assertRaisesRegex(
                bs.BuildFailure, "missing neutral support packages"):
            bs._validate_guarded_cooked_paths(paths)

    def test_guarded_inventory_rejects_duplicate_package_entries(self):
        paths = [
            "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin",
            "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap",
            "../../../WoT/Content/WoT/Maps/WoTFrontEnd.umap",
            *[
                "../../../WoT/Plugins/WoTNeutralShells/Content/"
                f"WoTMapShell{slot:02d}.umap"
                for slot in range(1, 20)
            ],
            *[
                "../../../WoT/Content" + package.removeprefix("/Game")
                + ".uasset" for package in EXPECTED_NEUTRAL_PACKAGES
            ],
            "../../../WoT/Content/WoT/Materials/M_WoT_World.uasset",
        ]

        with self.assertRaisesRegex(bs.BuildFailure, "duplicate"):
            bs._validate_guarded_cooked_paths(paths)

    def test_guarded_inventory_rejects_missing_plugin_descriptor(self):
        paths = [
            path for path in self.guarded_cooked_paths()
            if not path.endswith(".uplugin")
        ]
        with self.assertRaisesRegex(bs.BuildFailure, "plugin descriptor"):
            bs._validate_guarded_cooked_paths(paths)

    def test_unrealpak_listing_reroots_legacy_pak_entries(self):
        # Mac stages UFS files in the .pak, whose listing prints paths
        # relative to the announced mount point; IoStore prints full paths.
        output = "\n".join([
            'LogPakFile: Display: Listing WoT-Mac.pak with mount point "../../../"',
            'LogPakFile: Display: "WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin" offset: 10, size: 160 bytes, sha1: A, compression: None.',
            'LogPakFile: Display: "../../../Engine/Plugins/2D/Paper2D/Paper2D.uplugin" offset: 20, size: 9 bytes, sha1: B, compression: None.',
            'LogIoStore: Display: Mount point ../../../',
            'LogIoStore: Display: "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap" offset: 30, size: 726 bytes, hash: c, compression: Oodle.',
            'LogIoStore: Display: "<ShaderCode>" offset: 40, size: 1 bytes, hash: d, compression: Oodle.',
        ])
        self.assertEqual(bs._parse_unrealpak_listing(output), [
            "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin",
            "../../../Engine/Plugins/2D/Paper2D/Paper2D.uplugin",
            "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap",
            "<ShaderCode>",
        ])

    def test_guarded_inventory_allows_mac_engine_plugin_descriptors(self):
        # Mac UAT stages every enabled engine plugin's descriptor; Win64 none.
        paths = self.guarded_cooked_paths() + [
            "Engine/Plugins/2D/Paper2D/Paper2D.uplugin",
            "../../../Engine/Plugins/Runtime/Metasound/Metasound.uplugin",
        ]
        validation = bs._validate_guarded_cooked_paths(paths)
        self.assertEqual(validation["neutral_shell_plugin_descriptor_count"], 1)
        self.assertEqual(validation["engine_plugin_descriptor_count"], 2)

    def test_guarded_inventory_allows_the_optional_onboarding_descriptor(self):
        # UBT stages the descriptor of every plugin that links a module; the
        # onboarding aids plugin is optional, so it may or may not be there,
        # and the validation record keeps its shape either way.
        baseline = bs._validate_guarded_cooked_paths(self.guarded_cooked_paths())
        paths = self.guarded_cooked_paths() + [
            "../../../WoT/Plugins/WoTOnboarding/WoTOnboarding.uplugin"]
        self.assertEqual(bs._validate_guarded_cooked_paths(paths), baseline)

    def test_guarded_inventory_rejects_a_duplicate_onboarding_descriptor(self):
        descriptor = "../../../WoT/Plugins/WoTOnboarding/WoTOnboarding.uplugin"
        paths = self.guarded_cooked_paths() + [descriptor, descriptor]
        with self.assertRaisesRegex(bs.BuildFailure, "duplicate plugin descriptor"):
            bs._validate_guarded_cooked_paths(paths)

    def test_guarded_inventory_rejects_an_onboarding_descriptor_elsewhere(self):
        for descriptor in (
                "../../../WoT/Plugins/WoTOnboarding/wotonboarding.uplugin",
                "../../../WoT/Plugins/Other/WoTOnboarding.uplugin"):
            with self.subTest(descriptor=descriptor):
                paths = self.guarded_cooked_paths() + [descriptor]
                with self.assertRaisesRegex(bs.BuildFailure, "plugin descriptor"):
                    bs._validate_guarded_cooked_paths(paths)

    def test_guarded_inventory_onboarding_never_stands_in_for_the_shell(self):
        paths = [
            path for path in self.guarded_cooked_paths()
            if not path.endswith(".uplugin")
        ] + ["../../../WoT/Plugins/WoTOnboarding/WoTOnboarding.uplugin"]
        with self.assertRaisesRegex(bs.BuildFailure, "plugin descriptor"):
            bs._validate_guarded_cooked_paths(paths)

    def test_guarded_inventory_rejects_an_extra_project_plugin_descriptor(self):
        paths = self.guarded_cooked_paths() + [
            "../../../WoT/Plugins/Other/Other.uplugin"]
        with self.assertRaisesRegex(bs.BuildFailure, "plugin descriptor"):
            bs._validate_guarded_cooked_paths(paths)

    def test_guarded_inventory_rejects_an_engine_path_that_escapes(self):
        paths = self.guarded_cooked_paths() + [
            "Engine/Plugins/../../WoT/Plugins/Other/Other.uplugin"]
        with self.assertRaisesRegex(bs.BuildFailure, "plugin descriptor"):
            bs._validate_guarded_cooked_paths(paths)

    def test_guarded_inventory_rejects_misspelled_plugin_descriptor(self):
        paths = [
            path.replace("WoTNeutralShells.uplugin", "wotneutralshells.uplugin")
            for path in self.guarded_cooked_paths()
        ]
        with self.assertRaisesRegex(bs.BuildFailure, "plugin descriptor"):
            bs._validate_guarded_cooked_paths(paths)

    def test_guarded_mac_import_builds_the_editor_in_the_iso(self):
        iso = self.root / "iso"
        engine = self.root / "engine"
        calls = []

        def run_logged(step, command, log_path, **_kwargs):
            calls.append((step, command))
            return 0

        mac = SimpleNamespace(guarded=True, platform="Mac",
                              compile_cache="off")
        with mock.patch.object(bs, "_run_logged", side_effect=run_logged), \
                mock.patch.object(bs.build_root_cache, "modules_build_id",
                                  return_value="build-id"):
            row = bs._guarded_import_editor(mac, iso, engine, "import")
        self.assertEqual(row["format"], "wot-iso-editor-v1")
        self.assertEqual(row["build_id"], "build-id")
        self.assertEqual(len(calls), 1)
        step, command = calls[0]
        self.assertEqual(step, "iso-editor-import")
        self.assertIn(f"-project={iso / 'WoT.uproject'}", command)
        self.assertEqual(command[1:4], ["WoTEditor", "Mac", "Development"])

        win64 = SimpleNamespace(guarded=True, platform="Win64",
                                compile_cache="off")
        self.assertIsNone(
            bs._guarded_import_editor(win64, iso, engine, "import"))

    def test_iso_built_mac_editor_refuses_a_build_id_mismatch(self):
        ids = iter(("engine-id", "stale-id"))
        mac = SimpleNamespace(guarded=True, platform="Mac",
                              compile_cache="off")
        with mock.patch.object(bs, "_run_logged", return_value=0), \
                mock.patch.object(bs.build_root_cache, "modules_build_id",
                                  side_effect=lambda _path: next(ids)):
            with self.assertRaisesRegex(bs.BuildFailure, "BuildId"):
                bs._guarded_import_editor(
                    mac, self.root / "iso", self.root / "engine", "import")

    def test_shipped_containers_prefer_the_mac_bundle_over_loose_twins(self):
        staged = self.root / "staged"
        loose = staged / "Mac" / "WoT" / "Content" / "Paks"
        bundle = (staged / "Mac" / "WoT.app" / "Contents" / "UE" / "WoT"
                  / "Content" / "Paks")
        for directory in (loose, bundle):
            directory.mkdir(parents=True)
            for name in ("WoT-Mac.pak", "WoT-Mac.utoc", "global.utoc"):
                (directory / name).write_bytes(name.encode())

        self.assertEqual(
            bs._shipped_staged_containers(staged),
            sorted(bundle / name
                   for name in ("WoT-Mac.pak", "WoT-Mac.utoc", "global.utoc")))

    def test_shipped_containers_keep_every_unbundled_container(self):
        paks = self.root / "staged" / "Windows" / "WoT" / "Content" / "Paks"
        paks.mkdir(parents=True)
        for name in ("WoT-Windows.pak", "WoT-Windows.utoc", "global.utoc"):
            (paks / name).write_bytes(name.encode())

        self.assertEqual(
            bs._shipped_staged_containers(self.root / "staged"),
            sorted(paks.iterdir()))

    def _bounded_global_fixture(self, global_output: bytes | None):
        iso = self.root / "iso"
        paks = iso / "staged" / "Mac" / "WoT.app" / "Contents" / "UE" / \
            "WoT" / "Content" / "Paks"
        paks.mkdir(parents=True)
        (paks / "WoT-Mac.utoc").write_bytes(b"project")
        (paks / "global.utoc").write_bytes(b"global")
        tool = self.root / "UnrealPak"
        tool.write_bytes(b"synthetic-unrealpak")
        listing = "".join(
            f'LogPakFile: Display: "{path}" offset: 0,\n'
            for path in self.guarded_cooked_paths())
        timeouts = {}

        def run(command, **kwargs):
            name = Path(command[1]).name
            timeouts[name] = kwargs.get("timeout")
            if name == "global.utoc":
                # The Mac fatal handler blocks after printing (display asleep).
                raise subprocess.TimeoutExpired(
                    command, kwargs["timeout"], output=global_output)
            return subprocess.CompletedProcess(command, 0, stdout=listing,
                                               stderr="")
        return iso, tool, run, timeouts

    def test_global_listing_timeout_after_fatal_is_the_designed_branch(self):
        iso, tool, run, timeouts = self._bounded_global_fixture(
            b"LogMac: Error: appError called: Fatal error: Missing directory "
            b"index for container 'global.utoc'\n")
        with mock.patch.object(bs, "_unrealpak_executable", return_value=tool), \
                mock.patch.object(bs.subprocess, "run", side_effect=run):
            written = bs._write_guarded_cooked_inventory(
                iso, self.root / "engine", "Mac")
        receipt = json.loads(Path(written["path"]).read_text(encoding="utf-8"))
        rows = {Path(row["path"]).name: row for row in receipt["containers"]}
        self.assertEqual(rows["global.utoc"]["listing"],
                         "global-container-has-no-directory-index")
        self.assertIsNone(rows["global.utoc"]["entry_count"])
        self.assertEqual(rows["WoT-Mac.utoc"]["entry_count"],
                         len(self.guarded_cooked_paths()))
        self.assertEqual(timeouts["global.utoc"],
                         bs.GLOBAL_CONTAINER_LIST_TIMEOUT_SECONDS)
        self.assertIsNone(timeouts["WoT-Mac.utoc"])

    def test_global_listing_timeout_without_the_fatal_fails_loudly(self):
        iso, tool, run, _timeouts = self._bounded_global_fixture(None)
        with mock.patch.object(bs, "_unrealpak_executable", return_value=tool), \
                mock.patch.object(bs.subprocess, "run", side_effect=run):
            with self.assertRaisesRegex(bs.BuildFailure, "timed out"):
                bs._write_guarded_cooked_inventory(
                    iso, self.root / "engine", "Mac")

    def test_guarded_inventory_writer_rejects_duplicate_across_containers(self):
        iso = self.root / "iso"
        first = iso / "staged" / "Paks" / "first.pak"
        second = iso / "staged" / "Paks" / "second.pak"
        first.parent.mkdir(parents=True)
        first.write_bytes(b"first")
        second.write_bytes(b"second")
        tool = self.root / "UnrealPak"
        tool.write_bytes(b"synthetic-unrealpak")
        duplicate = "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap"
        descriptor = (
            "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin")
        listed = subprocess.CompletedProcess(
            [], 0, stdout=(
                f'LogPakFile: Display: "{descriptor}" offset: 0,\n'
                f'LogPakFile: Display: "{duplicate}" offset: 0,\n'),
            stderr="")

        with mock.patch.object(
                bs, "_unrealpak_executable", return_value=tool), \
                mock.patch.object(bs.subprocess, "run", return_value=listed):
            with self.assertRaisesRegex(bs.BuildFailure, "duplicate"):
                bs._write_guarded_cooked_inventory(
                    iso, self.root / "engine", "Mac")

    def test_guarded_inventory_rejects_other_project_content_roots(self):
        extras = (
            "../../../WoT/Content/Unexpected/Extra.uasset",
            "../../../WoT/Plugins/Other/Content/Retail.umap",
            "../../../wOt/Content/Unexpected/CaseVariant.uasset",
            "../../../WoT/PLUGINS/Other/Content/CaseVariant.umap",
        )
        for extra in extras:
            with self.subTest(extra=extra), self.assertRaisesRegex(
                    bs.BuildFailure, "unexpected"):
                bs._validate_guarded_cooked_paths([
                    *self.guarded_cooked_paths(), extra])

    def test_guarded_inventory_rejects_sidecars_without_primary_packages(self):
        sidecars = [
            (path if path.endswith(".uplugin")
             else str(Path(path).with_suffix(".uexp")))
            for path in self.guarded_cooked_paths()
        ]
        with self.assertRaisesRegex(bs.BuildFailure, "missing"):
            bs._validate_guarded_cooked_paths(sidecars)

    def test_guarded_receipts_require_bootstrap_map_in_staged_set(self):
        stamp = self.receipt_stamp(maps=bs._cook_maps(guarded=False))
        with self.assertRaisesRegex(bs.BuildFailure, "bootstrap map"):
            bs._guarded_receipt_bindings(stamp, "Mac")

    def test_guarded_receipts_reject_legacy_missing_neutral_support_proof(self):
        stamp = self.receipt_stamp()
        stamp.pop("neutral_support_authority")

        with self.assertRaisesRegex(bs.BuildFailure, "neutral support proof"):
            bs._guarded_receipt_bindings(stamp, "Mac")

    def test_guarded_import_fails_before_unreal_when_authority_is_missing(self):
        iso = self.root / "iso"
        iso.mkdir(parents=True, exist_ok=True)
        args = self.args(
            mesh_source="payload",
            exports_dir=self.root / "exports",
            conv_dir=self.root / "converted",
            retail_root=self.root / "original",
            editor_process_affinity=0xFF,
        )
        editor = self.root / "engine" / "UnrealEditor-Cmd"

        with mock.patch.object(
                bs, "_engine_root_and_editor",
                return_value=(self.root / "engine", editor)), \
                mock.patch.object(
                    bs, "_run_logged",
                    side_effect=bs.BuildFailure(
                        "Unreal import reached before authority")), \
                self.assertRaisesRegex(bs.BuildFailure, "neutral support"):
            bs.stage_import(args, iso)

    def test_guarded_cook_fails_before_build_when_authority_is_missing(self):
        iso = self.root / "iso"
        bootstrap = iso / "Content" / "WoT" / "Maps" / "WoTBootstrap.umap"
        bootstrap.parent.mkdir(parents=True, exist_ok=True)
        bootstrap.write_bytes(b"bootstrap")
        args = self.args(configuration="Test")
        editor = self.root / "engine" / "UnrealEditor-Cmd"

        with mock.patch.object(
                bs, "_engine_root_and_editor",
                return_value=(self.root / "engine", editor)), \
                mock.patch.object(
                    bs, "_run_guarded_target_build",
                    side_effect=bs.BuildFailure(
                        "target build reached before authority")), \
                self.assertRaisesRegex(bs.BuildFailure, "neutral support"):
            bs.stage_cook(args, iso)

    def test_guarded_receipts_require_every_neutral_shell_map(self):
        missing = "/WoTNeutralShells/WoTMapShell13"
        maps = [path for path in bs._cook_maps(guarded=True)
                if path != missing]
        stamp = self.receipt_stamp(maps=maps)
        with self.assertRaisesRegex(bs.BuildFailure, "WoTMapShell13"):
            bs._guarded_receipt_bindings(stamp, "Mac")

    def test_guarded_receipts_require_neutral_front_end_shell(self):
        maps = [path for path in bs._cook_maps(guarded=True)
                if path != "/Game/WoT/Maps/WoTFrontEnd"]
        stamp = self.receipt_stamp(maps=maps)
        with self.assertRaisesRegex(bs.BuildFailure, "front-end shell"):
            bs._guarded_receipt_bindings(stamp, "Mac")

    def test_guarded_cooked_inventory_rejects_retail_project_content(self):
        paths = [
            "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin",
            "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap",
            "../../../WoT/Content/WoT/Maps/WoTFrontEnd.umap",
            "../../../WoT/Plugins/WoTNeutralShells/Content/WoTMapShell01.umap",
            "../../../WoT/Content/WoT/Maps/Arena_01.umap",
        ]
        with self.assertRaisesRegex(bs.BuildFailure, "Arena_01"):
            bs._validate_guarded_cooked_paths(paths)

    def test_guarded_cooked_inventory_accepts_only_exact_neutral_consumers(self):
        paths = [
            "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin",
            "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap",
            "../../../WoT/Content/WoT/Maps/WoTFrontEnd.umap",
            *[
                "../../../WoT/Plugins/WoTNeutralShells/Content/"
                f"WoTMapShell{slot:02d}.umap"
                for slot in range(1, 20)
            ],
            *[
                "../../../WoT/Content" + package.removeprefix("/Game")
                + ".uasset" for package in EXPECTED_NEUTRAL_PACKAGES
            ],
            "../../../WoT/Content/ShaderArchive-WoT-PCD3D_SM6-PCD3D_SM6."
            "ushaderbytecode",
        ]
        self.assertEqual(
            bs._validate_guarded_cooked_paths(paths),
            {
                "project_content_count": 66,
                "retail_derived_count": 0,
                "neutral_map_count": 21,
                "neutral_support_package_count": 45,
                "neutral_shell_plugin_descriptor_count": 1,
                "engine_plugin_descriptor_count": 0,
            },
        )

    def test_guarded_inventory_binding_rejects_an_extra_staged_container(self):
        stamp = self.receipt_stamp()
        extra = self.root / "iso" / "staged" / "Paks" / "extra.pak"
        extra.write_bytes(b"unreceipted-container")
        with self.assertRaisesRegex(bs.BuildFailure, "container census"):
            bs._guarded_receipt_bindings(stamp, "Mac")

    def test_guarded_inventory_binding_rejects_legacy_v1_receipt(self):
        stamp = self.receipt_stamp()
        path = Path(stamp["cooked_container_inventory"]["path"])
        receipt = json.loads(path.read_text(encoding="utf-8"))
        receipt["format"] = "wot-guarded-cooked-container-inventory-v1"
        bs._write_json(path, receipt)
        stamp["cooked_container_inventory"]["sha256"] = bs._sha256_file(path)

        with self.assertRaisesRegex(bs.BuildFailure, "identity is wrong"):
            bs._guarded_receipt_bindings(stamp, "Mac")

    def test_guarded_layout_requires_only_the_main_artifact(self):
        mac = self.root / "mac"
        with self.assertRaisesRegex(bs.BuildFailure, "main artifact is missing"):
            bs._require_guarded_staged_layout(mac, "Mac")
        (mac / "WoT.app").mkdir(parents=True)
        self.assertEqual(bs._require_guarded_staged_layout(mac, "Mac"), mac / "WoT.app")
        win = self.root / "win"
        game = win / "WoT" / "Binaries" / "Win64" / "WoT-Win64-Test.exe"
        game.parent.mkdir(parents=True)
        game.write_bytes(b"game")
        self.assertEqual(bs._require_guarded_staged_layout(win, "Win64"), game)


    def test_guarded_manifest_binds_both_target_receipts_and_engine(self):
        evidence = self.guarded_evidence()
        manifest = bs._guarded_release_manifest(
            asset_name="WoT-Mac-guarded-abc123456.zip",
            sha256="a" * 64, size=123, platform="Mac",
            commit="abc123456",
            audit_stamp=evidence["audit"],
            cook_stamp=evidence["cook"], iso=evidence["iso"],
            source_snapshot=evidence["source"], dmg={
                "installed_tree_root": "b" * 64,
                "name": "WoT-Mac-guarded-abc123456.dmg",
                "sha256": "c" * 64,
                "size": 1,
                "verify_sidecar": {"name": "x", "sha256": "d" * 64,
                                   "size": 1},
                "volume_name": "WoT Reborn",
            })
        self.assertEqual(manifest["format"], "wot-guarded-release-v1")
        self.assertEqual(manifest["configuration"], "Test")
        self.assertEqual(set(manifest["target_receipts"]),
                         {"WoT"})
        for target in bs.GUARDED_RECEIPT_TARGETS:
            binding = manifest["target_receipts"][target]
            self.assertEqual(binding["compiled_receipt"]["target"], target)
            self.assertEqual(len(binding["compiled_receipt_sha256"]), 64)
            self.assertEqual(len(binding["engine_receipt_sha256"]), 64)
        self.assertEqual(
            manifest["engine_identity"],
            manifest["target_receipts"]["WoT"]["compiled_receipt"]["engine"])
        self.assertEqual(manifest["source_snapshot"], evidence["source"])
        self.assertNotIn("test_capabilities", manifest)
        self.assertEqual(
            manifest["audit"]["staged_tree_sha256"],
            evidence["audit"]["staged_tree"]["tree_sha256"])
        self.assertEqual(
            set(manifest["products"]["receipt"]["products"]),
            {"WoT"})
        self.assertEqual(
            manifest["zen_lifetime"]["receipt"]["mechanism"],
            "pipeline-sponsored-zen-v3")
        self.assertNotIn("authenticated_alternate_bind", manifest)


    def test_post_audit_staged_tree_mutation_is_refused(self):
        evidence = self.guarded_evidence()
        (evidence["iso"] / "staged" / "Mac" / "post-audit.bin").write_bytes(
            b"mutation")

        with self.assertRaisesRegex(bs.BuildFailure, "changed after"):
            bs._validate_staged_tree_receipt(
                evidence["iso"], evidence["audit"])

    def test_private_package_snapshot_defeats_transient_live_replacement(self):
        evidence = self.guarded_evidence()
        iso = evidence["iso"]
        live = iso / bs.STAGING_DIRECTORIES[bs.GUARDED_CONFIGURATION]
        relative = evidence["game"].relative_to(live)
        original = evidence["game"].read_bytes()
        with bs._private_staged_snapshot(
                iso, live, evidence["audit"]["staged_tree"]) as snapshot:
            if os.name != "nt":
                # POSIX owner-only mode; NTFS reports 0o777 for directories.
                self.assertEqual(
                    bs.stat.S_IMODE(snapshot.parent.stat().st_mode), 0o700)
            # Model an attacker replacing and restoring the live tree while
            # zip/DMG readers are active. Both readers receive only snapshot.
            evidence["game"].write_bytes(b"transient unaudited replacement")
            self.assertEqual((snapshot / relative).read_bytes(), original)
            evidence["game"].write_bytes(original)
            self.assertEqual(
                bs._staged_tree_inventory(live)["tree_sha256"],
                evidence["audit"]["staged_tree"]["tree_sha256"])
        self.assertEqual(list(iso.glob(bs.PACKAGE_SNAPSHOT_PREFIX + "*")), [])

    def test_package_startup_removes_only_owned_stale_snapshots(self):
        evidence = self.guarded_evidence()
        iso = evidence["iso"]
        owned = iso / (bs.PACKAGE_SNAPSHOT_PREFIX + "abandoned")
        owned.mkdir()
        bs._write_json(owned / bs.PACKAGE_SNAPSHOT_MARKER, {
            "format": bs.PACKAGE_SNAPSHOT_FORMAT,
            "iso": str(iso.resolve()),
        })
        (owned / "staged").mkdir()
        (owned / "staged" / "partial.bin").write_bytes(b"partial")
        unowned = iso / (bs.PACKAGE_SNAPSHOT_PREFIX + "not-ours")
        unowned.mkdir()
        (unowned / "keep.txt").write_text("user data", encoding="utf-8")

        self.assertEqual(bs._cleanup_stale_package_snapshots(iso), 1)
        self.assertFalse(owned.exists())
        self.assertTrue((unowned / "keep.txt").is_file())

    def test_guarded_win64_persistent_replacement_during_zip_is_detected(self):
        import zipfile

        iso = self.root / "iso"
        staged = iso / "staged" / "Windows"
        game = staged / "WoT" / "Binaries" / "Win64" / "WoT-Win64-Test.exe"
        game.parent.mkdir(parents=True)
        game.write_bytes(b"audited game")
        updater = staged / "InternalTools" / "WoTUpdater.exe"
        updater.parent.mkdir(parents=True)
        updater.write_bytes(b"audited updater")
        audited = bs._staged_tree_inventory(iso / "staged")
        source = {
            "format": bs.SOURCE_SNAPSHOT_FORMAT,
            "git_commit": "abc123456",
            "paths": list(bs.SOURCE_SNAPSHOT_PATHS),
            "tracked_diff_size": 0,
            "tracked_diff_sha256": "3" * 64,
            "untracked": [],
            "sha256": "4" * 64,
        }
        bs._write_stamp(iso, "seed", {
            "release_commit": "abc123456",
            "release_sequence": 0,
            "source_snapshot": source,
        })
        bs._write_stamp(iso, "cook", {
            "neutral_support_authority": {"sha256": "a" * 64},
        })
        bs._write_stamp(iso, "audit", {"cache_sha256": "c" * 64})
        original_write = zipfile.ZipFile.write
        replaced = False

        def replace_after_receipt_validation(
                archive, filename, arcname=None, compress_type=None,
                compresslevel=None):
            nonlocal replaced
            result = original_write(
                archive, filename, arcname, compress_type, compresslevel)
            if not replaced:
                game.write_bytes(b"persistent unaudited replacement")
                replaced = True
            return result

        products = {"receipt": {"stage_root": "Windows"}}
        with mock.patch.object(bs, "_git_commit", return_value="abc123456"), \
                mock.patch.object(bs, "_source_snapshot", return_value=source), \
                mock.patch.object(bs, "_guarded_receipt_bindings"), \
                mock.patch.object(
                    bs, "_neutral_support_lifecycle_receipt",
                    return_value={"sha256": "a" * 64}), \
                mock.patch.object(
                    bs, "_guarded_product_binding", return_value=products), \
                mock.patch.object(bs, "_guarded_zen_binding"), \
                mock.patch.object(
                    bs, "_validate_staged_tree_receipt",
                    return_value={"tree_sha256": audited["tree_sha256"]}), \
                mock.patch.object(bs, "_require_guarded_staged_layout"), \
                mock.patch.object(bs, "_require_staged_production_descriptor"), \
                mock.patch.object(bs, "_require_release_notes_absent"), \
                mock.patch.object(bs, "_require_staged_release_identity"), \
                mock.patch.object(bs, "_require_staged_release_version"), \
                mock.patch.object(
                    bs.windows_launcher, "build_launcher",
                    side_effect=lambda destination, _game, _configuration:
                        Path(destination).write_bytes(
                            b"MZ synthetic launcher")), \
                mock.patch.object(
                    zipfile.ZipFile, "write",
                    side_effect=replace_after_receipt_validation, autospec=True), \
                self.assertRaisesRegex(
                    bs.BuildFailure,
                    "staged tree changed while the archive was created"):
            bs.stage_package(self.args(platform="Win64"), iso)
        self.assertTrue(replaced)
        self.assertFalse(
            (iso / "release" / "WoT-Win64-guarded-abc123456.zip").exists())

    def test_staged_tree_rejects_loose_alternate_bind_sources_and_receipt(self):
        for relative in (
                "Win64/Shipping/Mesh.GLB",
                "Win64/Shipping/Mesh.META.JSON",
                "Win64/Shipping/AUTHENTICATED-ALTERNATE-BIND.JSON"):
            staged = self.root / "staged"
            path = staged / Path(relative)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"forbidden")
            with self.subTest(relative=relative), self.assertRaisesRegex(
                    bs.BuildFailure, "forbidden alternate-bind"):
                bs._staged_tree_inventory(staged)
            path.unlink()

    def test_staged_tree_rejects_renamed_glb_or_alternate_receipt_content(self):
        staged = self.root / "staged"
        path = staged / "Win64" / "Shipping" / "renamed.bin"
        path.parent.mkdir(parents=True)
        for content in (
                b"glTF" + b"\0" * 32,
                b'{"format":"wot-authenticated-alternate-bind-v1"}'):
            path.write_bytes(content)
            with self.subTest(content=content[:8]), self.assertRaisesRegex(
                    bs.BuildFailure, "forbidden alternate-bind"):
                bs._staged_tree_inventory(staged)

    def test_staged_tree_forbidden_name_gate_applies_before_file_type(self):
        for relative in (
                "Win64/Shipping/Mesh.GLB",
                "Win64/Shipping/x.meta.json",
                "Win64/Shipping/authenticated-alternate-bind.json"):
            with self.subTest(relative=relative), self.assertRaisesRegex(
                    bs.BuildFailure, "forbidden alternate-bind"):
                bs._reject_forbidden_alternate_bind_name(relative)

    def test_stale_final_product_swap_is_refused(self):
        evidence = self.guarded_evidence()
        evidence["game"].write_bytes(b"game-stale")

        with self.assertRaisesRegex(bs.BuildFailure, "changed after"):
            bs._guarded_product_binding(
                evidence["cook"], evidence["iso"], "Mac")

    def test_package_cache_validates_release_json_identity(self):
        evidence = self.guarded_evidence()
        iso = evidence["iso"]
        bs._write_stamp(iso, "seed", {
            "release_commit": "abc123456",
            "release_sequence": 0,
            "source_snapshot": evidence["source"],
            "neutral_support_authority":
                evidence["cook"]["neutral_support_authority"],
        })
        bs._write_stamp(iso, "import", {
            "neutral_support_authority":
                evidence["cook"]["neutral_support_authority"],
        })
        bs._write_stamp(iso, "cook", evidence["cook"])
        bs._write_stamp(iso, "audit", evidence["audit"])

        def fake_ditto(command, **_kwargs):
            if command[0] == "cp":
                shutil.copytree(command[-2], command[-1], symlinks=True)
                return SimpleNamespace(returncode=0, stdout="", stderr="")
            self.assertEqual(command[0], "ditto")
            self.assertIn("--norsrc", command)
            self.assertIn("--noextattr", command)
            write_fake_mac_zip(command[-1])
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        with mock.patch.object(
                bs, "_git_commit", return_value="abc123456"), \
                mock.patch.object(
                    bs, "_source_snapshot",
                    return_value=evidence["source"]), \
                mock.patch.object(
                    bs.subprocess, "run", side_effect=fake_ditto) as run:
            self.assertEqual(bs.stage_package(self.args(), iso), "ran")
            self.assertEqual(bs.stage_package(self.args(), iso), "cached")
            release_json = iso / "release" / "release.json"
            release_json.write_text("{}")
            self.assertEqual(bs.stage_package(self.args(), iso), "ran")
            self.assertEqual(
                sum(call.args[0][0] == "ditto" for call in run.call_args_list), 2)

    def test_sponsored_zen_lifetime_preserves_preexisting_processes(self):
        iso = self.root / "iso"
        engine = self.root / "engine"
        zen = engine / "Engine" / "Binaries" / "Mac" / "zenserver"
        zen.parent.mkdir(parents=True)
        zen.write_bytes(b"pinned zen")
        process = mock.Mock(pid=321, returncode=0)
        process.poll.return_value = None
        process.wait.return_value = 0

        foreign = {
            "pid": 55,
            "command": "/foreign/zenserver --port 8558 --owner-pid 44",
        }
        owner_pid = bs.os.getpid()
        data_dir = iso / "Saved" / f"GuardedZenSponsor-{owner_pid}"
        sponsor_command = [
            str(zen), "--port", "18558", "--allow-port-probing=false",
            "--http-forceloopback", "--data-dir", str(data_dir),
            "--owner-pid", str(owner_pid), "--detach=false", "--no-sentry",
            "--quiet",
        ]
        sponsor = {"pid": 321, "command": " ".join(sponsor_command)}
        monitor = {
            "interval_seconds": 0.1, "sample_count": 2,
            "started_utc": "2026-08-26T00:00:00+00:00",
            "finished_utc": "2026-08-26T00:01:00+00:00",
            "violation": None,
        }
        preflight = {
            "host": "127.0.0.1", "port": 18558,
            "exclusive_bind_succeeded": True,
            "checked_utc": "2026-08-26T00:00:00+00:00",
        }
        with mock.patch.object(
                bs, "_zen_processes", side_effect=[
                    [foreign],
                    [foreign, sponsor],
                    [foreign, sponsor],
                    [foreign],
                ]), \
                mock.patch.object(
                    bs, "_wait_for_zen_health",
                    return_value={
                        "url": bs.ZEN_SPONSOR_ENDPOINT + "/health/ready",
                        "status_code": 200,
                        "body_sha256": "3" * 64,
                        "checked_utc": "2026-07-29T00:00:00+00:00",
                    }), \
                mock.patch.object(
                    bs, "_prove_zen_endpoint_unoccupied",
                    return_value=preflight), \
                mock.patch.object(
                    bs, "_run_action_with_zen_monitor",
                    return_value=(0, monitor)), \
                mock.patch.object(bs.subprocess, "Popen",
                                  return_value=process) as popen, \
                mock.patch.object(bs, "_run_logged", return_value=0):
            row = bs._run_guarded_cook_with_sponsored_zen(
                ["RunUAT", "-AdditionalCookerOptions=-nullrhi "
                 "-skipzenstore -ddc=(ZenLocal) "
                 "-NoZenAutoLaunch=127.0.0.1:18558 "
                 + self.neutral_support_package_option()],
                iso / "logs" / "cook.log", iso, engine, "Mac")

        self.assertEqual(
            row["receipt"]["mechanism"], "pipeline-sponsored-zen-v3")
        self.assertEqual(row["receipt"]["before"], [foreign])
        self.assertEqual(row["receipt"]["after"], [foreign])
        self.assertEqual(row["receipt"]["health"]["status_code"], 200)
        self.assertEqual(row["receipt"]["sponsor"]["pid"], 321)
        self.assertTrue(row["receipt"]["cleanup"]["verified_stopped"])
        self.assertTrue(
            row["receipt"]["cleanup"]["data_directory_removed"])
        self.assertEqual(
            row["receipt"]["version"]["sha256"], bs._sha256_file(zen))
        process.terminate.assert_called_once()
        popen.assert_called_once()
        sponsor_command = popen.call_args.args[0]
        self.assertEqual(
            sponsor_command[sponsor_command.index("--port") + 1], "18558")
        self.assertIn("--allow-port-probing=false", sponsor_command)

    def test_sponsored_zen_refuses_missing_isolation_switches_before_start(self):
        for command in (
                ["RunUAT", "-AdditionalCookerOptions=-skipzenstore"],
                ["RunUAT", "-AdditionalCookerOptions="
                 "-NoZenAutoLaunch=127.0.0.1:18558"]):
            with self.subTest(command=command), \
                    mock.patch.object(bs, "_zen_processes", return_value=[]), \
                    mock.patch.object(bs.subprocess, "Popen") as popen, \
                    mock.patch.object(bs, "_run_logged") as run:
                with self.assertRaisesRegex(bs.BuildFailure, "isolated Zen"):
                    bs._run_guarded_cook_with_sponsored_zen(
                        command, self.root / "cook.log",
                        self.root / "iso", self.root / "engine", "Mac")
            run.assert_not_called()
            popen.assert_not_called()

    def test_preexisting_fixed_zen_endpoint_is_refused_before_start(self):
        foreign = {
            "pid": 55,
            "command": "/foreign/zenserver --port=18558 --owner-pid 44",
        }
        command = ["RunUAT", "-AdditionalCookerOptions=-skipzenstore "
                   "-ddc=(ZenLocal) "
                   "-NoZenAutoLaunch=127.0.0.1:18558 "
                   + self.neutral_support_package_option()]
        with mock.patch.object(bs, "_zen_processes", return_value=[foreign]), \
                mock.patch.object(bs.subprocess, "Popen") as popen, \
                mock.patch.object(bs, "_run_logged") as run:
            with self.assertRaisesRegex(bs.BuildFailure, "18558"):
                bs._run_guarded_cook_with_sponsored_zen(
                    command, self.root / "cook.log", self.root / "iso",
                    self.root / "engine", "Mac")
        popen.assert_not_called()
        run.assert_not_called()

    def test_sponsored_zen_refuses_changed_preexisting_process_set(self):
        iso = self.root / "iso"
        engine = self.root / "engine"
        zen = engine / "Engine" / "Binaries" / "Mac" / "zenserver"
        zen.parent.mkdir(parents=True)
        zen.write_bytes(b"pinned zen")
        process = mock.Mock(pid=321, returncode=0)
        process.poll.return_value = None
        process.wait.return_value = 0
        before = {"pid": 55, "command": "/foreign/zenserver --port 8558"}
        replaced = {"pid": 55, "command": "/foreign/zenserver --port 9999"}
        owner_pid = bs.os.getpid()
        sponsor_command = bs._canonical_zen_sponsor_command(
            str(zen), iso / "Saved" / f"GuardedZenSponsor-{owner_pid}",
            owner_pid)
        sponsor = {"pid": 321, "command": " ".join(sponsor_command)}
        command = ["RunUAT", "-AdditionalCookerOptions=-skipzenstore "
                   "-ddc=(ZenLocal) "
                   "-NoZenAutoLaunch=127.0.0.1:18558 "
                   + self.neutral_support_package_option()]
        with mock.patch.object(bs, "_zen_processes", side_effect=[
                [before], [before, sponsor], [replaced, sponsor], [replaced]
                ]), mock.patch.object(
                    bs, "_wait_for_zen_health", return_value={
                        "url": "http://127.0.0.1:18558/health/ready",
                        "status_code": 200,
                        "body_sha256": "3" * 64,
                        "checked_utc": "2026-08-26T00:00:00+00:00",
                    }), mock.patch.object(
                        bs, "_prove_zen_endpoint_unoccupied", return_value={
                            "host": "127.0.0.1", "port": 18558,
                            "exclusive_bind_succeeded": True,
                            "checked_utc": "2026-08-26T00:00:00+00:00",
                        }), mock.patch.object(
                        bs, "_run_action_with_zen_monitor", return_value=(0, {
                            "interval_seconds": 0.1, "sample_count": 2,
                            "started_utc": "2026-08-26T00:00:00+00:00",
                            "finished_utc": "2026-08-26T00:01:00+00:00",
                            "violation": None,
                        })), mock.patch.object(
                        bs.subprocess, "Popen", return_value=process), \
                mock.patch.object(bs, "_run_logged", return_value=0):
            with self.assertRaisesRegex(bs.BuildFailure, "preexisting Zen"):
                bs._run_guarded_cook_with_sponsored_zen(
                    command, iso / "logs" / "cook.log", iso, engine, "Mac")
        process.terminate.assert_called_once()

    def test_zen_partition_refuses_missing_owned_and_unexpected_foreign(self):
        before = [{"pid": 55, "command": "/foreign/zenserver --port 8558"}]
        sponsor_command = [
            "/engine/zenserver", "--port", "18558",
            "--allow-port-probing=false", "--http-forceloopback",
            "--data-dir", "/owned", "--owner-pid", "123",
            "--detach=false", "--no-sentry", "--quiet",
        ]
        sponsor = {
            "pid": 321,
            "command": " ".join(sponsor_command),
        }
        hostile = (
            before,
            [*before, sponsor,
             {"pid": 77, "command": "/unexpected/zenserver --port 9000"}],
            [{"pid": 55, "command": "/foreign/zenserver --port 9999"},
             sponsor],
            [*before,
             {"pid": 321, "command": "/engine/zenserver --port 8558"}],
        )
        for during in hostile:
            with self.subTest(during=during), self.assertRaises(bs.BuildFailure):
                bs._validate_zen_process_partition(
                    before, during, 321, sponsor_command)

    def test_zen_identity_refuses_boolean_and_duplicate_pids(self):
        hostile = (
            [{"pid": True, "command": "/engine/zenserver"}],
            [{"pid": 55, "command": "/engine/zenserver --port 1"},
             {"pid": 55, "command": "/engine/zenserver --port 2"}],
        )
        for rows in hostile:
            with self.subTest(rows=rows), self.assertRaises(bs.BuildFailure):
                bs._zen_identity_set(rows, "hostile")

    def test_sponsored_zen_detects_transient_process_during_cook(self):
        iso = self.root / "iso"
        engine = self.root / "engine"
        zen = engine / "Engine" / "Binaries" / "Mac" / "zenserver"
        zen.parent.mkdir(parents=True)
        zen.write_bytes(b"pinned zen")
        owner_pid = bs.os.getpid()
        data_dir = iso / "Saved" / f"GuardedZenSponsor-{owner_pid}"
        sponsor_command = [
            str(zen), "--port", "18558", "--allow-port-probing=false",
            "--http-forceloopback", "--data-dir", str(data_dir),
            "--owner-pid", str(owner_pid), "--detach=false", "--no-sentry",
            "--quiet",
        ]
        sponsor = {"pid": 321, "command": " ".join(sponsor_command)}
        unexpected = {"pid": 77, "command": "/unexpected/zenserver --port 9"}
        state = {"calls": 0, "running": False, "cleaned": False,
                 "emitted": False}

        def scan():
            state["calls"] += 1
            if state["calls"] == 1:
                return []
            if state["cleaned"]:
                return []
            if state["running"] and not state["emitted"]:
                state["emitted"] = True
                return [sponsor, unexpected]
            return [sponsor]

        def run(*_args, **_kwargs):
            state["running"] = True
            bs.time.sleep(0.25)
            state["running"] = False
            return 0

        process = mock.Mock(pid=321, returncode=0)
        process.poll.return_value = None
        process.wait.side_effect = lambda **_kwargs: state.update(
            cleaned=True) or 0
        preflight = {
            "host": "127.0.0.1", "port": 18558,
            "exclusive_bind_succeeded": True,
            "checked_utc": "2026-08-26T00:00:00+00:00",
        }
        command = ["RunUAT", "-AdditionalCookerOptions=-skipzenstore "
                   "-ddc=(ZenLocal) "
                   "-NoZenAutoLaunch=127.0.0.1:18558 "
                   + self.neutral_support_package_option()]
        with mock.patch.object(bs, "_zen_processes", side_effect=scan), \
                mock.patch.object(bs, "_prove_zen_endpoint_unoccupied",
                                  return_value=preflight, create=True), \
                mock.patch.object(bs, "_wait_for_zen_health", return_value={
                    "url": "http://127.0.0.1:18558/health/ready",
                    "status_code": 200, "body_sha256": "3" * 64,
                    "checked_utc": "2026-08-26T00:00:00+00:00"}), \
                mock.patch.object(bs.subprocess, "Popen", return_value=process), \
                mock.patch.object(bs, "_run_logged", side_effect=run):
            with self.assertRaisesRegex(
                    bs.BuildFailure, "during guarded Unreal lifecycle"):
                bs._run_guarded_cook_with_sponsored_zen(
                    command, iso / "logs" / "cook.log", iso, engine, "Mac")
        process.terminate.assert_called_once()

    def test_guarded_zen_binding_rejects_legacy_and_tampered_receipts(self):
        iso = self.root / "iso"
        iso.mkdir()
        path = iso / "receipts" / "zen-lifetime.json"
        path.parent.mkdir()
        engine = self.root / "engine"
        pinned_zen = engine / "Engine" / "Binaries" / "Mac" / "zenserver"
        pinned_zen.parent.mkdir(parents=True)
        pinned_zen.write_bytes(b"pinned zen")
        editor = pinned_zen.with_name("UnrealEditor-Cmd")
        editor.write_bytes(b"editor")
        runuat = engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.sh"
        runuat.parent.mkdir(parents=True)
        runuat.write_bytes(b"runuat")
        (iso / "WoT.uproject").write_text("{}", encoding="utf-8")
        owned_data_dir = iso / "Saved" / "GuardedZenSponsor-123"
        canonical_command = [
            str(pinned_zen), "--port", "18558",
            "--allow-port-probing=false", "--http-forceloopback",
            "--data-dir", str(owned_data_dir), "--owner-pid", "123",
            "--detach=false", "--no-sentry", "--quiet",
        ]
        command_rows = []
        for stage, kind in (
                ("import", "editor"),
                ("finalize-stripped-fractal-catalog", "editor"),
                ("generate-neutral-map-shells", "editor"),
                ("cook", "cook")):
            log = iso / "logs" / f"{stage}.log"
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text("owned Zen 127.0.0.1:18558\n", encoding="utf-8")
            script_name = {
                "import": "import_all.py",
                "finalize-stripped-fractal-catalog":
                    "finalize_stripped_fractal_catalog.py",
                "generate-neutral-map-shells":
                    "generate_neutral_map_shells.py",
            }.get(stage)
            script_argument = None
            if kind == "editor":
                script_argument = "-script=" + str(
                    iso / "Tools" / "import" / script_name).replace(
                        "\\", "/")
                if stage != "finalize-stripped-fractal-catalog":
                    script_argument += " --fixture=1"
            command = ([
                str(editor), str(iso / "WoT.uproject"),
                "-run=pythonscript",
                script_argument,
                "-ddc=(ZenLocal)", bs.ZEN_NO_AUTO_LAUNCH,
            ] if kind == "editor" else [
                str(runuat), "BuildCookRun",
                f"-project={iso / 'WoT.uproject'}", "-platform=Mac",
                "-clientconfig=Test", "-build", "-cook", "-stage", "-pak",
                f"-stagingdirectory={iso / 'staged'}",
                "-map=" + "+".join(bs._cook_maps(guarded=True)),
                "-AdditionalCookerOptions=-skipzenstore "
                "-ddc=(ZenLocal) " + bs.ZEN_NO_AUTO_LAUNCH + " "
                + self.neutral_support_package_option(),
            ])
            command_rows.append({
                "stage": stage, "kind": kind, "command": command,
                "log": str(log), "log_sha256": bs._sha256_file(log),
                "exit_code": 0,
                "stripped_ddc_environment_overrides":
                    ["UE-LocalDataCachePath"],
            })
        legacy = {
            "format": bs.ZEN_RECEIPT_FORMAT,
            "mechanism": "pipeline-sponsored-zen-v1",
            "cook_switch": "-skipzenstore",
            "endpoint": bs.ZEN_SPONSOR_ENDPOINT,
            "owner": {"kind": "pipeline-process", "pid": 123},
            "sponsor": {
                "pid": 321,
                "command": ["/engine/zenserver", "--port", "18558"],
                "log": "/iso/logs/zen-sponsor.log",
            },
            "version": {"path": "/engine/zenserver", "size": 123,
                        "sha256": "2" * 64},
            "before": [],
            "health": {
                "url": "http://127.0.0.1:18558/health/ready",
                "status_code": 200,
                "body_sha256": "3" * 64,
                "checked_utc": "2026-08-26T00:00:00+00:00",
            },
            "sponsored_processes_after_cook": [
                {"pid": 321, "command": "/engine/zenserver --port 18558"}],
            "after": [],
            "cleanup": {
                "action": "stop-pipeline-sponsor",
                "signal": "terminate", "forced": False,
                "wait_return_code": 0, "verified_stopped": True,
                "data_directory_removed": True,
            },
            "lifecycle_status": "success",
        }
        bs._write_json(path, legacy)
        def bound_stamp():
            return {
                "platform": "Mac", "engine": str(engine),
                "log": str(iso / "logs" / "cook.log"),
                "maps": list(bs._cook_maps(guarded=True)),
                "zen_lifetime": {
                    "path": str(path), "sha256": bs._sha256_file(path)},
            }

        legacy_stamp = bound_stamp()
        with self.assertRaisesRegex(bs.BuildFailure, "Zen lifetime"):
            bs._guarded_zen_binding(legacy_stamp, iso, "Mac")

        base = {
            **legacy,
            "mechanism": "pipeline-sponsored-zen-v3",
            "unreal_isolation_switches": [
                "-ddc=(ZenLocal)", bs.ZEN_NO_AUTO_LAUNCH],
            "endpoint": "http://127.0.0.1:18558",
            "before": [{"pid": 55, "command": "/foreign/zenserver"}],
            "endpoint_preflight": {
                "host": "127.0.0.1", "port": 18558,
                "exclusive_bind_succeeded": True,
                "checked_utc": "2026-08-26T00:00:00+00:00",
            },
            "processes_after_lifecycle": [
                {"pid": 55, "command": "/foreign/zenserver"},
                {"pid": 321,
                 "command": " ".join(canonical_command)},
            ],
            "after": [{"pid": 55, "command": "/foreign/zenserver"}],
            "monitor": {
                "interval_seconds": 0.1, "sample_count": 2,
                "started_utc": "2026-08-26T00:00:00+00:00",
                "finished_utc": "2026-08-26T00:01:00+00:00",
                "violation": None,
            },
            "unreal_commands": command_rows,
            "environment_policy": "strip-all-ue-datacache-overrides-v1",
            "stripped_ddc_environment_overrides":
                ["UE-LocalDataCachePath"],
            "cleanup": {**legacy["cleanup"], "target_pid": 321},
            "sponsor": {**legacy["sponsor"], "command": canonical_command},
            "version": {
                "path": str(pinned_zen),
                "size": pinned_zen.stat().st_size,
                "sha256": bs._sha256_file(pinned_zen),
            },
        }
        del base["cook_switch"]
        del base["sponsored_processes_after_cook"]
        legacy_v2 = {**base, "mechanism": "pipeline-sponsored-zen-v2"}
        bs._write_json(path, legacy_v2)
        with self.assertRaisesRegex(bs.BuildFailure, "Zen lifetime"):
            bs._guarded_zen_binding(bound_stamp(), iso, "Mac")
        bs._write_json(path, base)
        valid_stamp = bound_stamp()
        self.assertEqual(
            bs._guarded_zen_binding(
                valid_stamp, iso, "Mac")["receipt"]["mechanism"],
            "pipeline-sponsored-zen-v3")
        with self.assertRaisesRegex(bs.BuildFailure, "Zen lifetime"):
            bs._guarded_zen_binding(
                valid_stamp, self.root / "other-iso", "Mac")
        wrong_platform = bound_stamp()
        wrong_platform["platform"] = "Win64"
        with self.assertRaisesRegex(bs.BuildFailure, "Zen lifetime"):
            bs._guarded_zen_binding(wrong_platform, iso, "Mac")
        foreign_zen = self.root / "foreign" / "zenserver"
        foreign_zen.parent.mkdir()
        foreign_zen.write_bytes(b"foreign zen")
        foreign_command = bs._canonical_zen_sponsor_command(
            str(foreign_zen), owned_data_dir, 123)
        hostile = (
            {**base, "unexpected_top_level": True},
            {**base,
             "sponsor": {**base["sponsor"], "unexpected": True}},
            {**base,
             "unreal_commands": [{
                 **base["unreal_commands"][0], "exit_code": 9},
                 *base["unreal_commands"][1:]]},
            {**base,
             "cleanup": {
                 **base["cleanup"], "signal": 7, "forced": "false",
                 "wait_return_code": True, "verified_stopped": 1,
                 "data_directory_removed": 1}},
            {**base,
             "endpoint": "http://127.0.0.1:8558"},
            {**base,
             "health": {**base["health"], "status_code": 503}},
            {**base,
             "health": {
                 key: value for key, value in base["health"].items()
                 if key != "checked_utc"}},
            {**base,
             "health": {**base["health"],
                        "checked_utc": "2026-08-26T00:02:00+00:00"}},
            {**base, "environment_policy": "inherit-environment-v1"},
            {**base,
             "unreal_commands": [{
                 **base["unreal_commands"][0],
                 "stripped_ddc_environment_overrides": []},
                 *base["unreal_commands"][1:]]},
            {**base,
             "cleanup": {**base["cleanup"], "target_pid": 55}},
            {**base,
             "sponsor": {**base["sponsor"],
                         "log": "/iso/logs/arbitrary.log"}},
            {**base,
             "sponsor": {**base["sponsor"], "pid": 123},
             "processes_after_lifecycle": [
                 base["processes_after_lifecycle"][0], {
                     "pid": 123,
                     "command": " ".join(canonical_command)}],
             "cleanup": {**base["cleanup"], "target_pid": 123}},
            {**base,
             "sponsor": {**base["sponsor"], "command": [
                 "/engine/zenserver", "--quiet", "x", "y", "--port"]}},
            {**base,
             "owner": {"kind": "foreign-process", "pid": 123}},
            {**base,
             "version": {**base["version"], "sha256": "0" * 64}},
            {**base,
             "version": {
                 "path": str(foreign_zen),
                 "size": foreign_zen.stat().st_size,
                 "sha256": bs._sha256_file(foreign_zen)},
             "sponsor": {**base["sponsor"], "command": foreign_command},
             "processes_after_lifecycle": [
                 base["processes_after_lifecycle"][0], {
                     "pid": 321, "command": " ".join(foreign_command)}]},
            {**base,
             "sponsor": {**base["sponsor"], "command": [
                 *canonical_command, "--port=8558"]}},
            {**base,
             "processes_after_lifecycle": [
                 base["processes_after_lifecycle"][0], {
                 "pid": 321,
                 "command": " ".join([
                     *canonical_command[:6], "C:/foreign-data",
                     *canonical_command[7:]])}]},
            {**base,
             "endpoint_preflight": {
                 **base["endpoint_preflight"],
                 "exclusive_bind_succeeded": False}},
            {**base,
             "monitor": {**base["monitor"], "sample_count": 0}},
            {**base,
             "owner": {"kind": "pipeline-process", "pid": True},
             "sponsor": {**base["sponsor"], "command":
                 bs._canonical_zen_sponsor_command(
                     str(pinned_zen),
                     iso / "Saved" / "GuardedZenSponsor-True", True)},
             "processes_after_lifecycle": [
                 base["processes_after_lifecycle"][0], {
                 "pid": 321,
                 "command": " ".join(bs._canonical_zen_sponsor_command(
                     str(pinned_zen),
                     iso / "Saved" / "GuardedZenSponsor-True", True))}]},
            {**base,
             "sponsor": {**base["sponsor"], "pid": True},
             "processes_after_lifecycle": [
                 base["processes_after_lifecycle"][0], {
                 "pid": 1, "command": " ".join(canonical_command)}],
             "cleanup": {**base["cleanup"], "target_pid": True}},
            {**base,
             "before": [{"pid": 55,
                         "command": "/foreign/zenserver --port=18558"}],
             "processes_after_lifecycle": [
                 {"pid": 55,
                  "command": "/foreign/zenserver --port=18558"},
                 base["processes_after_lifecycle"][1]],
             "after": [{"pid": 55,
                        "command": "/foreign/zenserver --port=18558"}]},
            {**base,
             "unreal_commands": base["unreal_commands"][:-1]},
            {**base,
             "unreal_commands": [
                 base["unreal_commands"][1],
                 base["unreal_commands"][0],
                 *base["unreal_commands"][2:]]},
            {**base,
             "unreal_commands": [
                 {**base["unreal_commands"][0], "command": [
                     *base["unreal_commands"][0]["command"],
                     "-ddc=Shared"]},
                 *base["unreal_commands"][1:]]},
            {**base,
             "unreal_commands": [
                 *base["unreal_commands"][:3],
                 {**base["unreal_commands"][3],
                  "log_sha256": "0" * 64}]},
            {**base,
             "unreal_commands": [
                 {**base["unreal_commands"][0], "command": [
                     "C:/foreign/FakeEditor.exe",
                     *base["unreal_commands"][0]["command"][1:]]},
                 *base["unreal_commands"][1:]]},
            {**base,
             "unreal_commands": [
                 {**base["unreal_commands"][0], "command": [
                     base["unreal_commands"][0]["command"][0],
                     "C:/foreign/Other.uproject",
                     *base["unreal_commands"][0]["command"][2:]]},
                 *base["unreal_commands"][1:]]},
            {**base,
             "unreal_commands": [
                 base["unreal_commands"][0],
                 {**base["unreal_commands"][1], "command": [
                     value.replace(
                         "finalize_stripped_fractal_catalog.py",
                         "foreign.py")
                     for value in base["unreal_commands"][1]["command"]]},
                 *base["unreal_commands"][2:]]},
            {**base,
             "unreal_commands": [{
                 **base["unreal_commands"][0], "command": [
                     value.replace(
                         str(iso / "Tools" / "import" /
                             "import_all.py").replace("\\", "/"),
                         "C:/foreign/evil.py")
                     for value in base["unreal_commands"][0]["command"]
                 ] + ["-note=" + str(
                     iso / "Tools" / "import" /
                     "import_all.py").replace("\\", "/")]},
                 *base["unreal_commands"][1:]]},
            {**base,
             "unreal_commands": [{
                 **base["unreal_commands"][0], "command": [
                     value for value in
                     base["unreal_commands"][0]["command"]
                     if value != "-run=pythonscript"]},
                 *base["unreal_commands"][1:]]},
            {**base,
             "unreal_commands": [
                 *base["unreal_commands"][:3], {
                     **base["unreal_commands"][3], "command": [
                         *base["unreal_commands"][3]["command"],
                         "-project=C:/foreign/Other.uproject"]}]},
        )
        for receipt in hostile:
            with self.subTest(receipt=receipt):
                bs._write_json(path, receipt)
                stamp = bound_stamp()
                with self.assertRaisesRegex(bs.BuildFailure, "Zen lifetime"):
                    bs._guarded_zen_binding(stamp, iso, "Mac")

        win_bin = engine / "Engine" / "Binaries" / "Win64"
        win_bin.mkdir(parents=True)
        win_zen = win_bin / "zenserver.exe"
        win_editor = win_bin / "UnrealEditor-Cmd.exe"
        win_zen.write_bytes(b"pinned win zen")
        win_editor.write_bytes(b"win editor")
        win_runuat = engine / "Engine" / "Build" / "BatchFiles" / \
            "RunUAT.bat"
        win_runuat.write_bytes(b"win runuat")
        win_sponsor = bs._canonical_zen_sponsor_command(
            str(win_zen), owned_data_dir, 123)
        win_base = copy.deepcopy(base)
        win_base["version"] = {
            "path": str(win_zen), "size": win_zen.stat().st_size,
            "sha256": bs._sha256_file(win_zen),
        }
        win_base["sponsor"]["command"] = win_sponsor
        win_base["processes_after_lifecycle"][1] = {
            "pid": 321, "command": " ".join(win_sponsor)}
        for command_row in win_base["unreal_commands"][:3]:
            command_row["command"][0] = str(win_editor)
        win_cook = win_base["unreal_commands"][3]["command"]
        win_cook[0] = str(win_runuat)
        win_cook[win_cook.index("-platform=Mac")] = "-platform=Win64"
        win_cook.extend([
            "-NoXGE",
            "-UbtArgs=" + " ".join(bs.WIN64_SERIAL_LOCAL_UBT_ARGUMENTS),
        ])

        def win_stamp():
            return {
                "platform": "Win64", "engine": str(engine),
                "log": str(iso / "logs" / "cook.log"),
                "maps": list(bs._cook_maps(guarded=True)),
                "zen_lifetime": {
                    "path": str(path), "sha256": bs._sha256_file(path)},
            }

        with mock.patch.dict(
                bs.PINNED_SOURCE_ENGINES, {"Win64": engine}):
            bs._write_json(path, win_base)
            self.assertEqual(
                bs._guarded_zen_binding(
                    win_stamp(), iso, "Win64")["receipt"]["mechanism"],
                "pipeline-sponsored-zen-v3")
            exact_ubt = win_cook[-1]
            win_hostile = (
                [token for token in win_cook if token != exact_ubt],
                [token.replace("-MaxParallelActions=1",
                               "-MaxParallelActions=2")
                 if token == exact_ubt else token for token in win_cook],
                [*win_cook, exact_ubt],
                [*win_cook, "-MaxParallelActions=8"],
                [*win_cook, "-XGE"],
            )
            for hostile_cook in win_hostile:
                with self.subTest(hostile_cook=hostile_cook):
                    receipt = copy.deepcopy(win_base)
                    receipt["unreal_commands"][3]["command"] = hostile_cook
                    bs._write_json(path, receipt)
                    with self.assertRaisesRegex(
                            bs.BuildFailure, "Zen lifetime"):
                        bs._guarded_zen_binding(
                            win_stamp(), iso, "Win64")

        forbidden = copy.deepcopy(base)
        forbidden_log = Path(forbidden["unreal_commands"][0]["log"])
        forbidden_log.write_text(
            "LogDerivedDataCache: /fixture/preexisting-ddc: Maintenance finished\n",
            encoding="utf-8")
        forbidden["unreal_commands"][0]["log_sha256"] = \
            bs._sha256_file(forbidden_log)
        bs._write_json(path, forbidden)
        with self.assertRaisesRegex(bs.BuildFailure, "Zen lifetime"):
            bs._guarded_zen_binding(bound_stamp(), iso, "Mac")

    def test_zen_process_scan_handles_executable_paths_with_spaces(self):
        ps = mock.Mock(
            returncode=0,
            stdout=(
                "55 /fixture-root/Library/Application Support/Epic/"
                "UnrealEngine/Common/Zen/Install/zenserver --port 8558\n"
                "56 /usr/bin/helper --attachment=/tmp/zenserver.log\n"),
            stderr="")
        with mock.patch.object(bs.subprocess, "run", return_value=ps):
            self.assertEqual(bs._zen_processes(), [{
                "pid": 55,
                "command": (
                    "/fixture-root/Library/Application Support/Epic/"
                    "UnrealEngine/Common/Zen/Install/zenserver --port 8558"),
            }])

    def test_guarded_mac_package_normalizes_config_suffixed_app(self):
        iso = self.root / "iso"
        source_app = iso / "staged" / "Mac" / "WoT-Mac-Test.app"
        (source_app / "Contents" / "MacOS").mkdir(parents=True)
        (source_app / "Contents" / "MacOS" / "WoT-Mac-Test").write_bytes(b"game")
        updater = iso / "Binaries" / "Mac" / "WoTUpdater"
        updater.parent.mkdir(parents=True)
        updater.write_bytes(b"updater")
        entitlements = (iso / "Build" / "Mac" / "Resources" /
                        "Sandbox.Server.entitlements")
        entitlements.parent.mkdir(parents=True)
        entitlements.write_text("<plist/>")
        with mock.patch.object(bs.host_platform, "system", return_value="Darwin"), \
                mock.patch.object(
                    bs.mac_signing, "sign_app",
                    return_value={"signing": "adhoc", "team_id": None}) as sign, \
                mock.patch.object(bs, "local_value", return_value=None):
            staged = bs._prepare_guarded_stage(
                self.args(configuration="Test"), iso)
        normalized = staged / "WoT.app"
        self.assertTrue(normalized.is_dir())
        self.assertFalse(source_app.exists())
        self.assertFalse(
            (normalized / "Contents" / "Helpers" / "WoTUpdater").exists())
        sign.assert_called_once_with(
            normalized, entitlements, None, bs._mac_signing_runner)


    # ------------------------------------------------------------------
    # Guarded Shipping release: shipping-stage/-audit/-package
    # ------------------------------------------------------------------

    def shipping_args(self, **overrides):
        values = {"configuration": "Shipping", "cache": self.root / "cache"}
        values.update(overrides)
        return self.args(**values)

    def test_shipping_mode_resolves_only_when_guarded(self):
        args = self.args(configuration="Shipping")
        bs._resolve_build_mode(args)
        self.assertEqual(args.configuration, "Shipping")
        self.assertTrue(bs._is_shipping_release(args))
        self.assertFalse(bs._is_shipping_release(self.args()))
        with self.assertRaisesRegex(bs.BuildFailure, "frozen to Development"):
            bs._resolve_build_mode(SimpleNamespace(
                guarded=False, configuration="Shipping", platform="Mac",
                engine=None))

    def test_shipping_steps_are_refused_without_shipping_configuration(self):
        import contextlib
        import io
        iso = self.repo / ".stripped-pak"
        for configuration in ("Test", None):
            stderr = io.StringIO()
            arguments = ["--guarded", "--platform", "Mac",
                         "--cache", str(self.root / "cache"),
                         "--iso", str(iso), "--only-step", "shipping-stage"]
            if configuration:
                arguments += ["--configuration", configuration]
            with mock.patch.object(bs, "_ensure_repo_editor_module"), \
                    contextlib.redirect_stderr(stderr):
                code = bs.main(arguments)
            with self.subTest(configuration=configuration):
                self.assertEqual(code, 1)
                self.assertIn("require --guarded --configuration Shipping",
                              stderr.getvalue())

    def test_guarded_cook_is_always_the_test_client_even_for_shipping(self):
        engine = self.root / "engine"
        for name in ("RunUAT.sh", "RunUAT.bat"):
            path = engine / "Engine" / "Build" / "BatchFiles" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"runuat")
        iso = self.root / "iso"
        command = bs._cook_command(self.shipping_args(), iso, engine)
        self.assertIn("-clientconfig=Test", command)
        self.assertNotIn("-clientconfig=Shipping", command)

    def test_shipping_stage_command_stages_the_test_cook_without_building(self):
        engine = self.root / "engine"
        for name in ("RunUAT.sh", "RunUAT.bat"):
            path = engine / "Engine" / "Build" / "BatchFiles" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"runuat")
        iso = self.root / "iso"
        for platform_name in ("Mac", "Win64"):
            args = self.shipping_args(platform=platform_name)
            command = bs._shipping_stage_command(args, iso, engine)
            with self.subTest(platform=platform_name):
                self.assertEqual(command[1], "BuildCookRun")
                for token in ("-skipbuild", "-skipcook", "-stage", "-pak",
                              "-NoDebugInfo", "-clientconfig=Shipping",
                              f"-stagingdirectory={iso / 'staged-shipping'}"):
                    self.assertIn(token, command)
                for token in ("-build", "-cook"):
                    self.assertNotIn(token, command)
                self.assertFalse(any(token.startswith(
                    ("-map=", "-AdditionalCookerOptions", "-ddc="))
                    for token in command))
                self.assertEqual(
                    "-NoBootstrapExe" in command, platform_name == "Win64")
                bs._validate_shipping_stage_command(command, iso, platform_name)
        base = bs._shipping_stage_command(
            self.shipping_args(platform="Win64"), iso, engine)
        variants = {
            "cooks": base + ["-cook"],
            "builds": base + ["-build"],
            "map": base + ["-map=/Game/WoT/Maps/WoTBootstrap"],
            "test client": [token.replace("-clientconfig=Shipping",
                                          "-clientconfig=Test")
                            for token in base],
            "two clients": base + ["-clientconfig=Test"],
            "shared stage": [token.replace("staged-shipping", "staged")
                             for token in base],
            "bootstrap exe": [token for token in base
                              if token != "-NoBootstrapExe"],
            "debug info": [token for token in base
                           if token != "-NoDebugInfo"],
            "cooker options": base + ["-AdditionalCookerOptions=-ddc=x"],
        }
        for name, command in variants.items():
            with self.subTest(variant=name), self.assertRaisesRegex(
                    bs.BuildFailure, "shipping stage must be"):
                bs._validate_shipping_stage_command(command, iso, "Win64")

    def test_shipping_target_builds_use_their_own_configuration_and_receipts(self):
        iso = self.root / "iso"
        engine = self.root / "engine"
        test_receipt, _ = bs._guarded_receipt_paths(iso, "Win64", "WoT")
        shipping_receipt, shipping_compiled = bs._guarded_receipt_paths(
            iso, "Win64", "WoT", "Shipping")
        self.assertNotEqual(test_receipt, shipping_receipt)
        self.assertEqual(shipping_receipt.name, "Win64-WoT-Shipping-engine.json")
        self.assertEqual(shipping_compiled.name,
                         "Win64-WoT-Shipping-engine-compiled.json")
        command = bs._guarded_build_command(
            self.shipping_args(platform="Win64"), iso, engine, "WoT",
            shipping_receipt, "Shipping")
        self.assertEqual(
            command[command.index("-Configuration") + 1], "Shipping")
        self.assertIn("-Clean", command)
        self.assertIn("-SerialLocalBuild", command)
        self.assertEqual(
            bs._guarded_main_artifact_path(iso, "Win64", "Shipping").name,
            "WoT-Win64-Shipping.exe")

    def test_release_asset_names_match_release_formats(self):
        import release_formats
        for platform_name in ("Mac", "Win64"):
            for configuration in ("Test", "Shipping"):
                self.assertEqual(
                    bs._release_asset_name(
                        platform_name, "abc123456", True, configuration),
                    release_formats.guarded_install_zip_asset(
                        platform_name, "abc123456", configuration))
        self.assertEqual(
            bs._release_asset_name("Win64", "abc123456", True, "Shipping"),
            "WoT-Win64-guarded-shipping-abc123456.zip")
        self.assertEqual(bs._release_asset_name("Win64", "abc123456", True),
                         "WoT-Win64-guarded-abc123456.zip")

    def test_loose_cooked_output_refuses_missing_metadata_or_zen_store(self):
        iso = self.root / "iso"
        with self.assertRaisesRegex(bs.BuildFailure, "loose output"):
            bs._loose_cooked_output_identity(iso, "Win64")
        # MetadataDir is Saved/Cooked/<CookPlatform>/<ShortProjectName>/Metadata
        # (CopyBuildToStagingDirectory.Automation.cs:1370-1372).
        metadata = iso / "Saved" / "Cooked" / "Windows" / "WoT" / "Metadata"
        metadata.mkdir(parents=True)
        (metadata.parents[1] / "Metadata").mkdir()
        (metadata.parents[1] / "Metadata" / "packagestore.manifest").write_bytes(
            b"wrong directory")
        with self.assertRaisesRegex(bs.BuildFailure, "packagestore.manifest"):
            bs._loose_cooked_output_identity(iso, "Win64")
        (metadata / "packagestore.manifest").write_bytes(b"store")
        with self.assertRaisesRegex(bs.BuildFailure, "scriptobjects.bin"):
            bs._loose_cooked_output_identity(iso, "Win64")
        (metadata / "scriptobjects.bin").write_bytes(b"script")
        identity = bs._loose_cooked_output_identity(iso, "Win64")
        self.assertEqual(set(identity["metadata"]),
                         {"packagestore.manifest", "scriptobjects.bin"})
        (metadata.parents[1] / "ue.projectstore").write_text("{}")
        with self.assertRaisesRegex(bs.BuildFailure, "Zen project store"):
            bs._loose_cooked_output_identity(iso, "Win64")

    def test_cooked_twin_comparison_requires_the_same_package_census(self):
        paths = self.guarded_cooked_paths()
        test = {"paths": paths + ["../../../Engine/Config/Base.ini"],
                "containers": [
                    {"path": "Mac/Paks/a.utoc", "sha256": "1" * 64, "size": 1},
                    {"path": "Mac/Paks/global.utoc", "sha256": "2" * 64,
                     "size": 2}]}
        shipping = copy.deepcopy(test)
        shipping["containers"][0]["sha256"] = "3" * 64
        comparison = bs._cooked_twin_comparison(test, shipping)
        self.assertEqual(comparison["container_bytes"], {
            "identical": ["Mac/Paks/global.utoc"],
            "differs": ["Mac/Paks/a.utoc"]})
        self.assertEqual(comparison["project_package_count"], len(paths) - 1)
        self.assertTrue(comparison["all_listed_paths_identical"])
        missing = copy.deepcopy(shipping)
        missing["paths"] = missing["paths"][:1] + missing["paths"][2:]
        with self.assertRaisesRegex(bs.BuildFailure, "package census"):
            bs._cooked_twin_comparison(test, missing)
        extra = copy.deepcopy(shipping)
        extra["containers"].append(
            {"path": "Mac/Paks/b.utoc", "sha256": "4" * 64, "size": 4})
        with self.assertRaisesRegex(bs.BuildFailure, "container set"):
            bs._cooked_twin_comparison(test, extra)

    def test_test_cook_cache_does_not_require_binaries_updater(self):
        # The shipping-stage wrapper's -Clean removes the Test build outputs
        # from Binaries/; the receipted staged Test products stay the proof.
        evidence = self.guarded_evidence()
        iso = evidence["iso"]
        stamp = dict(evidence["cook"])
        stamp.update({
            "command_exit_code": 0,
            "clean_cook_strategy": "project-cooked-platform+staging-v1",
        })
        bs._write_stamp(iso, "cook", stamp)
        self.assertEqual(bs.stage_cook(self.args(), iso), "cached")

    # -- end-to-end over synthetic UAT/UBT -------------------------------

    def provision_shipping_iso(self) -> dict:
        evidence = self.guarded_evidence()
        iso = evidence["iso"]
        neutral = evidence["cook"]["neutral_support_authority"]
        bs._write_stamp(iso, "seed", {
            "release_commit": "abc123456",
            "release_sequence": 0,
            "source_snapshot": evidence["source"],
            "neutral_support_authority": neutral,
        })
        bs._write_stamp(iso, "import", {"neutral_support_authority": neutral})
        bs._write_stamp(iso, "cook", evidence["cook"])
        bs._write_stamp(iso, "audit", evidence["audit"])
        metadata = iso / "Saved" / "Cooked" / "Mac" / "WoT" / "Metadata"
        metadata.mkdir(parents=True)
        (metadata / "packagestore.manifest").write_bytes(b"store")
        (metadata / "scriptobjects.bin").write_bytes(b"script")
        entitlements = (iso / "Build" / "Mac" / "Resources"
                        / "Sandbox.Server.entitlements")
        entitlements.parent.mkdir(parents=True)
        entitlements.write_text("<plist/>")
        engine = self.root / "engine"
        for name in ("RunUAT.sh", "RunUAT.bat"):
            (engine / "Engine" / "Build" / "BatchFiles" / name).write_bytes(
                b"runuat")
        evidence["engine"] = engine
        test_compiled = json.loads(Path(
            evidence["cook"]["target_receipts"]["WoT"]["compiled"]
        ).read_text(encoding="utf-8"))
        evidence["engine_identity"] = test_compiled["engine"]
        return evidence

    def fake_shipping_build(self, evidence, *, game_definitions=None):
        def build(args, iso, engine_root, target, configuration="Test"):
            self.assertEqual(configuration, "Shipping")
            engine_path, compiled_path = bs._guarded_receipt_paths(
                iso, args.platform, target, configuration)
            engine_path.parent.mkdir(parents=True, exist_ok=True)
            bs._write_json(engine_path, {
                "format": "wot-ue58-source-receipt-v1",
                "platform": args.platform, **evidence["engine_identity"]})
            self.assertEqual(target, "WoT")
            definitions = dict(
                game_definitions if game_definitions is not None
                else bs.SHIPPING_GAME_DEFINITIONS)
            checks = ["configuration-is-Shipping",
                      "guard-definitions-present",
                      "test-only-definitions-absent"]
            bs._write_json(compiled_path, {
                "format": "wot-guarded-build-receipt-v1",
                "target": target, "platform": args.platform,
                "configuration": configuration, "definitions": definitions,
                "checks": checks, "engine": evidence["engine_identity"]})
            return {
                "engine": str(engine_path), "compiled": str(compiled_path),
                "engine_sha256": bs._sha256_file(engine_path),
                "compiled_sha256": bs._sha256_file(compiled_path),
            }
        return build

    def fake_shipping_uat(self, *, log_text="BUILD SUCCESSFUL\n",
                          recook=False):
        commands = []

        def run(step, command, log_path, *, env=None, check=True):
            self.assertEqual(step, "shipping-stage")
            commands.append((list(command), env))
            iso = Path(str(command[2]).split("=", 1)[1]).parent
            app = (iso / "staged-shipping" / "Mac" / "WoT-Mac-Shipping.app")
            game = app / "Contents" / "MacOS" / "WoT-Mac-Shipping"
            game.parent.mkdir(parents=True, exist_ok=True)
            game.write_bytes(b"shipping-game")
            with (app / "Contents" / "Info.plist").open("wb") as handle:
                plistlib.dump({"CFBundleExecutable": game.name}, handle)
            # UAT stages WoTRetailReader's NonUFS runtime dependency into
            # every client configuration.
            self.seed_production_descriptor(iso, app)
            # Same relative container layout as the Test fixture's stage.
            container = iso / "staged-shipping" / "Paks" / "test.utoc"
            container.parent.mkdir(parents=True, exist_ok=True)
            container.write_bytes(b"synthetic-container")
            if recook:
                (iso / "Saved" / "Cooked" / "Mac" / "WoT" / "Metadata"
                 / "packagestore.manifest").write_bytes(b"re-cooked")
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text(log_text, encoding="utf-8")
            return 0
        return run, commands

    def fake_shipping_inventory(self):
        def write(iso, engine_root, platform_name, configuration="Test"):
            self.assertEqual(configuration, "Shipping")
            staged = iso / "staged-shipping"
            containers = sorted(
                path for path in staged.rglob("*")
                if path.suffix in {".utoc", ".pak"})
            paths = self.guarded_cooked_paths()
            test_receipt = json.loads(
                (iso / "receipts" / "cooked-container-inventory.json")
                .read_text(encoding="utf-8"))
            receipt_path, log_path = bs._cooked_inventory_paths(
                iso, configuration)
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text("listing\n", encoding="utf-8")
            receipt = {
                "format": bs.COOKED_CONTAINER_RECEIPT_FORMAT,
                "platform": platform_name,
                "configuration": configuration,
                "tool": test_receipt["tool"],
                "containers": [{
                    "path": path.relative_to(staged).as_posix(),
                    "size": path.stat().st_size,
                    "sha256": bs._sha256_file(path),
                    "entry_count": len(paths),
                } for path in containers],
                "paths": test_receipt["paths"],
                "path_count": len(test_receipt["paths"]),
                "validation": test_receipt["validation"],
                "log": str(log_path),
                "log_sha256": bs._sha256_file(log_path),
            }
            bs._write_json(receipt_path, receipt)
            return {
                "path": str(receipt_path),
                "sha256": bs._sha256_file(receipt_path),
                "path_count": receipt["path_count"],
                "validation": receipt["validation"],
            }
        return write

    def run_shipping_stage(self, evidence, *, uat=None, build=None,
                           inventory=None):
        uat = uat or self.fake_shipping_uat()[0]
        with mock.patch.object(bs, "_git_commit", return_value="abc123456"), \
                mock.patch.object(bs, "_source_snapshot",
                                  return_value=evidence["source"]), \
                mock.patch.object(
                    bs, "_engine_root_and_editor",
                    return_value=(evidence["engine"],
                                  evidence["engine"] / "editor")), \
                mock.patch.object(bs, "_run_guarded_target_build",
                                  side_effect=build
                                  or self.fake_shipping_build(evidence)), \
                mock.patch.object(bs, "_run_logged", side_effect=uat), \
                mock.patch.object(bs.host_platform, "system", return_value="Darwin"), \
                mock.patch.object(
                    bs.mac_signing, "sign_app",
                    return_value={"signing": "adhoc", "team_id": None}), \
                mock.patch.object(bs, "local_value", return_value=None), \
                mock.patch.object(
                    bs, "_write_guarded_cooked_inventory",
                    side_effect=inventory or self.fake_shipping_inventory()):
            return bs.stage_shipping(self.shipping_args(), evidence["iso"])

    def test_shipping_stage_builds_stages_and_receipts_from_the_test_cook(self):
        evidence = self.provision_shipping_iso()
        iso = evidence["iso"]
        uat, commands = self.fake_shipping_uat()
        self.assertEqual(self.run_shipping_stage(evidence, uat=uat), "ran")
        self.assertEqual(len(commands), 1)
        command, environment = commands[0]
        bs._validate_shipping_stage_command(command, iso, "Mac")
        self.assertIsInstance(environment, dict)
        stamp = bs._read_stamp(iso, "shipping-stage")
        self.assertEqual(stamp["configuration"], "Shipping")
        self.assertEqual(stamp["maps"], evidence["cook"]["maps"])
        bindings = bs._guarded_receipt_bindings(stamp, "Mac", "Shipping")
        self.assertEqual(
            bindings["WoT"]["compiled_receipt"]["definitions"],
            bs.SHIPPING_GAME_DEFINITIONS)
        self.assertNotIn(
            "WOT_BOOTSTRAP_TEST_IDENTITIES",
            bindings["WoT"]["compiled_receipt"]["definitions"])
        products = bs._guarded_product_binding(stamp, iso, "Mac", "Shipping")
        self.assertEqual(
            products["receipt"]["products"]["WoT"]["path"],
            "Mac/WoT.app/Contents/MacOS/WoT-Mac-Shipping")
        self.assertEqual(
            {key: products["receipt"][key] for key in (
                "signing", "team_id", "notarization_submission_id", "stapled")},
            {"signing": "adhoc", "team_id": None,
             "notarization_submission_id": None, "stapled": False})
        stage = bs._shipping_stage_binding(stamp, iso, "Mac")["receipt"]
        self.assertEqual(stage["cooked_output"]["before"],
                         stage["cooked_output"]["after"])
        self.assertEqual(
            stage["test_twin_comparison"]["project_package_count"],
            len(self.guarded_cooked_paths()) - 1)
        # The audited Test tree is untouched by the Shipping stage.
        bs._validate_staged_tree_receipt(iso, evidence["audit"])
        self.assertTrue(
            (iso / "staged-shipping" / "Mac" / "WoT.app").is_dir())
        # A second run is a cache hit and runs nothing.
        again = self.fake_shipping_uat()
        self.assertEqual(
            self.run_shipping_stage(evidence, uat=again[0]), "cached")
        self.assertEqual(again[1], [])
        # Re-packaging Test does not throw the Shipping stage away.
        bs._invalidate_from(iso, "package")
        self.assertIsNotNone(bs._read_stamp(iso, "shipping-stage"))

    def test_shipping_stage_refuses_a_recook_zen_contact_or_cook_commandlet(self):
        cases = {
            "changed during the Shipping stage": {"recook": True},
            "preexisting DDC/Zen": {"log_text": "DDC 127.0.0.1:8558\n"},
            "ran a cook commandlet": {
                "log_text": "Running: UnrealEditor-Cmd.exe -run=Cook\n"},
        }
        for message, options in cases.items():
            with self.subTest(message=message):
                self.setUp()
                evidence = self.provision_shipping_iso()
                uat, _commands = self.fake_shipping_uat(**options)
                with self.assertRaisesRegex(bs.BuildFailure, message):
                    self.run_shipping_stage(evidence, uat=uat)
                self.assertIsNone(
                    bs._read_stamp(evidence["iso"], "shipping-stage"))

    def test_shipping_stage_refuses_a_test_identity_in_the_shipping_game(self):
        evidence = self.provision_shipping_iso()
        definitions = dict(bs.SHIPPING_GAME_DEFINITIONS)
        definitions["WOT_BOOTSTRAP_COMPILED_COMPONENT_IDENTITY"] = (
            '"wot-game-test-v1"')
        with self.assertRaisesRegex(bs.BuildFailure, "production Shipping"):
            self.run_shipping_stage(
                evidence, build=self.fake_shipping_build(
                    evidence, game_definitions=definitions))
        self.assertIsNone(bs._read_stamp(evidence["iso"], "shipping-stage"))

    def test_shipping_stage_requires_the_seed_to_be_head(self):
        evidence = self.provision_shipping_iso()
        with mock.patch.object(bs, "_git_commit", return_value="fffffffff"):
            with self.assertRaisesRegex(bs.BuildFailure, "differs from cooked"):
                bs.stage_shipping(self.shipping_args(), evidence["iso"])

    def test_shipping_audit_and_package_bind_the_test_twin(self):
        import release_formats
        evidence = self.provision_shipping_iso()
        iso = evidence["iso"]
        self.assertEqual(self.run_shipping_stage(evidence), "ran")
        audited = []

        def fake_audit(staged, cache, report_path):
            audited.append(Path(staged))
            report = {"verdict": "pass", "cache": str(cache),
                      "cache_sha256": "c" * 64, "container_count": 1,
                      "total_matches": 0}
            Path(report_path).write_text(json.dumps(report))
            return report

        def fake_ditto(command, **_kwargs):
            if command[0] == "cp":
                shutil.copytree(command[-2], command[-1], symlinks=True)
                return SimpleNamespace(returncode=0, stdout="", stderr="")
            self.assertEqual(command[0], "ditto")
            write_fake_mac_zip(
                command[-1],
                b"synthetic-archive " + command[-2].encode("utf-8"),
                {"WoT.app/Contents/UE/WoT/Build/WoTRelease/"
                 "release-version.json":
                 release_formats.release_version_bytes("")})
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        with mock.patch.object(bs.absence_audit, "audit",
                               side_effect=fake_audit), \
                mock.patch.object(bs, "_git_commit", return_value="abc123456"), \
                mock.patch.object(bs, "_source_snapshot",
                                  return_value=evidence["source"]), \
                mock.patch.object(
                    bs, "_require_release_notes_absent",
                    wraps=bs._require_release_notes_absent) as require_notes, \
                mock.patch.object(
                    bs, "_require_staged_release_identity",
                    wraps=bs._require_staged_release_identity) as require_identity, \
                mock.patch.object(
                    bs, "_require_staged_release_version",
                    wraps=bs._require_staged_release_version) as require_version, \
                mock.patch.object(bs.subprocess, "run", side_effect=fake_ditto):
            self.assertEqual(
                bs.stage_shipping_audit(self.shipping_args(), iso), "ran")
            self.assertEqual(audited, [iso / "staged-shipping"])
            self.assertTrue(
                (iso / "receipts" / "staged-tree-audit-shipping.json").is_file())
            self.assertEqual(bs.stage_package(self.args(), iso), "ran")
            self.assertEqual(
                bs.stage_shipping_package(self.shipping_args(), iso), "ran")
            self.assertEqual(require_notes.call_args_list, [
                mock.call(iso / "staged" / "Mac"),
                mock.call(iso / "staged-shipping" / "Mac"),
            ])
            self.assertEqual(require_identity.call_args_list, [
                mock.call(iso / "staged" / "Mac", iso),
                mock.call(iso / "staged-shipping" / "Mac", iso),
            ])
            self.assertEqual(require_version.call_args_list, [
                mock.call(iso / "staged" / "Mac", iso),
                mock.call(iso / "staged-shipping" / "Mac", iso),
            ])
            self.assertEqual(
                bs.stage_shipping_package(self.shipping_args(), iso), "cached")
            test_json = iso / "release" / "release.json"
            shipping_json = iso / "release-shipping" / "release.json"
            manifest = json.loads(shipping_json.read_text(encoding="utf-8"))
            self.assertEqual(manifest["configuration"], "Shipping")
            self.assertEqual(manifest["release_sequence"], 0)
            self.assertEqual(manifest["asset"],
                             "WoT-Mac-guarded-shipping-abc123456.zip")
            twin = manifest["test_twin"]
            self.assertEqual(set(twin), set(release_formats.TEST_TWIN_FIELDS))
            self.assertEqual(twin["release_json_sha256"],
                             bs._sha256_file(test_json))
            self.assertEqual(twin["release_sequence"], 0)
            self.assertEqual(twin["human_version"], "")
            self.assertEqual(twin["asset"], "WoT-Mac-guarded-abc123456.zip")
            self.assertEqual(
                manifest["zen_lifetime"]["receipt_sha256"],
                twin["zen_lifetime_receipt_sha256"])
            self.assertEqual(
                manifest["shipping_stage"]["receipt"]["configuration"],
                "Shipping")
            self.assertEqual(
                manifest["target_receipts"]["WoT"]["compiled_receipt"]
                ["configuration"], "Shipping")
            # The release tooling accepts exactly this shape.
            loaded = release_formats.load_guarded_release(shipping_json, "Mac")
            self.assertEqual(loaded.configuration, "Shipping")
            self.assertEqual(loaded.main_relative,
                             "Contents/MacOS/WoT-Mac-Shipping")
            self.assertEqual(loaded.test_twin, twin)
            self.assertEqual(
                release_formats.load_guarded_release(test_json, "Mac")
                .configuration, "Test")
            # A new Test package (a different funnel twin) re-binds the
            # Shipping package without rebuilding the Shipping stage.
            test_json.write_text("{}")
            self.assertEqual(bs.stage_package(self.args(), iso), "ran")
            self.assertIsNotNone(bs._read_stamp(iso, "shipping-stage"))
            self.assertIsNone(bs._read_stamp(iso, "shipping-package"))
            self.assertEqual(
                bs.stage_shipping_package(self.shipping_args(), iso), "ran")
            rebound = json.loads(shipping_json.read_text(encoding="utf-8"))
            self.assertEqual(rebound["test_twin"]["release_json_sha256"],
                             bs._sha256_file(test_json))

    def test_test_twin_binding_reads_each_platform_game_tree_version(self):
        import release_formats
        import zipfile

        commit = "abc123456"
        source_snapshot = {"sha256": "s" * 64}
        for platform_name in release_formats.PLATFORMS:
            with self.subTest(platform=platform_name):
                iso = self.root / f"test-twin-version-{platform_name}"
                release_dir = iso / "release"
                release_dir.mkdir(parents=True)
                asset_name = release_formats.guarded_install_zip_asset(
                    platform_name, commit, release_formats.TEST_CONFIGURATION)
                asset = release_dir / asset_name
                member = (
                    release_formats.GUARDED_TREE_ROOTS[platform_name] + "/"
                    + release_formats.release_version_relative(platform_name))
                with zipfile.ZipFile(asset, "w") as archive:
                    archive.writestr(
                        member,
                        release_formats.release_version_bytes("0.1.2"))
                manifest = {
                    "format": "wot-guarded-release-v1",
                    "configuration": release_formats.TEST_CONFIGURATION,
                    "platform": platform_name,
                    "git_commit": commit,
                    "release_sequence": 0,
                    "source_snapshot": source_snapshot,
                    "asset": asset_name,
                    "sha256": bs._sha256_file(asset),
                    "size": asset.stat().st_size,
                    "zen_lifetime": {"receipt_sha256": "z" * 64},
                    "cooked_container_inventory": {
                        "receipt_sha256": "c" * 64},
                    "audit": {"staged_tree_sha256": "t" * 64},
                }
                release_json = release_dir / "release.json"
                bs._write_json(release_json, manifest)
                bs._write_stamp(iso, "package", {
                    "mode": "guarded",
                    "manifest": str(release_json),
                    "release_json_sha256": bs._sha256_file(release_json),
                    "sha256": manifest["sha256"],
                })
                inventory = {
                    "receipt_sha256": "c" * 64,
                    "receipt": {},
                }
                comparison = {
                    "project_package_census_sha256": "p" * 64,
                    "container_bytes": {"identical": [], "differs": []},
                }
                with mock.patch.object(
                        bs, "_guarded_cooked_inventory_binding",
                        return_value=inventory), \
                        mock.patch.object(
                            bs, "_guarded_zen_binding", return_value={
                                "receipt_sha256": "z" * 64}), \
                        mock.patch.object(
                            bs, "_validate_staged_tree_receipt", return_value={
                                "tree_sha256": "t" * 64}), \
                        mock.patch.object(
                            bs, "_cooked_twin_comparison",
                            return_value=comparison):
                    twin = bs._test_twin_binding(
                        iso, platform_name, commit, source_snapshot,
                        {}, {}, 0)
                self.assertEqual(twin["human_version"], "0.1.2")

    def test_shipping_package_refuses_a_tampered_test_twin(self):
        evidence = self.provision_shipping_iso()
        iso = evidence["iso"]
        self.assertEqual(self.run_shipping_stage(evidence), "ran")

        def fake_ditto(command, **_kwargs):
            if command[0] == "cp":
                shutil.copytree(command[-2], command[-1], symlinks=True)
                return SimpleNamespace(returncode=0, stdout="", stderr="")
            write_fake_mac_zip(command[-1])
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        def fake_audit(staged, cache, report_path):
            report = {"verdict": "pass", "cache": str(cache),
                      "cache_sha256": "c" * 64, "container_count": 1,
                      "total_matches": 0}
            Path(report_path).write_text(json.dumps(report))
            return report

        with mock.patch.object(bs.absence_audit, "audit",
                               side_effect=fake_audit), \
                mock.patch.object(bs, "_git_commit", return_value="abc123456"), \
                mock.patch.object(bs, "_source_snapshot",
                                  return_value=evidence["source"]), \
                mock.patch.object(bs.subprocess, "run", side_effect=fake_ditto):
            bs.stage_shipping_audit(self.shipping_args(), iso)
            with self.assertRaisesRegex(bs.BuildFailure, "funnel twin"):
                bs.stage_shipping_package(self.shipping_args(), iso)
            bs.stage_package(self.args(), iso)
            test_zip = iso / "release" / "WoT-Mac-guarded-abc123456.zip"
            test_zip.write_bytes(b"swapped after packaging")
            with self.assertRaisesRegex(bs.BuildFailure, "intact"):
                bs.stage_shipping_package(self.shipping_args(), iso)


class ForbiddenBinaryLiteralAuditTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.staged = Path(self._tmp.name) / "staged"
        self.staged.mkdir()

    @staticmethod
    def pe(payload: bytes) -> bytes:
        image = bytearray(0x84)
        image[:2] = b"MZ"
        image[0x3c:0x40] = (0x80).to_bytes(4, "little")
        image[0x80:0x84] = b"PE\0\0"
        return bytes(image) + payload

    @staticmethod
    def macho(payload: bytes) -> bytes:
        return b"\xcf\xfa\xed\xfe" + payload

    @staticmethod
    def fat64(payload: bytes, swapped: bool = False) -> bytes:
        magic = b"\xbf\xba\xfe\xca" if swapped else b"\xca\xfe\xba\xbf"
        return magic + b"\x00\x00\x00\x00" + payload

    def test_injected_ascii_and_tchar_needles_are_refused(self):
        cases = (
            ("Win64", self.pe(b"prefix archive.org suffix")),
            ("Win64", self.pe("twot_gog_classic".encode("utf-16-le"))),
            ("Mac", self.macho("archive.org".encode("utf-32-le"))),
            ("Mac", self.fat64(b"prefix archive.org suffix")),
            ("Mac", self.fat64(
                "twot_gog_classic".encode("utf-32-le"), swapped=True)),
        )
        for index, (platform, image) in enumerate(cases):
            path = self.staged / f"image-{index}"
            path.write_bytes(image)
            with self.subTest(platform=platform, index=index), \
                    self.assertRaisesRegex(
                        bs.BuildFailure, "forbidden installer-source literal"):
                bs._audit_forbidden_binary_literals(self.staged, platform)
            path.unlink()

    def test_receipt_records_every_regular_image_and_ignores_symlinks(self):
        first = self.staged / "WoT.exe"
        second = self.staged / "InternalTools" / "WoTUpdater.exe"
        second.parent.mkdir()
        first.write_bytes(self.pe(b"clean-game"))
        second.write_bytes(self.pe(b"clean-updater"))
        alias = self.staged / "alias.exe"
        try:
            alias.symlink_to(first)
        except OSError:
            pass
        receipt = bs._audit_forbidden_binary_literals(self.staged, "Win64")
        self.assertEqual(receipt["platform"], "Win64")
        self.assertEqual(
            [row["path"] for row in receipt["scanned"]],
            ["InternalTools/WoTUpdater.exe", "WoT.exe"])
        self.assertTrue(all(len(row["sha256"]) == 64
                            for row in receipt["scanned"]))

    def test_fat64_images_are_recorded_in_the_audit_receipt(self):
        first = self.staged / "fat-magic-64"
        second = self.staged / "fat-cigam-64"
        first.write_bytes(self.fat64(b"clean-ascii"))
        second.write_bytes(self.fat64(b"clean-swapped", swapped=True))
        receipt = bs._audit_forbidden_binary_literals(self.staged, "Mac")
        self.assertEqual(
            [row["path"] for row in receipt["scanned"]],
            ["fat-cigam-64", "fat-magic-64"],
        )
        self.assertTrue(all(row["format"] == "Mach-O"
                            for row in receipt["scanned"]))

    def test_staged_tree_audit_refuses_crash_report_client_executable(self):
        game = self.staged / "WoT-Win64-Test.exe"
        game.write_bytes(self.pe(b"game"))
        receipt = bs._audit_no_crash_report_client(self.staged)
        self.assertEqual(receipt["forbidden"], [])
        crash = (self.staged / "Engine" / "Binaries" / "Win64"
                 / "CrashReportClient.exe")
        crash.parent.mkdir(parents=True)
        crash.write_bytes(self.pe(b"crash"))
        with self.assertRaisesRegex(
                bs.BuildFailure, "CrashReportClient executable"):
            bs._audit_no_crash_report_client(self.staged)
        crash.unlink()
        renamed = crash.parent / "CrashReportClient" / "helper.exe"
        renamed.parent.mkdir()
        renamed.write_bytes(self.pe(b"renamed crash client"))
        with self.assertRaisesRegex(
                bs.BuildFailure, "CrashReportClient executable"):
            bs._audit_no_crash_report_client(self.staged)


class StagedProductionDescriptorTests(unittest.TestCase):
    """Early releases shipped without the cache-v3 production descriptor and worked
    only where the build machine's copy existed at the compiled source path."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.iso = Path(self._tmp.name) / "iso"
        self.descriptor = b'{"format": "cache-v3-production-descriptor-v1"}\n'
        pin = hashlib.sha256(self.descriptor).hexdigest()
        source = self.iso / "Build" / "WoTRelease" / "cache-v3-production-descriptor.json"
        source.parent.mkdir(parents=True)
        source.write_bytes(self.descriptor)
        builder = self.iso / "Source" / "WoTRetailReader" / "Private" / "CacheBuilder.cpp"
        builder.parent.mkdir(parents=True)
        builder.write_text(
            "constexpr TCHAR WoTCacheExpectedProductionDescriptorSha256[] =\n"
            f'\tTEXT("{pin}");\n', encoding="utf-8")

    def stage(self, root: Path, data: bytes | None = None) -> Path:
        path = root / "WoT" / "Build" / "WoTRelease" / "cache-v3-production-descriptor.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self.descriptor if data is None else data)
        return path

    def test_win64_and_mac_layouts_pass(self):
        win = self.iso / "staged-test" / "Windows"
        self.assertEqual(
            bs._require_staged_production_descriptor(win.parent, self.iso)
            if False else None, None)
        staged = self.stage(win)
        self.assertEqual(bs._require_staged_production_descriptor(win, self.iso), staged)
        mac = self.iso / "staged-test" / "Mac"
        staged = self.stage(mac / "WoT.app" / "Contents" / "UE")
        self.assertEqual(bs._require_staged_production_descriptor(mac, self.iso), staged)
        # Real Mac staging also leaves the loose <Stage>/Mac/WoT twin.
        self.stage(mac)
        self.assertEqual(bs._require_staged_production_descriptor(mac, self.iso), staged)
        self.stage(mac / "WoT.app" / "Contents" / "Resources")
        with self.assertRaisesRegex(bs.BuildFailure, "found 2"):
            bs._require_staged_production_descriptor(mac, self.iso)

    def test_missing_duplicate_stale_or_unpinned_descriptor_fails(self):
        win = self.iso / "staged-test" / "Windows"
        with self.subTest("missing"):
            with self.assertRaisesRegex(bs.BuildFailure, "found 0"):
                bs._require_staged_production_descriptor(win, self.iso)
        with self.subTest("duplicate"):
            self.stage(win)
            self.stage(win / "Engine" / "Extra")
            with self.assertRaisesRegex(bs.BuildFailure, "found 2"):
                bs._require_staged_production_descriptor(win, self.iso)
            shutil.rmtree(win / "Engine")
        with self.subTest("stale bytes"):
            self.stage(win, b'{"stale": true}\n')
            with self.assertRaisesRegex(bs.BuildFailure, "not the ISO's"):
                bs._require_staged_production_descriptor(win, self.iso)
        with self.subTest("pin mismatch"):
            self.stage(win)
            builder = self.iso / "Source" / "WoTRetailReader" / "Private" / "CacheBuilder.cpp"
            builder.write_text(
                "constexpr TCHAR WoTCacheExpectedProductionDescriptorSha256[] =\n"
                f'\tTEXT("{"0" * 64}");\n', encoding="utf-8")
            with self.assertRaisesRegex(bs.BuildFailure, "compiled pin"):
                bs._require_staged_production_descriptor(win, self.iso)


class StagedReleaseNotesTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name) / "staged"

    def stage(self, root: Path, data: bytes | None = None) -> Path:
        path = root / "WoT" / "Build" / "WoTRelease" / "ReleaseNotes.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"forbidden release notes\n" if data is None else data)
        return path

    def test_absence_passes(self):
        bs._require_release_notes_absent(self.root)

    def test_win64_loose_copy_fails(self):
        self.stage(self.root)
        with self.assertRaisesRegex(bs.BuildFailure, "must not carry"):
            bs._require_release_notes_absent(self.root)

    def test_mac_bundled_and_loose_twin_each_fail(self):
        for location in (
                self.root / "WoT.app" / "Contents" / "UE",
                self.root):
            with self.subTest(location=location):
                shutil.rmtree(self.root, ignore_errors=True)
                self.stage(location)
                with self.assertRaisesRegex(bs.BuildFailure, "must not carry"):
                    bs._require_release_notes_absent(self.root)


class StagedReleaseIdentityTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.iso = Path(self._tmp.name) / "iso"
        self.relative = Path("Build", "WoTRelease", "release-identity.json")
        self.identity = (b'{"format":"wot-release-identity-v1",'
                         b'"release_commit":"abc123456",'
                         b'"release_sequence":5,"version":1}')
        source = self.iso / self.relative
        source.parent.mkdir(parents=True)
        source.write_bytes(self.identity)
        bs._write_stamp(self.iso, "seed", {"release_commit": "abc123456"})

    def stage(self, root: Path, data: bytes | None = None) -> Path:
        path = root / "WoT" / self.relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self.identity if data is None else data)
        return path

    def test_missing_duplicate_mismatch_malformed_and_commit_errors_fail(self):
        win = self.iso / "staged-test" / "Windows"
        with self.subTest("missing"):
            with self.assertRaisesRegex(bs.BuildFailure, "found 0"):
                bs._require_staged_release_identity(win, self.iso)
        with self.subTest("duplicate"):
            self.stage(win)
            self.stage(win / "Engine" / "Extra")
            with self.assertRaisesRegex(bs.BuildFailure, "found 2"):
                bs._require_staged_release_identity(win, self.iso)
            shutil.rmtree(win / "Engine")
        with self.subTest("mismatched bytes"):
            self.stage(win, b"{}")
            with self.assertRaisesRegex(bs.BuildFailure, "not the ISO's"):
                bs._require_staged_release_identity(win, self.iso)
        with self.subTest("malformed"):
            (self.iso / self.relative).write_bytes(b"{")
            self.stage(win, b"{")
            with self.assertRaisesRegex(bs.BuildFailure, "release identity"):
                bs._require_staged_release_identity(win, self.iso)
        with self.subTest("empty commit"):
            empty = (b'{"format":"wot-release-identity-v1",'
                     b'"release_commit":"","release_sequence":5,"version":1}')
            (self.iso / self.relative).write_bytes(empty)
            self.stage(win, empty)
            with self.assertRaisesRegex(bs.BuildFailure, "empty release_commit"):
                bs._require_staged_release_identity(win, self.iso)
        with self.subTest("wrong commit"):
            wrong = self.identity.replace(b"abc123456", b"def123456")
            (self.iso / self.relative).write_bytes(wrong)
            self.stage(win, wrong)
            with self.assertRaisesRegex(bs.BuildFailure, "cooked seed"):
                bs._require_staged_release_identity(win, self.iso)

    def test_mac_bundled_copy_wins_over_loose_twin(self):
        mac = self.iso / "staged-test" / "Mac"
        bundled = self.stage(mac / "WoT.app" / "Contents" / "UE")
        self.stage(mac, b"loose staging twin is not shipped")
        self.assertEqual(
            bs._require_staged_release_identity(mac, self.iso), bundled)


class StagedReleaseVersionTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.iso = Path(self._tmp.name) / "iso"
        self.relative = Path("Build", "WoTRelease", "release-version.json")
        self.version = (b'{"format":"wot-release-version-v1",'
                        b'"human_version":"0.1.2","version":1}')
        source = self.iso / self.relative
        source.parent.mkdir(parents=True)
        source.write_bytes(self.version)
        bs._write_stamp(self.iso, "seed", {"human_version": "0.1.2"})

    def stage(self, root: Path, data: bytes = None) -> Path:
        path = root / "WoT" / self.relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self.version if data is None else data)
        return path

    def test_missing_duplicate_mismatch_malformed_and_stamp_errors_fail(self):
        win = self.iso / "staged-test" / "Windows"
        with self.subTest("missing"):
            with self.assertRaisesRegex(bs.BuildFailure, "found 0"):
                bs._require_staged_release_version(win, self.iso)
        with self.subTest("duplicate"):
            self.stage(win)
            self.stage(win / "Engine" / "Extra")
            with self.assertRaisesRegex(bs.BuildFailure, "found 2"):
                bs._require_staged_release_version(win, self.iso)
            shutil.rmtree(win / "Engine")
        with self.subTest("mismatched bytes"):
            self.stage(win, b"{}")
            with self.assertRaisesRegex(bs.BuildFailure, "not the ISO's"):
                bs._require_staged_release_version(win, self.iso)
        with self.subTest("malformed"):
            (self.iso / self.relative).write_bytes(b"{")
            self.stage(win, b"{")
            with self.assertRaisesRegex(bs.BuildFailure, "release version"):
                bs._require_staged_release_version(win, self.iso)
        with self.subTest("stamp mismatch"):
            (self.iso / self.relative).write_bytes(self.version)
            self.stage(win)
            bs._write_stamp(self.iso, "seed", {"human_version": "0.2.0"})
            with self.assertRaisesRegex(bs.BuildFailure, "cooked seed"):
                bs._require_staged_release_version(win, self.iso)

    def test_mac_bundled_copy_wins_over_loose_twin(self):
        mac = self.iso / "staged-test" / "Mac"
        bundled = self.stage(mac / "WoT.app" / "Contents" / "UE")
        self.stage(mac, b"loose staging twin is not shipped")
        self.assertEqual(
            bs._require_staged_release_version(mac, self.iso), bundled)


class CompileCacheTests(unittest.TestCase):
    """The persistent build root: wiring, transfer and fail-closed seams."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        # Resolved: macOS temp dirs live under /var, a symlink to /private/var.
        self.root = Path(self._tmp.name).resolve()
        self.addCleanup(self._tmp.cleanup)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self._old_repo = bs.REPO
        bs.REPO = self.repo
        self.addCleanup(setattr, bs, "REPO", self._old_repo)
        self.engine = self.root / "engine"
        self._pinned = mock.patch.dict(bs.PINNED_SOURCE_ENGINES, {
            "Mac": self.engine, "Win64": self.engine})
        self._pinned.start()
        self.addCleanup(self._pinned.stop)
        self.iso = self.root / "iso"
        self.build_root = self.root / "cache" / "Win64-7deeb413d3dc"
        # The UBT timestamp slop is exercised in test_guarded_build_root;
        # here it would only make back-to-back fake builds sleep.
        slop = mock.patch.object(
            build_root_cache, "TIMESTAMP_SLOP_SECONDS", 0.0)
        slop.start()
        self.addCleanup(slop.stop)

    def args(self, **overrides):
        values = {
            "guarded": True, "configuration": "Test", "platform": "Win64",
            "engine": None, "editor_process_affinity": None,
            "compile_cache": "on",
        }
        values.update(overrides)
        return SimpleNamespace(**values)

    def ready_args(self, **overrides):
        """Args whose build root is already proven and prepared."""
        args = self.args(**overrides)
        identity = {"format": "wot-ue58-source-receipt-v1",
                    "platform": args.platform, "commit": "7deeb413d3dc",
                    "tag": "5.8.0-release", "toolchain": {}, "checks": []}
        build_root_cache.prepare(self.build_root, args.platform, identity)
        args._build_root_path = self.build_root
        args._build_root_ready = {"reset": None,
                                  "engine_receipt_sha256": "e" * 64}
        return args

    def seed_iso_inputs(self):
        for relative, text in (
                ("WoT.uproject", "{}"),
                ("Config/DefaultEngine.ini", "[x]\n"),
                ("Source/WoT/WoT.cpp", "int x;\n")):
            path = self.iso / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")

    # -- mode resolution ------------------------------------------------

    def test_compile_cache_resolution_is_guarded_and_win64_by_default(self):
        cases = (
            (dict(), "on"),
            (dict(platform="Mac"), "off"),
            (dict(platform="Mac", compile_cache="on"), "on"),
            (dict(compile_cache="off"), "off"),
            (dict(compile_cache=None), "on"),
            (dict(guarded=False, configuration=None, compile_cache="auto"),
             "off"),
        )
        for overrides, expected in cases:
            with self.subTest(overrides=overrides):
                args = self.args(**{"compile_cache": "auto", **overrides})
                bs._resolve_build_mode(args)
                self.assertEqual(args.compile_cache, expected)
        with self.assertRaisesRegex(bs.BuildFailure, "requires --guarded"):
            bs._resolve_build_mode(self.args(
                guarded=False, configuration=None, compile_cache="on"))

    def test_build_root_is_keyed_by_platform_and_pinned_engine(self):
        manifest = (self.repo / "EnginePatches" / "UE-5.8"
                    / "source-toolchain.json")
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps({"engine": {
            "commit": "7deeb413d3dc1fc034f48d1aacc0861301829d32"}}))
        args = self.args(build_root_parent=self.root / "parent")
        self.assertEqual(bs._build_root(args),
                         (self.root / "parent").resolve()
                         / "Win64-7deeb413d3dc")
        with self.assertRaisesRegex(bs.BuildFailure, "disjoint"):
            bs._validate_build_root(self.iso / "nested", self.iso)
        with self.assertRaisesRegex(bs.BuildFailure, "disjoint"):
            bs._validate_build_root(self.root, self.iso)
        with self.assertRaisesRegex(bs.BuildFailure, "seeded source"):
            bs._validate_build_root(self.repo / "Source" / "x", self.iso)
        bs._validate_build_root(self.repo / ".guarded-build" / "x", self.iso)

    # -- commands -------------------------------------------------------

    def test_iso_tools_receive_only_resolved_machine_paths(self):
        args = self.args(
            conv_dir=self.root / "conv",
            exports_dir=self.root / "reference" / "exports",
            retail_root=self.root / "reference" / "original")
        configured = {
            "installed_engine_root_win64": self.root / "installed",
            "visual_studio_root_win64": self.root / "visual-studio",
        }
        with mock.patch.object(
                bs, "local_path",
                side_effect=lambda key, required=False: configured.get(key)):
            environment = bs._iso_tool_environment(
                args, self.engine,
                source={
                    "WOT_SOURCE_ENGINE_ROOT_WIN64": "stale-source",
                    "WOT_VISUAL_STUDIO_ROOT_WIN64": "stale-vs",
                    "WOT_UNRELATED": "preserved",
                })

        self.assertEqual(environment["WOT_SOURCE_ENGINE_ROOT_WIN64"],
                         str(self.engine.resolve()))
        self.assertEqual(environment["WOT_INSTALLED_ENGINE_ROOT_WIN64"],
                         str(configured["installed_engine_root_win64"]))
        self.assertEqual(environment["WOT_VISUAL_STUDIO_ROOT_WIN64"],
                         str(configured["visual_studio_root_win64"]))
        self.assertEqual(environment["WOT_CONVERSION_ROOT"],
                         str(args.conv_dir.resolve()))
        self.assertEqual(environment["WOT_EXPORTS_DIR"],
                         str(args.exports_dir.resolve()))
        self.assertEqual(environment["WOT_RETAIL_ROOT"],
                         str(args.retail_root.resolve()))
        self.assertEqual(environment["WOT_REFERENCE_ROOT"],
                         str(args.retail_root.resolve().parent))
        self.assertEqual(environment["WOT_UNRELATED"], "preserved")

    def test_build_root_builds_run_the_iso_wrapper_incrementally(self):
        receipt = self.iso / "receipts" / "engine.json"
        win = bs._guarded_build_command(
            self.args(), self.iso, self.engine, "WoT", receipt, "Test",
            self.build_root)
        self.assertEqual(win[win.index("-File") + 1], str(
            self.iso / "Tools" / "build" / "build_guarded_ue58.ps1"))
        self.assertEqual(win[win.index("-Project") + 1],
                         str(self.build_root / "WoT.uproject"))
        self.assertIn("-Incremental", win)
        self.assertNotIn("-Clean", win)
        self.assertEqual(win[-1], "-SerialLocalBuild")
        mac = bs._guarded_build_command(
            self.args(platform="Mac"), self.iso, self.engine, "WoT",
            receipt, "Shipping", self.build_root)
        self.assertEqual(mac[-1], "--incremental")
        self.assertEqual(mac[mac.index("--project") + 1],
                         str(self.build_root / "WoT.uproject"))
        cold = bs._guarded_build_command(
            self.args(), self.iso, self.engine, "WoT", receipt)
        self.assertIn("-Clean", cold)
        self.assertEqual(cold[cold.index("-Project") + 1],
                         str(self.iso / "WoT.uproject"))

    def test_cook_skips_only_the_uat_builds_the_build_root_replaced(self):
        runuat = self.engine / "Engine" / "Build" / "BatchFiles"
        runuat.mkdir(parents=True)
        (runuat / "RunUAT.bat").write_text("@exit /b 0\n")
        (runuat / "RunUAT.sh").write_text("#!/bin/sh\n")
        cached = bs._cook_command(self.args(), self.iso, self.engine)
        for token in (*bs.BUILD_ROOT_UAT_SKIPS, "-build", "-cook", "-stage",
                      "-pak"):
            self.assertIn(token, cached)
        bs._validate_guarded_unreal_command(cached, "cook", "Win64")
        import run_first_run_funnel as funnel
        funnel._validate_release_unreal_command(cached, "cook", "Win64")
        cold = bs._cook_command(
            self.args(compile_cache="off"), self.iso, self.engine)
        self.assertFalse(set(bs.BUILD_ROOT_UAT_SKIPS) & set(cold))

    def test_editor_build_mirrors_the_uat_editor_block(self):
        command = bs._editor_build_command(
            self.args(), self.engine, self.build_root)
        project = self.build_root / "WoT.uproject"
        self.assertEqual(command[0], str(
            self.engine / "Engine" / "Build" / "BatchFiles" / "Build.bat"))
        self.assertEqual(command[1:4], [
            f"-Target=WoTEditor Win64 Development -Project={project}",
            f"-Target=ShaderCompileWorker Win64 Development -Project={project}",
            "-NoHotReload"])
        self.assertEqual(command[4:], list(bs.WIN64_SERIAL_LOCAL_UBT_ARGUMENTS))

    # -- engine proof and identity ---------------------------------------

    def test_active_build_root_without_private_patch_uses_hash_only_verification(self):
        args = self.args()
        args._build_root_path = self.build_root
        steps = []

        def run(step, command, log_path, **_kwargs):
            steps.append(step)
            if step == "build-root-engine":
                self.assertIn("--require-patched", command)
                receipt = Path(command[command.index("--receipt") + 1])
                receipt.write_text(json.dumps({
                    "format": "wot-ue58-source-receipt-v1",
                    "platform": "Win64", "commit": "7deeb413d3dc",
                    "tag": "5.8.0-release", "toolchain": {"cl": "1"},
                    "checks": ["patch-inputs-patched(4)"],
                    "engine": str(self.engine),
                    "manifest": str(self.iso / "x.json")}))
            else:
                self.assertEqual(step, "build-root-engine-patch")
                self.assertNotIn("--restore", command)
                self.assertIn("--verify-only", command)
                self.assertIn("--require-patched", command)
                self.assertNotIn("--patch", command)
            return 0

        with mock.patch.object(bs, "_run_logged", side_effect=run):
            self.assertEqual(
                bs._active_build_root(args, self.iso, self.engine),
                self.build_root)
            bs._active_build_root(args, self.iso, self.engine)
        self.assertEqual(steps, ["build-root-engine-patch",
                                 "build-root-engine"])
        state = build_root_cache.read_state(self.build_root)
        self.assertEqual(state["engine_identity"]["toolchain"], {"cl": "1"})
        self.assertNotIn("manifest", state["engine_identity"])
        self.assertIsNone(
            bs._active_build_root(self.args(compile_cache="off"),
                                  self.iso, self.engine))

    def test_active_build_root_keeps_private_patch_apply_behavior(self):
        args = self.args()
        args._build_root_path = self.build_root
        patch = (self.iso / "EnginePatches" / "UE-5.8"
                 / "guarded-bootstrap.patch")
        patch.parent.mkdir(parents=True)
        patch.write_text("private patch fixture\n")

        def run(step, command, _log_path, **_kwargs):
            if step == "build-root-engine-patch":
                self.assertEqual(command[command.index("--patch") + 1], str(patch))
                self.assertNotIn("--verify-only", command)
            else:
                self.assertNotIn("--require-patched", command)
                receipt = Path(command[command.index("--receipt") + 1])
                receipt.write_text(json.dumps({
                    "format": "wot-ue58-source-receipt-v1",
                    "platform": "Win64", "commit": "7deeb413d3dc",
                    "tag": "5.8.0-release", "toolchain": {"cl": "1"},
                    "checks": ["patch-inputs-patched(4)"],
                    "engine": str(self.engine), "manifest": "ignored"}))
            return 0

        with mock.patch.object(bs, "_run_logged", side_effect=run):
            self.assertEqual(
                bs._active_build_root(args, self.iso, self.engine),
                self.build_root)

    # -- target builds ---------------------------------------------------

    def fake_wrapper(self, *, fail=False, exe=None, before=None):
        exe = fake_pe(b"built-exe") if exe is None else exe
        def run(step, command, log_path, **_kwargs):
            project = Path(command[command.index("-Project") + 1]).parent
            target = command[command.index("-Target") + 1]
            configuration = command[command.index("-Configuration") + 1]
            self.assertEqual(project, self.build_root)
            self.assertEqual(
                (project / "Source" / "WoT" / "WoT.cpp").read_text(),
                "int x;\n")
            if before is not None:
                before(project)
            if fail:
                raise bs.BuildFailure("wrapper failed")
            engine_receipt = Path(command[command.index("-Receipt") + 1])
            engine_receipt.write_text('{"engine": true}\n')
            engine_receipt.with_name(
                engine_receipt.stem + "-compiled.json").write_text(
                '{"compiled": true}\n')
            binaries = project / "Binaries" / "Win64"
            binaries.mkdir(parents=True, exist_ok=True)
            name = f"{target}-Win64-{configuration}"
            (binaries / f"{name}.exe").write_bytes(exe)
            (binaries / f"{name}.target").write_text(json.dumps({
                "TargetName": target, "Platform": "Win64",
                "Configuration": configuration,
                "Version": {"BuildId": "b"},
                "BuildProducts": [{
                    "Path": f"$(ProjectDir)/Binaries/Win64/{name}.exe",
                    "Type": "Executable"}],
                "RuntimeDependencies": [{
                    "Path": "$(ProjectDir)/WoT.uproject", "Type": "UFS"}],
            }))
            return 0
        return run

    def test_target_build_mirrors_builds_and_transfers_verified(self):
        self.seed_iso_inputs()
        args = self.ready_args()
        with mock.patch.object(bs, "_run_logged",
                               side_effect=self.fake_wrapper()):
            row = bs._run_guarded_target_build(
                args, self.iso, self.engine, "WoT")
        product = self.iso / "Binaries" / "Win64" / "WoT-Win64-Test.exe"
        self.assertEqual(product.read_bytes(), fake_pe(b"built-exe"))
        self.assertTrue(
            (self.iso / "Binaries" / "Win64" / "WoT-Win64-Test.target")
            .is_file())
        cache = row["compile_cache"]
        self.assertEqual(cache["format"], bs.COMPILE_CACHE_FORMAT)
        self.assertEqual(cache["sync"]["copied"], 3)
        transfer = json.loads(Path(cache["transfer_receipt"]).read_text())
        self.assertEqual(
            build_root_cache.product_digest(
                transfer, "Binaries/Win64/WoT-Win64-Test.exe"),
            bs._sha256_file(product))
        self.assertEqual(transfer["input_tree"]["sha256"],
                         cache["input_tree_sha256"])
        self.assertIsNotNone(build_root_cache.read_state(
            self.build_root)["last_ubt_exit_epoch"])

    def test_failed_build_still_records_the_ubt_exit(self):
        self.seed_iso_inputs()
        args = self.ready_args()
        with mock.patch.object(bs, "_run_logged",
                               side_effect=self.fake_wrapper(fail=True)):
            with self.assertRaisesRegex(bs.BuildFailure, "wrapper failed"):
                bs._run_guarded_target_build(
                    args, self.iso, self.engine, "WoT")
        self.assertIsNotNone(build_root_cache.read_state(
            self.build_root)["last_ubt_exit_epoch"])

    def test_interrupted_build_is_undone_before_the_next_ubt_run(self):
        """A link killed mid-write left a truncated exe that the next
        incremental UBT run took as up to date."""
        self.seed_iso_inputs()
        args = self.ready_args()
        partial = (self.build_root / "Binaries" / "Win64"
                   / "WoT-Win64-Test.exe")

        def die_mid_link(_project):
            partial.parent.mkdir(parents=True, exist_ok=True)
            partial.write_bytes(fake_pe(b"built-exe")[:-100])

        with mock.patch.object(bs, "_run_logged", side_effect=self.fake_wrapper(
                fail=True, before=die_mid_link)):
            with self.assertRaisesRegex(bs.BuildFailure, "wrapper failed"):
                bs._run_guarded_target_build(
                    args, self.iso, self.engine, "WoT")
        self.assertEqual(
            build_root_cache.read_state(self.build_root)[
                build_root_cache.IN_FLIGHT_KEY]["label"], "WoT-Test")
        seen = []
        with mock.patch.object(bs, "_run_logged", side_effect=self.fake_wrapper(
                before=lambda _project: seen.append(partial.exists()))):
            bs._run_guarded_target_build(args, self.iso, self.engine, "WoT")
        self.assertEqual(seen, [False])
        self.assertNotIn(build_root_cache.IN_FLIGHT_KEY,
                         build_root_cache.read_state(self.build_root))
        self.assertEqual(
            (self.iso / "Binaries" / "Win64" / "WoT-Win64-Test.exe")
            .read_bytes(), fake_pe(b"built-exe"))

    def test_staged_win64_images_must_be_the_transferred_products(self):
        self.seed_iso_inputs()
        args = self.ready_args()
        receipts = {}
        with mock.patch.object(bs, "_run_logged",
                               side_effect=self.fake_wrapper()):
            for target in bs.GUARDED_RECEIPT_TARGETS:
                receipts[target] = bs._run_guarded_target_build(
                    args, self.iso, self.engine, target)
        digests = {
            target: bs._sha256_file(
                self.iso / "Binaries" / "Win64"
                / f"{target}-Win64-Test.exe")
            for target in bs.GUARDED_RECEIPT_TARGETS}
        product_receipt = {"products": {
            target: {"sha256": digest} for target, digest in digests.items()}}
        bs._bind_transferred_products(
            product_receipt, receipts, "Win64", "Test")
        bs._bind_transferred_products(
            {"products": {}}, receipts, "Mac", "Test")
        bs._bind_transferred_products(
            {"products": {}}, {"WoT": {}, "WoTUpdater": {}}, "Win64", "Test")
        swapped = copy.deepcopy(product_receipt)
        swapped["products"]["WoT"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(bs.BuildFailure, "not the build-root"):
            bs._bind_transferred_products(swapped, receipts, "Win64", "Test")
        Path(receipts["WoT"]["compile_cache"]["transfer_receipt"]).write_text(
            "{}")
        with self.assertRaisesRegex(bs.BuildFailure, "changed"):
            bs._bind_transferred_products(
                product_receipt, receipts, "Win64", "Test")

    def fake_editor_build(self, build_id="engine-build"):
        def run(step, command, log_path, **_kwargs):
            self.assertTrue(step.startswith("build-root-editor-"))
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text("built\n")
            binaries = self.build_root / "Binaries" / "Win64"
            binaries.mkdir(parents=True, exist_ok=True)
            (binaries / "UnrealEditor-WoT.dll").write_bytes(
                fake_pe(b"editor"))
            (binaries / "UnrealEditor.modules").write_text(json.dumps({
                "BuildId": build_id,
                "Modules": {"WoT": "UnrealEditor-WoT.dll"}}))
            (binaries / "WoTEditor.target").write_text(json.dumps({
                "TargetName": "WoTEditor", "Platform": "Win64",
                "Configuration": "Development",
                "Version": {"BuildId": build_id},
                "BuildProducts": [
                    {"Path": "$(EngineDir)/Binaries/Win64/UnrealEditor.exe",
                     "Type": "Executable"},
                    {"Path": "$(ProjectDir)/Binaries/Win64/"
                             "UnrealEditor-WoT.dll",
                     "Type": "DynamicLibrary"},
                    {"Path": "$(ProjectDir)/Binaries/Win64/"
                             "UnrealEditor.modules",
                     "Type": "RequiredResource"}],
                "RuntimeDependencies": [
                    {"Path": "$(ProjectDir)/WoT.uproject", "Type": "UFS"}],
            }))
            return 0
        return run

    def test_build_root_editor_installs_a_matching_snapshot_editor(self):
        self.seed_iso_inputs()
        engine_modules = (self.engine / "Engine" / "Binaries" / "Win64"
                          / "UnrealEditor.modules")
        engine_modules.parent.mkdir(parents=True)
        engine_modules.write_text('{"BuildId": "engine-build"}')
        args = self.ready_args()
        with mock.patch.object(bs, "_run_logged",
                               side_effect=self.fake_editor_build()):
            row = bs._build_root_editor(args, self.iso, self.engine, "cook")
        self.assertEqual(row["build_id"], "engine-build")
        self.assertEqual(row["phase"], "cook")
        self.assertEqual(
            (self.iso / "Binaries" / "Win64" / "UnrealEditor-WoT.dll")
            .read_bytes(), fake_pe(b"editor"))
        with mock.patch.object(bs, "_run_logged",
                               side_effect=self.fake_editor_build("stale")):
            with self.assertRaisesRegex(bs.BuildFailure, "BuildId"):
                bs._build_root_editor(args, self.iso, self.engine, "cook")

    def test_stage_import_builds_the_editor_before_any_commandlet(self):
        args = self.ready_args(
            conv_dir=self.root / "converted",
            exports_dir=self.root / "exports",
            retail_root=self.root / "retail", mesh_source="prepared")
        events = []

        def run(step, _command, log_path, **_kwargs):
            events.append(step)
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text(bs.IMPORT_SUCCESS_MARKERS[0])
            return 0

        def editor(*_args):
            events.append("build-root-editor")
            return {"phase": "import"}

        with (mock.patch.object(bs, "_read_stamp", return_value=None),
              mock.patch.object(bs, "_invalidate_from"),
              mock.patch.object(bs, "_prepare_import_generated_outputs"),
              mock.patch.object(bs, "_neutral_support_lifecycle_receipt",
                                return_value={"authority": 1}),
              mock.patch.object(bs, "_engine_root_and_editor",
                                return_value=(self.engine,
                                              self.engine / "editor")),
              mock.patch.object(bs, "_build_root_editor",
                                side_effect=editor),
              mock.patch.object(bs, "_run_logged", side_effect=run),
              mock.patch.object(bs, "_run_fresh_fractal_finalize",
                                return_value={}),
              mock.patch.object(bs, "_run_neutral_shell_generation",
                                return_value={}),
              mock.patch.object(bs, "_write_stamp") as write):
            self.assertEqual(bs.stage_import(args, self.iso), "ran")
        self.assertEqual(events, ["build-root-editor", "import"])
        stamp = write.call_args.args[2]
        self.assertEqual(stamp["import_editor"], {"phase": "import"})
        self.assertEqual(stamp["guarded_import_steps"],
                         list(bs.GUARDED_IMPORT_STEPS))
        self.assertEqual(stamp["import_editor_source"], "build-root-v1")

    @unittest.skipUnless(os.name == "nt", "PowerShell wrapper is Win64-only")
    def test_incremental_wrapper_verifies_engine_builds_prunes_then_records(self):
        powershell = shutil.which("powershell.exe")
        if powershell is None:
            self.skipTest("Windows PowerShell is unavailable")
        build = self.engine / "Engine" / "Build" / "BatchFiles" / "Build.bat"
        ubt = (self.engine / "Engine" / "Binaries" / "DotNET" /
               "UnrealBuildTool" / "UnrealBuildTool.dll")
        build.parent.mkdir(parents=True)
        ubt.parent.mkdir(parents=True)
        build.write_bytes(b"@echo FAKE_BUILD_ARGS:%*\r\n@exit /b 0\r\n")
        ubt.write_bytes(b"fake")
        project = self.build_root / "WoT.uproject"
        project.parent.mkdir(parents=True)
        project.write_text("{}", encoding="utf-8")
        kept = (self.build_root / "Intermediate" / "Build" / "Win64" / "x64"
                / "WoT" / "Test" / "Core" / "Module.Core.cpp.obj")
        kept.parent.mkdir(parents=True)
        kept.write_bytes(b"obj")
        receipt = self.root / "receipts" / "engine.json"
        wrapper = Path(__file__).resolve().parents[1] / "build" / \
            "build_guarded_ue58.ps1"
        quoted = lambda value: str(value).replace("'", "''")
        prefix = (
            "function global:python { Write-Output ('PYTHON ' + "
            "($args -join ' ')); $global:LASTEXITCODE=0 }; "
            f"& '{quoted(wrapper)}' -Engine '{quoted(self.engine)}' "
            f"-Project '{quoted(project)}' -Target WoT -Platform Win64 "
            f"-Configuration Test -Receipt '{quoted(receipt)}' "
            "-SerialLocalBuild")
        both = subprocess.run(
            [powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy",
             "Bypass", "-Command", prefix + " -Clean -Incremental"],
            capture_output=True, text=True)
        self.assertNotEqual(both.returncode, 0)
        self.assertIn("mutually exclusive", both.stdout + both.stderr)
        self.assertNotIn("PYTHON", both.stdout)
        incremental = subprocess.run(
            [powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy",
             "Bypass", "-Command", prefix + " -Incremental"],
            capture_output=True, text=True)
        self.assertEqual(incremental.returncode, 0, incremental.stderr)
        lines = [line for line in incremental.stdout.splitlines()
                 if line.startswith(("PYTHON", "FAKE_BUILD_ARGS"))]
        order = [next(name for name in (
            "verify_ue58_source.py", "apply_engine_patch.py",
            "FAKE_BUILD_ARGS", "prune_stale_module_intermediates.py",
            "record_build_receipt.py") if name in line) for line in lines]
        self.assertEqual(order, [
            "verify_ue58_source.py", "apply_engine_patch.py",
            "FAKE_BUILD_ARGS", "prune_stale_module_intermediates.py",
            "record_build_receipt.py"])
        self.assertNotIn("--restore", incremental.stdout)
        self.assertIn(f"--project {project}", lines[3])
        self.assertTrue(kept.is_file())

    def test_main_locks_the_build_root_and_skips_the_repo_editor_resync(self):
        self.iso.mkdir()
        bs._write_stamp(self.iso, "seed", {"seeded": True})
        held = []

        class FakeLock:
            def __init__(self, root):
                self.root = root

            def __enter__(self):
                held.append(("enter", self.root))
                return self

            def __exit__(self, *_exc):
                held.append(("exit", self.root))

        def resolve(args):
            args.compile_cache = "on"

        def stage(name):
            def run(*_args, **_kwargs):
                held.append(name)
                bs._write_stamp(self.iso, name, {"ran": True})
                return "ran"
            return run

        with mock.patch.object(bs, "_resolve_build_mode",
                               side_effect=resolve), \
                mock.patch.object(bs, "_validate_iso", return_value=self.iso), \
                mock.patch.object(bs, "_require_reviewed_innoextract"), \
                mock.patch.object(bs, "_build_root",
                                  return_value=self.build_root), \
                mock.patch.object(bs.build_root_cache, "BuildRootLock",
                                  FakeLock), \
                mock.patch.object(bs, "_ensure_repo_editor_module") as resync, \
                mock.patch.object(bs, "_engine_root_and_editor",
                                  return_value=(self.engine,
                                                self.engine / "editor")), \
                mock.patch.object(bs, "stage_import",
                                  side_effect=stage("import")), \
                mock.patch.object(bs, "stage_marker",
                                  side_effect=stage("marker")), \
                mock.patch.object(bs, "stage_cook", side_effect=stage("cook")), \
                mock.patch.object(bs, "_run_guarded_unreal_lifecycle",
                                  side_effect=lambda *a: (a[3](), {"z": 1})), \
                mock.patch.object(bs, "write_summary",
                                  return_value=self.iso / "summary.json"):
            exit_code = bs.main([
                "--iso", str(self.iso), "--cache", str(self.root / "cache"),
                "--guarded", "--platform", "Win64",
                "--only-step", "import,marker,cook"])
        self.assertEqual(exit_code, 0)
        resync.assert_not_called()
        self.assertEqual(held, [("enter", self.build_root), "import",
                                "marker", "cook", ("exit", self.build_root)])


class ReviewedInnoextractTests(unittest.TestCase):
    """A guarded package carries only the pinned innoextract library."""

    VENDOR = bs.REPO / "ThirdParty" / "innoextract" / "1.9"

    def _vendor_copy(self, root: Path) -> Path:
        vendor = root / "vendor"
        shutil.copytree(self.VENDOR, vendor, ignore=shutil.ignore_patterns("lib"))
        return vendor

    def _pin(self, vendor: Path, platform: str, data: bytes) -> Path:
        manifest = vendor / "vendor-manifest.json"
        document = json.loads(manifest.read_bytes())
        document["artifacts"]["libraries"][platform]["sha256"] = (
            hashlib.sha256(data).hexdigest())
        manifest.write_bytes((json.dumps(
            document, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii"))
        return manifest

    def test_a_contributor_build_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            vendor = self._vendor_copy(Path(temporary))
            manifest = self._pin(vendor, "Win64", b"reviewed")
            library = vendor / "lib/Win64/wot_innoextract.lib"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"contributor build")
            with self.assertRaisesRegex(bs.BuildFailure, "fetch_innoextract_libs"):
                bs._require_reviewed_innoextract("Win64", manifest)

    def test_a_missing_library_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            vendor = self._vendor_copy(Path(temporary))
            manifest = self._pin(vendor, "Mac", b"reviewed")
            with self.assertRaisesRegex(bs.BuildFailure, "library is missing"):
                bs._require_reviewed_innoextract("Mac", manifest)

    def test_the_pinned_library_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            vendor = self._vendor_copy(Path(temporary))
            manifest = self._pin(vendor, "Win64", b"reviewed")
            library = vendor / "lib/Win64/wot_innoextract.lib"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"reviewed")
            self.assertEqual(bs._require_reviewed_innoextract("Win64", manifest),
                             hashlib.sha256(b"reviewed").hexdigest())

    def test_guarded_main_checks_before_any_stage(self):
        import contextlib
        import io
        iso = Path(tempfile.gettempdir()) / "wot-unused-iso"
        refused = bs.BuildFailure("guarded packaging needs the reviewed library")
        with mock.patch.object(bs, "_resolve_build_mode"), \
                mock.patch.object(bs, "_validate_iso", return_value=iso), \
                mock.patch.object(bs, "_require_reviewed_innoextract",
                                  side_effect=refused) as check, \
                mock.patch.object(bs, "_run_guarded_unreal_lifecycle") as life, \
                mock.patch.object(bs, "stage_seed") as seed, \
                contextlib.redirect_stderr(io.StringIO()):
            exit_code = bs.main([
                "--iso", str(iso), "--cache", str(iso / "cache"),
                "--guarded", "--platform", "Win64",
                "--only-step", "import,marker,cook"])
        self.assertEqual(exit_code, 1)
        check.assert_called_once_with("Win64")
        life.assert_not_called()
        seed.assert_not_called()


class GuardedImportPruneTests(unittest.TestCase):
    def args(self, guarded=True):
        root = Path(tempfile.gettempdir())
        return SimpleNamespace(
            guarded=guarded, configuration="Test" if guarded else
            "Development", platform="Win64", editor_process_affinity=None,
            compile_cache="off", conv_dir=root / "converted",
            exports_dir=root / "exports", retail_root=root / "retail",
            mesh_source="prepared")

    def test_guarded_import_runs_exactly_the_shipping_producers(self):
        import import_all
        editor = Path(tempfile.gettempdir()) / "UnrealEditor-Cmd.exe"
        iso = Path(tempfile.gettempdir()) / "iso"
        script = next(value for value in bs._import_command(
            self.args(), iso, editor) if value.startswith("-script="))
        argv = script.split()[1:]
        self.assertIn(
            "--only=bootstrap-map,frontend-map,mover-wave-runtime", argv)
        self.assertIn("--skip=meshes,pickups", argv)
        options = import_all._parser().parse_args(argv)
        selected = (import_all._split_options(options.only)
                    - import_all._split_options(options.skip))
        self.assertEqual(selected, {
            "bootstrap-map", "frontend-map", "mover-wave-runtime"})
        # frontend-map is skip-if-exists: the seeded front end ships as-is.
        spec = next(step for step in import_all.IMPORT_STEP_SPECS
                    if step.name == "frontend-map")
        self.assertIn("skip if it already exists", spec.summary)
        development = next(value for value in bs._import_command(
            self.args(guarded=False), iso, editor)
            if value.startswith("-script="))
        self.assertNotIn("--only=", development)

    def test_import_cache_identity_binds_the_pruned_step_set(self):
        guarded = bs._import_cache_identity(self.args())
        self.assertEqual(guarded["guarded_import_steps"],
                         ["bootstrap-map", "frontend-map",
                          "mover-wave-runtime"])
        self.assertEqual(guarded["import_editor_source"],
                         "seeded-repo-binaries-v1")
        self.assertNotIn("guarded_import_steps",
                         bs._import_cache_identity(self.args(guarded=False)))

    def test_full_policy_reproduces_the_previous_guarded_import(self):
        import import_all
        args = self.args()
        args.guarded_import = "full"
        editor = Path(tempfile.gettempdir()) / "UnrealEditor-Cmd.exe"
        script = next(value for value in bs._import_command(
            args, Path(tempfile.gettempdir()) / "iso", editor)
            if value.startswith("-script="))
        argv = script.split()[1:]
        self.assertNotIn("--only=", script)
        self.assertIn("--skip=meshes,pickups", argv)
        options = import_all._parser().parse_args(argv)
        self.assertEqual(options.only, [])
        self.assertEqual(import_all._split_options(options.skip),
                         {"meshes", "pickups"})
        identity = bs._import_cache_identity(args)
        self.assertEqual(identity["guarded_import_steps"],
                         ["all-except:meshes,pickups"])
        self.assertNotEqual(identity,
                            bs._import_cache_identity(self.args()))
        args.guarded_import = "bogus"
        with self.assertRaisesRegex(bs.BuildFailure, "guarded-import"):
            bs._import_command(args, Path(tempfile.gettempdir()), editor)


class ReferenceClosureTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.iso = Path(self._tmp.name) / "iso"
        self.write_package("/Game/WoT/Maps/WoTBootstrap", b"code-only",
                           ".umap")
        self.write_package(
            "/Game/WoT/Maps/WoTFrontEnd",
            b"\x00/Game/WoT/Materials/Instances/MI_GammaBoundary"
            b".MI_GammaBoundary\x00", ".umap")
        for package in bs.NEUTRAL_SUPPORT_PACKAGES:
            self.write_package(package, b"\x00/Script/Engine\x00")
        self.write_package(
            "/Game/WoT/Materials/M_WoT_World",
            b"\x00/Game/WoT/Materials/MPC_WoTDynamicLights\x00"
            b"/Game/WoT/Textures/WOTDetail/T_WoT_Neutral.T_WoT_Neutral\x00"
            b"/Game/WoT/Textures/WOTDetail\x00")
        for name in bs.NEUTRAL_SHELL_NAMES:
            self.write_package(
                f"/WoTNeutralShells/{name}",
                b"\x00/Game/WoT/Materials/M_WoT_EntryWorld\x00"
                b"/Game/WoTNeutralShellStaging/" + name.encode() + b"\x00",
                ".umap")
        # Unreferenced retail-derived content the pruned import leaves alone.
        self.write_package("/Game/WoT/Maps/Arena_01", b"retail", ".umap")
        self.write_package("/Game/WoT/Materials/MF_Helper_2", b"helper")

    def write_package(self, package: str, data: bytes,
                      extension: str = ".uasset") -> Path:
        mount, _, rest = package[1:].partition("/")
        base = (self.iso / "Content" if mount == "Game" else
                self.iso / bs.NEUTRAL_SHELL_PLUGIN_ROOT / "Content")
        path = base / (rest + extension)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_closure_of_shipped_roots_stays_inside_the_allowlist(self):
        result = bs._verify_guarded_reference_closure(self.iso)
        self.assertEqual(result["format"], bs.REFERENCE_CLOSURE_FORMAT)
        allowed = {bs.BOOTSTRAP_MAP, bs.FRONT_END_MAP,
                   *bs.NEUTRAL_SHELL_MAPS, *bs.NEUTRAL_SUPPORT_PACKAGES}
        self.assertTrue(set(result["packages"]) <= allowed)
        self.assertIn("/Game/WoT/Materials/MPC_WoTDynamicLights",
                      result["packages"])
        self.assertIn("/Game/WoTNeutralShellStaging/WoTMapShell01",
                      result["unresolved_names"])
        self.assertIn("/Game/WoT/Textures/WOTDetail",
                      result["unresolved_names"])
        again = bs._verify_guarded_reference_closure(self.iso)
        self.assertEqual(again["closure_sha256"], result["closure_sha256"])

    def test_any_reachable_non_shipped_package_fails_closed(self):
        cases = (
            ("hard or soft reference",
             "/Game/WoT/Materials/M_WoT_Master",
             b"\x00/Game/WoT/Maps/Arena_01.Arena_01\x00", ".uasset",
             "Arena_01"),
            ("FName number-suffix split",
             "/Game/WoT/Materials/M_WoT_Decal",
             b"\x00/Game/WoT/Materials/MF_Helper\x00", ".uasset",
             "MF_Helper_2"),
            ("sidecar string data",
             "/Game/WoT/Materials/M_WoT_MeshLit",
             b"\x00/Game/WoT/Maps/Arena_01\x00", ".uexp", "Arena_01"),
        )
        for label, package, data, extension, expected in cases:
            with self.subTest(label):
                path = self.write_package(package, data, extension)
                with self.assertRaisesRegex(bs.BuildFailure, expected):
                    bs._verify_guarded_reference_closure(self.iso)
                if extension == ".uexp":
                    path.unlink()
                else:
                    path.write_bytes(b"\x00/Script/Engine\x00")

    def test_missing_shipped_package_fails_closed(self):
        (self.iso / bs.NEUTRAL_SHELL_PLUGIN_ROOT / "Content"
         / "WoTMapShell07.umap").unlink()
        with self.assertRaisesRegex(bs.BuildFailure, "WoTMapShell07"):
            bs._verify_guarded_reference_closure(self.iso)


class BuildMachinePathTests(unittest.TestCase):
    """Shipped Mac binaries must not point back into the build machine."""

    def setUp(self):
        self.app = Path("/stage/Mac/WoT.app")
        self.exe = self.app / "Contents" / "MacOS" / "WoT-Mac-Test"
        self.dylib = (self.app / "Contents" / "UE" / "Engine" / "Binaries" /
                      "ThirdParty" / "EOSSDK" / "Mac" / "libEOSSDK.dylib")

    def test_bundle_and_install_relative_rpaths_stay(self):
        kept = ["@executable_path/", "@executable_path/../../../",
                "@executable_path/../UE/WoT/Binaries/Mac", "@loader_path/",
                "@loader_path/../../../.", "@rpath/x"]
        self.assertEqual(bs._escaping_rpaths(self.exe, kept, self.app), [])

    def test_build_tree_rpaths_escape(self):
        leaked = ("@loader_path/../../../../../../../../../opt/example/"
                  "UnrealEngine-source/Engine/Binaries/ThirdParty/EOSSDK/Mac")
        absolute = "/synthetic/fixture-engine/Engine/Binaries"
        self.assertEqual(
            bs._escaping_rpaths(self.exe, [leaked, absolute], self.app),
            [leaked, absolute])

    def test_absolute_system_rpaths_stay(self):
        system = ["/System/Library/Frameworks/D3DMetal.framework/Resources",
                  "/usr/lib/swift"]
        self.assertEqual(bs._escaping_rpaths(self.dylib, system, self.app), [])

    def test_loader_path_resolves_from_each_binary(self):
        # Seven levels up from the dylib's directory is still inside the app;
        # nine leaves the install directory.
        inside = "@loader_path/../../../../../../../MacOS"
        outside = "@loader_path/../../../../../../../../../elsewhere"
        self.assertEqual(
            bs._escaping_rpaths(self.dylib, [inside, outside], self.app),
            [outside])

    def test_otool_rpaths_parse(self):
        listing = (
            "Load command 20\n          cmd LC_RPATH\n      cmdsize 32\n"
            "         path @executable_path/ (offset 12)\n"
            "Load command 21\n          cmd LC_RPATH\n      cmdsize 120\n"
            "         path @loader_path/../../x y (offset 12)\n")
        with mock.patch.object(bs.subprocess, "run", return_value=SimpleNamespace(
                returncode=0, stdout=listing, stderr="")):
            self.assertEqual(bs._mach_o_rpaths(Path("/b")),
                             ["@executable_path/", "@loader_path/../../x y"])

    def test_universal_binary_rpath_is_deleted_once(self):
        leaked = "/synthetic/internal-sdk/PrivateFrameworks"
        calls = []

        def run(args, **_kwargs):
            calls.append(args)
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        with mock.patch.object(bs, "_guarded_mac_binaries",
                               return_value=[self.dylib]), \
                mock.patch.object(bs, "_mach_o_rpaths",
                                  return_value=[leaked, leaked]), \
                mock.patch.object(bs.subprocess, "run", side_effect=run):
            self.assertEqual(bs._strip_build_tree_rpaths(self.app), 1)
        self.assertEqual(calls, [
            ["install_name_tool", "-delete_rpath", leaked, str(self.dylib)],
            ["codesign", "--force", "--sign", "-", str(self.dylib)],
            ["codesign", "--verify", "--strict", str(self.dylib)]])

    def test_untouched_binary_is_not_resigned(self):
        calls = []

        def run(args, **_kwargs):
            calls.append(args)
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        with mock.patch.object(bs, "_guarded_mac_binaries",
                               return_value=[self.dylib]), \
                mock.patch.object(bs, "_mach_o_rpaths",
                                  return_value=["@loader_path"]), \
                mock.patch.object(bs.subprocess, "run", side_effect=run):
            self.assertEqual(bs._strip_build_tree_rpaths(self.app), 0)
        self.assertEqual(calls, [])

    def test_failed_resign_after_rpath_delete_is_refused(self):
        leaked = "/synthetic/ci-build/lib"

        def run(args, **_kwargs):
            return SimpleNamespace(
                returncode=1 if args[0] == "codesign" else 0,
                stdout="", stderr="synthetic codesign failure")

        with mock.patch.object(bs, "_guarded_mac_binaries",
                               return_value=[self.dylib]), \
                mock.patch.object(bs, "_mach_o_rpaths", return_value=[leaked]), \
                mock.patch.object(bs.subprocess, "run", side_effect=run):
            with self.assertRaisesRegex(bs.BuildFailure, "cannot re-sign"):
                bs._strip_build_tree_rpaths(self.app)

    def test_gate_refuses_an_embedded_build_path(self):
        with tempfile.TemporaryDirectory() as scratch:
            app = Path(scratch) / "WoT.app"
            exe = app / "Contents" / "MacOS" / "WoT-Mac-Test"
            exe.parent.mkdir(parents=True)
            exe.write_bytes(b"\xcf\xfa\xed\xfe" + b"..." +
                            b"/build/iso/Source/WoT/X.cpp" + b"\0")
            with mock.patch.object(bs, "_mach_o_rpaths", return_value=[]):
                with self.assertRaisesRegex(bs.BuildFailure, "embeds the build path"):
                    bs._require_no_build_machine_paths(app, [Path("/build/iso")])
                bs._require_no_build_machine_paths(app, [Path("/other")])

class MacZipAppleDoubleTests(unittest.TestCase):
    """The Mac zip step refuses AppleDouble members instead of shipping them."""

    def test_clean_zip_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            asset = Path(tmp) / "WoT-Mac.zip"
            write_fake_mac_zip(asset)
            bs._require_no_appledouble_entries(asset)

    def test_appledouble_and_macosx_members_are_refused(self):
        for member in ("WoT.app/Contents/MacOS/._WoT-Mac-Test",
                       "WoT.app/Contents/._MacOS",
                       "__MACOSX/WoT.app/._Info.plist"):
            with self.subTest(member=member), \
                    tempfile.TemporaryDirectory() as tmp:
                asset = Path(tmp) / "WoT-Mac.zip"
                write_fake_mac_zip(asset, extra=(member,))
                with self.assertRaisesRegex(bs.BuildFailure, "AppleDouble"):
                    bs._require_no_appledouble_entries(asset)

    def test_unreadable_zip_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            asset = Path(tmp) / "WoT-Mac.zip"
            asset.write_bytes(b"not a zip")
            with self.assertRaisesRegex(bs.BuildFailure, "unreadable"):
                bs._require_no_appledouble_entries(asset)


if __name__ == "__main__":
    unittest.main()
