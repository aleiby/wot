#!/usr/bin/env python3
"""Synthetic source-tree specs for the pinned world-start inventory."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import apply_engine_patch as patcher
import check_world_start_hooks as checker

DEPENDENCY = "Engine/Build/Commit.gitdeps.xml"

TICK_STATEMENTS = {
    "CopyWorldData()",
    "CurrentContext.SetCurrentWorld(LoadedWorld)",
    "GWorld=LoadedWorld",
    "LoadedWorld->InitWorld()",
    "LoadedWorld->CreateAISystem()",
    "LoadedWorld->InitializeActorsForPlay(PendingTravelURL,false)",
    "FNavigationSystem::AddNavigationSystemToWorld(*LoadedWorld,FNavigationSystemRunMode::GameMode)",
    "LoadedWorld->BeginPlay()",
}
LOAD_MAP_STATEMENTS = {
    "GWorld=NewWorld",
    "WorldContext.SetCurrentWorld(NewWorld)",
    "WorldContext.World()->InitWorld()",
    "MovePendingLevel(WorldContext)",
    "if(!WorldContext.World()->Listen(URL))",
    "WorldContext.World()->CreateAISystem()",
    "WorldContext.World()->InitializeActorsForPlay(URL,true,&Context)",
    "FNavigationSystem::AddNavigationSystemToWorld(*WorldContext.World(),FNavigationSystemRunMode::GameMode)",
    "WorldContext.World()->BeginPlay()",
}


def _statements(relative: str) -> list[str]:
    return [statement for (path, _kind, statement), count
            in checker.PINNED_INVENTORY.items() if path == relative
            for _ in range(count)]


def _emit(lines: list[str]) -> str:
    return "\n".join("    " + line + ";" for line in lines) + "\n"


class SyntheticHookEngine:
    def __init__(self, root: Path, *, hook_before: bool = True,
                 include_macro: bool = True, omitted_identity: str = "",
                 second_hook: bool = False):
        self.root = root
        self.source_manifest = root.parent / "source-toolchain.json"
        self.patch_manifest = root.parent / "manifest.json"
        (root / "Engine" / "Build").mkdir(parents=True)
        (root / DEPENDENCY).write_text("<DependencyManifest/>\n")
        self.write_sources(hook_before=hook_before, include_macro=include_macro,
                           omitted_identity=omitted_identity, second_hook=second_hook)

        for relative in patcher.PATCHED_PATHS:
            if relative in patcher.CREATED_PATHS or relative in {
                    checker.WORLD_CPP, checker.UNREAL_CPP}:
                continue
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"synthetic pristine {relative}\n")

        self.git("init", "-q")
        self.git("config", "user.email", "spec@example.invalid")
        self.git("config", "user.name", "spec")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "synthetic pinned engine")
        self.commit = self.output("rev-parse", "HEAD")
        self.git("tag", "5.8.0-release")
        self.write_manifests()

    def git(self, *args: str) -> None:
        subprocess.run(["git", "-C", str(self.root), *args], check=True,
                       capture_output=True)

    def output(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.root), *args], check=True,
                              capture_output=True, text=True).stdout.strip()

    def _gate(self, identities: tuple[str, ...], include_macro: bool,
              omitted_identity: str, second_hook: bool) -> str:
        values = [identity for identity in identities if identity != omitted_identity]
        lines = []
        if include_macro:
            lines.append("#if WOT_GUARDED_BOOTSTRAP")
        lines.extend(f"    auto Path{index} = EWoTWorldStartPath::{identity};"
                     for index, identity in enumerate(values))
        lines.append(
            "    if (!WoTGuardedStartup_MaterializeWorld(World, Path0, &Error)) return false;")
        if second_hook:
            lines.append(
                "    WoTGuardedStartup_MaterializeWorld(World, Path0, &Error);")
        if include_macro:
            lines.append("#endif")
        return "\n".join(lines) + "\n"

    def write_sources(self, *, hook_before: bool = True,
                      include_macro: bool = True, omitted_identity: str = "",
                      second_hook: bool = False) -> None:
        preamble = (
            '#include "WoTWorldMaterialization.h"\n'
            'extern "C" bool WoTGuardedStartup_MaterializeWorld(\n'
            '    UWorld* World, EWoTWorldStartPath Path, FString* Error);\n'
            'extern "C" bool WoTGuardedStartup_CommitWorldExposure(\n'
            '    UWorld* World, FString* Error);\n\n')
        world_all = _statements(checker.WORLD_CPP)
        tick = [
            "CurrentWorld->BeginTearingDown()",
            "FWorldDelegates::OnSeamlessTravelTransition.Broadcast(CurrentWorld)",
        ] + [line for line in world_all if line in TICK_STATEMENTS]
        tick.insert(tick.index("GWorld=LoadedWorld") + 1,
                    "#if WOT_GUARDED_BOOTSTRAP\n"
                    "    if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;\n"
                    "#endif")
        world_other = [line for line in world_all if line not in TICK_STATEMENTS]
        seamless_gate = self._gate(checker.PATH_IDENTITIES[3:], include_macro,
                                   omitted_identity, second_hook)
        tick_body = (seamless_gate + _emit(tick)) if hook_before else (
            _emit(tick[:1]) + seamless_gate + _emit(tick[1:]))
        world = preamble + (
            "void KnownCoveredWorldObservers()\n{\n" + _emit(world_other) + "}\n\n"
            "UWorld* FSeamlessTravelHandler::Tick()\n{\n" + tick_body
            + "    return true;\n}\n")

        unreal_all = _statements(checker.UNREAL_CPP)
        load = [line for line in unreal_all if line in LOAD_MAP_STATEMENTS]
        unreal_other = [line for line in unreal_all if line not in LOAD_MAP_STATEMENTS]
        load_gate = self._gate(checker.PATH_IDENTITIES[:3], include_macro,
                               omitted_identity, second_hook)
        load.insert(load.index("WorldContext.SetCurrentWorld(NewWorld)") + 1,
                    "#if WOT_GUARDED_BOOTSTRAP\n"
                    "    if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;\n"
                    "#endif")
        load_body = (load_gate + _emit(load)) if hook_before else (
            _emit(load[:1]) + load_gate + _emit(load[1:]))
        unreal = preamble + (
            "void KnownCoveredEngineObservers()\n{\n" + _emit(unreal_other) + "}\n\n"
            "bool UEngine::LoadMap(FWorldContext& WorldContext)\n{\n" + load_body
            + "    return true;\n}\n")

        for relative, text in ((checker.WORLD_CPP, world),
                               (checker.UNREAL_CPP, unreal)):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

        header = self.root / checker.CONTRACT_HEADER
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(
            '#pragma once\n#include "CoreTypes.h"\n\n'
            'enum class EWoTWorldStartPath : uint8\n{\n'
            + "".join(f"    {identity},\n" for identity in checker.PATH_IDENTITIES)
            + '};\n')

    def write_manifests(self) -> None:
        pristine = {
            relative: patcher.sha256_file(self.root / relative)
            for relative in patcher.PATCHED_PATHS
            if relative not in patcher.CREATED_PATHS
        }
        source = {
            "format": patcher.source_verifier.MANIFEST_FORMAT,
            "engine": {"repository": "synthetic.invalid", "tag": "5.8.0-release",
                       "commit": self.commit},
            "dependencies": {DEPENDENCY: patcher.sha256_file(self.root / DEPENDENCY)},
            "pristine_sources": pristine,
            "toolchains": {"Mac": {"apple_clang": "unused", "xcode_path": "unused",
                                    "macos_sdk": "unused"}},
            "rejected_install_roots": [],
        }
        self.source_manifest.write_text(json.dumps(source))
        patch = {
            "format": patcher.MANIFEST_FORMAT,
            "source_manifest": self.source_manifest.name,
            "patch_sha256": None,
            "record_command": "apply_engine_patch.py ... --record",
            "files": [{
                "path": relative,
                "created": relative in patcher.CREATED_PATHS,
                **({} if relative in patcher.CREATED_PATHS else {
                    "pristine_sha256": pristine[relative]}),
                "patched_sha256": None,
            } for relative in patcher.PATCHED_PATHS],
        }
        self.patch_manifest.write_text(json.dumps(patch))


class CheckWorldStartHooksTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def fixture(self, **options) -> SyntheticHookEngine:
        return SyntheticHookEngine(self.root / "engine", **options)

    def test_all_five_paths_pass_before_the_pinned_observer_inventory(self):
        engine = self.fixture()
        self.assertEqual(checker.check_engine(engine.root), 40)

    def test_missing_materialization_fails(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        world.write_text(world.read_text().replace(
            "if (!WoTGuardedStartup_MaterializeWorld(World, Path0, &Error)) return false;",
            "if (!UnrelatedCall(World, Path0, &Error)) return false;"))
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "exactly one materialization"):
            checker.check_engine(engine.root)

    def test_pristine_engine_shape_without_materialization_is_rejected(self):
        engine = self.fixture()
        for relative in (checker.WORLD_CPP, checker.UNREAL_CPP):
            path = engine.root / relative
            path.write_text(path.read_text().replace(
                "if (!WoTGuardedStartup_MaterializeWorld(World, Path0, &Error)) return false;",
                "if (!UnrelatedCall(World, Path0, &Error)) return false;"))
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "exactly one materialization"):
            checker.check_engine(engine.root)

    def test_comments_cannot_spoof_a_materialization_call(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        world.write_text(world.read_text().replace(
            "WoTGuardedStartup_MaterializeWorld(World, Path0, &Error)",
            "OtherCall(World, Path0, &Error) // WoTGuardedStartup_MaterializeWorld(World, Path0, &Error)"))
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "exactly one materialization"):
            checker.check_engine(engine.root)

    def test_an_observer_before_materialization_fails(self):
        engine = self.fixture(hook_before=False)
        with self.assertRaisesRegex(checker.HookCheckFailed, "ORDER VIOLATION"):
            checker.check_engine(engine.root)

    def test_seamless_teardown_before_materialization_fails(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        text = world.read_text()
        gate = "if (!WoTGuardedStartup_MaterializeWorld(World, Path0, &Error)) return false;"
        teardown = "CurrentWorld->BeginTearingDown();"
        text = text.replace(teardown, "")
        text = text.replace(gate, teardown + "\n    " + gate)
        world.write_text(text)
        with self.assertRaisesRegex(checker.HookCheckFailed, "ORDER VIOLATION"):
            checker.check_engine(engine.root)

    def test_seamless_transition_delegate_before_materialization_fails(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        text = world.read_text()
        gate = "if (!WoTGuardedStartup_MaterializeWorld(World, Path0, &Error)) return false;"
        delegate = "FWorldDelegates::OnSeamlessTravelTransition.Broadcast(CurrentWorld);"
        text = text.replace(delegate, "")
        text = text.replace(gate, delegate + "\n    " + gate)
        world.write_text(text)
        with self.assertRaisesRegex(checker.HookCheckFailed, "ORDER VIOLATION"):
            checker.check_engine(engine.root)

    def test_missing_exposure_commit_fails(self):
        engine = self.fixture()
        unreal = engine.root / checker.UNREAL_CPP
        unreal.write_text(unreal.read_text().replace(
            "if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;",
            "if (!UnrelatedCommit(World, &Error)) return false;"))
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "exactly one exposure commit"):
            checker.check_engine(engine.root)

    def test_missing_seamless_exposure_commit_fails(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        world.write_text(world.read_text().replace(
            "if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;",
            "if (!UnrelatedCommit(World, &Error)) return false;"))
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "exactly one exposure commit"):
            checker.check_engine(engine.root)

    def test_exposure_commit_before_context_publication_fails(self):
        engine = self.fixture()
        unreal = engine.root / checker.UNREAL_CPP
        text = unreal.read_text()
        commit = "if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;"
        text = text.replace(commit, "")
        text = text.replace("GWorld=NewWorld;", "    " + commit + "\n    GWorld=NewWorld;")
        unreal.write_text(text)
        with self.assertRaisesRegex(checker.HookCheckFailed, "ORDER VIOLATION"):
            checker.check_engine(engine.root)

    def test_exposure_commit_after_initialization_fails(self):
        engine = self.fixture()
        unreal = engine.root / checker.UNREAL_CPP
        text = unreal.read_text()
        commit = "if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;"
        text = text.replace(commit, "")
        text = text.replace("WorldContext.World()->InitWorld();",
                            "WorldContext.World()->InitWorld();\n    " + commit)
        unreal.write_text(text)
        with self.assertRaisesRegex(checker.HookCheckFailed, "ORDER VIOLATION"):
            checker.check_engine(engine.root)

    def test_seamless_exposure_commit_before_gworld_fails(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        text = world.read_text()
        commit = "if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;"
        text = text.replace(commit, "")
        text = text.replace("GWorld=LoadedWorld;",
                            "    " + commit + "\n    GWorld=LoadedWorld;")
        world.write_text(text)
        with self.assertRaisesRegex(checker.HookCheckFailed, "ORDER VIOLATION"):
            checker.check_engine(engine.root)

    def test_seamless_exposure_commit_after_initialization_fails(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        text = world.read_text()
        commit = "if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;"
        text = text.replace(commit, "")
        text = text.replace("LoadedWorld->InitWorld();",
                            "LoadedWorld->InitWorld();\n    " + commit)
        world.write_text(text)
        with self.assertRaisesRegex(checker.HookCheckFailed, "ORDER VIOLATION"):
            checker.check_engine(engine.root)

    def test_ignoring_a_false_exposure_commit_fails_open(self):
        engine = self.fixture()
        unreal = engine.root / checker.UNREAL_CPP
        unreal.write_text(unreal.read_text().replace(
            "if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;",
            "WoTGuardedStartup_CommitWorldExposure(World, &Error);"))
        with self.assertRaisesRegex(checker.HookCheckFailed, "FAIL-OPEN COMMIT"):
            checker.check_engine(engine.root)

    def test_ignoring_a_false_seamless_exposure_commit_fails_open(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        world.write_text(world.read_text().replace(
            "if (!WoTGuardedStartup_CommitWorldExposure(World, &Error)) return false;",
            "WoTGuardedStartup_CommitWorldExposure(World, &Error);"))
        with self.assertRaisesRegex(checker.HookCheckFailed, "FAIL-OPEN COMMIT"):
            checker.check_engine(engine.root)

    def test_each_named_runtime_path_is_required(self):
        engine = self.fixture(omitted_identity="RemoteClientSeamlessTravel")
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "UNCOVERED PATH.*RemoteClientSeamlessTravel"):
            checker.check_engine(engine.root)

    def test_comments_cannot_spoof_a_missing_path_identity(self):
        engine = self.fixture(omitted_identity="RemoteClientSeamlessTravel")
        world = engine.root / checker.WORLD_CPP
        world.write_text(world.read_text().replace(
            "UWorld* FSeamlessTravelHandler::Tick()",
            "// RemoteClientSeamlessTravel\nUWorld* FSeamlessTravelHandler::Tick()"))
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "UNCOVERED PATH.*RemoteClientSeamlessTravel"):
            checker.check_engine(engine.root)

    def test_a_new_observer_call_site_fails_even_when_an_identical_one_is_covered(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        world.write_text(world.read_text() + "\nvoid NewPath(){ CreateAISystem(); }\n")
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "CALL-SITE INVENTORY CHANGED.*uncovered/new"):
            checker.check_engine(engine.root)

    def test_a_new_kind_of_begin_play_call_site_fails(self):
        engine = self.fixture()
        unreal = engine.root / checker.UNREAL_CPP
        unreal.write_text(unreal.read_text() +
                          "\nvoid SixthPath(){ SurpriseWorld->BeginPlay(); }\n")
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "CALL-SITE INVENTORY CHANGED"):
            checker.check_engine(engine.root)

    def test_a_removed_or_changed_pinned_call_site_fails(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        world.write_text(world.read_text().replace("LoadedWorld->BeginPlay();", ""))
        with self.assertRaisesRegex(checker.HookCheckFailed, "missing/changed"):
            checker.check_engine(engine.root)

    def test_a_hook_outside_the_guarded_build_definition_fails(self):
        engine = self.fixture(include_macro=False)
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "not guarded by WOT_GUARDED_BOOTSTRAP"):
            checker.check_engine(engine.root)

    def test_duplicate_materialization_on_a_path_fails(self):
        engine = self.fixture(second_hook=True)
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "exactly one materialization.*found 2"):
            checker.check_engine(engine.root)

    def test_ignoring_a_false_materialization_result_fails_open(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        world.write_text(world.read_text().replace(
            "if (!WoTGuardedStartup_MaterializeWorld(World, Path0, &Error)) return false;",
            "WoTGuardedStartup_MaterializeWorld(World, Path0, &Error);"))
        with self.assertRaisesRegex(checker.HookCheckFailed, "FAIL-OPEN HOOK"):
            checker.check_engine(engine.root)

    def test_a_missing_engine_source_file_fails_closed(self):
        engine = self.fixture()
        (engine.root / checker.WORLD_CPP).unlink()
        with self.assertRaisesRegex(checker.HookCheckFailed, "cannot read engine source"):
            checker.check_engine(engine.root)

    def test_the_enum_order_is_part_of_the_contract(self):
        engine = self.fixture()
        header = engine.root / checker.CONTRACT_HEADER
        header.write_text(header.read_text().replace(
            "    ListenHostTravel,\n    RemoteClientTravel,",
            "    RemoteClientTravel,\n    ListenHostTravel,"))
        with self.assertRaisesRegex(checker.HookCheckFailed,
                                    "enumerator names/order changed"):
            checker.check_engine(engine.root)

    def test_the_contract_header_cannot_grow_a_second_declaration(self):
        engine = self.fixture()
        header = engine.root / checker.CONTRACT_HEADER
        header.write_text(header.read_text() + "\nstruct FLaunchLayerLeak {};\n")
        with self.assertRaisesRegex(checker.HookCheckFailed, "contain only the enum"):
            checker.check_engine(engine.root)

    def test_engine_sources_must_not_reach_into_launch_for_the_enum(self):
        engine = self.fixture()
        world = engine.root / checker.WORLD_CPP
        world.write_text(world.read_text().replace(
            '#include "WoTWorldMaterialization.h"',
            '#include "../../Launch/Public/WoTGuardedEngineLoop.h"'))
        with self.assertRaisesRegex(checker.HookCheckFailed, "ENGINE LAYERING"):
            checker.check_engine(engine.root)

    def test_cli_reuses_source_provenance_and_accepts_the_synthetic_git_tree(self):
        engine = self.fixture()
        with mock.patch.object(patcher.source_verifier.platform, "system",
                               return_value="Darwin"):
            self.assertEqual(0, checker.main([
                "--engine-manifest", str(engine.patch_manifest),
                "--engine", str(engine.root),
            ]))

    def test_cli_resolves_engine_from_local_configuration(self):
        engine = self.fixture()
        with mock.patch.object(patcher.source_verifier.platform, "system",
                               return_value="Darwin"), \
                mock.patch.object(checker.platform, "system",
                                  return_value="Darwin"), \
                mock.patch.object(checker, "source_engine_root",
                                  return_value=engine.root):
            self.assertEqual(0, checker.main([
                "--engine-manifest", str(engine.patch_manifest),
            ]))

    def test_the_frozen_inventory_is_the_real_ue58_inventory_size(self):
        self.assertEqual(sum(checker.PINNED_INVENTORY.values()), 40)
        self.assertEqual(len(checker.PATH_IDENTITIES), 5)


if __name__ == "__main__":
    unittest.main()
