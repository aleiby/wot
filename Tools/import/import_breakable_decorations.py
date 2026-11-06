"""Upgrade retail BreakableDecoration authors to AWoTBreakableDecoration (G36).

The only MP Frag() authors are Arena_06's BarrelTap x8 (the Arena_12/Battle2_02
WireLamps expand WOTDecoration directly — no Health/fragmentClass, census-closed
as plain decorations). place_decorations (import_map.py:3446-3487) placed the
barrels as visual-only AWoTDecoration; this patcher swaps each one for an
AWoTBreakableDecoration with the leaf-CDO break config and the AUTHORED
per-instance collision. NOTE: the T3D authors are NOT pure-CDO — six barrels
carry DrawScale=2 with CollisionRadius=55/CollisionHeight=40 and two carry
DrawScale=1.1 with 28/21.5-ish cylinders; honoring the authored world-space
collision is what makes the two stacked piles actually rest on each other
(support-loss collapse, PORT_NOTES G36). DrawScale rides the MESH component
relative scale (the collision root stays unscaled so UE1's world-unit
CollisionRadius/Height stamp 1:1).

Mirrors import_exploding_walls.py / import_path_nodes.py: plan_map() is
stdlib-only for dry inspection; re-runs are idempotent by stable T3D label
(already-upgraded actors are just re-stamped), one save per map.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_breakable_decorations.py [<Map> ...]"
"""
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import T3D_DIR  # noqa: E402
from t3d import parse, typed  # noqa: E402


# The only map with retail BreakableDecoration authors (repo-wide t3d census).
BREAKABLE_MAPS = ["Arena_06"]

# class -> (mesh asset dir name, leaf CDO defaults). BarrelTap.uc:21-31 over
# BreakableDecoration.uc:117-129: Health=20, WoodFragments, fragmentSize=1.75,
# Mass=60, CollisionRadius=29 (Breakable CDO), CollisionHeight=22 (BarrelTap).
BREAKABLE_CLASSES = {
    "BarrelTap": {
        "mesh": "BarrelTap",
        "fragment_type": "Wood",  # fragmentClass=Class'WOT.WoodFragments'
        "health": 20,
        "fragment_size": 1.75,
        "mass": 60.0,
        "min_damage_velocity_z": 500,
        "collision_radius": 29.0,
        "collision_height": 22.0,
    },
}

# Importer-baked MeshLight state (UWoTMeshLight, gamma scheme) survives the
# decoration -> breakable swap by copying the component's editor properties.
# Reflected names drop the b prefix on bools (baked).
MESH_LIGHT_PROPS = (
    "material_slot", "local_origin", "local_normal", "light_pos_r",
    "light_color", "light_wave_group", "light_dir_cone", "wave_means",
    "wave_amplitudes", "mover_surfaces", "ambient", "scale_glow", "baked",
)


def _bool(value):
    return str(value or "").lower() == "true"


def plan_map(map_name):
    """Return the BreakableDecoration stamps from one retail T3D."""
    stamps = []
    for actor in typed(parse(T3D_DIR / f"{map_name}.t3d")):
        cdo = BREAKABLE_CLASSES.get(actor["cls"])
        if cdo is None:
            continue
        props = actor.get("props", {})
        stamps.append({
            "label": actor["name"],
            "mesh": cdo["mesh"],
            "fragment_type": cdo["fragment_type"],
            "location": actor["location"],
            "rotation": actor["rotation"],
            "draw_scale": float(props.get("DrawScale", 1.0)),
            # Instance overrides beat the leaf CDO; the CDO backfills the
            # unexported slots (T3D exports only non-default values).
            "health": int(props.get("Health", cdo["health"])),
            "fragment_size": float(props.get("fragmentSize",
                                             cdo["fragment_size"])),
            "mass": float(props.get("Mass", cdo["mass"])),
            "min_damage_velocity_z": int(props.get(
                "MinDamageVelocityZ", cdo["min_damage_velocity_z"])),
            "collision_radius": float(props.get("CollisionRadius",
                                                cdo["collision_radius"])),
            "collision_height": float(props.get("CollisionHeight",
                                                cdo["collision_height"])),
            "event_name": props.get("Event", ""),
            "only_triggerable": _bool(props.get("bOnlyTriggerable")),
        })
    return stamps


def _rotator(unreal, rotation):
    scale = 360.0 / 65536.0
    return unreal.Rotator(roll=rotation[2] * scale,
                          pitch=rotation[0] * scale,
                          yaw=rotation[1] * scale)


