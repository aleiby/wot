"""Reconcile retail AngrealSpawnPool defaults into already-imported MP maps.

The plan half is stdlib-only and folds T3D instance deltas over the exported
UnrealScript class defaults through spawn_pool_defaults.py. The editor half
re-stamps existing actors and places pools that the former authored-only import
incorrectly omitted. Re-runs reconcile by import_map.py's stable pool label.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_spawn_pools.py [<Map> ...]"

Completion details are written to Saved/import_spawn_pools_result.json because
commandlet stdout and unreal.log are not reliable completion signals.
"""
import json
import sys
from collections import Counter
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import MP_MAPS as _KNOWN_MP_MAPS, T3D_DIR  # noqa: E402
from spawn_pool_defaults import (  # noqa: E402
    POOL_CLASSES,
    pool_entries,
    pool_unported,
)
from t3d import parse, typed  # noqa: E402


REPO = _HERE.parents[1]
RESULT_FILE = REPO / "Saved" / "import_spawn_pools_result.json"
MP_MAPS = [name for name in _KNOWN_MP_MAPS
           if (T3D_DIR / f"{name}.t3d").is_file()]


def plan_map(map_name):
    """Return effective pool stamps for every retail pool instance."""
    stamps = []
    for actor in typed(parse(T3D_DIR / f"{map_name}.t3d")):
        cls = actor["cls"]
        if cls not in POOL_CLASSES:
            continue
        props = actor.get("props", {})
        stamps.append({
            "label": f"{cls}_{actor['name']}",
            "class": cls,
            "name": actor["name"],
            "location": actor["location"],
            "rotation": actor["rotation"],
            "entries": pool_entries(props, cls),
            "unported": pool_unported(props, cls),
        })
    return stamps


def _rotator(unreal, rotation):
    scale = 360.0 / 65536.0
    return unreal.Rotator(roll=rotation[2] * scale,
                          pitch=rotation[0] * scale,
                          yaw=rotation[1] * scale)


def _matching_pool_actors(unreal, actors, label):
    return [actor for actor in actors
            if isinstance(actor, unreal.WoTAngrealSpawnPool)
            and actor.get_actor_label() == label]


def _load_importer_helpers():
    """Import editor helpers without treating patcher args as importer args."""
    saved_argv = sys.argv
    try:
        sys.argv = [str(_HERE / "import_map.py")]
        from import_map import POOL_RESPAWN, _pickup_class
    finally:
        sys.argv = saved_argv
    return POOL_RESPAWN, _pickup_class


def _stamp_map(unreal, eas, plan, pool_respawn, pickup_class):
    actors = list(eas.get_all_level_actors())
    counts = {
        "planned": len(plan),
        "restamped": 0,
        "newly_placed": 0,
        "still_empty": 0,
        "unported_skips": 0,
    }
    unported_classes = Counter()
    resolution_errors = []

    for stamp in plan:
        for name, _weight in stamp["unported"]:
            unported_classes[name] += 1
        matches = _matching_pool_actors(unreal, actors, stamp["label"])
        if not stamp["entries"]:
            for actor in matches:
                eas.destroy_actor(actor)
                actors.remove(actor)
            counts["still_empty"] += 1
            unreal.log(f"SPAWNPOOL-PATCH empty: {stamp['label']}")
            continue

        pool = []
        weights = []
        for ue5, weight in stamp["entries"]:
            cls_obj = pickup_class(ue5)
            if cls_obj is None:
                resolution_errors.append(f"{stamp['label']}:{ue5}")
                continue
            pool.append(cls_obj)
            weights.append(weight)
        if len(pool) != len(stamp["entries"]):
            unreal.log_error(
                f"SPAWNPOOL-PATCH {stamp['label']}: unresolved pickup class")
            continue

        actor = matches[0] if matches else None
        for duplicate in matches[1:]:
            eas.destroy_actor(duplicate)
            actors.remove(duplicate)
        if actor is None:
            actor = eas.spawn_actor_from_class(
                unreal.WoTAngrealSpawnPool,
                unreal.Vector(*stamp["location"]))
            if actor is None:
                resolution_errors.append(f"{stamp['label']}:spawn failed")
                continue
            actors.append(actor)
            actor.set_actor_label(stamp["label"])
            actor.set_actor_location(unreal.Vector(*stamp["location"]),
                                     False, False)
            actor.set_actor_rotation(_rotator(unreal, stamp["rotation"]), False)
            actor.set_editor_property(
                "respawn_time", pool_respawn[stamp["class"]])
            actor.set_editor_property("respawn", True)
            counts["newly_placed"] += 1
        else:
            counts["restamped"] += 1
        actor.set_editor_property("pool", pool)
        actor.set_editor_property("weight", weights)

    counts["unported_skips"] = sum(unported_classes.values())
    counts["unported_classes"] = dict(sorted(unported_classes.items()))
    return counts, resolution_errors


