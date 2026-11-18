#!/usr/bin/env python3
"""Deterministic ordered /Content rebuild orchestrator.

Run directly with ``--dry-run`` to inspect the plan without Unreal Engine, or
run the script once through UnrealEditor-Cmd's PythonScript commandlet.  The
individual importers execute in this process via ``runpy`` so Unreal starts
only once and failures retain the logical step and item name.

``bake_map.py`` is deliberately not part of this plan: it is an asynchronous
GPU Lightmass driver which quits the editor and needs a RenderOffscreen
session.  The canonical maps use the decoded UE1 lightmap atlases instead.
``smoke.py`` is a diagnostic, not a content producer.
"""
from __future__ import annotations

import argparse
import os
import runpy
import sys
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, Sequence


REPO = Path(__file__).resolve().parents[2]
IMPORT_DIR = REPO / "Tools" / "import"
CONVERT_DIR = REPO / "Tools" / "convert"
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import exports_dir, original_dir  # noqa: E402

MP_MAPS = (
    *(f"Arena_{i:02d}" for i in (1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12)),
    *(f"Battle2_{i:02d}" for i in range(1, 6)),
    "Battle3_01", "Battle3_02", "Battle4_01",
)

# AbsorbVisual (fins) and the AMA pickup (a box) also carry bMeshCurvy but
# are not spheres, so they stay flat.
CURVY_MESHES = (
    "AMApearl",
    "AngrealShieldPickup",
)

# Shared with Tools/dist/bootstrap.py.  These are the System packages known to
# contain Sound exports; the other 27 sound packages live in Sounds/*.uax.
SYSTEM_SOUND_PACKAGES = (
    "Angreal", "Environment", "Fire", "IpServer", "Legend", "MachinShin",
    "ParticleSystems", "UBrowser", "UWindow", "WOT", "WoTBase",
    "WoTBrowser", "WoTDecorations", "WoTPawns", "WoTTraps",
)


@dataclass(frozen=True)
class StepSpec:
    name: str
    script: str
    summary: str
    inputs: tuple[str, ...]
    outputs: tuple[str, ...]
    aliases: tuple[str, ...] = ()
    note: str = ""