def _find_skeletal_mesh(unreal, mesh_name):
    """The place_decorations mesh lookup (import_map.py:3462-3469)."""
    base = f"/Game/WoT/Meshes/{mesh_name}"
    if not unreal.EditorAssetLibrary.does_directory_exist(base):
        return None
    for asset in unreal.EditorAssetLibrary.list_assets(base, recursive=True):
        obj = unreal.load_asset(asset.split(".")[0])
        if isinstance(obj, unreal.SkeletalMesh):
            return obj
    return None


def _copy_mesh_light(unreal, source, target):
    """Preserve the baked actor lighting across the actor swap."""
    src = source.get_editor_property("mesh_light")
    dst = target.get_editor_property("mesh_light")
    if src is None or dst is None:
        return
    for prop in MESH_LIGHT_PROPS:
        try:
            dst.set_editor_property(prop, src.get_editor_property(prop))
        except Exception:  # noqa: BLE001 — optional per-branch properties
            continue


def _upgrade_actor(unreal, eas, actors, stamp):
    """Swap the label's AWoTDecoration for an AWoTBreakableDecoration."""
    location = unreal.Vector(*stamp["location"])
    matches = [a for a in actors if a.get_actor_label() == stamp["label"]]
    keep = next((a for a in matches
                 if isinstance(a, unreal.WoTBreakableDecoration)), None)
    donor = next((a for a in matches
                  if a is not keep and isinstance(a, unreal.WoTDecoration)),
                 None)
    if keep is None:
        keep = eas.spawn_actor_from_class(unreal.WoTBreakableDecoration,
                                          location)
        keep.set_actor_label(stamp["label"])
        actors.append(keep)
    if donor is not None:
        _copy_mesh_light(unreal, donor, keep)
    for actor in matches:
        if actor is not keep:
            eas.destroy_actor(actor)
            actors.remove(actor)
    keep.set_actor_location(location, False, False)
    if any(stamp["rotation"]):
        keep.set_actor_rotation(_rotator(unreal, stamp["rotation"]), False)

    # Rebind the visual exactly as place_decorations does (import_map.py:
    # 3473-3483): skeletal mesh + both lighting channels. DrawScale goes on the
    # MESH relative scale, not the actor — the collision root carries UE1's
    # world-unit collision. No BarrelTap author has Skin=/Style= (t3d census),
    # so the _ml_skin_override leg is not applicable here.
    mesh_component = keep.get_editor_property("mesh")
    mesh_component.set_editor_property(
        "lighting_channels",
        unreal.LightingChannels(channel0=True, channel1=True))
    asset = _find_skeletal_mesh(unreal, stamp["mesh"])
    if asset is not None:
        mesh_component.set_skeletal_mesh_asset(asset)
    else:
        unreal.log(f"BREAKABLE-SKIP {stamp['label']}: "
                   f"mesh {stamp['mesh']!r} unavailable")
    scale = stamp["draw_scale"]
    mesh_component.set_editor_property(
        "relative_scale3d", unreal.Vector(scale, scale, scale))

    # Break config (reflected names drop the b prefix on bools).
    for prop in ("health", "fragment_size", "mass", "min_damage_velocity_z",
                 "only_triggerable", "event_name"):
        keep.set_editor_property(prop, stamp[prop])
    keep.set_editor_property(
        "fragment_type",
        getattr(unreal.WoTFragmentType, stamp["fragment_type"].upper()))
    # Box stand-in for the UE1 squat cylinder: extent (R, R, HalfHeight).
    keep.get_editor_property("collision").set_box_extent(unreal.Vector(
        stamp["collision_radius"], stamp["collision_radius"],
        stamp["collision_height"]))
    return keep


def main():
    import unreal

    maps = [name for name in sys.argv[1:] if not name.startswith("-")] \
        or BREAKABLE_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"BREAKABLE {map_name} skipped: no imported map")
            continue
        if not les.load_level(asset):
            unreal.log_error(f"BREAKABLE {map_name}: load_level failed")
            continue
        plan = plan_map(map_name)
        actors = list(eas.get_all_level_actors())
        for stamp in plan:
            _upgrade_actor(unreal, eas, actors, stamp)
        les.save_current_level()
        unreal.log(f"BREAKABLE {map_name}: breakables={len(plan)}")


if __name__ == "__main__":
    main()
