"""Extract mover brush models per map → GLB + movers.json.

Usage: python3 movers2glb.py <map.wot> <t3d_json> <out_dir> [tex_root]
Mover models are stored in LOCAL brush coordinates (verified: bounds nowhere
near BasePos), so each GLB is placed at the actor's Location/BaseRot by the
importer. movers.json carries keys/times/state/tag/event per mover plus
trigger/door-trigger/dispatcher/counter actors for the event graph.
"""
import json
import re
import sys
from pathlib import Path

from bsp2obj import (build_groups, groups_to_glb, load_tex_sizes,
                     split_mover_groups_by_plane)
from lightmaps import decode_model_full, point_zone, zone_actor_name
from ue1pkg import Package, decode_model

TUP = re.compile(r"([A-Za-z]+)=(-?[0-9.]+)")

# Engine/WOT Mover-family classes (each `expand Mover` in the original). The T3D
# places these as brush actors sharing the Mover Brush/KeyPos/KeyRot structure,
# so movers2glb extracts every one's geometry identically. The importer decides
# behavior by class: base FSM for Mover/LockableMover, dedicated subclasses for
# Door/Portcullis/Exploding/Rotating/Loop/Timed/Assert and the native
# Elevator/Attach/Gradual/Mix implementations. Before this, only cls=="Mover"
# was collected, so subclass brushes had NO geometry on the imported maps.
MOVER_CLASSES = ("Mover", "DoorMover", "LockableMover", "PortcullisMover",
                 "ExplodingMover", "RotatingMover", "LoopMover", "TimedMover",
                 "AssertMover", "ElevatorMover", "AttachMover", "GradualMover",
                 "MixMover")

# Ported subclasses whose auto state is their OWN custom state (ExplodingMover /
# LoopMove / TriggerTimedMover / AssertTriggerOpenTimed), not a base-Mover FSM
# state. T3D omits InitialState when it equals the class default, so for these
# the unauthored fallback must NOT be the base "BumpOpenTimed": emit "" so the
# importer can tell "unauthored → native subclass FSM" apart from an authored
# base-FSM override (UE1 semantics: an instance InitialState replaces the class
# auto state completely — e.g. Battle2_02 AssertMover2 authors TriggerOpenTimed).
SUBCLASS_STATE_MOVERS = {"ExplodingMover", "RotatingMover", "LoopMover",
                         "TimedMover", "AssertMover"}

SOUND_RE = re.compile(r"Sound'([^'.]+)\.(?:[^'.]+\.)?([^'.]+)'")
ASSET_RE = re.compile(r"(?:Texture|Material)'(?:[^'.]+\.)*([^'.]+)'")


def tup(s, keys):
    d = {k.lower(): float(v) for k, v in TUP.findall(s or "")}
    return [d.get(k, 0.0) for k in keys]


def collect(actors, cls):
    return [a for a in actors if a["cls"] == cls]


def indexed(props, name, count, convert_value=float, default=None):
    """Read a UE1 fixed array, retaining its declared size and class defaults.

    T3D exports ONLY the slots that differ from the class default, so every
    unexported slot must be backfilled PER SLOT from the class defaultproperties:
    pass `default` as a per-slot list when the defaults are non-uniform (e.g.
    ExplodingMover.ActivatedBy defaults to slot0='All', rest empty), or a single
    scalar when they are uniform. An explicitly exported value always wins,
    including an authored `None`."""
    defaults = default if isinstance(default, (list, tuple)) else [default] * count
    out = []
    for i in range(count):
        value = props.get(f"{name}({i})")
        out.append(convert_value(value) if value is not None else defaults[i])
    return out


def authored_indexed(props, name, count, convert_value=float):
    """Return a fixed authored array only when at least one slot is present."""
    if not any(f"{name}({i})" in props for i in range(count)):
        return None
    default = 0.0 if convert_value is float else ""
    return indexed(props, name, count, convert_value, default)


def unreal_string(value):
    """T3D localized string literal, retaining authored empty-string presence."""
    if value is None:
        return None
    value = str(value)
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1].replace(r'\"', '"')
    return value


def encroach_name(value):
    name = str(value or "ME_ReturnWhenEncroach")
    if name.startswith("ME_"):
        name = name[3:]
    return name.replace("WhenEncroach", "")


def sound_subpath(value):
    """Sound'Pkg.Group.Name' -> 'Pkg/Name' (sound import folds the group)."""
    if not value or value.strip() in ("", "None"):
        return ""
    match = SOUND_RE.search(value)
    return f"{match.group(1)}/{match.group(2)}" if match else ""


