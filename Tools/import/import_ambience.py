"""Additive ambience pass over ALREADY-IMPORTED maps (presentation Slice 8d).

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_ambience.py [<Map> ...]"     (default: the 19 MP maps)

Patches each /Game/WoT/Maps/<Map> level in place — it does NOT re-run the map
import (BSP/lighting stay untouched; the full import lives on the Windows box):
 1. places one AWoTAmbientSound per T3D actor whose EFFECTIVE AmbientSound is
    set (instance override, else the .uc class-default chain), with the
    effective byte SoundRadius/Volume/Pitch and — for lit carriers — the static
    LightBrightness/255 volume factor (UE1 pulsed with the live light);
 2. places one AWoTDynamicAmbientSound per DynamicAmbientSound keypoint;
 3. stamps the mover sound refs (Opening/Opened/Closing/Closed/MoveAmbient +
    bytes) onto the placed WoTMover actors, matched by label;
 4. stamps decoration ambient-anim data (bAutoAnimate chain: Seaweed;
    DragonFly's hardcoded LoopAnim + Fish's PlayAnim cycle as special cases)
    and the level-placed corpse poses (instance AnimSequence/AnimFrame) onto
    the placed WoTDecoration actors, matched by label.

Idempotent: existing WoTAmbientSound/WoTDynamicAmbientSound actors are deleted
before placement; stamps overwrite. The stdlib section (everything before
main()) imports without `unreal` for dry-runs:
    python3 -c "import import_ambience as ia; print(ia.plan_map('Battle2_05'))"

Engine-class defaults are hardcoded from UE1's Engine classes (retail Engine
script is stripped): Actor 32/128/64 bytes + LT_None/LightBrightness 0; Light
LT_Steady/64; the AmbientSound keypoint 64/190/64.
"""
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import exports_dir  # noqa: E402
EXPORTS = Path(os.environ.get("WOT_EXPORTS_DIR", exports_dir(required=False)))
SCRIPT = EXPORTS / "script"
T3D_DIR = EXPORTS / "t3d"
sys.path.insert(0, str(REPO / "Tools" / "convert"))
from t3d import parse, typed  # noqa: E402

MP_MAPS = [f"Arena_{i:02d}" for i in (1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12)] + \
          [f"Battle2_{i:02d}" for i in range(1, 6)] + \
          ["Battle3_01", "Battle3_02", "Battle4_01"]

SOUND_RE = re.compile(r"Sound'([^'.]+)\.(?:[^'.]+\.)?([^'.]+)'")

# Engine-class defaults (UE1; retail Engine script is stripped). Chain
# walking stops at these roots. Values: only the props this pass resolves.
ENGINE_DEFAULTS = {
    "actor": {"SoundRadius": "32", "SoundVolume": "128", "SoundPitch": "64",
              "LightType": "LT_None", "LightBrightness": "0"},
    "light": {"LightType": "LT_Steady", "LightBrightness": "64"},
    "ambientsound": {"SoundRadius": "64", "SoundVolume": "190", "SoundPitch": "64"},
}
# Engine parentage for the chain (game .uc chains dead-end at these).
ENGINE_PARENTS = {"light": "actor", "ambientsound": "keypoint", "keypoint": "actor",
                  "decoration": "actor", "mover": "actor", "triggers": "actor",
                  "trigger": "triggers", "effects": "actor", "info": "actor",
                  "pawn": "actor", "projectile": "actor", "navigationpoint": "actor",
                  "playerstart": "navigationpoint"}

MOVER_ONESHOTS = ("OpeningSound", "OpenedSound", "ClosingSound", "ClosedSound")


def _load_classes():
    """class(lower) -> (parent(lower), defaultproperties dict) from exports/script."""
    classes = {}
    decl = re.compile(r"^\s*class\s+(\w+)\s+(?:expands|extends)\s+(\w+)",
                      re.I | re.M)
    for uc in SCRIPT.rglob("*.uc"):
        text = uc.read_text(errors="ignore")
        m = decl.search(text)
        if not m:
            continue
        defaults = {}
        dp = text.rfind("defaultproperties")
        if dp != -1:
            for line in text[dp:].splitlines()[1:]:
                line = line.strip()
                if line.startswith("}"):
                    break
                if "=" in line and not line.startswith("//"):
                    k, v = line.split("=", 1)
                    defaults[k.strip()] = v.strip()
        classes[m.group(1).lower()] = (m.group(2).lower(), defaults)
    return classes


_CLASSES = None


def classes():
    global _CLASSES
    if _CLASSES is None:
        _CLASSES = _load_classes()
    return _CLASSES


