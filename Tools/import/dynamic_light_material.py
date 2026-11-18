"""Shared material-graph helpers for G45 runtime actor lights.

Import-safe: callers pass Unreal's embedded ``unreal`` module explicitly.
The one MPC carries eight position/radius vectors and eight display-color/
shape vectors; every receiver family consumes that same bounded contract.

Atlas-consuming world masters evaluate the term the way UE1 did for BSP
surfaces: per lightmap meshel, then bilinearly reconstructed (see
``_DYNAMIC_MESHEL_HLSL``). Movers and meshes keep the per-pixel
``_DYNAMIC_SURFACE_HLSL`` (no atlas lattice exists for them).

A slot colour's alpha selects the UE1 spatial function (EWoTDynLightShape):
0 NonIncidence (spatial_NonIncidence), 1 Incidence, 2 Shell (spatial_Shell:
zero unless 0.8 < d/R < 1, then 1 - 10|d/R - 0.9|). Meshes ignore it, as
UE1's vertex lighting ignored LightEffect (FLightManager::Light).
"""

MATERIAL_ROOT = "/Game/WoT/Materials"
DYNAMIC_LIGHT_COLLECTION_PATH = (
    f"{MATERIAL_ROOT}/MPC_WoTDynamicLights")
DYNAMIC_LIGHT_GRAPH_TAG = "WoT.DynamicActorLightGraph"
# v2: the world (atlas) family evaluates at lightmap meshel centres and
# bilinearly reconstructs, instead of per pixel (the hard circular edge).
# v3: LE_Shell gets its own falloff (it was drawn as NonIncidence, flooding
# the whole of AMAPearl's radius instead of its thin outer shell).
DYNAMIC_LIGHT_GRAPH_VERSION = "g45-v3-shell"
DYNAMIC_LIGHT_SLOTS = 8
DYNAMIC_LIGHT_ENABLE_PARAMETER = "DynLightEnable"


def clear_material_graph(unreal, material):
    """Delete every expression of ``material`` before a master is rebuilt.

    UMaterialEditingLibrary::DeleteAllMaterialExpressions (UE 5.8
    MaterialEditingLibrary.cpp:559-567) range-iterates Material->GetExpressions()
    while DeleteMaterialExpression removes from that same collection (:609),
    so it skips every other node. Each rebuild through it left half of the old
    graph behind as zero-consumer orphans; orphan parameters still register
    (FMaterialCachedExpressionData::UpdateForExpressions walks every
    expression, MaterialCachedData.cpp:909), so an old DynLightEnable/Lightmap
    duplicate survives beside the new one. Iterate a snapshot instead."""
    mel = unreal.MaterialEditingLibrary
    for expression in list(mel.get_material_expressions(material)):
        mel.delete_material_expression(material, expression)
    remaining = len(list(mel.get_material_expressions(material)))
    if remaining:
        raise RuntimeError(
            f"{material.get_path_name()}: {remaining} expressions survived "
            "the graph clear")


def dynamic_vector_names():
    return tuple(
        name
        for index in range(DYNAMIC_LIGHT_SLOTS)
        for name in (f"DL{index}_PosR", f"DL{index}_Color")
    )


