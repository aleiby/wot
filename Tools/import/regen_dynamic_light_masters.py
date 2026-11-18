"""Regenerate every G45 dynamic-light master graph in place (no map import).

The master builders are tag-versioned (DYNAMIC_LIGHT_GRAPH_TAG /
_ML_MOVER_GRAPH_VERSION / LIGHT_GRAPH_VERSION): calling them rebuilds only
masters whose stored tag mismatches, so this commandlet is idempotent and
safe to rerun after any graph-version bump.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script=".../regen_dynamic_light_masters.py" -unattended -nullrhi -stdout

Writes Saved/regen_dynamic_light_masters_result.json (commandlet stdout is
unreliable; the result file is the verification surface).
"""

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
REPO = HERE.parents[1]
RESULT_FILE = REPO / "Saved" / "regen_dynamic_light_masters_result.json"

import unreal  # noqa: E402

import import_map  # noqa: E402  (CLI is __main__-guarded; import is inert)
import mesh_master_bootstrap  # noqa: E402
from dynamic_light_material import (  # noqa: E402
    DYNAMIC_LIGHT_COLLECTION_PATH,
    DYNAMIC_LIGHT_GRAPH_TAG,
    DYNAMIC_LIGHT_GRAPH_VERSION,
    ensure_dynamic_light_collection,
)


def main():
    result = {"ok": False, "steps": {}}

    ensure_dynamic_light_collection(unreal)
    result["steps"]["mpc"] = unreal.EditorAssetLibrary.does_asset_exist(
        DYNAMIC_LIGHT_COLLECTION_PATH)

    world = import_map.make_world_masters()
    result["steps"]["world_masters"] = sorted(
        m.get_name() for m in world.values())

    mesh_translucent = import_map.make_mesh_translucent_master()
    result["steps"]["mesh_translucent"] = mesh_translucent.get_name()

    import_map.make_moverlit_masters()
    result["steps"]["mover_masters"] = True

    mesh = mesh_master_bootstrap.ensure_mesh_masters(unreal)
    result["steps"]["mesh_masters"] = sorted(mesh.keys())

    # Conversion-side child-sky scene-blend masters (tag-gated like the rest;
    # setup_childsky's own runs also self-heal them).
    sys.path.insert(0, str(REPO / "Tools" / "convert"))
    try:
        import setup_childsky
        setup_childsky.scene_blend_master(animated=False)
        setup_childsky.scene_blend_master(animated=True)
        result["steps"]["scene_blend_masters"] = True
    except Exception as error:  # recorded, not swallowed: ok stays False
        result["steps"]["scene_blend_masters"] = f"FAILED: {error}"

    # Verify the tag landed on every atlas-consuming world master.
    eal = unreal.EditorAssetLibrary
    tags = {}
    for name in (
            "M_WoT_World", "M_WoT_WorldMasked", "M_WoT_WorldWave",
            "M_WoT_WorldWaveMasked", "M_WoT_WorldTranslucent",
            "M_WoT_WorldModulated", "M_WoT_WorldWaveTranslucent",
            "M_WoT_WorldWaveModulated"):
        path = f"/Game/WoT/Materials/{name}"
        if not eal.does_asset_exist(path):
            tags[name] = "MISSING"
            continue
        tags[name] = str(eal.get_metadata_tag(
            unreal.load_asset(path), DYNAMIC_LIGHT_GRAPH_TAG))
    result["steps"]["world_tags"] = tags
    result["steps"]["mesh_translucent_tag"] = str(eal.get_metadata_tag(
        mesh_translucent, import_map._MESH_TRANSLUCENT_GRAPH_TAG))
    result["ok"] = all(
        v == DYNAMIC_LIGHT_GRAPH_VERSION for v in tags.values()) and (
        result["steps"]["scene_blend_masters"] is True) and (
        result["steps"]["mesh_translucent_tag"] ==
        import_map._MESH_TRANSLUCENT_GRAPH_VERSION)

    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULT_FILE.write_text(json.dumps(result, indent=2))


main()