# This table is intentionally data, not prose hidden in main().  The bootstrap
# imports it (and MP_MAPS) when displaying the engine-side half of its plan.
IMPORT_STEP_SPECS = (
    StepSpec(
        "bootstrap-map", "generate_bootstrap_map.py", "Create the dedicated payload-free guarded bootstrap world.",
        ("/Script/WoT.WoTBootstrapGameMode",),
        ("/Game/WoT/Maps/WoTBootstrap",),
        aliases=("bootstrap", "generate-bootstrap-map"),
        note="No-replace generation targets this project's Content root only through the explicit allow flag.",
    ),
    StepSpec(
        "sounds", "import_sounds.py", "Import every extracted WAV, force PCM cook, and derive looping flags.",
        ("{exports}/sounds/<Pkg>/*.wav", "{exports}/script/**/*.uc", "{exports}/t3d/*.t3d"),
        ("/Game/WoT/Sounds/<Pkg>/<Name>",),
        aliases=("sound", "import-sounds"),
    ),
    StepSpec(
        "flipbooks", "import_flipbooks.py", "Create the fractal live masters/MICs and the runtime fractal catalog before material consumers (sheet lane retired).",
        ("{conv}/flipbooks/<Pkg>/_flipbooks.json",),
        ("/Game/WoT/Materials/Instances/MI_<Tex>",
         "/Game/WoT/Materials/M_WoT_FractalLive*", "/Game/WoT/Data/DA_WoTFractalCatalog"),
        aliases=("flipbook", "import-flipbooks"),
    ),
    StepSpec(
        "foundation", "import_map.py", "Seed one map to create the shared world, mesh, and particle masters.",
        ("{conv}/<first-map>.bsp.json", "{conv}/<first-map>.glb", "{conv}/tex/**"),
        ("/Game/WoT/Materials/M_WoT_*", "/Game/WoT/Maps/<first-map> (provisional)"),
        aliases=("seed", "masters"),
        note="The seed map is rebuilt after meshes/pickups; this breaks the legacy clean-build master/mesh cycle.",
    ),
    StepSpec(
        "sky-window-material", "sky_window_material.py",
        "Generate the opaque view-direction cube sampler used by runtime FakeBackdrop windows.",
        ("/Engine/EngineResources/DefaultTextureCube",),
        ("/Game/WoT/Materials/M_WoT_SkyWindow",),
        aliases=("sky-window", "generate-sky-window-material"),
    ),
    StepSpec(
        "sky-layer-materials", "sky_layer_materials.py",
        "Duplicate the Entry SceneBlend master into the modulate/add passes that stack translucent child-sky layers.",
        ("/Game/WoT/Materials/M_WoT_EntryTranslucent_SceneBlend",),
        ("/Game/WoT/Materials/M_WoT_EntrySkyModulate", "/Game/WoT/Materials/M_WoT_EntrySkyAdd"),
        aliases=("sky-layers", "generate-sky-layer-materials"),
    ),
    StepSpec(
        "mover-wave-runtime", "bootstrap_mover_wave_instances.py",
        "Compile the six-slot world-response permutations used by runtime static movers.",
        ("/Game/WoT/Materials/M_WoT_WorldWave*",),
        ("/Game/WoT/Materials/MI_WoT_MoverWaveRuntime*",),
        aliases=("mover-wave", "bootstrap-mover-wave-instances"),
    ),
    StepSpec(
        "meshes", "import_mesh.py", "Rebuild retail vertex-animation GLBs, import them, and bind their skins.",
        ("{retail}/System/*.u", "{exports}/script/**/*.uc", "{conv}/tex/<Pkg>/*.png"),
        ("/Game/WoT/Meshes/<Mesh>", "/Game/WoT/Materials/Instances/MI_<Skin>"),
        aliases=("mesh", "import-mesh"),
        note="Reconstructs all James pairs and GLBs from the retail packages first.",
    ),
    StepSpec(
        "animdata", "import_animdata.py", "Build the animation-sequence/notifies data table from mesh metadata.",
        ("{conv}/mesh/*.meta.json",),
        ("/Game/WoT/Anim/DT_WoTAnimSequences",),
        aliases=("animations", "import-animdata"),
    ),
    StepSpec(
        "weave-sprites", "import_weave_sprites.py", "Import authored weave and impact sprite series.",
        ("{conv}/tex/Angreal/*.png",),
        ("/Game/WoT/Textures/Angreal/<Sprite>",),
        aliases=("weave", "import-weave-sprites"),
    ),
    StepSpec(
        "pickups", "import_pickups.py", "Generate asset-binding pickup leaf Blueprints after their meshes exist.",
        ("/Game/WoT/Meshes/<PickupMesh>", "{exports}/script/Angreal/*.uc"),
        ("/Game/WoT/Blueprints/Pickups/BP_*",),
        aliases=("pickup", "import-pickups"),
    ),
    StepSpec(
        "angreal-hands", "import_angreal_hands.py", "Build the shipped angreal hand-order table.",
        ("{repo}/Tools/import/data/angreal_hands.csv",),
        ("/Game/WoT/Data/DT_WoTAngrealHands",),
        aliases=("hands", "import-angreal-hands"),
    ),
    StepSpec(
        "hud-fonts", "import_hud_fonts.py", "Build offline UE5 fonts from the decoded WOT.u font atlases.",
        ("{retail}/System/WOT.u", "{conv}/fonts/_manifest.json"),
        ("/Game/WoT/UI/Fonts",),
        aliases=("fonts", "import-hud-fonts"),
    ),
    StepSpec(
        "hud-textures", "import_hud_textures.py", "Import HUD/scoreboard textures and build their registry.",
        ("{conv}/tex/WOT/_manifest.json", "{conv}/tex/WoTBase/_manifest.json"),
        ("/Game/WoT/UI/DA_WoTHUDTextures", "/Game/WoT/UI/<HUDTexture>"),
        aliases=("hud", "import-hud-textures"),
        note="SB-D10: existing textures and payload bindings are saved only when a setting or binding differs.",
    ),
    StepSpec(
        "maps", "import_map.py", "Rebuild all selected MP maps with meshes, pickups, movers, zones, lighting, and child skies.",
        ("{conv}/<Map>.bsp.json", "{conv}/<Map>.glb", "{conv}/<Map>.t3d.json",
         "{conv}/<Map>_lmatlas.*", "{conv}/movers/<Map>/movers.json",
         "{exports}/{{fog,leaves,zones}}/<Map>.*.json"),
        ("/Game/WoT/Maps/<Map>", "/Game/WoT/Maps/Geo/<Map>"),
        aliases=("map", "import-map"),
    ),
    StepSpec(
        "trap-skins", "import_trap_skins.py", "Bind trap stage and beam skin material instances.",
        ("/Game/WoT/Materials/M_WoT_Master", "{conv}/tex/{{WoTTraps,Angreal}}/*.png"),
        ("/Game/WoT/Materials/Instances/MI_TrapWall*", "/Game/WoT/Materials/Instances/MI_LB_A*"),
        aliases=("traps", "import-trap-skins"),
    ),
    StepSpec(
        "gore-fx", "import_gore_fx.py", "Import blood, scorch, and decal textures/materials after particle masters exist.",
        ("{conv}/tex/{{ParticleSystems,Angreal,WoTPawns}}/*.png", "/Game/WoT/Materials/M_WoT_ParticleMod"),
        ("/Game/WoT/Materials/Instances/MI_PSMod_*", "/Game/WoT/Materials/Instances/MI_Decal_*"),
        aliases=("gore", "import-gore-fx"),
    ),
    StepSpec(
        "flame-fx", "import_flame_fx.py", "Build flipbook/static particle materials for traps and weave effects.",
        ("{conv}/flipbooks/**", "{conv}/tex/**", "/Game/WoT/Materials/M_WoT_Particle*"),
        ("/Game/WoT/Materials/M_WoT_Particle*FB", "/Game/WoT/Materials/Instances/MI_*FB_*"),
        aliases=("flames", "import-flame-fx"),
    ),
    StepSpec(
        "strip-payload-textures", "strip_payload_textures.py", "Replace every payload-bound retail Texture2D source with a dimension-matched placeholder.",
        ("/Game/WoT/** (Texture2D registry scan)", "/Script/WoT.WoTPayloadAssetUserData"),
        ("/Game/WoT/** payload-bound Texture2D sources (patched in place)",),
        aliases=("strip-textures", "finalize-payload-textures"),
        note="Stripped payload mode only; /Game/WoT/Textures/Flipbooks is defensively excluded.",
    ),
    StepSpec(
        "ambience", "import_ambience.py", "Patch imported maps with ambient emitters, mover sounds, and decoration animation.",
        ("{exports}/t3d/<Map>.t3d", "{exports}/script/**/*.uc", "/Game/WoT/Sounds/**"),
        ("/Game/WoT/Maps/<Map> (patched in place)",),
        aliases=("import-ambience",),
    ),
    StepSpec(
        "water-zones", "import_water_zones.py", "Stamp water-zone entry/exit sound bindings.",
        ("{exports}/t3d/<Map>.t3d", "/Game/WoT/Sounds/**"),
        ("/Game/WoT/Maps/<Map> (patched in place)",),
        aliases=("water", "import-water-zones"),
    ),
    StepSpec(
        "gameplay-look", "import_gameplay_look.py", "Converge the final flat-sRGB gameplay post-process volume on every map.",
        ("/Game/WoT/Maps/<Map>",),
        ("/Game/WoT/Maps/<Map> (patched in place)",),
        aliases=("look", "import-gameplay-look"),
    ),
    StepSpec(
        "frontend-map", "import_frontend_map.py", "Create the code-driven front-end map last (skip if it already exists).",
        ("/Script/WoT.WoTFrontEndGameMode",),
        ("/Game/WoT/Maps/WoTFrontEnd",),
        aliases=("frontend", "import-frontend-map"),
        note="Skip-if-exists avoids nondeterministic world-save GUID/date churn; use the leaf script's --force explicitly when needed.",
    ),
)