def class_default(cls, prop):
    """Walk the defaultproperties chain (game .uc, then engine table)."""
    seen = set()
    cur = cls.lower()
    while cur and cur not in seen:
        seen.add(cur)
        if cur in ENGINE_DEFAULTS and prop in ENGINE_DEFAULTS[cur]:
            return ENGINE_DEFAULTS[cur][prop]
        if cur in classes():
            parent, defaults = classes()[cur]
            if prop in defaults:
                return defaults[prop]
            cur = parent
        else:
            cur = ENGINE_PARENTS.get(cur)
    return None


def effective(actor, prop):
    v = actor["props"].get(prop)
    return v if v is not None else class_default(actor["cls"], prop)


def sound_subpath(value):
    """Sound'Pkg.Group.Name' -> 'Pkg/Name' (group folds at import), else None."""
    if not value or value.strip() in ("None", ""):
        return None
    m = SOUND_RE.search(value)
    return f"{m.group(1)}/{m.group(2)}" if m else None


def byte_of(actor, prop, fallback):
    v = effective(actor, prop)
    try:
        return max(0, min(255, int(float(v))))
    except (TypeError, ValueError):
        return fallback


def is_mover(cls):
    return cls.lower().endswith("mover")


# The two shipped hardcoded-animation decorations (mechanism in the .uc, data
# here): DragonFly.uc PreBeginPlay LoopAnim('DRAGONFLY', RandRange(Min,Max));
# Fish.uc AnimatedFish state PlayAnim chain at ONE rolled rate.
SPECIAL_DECO = {
    "dragonfly": dict(auto=True, sequence="DRAGONFLY"),
    "fish": dict(cycle=["Frame01", "Frame02", "Frame01", "Frame03"]),
}


def plan_map(map_name):
    """Everything the UE pass needs for one map, stdlib-only."""
    actors = typed(parse(T3D_DIR / f"{map_name}.t3d"))
    plan = dict(emitters=[], dynamics=[], movers={}, decos={})
    for a in actors:
        cls = a["cls"].lower()
        if cls == "dynamicambientsound":
            sounds = []
            for i in range(6):
                sub = sound_subpath(a["props"].get(f"Sounds({i})"))
                sounds.append(sub or "")
            # shipped BeginPlay counts to the first empty slot
            while sounds and not sounds[-1]:
                sounds.pop()
            plan["dynamics"].append(dict(
                name=a["name"], location=a["location"], sounds=sounds,
                initially_on=a["props"].get("bInitiallyOn", "").lower() == "true",
                probability=float(a["props"].get("playProbability",
                                  class_default(cls, "playProbability") or 0.6)),
                min_recheck=float(a["props"].get("minReCheckTime",
                                  class_default(cls, "minReCheckTime") or 5.0)),
                max_recheck=float(a["props"].get("maxReCheckTime",
                                  class_default(cls, "maxReCheckTime") or 10.0)),
                dont_repeat=a["props"].get("bDontRepeat", "").lower() == "true",
                radius=byte_of(a, "SoundRadius", 32),
                volume=byte_of(a, "SoundVolume", 42)))
            continue
        if is_mover(cls):
            snd = {}
            for prop in MOVER_ONESHOTS + ("MoveAmbientSound",):
                sub = sound_subpath(a["props"].get(prop))  # instance-only on movers
                if sub:
                    snd[prop] = sub
            if snd:
                snd.update(radius=byte_of(a, "SoundRadius", 32),
                           volume=byte_of(a, "SoundVolume", 128),
                           pitch=byte_of(a, "SoundPitch", 64))
                plan["movers"][a["name"]] = snd
            continue
        # placed ambient emitters (any class, incl. class-default carriers)
        sub = sound_subpath(effective(a, "AmbientSound"))
        if sub:
            light = 1.0
            if (effective(a, "LightType") or "LT_None") != "LT_None":
                try:
                    light = min(255, int(float(effective(a, "LightBrightness")
                                               or 0))) / 255.0
                except ValueError:
                    light = 0.0
            plan["emitters"].append(dict(
                name=a["name"], location=a["location"], sound=sub,
                radius=byte_of(a, "SoundRadius", 32),
                volume=byte_of(a, "SoundVolume", 128),
                pitch=byte_of(a, "SoundPitch", 64), light=round(light, 4)))
        # decoration ambient anim / pose stamps
        special = SPECIAL_DECO.get(cls)
        auto = (special or {}).get("auto") or \
            (class_default(cls, "bAutoAnimate") or "").lower() == "true"
        cycle = (special or {}).get("cycle")
        pose = None if (auto or cycle) else a["props"].get("AnimSequence")
        if not (auto or cycle or pose):
            continue
        d = dict(name=a["name"])
        if auto:
            d["auto"] = True
            d["sequence"] = (special or {}).get("sequence") or \
                effective(a, "AnimationSequence") or "All"
        if cycle:
            d["cycle"] = cycle
        if auto or cycle:
            d["min_rate"] = float(effective(a, "MinAnimRate") or 1.0)
            d["max_rate"] = float(effective(a, "MaxAnimRate") or 1.0)
        if pose:
            d["pose"] = pose
            d["pose_frame"] = float(a["props"].get("AnimFrame", 0.0))
        plan["decos"][a["name"]] = d
    return plan