def ensure_dynamic_light_collection(unreal):
    """Create/reconcile the eight-slot vector-only MPC contract."""
    eal = unreal.EditorAssetLibrary
    exists = eal.does_asset_exist(DYNAMIC_LIGHT_COLLECTION_PATH)
    if exists:
        collection = unreal.load_asset(DYNAMIC_LIGHT_COLLECTION_PATH)
    else:
        collection = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            "MPC_WoTDynamicLights", MATERIAL_ROOT,
            unreal.MaterialParameterCollection,
            unreal.MaterialParameterCollectionFactoryNew())
    if collection is None:
        raise RuntimeError(
            f"could not create or load {DYNAMIC_LIGHT_COLLECTION_PATH}")

    expected = dynamic_vector_names()
    current = tuple(
        str(row.get_editor_property("parameter_name"))
        for row in collection.get_editor_property("vector_parameters"))
    has_scalars = bool(collection.get_editor_property("scalar_parameters"))
    dirty = False
    if current != expected:
        rows = []
        for name in expected:
            row = unreal.CollectionVectorParameter()
            row.set_editor_property("parameter_name", name)
            row.set_editor_property(
                "default_value", unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
            rows.append(row)
        collection.set_editor_property("vector_parameters", rows)
        dirty = True
    if has_scalars:
        collection.set_editor_property("scalar_parameters", [])
        dirty = True
    if dirty:
        eal.save_asset(DYNAMIC_LIGHT_COLLECTION_PATH)
    elif not exists:
        eal.save_asset(DYNAMIC_LIGHT_COLLECTION_PATH)
    return collection


_DYNAMIC_SURFACE_HLSL = """
float4 pr[8] = { DP0, DP1, DP2, DP3, DP4, DP5, DP6, DP7 };
float4 cl[8] = { DC0, DC1, DC2, DC3, DC4, DC5, DC6, DC7 };
float3 N = normalize(Nrm);
float3 sum = 0;
[unroll] for (int i = 0; i < 8; i++)
{
    if (pr[i].w <= 0.0 || dot(cl[i].rgb, cl[i].rgb) <= 0.0) continue;
    float3 Lw = pr[i].xyz - WP;
    float d = length(Lw);
    float s = d / pr[i].w;
    if (s >= 1.0) continue;
    float attenN = saturate(1.02 - s);
    float cosF = max(0.0, dot(N, Lw / max(d, 0.001)));
    float attenI = cosF * (1.0 - s) * (1.0 - s) * (2.0 * s + 1.0);
    float attenS = (s > 0.8) ? 1.0 - 10.0 * abs(s - 0.9) : 0.0;
    float atten = (cl[i].a > 1.5) ? attenS : lerp(attenN, attenI, saturate(cl[i].a));
    sum += cl[i].rgb * atten;
}
return sum * DynEnable;
""".strip()


def build_dynamic_surface_sum(unreal, material, expr, x, y, *,
                              enable_parameter=DYNAMIC_LIGHT_ENABLE_PARAMETER,
                              description="UE1DynamicSurfaceLights"):
    """Return sum(attenuation * display FloatColor) for the eight MPC slots."""
    mel = unreal.MaterialEditingLibrary
    collection = ensure_dynamic_light_collection(unreal)
    custom = expr(unreal.MaterialExpressionCustom, x, y)
    custom.set_editor_property("code", _DYNAMIC_SURFACE_HLSL)
    custom.set_editor_property(
        "output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", description)

    inputs = []
    wires = []
    for name, cls, offset in (
            ("WP", unreal.MaterialExpressionWorldPosition, 0),
            ("Nrm", unreal.MaterialExpressionVertexNormalWS, 80)):
        node = expr(cls, x - 360, y + offset)
        item = unreal.CustomInput()
        item.set_editor_property("input_name", name)
        inputs.append(item)
        wires.append((node, name))
    enabled = expr(
        unreal.MaterialExpressionScalarParameter, x - 360, y + 170)
    enabled.set_editor_property("parameter_name", enable_parameter)
    enabled.set_editor_property("default_value", 1.0)
    item = unreal.CustomInput()
    item.set_editor_property("input_name", "DynEnable")
    inputs.append(item)
    wires.append((enabled, "DynEnable"))

    row_y = y + 270
    for index in range(DYNAMIC_LIGHT_SLOTS):
        for input_name, parameter_name, offset in (
                (f"DP{index}", f"DL{index}_PosR", 0),
                (f"DC{index}", f"DL{index}_Color", 55)):
            node = expr(
                unreal.MaterialExpressionCollectionParameter,
                x - 360, row_y + offset)
            node.set_editor_property("collection", collection)
            node.set_editor_property("parameter_name", parameter_name)
            custom_input = unreal.CustomInput()
            custom_input.set_editor_property("input_name", input_name)
            inputs.append(custom_input)
            wires.append((node, input_name))
        row_y += 120

    custom.set_editor_property("inputs", inputs)
    for node, input_name in wires:
        mel.connect_material_expressions(node, "", custom, input_name)
    return custom


# UE1 lit BSP surfaces through the lightmap, never per pixel
# (FLightManager::SetupForSurf): each moving light's spatial function runs
# once per lightmap meshel at VertexBase + u*VertexDU + v*VertexDV
# (SPATIAL_PRE/SPATIAL_BEGIN1; spatial_NonIncidence behind the table bound
# SqrtOfs < 4096 with RRadiusMult = 4093/R^2 from ComputeFromActor), Merge
# adds the result into the RGBA7 stream, and the hardware bilinearly filters
# that meshel grid across the polygon. The atlas contract makes the meshel
# lattice the atlas texel-centre grid exactly
# (lightmap_atlas.py: UV1 = (x + u + 0.5) / W, verified by its round trip;
# UE1's own mapping likewise puts meshel u at lightmap texel u + 0.5 in
# the Direct3D driver),
# so a pixel's lattice coordinate is q = UV1 * AtlasSize - 0.5. A BSP polygon
# is planar and UV1 is affine on it, so world position is affine in q and the
# screen-space derivatives give the world step per meshel (the 2x2 Jacobian
# inverse below); camera-relative positions keep those differences precise.
# Each light is evaluated at the four surrounding meshel centres and
# bilinearly weighted - the per-texel formula reconstructed the way the
# hardware did, instead of evaluated per pixel (which left the 0.02
# NonIncidence step at s = 1 as a crisp ring).
#
# Fallbacks (PORT_NOTES "Dynamic lights on world surfaces and meshes"): a pixel
# within sixteen float ulps of a lattice point in both axes evaluates at its
# own position (equal to the reconstruction there to within that distance,
# and the case of every constant-UV1 fullbright slot - all its vertices sit on
# one atlas texel centre, so its interpolated q is an integer to a few ulps -
# which keeps its prior per-pixel behaviour); a numerically singular lattice
# Jacobian (edge-on) evaluates per pixel.
_DYNAMIC_MESHEL_HLSL = """
float4 pr[8] = { DP0, DP1, DP2, DP3, DP4, DP5, DP6, DP7 };
float4 cl[8] = { DC0, DC1, DC2, DC3, DC4, DC5, DC6, DC7 };
float3 N = normalize(Nrm);
float2 q = UV1 * AtlasSize - 0.5;
float2 q0 = floor(q);
float2 f = q - q0;
float2 dqx = ddx(q);
float2 dqy = ddy(q);
float3 dwx = ddx(WPRel);
float3 dwy = ddy(WPRel);
float det = dqx.x * dqy.y - dqx.y * dqy.x;
float qulp = exp2(floor(log2(max(max(abs(q.x), abs(q.y)), 1.0))) - 23.0);
float2 fn = min(f, 1.0 - f);
bool onMeshel = fn.x <= 16.0 * qulp && fn.y <= 16.0 * qulp;
bool lattice = !onMeshel
    && abs(det) > 1.0e-4 * (dot(dqx, dqx) + dot(dqy, dqy));
float3 corner[4];
float weight[4];
if (lattice)
{
    float3 Ju = (dwx * dqy.y - dwy * dqx.y) / det;
    float3 Jv = (dwy * dqx.x - dwx * dqy.x) / det;
    float3 base = WP - Ju * f.x - Jv * f.y;
    corner[0] = base;
    corner[1] = base + Ju;
    corner[2] = base + Jv;
    corner[3] = base + Ju + Jv;
    weight[0] = (1.0 - f.x) * (1.0 - f.y);
    weight[1] = f.x * (1.0 - f.y);
    weight[2] = (1.0 - f.x) * f.y;
    weight[3] = f.x * f.y;
}
else
{
    corner[0] = WP;
    corner[1] = WP;
    corner[2] = WP;
    corner[3] = WP;
    weight[0] = 1.0;
    weight[1] = 0.0;
    weight[2] = 0.0;
    weight[3] = 0.0;
}
float3 sum = 0;
[unroll] for (int i = 0; i < 8; i++)
{
    if (pr[i].w <= 0.0 || dot(cl[i].rgb, cl[i].rgb) <= 0.0) continue;
    float shape = cl[i].a;
    float atten = 0.0;
    [unroll] for (int k = 0; k < 4; k++)
    {
        float3 Lw = pr[i].xyz - corner[k];
        float d = length(Lw);
        float s = d / pr[i].w;
        float attenN = (s * s * 4093.0 < 4095.5) ? saturate(1.02 - s) : 0.0;
        float cosF = max(0.0, dot(N, Lw / max(d, 0.001)));
        float attenI = (s < 1.0)
            ? cosF * (1.0 - s) * (1.0 - s) * (2.0 * s + 1.0) : 0.0;
        float attenS = (s > 0.8 && s < 1.0) ? 1.0 - 10.0 * abs(s - 0.9) : 0.0;
        atten += weight[k] * ((shape > 1.5) ? attenS
            : lerp(attenN, attenI, saturate(shape)));
    }
    sum += cl[i].rgb * atten;
}
return sum * DynEnable;
""".strip()


def _connect_or_die(mel, source, source_pin, target, target_pin):
    """connect_material_expressions returns False on a bad pin name and the
    graph then compiles broken; fail the build instead."""
    if not mel.connect_material_expressions(
            source, source_pin, target, target_pin):
        raise RuntimeError(
            f"material pin connection failed: {source} '{source_pin}' -> "
            f"{target} '{target_pin}'")


def build_dynamic_meshel_sum(unreal, material, expr, lightmap_parameter,
                             x, y, *,
                             enable_parameter=DYNAMIC_LIGHT_ENABLE_PARAMETER,
                             description="UE1DynamicWorldMeshelLights"):
    """Return the meshel-reconstructed sum(attenuation * FloatColor).

    ``lightmap_parameter`` is the master's existing atlas sampler parameter.
    Its size is read through a same-named TextureObjectParameter carrying
    the identical default texture and sampler type, so the duplicate shares
    every MIC/MID override: UE keys texture parameter values by name, and
    UMaterial::CheckInValidStateForCompilation (Material.cpp:7321-7345,
    FMaterialCachedExpressionData::CheckDuplicateParameterValuesEqual,
    MaterialCachedData.cpp:285-286) rejects duplicates only when their
    default textures differ."""
    mel = unreal.MaterialEditingLibrary
    collection = ensure_dynamic_light_collection(unreal)
    default_texture = lightmap_parameter.get_editor_property("texture")
    if default_texture is None:
        raise RuntimeError(
            "dynamic meshel sum needs the atlas sampler's linear default "
            "texture (an unset default fails sampler-type validation)")
    custom = expr(unreal.MaterialExpressionCustom, x, y)
    custom.set_editor_property("code", _DYNAMIC_MESHEL_HLSL)
    custom.set_editor_property(
        "output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", description)

    inputs = []
    wires = []

    def add_input(name, node):
        item = unreal.CustomInput()
        item.set_editor_property("input_name", name)
        inputs.append(item)
        wires.append((node, name))

    add_input("WP", expr(unreal.MaterialExpressionWorldPosition, x - 420, y))
    world_relative = expr(
        unreal.MaterialExpressionWorldPosition, x - 420, y + 60)
    world_relative.set_editor_property(
        "world_position_shader_offset",
        unreal.WorldPositionIncludedOffsets.WPT_CAMERA_RELATIVE)
    add_input("WPRel", world_relative)
    add_input("Nrm", expr(
        unreal.MaterialExpressionVertexNormalWS, x - 420, y + 120))
    lattice_uv = expr(
        unreal.MaterialExpressionTextureCoordinate, x - 420, y + 180)
    lattice_uv.set_editor_property("coordinate_index", 1)
    add_input("UV1", lattice_uv)
    atlas = expr(
        unreal.MaterialExpressionTextureObjectParameter, x - 620, y + 240)
    atlas.set_editor_property(
        "parameter_name",
        lightmap_parameter.get_editor_property("parameter_name"))
    atlas.set_editor_property("texture", default_texture)
    atlas.set_editor_property(
        "sampler_type", lightmap_parameter.get_editor_property("sampler_type"))
    atlas_size = expr(
        unreal.MaterialExpressionTextureProperty, x - 420, y + 240)
    atlas_size.set_editor_property(
        "property", unreal.MaterialExposedTextureProperty.TMTM_TEXTURE_SIZE)
    _connect_or_die(mel, atlas, "", atlas_size, "")
    add_input("AtlasSize", atlas_size)
    enabled = expr(unreal.MaterialExpressionScalarParameter, x - 420, y + 300)
    enabled.set_editor_property("parameter_name", enable_parameter)
    enabled.set_editor_property("default_value", 1.0)
    add_input("DynEnable", enabled)

    row_y = y + 380
    for index in range(DYNAMIC_LIGHT_SLOTS):
        for input_name, parameter_name, offset in (
                (f"DP{index}", f"DL{index}_PosR", 0),
                (f"DC{index}", f"DL{index}_Color", 55)):
            node = expr(
                unreal.MaterialExpressionCollectionParameter,
                x - 420, row_y + offset)
            node.set_editor_property("collection", collection)
            node.set_editor_property("parameter_name", parameter_name)
            add_input(input_name, node)
        row_y += 120

    custom.set_editor_property("inputs", inputs)
    for node, input_name in wires:
        _connect_or_die(mel, node, "", custom, input_name)
    return custom


def build_dynamic_world_factor(unreal, material, expr,
                               decoded_lightmap, decoded_pin, x, y, *,
                               lightmap_parameter):
    """Add G45 before texture modulation and clamp at UE1 stream byte 127.

    The moving-light term is reconstructed from lightmap meshels
    (``build_dynamic_meshel_sum``); ``lightmap_parameter`` is the master's
    atlas sampler parameter (required: its texel grid is the meshel grid)."""
    mel = unreal.MaterialEditingLibrary
    dynamic_sum = build_dynamic_meshel_sum(
        unreal, material, expr, lightmap_parameter, x - 260, y + 120)
    stream_scale = expr(unreal.MaterialExpressionMultiply, x, y + 120)
    stream_scale.set_editor_property("const_b", 254.0 / 63.75)
    mel.connect_material_expressions(dynamic_sum, "", stream_scale, "A")
    added = expr(unreal.MaterialExpressionAdd, x + 150, y)
    mel.connect_material_expressions(
        decoded_lightmap, decoded_pin, added, "A")
    mel.connect_material_expressions(stream_scale, "", added, "B")
    ceiling = expr(unreal.MaterialExpressionConstant, x + 150, y + 150)
    ceiling.set_editor_property("r", 127.0 / 63.75)
    clamped = expr(unreal.MaterialExpressionMin, x + 300, y)
    mel.connect_material_expressions(added, "", clamped, "A")
    mel.connect_material_expressions(ceiling, "", clamped, "B")
    return clamped