class ImportStepError(RuntimeError):
    """A named logical import step failed."""


def _normal(name: str) -> str:
    value = name.strip().lower().replace("_", "-")
    if value.startswith("import-"):
        value = value[len("import-"):]
    return value


def _step_names() -> dict[str, str]:
    result: dict[str, str] = {}
    for step in IMPORT_STEP_SPECS:
        for name in (step.name, step.script.removesuffix(".py"), *step.aliases):
            result[_normal(name)] = step.name
    return result


def _split_options(values: Iterable[str]) -> set[str]:
    aliases = _step_names()
    result: set[str] = set()
    unknown: list[str] = []
    for raw in values:
        for item in raw.split(","):
            if not item.strip():
                continue
            key = _normal(item)
            if key not in aliases:
                unknown.append(item)
            else:
                result.add(aliases[key])
    if unknown:
        raise ValueError(
            f"unknown step(s): {', '.join(unknown)}; choices: "
            + ", ".join(step.name for step in IMPORT_STEP_SPECS))
    return result


def _format(text: str, args: argparse.Namespace) -> str:
    return text.format(
        repo=args.repo,
        conv=args.conv_dir,
        exports=args.exports_dir,
        retail=args.retail_root,
    )


def _mesh_metadata(script_root: Path) -> dict:
    """Load the one #exec-derived mesh census shared by conversion/import."""
    added = str(CONVERT_DIR) not in sys.path
    if added:
        sys.path.insert(0, str(CONVERT_DIR))
    try:
        from ue3d import parse_exec_metadata
        return parse_exec_metadata(script_root)
    finally:
        if added:
            sys.path.remove(str(CONVERT_DIR))


