"""Unreal-material helpers for centered lightmap response atlases.

The module deliberately imports no ``unreal`` package at module load time so
its manifest/binding policy remains unit-testable with the ordinary Python
interpreter.  Callers pass the editor module into the graph and MIC helpers.
"""

import sys
from pathlib import Path


CONVERT_DIR = Path(__file__).resolve().parents[1] / "convert"
if str(CONVERT_DIR) not in sys.path:
    sys.path.insert(0, str(CONVERT_DIR))

from light_animation import (  # noqa: E402
    MAX_RESPONSE_TERMS,
    MPC_SCALAR_DEFAULTS,
    material_term_bindings,
)
from dynamic_light_material import build_dynamic_world_factor


RESPONSE_TEXTURE_PARAMETERS = tuple(
    f"Delta{index}" for index in range(MAX_RESPONSE_TERMS))
USE_RESPONSE_PARAMETERS = tuple(
    f"UseDelta{index}" for index in range(MAX_RESPONSE_TERMS))
WAVE_INDEX_PARAMETERS = tuple(
    f"DeltaWaveIndex{index}" for index in range(MAX_RESPONSE_TERMS))
EFFECT_SALT_PARAMETERS = tuple(
    f"DeltaEffectSalt{index}" for index in range(MAX_RESPONSE_TERMS))
ATLAS_WIDTH_PARAMETER = "LightmapWidth"
ATLAS_HEIGHT_PARAMETER = "LightmapHeight"
WORLD_RESPONSE_GRAPH_TAG = "WoT.WorldWave.ResponseGraph"
WORLD_RESPONSE_GRAPH_VERSION = "point-shimmer-v3"


def _connect_or_die(mel, source, source_pin, target, target_pin):
    """connect_material_expressions returns False on a bad pin name and the
    graph silently compiles broken — fail the
    import instead."""
    if not mel.connect_material_expressions(
            source, source_pin, target, target_pin):
        raise RuntimeError(
            f"material pin connection failed: {source} '{source_pin}' -> "
            f"{target} '{target_pin}'")

# These are the only masters carrying the response graph. Builders use
# WORLD_RESPONSE_GRAPH_TAG to rebuild stale graphs in place, preserving every
# MIC parent reference. The existing nonanimated masters are intentionally
# absent.
ANIMATED_MASTER_NAMES = (
    "M_WoT_WorldWave",
    "M_WoT_WorldWaveMasked",
    "M_WoT_WorldWaveTranslucent",
    "M_WoT_WorldWaveModulated",
    "M_WoT_WorldWaveTranslucentFlip",
    "M_WoT_WorldWaveModulatedFlip",
    "M_WoT_WorldWaveT_SceneBlend",
)


# WaveIndex < 0 and EffectSalt == 0 are coefficient identities. A record can
# therefore select z, n, or z*n without a dynamic term-kind branch. Retail
# applies shimmer to discrete lightmap texels before filtering, so each slot
# point-loads the four bilinear neighbors, hashes each coordinate independently,
# multiplies response*noise per texel, and only then combines them. Freeze
# returns literal zero before any load or hash.
RESPONSE_COEFFICIENT_HLSL = r"""
if (Freeze >= 0.5)
{
    return float3(0.0, 0.0, 0.0);
}
float z = 1.0;
if (WaveIndex >= 0.0)
{
    z = (WaveIndex < 0.5) ? (2.0 * Wave0 - 1.0)
      : (WaveIndex < 1.5) ? (2.0 * Wave1 - 1.0)
      : (WaveIndex < 2.5) ? (2.0 * Wave2 - 1.0)
                          : (2.0 * Wave3 - 1.0);
}

uint2 dims = max((uint2)round(AtlasSize), uint2(1u, 1u));
float2 samplePos = saturate(LightmapUV) * float2(dims) - 0.5;
int2 lo = (int2)floor(samplePos);
float2 blend = frac(samplePos);
int2 coords[4];
coords[0] = lo;
coords[1] = lo + int2(1, 0);
coords[2] = lo + int2(0, 1);
coords[3] = lo + int2(1, 1);
float weights[4];
weights[0] = (1.0 - blend.x) * (1.0 - blend.y);
weights[1] = blend.x * (1.0 - blend.y);
weights[2] = (1.0 - blend.x) * blend.y;
weights[3] = blend.x * blend.y;

uint frame = ((uint)round(max(NoiseFrame, 0.0))) & 0x00ffffffu;
uint salt = (uint)round(EffectSalt);
float3 filtered = float3(0.0, 0.0, 0.0);
[unroll] for (int i = 0; i < 4; ++i)
{
    uint2 texel = (uint2)clamp(
        coords[i], int2(0, 0), int2(dims) - int2(1, 1));
    float n = 1.0;
    if (salt != 0u)
    {
        uint hy = texel.y + 0x9e3779b9u;
        hy ^= hy >> 16;
        hy *= 0x7feb352du;
        hy ^= hy >> 15;
        hy *= 0x846ca68bu;
        hy ^= hy >> 16;
        uint hf = frame + salt * 0x85ebca6bu;
        hf ^= hf >> 16;
        hf *= 0x7feb352du;
        hf ^= hf >> 15;
        hf *= 0x846ca68bu;
        hf ^= hf >> 16;
        uint h = texel.x ^ hy ^ hf;
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        float u = float(h >> 8) * (1.0 / 16777216.0);
        n = 2.0 * u - 1.0;
    }
    float3 response = ResponseTexture.Load(int3(texel, 0)).rgb;
    filtered += weights[i] * n * response;
}
return z * filtered;
""".strip()


