"""Patch SoundDispatchers and BudgetInfo edit events into live Battle maps.

The plan_map() half is stdlib-only for dry inspection. Re-runs update actors by
stable T3D label, remove duplicates of this pass's actor types, and save each
loaded map once.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_sound_dispatchers.py [<Map> ...]"
"""
import re
import sys
from collections import Counter
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import T3D_DIR, sound_subpath  # noqa: E402
from t3d import parse, typed  # noqa: E402


BATTLE_MAPS = [
    "Battle2_01", "Battle2_02", "Battle2_03", "Battle2_05",
    "Battle3_01", "Battle3_02", "Battle4_01",
]
_EVENT_KEY = re.compile(r"EventList\((\d+)\)$")


def _bool(value):
    return str(value or "").lower() == "true"


def _event(value):
    text = str(value or "").strip()
    if text.startswith("(") and text.endswith(")"):
        text = text[1:-1]
    fields = {}
    for item in text.split(","):
        if "=" in item:
            key, field_value = item.split("=", 1)
            fields[key.strip()] = field_value.strip()
    subpath = sound_subpath(fields.get("Sound"))
    sound = ""
    if subpath:
        leaf = subpath.rsplit("/", 1)[-1]
        sound = f"/Game/WoT/Sounds/{subpath}.{leaf}"
    return {
        "delay": float(fields.get("Delay", 0.0)),
        "event": fields.get("Event", ""),
        "sound_source_tag": fields.get("SoundSourceTag", ""),
        "sound": sound,
        "sound_table_tag": fields.get("SoundTableTag", ""),
        "sound_table_slot": fields.get("SoundTableSlot", ""),
    }


def _events(props):
    entries = {}
    for key, value in props.items():
        match = _EVENT_KEY.match(key)
        if match:
            entries[int(match.group(1))] = _event(value)
    if not entries:
        return []
    return [entries.get(index, _event(""))
            for index in range(max(entries) + 1)]


def plan_map(map_name):
    """Return dispatcher, marker, and budget stamps from one retail T3D."""
    actors = typed(parse(T3D_DIR / f"{map_name}.t3d"))
    by_tag = {}
    tag_counts = Counter()
    for actor in actors:
        tag = actor.get("props", {}).get("Tag")
        if tag:
            tag_counts[tag] += 1
            by_tag.setdefault(tag, actor)  # preserve UE1 AllActors order
    dispatchers = []
    markers = {}
    budgets = {}
    for actor in actors:
        props = actor.get("props", {})
        if actor["cls"] == "SoundDispatcher":
            entries = _events(props)
            dispatchers.append({
                "label": actor["name"],
                "location": actor["location"],
                "rotation": actor["rotation"],
                "tag_name": props.get("Tag", ""),
                "event_name": props.get("Event", ""),
                "trigger_type": props.get("TriggerType", "TT_PlayerProximity"),
                "re_trigger_delay": float(props.get("ReTriggerDelay", 0.0)),
                "collision_radius": float(props.get("CollisionRadius", 40.0)),
                "collision_height": float(props.get("CollisionHeight", 40.0)),
                "event_list": entries,
                "sound_volume": float(props.get("SoundVolume", 1.0)),
                "sound_radius": float(props.get("SoundRadius", 2048.0)),
                "sound_pitch": float(props.get("SoundPitch", 1.0)),
                "sound_override": _bool(props.get("bSoundOverride")),
                "max_distance": int(float(props.get("MaxDistance", 0))),
                "from_sound_source": _bool(props.get("bFromSoundSource")),
                "terminate": _bool(props.get("bTerminate")),
            })
            for entry in entries:
                tag = entry["sound_source_tag"]
                if not tag or tag in markers:
                    continue
                source = by_tag.get(tag)
                if source and source["cls"] in ("Trigger", "SoundDispatcher") \
                        and tag_counts[tag] == 1:
                    continue
                markers[tag] = (source or actor)["location"]
        elif actor["cls"].endswith("Budget") \
                and actor["cls"] != "BudgetTrigger":
            budgets[actor["name"]] = {
                "begin_edit_event": props.get("BeginEditEvent", ""),
                "end_edit_event": props.get("EndEditEvent", ""),
            }
    return {
        "dispatchers": dispatchers,
        "markers": [{"label": f"WoTTaggedMarker_{tag}", "tag_name": tag,
                     "location": location}
                    for tag, location in sorted(markers.items())],
        "budgets": budgets,
    }