def _mesh_names(script_root: Path) -> list[str]:
    metadata = _mesh_metadata(script_root)
    return sorted((item["name"] for item in metadata.values()), key=str.lower)


def print_plan(args: argparse.Namespace, selected: set[str]) -> None:
    try:
        mesh_count: object = len(_mesh_names(args.exports_dir / "script"))
    except (FileNotFoundError, KeyError, OSError, ValueError):
        mesh_count = "<discovered at execution>"
    print("WoT ordered /Content rebuild plan (no engine launched)")
    print(f"  repo:       {args.repo}")
    print(f"  retail:     {args.retail_root}")
    print(f"  exports:    {args.exports_dir}")
    print(f"  converted:  {args.conv_dir}")
    print(f"  maps:       {len(args.maps)} ({', '.join(args.maps)})")
    print(f"  meshes:     {mesh_count}")
    print(f"  mesh source: {args.mesh_source}")
    print(f"  payload mode: {args.payload_mode}")
    print("  mesh GLBs:  " + ("stripped carriers enabled"
          if args.payload_mode == "stripped" else "canonical carrier-free"))
    print("  session:    one UnrealEditor-Cmd PythonScript session")
    for index, step in enumerate(IMPORT_STEP_SPECS, 1):
        state = "RUN " if step.name in selected else "SKIP"
        print(f"\n{index:02d}. [{state}] {step.name} — {step.summary}")
        print(f"    script:  Tools/import/{step.script}")
        for item in step.inputs:
            print(f"    input:   {_format(item, args)}")
        for item in step.outputs:
            print(f"    output:  {_format(item, args)}")
        if step.note:
            print(f"    note:    {step.note}")
    print("\nExcluded by design:")
    print("  bake-map — GPU Lightmass is asynchronous, quits the editor, and is superseded by decoded UE1 lightmap atlases.")
    print("  smoke    — commandlet diagnostic only; it produces no /Content assets.")