def asset_name(value):
    """Texture'Pkg.Group.Name' -> bare imported asset name."""
    if not value or value.strip() in ("", "None"):
        return ""
    match = ASSET_RE.search(value)
    return match.group(1) if match else ""


def convert(map_path, t3d_json, out_dir, tex_root=None):
    tex_sizes = load_tex_sizes(tex_root) if tex_root else {}
    pkg = Package(map_path)
    by_name = {e["name"]: e for e in pkg.exports}
    data = json.loads(Path(t3d_json).read_text())["actors"]
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Level BSP for mover runtime-zone resolution: UE1's runtime zone
    # assignment classifies each mover's Location against the level model at
    # spawn, and the surface lighting seeds that zone's ambient. The T3D
    # Region is the stale editor value (LevelInfo0), so resolve it here.
    lvl_models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
    level_bsp = decode_model_full(pkg, max(lvl_models, key=lambda e: e["size"]))

    movers = []
    materials = {}      # mover material slots → {package, texture, flags}, so the
                        # importer can import mover-ONLY textures (never on a world
                        # surface, e.g. TileFlor2021) and build their MeshLit MICs.
    for a in [x for x in data if x["cls"] in MOVER_CLASSES]:
        p = a["props"]
        m = re.search(r"Model'[^.]+\.(\w+)'", p.get("Brush", ""))
        if not m or m.group(1) not in by_name:
            continue
        model = m.group(1)
        bsp = decode_model(pkg, by_name[model])
        groups, texflags, stats = build_groups(
            pkg, bsp, tex_sizes, include_planes=True)
        if not stats["tris"]:
            continue
        groups, texflags, surfaces, plane_extras = split_mover_groups_by_plane(
            groups, texflags, bsp["points"])
        groups_to_glb(out_dir / f"{model}.glb", model, groups, bsp["points"],
                      primitive_extras=plane_extras)
        source_materials = {surface["slot"]: surface["source_material"]
                            for surface in surfaces}
        for mat, (pk, tx, fl) in texflags.items():
            materials.setdefault(mat, dict(
                package=pk, texture=tx, flags=fl,
                source_material=source_materials[mat]))
        keys = {}
        for k, v in p.items():
            km = re.match(r"Key(Pos|Rot)\((\d)\)", k)
            if km:
                kind, idx = km.group(1), int(km.group(2))
                keys.setdefault(idx, {})[kind] = tup(
                    v, "xyz" if kind == "Pos" else ("pitch", "yaw", "roll"))
        base_pos = tup(p.get("BasePos", ""), "xyz") if "BasePos" in p else a["location"]
        zone = zone_actor_name(pkg, level_bsp, point_zone(level_bsp, base_pos))
        subclass = {}
        if a["cls"] == "ExplodingMover":
            subclass.update(
                explosion_size=float(p.get("ExplosionSize", 200.0)),
                explosion_dimensions=float(p.get("ExplosionDimensions", 120.0)),
                wall_particle_size=float(p.get("WallParticleSize", 1.0)),
                wood_particle_size=float(p.get("WoodParticleSize", 1.0)),
                glass_particle_size=float(p.get("GlassParticleSize", 1.0)),
                num_wall_chunks=int(p.get("NumWallChunks", 10)),
                num_wood_chunks=int(p.get("NumWoodChunks", 3)),
                num_glass_chunks=int(p.get("NumGlassChunks", 0)),
                health=int(p.get("Health", 0)),
                # Per-slot class defaults (ExplodingMover.uc defaultproperties:
                # ActivatedBy(0)=All, slots 1-4 empty). A partial export like
                # Mission_03 ExplodingMover1's `ActivatedBy(1)=Air` keeps the
                # default 'All' in slot 0; an authored `ActivatedBy(0)=None`
                # (Mission_05a) still overrides it away.
                activated_by=indexed(p, "ActivatedBy", 5, str,
                                     ["All", "", "", "", ""]),
                wall_texture_name=asset_name(p.get("WallTexture")),
                wood_texture_name=asset_name(p.get("WoodTexture")),
                glass_texture_name=asset_name(p.get("GlassTexture")),
                breaking_sound=sound_subpath(p.get("BreakingSound")),
                translucent_glass=p.get("bTranslucentGlass", "").lower() == "true",
                unlit_glass=p.get("bUnlitGlass", "").lower() == "true",
                only_triggerable=p.get("bOnlyTriggerable", "").lower() == "true")
        elif a["cls"] == "RotatingMover":
            subclass["rotate_rate"] = tup(
                p.get("RotateRate", ""), ("pitch", "yaw", "roll"))
        elif a["cls"] == "LoopMover":
            subclass["next_key_num"] = int(p.get("NextKeyNum", 0))
        elif a["cls"] == "TimedMover":
            # Uniform scalar defaults are exact here: TimedMover.uc ships empty
            # defaultproperties, so every unexported slot is 0.0/no-sound.
            subclass.update(
                delays=indexed(p, "Delays", 8, float, 0.0),
                move_times=indexed(p, "MoveTimes", 8, float, 0.0),
                sounds=indexed(p, "Sounds", 8, sound_subpath, ""))
        elif a["cls"] == "AssertMover":
            # AssertMover.uc likewise declares no OpenTimes/CloseTimes defaults.
            subclass.update(
                open_times=indexed(p, "OpenTimes", 6, float, 0.0),
                close_times=indexed(p, "CloseTimes", 6, float, 0.0),
                once_only_stop_open=p.get("bOnceOnlyStopOpen", "").lower() == "true",
                wait_unassert_time=float(p.get("WaitUnAssertTime", 0.0)),
                close_immediately=p.get("bCloseImmediately", "").lower() == "true",
                last_key_num=int(p.get("LastKeyNum", 0)))
        elif a["cls"] in ("GradualMover", "MixMover"):
            for source, dest, converter in (
                    ("OpenTimes", "open_times", float),
                    ("CloseTimes", "close_times", float),
                    ("Tags", "tags", str),
                    ("Events", "events", str)):
                values = authored_indexed(p, source, 6, converter)
                if values is not None:
                    subclass[dest] = values
            if a["cls"] == "MixMover" and "AttachTag" in p:
                subclass["attach_tag"] = p["AttachTag"]
        elif a["cls"] == "AttachMover" and "AttachTag" in p:
            subclass["attach_tag"] = p["AttachTag"]

        # Keep this table exactly aligned with subclass .uc defaults, as the
        # class_default walk in import_mover_gameplay.py does for restamps.
        # PortcullisMover and ExplodingMover both override Mover's Return default.
        default_encroach = ("Ignore" if a["cls"] in
                            ("PortcullisMover", "ExplodingMover") else "Return")
        default_damage = 1 if a["cls"] == "PortcullisMover" else 0
        mover = dict(
            name=a["name"], model=model, cls=a["cls"],
            base_pos=base_pos, zone=zone,
            base_rot=tup(p.get("BaseRot", ""), ("pitch", "yaw", "roll")),
            keys={str(i): keys[i] for i in sorted(keys)},
            move_time=float(p.get(
                "MoveTime", 0.0 if a["cls"] == "ExplodingMover" else 1.0)),
            stay_open_time=float(p.get("StayOpenTime", 4.0)),
            delay_time=float(p.get("DelayTime", 0.0)),
            initial_state=p.get(
                "InitialState",
                "" if a["cls"] in SUBCLASS_STATE_MOVERS else "BumpOpenTimed"),
            dynamic_light_mover=(p.get("bDynamicLightMover", "").lower()
                                 == "true"),
            special_lit=p.get("bSpecialLit", "").lower() == "true",
            surfaces=surfaces,
            once_only=p.get(
                "bTriggerOnceOnly",
                "True" if a["cls"] == "ExplodingMover" else "").lower() == "true",
            damage_triggered=p.get("bDamageTriggered", "").lower() == "true",
            encroach_type=encroach_name(
                p.get("MoverEncroachType", default_encroach)),
            encroach_damage=int(p.get("EncroachDamage", default_damage)),
            slave=p.get("bSlave", "").lower() == "true",
            # DoorMover carries an editable bLockable; LockableMover is always a
            # key-gated door by class. bStayUnlocked controls repeated unlock presentation.
            lockable=p.get("bLockable", "").lower() == "true",
            stay_unlocked=p.get("bStayUnlocked", "").lower() == "true",
            tag=p.get("Tag", ""), event=p.get("Event", ""))
        if "bUnlocked" in p:
            mover["unlocked"] = p["bUnlocked"].lower() == "true"
        if "ReturnGroup" in p:
            mover["return_group"] = p["ReturnGroup"]
        lock_fields = {
            "LockedSound": ("locked_sound", sound_subpath),
            "UnLockSound": ("unlock_sound", sound_subpath),
            "LockedMessage": ("locked_message", unreal_string),
            "UnLockMessage": ("unlock_message", unreal_string),
            "LockedMessageDuration": ("locked_message_duration", float),
            "UnLockMessageDuration": ("unlock_message_duration", float),
            "MinLockSoundTime": ("min_lock_sound_time", float),
            "UnLockDuration": ("unlock_duration", float),
        }
        for source, (dest, converter) in lock_fields.items():
            if source in p:
                mover[dest] = converter(p[source])
        mover.update(subclass)
        movers.append(mover)

    def evt_actors(cls, extra):
        out = []
        for a in collect(data, cls):
            p = a["props"]
            e = dict(name=a["name"], location=a["location"],
                     tag=p.get("Tag", ""), event=p.get("Event", ""))
            e.update(extra(p))
            out.append(e)
        return out

    triggers = evt_actors("Trigger", lambda p: dict(
        radius=float(p.get("CollisionRadius", 40)),
        height=float(p.get("CollisionHeight", 40)),
        once_only=p.get("bTriggerOnceOnly", "").lower() == "true",
        retrigger=float(p.get("ReTriggerDelay", 0.0))))
    door_triggers = evt_actors("DoorTrigger", lambda p: dict(
        radius=float(p.get("CollisionRadius", 40)),
        height=float(p.get("CollisionHeight", 40)),
        once_only=p.get("bTriggerOnceOnly", "").lower() == "true",
        retrigger=float(p.get("ReTriggerDelay", 0.0)),
        # The default DIR_Forward is normally omitted from T3D; retain a compact
        # importer-facing value while accepting the exported enum spelling.
        direction=("Backward" if p.get("Direction", "DIR_Forward") ==
                   "DIR_Backward" else "Forward")))
    stage_triggers = evt_actors("StageTrigger", lambda p: dict(
        radius=float(p.get("CollisionRadius", 40)),
        height=float(p.get("CollisionHeight", 40)),
        once_only=p.get("bTriggerOnceOnly", "").lower() == "true",
        retrigger=float(p.get("ReTriggerDelay", 0.0)),
        tag_dec=p.get("TagDec", ""), tag_inc=p.get("TagInc", ""),
        max_key_frame=int(p.get("MaxKeyFrame", 0)),
        water_events=[p.get(f"WaterEvents({i})", "") for i in range(8)],
        dispatcher_tags=[p.get(f"DispatcherTags({i})", "") for i in range(8)]))
    # Portcullis: the invisible archway trigger (Class=Portcullis, not the
    # PortcullisMover brush). Its Event is the tag of the PortcullisMover it owns
    # (Portcullis.Event -> Mover.Tag); the importer sets AWoTPortcullis.MoverTag
    # from it. Collision defaults to the class's 64x160
    # (exports/script/WoTTraps/Portcullis.uc defaultproperties), but instances
    # author overrides (Battle2_02 radius 50 x7, Battle3_02 radius 40/32).
    portcullis_triggers = evt_actors("Portcullis", lambda p: dict(
        radius=float(p.get("CollisionRadius", 64)),
        height=float(p.get("CollisionHeight", 160))))
    def dispatcher_payload(p):
        return dict(
            out_events=[p.get(f"OutEvents({i})", "") for i in range(8)],
            out_delays=[float(p.get(f"OutDelays({i})", 0)) for i in range(8)])
    # WOTDispatcher is the shipped subclass that adds StageTrigger checks; both
    # classes share the same native AWoTDispatcher representation.
    dispatchers = (evt_actors("Dispatcher", dispatcher_payload) +
                   evt_actors("WOTDispatcher", dispatcher_payload))
    counters = evt_actors("Counter", lambda p: dict(
        num=int(p.get("NumToCount", 1))))

    (out_dir / "movers.json").write_text(json.dumps(dict(
        movers=movers, materials=materials, triggers=triggers,
        door_triggers=door_triggers, stage_triggers=stage_triggers,
        portcullis_triggers=portcullis_triggers,
        dispatchers=dispatchers, counters=counters), indent=1))
    print(f"{Path(map_path).stem}: movers={len(movers)} triggers={len(triggers)} "
          f"door_triggers={len(door_triggers)} "
          f"stage_triggers={len(stage_triggers)} "
          f"portcullis_triggers={len(portcullis_triggers)} "
          f"dispatchers={len(dispatchers)} counters={len(counters)}")


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2], sys.argv[3],
            sys.argv[4] if len(sys.argv) > 4 else None)