def _expression(unreal, material, cls, x, y):
    return unreal.MaterialEditingLibrary.create_material_expression(
        material, cls, x, y)


def _scalar_parameter(unreal, material, name, default, x, y):
    node = _expression(
        unreal, material, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", float(default))
    return node


def _collection_parameter(unreal, material, collection, name, x, y):
    node = _expression(
        unreal, material, unreal.MaterialExpressionCollectionParameter, x, y)
    node.set_editor_property("collection", collection)
    node.set_editor_property("parameter_name", name)
    return node


def build_response_lightmap(unreal, material, base_sample, collection,
                            linear_sampler, x=-900, y=300):
    """Return ``base + sum(coefficient*response)`` clamped to texture range.

    The caller performs the existing single x2 overbright decode afterward.
    Response textures receive no separate x2 or signed bias.  Every sample is
    reconstructed from UV1, preserving the base atlas packing/gutter contract.
    """
    mel = unreal.MaterialEditingLibrary
    wave_nodes = []
    scalar_names = tuple(name for name, _default in MPC_SCALAR_DEFAULTS)
    expected_names = (
        "Wave0", "Wave1", "Wave2", "Wave3", "NoiseFrame", "Freeze")
    if scalar_names != expected_names:
        raise RuntimeError("material and Wave 4 MPC scalar contracts drifted")
    for index, name in enumerate(scalar_names[:4]):
        wave_nodes.append(_collection_parameter(
            unreal, material, collection, name,
            x - 680, y + 650 + index * 90))
    noise = _collection_parameter(
        unreal, material, collection, scalar_names[4], x - 680, y + 1020)
    freeze = _collection_parameter(
        unreal, material, collection, scalar_names[5], x - 680, y + 1110)

    uv1 = _expression(
        unreal, material, unreal.MaterialExpressionTextureCoordinate,
        x - 680, y + 1220)
    uv1.set_editor_property("coordinate_index", 1)
    width = _scalar_parameter(
        unreal, material, ATLAS_WIDTH_PARAMETER, 1024.0,
        x - 680, y + 1310)
    height = _scalar_parameter(
        unreal, material, ATLAS_HEIGHT_PARAMETER, 1024.0,
        x - 680, y + 1400)
    atlas_size = _expression(
        unreal, material, unreal.MaterialExpressionAppendVector,
        x - 500, y + 1350)
    mel.connect_material_expressions(width, "", atlas_size, "A")
    mel.connect_material_expressions(height, "", atlas_size, "B")

    current = base_sample
    current_pin = "RGB"
    custom_inputs = (
        ("Wave0", wave_nodes[0], ""),
        ("Wave1", wave_nodes[1], ""),
        ("Wave2", wave_nodes[2], ""),
        ("Wave3", wave_nodes[3], ""),
        ("NoiseFrame", noise, ""),
        ("Freeze", freeze, ""),
        ("LightmapUV", uv1, ""),
        ("AtlasSize", atlas_size, ""),
    )
    for index in range(MAX_RESPONSE_TERMS):
        row_y = y + index * 260
        response = linear_sampler(_expression(
            unreal, material, unreal.MaterialExpressionTextureObjectParameter,
            x, row_y))
        response.set_editor_property(
            "parameter_name", RESPONSE_TEXTURE_PARAMETERS[index])
        wave_index = _scalar_parameter(
            unreal, material, WAVE_INDEX_PARAMETERS[index], -1.0,
            x, row_y + 90)
        effect_salt = _scalar_parameter(
            unreal, material, EFFECT_SALT_PARAMETERS[index], 0.0,
            x, row_y + 170)

        coefficient = _expression(
            unreal, material, unreal.MaterialExpressionCustom,
            x + 190, row_y + 90)
        coefficient.set_editor_property("code", RESPONSE_COEFFICIENT_HLSL)
        coefficient.set_editor_property(
            "output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
        coefficient.set_editor_property(
            "description", f"LightResponsePointFilter{index}")
        input_rows = []
        for input_name, _, _ in custom_inputs:
            item = unreal.CustomInput()
            item.set_editor_property("input_name", input_name)
            input_rows.append(item)
        for input_name in ("WaveIndex", "EffectSalt", "ResponseTexture"):
            item = unreal.CustomInput()
            item.set_editor_property("input_name", input_name)
            input_rows.append(item)
        coefficient.set_editor_property("inputs", input_rows)
        for input_name, source, source_pin in custom_inputs:
            mel.connect_material_expressions(
                source, source_pin, coefficient, input_name)
        mel.connect_material_expressions(
            wave_index, "", coefficient, "WaveIndex")
        mel.connect_material_expressions(
            effect_salt, "", coefficient, "EffectSalt")
        mel.connect_material_expressions(
            response, "", coefficient, "ResponseTexture")
        added = _expression(
            unreal, material, unreal.MaterialExpressionAdd,
            x + 570, row_y)
        mel.connect_material_expressions(current, current_pin, added, "A")
        mel.connect_material_expressions(coefficient, "", added, "B")
        switch = _expression(
            unreal, material, unreal.MaterialExpressionStaticSwitchParameter,
            x + 730, row_y)
        switch.set_editor_property(
            "parameter_name", USE_RESPONSE_PARAMETERS[index])
        switch.set_editor_property("default_value", False)
        # StaticSwitchParameter pins are named True/False (NOT A/B — the A/B
        # names silently failed to connect, every animated master compiled
        # with missing switch inputs, and failed shaders render BLACK).
        # The False branch never references this
        # slot's sampler, so unused deltas compile out.
        _connect_or_die(mel, added, "", switch, "True")
        _connect_or_die(mel, current, current_pin, switch, "False")
        current = switch
        current_pin = ""

    floor_zero = _expression(
        unreal, material, unreal.MaterialExpressionMax,
        x + 900, y + 610)
    zero = _expression(
        unreal, material, unreal.MaterialExpressionConstant,
        x + 900, y + 730)
    zero.set_editor_property("r", 0.0)
    mel.connect_material_expressions(current, current_pin, floor_zero, "A")
    mel.connect_material_expressions(zero, "", floor_zero, "B")
    clamp_max = _expression(
        unreal, material, unreal.MaterialExpressionConstant,
        x + 1050, y + 770)
    clamp_max.set_editor_property("r", 254.0 / 255.0)
    ceiling = _expression(
        unreal, material, unreal.MaterialExpressionMin,
        x + 1050, y + 650)
    mel.connect_material_expressions(floor_zero, "", ceiling, "A")
    mel.connect_material_expressions(clamp_max, "", ceiling, "B")
    return ceiling


def build_animated_scene_blend_graph(unreal, material, collection,
                                     linear_sampler, panned_uv):
    """Build the shared animated child-sky SceneBlend graph in place."""
    mel = unreal.MaterialEditingLibrary

    def expr(cls, x, y):
        return _expression(unreal, material, cls, x, y)

    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property(
        "blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("two_sided", True)
    tex = linear_sampler(expr(
        unreal.MaterialExpressionTextureSampleParameter2D, -1100, 0))
    tex.set_editor_property("parameter_name", "Tex")
    mel.connect_material_expressions(panned_uv, "", tex, "UVs")
    lightmap = linear_sampler(expr(
        unreal.MaterialExpressionTextureSampleParameter2D, -1100, 300))
    lightmap.set_editor_property("parameter_name", "Lightmap")
    lightmap.set_editor_property("const_coordinate", 1)
    response = build_response_lightmap(
        unreal, material, lightmap, collection, linear_sampler, -1550, 300)
    decoded = expr(unreal.MaterialExpressionMultiply, -900, 300)
    decoded.set_editor_property("const_b", 2.0)
    mel.connect_material_expressions(response, "", decoded, "A")
    light_factor = build_dynamic_world_factor(
        unreal, material, expr, decoded, "", -720, 300,
        lightmap_parameter=lightmap)

    product = expr(unreal.MaterialExpressionMultiply, -700, 150)
    mel.connect_material_expressions(tex, "RGB", product, "A")
    mel.connect_material_expressions(light_factor, "", product, "B")
    one_minus_tex = expr(unreal.MaterialExpressionOneMinus, -700, -150)
    mel.connect_material_expressions(tex, "RGB", one_minus_tex, "")
    destination_factor = expr(
        unreal.MaterialExpressionMultiply, -500, -200)
    mel.connect_material_expressions(light_factor, "", destination_factor, "A")
    mel.connect_material_expressions(
        one_minus_tex, "", destination_factor, "B")
    scene_color = expr(unreal.MaterialExpressionSceneColor, -500, -350)
    destination = expr(unreal.MaterialExpressionMultiply, -300, -250)
    mel.connect_material_expressions(scene_color, "", destination, "A")
    mel.connect_material_expressions(destination_factor, "", destination, "B")
    output = expr(unreal.MaterialExpressionAdd, -150, 0)
    mel.connect_material_expressions(product, "", output, "A")
    mel.connect_material_expressions(destination, "", output, "B")
    mel.connect_material_property(
        output, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    one = expr(unreal.MaterialExpressionConstant, -150, 150)
    one.set_editor_property("r", 1.0)
    mel.connect_material_property(one, "", unreal.MaterialProperty.MP_OPACITY)


def bind_response_parameters(unreal, material_instance, atlas_document,
                             response_textures):
    """Stamp one animated MIC from the manifest, clearing all unused slots."""
    try:
        width, height = atlas_document["size"]
        animation = atlas_document["animation"]
    except (KeyError, TypeError, ValueError):
        raise ValueError("lightmap atlas document lacks size/animation") from None
    bindings = material_term_bindings(animation)
    mel = unreal.MaterialEditingLibrary
    mel.set_material_instance_scalar_parameter_value(
        material_instance, ATLAS_WIDTH_PARAMETER, float(width))
    mel.set_material_instance_scalar_parameter_value(
        material_instance, ATLAS_HEIGHT_PARAMETER, float(height))
    for index in range(MAX_RESPONSE_TERMS):
        used = index < len(bindings)
        mel.set_material_instance_static_switch_parameter_value(
            material_instance, USE_RESPONSE_PARAMETERS[index], used)
        binding = bindings[index] if used else dict(
            wave_index=-1, effect_salt=0)
        mel.set_material_instance_scalar_parameter_value(
            material_instance, WAVE_INDEX_PARAMETERS[index],
            float(binding["wave_index"]))
        mel.set_material_instance_scalar_parameter_value(
            material_instance, EFFECT_SALT_PARAMETERS[index],
            float(binding["effect_salt"]))
        if used:
            name = binding["response_png"]
            try:
                texture = response_textures[name]
            except KeyError:
                raise ValueError(
                    f"material response texture {name!r} was not imported") from None
            mel.set_material_instance_texture_parameter_value(
                material_instance, RESPONSE_TEXTURE_PARAMETERS[index], texture)
    return len(bindings)
