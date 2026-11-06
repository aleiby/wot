"""Generate M_WoT_EntryTranslucent_Mirror, the retail PF_Mirrored composite.

Retail (the original renderer's OccludeBsp and DrawFrame): the mirror child
frame is drawn first, then the PF_Translucent mirror surface over it with
PF_Occlude.  Single-TMU Glide draws a lit translucent surface as the texture
pass ONE/ONE_MINUS_SRC_COLOR and the light-map pass DST_COLOR/SRC_COLOR:

    FB = M * (Tex + Dst * (1 - Tex))      per channel, Dst = the reflection

with M the scale-2 light map (plus the G45 dynamic term) exactly as the
Entry-family masters and setup_childsky.scene_blend_master compute it.  The
reflection R is the mirror capture's render target sampled at the surface
point's projection into the capture (WoTRuntimeMirror.cpp
WoTMirrorProjectToCapture is the C++ twin of PROJECTION_HLSL).  Because R
already holds the complete reflected scene, the result does not depend on
what lies behind the sheet: the master is OPAQUE (writes depth like
PF_Occlude) and unlit, one-sided like every census mirror.  It pans Tex on
WoTSceneTime like every Entry-family master (the runtime world drives it).

Run inside Unreal's Python commandlet with the closed destination:

    UnrealEditor-Cmd.exe WoT.uproject -run=pythonscript \
      -script="Tools/import/mirror_material.py --output-root /Game/WoT/Materials"

Import-safe: ``unreal`` is only imported by the command-line entry point.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

MATERIAL_ROOT = "/Game/WoT/Materials"
MATERIAL_NAME = "M_WoT_EntryTranslucent_Mirror"
MATERIAL_PATH = f"{MATERIAL_ROOT}/{MATERIAL_NAME}"
NEUTRAL_TEXTURE = "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral"
GRAPH_TAG = "WoT.MirrorGraph"
GRAPH_VERSION = "mirror-v1"
RECEIPT_FORMAT = "wot-mirror-material-receipt-v1"

# Parameter names shared with Source/WoT/World/WoTRuntimeMirror.h and the
# section parameters every runtime BSP MID carries (WoTWorldMaterializer.cpp).
SECTION_TEXTURES = ("Tex", "Lightmap")
SECTION_SCALARS = ("PanU", "PanV", "WoTSceneTime", "DynLightEnable")
REFLECTION_PARAMETER = "Reflection"
VIEW_VECTORS = ("MirrorOrigin", "MirrorAxisX", "MirrorAxisY", "MirrorAxisZ",
                "MirrorTan")
LIGHTMAP_MULTIPLIER = 2.0

# Capture-target UV of the surface point (u right, v down): the capture's own
# perspective (SceneCaptureRendering.cpp BuildProjectionMatrix) with the axes
# and tangents the runtime driver writes every frame.
PROJECTION_HLSL = """
float3 D = WP - Origin.xyz;
float X = max(dot(D, AxisX.xyz), 1e-4);
return float2(0.5 + 0.5 * dot(D, AxisY.xyz) / (X * Tan.x),
              0.5 - 0.5 * dot(D, AxisZ.xyz) / (X * Tan.y));
