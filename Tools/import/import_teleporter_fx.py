"""Restamp TeleporterWithEffects presentation on already-imported MP maps.

The stdlib plan_map() half imports without unreal for dry-runs.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_teleporter_fx.py [<Map> ...]"
"""
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import MP_MAPS, T3D_DIR, sound_subpath  # noqa: E402
from t3d import parse, typed  # noqa: E402


PRESENTATION_AXES = {
    "EnterEffectClass": ("enter_effect_config", lambda value: _effect(value)),
    "ExitEffectClass": ("exit_effect_config", lambda value: _effect(value)),
    "EnterEffectSound": ("enter_effect_sound_path", lambda value: sound_subpath(value)),
    "ExitEffectSound": ("exit_effect_sound_path", lambda value: sound_subpath(value)),
}


def _effect(value):
    """Class'WOT.WOTSparks' -> WOTSparks; unset stays independently absent."""
    value = str(value or "")
    if "'" in value:
        value = value.split("'", 1)[1].rsplit("'", 1)[0]
    return value.rsplit(".", 1)[-1] if value else ""


def plan_map(map_name):
    """actor label -> four independently-authored effect/sound axes."""
    actors = typed(parse(T3D_DIR / f"{map_name}.t3d"))
    plan = {}
    for actor in actors:
        if actor["cls"] not in ("TeleporterWithEffects", "Teleporter"):
            continue
        payload = {}
        for source, (dest, converter) in PRESENTATION_AXES.items():
            # Explicit empty values are part of the plan: a restamp must clear
            # stale values exactly as a fresh import would.
            payload[dest] = converter(actor["props"].get(source, "")) or ""
        plan[actor["name"]] = payload
    return plan


def _actor_value(value):
    text = str(value or "")
    return "" if text == "None" else text


def restamp_actor(actor, payload):
    """Write every axis; return whether this actor merits census/save."""
    changed = any(_actor_value(actor.get_editor_property(prop)) != value
                  for prop, value in payload.items())
    for prop, value in payload.items():
        actor.set_editor_property(prop, value)
    return any(payload.values()) or changed


def main():
    import unreal

    maps = [m for m in sys.argv[1:] if not m.startswith("-")] or MP_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"TELEPORTFX {map_name} stamped=0 miss=0")
            continue
        les.load_level(asset)
        by_label = {actor.get_actor_label(): actor
                    for actor in eas.get_all_level_actors()}
        stamped = miss = 0
        for label, payload in plan_map(map_name).items():
            actor = by_label.get(label)
            if not isinstance(actor, unreal.WoTTeleporter):
                if any(payload.values()):
                    miss += 1
                continue
            if restamp_actor(actor, payload):
                stamped += 1
        if stamped:
            les.save_current_level()
        unreal.log(f"TELEPORTFX {map_name} stamped={stamped} miss={miss}")


if __name__ == "__main__":
    main()
