"""Restamp mover gameplay on ALREADY-IMPORTED MP maps (G26+G27).

Re-run this pass after any T3D re-export. It patches mover group/encroach and
lock-presentation properties in place without touching BSP, lighting, or mesh
assets. The stdlib plan_map() half imports without unreal for dry-runs.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_mover_gameplay.py [<Map> ...]"
"""
import sys
from pathlib import Path

# UE's embedded python runs this by absolute -script path with no repo dirs on
# sys.path; mirror import_ambience's own bootstrap before importing it.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import (MP_MAPS, T3D_DIR, class_default,  # noqa: E402
                             sound_subpath)

from t3d import parse, typed  # noqa: E402


def _bool(value):
    return str(value or "").lower() == "true"


def _string(value):
    value = str(value)
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1].replace(r'\"', '"')
    return value


def _encroach(value):
    name = str(value or "ME_ReturnWhenEncroach")
    if name.startswith("ME_"):
        name = name[3:]
    return name.replace("WhenEncroach", "")


def _sound(value):
    return sound_subpath(value) or ""


def plan_map(map_name):
    """actor label -> importer property payload, stdlib-only."""
    actors = typed(parse(T3D_DIR / f"{map_name}.t3d"))
    plan = {}
    for actor in actors:
        if not actor["cls"].lower().endswith("mover"):
            continue
        props = actor["props"]
        class_encroach = class_default(actor["cls"], "MoverEncroachType")
        class_damage = class_default(actor["cls"], "EncroachDamage")
        stamp = {
            "encroach_type": _encroach(
                props.get("MoverEncroachType", class_encroach)),
            "encroach_damage": int(float(props.get(
                "EncroachDamage", class_damage or 0))),
            "slave": _bool(props.get("bSlave")),
        }
        if "ReturnGroup" in props:
            stamp["return_group"] = props["ReturnGroup"]
        if "bUnlocked" in props:
            stamp["unlocked"] = _bool(props["bUnlocked"])
        authored = {
            "LockedSound": ("locked_sound_path", _sound),
            "UnLockSound": ("un_lock_sound_path", _sound),
            "LockedMessage": ("locked_message", _string),
            "UnLockMessage": ("un_lock_message", _string),
            "LockedMessageDuration": ("locked_message_duration", float),
            "UnLockMessageDuration": ("un_lock_message_duration", float),
            "MinLockSoundTime": ("min_lock_sound_time", float),
            "UnLockDuration": ("un_lock_duration", float),
        }
        for source, (dest, converter) in authored.items():
            if source in props:
                stamp[dest] = converter(props[source])
        if actor["cls"] == "LockableMover" or _bool(props.get("bLockable")):
            stamp["lockable"] = True
            stamp["stay_unlocked"] = _bool(props.get("bStayUnlocked"))
        plan[actor["name"]] = stamp
    return plan


def main():
    import unreal

    maps = [m for m in sys.argv[1:] if not m.startswith("-")] or MP_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    encroach_types = {
        "Stop": unreal.WoTMoverEncroachType.STOP_WHEN_ENCROACH,
        "Crush": unreal.WoTMoverEncroachType.CRUSH_WHEN_ENCROACH,
        "Return": unreal.WoTMoverEncroachType.RETURN_WHEN_ENCROACH,
        "Ignore": unreal.WoTMoverEncroachType.IGNORE_WHEN_ENCROACH,
    }
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"MOVERGAMEPLAY {map_name} stamped=0 miss=0")
            continue
        les.load_level(asset)
        by_label = {actor.get_actor_label(): actor
                    for actor in eas.get_all_level_actors()}
        stamped = miss = 0
        for label, payload in plan_map(map_name).items():
            actor = by_label.get(label)
            if not isinstance(actor, unreal.WoTMover):
                miss += 1
                continue
            for prop, value in payload.items():
                if prop == "encroach_type":
                    value = encroach_types[value]
                actor.set_editor_property(prop, value)
            stamped += 1
        if stamped:
            les.save_current_level()   # mover-less maps: skip the save churn
        unreal.log(f"MOVERGAMEPLAY {map_name} stamped={stamped} miss={miss}")


if __name__ == "__main__":
    main()
