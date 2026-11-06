"""Create the fractal live masters, per-record MI_<Name> instances, and
DA_WoTFractalCatalog for Fire/Wet/Wave/Ice.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_flipbooks.py <flipbooks_root>"

The pregenerated SHEET lane is RETIRED (PORT_NOTES "Procedural textures animate when recently seen"):
no sheet textures are imported and no sheet-scrub masters are created. Each
record's MI_<Name> is a pure identity child of M_WoT_FractalLiveAdd (fire) or
M_WoT_FractalLiveLit (wet/wave) — the runtime fractal subsystem binds the live
procedural texture; the parent's default is the never-rendered pre-activation
fallback. Slot-name convention matches map/mesh importers, so surfaces that
referenced a fractal texture pick these up on the next map rebuild.
"""
import base64
import json
import os
import sys
from pathlib import Path

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import local_path  # noqa: E402

RAW_ARGS = sys.argv[1:]
RELEASE_GATED = "--stripped" in RAW_ARGS
POSITIONALS = [argument for argument in RAW_ARGS
               if not argument.startswith("--")]

# Fractal-source SEED fields hold the retail p8 (palette indices + initial
# bitmaps) the runtime sim runs from. They are embedded in DA_WoTFractalCatalog
# and thus ship retail pixel data into the stripped pak (the sole remaining
# packaged-data leak; PORT_NOTES "Built from the player's own copy of the game"). The clean fix rehydrates the seed from
# the payload cache at bind time via the record's source_texture_ref /
# glass_texture_ref. That needs the Windows WoTFractalSubsystem C++ half; until
# it lands this stays DORMANT (default off) so the current fleet is unchanged.
# Activation is a single opt-in flag, flipped in lockstep with the C++.
FRACTAL_SEED_STRIP_ENV = "WOT_STRIP_FRACTAL_SEED"
_FRACTAL_SEED_PROPS = (
    "palette_rgba", "initial_bitmap", "source_indices", "glass_indices")


def fractal_seed_stripped(environ=None):
    """True when the stripped-pak fractal-seed strip is opted in (coordinated
    with the Windows cache-rehydration; dormant/off by default)."""
    env = os.environ if environ is None else environ
    return env.get(FRACTAL_SEED_STRIP_ENV, "0") == "1"


def fractal_record_sources_retail(info):
    """True when a fractal record's seed is a COPY of a retail SOURCE texture
    (wet/ice: palette/indices/initial are the source's payload). Those are the
    only fractal records that leak retail pixels, and the runtime rehydrates them
    from the payload cache by `source_texture`. Records with no source
    (fire/wave: baker-GENERATED palette+initial, never a leak) or a live-fractal
    source keep their embedded seed — the runtime cannot rehydrate what the cache
    does not hold, and there is nothing retail to strip."""
    return bool(info.get("source_texture")) and not info.get(
        "source_is_fractal", False)


def fractal_seed_property_value(info, prop, environ=None):
    """Decoded retail p8 seed bytes for a fractal record property, or [] when the
    stripped-pak seed strip is opted in AND the record copies a retail source —
    the runtime then rehydrates that seed from the payload cache. Baker-generated
    (fire/wave) seeds are kept so their sims survive the stripped pak."""
    if fractal_seed_stripped(environ) and fractal_record_sources_retail(info):
        return []
    return list(base64.b64decode(info.get(prop, "")))

ROOT = Path(POSITIONALS[0] if POSITIONALS
            else local_path("conversion_root") / "flipbooks")
ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary
CATALOG_PATH = "/Game/WoT/Data/DA_WoTFractalCatalog"

