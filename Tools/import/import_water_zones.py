"""Zone Entry/Exit splash sounds over ALREADY-IMPORTED maps (water-splash slice).

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_water_zones.py [<Map> ...]"    (default: the 19 MP maps)

Stamps entry_sound_sub_path / exit_sound_sub_path onto the placed WoTZoneVolume
actors (labels Zone_<name>, import_map.py place_zones), from each T3D
water-zone actor's authored EntrySound/ExitSound (Engine.ZoneInfo defaults both
None; WOT.WaterZone authors none either — its sound imports are commented out,
WaterZone.uc:9-12 — so only ~16 zones across 8 MP maps sound at all). The
shipped EntryActor/ExitActor visual chain (WaterImpact -> WaterRing) is
MESH-LESS in retail 333B and is not ported (PORT_NOTES "Water-splash slice").

Idempotent: stamps overwrite, including clearing to None where unauthored.
The stdlib section imports without `unreal` for dry-runs:
    python3 -c "import import_water_zones as iwz; print(iwz.plan_map('Arena_12'))"
"""
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import exports_dir  # noqa: E402
EXPORTS = Path(os.environ.get("WOT_EXPORTS_DIR", exports_dir(required=False)))
T3D_DIR = EXPORTS / "t3d"
sys.path.insert(0, str(REPO / "Tools" / "convert"))
from t3d import parse, typed  # noqa: E402

MP_MAPS = [f"Arena_{i:02d}" for i in (1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12)] + \
          [f"Battle2_{i:02d}" for i in range(1, 6)] + \
          ["Battle3_01", "Battle3_02", "Battle4_01"]

SOUND_RE = re.compile(r"Sound'([^'.]+)\.(?:[^'.]+\.)?([^'.]+)'")


def sound_subpath(value):
    """Sound'Pkg.Group.Name' -> 'Pkg/Name' (group folds at import), else None."""
    if not value or value.strip() in ("None", ""):
        return None
    m = SOUND_RE.search(value)
    return f"{m.group(1)}/{m.group(2)}" if m else None


def is_water_zone(actor):
    """WaterZone's class default is bWaterZone=True; any ZoneInfo can author it
    (Arena_11's pain pool does). Warp-portal actors never author bWaterZone."""
    if actor["props"].get("bWaterZone", "").lower() == "true":
        return True
    return actor["cls"].lower() == "waterzone" and \
        actor["props"].get("bWaterZone", "").lower() != "false"


def plan_map(map_name):
    """{zone actor name: (entry_subpath|None, exit_subpath|None)}, stdlib-only."""
    zones = {}
    for a in typed(parse(T3D_DIR / f"{map_name}.t3d")):
        if not is_water_zone(a):
            continue
        zones[a["name"]] = (sound_subpath(a["props"].get("EntrySound")),
                            sound_subpath(a["props"].get("ExitSound")))
    return zones


def main():
    import unreal

    maps = [m for m in sys.argv[1:] if not m.startswith("-")] or MP_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"WATERZONE-SKIP {map_name}: no imported map")
            continue
        plan = plan_map(map_name)
        les.load_level(asset)
        by_label = {}
        for act in eas.get_all_level_actors():
            by_label.setdefault(act.get_actor_label(), act)
        hit = miss = authored = 0
        for name, (entry, exit_) in plan.items():
            act = by_label.get(f"Zone_{name}")
            if not isinstance(act, unreal.WoTZoneVolume):
                miss += 1
                unreal.log(f"WATERZONE-MISS {map_name} zone {name!r}")
                continue
            act.set_editor_property("entry_sound_sub_path", entry or "")
            act.set_editor_property("exit_sound_sub_path", exit_ or "")
            hit += 1
            if entry or exit_:
                authored += 1
        les.save_current_level()
        unreal.log(f"WATERZONE-OK {map_name}: stamped={hit}/{hit + miss} "
                   f"authored={authored}")


if __name__ == "__main__":
    main()