def _log(message: str, *, error: bool = False) -> None:
    unreal = sys.modules.get("unreal")
    if unreal is not None:
        fn = getattr(unreal, "log_error" if error else "log", None)
        if fn:
            fn(message)
            return
    print(message, file=sys.stderr if error else sys.stdout, flush=True)


def _path_arg(path: Path) -> str:
    # import_sounds historically detects its root by a slash in argv[1].
    return path.resolve().as_posix()


def mesh_mapping_carriers_requested(environ=None) -> bool:
    """Use carriers only inside an explicitly selected stripped workspace."""
    environ = os.environ if environ is None else environ
    return (environ.get("WOT_MESH_IMPORT_MODE", "").casefold() == "stripped"
            or environ.get("WOT_PAYLOAD_MODE", "").casefold() == "stripped")


def bsp_stripped_maps_requested(environ=None) -> bool:
    """Select split-aware BSP carriers in the composed stripped workspace."""
    environ = os.environ if environ is None else environ
    return environ.get("WOT_PAYLOAD_MODE", "").casefold() == "stripped"


def _mesh_glb_has_mapping_carriers(path: Path) -> bool:
    added = str(CONVERT_DIR) not in sys.path
    if added:
        sys.path.insert(0, str(CONVERT_DIR))
    try:
        from gltf import skinned_glb_has_mapping_carriers
        return skinned_glb_has_mapping_carriers(path)
    finally:
        if added:
            sys.path.remove(str(CONVERT_DIR))


def _validate_mesh_outputs(conv_mesh: Path, names: Sequence[str],
                           expected_source: Optional[str] = None,
                           expected_carriers: Optional[bool] = None) -> None:
    import json

    for name in names:
        glb = conv_mesh / f"{name}.glb"
        meta = conv_mesh / f"{name}.meta.json"
        if not glb.is_file():
            raise FileNotFoundError(f"missing converted mesh GLB: {glb}")
        if not meta.is_file():
            raise FileNotFoundError(f"missing converted mesh metadata: {meta}")
        metadata = json.loads(meta.read_text())
        if expected_source is not None:
            source = metadata.get("source")
            if source != expected_source:
                raise ValueError(
                    f"{meta} records source {source!r}, expected {expected_source!r}")
        if expected_carriers is not None:
            actual_carriers = _mesh_glb_has_mapping_carriers(glb)
            recorded_carriers = bool(metadata.get("mapping_carriers", False))
            if actual_carriers != expected_carriers:
                state = "with" if actual_carriers else "without"
                expected = "with" if expected_carriers else "without"
                raise ValueError(
                    f"{glb} is {state} mapping carriers; expected {expected}")
            if recorded_carriers != actual_carriers:
                raise ValueError(
                    f"{meta} carrier mode disagrees with {glb}")


