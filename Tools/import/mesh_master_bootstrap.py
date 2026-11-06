"""Import-safe builders for the MeshLit material family.

There is intentionally no top-level ``unreal`` (or numpy) import.  Callers in
Unreal's embedded Python pass the loaded module explicitly.  This lets
``import_mesh.py`` validate its inputs and bootstrap the typed masters before
it deletes an existing mesh directory.
"""

from dynamic_light_material import (
    DYNAMIC_LIGHT_SLOTS,
    clear_material_graph,
    ensure_dynamic_light_collection,
)

MATERIAL_ROOT = "/Game/WoT/Materials"
NEUTRAL_PATH = "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral"
LIGHT_WAVE_COLLECTION_PATH = "/Game/WoT/Materials/MPC_WoTLightWave"
KEYCOLOR_GRAPH_VERSION = "1"
KEYCOLOR_GRAPH_TAG = "WoT.MeshLit.KeyColorGraph"
LIGHT_GRAPH_VERSION = "wave-groups-dynamic-g45-v2"
LIGHT_GRAPH_TAG = "WoT.MeshLit.LightGraph"

_ML_HLSL = """
float3 Pv = WP - Cam;
float PP = max(dot(Pv, Pv), 1.0);
float3 N = normalize(Nrm);
float4 pr[6] = { float4(P0, R0), float4(P1, R1), float4(P2, R2),
                 float4(P3, R3), float4(P4, R4), float4(P5, R5) };
float3 cl[6] = { C0, C1, C2, C3, C4, C5 };
float wg[6] = { G0, G1, G2, G3, G4, G5 };
float4 dpr[8] = { DP0, DP1, DP2, DP3, DP4, DP5, DP6, DP7 };
float4 dcl[8] = { DC0, DC1, DC2, DC3, DC4, DC5, DC6, DC7 };
float4 waves = float4(Wave0, Wave1, Wave2, Wave3);
float3 sum = 0;
[unroll] for (int i = 0; i < 6; i++)
{
    if (pr[i].w <= 0) continue;
    float3 Lw = pr[i].xyz - WP;
    float d2 = dot(Lw, Lw);
    float d = sqrt(d2);
    float g = 1.0 + dot(Lw, N) / max(d, 0.001);
    g = g * g - 1.5;
    g = max(g, 0.0);
    float3 Lv = pr[i].xyz - Cam;
    float3 Mr = Lv - 2.0 * N * dot(N, Lw);
    float S = dot(Mr, Pv) - PP;
    if (S > 0) g += 6.0 * S * S / (max(d2, 1.0) * PP);
    g *= 1.0 - d * pr[i].w;
    float ratio = 1.0;
    int group = (int)floor(wg[i] + 0.5);
    if (group >= 0 && group < 4 && MLWaveMeans[group] > 0.0)
    {
        ratio += MLWaveAmplitudes[group]
               * (2.0 * waves[group] - 1.0) / MLWaveMeans[group];
    }
    if (g > 0) sum += cl[i] * (g * ratio);
}
[unroll] for (int i = 0; i < 8; i++)
{
    if (DynEnable <= 0.0 || dpr[i].w <= 0.0
        || dot(dcl[i].rgb, dcl[i].rgb) <= 0.0) continue;
    float3 Lw = dpr[i].xyz - WP;
    float d2 = dot(Lw, Lw);
    float d = sqrt(d2);
    float g = 1.0 + dot(Lw, N) / max(d, 0.001);
    g = max(g * g - 1.5, 0.0);
    float3 Lv = dpr[i].xyz - Cam;
    float3 Mr = Lv - 2.0 * N * dot(N, Lw);
    float S = dot(Mr, Pv) - PP;
    if (S > 0) g += 6.0 * S * S / (max(d2, 1.0) * PP);
    g *= 1.0 - d / dpr[i].w;
    if (g > 0) sum += dcl[i].rgb * g;
}
return min(SG * 1.4 * sum + Amb, 1.0);
"""