""".strip()


def project_to_capture(point, origin, axis_x, axis_y, axis_z, tan_xy):
    """Python twin of PROJECTION_HLSL (and WoTMirrorProjectToCapture)."""
    delta = [p - o for p, o in zip(point, origin)]

    def dot(a, b):
        return sum(x * y for x, y in zip(a, b))

    depth = max(dot(delta, axis_x), 1e-4)
    return (0.5 + 0.5 * dot(delta, axis_y) / (depth * tan_xy[0]),
            0.5 - 0.5 * dot(delta, axis_z) / (depth * tan_xy[1]))


def composite(texel, reflection, light):
    """Per-channel retail translucent-over-mirror result M*(T + R*(1-T))."""
    return tuple(m * (t + r * (1.0 - t))
                 for t, r, m in zip(texel, reflection, light))


def graph_contract() -> dict[str, Any]:
    return {
        "material": MATERIAL_NAME,
        "blend": "opaque",
        "shading_model": "unlit",
        "two_sided": False,
        "section_textures": SECTION_TEXTURES,
        "section_scalars": SECTION_SCALARS,
        "reflection_parameter": REFLECTION_PARAMETER,
        "view_vectors": VIEW_VECTORS,
        "lightmap_multiplier": LIGHTMAP_MULTIPLIER,
        "lightmap_uv_channel": 1,
        "pan_time_parameter": "WoTSceneTime",
        "dynamic_light_default": 0.0,
        "texture_defaults": NEUTRAL_TEXTURE,
        "projection_hlsl": PROJECTION_HLSL,
    }


def validate_output_root(output_root: str) -> str:
    if output_root != MATERIAL_ROOT:
        raise ValueError(
            f"output root must be the closed material root {MATERIAL_ROOT}")
    return output_root


def build(unreal: Any) -> Any:
    """(Re)create the master from scratch. A stale asset is deleted, never
    edited in place (the pinned engine's delete_all_material_expressions
    mutates its collection mid-loop - entry_weather_materials.py)."""
    import sys
    here = Path(__file__).resolve().parent
    if str(here) not in sys.path:
        sys.path.insert(0, str(here))
    from dynamic_light_material import (  # noqa: E402
        DYNAMIC_LIGHT_GRAPH_TAG,
        DYNAMIC_LIGHT_GRAPH_VERSION,
        build_dynamic_world_factor,
    )

    eal = unreal.EditorAssetLibrary
    mel = unreal.MaterialEditingLibrary
    if not eal.does_asset_exist(NEUTRAL_TEXTURE):
        raise RuntimeError(f"approved neutral texture is missing: {NEUTRAL_TEXTURE}")
    neutral = unreal.load_asset(NEUTRAL_TEXTURE)
    if eal.does_asset_exist(MATERIAL_PATH) and not eal.delete_asset(MATERIAL_PATH):
        raise RuntimeError(f"could not replace stale {MATERIAL_PATH}")
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        MATERIAL_NAME, MATERIAL_ROOT, unreal.Material,
        unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError(f"could not create {MATERIAL_PATH}")
    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("two_sided", False)

    def expr(cls, x, y):
        return mel.create_material_expression(material, cls, x, y)

    def texture_parameter(name, x, y):
        node = expr(unreal.MaterialExpressionTextureSampleParameter2D, x, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("texture", neutral)
        node.set_editor_property(
            "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        return node

    def scalar_parameter(name, x, y, default=0.0):
        node = expr(unreal.MaterialExpressionScalarParameter, x, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", default)
        return node

    # Tex at UV0 + (PanU, PanV) * WoTSceneTime (the Entry-family panner).
    uv0 = expr(unreal.MaterialExpressionTextureCoordinate, -1800, -400)
    pan_u = scalar_parameter("PanU", -1800, -300)
    pan_v = scalar_parameter("PanV", -1800, -220)
    scene_time = scalar_parameter("WoTSceneTime", -1800, -140)
    rate = expr(unreal.MaterialExpressionAppendVector, -1640, -260)
    mel.connect_material_expressions(pan_u, "", rate, "A")
    mel.connect_material_expressions(pan_v, "", rate, "B")
    offset = expr(unreal.MaterialExpressionMultiply, -1500, -220)
    mel.connect_material_expressions(rate, "", offset, "A")
    mel.connect_material_expressions(scene_time, "", offset, "B")
    panned = expr(unreal.MaterialExpressionAdd, -1360, -330)
    mel.connect_material_expressions(uv0, "", panned, "A")
    mel.connect_material_expressions(offset, "", panned, "B")
    tex = texture_parameter("Tex", -1150, -330)
    mel.connect_material_expressions(panned, "", tex, "UVs")

    # M = min(2*Lightmap(UV1) + G45 dynamic term, 127/63.75).
    lightmap = texture_parameter("Lightmap", -1150, 250)
    lightmap.set_editor_property("const_coordinate", 1)
    decoded = expr(unreal.MaterialExpressionMultiply, -950, 250)
    decoded.set_editor_property("const_b", LIGHTMAP_MULTIPLIER)
    mel.connect_material_expressions(lightmap, "RGB", decoded, "A")
    # The G45 term on the lightmap meshel lattice, like every atlas-consuming
    # world master (dynamic_light_material.build_dynamic_meshel_sum): the
    # atlas size comes from a same-named TextureObjectParameter "Lightmap".
    light = build_dynamic_world_factor(unreal, material, expr, decoded, "",
                                       -760, 250, lightmap_parameter=lightmap)
    for node in mel.get_material_expressions(material):
        if (str(node.get_class().get_name()) == "MaterialExpressionScalarParameter"
                and str(node.get_editor_property("parameter_name"))
                == "DynLightEnable"):
            node.set_editor_property("default_value", 0.0)

    # R = Reflection at the surface point's projection into the capture.
    projection = expr(unreal.MaterialExpressionCustom, -1150, 700)
    projection.set_editor_property("code", PROJECTION_HLSL)
    projection.set_editor_property(
        "output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    projection.set_editor_property("description", "UE1MirrorCaptureUV")
    inputs = []
    world_position = expr(unreal.MaterialExpressionWorldPosition, -1450, 620)
    wires = [(world_position, "WP")]
    for index, (pin, parameter) in enumerate(
            (("Origin", "MirrorOrigin"), ("AxisX", "MirrorAxisX"),
             ("AxisY", "MirrorAxisY"), ("AxisZ", "MirrorAxisZ"),
             ("Tan", "MirrorTan"))):
        node = expr(unreal.MaterialExpressionVectorParameter,
                    -1450, 700 + 80 * index)
        node.set_editor_property("parameter_name", parameter)
        wires.append((node, pin))
    for _node, pin in wires:
        item = unreal.CustomInput()
        item.set_editor_property("input_name", pin)
        inputs.append(item)
    projection.set_editor_property("inputs", inputs)
    for node, pin in wires:
        mel.connect_material_expressions(node, "", projection, pin)
    reflection = texture_parameter(REFLECTION_PARAMETER, -900, 700)
    mel.connect_material_expressions(projection, "", reflection, "UVs")

    # Emissive = M * (Tex + R * (1 - Tex)).
    one_minus = expr(unreal.MaterialExpressionOneMinus, -700, -150)
    mel.connect_material_expressions(tex, "RGB", one_minus, "")
    transmitted = expr(unreal.MaterialExpressionMultiply, -520, 500)
    mel.connect_material_expressions(reflection, "RGB", transmitted, "A")
    mel.connect_material_expressions(one_minus, "", transmitted, "B")
    sheet = expr(unreal.MaterialExpressionAdd, -350, 0)
    mel.connect_material_expressions(tex, "RGB", sheet, "A")
    mel.connect_material_expressions(transmitted, "", sheet, "B")
    lit = expr(unreal.MaterialExpressionMultiply, -180, 150)
    mel.connect_material_expressions(sheet, "", lit, "A")
    mel.connect_material_expressions(light, "", lit, "B")
    mel.connect_material_property(
        lit, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    eal.set_metadata_tag(material, GRAPH_TAG, GRAPH_VERSION)
    eal.set_metadata_tag(
        material, DYNAMIC_LIGHT_GRAPH_TAG, DYNAMIC_LIGHT_GRAPH_VERSION)
    errors = list(mel.recompile_material(material))
    if errors:
        raise RuntimeError(f"{MATERIAL_PATH}: shader compile failed: "
                           + "; ".join(str(error) for error in errors))
    if not eal.save_asset(MATERIAL_PATH):
        raise RuntimeError(f"could not save {MATERIAL_PATH}")
    return material


def validate(unreal: Any, material: Any) -> dict[str, Any]:
    """Structural receipt of the saved graph (parameter inventory, defaults,
    blend) so a stale or hand-edited asset fails loudly."""
    mel = unreal.MaterialEditingLibrary
    path = MATERIAL_PATH
    if material.get_editor_property("blend_mode") != unreal.BlendMode.BLEND_OPAQUE:
        raise RuntimeError(f"{path}: blend mode is not opaque")
    if material.get_editor_property(
            "shading_model") != unreal.MaterialShadingModel.MSM_UNLIT:
        raise RuntimeError(f"{path}: shading model is not unlit")
    if bool(material.get_editor_property("two_sided")):
        raise RuntimeError(f"{path}: master must be one-sided")
    parameters = {}
    for node in mel.get_material_expressions(material):
        cls = str(node.get_class().get_name())
        try:
            name = str(node.get_editor_property("parameter_name"))
        except Exception:
            continue
        parameters.setdefault(name, []).append(cls)
        if cls == "MaterialExpressionTextureSampleParameter2D":
            texture = node.get_editor_property("texture")
            if texture is None or str(texture.get_path_name()).split(".")[0] \
                    != NEUTRAL_TEXTURE:
                raise RuntimeError(f"{path}: {name} default is not neutral")
    expected = {name: ["MaterialExpressionTextureSampleParameter2D"]
                for name in SECTION_TEXTURES + (REFLECTION_PARAMETER,)}
    # The meshel reconstruction reads the atlas size through a TextureObject
    # parameter sharing the sampler's name and default (one parameter value).
    expected["Lightmap"] = sorted([
        "MaterialExpressionTextureSampleParameter2D",
        "MaterialExpressionTextureObjectParameter"])
    expected.update({name: ["MaterialExpressionVectorParameter"]
                     for name in VIEW_VECTORS})
    for name in SECTION_SCALARS:
        expected[name] = ["MaterialExpressionScalarParameter"]
    for name, classes in expected.items():
        if sorted(parameters.get(name) or []) != classes:
            raise RuntimeError(
                f"{path}: parameter {name} is {parameters.get(name)}, "
                f"expected {classes}")
    return {"format": RECEIPT_FORMAT, "graph_version": GRAPH_VERSION,
            "package": path, "parameters": sorted(expected)}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--validate-only", action="store_true")
    return parser


def main() -> None:
    import unreal  # type: ignore

    args = _parser().parse_args()
    validate_output_root(str(args.output_root))
    if args.validate_only:
        material = unreal.load_asset(MATERIAL_PATH)
        if material is None:
            raise RuntimeError(f"{MATERIAL_PATH} is absent")
    else:
        material = build(unreal)
    receipt = validate(unreal, material)
    result = Path(unreal.Paths.project_saved_dir()) / "mirror_material_result.json"
    result.write_text(json.dumps(receipt, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