def make_live_master(name, additive):
    """Identity-UV counterpart of a shared flipbook master."""
    full = f"/Game/WoT/Materials/{name}"
    if EAL.does_asset_exist(full):
        return full
    m = ATT.create_asset(name, "/Game/WoT/Materials", unreal.Material,
                         unreal.MaterialFactoryNew())
    tc = MEL.create_material_expression(
        m, unreal.MaterialExpressionTextureCoordinate, -400, 0)
    tex = MEL.create_material_expression(
        m, unreal.MaterialExpressionTextureSampleParameter2D, -200, 0)
    tex.set_editor_property("parameter_name", "Tex")
    MEL.connect_material_expressions(tc, "", tex, "UVs")
    if additive:
        m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
        m.set_editor_property("shading_model",
                              unreal.MaterialShadingModel.MSM_UNLIT)
        MEL.connect_material_property(
            tex, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    else:
        rough = MEL.create_material_expression(
            m, unreal.MaterialExpressionConstant, -200, 250)
        rough.set_editor_property("r", 1.0)
        MEL.connect_material_property(rough, "",
                                      unreal.MaterialProperty.MP_ROUGHNESS)
        MEL.connect_material_property(
            tex, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.recompile_material(m)
    EAL.save_asset(full)
    return full


def _runtime_record(package, asset_key, info):
    record = unreal.WoTFractalTextureRecord()
    enum_name = info["class_name"].removesuffix("Texture").upper()
    record.set_editor_property("package_name", unreal.Name(package))
    record.set_editor_property(
        "texture_name", unreal.Name(info.get("texture_name", asset_key)))
    record.set_editor_property(
        "object_path", info.get("object_path", f"{package}.{asset_key}"))
    record.set_editor_property(
        "fractal_class", getattr(unreal.WoTFractalClass, enum_name))
    for prop in ("width", "height", "prime_count", "wave_amp",
                 "bump_map_light", "bump_map_angle", "phong_range",
                 "phong_size", "render_heat", "sparks_limit",
                 "source_width", "source_height", "glass_width",
                 "glass_height", "panning_style", "time_method",
                 "horiz_pan_speed", "vert_pan_speed", "frequency",
                 "amplitude"):
        record.set_editor_property(prop, int(info.get(prop, 0)))
    for prop in ("min_frame_rate", "max_frame_rate", "master_count",
                 "u_displace", "v_displace", "u_position", "v_position"):
        record.set_editor_property(prop, float(info.get(prop, 0.0)))
    record.set_editor_property("rising", bool(info.get("rising", False)))
    record.set_editor_property("move_ice", bool(info.get("move_ice", False)))
    record.set_editor_property(
        "source_texture_ref", info.get("source_texture", ""))
    record.set_editor_property(
        "source_is_fractal", bool(info.get("source_is_fractal", False)))
    record.set_editor_property(
        "glass_texture_ref", info.get("glass_texture", ""))
    record.set_editor_property(
        "glass_is_fractal", bool(info.get("glass_is_fractal", False)))
    for prop in _FRACTAL_SEED_PROPS:
        record.set_editor_property(prop, fractal_seed_property_value(info, prop))
    drops = []
    for raw in info["drops"]:
        drop = unreal.WoTFractalDrop()
        for prop, value in zip(
                ("type", "depth", "x", "y", "byte_a", "byte_b",
                 "byte_c", "byte_d"), raw):
            drop.set_editor_property(prop, int(value))
        drops.append(drop)
    record.set_editor_property("drops", drops)
    sparks = []
    for raw in info.get("sparks", []):
        spark = unreal.WoTFractalSpark()
        for prop, value in zip(
                ("type", "heat", "x", "y", "byte_a", "byte_b",
                 "byte_c", "byte_d"), raw):
            spark.set_editor_property(prop, int(value))
        sparks.append(spark)
    record.set_editor_property("sparks", sparks)
    return record


def save_runtime_catalog(records, live_lit_master, live_add_master):
    catalog_class = unreal.load_class(None, "/Script/WoT.WoTFractalCatalog")
    if not catalog_class:
        raise RuntimeError(
            "UWoTFractalCatalog not loaded - build the WoT editor module first")
    if EAL.does_asset_exist(CATALOG_PATH):
        catalog = unreal.load_asset(CATALOG_PATH)
    else:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", catalog_class)
        catalog = ATT.create_asset(
            "DA_WoTFractalCatalog", "/Game/WoT/Data", catalog_class, factory)
    if not catalog:
        raise RuntimeError(f"failed to create {CATALOG_PATH}")
    catalog.set_editor_property("textures", records)
    catalog.set_editor_property("live_lit_master", live_lit_master)
    catalog.set_editor_property("live_add_master", live_add_master)
    EAL.save_asset(CATALOG_PATH)


def import_static_frame(package, info, source_dir):
    """Import a representative procedural frame for gameplay MID Tex identity.

    The live subsystem replaces this texture on the following tick. It is not a
    pregenerated animation sheet and never advances at runtime.
    """
    filename = info.get("static_frame")
    if not filename:
        return
    texture_name = info.get("texture_name")
    if not texture_name:
        raise RuntimeError(f"{package}: static frame has no texture_name")
    destination = f"/Game/WoT/Textures/{package}/{texture_name}"
    if EAL.does_asset_exist(destination):
        return
    source = source_dir / filename
    if not source.exists():
        raise RuntimeError(f"missing procedural static frame {source}")
    task = unreal.AssetImportTask()
    task.filename = str(source)
    task.destination_path = f"/Game/WoT/Textures/{package}"
    task.destination_name = texture_name
    task.automated = True
    task.save = True
    ATT.import_asset_tasks([task])
    if not EAL.does_asset_exist(destination):
        raise RuntimeError(f"failed to import {destination} from {source}")


def main():
    live_add_master = unreal.load_asset(
        make_live_master("M_WoT_FractalLiveAdd", True))
    live_lit_master = unreal.load_asset(
        make_live_master("M_WoT_FractalLiveLit", False))
    n_mi = 0
    runtime = []
    for man in sorted(ROOT.glob("*/_flipbooks.json")):
        pkgname = man.parent.name
        data = json.loads(man.read_text())
        for name, info in data["fire"].items():
            import_static_frame(pkgname, info, man.parent)
            mi_path = f"/Game/WoT/Materials/Instances/MI_{name}"
            if not EAL.does_asset_exist(mi_path):
                mi = ATT.create_asset(f"MI_{name}",
                                      "/Game/WoT/Materials/Instances",
                                      unreal.MaterialInstanceConstant,
                                      unreal.MaterialInstanceConstantFactoryNew())
                n_mi += 1
            else:
                mi = unreal.load_asset(mi_path)
            # Restamp every run: an identity child of the live master, with any
            # legacy SheetTex/grid overrides dropped so the retired sheet assets
            # hold no referencers.
            is_fire = info.get("kind") not in ("wet", "wave")
            dirty = False
            parent = live_add_master if is_fire else live_lit_master
            if mi.get_editor_property("parent") != parent:
                mi.set_editor_property("parent", parent)
                dirty = True
            if mi.get_editor_property("texture_parameter_values")                     or mi.get_editor_property("scalar_parameter_values"):
                mi.set_editor_property("texture_parameter_values", [])
                mi.set_editor_property("scalar_parameter_values", [])
                dirty = True
            if dirty:
                EAL.save_asset(mi_path)
        if not RELEASE_GATED:
            for name, info in sorted(data.get("runtime", {}).items()):
                runtime.append(_runtime_record(pkgname, name, info))
    runtime.sort(key=lambda item: str(
        item.get_editor_property("object_path")).casefold())
    if not RELEASE_GATED:
        save_runtime_catalog(runtime, live_lit_master, live_add_master)
    unreal.log(
        f"FLIPBOOKS-OK instances={n_mi} runtime={len(runtime)} "
        f"mode={'guarded' if RELEASE_GATED else 'developer'}")


main()