def _prepare_mesh_inputs(args: argparse.Namespace) -> list[str]:
    """Build mode-correct James/GLB inputs before the Unreal leaf importer."""
    from class2mesh import write_class_to_mesh
    from mesh2glb import (RETAIL_SOURCE, convert, mesh_names,
                          reconstruct_retail_james)

    script_root = args.exports_dir.resolve() / "script"
    metadata = _mesh_metadata(script_root)
    names = mesh_names(metadata)
    if not names:
        raise ValueError(f"no #exec mesh metadata under {script_root}")
    conv = args.conv_dir.resolve()
    conv_mesh = conv / "mesh"
    carriers = mesh_mapping_carriers_requested()
    curvy_names = [f"{name}_Curvy" for name in CURVY_MESHES]

    if args.mesh_source == "prepared":
        _validate_mesh_outputs(
            conv_mesh, names, expected_carriers=carriers)
        _validate_mesh_outputs(
            conv_mesh, curvy_names, expected_carriers=False)
        return names + curvy_names

    mapping = write_class_to_mesh(conv / "class2mesh.json", script_root)
    known = {name.casefold() for name in names}
    unknown = sorted({name for name in mapping.values()
                      if name.casefold() not in known}, key=str.lower)
    if unknown:
        raise ValueError(
            "class2mesh references meshes outside the #exec census: "
            + ", ".join(unknown))

    if args.mesh_source == RETAIL_SOURCE:
        models_dir = conv / "retail-meshes"
        reconstruct_retail_james(
            metadata, models_dir, args.retail_root.resolve(), names)
        model_dirs = [models_dir]
        source = RETAIL_SOURCE
    else:
        raise ValueError(f"unknown mesh source: {args.mesh_source}")

    carrier_mode = "carriers" if carriers else "canonical-carrier-free"
    _log(f"IMPORT-ALL PREP meshes/{source}/{carrier_mode}: {len(names)} GLB(s)")
    for index, name in enumerate(names, 1):
        _log(f"IMPORT-ALL CONVERT meshes [{index}/{len(names)}] {name}")
        convert(name, conv_mesh, meta_all=metadata, model_dirs=model_dirs,
                source=source, script_root=script_root, carriers=carriers)
    for index, name in enumerate(CURVY_MESHES, 1):
        _log(f"IMPORT-ALL CONVERT curvy meshes "
             f"[{index}/{len(CURVY_MESHES)}] {name}")
        convert(name, conv_mesh, meta_all=metadata, model_dirs=model_dirs,
                source=source, script_root=script_root, carriers=False,
                curvy_levels=2)
    _validate_mesh_outputs(
        conv_mesh, names, source, expected_carriers=carriers)
    _validate_mesh_outputs(
        conv_mesh, curvy_names, source, expected_carriers=False)
    return names + curvy_names