def _rotator(unreal, rotation):
    scale = 360.0 / 65536.0
    return unreal.Rotator(roll=rotation[2] * scale,
                          pitch=rotation[0] * scale,
                          yaw=rotation[1] * scale)


def _event_rows(unreal, entries):
    rows = []
    for entry in entries:
        row = unreal.WoTSoundDispatcherEvent()
        for field in ("delay", "event", "sound_source_tag",
                      "sound_table_tag", "sound_table_slot"):
            row.set_editor_property(field, entry[field])
        if entry["sound"]:
            sound_asset = unreal.load_asset(entry["sound"])
            if sound_asset is None:
                raise RuntimeError(f"missing SoundDispatcher sound {entry['sound']}")
            # The Python bridge nativizes TSoftObjectPtr as an object property;
            # assigning the UObject records the same soft asset path in the map.
            row.set_editor_property("sound", sound_asset)
        rows.append(row)
    return rows


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
    trigger_types = {
        "TT_PlayerProximity": unreal.WoTTriggerType.PLAYER_PROXIMITY,
        "TT_PawnProximity": unreal.WoTTriggerType.PAWN_PROXIMITY,
        "TT_ClassProximity": unreal.WoTTriggerType.CLASS_PROXIMITY,
        "TT_AnyProximity": unreal.WoTTriggerType.ANY_PROXIMITY,
        "TT_Shoot": unreal.WoTTriggerType.SHOOT,
    }
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"SOUNDDISPATCHER {map_name} skipped: no imported map")
            continue
        if not les.load_level(asset):
            unreal.log_error(f"SOUNDDISPATCHER {map_name}: load_level failed")
            continue
        plan = plan_map(map_name)
        actors = list(eas.get_all_level_actors())

        for stamp in plan["dispatchers"]:
            location = unreal.Vector(*stamp["location"])
            actor = _single_actor(eas, actors, stamp["label"],
                                  unreal.WoTSoundDispatcher, location)
            actor.set_actor_rotation(_rotator(unreal, stamp["rotation"]), False)
            for prop in ("tag_name", "event_name", "re_trigger_delay",
                         "sound_volume", "sound_radius", "sound_pitch",
                         "sound_override", "max_distance", "from_sound_source",
                         "terminate"):
                actor.set_editor_property(prop, stamp[prop])
            actor.set_editor_property(
                "trigger_type", trigger_types[stamp["trigger_type"]])
            actor.set_editor_property(
                "event_list", _event_rows(unreal, stamp["event_list"]))
            radius = stamp["collision_radius"]
            actor.get_editor_property("capsule").set_capsule_size(
                radius, max(stamp["collision_height"], radius), False)

        for stamp in plan["markers"]:
            marker = _single_actor(
                eas, actors, stamp["label"], unreal.WoTTaggedMarker,
                unreal.Vector(*stamp["location"]))
            marker.set_editor_property("tag_name", stamp["tag_name"])

        by_label = {actor.get_actor_label(): actor for actor in actors}
        budget_misses = 0
        for label, stamp in plan["budgets"].items():
            budget = by_label.get(label)
            if not isinstance(budget, unreal.WoTBudgetInfo):
                budget_misses += 1
                continue
            for prop, value in stamp.items():
                budget.set_editor_property(prop, value)

        les.save_current_level()
        unreal.log(
            f"SOUNDDISPATCHER {map_name}: dispatchers={len(plan['dispatchers'])} "
            f"markers={len(plan['markers'])} budgets={len(plan['budgets'])} "
            f"budget_misses={budget_misses}")


if __name__ == "__main__":
    main()
