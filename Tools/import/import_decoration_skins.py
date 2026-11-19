"""Restamp placed-decoration procedural skins and collision on already-imported
MP maps, as a fresh import_map.py now places them.

* A Fire/Wet/IceTexture slot-0 skin (Skin= or MultiSkins(0)=) has no still
  asset. The decoration gets its fractal identity instance (MI_MeshT_<Pkg>__<Tex>
  for STY_Translucent, else MI_Skin_<Pkg>__<Tex><type>) on the neutral
  pre-activation texture; the fractal subsystem binds the live simulation.
  Earlier imports skipped these skins and left the class skin showing:
  Arena_06's teleporter shimmer quads (WetTexture'ScottT.S_Tele_effect') drew
  JTwoTriangles0.
* A decoration whose retail bCollideActors is off (instance or shipped class)
  neither touches nor blocks. Earlier imports left every decoration on the
  class-default blocking cylinder, scaled with DrawScale.

The stdlib plan_map() half imports without unreal for dry-runs.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_decoration_skins.py [<Map> ...]"
"""
import re
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import MP_MAPS, SCRIPT, T3D_DIR  # noqa: E402
from mesh_material_types import (  # noqa: E402
    GAMMA_SLOT_MASTERS,
    actor_style_suffix,
    effective_material_type_suffix,
    material_type_suffix,
    procedural_skin_identity,
    procedural_skin_slug,
    slot_zero_skin_reference,
)
from t3d import parse, typed  # noqa: E402
from texture_sampling import WORLD, verify_texture_sampling  # noqa: E402

MATERIAL_ROOT = "/Game/WoT/Materials"
INSTANCE_ROOT = "/Game/WoT/Materials/Instances"
NEUTRAL_TEXTURE = "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral"

_uc_files = None
_uc_cache = {}


def _class_defaults(cls):
    """(parent, {key: value}) of a shipped class's defaultproperties."""
    global _uc_files
    key = cls.lower()
    if key in _uc_cache:
        return _uc_cache[key]
    if _uc_files is None:
        _uc_files = {path.stem.lower(): path for path in SCRIPT.rglob("*.uc")}
    parent, values = None, {}
    path = _uc_files.get(key)
    if path:
        text = path.read_text(encoding="latin-1")
        match = re.search(r"^\s*class\s+\w+\s+(?:expands|extends)\s+(\w+)",
                          text, re.M | re.I)
        parent = match.group(1) if match else None
        block = re.search(r"^defaultproperties\s*\{(.*?)^\}", text, re.M | re.S | re.I)
        if block:
            for line in block.group(1).splitlines():
                name, sep, value = line.strip().partition("=")
                if sep:
                    values.setdefault(name.strip(), value.strip())
    _uc_cache[key] = (parent, values)
    return parent, values


def class_property(cls, name):
    """A class-default property through the shipped .uc chain, else None."""
    seen = set()
    while cls and cls.lower() not in seen:
        seen.add(cls.lower())
        parent, values = _class_defaults(cls)
        if name in values:
            return values[name]
        cls = parent
    return None


def _is_true(value):
    return value is True or str(value).strip().lower() == "true"


def decoration_plan(actor):
    """The restamp payload for one T3D actor, or None when nothing applies."""
    props = actor.get("props", {})
    procedural = procedural_skin_identity(slot_zero_skin_reference(props))
    collide = props.get("bCollideActors")
    if collide is None:
        collide = class_property(actor["cls"], "bCollideActors")
    collision_off = collide is not None and not _is_true(collide)
    if procedural is None and not collision_off:
        return None
    style = props.get("Style")
    if style is None:
        style = class_property(actor["cls"], "Style") or ""
    return {"skin": procedural, "style": str(style), "collision_off": collision_off}


def plan_map(map_name):
    """actor label -> restamp payload, for the actors that need one."""
    plan = {}
    for actor in typed(parse(T3D_DIR / f"{map_name}.t3d")):
        payload = decoration_plan(actor)
        if payload is not None:
            plan[actor["name"]] = payload
    return plan


def skin_instance(style, slot_suffix, package, texture):
    """(instance name, master name) of a procedural skin, as import_map.py
    names it: the identity slug carries the record to the fractal subsystem."""
    stem = procedural_skin_slug(package, texture)
    if actor_style_suffix(style) == "_T":
        return f"MI_MeshT_{stem}", GAMMA_SLOT_MASTERS["_T"]
    type_suffix = effective_material_type_suffix(slot_suffix, style)
    return f"MI_Skin_{stem}{type_suffix}", GAMMA_SLOT_MASTERS[type_suffix]


def _root_name(unreal, material):
    current = material
    while isinstance(current, unreal.MaterialInstance):
        parent = current.get_editor_property("parent")
        if parent is None:
            break
        current = parent
    return current.get_name() if current is not None else ""


def restamp_skin(unreal, actor, payload):
    """Bind the identity instance on slot 0; return whether it changed."""
    EAL = unreal.EditorAssetLibrary
    MEL = unreal.MaterialEditingLibrary
    component = actor.get_editor_property("mesh")
    current = component.get_material(0)
    slot_suffix = material_type_suffix(_root_name(unreal, current))
    package, texture = payload["skin"]
    name, master_name = skin_instance(payload["style"], slot_suffix, package, texture)
    if current is not None and current.get_name() == name:
        return False
    path = f"{INSTANCE_ROOT}/{name}"
    if EAL.does_asset_exist(path):
        instance = unreal.load_asset(path)
    else:
        instance = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, INSTANCE_ROOT, unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew())
    master = unreal.load_asset(f"{MATERIAL_ROOT}/{master_name}")
    if master is None:
        raise RuntimeError(f"missing {MATERIAL_ROOT}/{master_name}")
    # The world-family neutral import_map.py owns (_import_neutral); read only.
    neutral = verify_texture_sampling(unreal, NEUTRAL_TEXTURE, WORLD, gamma_space=True)
    if instance.get_editor_property("parent") != master:
        instance.set_editor_property("parent", master)
    MEL.set_material_instance_texture_parameter_value(instance, "Tex", neutral)
    EAL.save_asset(path)
    component.set_material(0, instance)
    return True


def restamp_collision(actor, payload):
    """Turn a non-colliding decoration's cylinder off; return whether it changed."""
    if not payload["collision_off"] or not actor.get_editor_property("collide_actors"):
        return False
    actor.set_ue1_collision(False, False, False, 22.0, 22.0)
    return True


def main():
    import unreal

    maps = [m for m in sys.argv[1:] if not m.startswith("-")] or MP_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"DECOSKINS {map_name} skins=0 collision=0 miss=0")
            continue
        plan = plan_map(map_name)
        if not plan:
            unreal.log(f"DECOSKINS {map_name} skins=0 collision=0 miss=0")
            continue
        les.load_level(asset)
        by_label = {actor.get_actor_label(): actor
                    for actor in eas.get_all_level_actors()}
        skins = collision = miss = 0
        for label, payload in plan.items():
            actor = by_label.get(label)
            if not isinstance(actor, unreal.WoTDecoration):
                # Only placed decorations are restamped; a missing procedural
                # skin consumer is worth a count.
                miss += int(payload["skin"] is not None)
                continue
            if payload["skin"] is not None and restamp_skin(unreal, actor, payload):
                skins += 1
            if restamp_collision(actor, payload):
                collision += 1
        if skins or collision:
            les.save_current_level()
        unreal.log(f"DECOSKINS {map_name} skins={skins} collision={collision} miss={miss}")


if __name__ == "__main__":
    main()
