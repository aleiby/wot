#!/usr/bin/env python3
"""Inventory UE 5.8 packaged world-start observers and their guarded hooks.

This is intentionally a pinned-source checker, not a general C++ parser.  Every
observer call site in the two UE 5.8 translation units is recorded below.  A new
call site therefore fails as an unknown inventory entry instead of silently
becoming a sixth route around materialization.
"""
from __future__ import annotations

import argparse
import platform
import re
import sys
from collections import Counter
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_DIR))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import apply_engine_patch as patch_tool
from wot_config import ConfigurationError, source_engine_root

WORLD_CPP = "Engine/Source/Runtime/Engine/Private/World.cpp"
UNREAL_CPP = "Engine/Source/Runtime/Engine/Private/UnrealEngine.cpp"
CONTRACT_HEADER = "Engine/Source/Runtime/Engine/Public/WoTWorldMaterialization.h"
HOOK = "WoTGuardedStartup_MaterializeWorld"
COMMIT_HOOK = "WoTGuardedStartup_CommitWorldExposure"
PATH_IDENTITIES = (
    "NormalInitialLoad",
    "ListenHostTravel",
    "RemoteClientTravel",
    "ListenHostSeamlessTravel",
    "RemoteClientSeamlessTravel",
)


class HookCheckFailed(Exception):
    """The source inventory or materialization order is not complete."""


def _norm(line: str) -> str:
    line = re.sub(r"/\*.*?\*/", "", line)
    line = line.split("//", 1)[0]
    return re.sub(r"\s+", "", line).rstrip(";")


# Exact semantic statements from pristine 5.8.0-release.  Null detach/cleanup
# operations are excluded because they do not expose a candidate world.  The
# editor initialization and fatal-error dummy world are retained in the frozen
# inventory (and explicitly marked out of the two packaged success choke points)
# so an engine bump cannot alter them unnoticed either.
PINNED_INVENTORY = Counter({
    # World.cpp: observer implementations reached only after one of the gates.
    (WORLD_CPP, "physics", "CreatePhysicsScene(WorldSettings)"): 1,
    (WORLD_CPP, "navigation", "FNavigationSystem::AddNavigationSystemToWorld(*this,FNavigationSystemRunMode::InvalidMode,WorldSettings->GetNavigationSystemConfig(),false)"): 1,
    (WORLD_CPP, "ai", "CreateAISystem()"): 1,
    (WORLD_CPP, "actors", "AISystem->InitializeActorsForPlay(bResetTime)"): 1,
    (WORLD_CPP, "replication", "NetDriver->SetWorld(this)"): 1,
    (WORLD_CPP, "replication", "SourceCollection->SetNetDriver(NetDriver)"): 1,
    (WORLD_CPP, "replication", "StaticCollection->SetNetDriver(NetDriver)"): 1,
    (WORLD_CPP, "replication", "LoadedWorld->SetNetDriver(NetDriver)"): 1,
    (WORLD_CPP, "replication", "LoadedCollection->SetNetDriver(NetDriver)"): 1,
    (WORLD_CPP, "replication", "LoadedStaticCollection->SetNetDriver(NetDriver)"): 1,
    (WORLD_CPP, "replication", "NetDriver->SetWorld(LoadedWorld)"): 1,
    (WORLD_CPP, "replication-gateway", "CopyWorldData()"): 1,
    (WORLD_CPP, "world-exposure", "CurrentContext.SetCurrentWorld(LoadedWorld)"): 1,
    (WORLD_CPP, "world-exposure", "GWorld=LoadedWorld"): 1,
    (WORLD_CPP, "initialization-gateway", "LoadedWorld->InitWorld()"): 1,
    (WORLD_CPP, "initialization-gateway", "InitWorld(IVS)"): 2,
    (WORLD_CPP, "ai", "LoadedWorld->CreateAISystem()"): 1,
    (WORLD_CPP, "actors", "LoadedWorld->InitializeActorsForPlay(PendingTravelURL,false)"): 1,
    (WORLD_CPP, "navigation", "FNavigationSystem::AddNavigationSystemToWorld(*LoadedWorld,FNavigationSystemRunMode::GameMode)"): 1,
    (WORLD_CPP, "play", "LoadedWorld->BeginPlay()"): 1,

    # UnrealEngine.cpp: two known non-success/editor sites plus LoadMap and its
    # remote-client helper.  They remain inventoried even where they are outside
    # the packaged success paths checked for ordering below.
    (UNREAL_CPP, "world-exposure", "InitialWorldContext.SetCurrentWorld(UWorld::CreateWorld(EWorldType::Editor,true))"): 1,
    (UNREAL_CPP, "world-exposure", "GWorld=InitialWorldContext.World()"): 1,
    (UNREAL_CPP, "world-exposure", "Context.SetCurrentWorld(UWorld::CreateWorld(Context.WorldType,false))"): 1,
    (UNREAL_CPP, "world-exposure", "GWorld=NewWorld"): 1,
    (UNREAL_CPP, "world-exposure", "WorldContext.SetCurrentWorld(NewWorld)"): 1,
    (UNREAL_CPP, "initialization-gateway", "WorldContext.World()->InitWorld()"): 1,
    (UNREAL_CPP, "replication-gateway", "MovePendingLevel(WorldContext)"): 1,
    (UNREAL_CPP, "replication-gateway", "if(!WorldContext.World()->Listen(URL))"): 1,
    (UNREAL_CPP, "ai", "WorldContext.World()->CreateAISystem()"): 1,
    (UNREAL_CPP, "actors", "WorldContext.World()->InitializeActorsForPlay(URL,true,&Context)"): 1,
    (UNREAL_CPP, "navigation", "FNavigationSystem::AddNavigationSystemToWorld(*WorldContext.World(),FNavigationSystemRunMode::GameMode)"): 1,
    (UNREAL_CPP, "play", "WorldContext.World()->BeginPlay()"): 1,
    (UNREAL_CPP, "replication", "Context.World()->SetNetDriver(Context.PendingNetGame->NetDriver)"): 1,
    (UNREAL_CPP, "replication", "NetDriver->SetWorld(Context.World())"): 1,
    (UNREAL_CPP, "replication", "SourceLevels.SetNetDriver(NetDriver)"): 1,
    (UNREAL_CPP, "replication", "StaticLevels->SetNetDriver(NetDriver)"): 1,
    (UNREAL_CPP, "replication", "DemoNetDriver->SetWorld(Context.World())"): 1,
    (UNREAL_CPP, "replication", "Context.World()->SetDemoNetDriver(DemoNetDriver)"): 1,
    (UNREAL_CPP, "replication", "MainLevels.SetDemoNetDriver(DemoNetDriver)"): 1,
})


