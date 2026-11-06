"""Replace fixed AngrealInv* decoration stand-ins with functional pickups.

The stdlib plan half re-derives its class table from the exported inheritance
graph, the pre-graduation class2mesh mapping, and all 19 shipped MP T3Ds.  The
editor half resolves each generated leaf BP and native port class separately;
a missing dependency skips that class with an explicit result record and never
removes its visual donor.  Re-runs reconcile by stable retail actor label and
delete a legacy AWoTDecoration only when both its expected label and its
class2mesh-derived mesh match.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_fixed_pickups.py [<Map> ...]"
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))
sys.path.insert(0, str(_HERE.parents[0]))

from class2mesh import class_to_mesh, inventory_derived_classes  # noqa: E402
from fixed_pickup_lib import plan_t3d, reconcile_map  # noqa: E402
from import_ambience import T3D_DIR  # noqa: E402
from t3d import parse, typed  # noqa: E402
from wot_config import exports_dir  # noqa: E402


REPO = _HERE.parents[1]
SCRIPT_ROOT = exports_dir(required=False) / "script"
PICKUP_CONTENT = REPO / "Content" / "WoT" / "Blueprints" / "Pickups"
RESULT_FILE = REPO / "Saved" / "import_fixed_pickups_result.json"
MP_MAPS = [
    "Arena_01", "Arena_02", "Arena_03", "Arena_04", "Arena_05",
    "Arena_06", "Arena_07", "Arena_08", "Arena_09", "Arena_11",
    "Arena_12", "Battle2_01", "Battle2_02", "Battle2_03",
    "Battle2_04", "Battle2_05", "Battle3_01", "Battle3_02",
    "Battle4_01",
]

_SOUND = re.compile(r"Sound'([^'.]+)\.(?:[^'.]+\.)?([^'.]+)'")


def _bool(value):
    return str(value).lower() == "true"


def _sound_asset(value):
    """Sound'Pkg.Group.Name' -> imported soft-object path."""
    match = _SOUND.search(value or "")
    if not match:
        return ""
    package, name = match.groups()
    return f"/Game/WoT/Sounds/{package}/{name}.{name}"


def make_class_record(retail_class, legacy_mesh, content_dir=PICKUP_CONTENT):
    """Derive the native/BP names and perform the pure disk-side BP check."""
    port_class = f"WoT{retail_class}"
    bp_name = f"BP_{port_class}"
    bp_file = Path(content_dir) / f"{bp_name}.uasset"
    record = {
        "retail_class": retail_class,
        "port_class": port_class,
        "bp_name": bp_name,
        "bp_asset": f"/Game/WoT/Blueprints/Pickups/{bp_name}",
        "bp_file": str(bp_file),
        "legacy_mesh": legacy_mesh,
        "status": "READY",
    }
    if not bp_file.is_file():
        record["status"] = "SKIP"
        record["reason"] = f"missing generated leaf BP on disk: {bp_file}"
    return record


def derive_class_table(t3d_dir=T3D_DIR, script_root=SCRIPT_ROOT,
                       content_dir=PICKUP_CONTENT, maps=MP_MAPS):
    """Derive the table of fixed Inventory classes authored in shipped MP."""
    raw_meshes = class_to_mesh(script_root, functional_pickup_imports=set())
    inventory_classes = inventory_derived_classes(script_root)
    candidates = (set(raw_meshes) & inventory_classes) - {"andilayroot"}
    authored = {}
    for map_name in maps:
        for actor in typed(parse(Path(t3d_dir) / f"{map_name}.t3d")):
            key = actor["cls"].lower()
            if key in candidates:
                authored.setdefault(key, actor["cls"])
    return {
        key: make_class_record(authored[key], raw_meshes[key], content_dir)
        for key in sorted(authored)
    }


CLASS_TABLE = derive_class_table()
FIXED_MAPS = [
    map_name for map_name in MP_MAPS
    if any(actor["cls"].lower() in CLASS_TABLE
           for actor in typed(parse(T3D_DIR / f"{map_name}.t3d")))
]


def _make_stamp(actor, record):
    props = actor.get("props", {})
    overrides = {}
    if "RespawnTime" in props:
        overrides["respawn_time"] = float(props["RespawnTime"])
    if "MinInitialCharges" in props:
        overrides["min_initial_charges"] = int(props["MinInitialCharges"])
    if "MaxInitialCharges" in props:
        overrides["max_initial_charges"] = int(props["MaxInitialCharges"])
    sp_charges = {
        index: int(props[f"SPInitialCharges({index})"])
        for index in range(3) if f"SPInitialCharges({index})" in props
    }
    if sp_charges:
        overrides["sp_initial_charges"] = sp_charges
    if "RespawnSound" in props:
        overrides["respawn_sound"] = _sound_asset(props["RespawnSound"])
    # Supported even though the shipped census has zero such authors.
    if "bMPAppearEffect" in props:
        overrides["mp_appear_effect"] = _bool(props["bMPAppearEffect"])
    if "CollisionRadius" in props:
        overrides["collision_radius"] = float(props["CollisionRadius"])

    unsupported = {}
    if "bDynamicLight" in props:
        # UE1 Actor.bDynamicLight has no equivalent port UPROPERTY.  Preserve
        # the finding in the plan/result instead of silently pretending it was
        # stamped or widening an unrelated presentation surface.
        unsupported["bDynamicLight"] = props["bDynamicLight"]
    return {
        "label": actor["name"],
        "class_key": actor["cls"].lower(),
        "retail_class": record["retail_class"],
        "port_class": record["port_class"],
        "bp_name": record["bp_name"],
        "legacy_mesh": record["legacy_mesh"],
        "location": actor["location"],
        "rotation": actor["rotation"],
        "tag": props.get("Tag", record["retail_class"]),
        "overrides": overrides,
        "unsupported_overrides": unsupported,
    }


def plan_map(map_name, class_table=None, t3d_dir=None):
    """Return every functional fixed-pickup stamp from one retail MP T3D."""
    class_table = CLASS_TABLE if class_table is None else class_table
    t3d_dir = T3D_DIR if t3d_dir is None else Path(t3d_dir)

    def accepts(actor):
        return actor["cls"].lower() in class_table

    def make_stamp(actor):
        return _make_stamp(actor, class_table[actor["cls"].lower()])

    return plan_t3d(t3d_dir / f"{map_name}.t3d", parse, typed,
                    accepts, make_stamp)


def census(maps=MP_MAPS):
    """Pure, deterministic per-map/per-class authored counts."""
    result = {}
    for map_name in maps:
        counts = Counter(stamp["retail_class"] for stamp in plan_map(map_name))
        if counts:
            result[map_name] = dict(sorted(counts.items()))
    return result


def _resolve_runtime_classes(unreal, class_table, class_keys):
    """Resolve native classes and generated BPs, independently per class."""
    runtime = {}
    results = {}
    for key in sorted(class_keys):
        record = class_table[key]
        status = {name: record[name] for name in (
            "retail_class", "port_class", "bp_name", "bp_asset",
            "bp_file", "legacy_mesh")}
        if record["status"] != "READY":
            status.update(status="SKIP", reason=record["reason"])
            results[key] = status
            continue
        base_class = getattr(unreal, record["port_class"], None)
        if base_class is None:
            status.update(
                status="SKIP",
                reason=f"missing reflected native class unreal.{record['port_class']}")
            results[key] = status
            continue
        if not unreal.EditorAssetLibrary.does_asset_exist(record["bp_asset"]):
            status.update(
                status="SKIP",
                reason=f"BP exists on disk but not in asset registry: {record['bp_asset']}")
            results[key] = status
            continue
        blueprint = unreal.load_asset(record["bp_asset"])
        pickup_class = blueprint.generated_class() if blueprint else None
        if pickup_class is None:
            status.update(
                status="SKIP",
                reason=f"could not resolve generated class for {record['bp_asset']}")
            results[key] = status
            continue
        status["status"] = "READY"
        results[key] = status
        runtime[key] = {
            "base_class": base_class,
            "pickup_class": pickup_class,
        }
    return runtime, results


def _stamp_actor(unreal, pickup, stamp):
    overrides = stamp["overrides"]
    for prop in ("respawn_time", "min_initial_charges",
                 "max_initial_charges", "mp_appear_effect"):
        if prop in overrides:
            pickup.set_editor_property(prop, overrides[prop])
    if "sp_initial_charges" in overrides:
        values = list(pickup.get_editor_property("sp_initial_charges"))
        for index, value in overrides["sp_initial_charges"].items():
            values[index] = value
        pickup.set_editor_property("sp_initial_charges", values)
    if "respawn_sound" in overrides:
        sound_path = overrides["respawn_sound"]
        sound = unreal.load_asset(sound_path) if sound_path else None
        if sound is None:
            raise RuntimeError(
                f"missing authored RespawnSound {sound_path!r} for {stamp['label']}")
        pickup.set_editor_property("respawn_sound", sound)
    if "collision_radius" in overrides:
        pickup.root_component.set_editor_property(
            "sphere_radius", overrides["collision_radius"])
    pickup.set_editor_property(
        "tags", [unreal.Name(stamp["tag"])] if stamp["tag"] else [])


def _write_results(results):
    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULT_FILE.write_text(json.dumps(results, indent=2) + "\n",
                           encoding="utf-8")


def main():
    import unreal

    maps = [name for name in sys.argv[1:] if not name.startswith("-")] \
        or FIXED_MAPS
    plans = {map_name: plan_map(map_name) for map_name in maps}
    class_keys = {stamp["class_key"] for plan in plans.values()
                  for stamp in plan}
    runtime, class_results = _resolve_runtime_classes(
        unreal, CLASS_TABLE, class_keys)
    results = {"classes": class_results, "maps": {}}
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    failed = []

    for map_name in maps:
        plan = plans[map_name]
        result = {"status": "FAIL", "saved": False,
                  "planned": len(plan), "classes": {}}
        results["maps"][map_name] = result
        skipped = [stamp for stamp in plan
                   if stamp["class_key"] not in runtime]
        for key, count in sorted(Counter(
                stamp["class_key"] for stamp in skipped).items()):
            result["classes"][key] = {
                "status": "SKIP", "planned": count,
                "reason": class_results[key]["reason"],
            }
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            result["error"] = "no imported map"
            failed.append(map_name)
            continue
        if not les.load_level(asset):
            result["error"] = "load_level failed"
            failed.append(map_name)
            continue
        ready_plan = [stamp for stamp in plan
                      if stamp["class_key"] in runtime]
        try:
            counts = reconcile_map(
                unreal, eas, ready_plan, runtime,
                lambda pickup, stamp: _stamp_actor(unreal, pickup, stamp),
                include_per_class=True)
            ready_classes = counts.pop("classes")
            for key, class_counts in ready_classes.items():
                class_counts["status"] = (
                    "PASS" if (class_counts["verified_pickups"]
                               == class_counts["planned"]
                               and class_counts["legacy_remaining"] == 0
                               and not class_counts["bad_labels"])
                    else "FAIL")
                result["classes"][key] = class_counts
            # Preserve the total authored count while exposing how many were
            # safely reconciled; skipped classes remain untouched and fail the
            # map rather than masquerading as success.
            result.update(counts)
            result["planned"] = len(plan)
            result["reconciled_planned"] = len(ready_plan)
            result["unsupported_overrides"] = [
                {"label": stamp["label"],
                 "properties": stamp["unsupported_overrides"]}
                for stamp in plan if stamp["unsupported_overrides"]
            ]
            result["saved"] = bool(les.save_current_level())
            if (not skipped
                    and result["verified_pickups"] == len(plan)
                    and result["legacy_remaining"] == 0
                    and not result["bad_labels"] and result["saved"]):
                result["status"] = "PASS"
            else:
                failed.append(map_name)
            unreal.log(
                f"FIXED-PICKUPS {map_name} {result['status']}: "
                f"planned={len(plan)} ready={len(ready_plan)} "
                f"restamped={result['restamped']} "
                f"new={result['newly_placed']} "
                f"legacy_removed={result['legacy_removed']} "
                f"verified={result['verified_pickups']} "
                f"legacy_remaining={result['legacy_remaining']}")
        except Exception as exc:  # noqa: BLE001 - persist complete diagnostics
            result["error"] = str(exc)
            failed.append(map_name)

    _write_results(results)
    if failed:
        raise RuntimeError(
            "fixed pickup patch failed for "
            + ", ".join(sorted(set(failed))) + f"; see {RESULT_FILE}")


if __name__ == "__main__":
    main()
