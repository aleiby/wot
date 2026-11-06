"""Patch citadel-parked MashadarGuides into live Battle maps (G15).

Mirrors import_map.py's MashadarGuide branch for maps that are already
imported, the import_sound_dispatchers.py precedent: plan_map() is
stdlib-only for dry inspection; re-runs update actors by stable T3D label,
remove duplicates, and save each loaded map once. MashadarManager /
RestPoint / MashadarPathNode are runtime-folded (PORT_NOTES 2026-07-19).

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_mashadar_guides.py [<Map> ...]"
"""
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import T3D_DIR  # noqa: E402
from t3d import parse, typed  # noqa: E402


BATTLE_MAPS = [
    "Battle2_01", "Battle2_02", "Battle3_01", "Battle3_02", "Battle4_01",
]


def plan_map(map_name):
    """Return the MashadarGuide stamps from one retail T3D."""
    guides = []
    for actor in typed(parse(T3D_DIR / f"{map_name}.t3d")):
        if actor["cls"] != "MashadarGuide":
            continue
        props = actor.get("props", {})
        guides.append({
            "label": actor["name"],
            "location": actor["location"],
            "rotation": actor["rotation"],
            "team": int(props.get("Team", 255)),
            "search_radius": float(props.get("SearchRadius", 600.0)),
            # MashadarGuide.uc:247: deployed guides trigger at TriggerRadius; the
            # Battle maps author per-instance overrides (e.g. 500 vs the 200 CDO).
            "trigger_radius": float(props.get("TriggerRadius", 200.0)),
        })
    return guides


def _rotator(unreal, rotation):
    scale = 360.0 / 65536.0
    return unreal.Rotator(roll=rotation[2] * scale,
                          pitch=rotation[0] * scale,
                          yaw=rotation[1] * scale)


def _single_actor(eas, actors, label, actor_class, location):
    matches = [actor for actor in actors if actor.get_actor_label() == label]
    keep = next((actor for actor in matches if isinstance(actor, actor_class)), None)
    for actor in matches:
        if actor != keep:
            eas.destroy_actor(actor)
    if keep is None:
        keep = eas.spawn_actor_from_class(actor_class, location)
        keep.set_actor_label(label)
        actors.append(keep)
    keep.set_actor_location(location, False, False)
    return keep


def main():
    import unreal

    maps = [name for name in sys.argv[1:] if not name.startswith("-")] or BATTLE_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"MASHADARGUIDE {map_name} skipped: no imported map")
            continue
        if not les.load_level(asset):
            unreal.log_error(f"MASHADARGUIDE {map_name}: load_level failed")
            continue
        plan = plan_map(map_name)
        actors = list(eas.get_all_level_actors())
        for stamp in plan:
            actor = _single_actor(eas, actors, stamp["label"],
                                  unreal.WoTMashadarGuide,
                                  unreal.Vector(*stamp["location"]))
            actor.set_actor_rotation(_rotator(unreal, stamp["rotation"]), False)
            actor.set_editor_property("team", stamp["team"])
            actor.set_editor_property("search_radius", stamp["search_radius"])
            actor.set_editor_property("trigger_radius", stamp["trigger_radius"])
            actor.set_editor_property("citadel_placed", True)
        les.save_current_level()
        unreal.log(f"MASHADARGUIDE {map_name}: guides={len(plan)}")


if __name__ == "__main__":
    main()
