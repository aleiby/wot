"""Replace inert AndilayRoot decorations with functional pickup actors.

The plan half is stdlib-only and carries every authored transform,
RespawnTime, CollisionRadius, bMPAppearEffect, and Tag from the 11 MP T3Ds.
The editor half reconciles by the stable retail actor label, places
BP_WoTAndilayRoot (the generated leaf that binds the mesh), and removes the
legacy AWoTDecoration only when BOTH its label is an expected AndilayRoot
author and its bound mesh is AndilayRoot.

Re-runs restamp one pickup per label, remove duplicate pickups/legacy donors,
verify exact per-map counts, save each map once, and write a machine-readable
dump to Saved/import_andilay_roots_result.json.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_andilay_roots.py [<Map> ...]"
"""
import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import T3D_DIR  # noqa: E402
from fixed_pickup_lib import plan_t3d, reconcile_map  # noqa: E402
from t3d import parse, typed  # noqa: E402


ROOT_MAPS = [
    "Arena_01", "Arena_02", "Arena_03", "Arena_05", "Arena_06",
    "Arena_09", "Arena_11", "Arena_12", "Battle2_03", "Battle2_05",
    "Battle4_01",
]
REPO = _HERE.parents[1]
RESULT_FILE = REPO / "Saved" / "import_andilay_roots_result.json"
PICKUP_BP = "/Game/WoT/Blueprints/Pickups/BP_WoTAndilayRoot"


def plan_map(map_name):
    """Return every authored AndilayRoot stamp from one retail T3D."""
    def make_stamp(actor):
        props = actor.get("props", {})
        return {
            "label": actor["name"],
            "location": actor["location"],
            "rotation": actor["rotation"],
            "respawn_time": float(props.get("RespawnTime", 0.0)),
            "collision_radius": float(props.get("CollisionRadius", 10.0)),
            "mp_appear_effect": (
                str(props.get("bMPAppearEffect", "True")).lower() == "true"),
            # Actor.Tag defaults to the class name in UE1. All shipped MP
            # authors export it, but retain the CDO fallback for synthetic/mod maps.
            "tag": props.get("Tag", "AndilayRoot"),
        }
    return plan_t3d(
        T3D_DIR / f"{map_name}.t3d", parse, typed,
        lambda actor: actor["cls"].lower() == "andilayroot", make_stamp)


def _stamp_map(unreal, eas, plan, pickup_class):
    shared_plan = [dict(stamp, class_key="andilayroot",
                        legacy_mesh="AndilayRoot") for stamp in plan]

    def stamp_actor(pickup, stamp):
        pickup.set_editor_property("respawn_time", stamp["respawn_time"])
        pickup.root_component.set_editor_property(
            "sphere_radius", stamp["collision_radius"])
        pickup.set_editor_property(
            "mp_appear_effect", stamp["mp_appear_effect"])
        pickup.set_editor_property(
            "tags", [unreal.Name(stamp["tag"])] if stamp["tag"] else [])
    runtime = {"andilayroot": {
        "base_class": unreal.WoTAndilayRoot,
        "pickup_class": pickup_class,
    }}
    return reconcile_map(unreal, eas, shared_plan, runtime, stamp_actor)


def _load_pickup_class(unreal):
    if not unreal.EditorAssetLibrary.does_asset_exist(PICKUP_BP):
        raise RuntimeError(
            f"missing {PICKUP_BP}; run import_pickups.py after building G38")
    blueprint = unreal.load_asset(PICKUP_BP)
    pickup_class = blueprint.generated_class() if blueprint else None
    if pickup_class is None:
        raise RuntimeError(f"could not resolve generated class for {PICKUP_BP}")
    return pickup_class


def main():
    import unreal

    maps = [name for name in sys.argv[1:] if not name.startswith("-")] \
        or ROOT_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    results = {}
    failed = []

    try:
        pickup_class = _load_pickup_class(unreal)
    except Exception as exc:  # noqa: BLE001 - persist a useful commandlet result
        for map_name in maps:
            results[map_name] = {"status": "FAIL", "error": str(exc)}
        RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
        RESULT_FILE.write_text(json.dumps(results, indent=2) + "\n",
                               encoding="utf-8")
        raise

    for map_name in maps:
        result = {"status": "FAIL", "saved": False}
        results[map_name] = result
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            result["error"] = "no imported map"
            failed.append(map_name)
            continue
        if not les.load_level(asset):
            result["error"] = "load_level failed"
            failed.append(map_name)
            continue
        try:
            plan = plan_map(map_name)
            result.update(_stamp_map(unreal, eas, plan, pickup_class))
            result["saved"] = bool(les.save_current_level())
            if (result["verified_pickups"] == result["planned"]
                    and result["legacy_remaining"] == 0
                    and not result["bad_labels"] and result["saved"]):
                result["status"] = "PASS"
            else:
                failed.append(map_name)
            unreal.log(
                f"ANDILAYROOT {map_name} {result['status']}: "
                f"planned={result['planned']} restamped={result['restamped']} "
                f"new={result['newly_placed']} "
                f"legacy_removed={result['legacy_removed']} "
                f"verified={result['verified_pickups']} "
                f"legacy_remaining={result['legacy_remaining']}")
        except Exception as exc:  # noqa: BLE001 - keep the per-map dump complete
            result["error"] = str(exc)
            failed.append(map_name)

    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULT_FILE.write_text(json.dumps(results, indent=2) + "\n",
                           encoding="utf-8")
    if failed:
        raise RuntimeError(
            "AndilayRoot patch failed for " + ", ".join(sorted(set(failed)))
            + f"; see {RESULT_FILE}")


if __name__ == "__main__":
    main()