def main():
    import unreal

    maps = [m for m in sys.argv[1:] if not m.startswith("-")] or MP_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"AMBIENCE-SKIP {map_name}: no imported map")
            continue
        plan = plan_map(map_name)
        les.load_level(asset)
        by_label = {}
        for act in eas.get_all_level_actors():
            by_label.setdefault(act.get_actor_label(), act)
            if isinstance(act, (unreal.WoTAmbientSound,
                                unreal.WoTDynamicAmbientSound)):
                eas.destroy_actor(act)

        for e in plan["emitters"]:
            act = eas.spawn_actor_from_class(unreal.WoTAmbientSound,
                                             unreal.Vector(*e["location"]))
            act.set_actor_label(f"Amb_{e['name']}")
            act.set_editor_property("sound_sub_path", e["sound"])
            act.set_editor_property("sound_radius", e["radius"])
            act.set_editor_property("sound_volume", e["volume"])
            act.set_editor_property("sound_pitch", e["pitch"])
            act.set_editor_property("light_volume_factor", e["light"])

        for d in plan["dynamics"]:
            act = eas.spawn_actor_from_class(unreal.WoTDynamicAmbientSound,
                                             unreal.Vector(*d["location"]))
            act.set_actor_label(f"Dyn_{d['name']}")
            act.set_editor_property("sound_sub_paths",
                                    [unreal.Name(s) for s in d["sounds"]])
            act.set_editor_property("initially_on", d["initially_on"])
            act.set_editor_property("play_probability", d["probability"])
            act.set_editor_property("min_re_check_time", d["min_recheck"])
            act.set_editor_property("max_re_check_time", d["max_recheck"])
            act.set_editor_property("dont_repeat", d["dont_repeat"])
            act.set_editor_property("sound_radius", d["radius"])
            act.set_editor_property("sound_volume", d["volume"])

        mv_hit = mv_miss = 0
        prop_map = {"OpeningSound": "opening_sound_path",
                    "OpenedSound": "opened_sound_path",
                    "ClosingSound": "closing_sound_path",
                    "ClosedSound": "closed_sound_path",
                    "MoveAmbientSound": "move_ambient_sound_path"}
        for name, snd in plan["movers"].items():
            act = by_label.get(name)
            if not isinstance(act, unreal.WoTMover):
                mv_miss += 1
                unreal.log(f"AMBIENCE-MISS {map_name} mover {name!r}")
                continue
            for prop, sub in snd.items():
                if prop in prop_map:
                    act.set_editor_property(prop_map[prop], sub)
            act.set_editor_property("sound_radius", snd["radius"])
            act.set_editor_property("sound_volume", snd["volume"])
            act.set_editor_property("sound_pitch", snd["pitch"])
            mv_hit += 1

        deco_hit = deco_miss = 0
        for name, d in plan["decos"].items():
            act = by_label.get(name)
            if not isinstance(act, unreal.WoTDecoration):
                deco_miss += 1
                unreal.log(f"AMBIENCE-MISS {map_name} deco {name!r}")
                continue
            if d.get("auto"):
                act.set_editor_property("auto_animate", True)
                act.set_editor_property("animation_sequence", d["sequence"])
            if d.get("cycle"):
                act.set_editor_property("play_anim_cycle",
                                        [unreal.Name(s) for s in d["cycle"]])
            if "min_rate" in d:
                act.set_editor_property("min_anim_rate", d["min_rate"])
                act.set_editor_property("max_anim_rate", d["max_rate"])
            if d.get("pose"):
                act.set_editor_property("pose_sequence", d["pose"])
                act.set_editor_property("pose_frame", d["pose_frame"])
            deco_hit += 1

        les.save_current_level()
        unreal.log(f"AMBIENCE-OK {map_name}: amb={len(plan['emitters'])} "
                   f"dyn={len(plan['dynamics'])} mover={mv_hit}/{mv_hit + mv_miss} "
                   f"deco={deco_hit}/{deco_hit + deco_miss}")


if __name__ == "__main__":
    main()