def _classify(line: str) -> str | None:
    compact = _norm(line)
    if not compact or compact.startswith("//"):
        return None
    # Function declarations and fluent InitializationValues setters are not
    # call sites.  Null operations only remove exposure.
    if re.match(r"^(?:UAISystemBase\*|void)UWorld::(?:CreateAISystem|CreatePhysicsScene|InitializeActorsForPlay|BeginPlay|InitWorld)", compact):
        return None
    if "CreatePhysicsScene(" in compact and ".CreatePhysicsScene(" not in compact:
        return "physics"
    if "AddNavigationSystemToWorld(" in compact:
        return "navigation"
    if "CreateAISystem(" in compact and ".CreateAISystem(" not in compact:
        return "ai"
    if "InitializeActorsForPlay(" in compact:
        return "actors"
    if re.search(r"\bBeginPlay\(", compact):
        return "play"
    if re.search(r"(?:->|\.)Set(?:Demo)?NetDriver\(", compact):
        if re.search(r"Set(?:Demo)?NetDriver\((?:NULL|nullptr)\)", compact):
            return None
        return "replication"
    if "->SetWorld(" in compact:
        if re.search(r"SetWorld\((?:NULL|nullptr)\)", compact):
            return None
        return "replication"
    if compact == "CopyWorldData()":
        return "replication-gateway"
    if compact == "MovePendingLevel(WorldContext)":
        return "replication-gateway"
    if "WorldContext.World()->Listen(URL)" in compact:
        return "replication-gateway"
    if re.search(r"\bInitWorld\(", compact):
        return "initialization-gateway"
    if re.search(r"(?:^|[^=!<>])GWorld=(?![=])", compact):
        if compact.endswith("=NULL") or compact.endswith("=nullptr"):
            return None
        return "world-exposure"
    if ".SetCurrentWorld(" in compact:
        if re.search(r"SetCurrentWorld\((?:NULL|nullptr)\)", compact):
            return None
        return "world-exposure"
    return None