def main():
    import unreal

    maps = [name for name in sys.argv[1:] if not name.startswith("-")] \
        or MP_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    results = {}
    failed = []

    try:
        pool_respawn, pickup_class = _load_importer_helpers()
    except Exception as exc:  # noqa: BLE001 - commandlet must leave a result
        # Same zero-initialized schema the per-map path uses, so a setup
        # failure still yields the full count layout consumers expect.
        for map_name in maps:
            results[map_name] = {
                "status": "FAIL",
                "planned": 0,
                "restamped": 0,
                "newly_placed": 0,
                "still_empty": 0,
                "unported_skips": 0,
                "unported_classes": {},
                "missing_after_run": [],
                "saved": False,
                "error": str(exc),
            }
        RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
        RESULT_FILE.write_text(
            json.dumps(results, indent=2) + "\n", encoding="utf-8")
        raise RuntimeError(
            f"spawn-pool patch setup failed; see {RESULT_FILE}: {exc}") from exc

    for map_name in maps:
        result = {
            "status": "FAIL",
            "planned": 0,
            "restamped": 0,
            "newly_placed": 0,
            "still_empty": 0,
            "unported_skips": 0,
            "unported_classes": {},
            "missing_after_run": [],
            "saved": False,
        }
        results[map_name] = result
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            result["error"] = "no imported map"
            failed.append(map_name)
            unreal.log_error(f"SPAWNPOOL-PATCH {map_name}: no imported map")
            continue
        if not les.load_level(asset):
            result["error"] = "load_level failed"
            failed.append(map_name)
            unreal.log_error(f"SPAWNPOOL-PATCH {map_name}: load_level failed")
            continue

        try:
            plan = plan_map(map_name)
            counts, resolution_errors = _stamp_map(
                unreal, eas, plan, pool_respawn, pickup_class)
            result.update(counts)
            expected = {stamp["label"] for stamp in plan if stamp["entries"]}
            after = list(eas.get_all_level_actors())
            present = {
                actor.get_actor_label() for actor in after
                if isinstance(actor, unreal.WoTAngrealSpawnPool)
            }
            missing = sorted(expected - present)
            result["missing_after_run"] = missing
            if resolution_errors:
                result["resolution_errors"] = resolution_errors
            result["saved"] = bool(les.save_current_level())
            if not missing and not resolution_errors and result["saved"]:
                result["status"] = "PASS"
            else:
                failed.append(map_name)
            classes = ", ".join(
                f"{name}:{count}"
                for name, count in result["unported_classes"].items()) or "none"
            unreal.log(
                f"SPAWNPOOL-PATCH {map_name} {result['status']}: "
                f"restamped={result['restamped']} "
                f"newly_placed={result['newly_placed']} "
                f"still_empty={result['still_empty']} "
                f"unported_skips={result['unported_skips']} "
                f"classes={classes} missing={len(missing)}")
        except Exception as exc:  # noqa: BLE001 - persist failure before abort
            result["error"] = str(exc)
            failed.append(map_name)
            unreal.log_error(f"SPAWNPOOL-PATCH {map_name}: {exc}")

    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULT_FILE.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    if failed:
        raise RuntimeError(
            "spawn-pool patch failed for " + ", ".join(sorted(set(failed)))
            + f"; see {RESULT_FILE}")


if __name__ == "__main__":
    main()
