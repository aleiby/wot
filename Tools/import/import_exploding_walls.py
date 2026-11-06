"""Patch retail WOTExplodingWall actors into live Battle maps (G35).

The six MP authors (Battle2_05 x3 + Battle4_01 x3, identical configs) are
invisible 32x32 projectile occluders whose break path is activation-dead in
shipped MP (ActivatedBy=break01, which nothing emits); the
stamp restores the parity-locked presence plus the full authored break config
for the faithful AWoTExplodingWall port. Mirrors the import_path_nodes.py
patcher precedent: plan_map() is stdlib-only for dry inspection; re-runs update
actors by stable T3D label, remove duplicates, and save each loaded map once.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_exploding_walls.py [<Map> ...]"
"""
import re
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import T3D_DIR  # noqa: E402
from t3d import parse, typed  # noqa: E402


# The only maps with retail WOTExplodingWall authors (repo-wide t3d census).
WALL_MAPS = ["Battle2_05", "Battle4_01"]

# Reference-name conventions shared with the mover pipeline
# (Tools/convert/movers2glb.py:43-44,98-111): textures fold to the bare imported
# asset name; sounds fold to the 'Pkg/Name' subpath WoTSound::LoadBySubPath
# resolves under /Game/WoT/Sounds.
SOUND_RE = re.compile(r"Sound'([^'.]+)\.(?:[^'.]+\.)?([^'.]+)'")
ASSET_RE = re.compile(r"(?:Texture|Material)'(?:[^'.]+\.)*([^'.]+)'")


def asset_name(value):
    """Texture'Pkg.Group.Name' -> bare imported asset name."""
    if not value or value.strip() in ("", "None"):
        return ""
    match = ASSET_RE.search(value)
    return match.group(1) if match else ""


def sound_subpath(value):
    """Sound'Pkg.Group.Name' -> 'Pkg/Name' (sound import folds the group)."""
    if not value or value.strip() in ("", "None"):
        return ""
    match = SOUND_RE.search(value)
    return f"{match.group(1)}/{match.group(2)}" if match else ""


def _bool(value):
    return str(value or "").lower() == "true"


def indexed(props, name, count, default):
    """Read a UE1 fixed array, backfilling class defaults per slot.

    T3D exports only the slots that differ from the class default
    (movers2glb.py indexed() convention); WOTExplodingWall.uc defaults
    ActivatedBy(0)=exploded with slots 1-4 empty.
    """
    return [props.get(f"{name}({i})", default[i]) for i in range(count)]


def plan_map(map_name):
    """Return the WOTExplodingWall stamps from one retail T3D."""
    walls = []
    for actor in typed(parse(T3D_DIR / f"{map_name}.t3d")):
        if actor["cls"] != "WOTExplodingWall":
            continue
        props = actor.get("props", {})
        walls.append({
            "label": actor["name"],
            "location": actor["location"],
            "rotation": actor["rotation"],
            # WOTExplodingWall.uc defaultproperties backfill the unexported
            # slots (the six MP authors export size/wood/chunks/health/gate).
            "explosion_size": float(props.get("ExplosionSize", 200.0)),
            "explosion_dimensions": float(props.get("ExplosionDimensions", 120.0)),
            "wall_particle_size": float(props.get("WallParticleSize", 1.0)),
            "wood_particle_size": float(props.get("WoodParticleSize", 1.0)),
            "glass_particle_size": float(props.get("GlassParticleSize", 1.0)),
            "num_wall_chunks": int(props.get("NumWallChunks", 10)),
            "num_wood_chunks": int(props.get("NumWoodChunks", 3)),
            "num_glass_chunks": int(props.get("NumGlassChunks", 0)),
            "wall_texture_name": asset_name(props.get("WallTexture")),
            "wood_texture_name": asset_name(props.get("WoodTexture")),
            "glass_texture_name": asset_name(props.get("GlassTexture")),
            "health": int(props.get("Health", 0)),
            "activated_by": indexed(props, "ActivatedBy", 5,
                                    ["exploded", "", "", "", ""]),
            "breaking_sound": sound_subpath(props.get("BreakingSound")),
            "translucent_glass": _bool(props.get("bTranslucentGlass")),
            "unlit_glass": _bool(props.get("bUnlitGlass")),
            "only_triggerable": _bool(props.get("bOnlyTriggerable")),
            "tag_name": props.get("Tag", ""),
            "event_name": props.get("Event", ""),
        })
    return walls


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

    maps = [name for name in sys.argv[1:] if not name.startswith("-")] or WALL_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"EXPLODINGWALL {map_name} skipped: no imported map")
            continue
        if not les.load_level(asset):
            unreal.log_error(f"EXPLODINGWALL {map_name}: load_level failed")
            continue
        plan = plan_map(map_name)
        actors = list(eas.get_all_level_actors())
        for stamp in plan:
            wall = _single_actor(eas, actors, stamp["label"],
                                 unreal.WoTExplodingWall,
                                 unreal.Vector(*stamp["location"]))
            if any(stamp["rotation"]):
                wall.set_actor_rotation(_rotator(unreal, stamp["rotation"]), False)
            # Reflected names drop the b prefix on bools (translucent_glass ...),
            # matching the ExplodingMover stamping in import_map.py:1839-1858.
            for prop in ("explosion_size", "explosion_dimensions",
                         "wall_particle_size", "wood_particle_size",
                         "glass_particle_size", "num_wall_chunks",
                         "num_wood_chunks", "num_glass_chunks",
                         "wall_texture_name", "wood_texture_name",
                         "glass_texture_name", "health", "breaking_sound",
                         "translucent_glass", "unlit_glass",
                         "only_triggerable", "tag_name", "event_name"):
                wall.set_editor_property(prop, stamp[prop])
            wall.set_editor_property(
                "activated_by", [unreal.Name(v) for v in stamp["activated_by"]])
        les.save_current_level()
        unreal.log(f"EXPLODINGWALL {map_name}: walls={len(plan)}")


if __name__ == "__main__":
    main()