def inventory_file(relative: str, text: str) -> Counter:
    found: Counter = Counter()
    in_block_comment = False
    for raw in text.splitlines():
        line = raw
        if in_block_comment:
            if "*/" not in line:
                continue
            line = line.split("*/", 1)[1]
            in_block_comment = False
        while "/*" in line:
            before, after = line.split("/*", 1)
            if "*/" in after:
                line = before + after.split("*/", 1)[1]
            else:
                line = before
                in_block_comment = True
                break
        kind = _classify(line)
        if kind:
            found[(relative, kind, _norm(line))] += 1
    return found


def _mask_cpp(text: str) -> str:
    """Blank comments and literals while preserving offsets for brace matching."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    return pattern.sub(lambda match: re.sub(r"[^\n]", " ", match.group(0)), text)


def function_body(text: str, signature: str) -> tuple[str, int]:
    start = text.find(signature)
    if start < 0:
        raise HookCheckFailed(f"missing pinned function {signature}")
    masked = _mask_cpp(text)
    opening = masked.find("{", start)
    if opening < 0:
        raise HookCheckFailed(f"missing body for {signature}")
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index], opening + 1
    raise HookCheckFailed(f"unterminated body for {signature}")


def _require_gate(text: str, signature: str, first_observer: str,
                  identities: tuple[str, ...],
                  required_prior_observers: tuple[str, ...] = ()) -> None:
    body, _ = function_body(text, signature)
    masked_body = _mask_cpp(body)
    hook_positions = [match.start() for match in re.finditer(
        rf"\b{HOOK}\s*\(", masked_body)]
    if len(hook_positions) != 1:
        raise HookCheckFailed(
            f"{signature} must contain exactly one materialization transition; found {len(hook_positions)}")
    observer_match = re.search(first_observer, masked_body)
    if observer_match is None:
        raise HookCheckFailed(f"{signature} lost pinned earliest observer {first_observer}")
    observer = observer_match.start()
    if hook_positions[0] >= observer:
        raise HookCheckFailed(
            f"ORDER VIOLATION: {signature} reaches {first_observer} before materialization")
    for required in required_prior_observers:
        required_match = re.search(required, masked_body)
        if required_match is None:
            raise HookCheckFailed(
                f"{signature} lost required destructive observer {required}")
        if hook_positions[0] >= required_match.start():
            raise HookCheckFailed(
                f"ORDER VIOLATION: {signature} reaches {required} before materialization")
    hook_prefix = masked_body[max(0, hook_positions[0] - 32):hook_positions[0]]
    if not re.search(r"if\s*\(\s*!\s*$", hook_prefix):
        raise HookCheckFailed(
            f"FAIL-OPEN HOOK: {signature} does not branch on a false materialization result")
    prefix = masked_body[:hook_positions[0]]
    guarded = list(re.finditer(r"#\s*if\s+WOT_GUARDED_BOOTSTRAP\b", prefix))
    if not guarded or "#endif" in prefix[guarded[-1].end():]:
        raise HookCheckFailed(f"{signature} materialization call is not guarded by WOT_GUARDED_BOOTSTRAP")
    for identity in identities:
        if not re.search(rf"\b{re.escape(identity)}\b", prefix):
            raise HookCheckFailed(f"UNCOVERED PATH: {signature} does not classify {identity}")


def _require_load_exposure_commit(text: str) -> None:
    signature = "bool UEngine::LoadMap("
    body, _ = function_body(text, signature)
    masked = _mask_cpp(body)
    commits = list(re.finditer(rf"\b{COMMIT_HOOK}\s*\(", masked))
    if len(commits) != 1:
        raise HookCheckFailed(
            f"{signature} must contain exactly one exposure commit; found {len(commits)}")
    positions = []
    for label, pattern in (
            ("materialization", rf"\b{HOOK}\s*\("),
            ("GWorld exposure", r"GWorld\s*=\s*NewWorld"),
            ("context exposure", r"WorldContext\s*\.\s*SetCurrentWorld\s*\(\s*NewWorld\s*\)"),
            ("exposure commit", rf"\b{COMMIT_HOOK}\s*\("),
            ("initialization", r"WorldContext\s*\.\s*World\s*\(\s*\)\s*->\s*InitWorld\s*\(")):
        match = re.search(pattern, masked)
        if match is None:
            raise HookCheckFailed(f"{signature} lost pinned {label}")
        positions.append(match.start())
    if positions != sorted(positions) or len(set(positions)) != len(positions):
        raise HookCheckFailed(
            "ORDER VIOLATION: LoadMap must materialize, expose GWorld/context, "
            "commit readiness, then initialize")
    commit_prefix = masked[max(0, commits[0].start() - 32):commits[0].start()]
    if not re.search(r"if\s*\(\s*!\s*$", commit_prefix):
        raise HookCheckFailed(
            "FAIL-OPEN COMMIT: LoadMap does not branch on a false exposure commit")
    prefix = masked[:commits[0].start()]
    guarded = list(re.finditer(r"#\s*if\s+WOT_GUARDED_BOOTSTRAP\b", prefix))
    if not guarded or "#endif" in prefix[guarded[-1].end():]:
        raise HookCheckFailed(
            "LoadMap exposure commit is not guarded by WOT_GUARDED_BOOTSTRAP")


def _require_seamless_exposure_commit(text: str) -> None:
    signature = "UWorld* FSeamlessTravelHandler::Tick("
    body, _ = function_body(text, signature)
    masked = _mask_cpp(body)
    commits = list(re.finditer(rf"\b{COMMIT_HOOK}\s*\(", masked))
    if len(commits) != 1:
        raise HookCheckFailed(
            f"{signature} must contain exactly one exposure commit; found {len(commits)}")
    positions = []
    for label, pattern in (
            ("materialization", rf"\b{HOOK}\s*\("),
            ("context exposure",
             r"CurrentContext\s*\.\s*SetCurrentWorld\s*\(\s*LoadedWorld\s*\)"),
            ("GWorld exposure", r"GWorld\s*=\s*LoadedWorld"),
            ("exposure commit", rf"\b{COMMIT_HOOK}\s*\("),
            ("initialization", r"LoadedWorld\s*->\s*InitWorld\s*\(")):
        match = re.search(pattern, masked)
        if match is None:
            raise HookCheckFailed(f"{signature} lost pinned {label}")
        positions.append(match.start())
    if positions != sorted(positions) or len(set(positions)) != len(positions):
        raise HookCheckFailed(
            "ORDER VIOLATION: seamless travel must materialize, expose the "
            "context/GWorld, commit readiness, then initialize")
    commit_prefix = masked[max(0, commits[0].start() - 32):commits[0].start()]
    if not re.search(r"if\s*\(\s*!\s*$", commit_prefix):
        raise HookCheckFailed(
            "FAIL-OPEN COMMIT: seamless travel does not branch on a false exposure commit")
    prefix = masked[:commits[0].start()]
    guarded = list(re.finditer(r"#\s*if\s+WOT_GUARDED_BOOTSTRAP\b", prefix))
    if not guarded or "#endif" in prefix[guarded[-1].end():]:
        raise HookCheckFailed(
            "seamless exposure commit is not guarded by WOT_GUARDED_BOOTSTRAP")


def _require_contract_header(text: str) -> None:
    masked = _mask_cpp(text)
    definitions = list(re.finditer(
        r"\benum\s+class\s+EWoTWorldStartPath\s*:\s*uint8\s*\{(.*?)\}\s*;",
        masked, re.S))
    if len(definitions) != 1:
        raise HookCheckFailed(
            f"WORLD PATH CONTRACT: expected one EWoTWorldStartPath definition; found {len(definitions)}")
    enumerators = [item.strip() for item in definitions[0].group(1).split(",")
                   if item.strip()]
    if enumerators != list(PATH_IDENTITIES):
        raise HookCheckFailed(
            "WORLD PATH CONTRACT: enumerator names/order changed: "
            f"{enumerators or 'none'}")
    remainder = masked[:definitions[0].start()] + masked[definitions[0].end():]
    remainder = re.sub(r"^\s*#.*$", "", remainder, flags=re.MULTILINE)
    if remainder.strip():
        raise HookCheckFailed(
            "WORLD PATH CONTRACT: WoTWorldMaterialization.h must contain only the enum")


def _require_engine_layering(relative: str, text: str) -> None:
    include_marked = re.sub(
        r'^\s*#\s*include\s+"WoTWorldMaterialization\.h"\s*$',
        "WOT_WORLD_CONTRACT_INCLUDE", text, flags=re.MULTILINE)
    if _mask_cpp(include_marked).count("WOT_WORLD_CONTRACT_INCLUDE") != 1:
        raise HookCheckFailed(
            f"ENGINE LAYERING: {relative} must include WoTWorldMaterialization.h exactly once")
    if "WoTGuardedEngineLoop.h" in text or "../../Launch/" in text:
        raise HookCheckFailed(
            f"ENGINE LAYERING: {relative} still reaches into the Launch module")
    linkage_marked = re.sub(r'extern\s+"C"', "extern C_LINKAGE", text)
    masked = _mask_cpp(linkage_marked)
    declaration = re.compile(
        rf'extern\s+C_LINKAGE\s+bool\s+{HOOK}\s*\(\s*'
        r'UWorld\s*\*\s*World\s*,\s*EWoTWorldStartPath\s+Path\s*,\s*'
        r'FString\s*\*\s*Error\s*\)\s*;')
    if len(declaration.findall(masked)) != 1:
        raise HookCheckFailed(
            f"ENGINE CONTRACT: {relative} must own exactly one local C hook declaration")
    commit_declaration = re.compile(
        rf'extern\s+C_LINKAGE\s+bool\s+{COMMIT_HOOK}\s*\(\s*'
        r'UWorld\s*\*\s*World\s*,\s*FString\s*\*\s*Error\s*\)\s*;')
    if len(commit_declaration.findall(masked)) != 1:
        raise HookCheckFailed(
            f"ENGINE CONTRACT: {relative} must own exactly one local C exposure-commit declaration")


def check_engine(engine: Path) -> int:
    texts = {}
    actual: Counter = Counter()
    for relative in (WORLD_CPP, UNREAL_CPP, CONTRACT_HEADER):
        path = engine / relative
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise HookCheckFailed(f"cannot read engine source {path}: {exc}") from exc
        texts[relative] = text
        if relative != CONTRACT_HEADER:
            actual.update(inventory_file(relative, text))
    if actual != PINNED_INVENTORY:
        new = list((actual - PINNED_INVENTORY).elements())
        missing = list((PINNED_INVENTORY - actual).elements())
        raise HookCheckFailed(
            "CALL-SITE INVENTORY CHANGED: uncovered/new="
            f"{new or 'none'}; missing/changed={missing or 'none'}")

    _require_contract_header(texts[CONTRACT_HEADER])
    _require_engine_layering(UNREAL_CPP, texts[UNREAL_CPP])
    _require_engine_layering(WORLD_CPP, texts[WORLD_CPP])
    _require_gate(
        texts[UNREAL_CPP], "bool UEngine::LoadMap(", r"GWorld\s*=\s*NewWorld",
        PATH_IDENTITIES[:3])
    _require_load_exposure_commit(texts[UNREAL_CPP])
    _require_gate(
        texts[WORLD_CPP], "UWorld* FSeamlessTravelHandler::Tick(",
        r"CurrentWorld\s*->\s*BeginTearingDown\s*\(\s*\)",
        PATH_IDENTITIES[3:], (
            r"FWorldDelegates\s*::\s*OnSeamlessTravelTransition\s*\.\s*Broadcast\s*\(\s*CurrentWorld\s*\)",
            r"CopyWorldData\s*\(\s*\)",
        ))
    _require_seamless_exposure_commit(texts[WORLD_CPP])
    return sum(actual.values())


def _resolve_engine(manifest_path: Path, document: dict, explicit: Path | None) -> Path:
    if explicit is not None:
        return explicit
    lane = {"Darwin": "Mac", "Windows": "Win64"}.get(platform.system())
    if lane is None:
        raise HookCheckFailed(
            "no --engine was supplied and this host has no source-engine lane")
    try:
        root = source_engine_root(lane)
    except ConfigurationError as exc:
        raise HookCheckFailed(str(exc)) from exc
    assert root is not None
    return root


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-manifest", required=True, type=Path,
                        help="the guarded engine patch manifest")
    parser.add_argument("--engine", type=Path,
                        help="source-engine root (otherwise resolved from local configuration)")
    args = parser.parse_args(argv)
    try:
        document, records = patch_tool.load_manifest(
            args.engine_manifest, allow_unrecorded=True)
        engine = _resolve_engine(args.engine_manifest, document, args.engine)
        source = patch_tool.verify_pinned_engine(engine, document, args.engine_manifest)
        patch_tool.verify_manifest_agrees_with_source(records, source)
        count = check_engine(engine)
    except (HookCheckFailed, patch_tool.PatchRejected) as exc:
        print(f"WORLD START HOOKS REJECTED: {exc}", file=sys.stderr)
        return 1
    print(f"WORLD START HOOKS VERIFIED: 5 paths, {count} pinned observer call sites")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