def _linear_sampler(unreal, node, gamma_space):
    if gamma_space:
        neutral = unreal.load_asset(NEUTRAL_PATH)
        if neutral is None:
            raise RuntimeError(f"linear sampler default texture is missing: {NEUTRAL_PATH}")
        node.set_editor_property("texture", neutral)
        node.set_editor_property(
            "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    return node


def light_wave_parameter_nodes(unreal, material, expr, x, y):
    """Build the shared four-wave MPC and per-map mean/amplitude inputs."""
    collection = unreal.load_asset(LIGHT_WAVE_COLLECTION_PATH)
    if collection is None:
        raise RuntimeError(
            f"light-wave MPC is missing: {LIGHT_WAVE_COLLECTION_PATH}")
    sources = []
    for index in range(4):
        node = expr(unreal.MaterialExpressionCollectionParameter,
                    x, y + index * 70)
        node.set_editor_property("collection", collection)
        node.set_editor_property("parameter_name", f"Wave{index}")
        sources.append((f"Wave{index}", node))
    means = expr(unreal.MaterialExpressionVectorParameter, x, y + 310)
    means.set_editor_property("parameter_name", "MLWaveMeans")
    means.set_editor_property(
        "default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    amplitudes = expr(unreal.MaterialExpressionVectorParameter, x, y + 390)
    amplitudes.set_editor_property("parameter_name", "MLWaveAmplitudes")
    amplitudes.set_editor_property(
        "default_value", unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    sources.extend((
        ("MLWaveMeans", means),
        ("MLWaveAmplitudes", amplitudes),
    ))
    return sources


def _wire_light(unreal, material, expr):
    mel = unreal.MaterialEditingLibrary
    custom = expr(unreal.MaterialExpressionCustom, -400, 100)
    custom.set_editor_property("code", _ML_HLSL)
    custom.set_editor_property(
        "output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", "UE1MeshLight")
    inputs = []
    wires = []
    for name, cls, y in (
            ("WP", unreal.MaterialExpressionWorldPosition, 0),
            ("Nrm", unreal.MaterialExpressionVertexNormalWS, 80),
            ("Cam", unreal.MaterialExpressionCameraPositionWS, 160)):
        node = expr(cls, -700, y)
        item = unreal.CustomInput()
        item.set_editor_property("input_name", name)
        inputs.append(item)
        wires.append((node, name))
    for name, node in light_wave_parameter_nodes(
            unreal, material, expr, -950, 240):
        item = unreal.CustomInput()
        item.set_editor_property("input_name", name)
        inputs.append(item)
        wires.append((node, name))
    y = 240
    for index in range(6):
        pos = expr(unreal.MaterialExpressionVectorParameter, -700, y)
        pos.set_editor_property("parameter_name", f"MLPos{index}")
        radius = expr(unreal.MaterialExpressionScalarParameter, -700, y + 60)
        radius.set_editor_property("parameter_name", f"MLRad{index}")
        radius.set_editor_property("default_value", 0.0)
        color = expr(unreal.MaterialExpressionVectorParameter, -700, y + 120)
        color.set_editor_property("parameter_name", f"MLCol{index}")
        group = expr(unreal.MaterialExpressionScalarParameter, -700, y + 180)
        group.set_editor_property("parameter_name", f"MLWaveGroup{index}")
        group.set_editor_property("default_value", -1.0)
        for name, node in ((f"P{index}", pos), (f"R{index}", radius),
                           (f"C{index}", color), (f"G{index}", group)):
            item = unreal.CustomInput()
            item.set_editor_property("input_name", name)
            inputs.append(item)
            wires.append((node, name))
        y += 240
    dynamic_collection = ensure_dynamic_light_collection(unreal)
    for index in range(DYNAMIC_LIGHT_SLOTS):
        for name, parameter_name, offset in (
                (f"DP{index}", f"DL{index}_PosR", 0),
                (f"DC{index}", f"DL{index}_Color", 60)):
            node = expr(
                unreal.MaterialExpressionCollectionParameter,
                -700, y + offset)
            node.set_editor_property("collection", dynamic_collection)
            node.set_editor_property("parameter_name", parameter_name)
            item = unreal.CustomInput()
            item.set_editor_property("input_name", name)
            inputs.append(item)
            wires.append((node, name))
        y += 130
    dynamic_enable = expr(
        unreal.MaterialExpressionScalarParameter, -700, y)
    dynamic_enable.set_editor_property("parameter_name", "MLDynEnable")
    dynamic_enable.set_editor_property("default_value", 1.0)
    item = unreal.CustomInput()
    item.set_editor_property("input_name", "DynEnable")
    inputs.append(item)
    wires.append((dynamic_enable, "DynEnable"))
    y += 80
    ambient = expr(unreal.MaterialExpressionVectorParameter, -700, y)
    ambient.set_editor_property("parameter_name", "MLAmbient")
    scale_glow = expr(unreal.MaterialExpressionScalarParameter, -700, y + 60)
    scale_glow.set_editor_property("parameter_name", "MLScaleGlow")
    scale_glow.set_editor_property("default_value", 1.0)
    for name, node in (("Amb", ambient), ("SG", scale_glow)):
        item = unreal.CustomInput()
        item.set_editor_property("input_name", name)
        inputs.append(item)
        wires.append((node, name))
    custom.set_editor_property("inputs", inputs)
    for node, name in wires:
        mel.connect_material_expressions(node, "", custom, name)
    return custom


def _build_opaque_family(unreal, gamma_space=True):
    mel = unreal.MaterialEditingLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    eal = unreal.EditorAssetLibrary
    result = {}
    variants = (
        ("M_WoT_MeshLit", False, False, True, True),
        # Masked pixels are clipped, but carrying the same parameter keeps every
        # MeshLit MI/MID self-describing across typed runtime reparenting.
        ("M_WoT_MeshLitMasked", True, True, False, True),
        ("M_WoT_MeshLit2S", "2s", True, True, True),
    )
    for name, key, two_sided, reconstruct_key, carry_key in variants:
        path = f"{MATERIAL_ROOT}/{name}"
        exists = eal.does_asset_exist(path)
        current = unreal.load_asset(path) if exists else None
        key_tagged = (exists and eal.get_metadata_tag(
            current, KEYCOLOR_GRAPH_TAG) == KEYCOLOR_GRAPH_VERSION)
        light_tagged = (exists and eal.get_metadata_tag(
            current, LIGHT_GRAPH_TAG) == LIGHT_GRAPH_VERSION)
        rebuild = exists and (not light_tagged or (carry_key and not key_tagged))
        if exists and not rebuild:
            if current.get_editor_property("two_sided") != two_sided:
                current.set_editor_property("two_sided", two_sided)
                mel.recompile_material(current)
                eal.save_asset(path)
            result[key] = current
            continue
        if rebuild:
            material = current
            clear_material_graph(unreal, material)
        else:
            material = tools.create_asset(
                name, MATERIAL_ROOT, unreal.Material, unreal.MaterialFactoryNew())

        def expr(cls, x, y):
            return mel.create_material_expression(material, cls, x, y)

        material.set_editor_property(
            "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        material.set_editor_property("two_sided", two_sided)
        material.set_editor_property("used_with_skeletal_mesh", True)
        material.set_editor_property("used_with_static_lighting", True)
        texture = _linear_sampler(
            unreal,
            expr(unreal.MaterialExpressionTextureSampleParameter2D, -700, -200),
            gamma_space)
        texture.set_editor_property("parameter_name", "Tex")
        light = _wire_light(unreal, material, expr)
        color = texture
        color_pin = "RGB"
        if carry_key:
            key_color = expr(unreal.MaterialExpressionVectorParameter, -650, -60)
            key_color.set_editor_property("parameter_name", "KeyColor")
            key_color.set_editor_property(
                "default_value", unreal.LinearColor(0.0, 0.0, 0.0, 1.0))
        if reconstruct_key:
            rgb_alpha = expr(unreal.MaterialExpressionMultiply, -450, -220)
            mel.connect_material_expressions(texture, "RGB", rgb_alpha, "A")
            mel.connect_material_expressions(texture, "A", rgb_alpha, "B")
            inverse_alpha = expr(unreal.MaterialExpressionOneMinus, -450, -80)
            mel.connect_material_expressions(texture, "A", inverse_alpha, "")
            keyed = expr(unreal.MaterialExpressionMultiply, -280, -80)
            mel.connect_material_expressions(key_color, "", keyed, "A")
            mel.connect_material_expressions(inverse_alpha, "", keyed, "B")
            color = expr(unreal.MaterialExpressionAdd, -100, -180)
            mel.connect_material_expressions(rgb_alpha, "", color, "A")
            mel.connect_material_expressions(keyed, "", color, "B")
            color_pin = ""
        elif carry_key:
            # Keep masked RGB unchanged for every nonzero alpha (including
            # generated mip edges); the added term exists only at fully
            # transparent pixels, which the opacity mask discards.
            alpha_ceil = expr(unreal.MaterialExpressionCeil, -450, -80)
            mel.connect_material_expressions(texture, "A", alpha_ceil, "")
            inverse_alpha = expr(unreal.MaterialExpressionOneMinus, -300, -80)
            mel.connect_material_expressions(alpha_ceil, "", inverse_alpha, "")
            keyed = expr(unreal.MaterialExpressionMultiply, -150, -80)
            mel.connect_material_expressions(key_color, "", keyed, "A")
            mel.connect_material_expressions(inverse_alpha, "", keyed, "B")
            color = expr(unreal.MaterialExpressionAdd, 0, -180)
            mel.connect_material_expressions(texture, "RGB", color, "A")
            mel.connect_material_expressions(keyed, "", color, "B")
            color_pin = ""
        emissive = expr(unreal.MaterialExpressionMultiply, 100, -50)
        mel.connect_material_expressions(color, color_pin, emissive, "A")
        mel.connect_material_expressions(light, "", emissive, "B")
        mel.connect_material_property(
            emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        if key is True:
            material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
            material.set_editor_property("opacity_mask_clip_value", 0.5)
            mel.connect_material_property(
                texture, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
        if carry_key:
            eal.set_metadata_tag(material, KEYCOLOR_GRAPH_TAG, KEYCOLOR_GRAPH_VERSION)
        eal.set_metadata_tag(material, LIGHT_GRAPH_TAG, LIGHT_GRAPH_VERSION)
        mel.recompile_material(material)
        eal.save_asset(path)
        result[key] = material
    return result


def _build_translucent(unreal, gamma_space=True):
    path = f"{MATERIAL_ROOT}/M_WoT_MeshLitT"
    eal = unreal.EditorAssetLibrary
    mel = unreal.MaterialEditingLibrary
    exists = eal.does_asset_exist(path)
    current = unreal.load_asset(path) if exists else None
    tagged = (exists and eal.get_metadata_tag(
        current, LIGHT_GRAPH_TAG) == LIGHT_GRAPH_VERSION)
    if tagged:
        return current
    if exists:
        material = current
        clear_material_graph(unreal, material)
    else:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_WoT_MeshLitT", MATERIAL_ROOT,
            unreal.Material, unreal.MaterialFactoryNew())

    def expr(cls, x, y):
        return mel.create_material_expression(material, cls, x, y)

    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property(
        "blend_mode", unreal.BlendMode.BLEND_ALPHA_COMPOSITE)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("used_with_skeletal_mesh", True)
    texture = _linear_sampler(
        unreal,
        expr(unreal.MaterialExpressionTextureSampleParameter2D, -700, -250),
        gamma_space)
    texture.set_editor_property("parameter_name", "Tex")
    env_switch = expr(unreal.MaterialExpressionScalarParameter, -1100, -400)
    env_switch.set_editor_property("parameter_name", "EnvMap")
    env_switch.set_editor_property("default_value", 0.0)
    env = expr(unreal.MaterialExpressionCustom, -1100, -300)
    env.set_editor_property("code", (
        "float3 D = normalize(WP - Cam);\n"
        "float3 Nn = normalize(Nrm);\n"
        "float3 R = D - 2.0 * Nn * dot(D, Nn);\n"
        "return float2((R.x + 1.0) * 0.5, (R.y + 1.0) * 0.5);\n"))
    env.set_editor_property(
        "output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    env.set_editor_property("description", "UE1EnviroMap")
    env_inputs = []
    for name, cls in (("WP", unreal.MaterialExpressionWorldPosition),
                      ("Cam", unreal.MaterialExpressionCameraPositionWS),
                      ("Nrm", unreal.MaterialExpressionVertexNormalWS)):
        node = expr(cls, -1300, -300)
        item = unreal.CustomInput()
        item.set_editor_property("input_name", name)
        env_inputs.append((item, node, name))
    env.set_editor_property("inputs", [item for item, _node, _name in env_inputs])
    for _item, node, name in env_inputs:
        mel.connect_material_expressions(node, "", env, name)
    uv0 = expr(unreal.MaterialExpressionTextureCoordinate, -1100, -180)
    uv = expr(unreal.MaterialExpressionLinearInterpolate, -900, -260)
    mel.connect_material_expressions(uv0, "", uv, "A")
    mel.connect_material_expressions(env, "", uv, "B")
    mel.connect_material_expressions(env_switch, "", uv, "Alpha")
    mel.connect_material_expressions(uv, "", texture, "UVs")
    rgba = expr(unreal.MaterialExpressionMultiply, -520, -200)
    mel.connect_material_expressions(texture, "RGB", rgba, "A")
    mel.connect_material_expressions(texture, "A", rgba, "B")
    light = _wire_light(unreal, material, expr)
    source = expr(unreal.MaterialExpressionMultiply, -180, -120)
    mel.connect_material_expressions(rgba, "", source, "A")
    mel.connect_material_expressions(light, "", source, "B")
    mel.connect_material_property(source, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    channels = []
    for index, channel in enumerate("rgb"):
        mask = expr(unreal.MaterialExpressionComponentMask, -60, -300 + index * 70)
        for component in "rgba":
            mask.set_editor_property(component, component == channel)
        mel.connect_material_expressions(source, "", mask, "")
        channels.append(mask)
    max1 = expr(unreal.MaterialExpressionMax, 40, -260)
    mel.connect_material_expressions(channels[0], "", max1, "A")
    mel.connect_material_expressions(channels[1], "", max1, "B")
    max2 = expr(unreal.MaterialExpressionMax, 140, -220)
    mel.connect_material_expressions(max1, "", max2, "A")
    mel.connect_material_expressions(channels[2], "", max2, "B")
    mel.connect_material_property(max2, "", unreal.MaterialProperty.MP_OPACITY)
    mel.recompile_material(material)
    eal.set_metadata_tag(material, LIGHT_GRAPH_TAG, LIGHT_GRAPH_VERSION)
    eal.save_asset(path)
    return material


def ensure_mesh_masters(unreal, gamma_space=True):
    """Create/repair every typed MeshLit master and return them by asset name."""
    eal = unreal.EditorAssetLibrary
    # External prerequisites are checked BEFORE any builder runs (the safety
    # pin test_mesh_master_bootstrap asserts exactly this ordering). The
    # dynamic-light collection is deliberately NOT in this list: it is an
    # in-repo asset this bootstrap ensures itself, right after the gate.
    required_inputs = (
        f"{MATERIAL_ROOT}/M_WoT_Master",
        LIGHT_WAVE_COLLECTION_PATH,
        NEUTRAL_PATH if gamma_space else None,
    )
    missing = [path for path in required_inputs
               if path and not eal.does_asset_exist(path)]
    if missing:
        raise RuntimeError(
            "mesh-master bootstrap prerequisites are missing: " + ", ".join(missing))
    ensure_dynamic_light_collection(unreal)
    opaque = _build_opaque_family(unreal, gamma_space)
    translucent = _build_translucent(unreal, gamma_space)
    import make_mesh_modulate
    modulate = make_mesh_modulate.make_mesh_modulate_master(gamma_space)
    result = {
        "M_WoT_MeshLit": opaque[False],
        "M_WoT_MeshLitMasked": opaque[True],
        "M_WoT_MeshLit2S": opaque["2s"],
        "M_WoT_MeshLitT": translucent,
        "M_WoT_MeshModulate": modulate,
    }
    missing = [name for name, material in result.items() if material is None]
    if missing:
        raise RuntimeError("mesh-master bootstrap failed: " + ", ".join(missing))
    return result


def make_meshlit_masters(unreal, gamma_space=True):
    masters = ensure_mesh_masters(unreal, gamma_space)
    return {
        False: masters["M_WoT_MeshLit"],
        True: masters["M_WoT_MeshLitMasked"],
        "2s": masters["M_WoT_MeshLit2S"],
    }


def make_meshlit_translucent_master(unreal, gamma_space=True):
    return ensure_mesh_masters(unreal, gamma_space)["M_WoT_MeshLitT"]