def _invocations(step: StepSpec, args: argparse.Namespace) -> list[tuple[str, list[str]]]:
    conv = args.conv_dir.resolve()
    exports = args.exports_dir.resolve()
    retail = args.retail_root.resolve()
    first_map = args.maps[0]
    common_map = lambda name: [name, _path_arg(conv), args.map_mode]
    stripped = args.payload_mode == "stripped"

    if step.name == "bootstrap-map":
        unreal = sys.modules.get("unreal")
        if unreal is None:
            raise RuntimeError("bootstrap-map generation requires Unreal")
        content = Path(unreal.Paths.convert_relative_path_to_full(
            unreal.Paths.project_content_dir())).resolve()
        saved = Path(unreal.Paths.convert_relative_path_to_full(
            unreal.Paths.project_saved_dir())).resolve()
        evidence = saved / "ImportReceipts"
        return [("canonical bootstrap world", [
            "--output-root", _path_arg(content),
            "--denied-read-open-receipt", _path_arg(evidence / "bootstrap-map-denied-read-open.json"),
            "--output-identity-receipt", _path_arg(evidence / "bootstrap-map-output-identity.json"),
            "--require-empty-output", "--no-replace", "--allow-project-content",
        ])]

    if step.name == "sounds":
        suffix = (["--stripped", "--dest", "/Game/WoT/Sounds",
                   "--allow-canonical"] if stripped else [])
        return [("all packages", [_path_arg(exports / "sounds"), *suffix])]
    if step.name == "flipbooks":
        suffix = ["--stripped"] if stripped else []
        return [("all sheets", [_path_arg(conv / "flipbooks"), *suffix])]
    if step.name == "foundation":
        # In composed stripped mode the foundation still has to build the full
        # map/material structure. --bsp-stripped makes import_map suppress its
        # texture-only interpretation of WOT_PAYLOAD_MODE; the maps step later
        # overwrites every used texture with a stripped canonical-path asset.
        suffix = ["--bsp-stripped"] if stripped else []
        return [(first_map, [*common_map(first_map), *suffix])]
    if step.name in ("sky-window-material", "sky-layer-materials"):
        return [("master", ["--output-root", "/Game/WoT/Materials"])]
    if step.name == "meshes":
        names = getattr(args, "_mesh_names", None)
        if names is None:
            names = _mesh_names(exports / "script")
        _validate_mesh_outputs(conv / "mesh", names)
        suffix = (["--stripped", "--dest", "/Game/WoT/Meshes",
                   "--allow-canonical"] if stripped else [])
        return [(name, [name, _path_arg(conv / "mesh"),
                        _path_arg(conv / "tex"), *suffix]) for name in names]
    if step.name == "animdata":
        suffix = ["--stripped"] if stripped else []
        return [("sequence table", [_path_arg(conv / "mesh"), *suffix])]
    if step.name in {"weave-sprites", "gore-fx", "flame-fx"}:
        return [("all", [_path_arg(conv)])]
    if step.name == "pickups":
        return [("all", [])]
    if step.name == "angreal-hands":
        return [("hand table", [_path_arg(IMPORT_DIR / "data" / "angreal_hands.csv")])]
    if step.name == "hud-fonts":
        suffix = (["--stripped", "--dest", "/Game/WoT/UI/Fonts",
                   "--allow-canonical"] if stripped else [])
        return [("fonts", [_path_arg(retail), _path_arg(conv / "fonts"), *suffix])]
    if step.name == "hud-textures":
        suffix = (["--stripped", "--dest", "/Game/WoT/UI",
                   "--allow-canonical"] if stripped else [])
        return [("textures", [_path_arg(retail), _path_arg(conv / "tex"), *suffix])]
    if step.name == "maps":
        if not stripped:
            return [(name, common_map(name)) for name in args.maps]
        # import_map's BSP mode is deliberately a complete map build, while
        # its texture mode deliberately returns before touching the map. Run
        # those compatible leaf contracts in order inside the disposable
        # project: structure/BSP carriers, then canonical-path placeholders.
        result = []
        for name in args.maps:
            result.append((f"{name} structure+BSP",
                           [*common_map(name), "--bsp-stripped"]))
            result.append((f"{name} textures", [
                *common_map(name), "--stripped", "--dest",
                "/Game/WoT/Textures", "--allow-canonical",
            ]))
        return result
    if step.name in {"ambience", "water-zones", "gameplay-look"}:
        return [(f"{len(args.maps)} maps", list(args.maps))]
    if step.name == "trap-skins":
        return [("all", [_path_arg(conv / "tex")])]
    if step.name in {"frontend-map", "mover-wave-runtime"}:
        return [("all", [])]
    if step.name == "strip-payload-textures":
        if not stripped:
            return []
        return [("all payload-bound Texture2Ds", [])]
    raise AssertionError(f"no invocation adapter for {step.name}")


def _run_script(step: StepSpec, label: str, argv: Sequence[str]) -> None:
    script = IMPORT_DIR / step.script
    saved_argv = sys.argv
    sys.argv = [str(script), *argv]
    try:
        runpy.run_path(str(script), run_name="__main__")
    except SystemExit as exc:
        if exc.code not in (None, 0):
            raise ImportStepError(
                f"step '{step.name}' item '{label}' exited with {exc.code}") from exc
    except Exception as exc:
        raise ImportStepError(
            f"step '{step.name}' item '{label}' failed: {exc}") from exc
    finally:
        sys.argv = saved_argv


def execute(args: argparse.Namespace, selected: set[str]) -> None:
    # The individual importers resolve these portable roots.  Restore the
    # caller's environment because import_all can also be imported by tooling.
    env = {
        "WOT_RETAIL_ROOT": str(args.retail_root.resolve()),
        "WOT_EXPORTS_DIR": str(args.exports_dir.resolve()),
        "WOT_CONV_DIR": str(args.conv_dir.resolve()),
        "WOT_PAYLOAD_MODE": args.payload_mode,
    }
    saved_env = {key: os.environ.get(key) for key in env}
    saved_path = list(sys.path)
    os.environ.update(env)
    for path in (str(IMPORT_DIR), str(CONVERT_DIR), str(REPO)):
        if path not in sys.path:
            sys.path.insert(0, path)

    completed = 0
    try:
        if "meshes" in selected:
            args._mesh_names = _prepare_mesh_inputs(args)
        for step in IMPORT_STEP_SPECS:
            if step.name not in selected:
                continue
            try:
                invocations = _invocations(step, args)
            except Exception as exc:
                raise ImportStepError(
                    f"step '{step.name}' could not resolve its inputs: {exc}") from exc
            _log(f"IMPORT-ALL STEP {step.name}: {len(invocations)} invocation(s)")
            for index, (label, argv) in enumerate(invocations, 1):
                _log(f"IMPORT-ALL RUN {step.name} [{index}/{len(invocations)}] {label}")
                _run_script(step, label, argv)
            completed += 1
            _log(f"IMPORT-ALL OK {step.name}")
    except Exception:
        _log("IMPORT-ALL FAILED\n" + traceback.format_exc(), error=True)
        raise
    finally:
        sys.path[:] = saved_path
        for key, value in saved_env.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
    _log(f"IMPORT-ALL COMPLETE steps={completed}/{len(selected)}")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true",
                        help="print the resolved ordered plan without importing unreal")
    parser.add_argument("--only", action="append", default=[], metavar="STEP",
                        help="run only this step (repeat or use comma-separated names)")
    parser.add_argument("--skip", action="append", default=[], metavar="STEP",
                        help="skip this step (repeat or use comma-separated names)")
    parser.add_argument("--conv-dir", type=Path, default=REPO / "Intermediate" / "Conv")
    parser.add_argument("--exports-dir", type=Path)
    parser.add_argument("--retail-root", type=Path)
    parser.add_argument("--payload-mode", choices=("full", "stripped"),
                        default="full",
                        help="build canonical payloads (default) or guarded "
                             "canonical-path placeholders in a disposable project")
    parser.add_argument(
        "--mesh-source", choices=("retail-package", "prepared"),
        default="retail-package",
        help="mesh conversion source; 'prepared' is reserved for the bootstrap's "
             "already-converted work tree")
    parser.add_argument("--map-mode", choices=("preview", "baked"), default="preview")
    parser.add_argument("--maps", nargs="+", default=list(MP_MAPS), metavar="MAP",
                        help="map subset for map and post-map steps (default: 19 MP maps)")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    args.exports_dir = args.exports_dir or exports_dir()
    args.retail_root = args.retail_root or original_dir()
    args.repo = REPO
    try:
        only = _split_options(args.only)
        skip = _split_options(args.skip)
    except ValueError as exc:
        parser.error(str(exc))
    known_maps = set(MP_MAPS)
    unknown_maps = [name for name in args.maps if name not in known_maps]
    if unknown_maps:
        parser.error(f"unknown MP map(s): {', '.join(unknown_maps)}")
    if not args.maps:
        parser.error("--maps must contain at least one map")
    selected = ({step.name for step in IMPORT_STEP_SPECS} if not only else only) - skip
    if args.payload_mode != "stripped":
        selected.discard("strip-payload-textures")
    else:
        # Guarded retail content gets neutral semantic carriers. The animation
        # table and cooked offline-font metrics are developer oracles, not
        # release inputs; their first-use parsers read cache-v3 objects.
        selected.difference_update({"animdata", "hud-fonts"})
    if args.dry_run:
        print_plan(args, selected)
        return 0
    if not selected:
        _log("IMPORT-ALL COMPLETE steps=0/0 (all steps filtered)")
        return 0
    execute(args, selected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
