"""Build the retail-style modulated mesh material master.

The blend is FB = 2 * Tex * Dst per channel, with neutral gray as identity.
The source is the raw texture sample: vertex lighting and actor intensity are
ignored, and the material adds no fog term or secondary fog pass.
"""
import unreal


ASSET_PATH = "/Game/WoT/Materials/M_WoT_MeshModulate"
NEUTRAL_PATH = "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral"


def _lin_sampler(node, gamma_space=True):
    """Match the import pipeline's raw display-byte texture sampling."""
    if gamma_space:
        neutral = unreal.load_asset(NEUTRAL_PATH)
        if neutral is None:
            raise RuntimeError(
                f"linear sampler default texture is missing: {NEUTRAL_PATH}")
        node.set_editor_property("texture", neutral)
        node.set_editor_property(
            "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    return node


def make_mesh_modulate_master(gamma_space=True):
    """Create M_WoT_MeshModulate, or load the existing idempotent asset."""
    eal = unreal.EditorAssetLibrary
    if eal.does_asset_exist(ASSET_PATH):
        material = unreal.load_asset(ASSET_PATH)
        changed = False
        # MTT_Modulate is inherently two-sided in the shipped mesh importer.
        if not material.get_editor_property("two_sided"):
            material.set_editor_property("two_sided", True)
            changed = True
        # Runtime STY_Modulated sprites use instanced plane meshes. Without
        # this cooked usage the 2x graph falls back to the default material.
        if not material.get_editor_property("used_with_instanced_static_meshes"):
            material.set_editor_property("used_with_instanced_static_meshes", True)
            changed = True
        if changed:
            unreal.MaterialEditingLibrary.recompile_material(material)
            eal.save_asset(ASSET_PATH)
        return material

    mel = unreal.MaterialEditingLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(
        "M_WoT_MeshModulate", "/Game/WoT/Materials",
        unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MODULATE)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("used_with_skeletal_mesh", True)
    material.set_editor_property("used_with_instanced_static_meshes", True)
    # Retail modulated skips the fog pass entirely; UE5 defaults translucency
    # vertex fog ON, which would fade the multiplier toward identity in fog.
    material.set_editor_property("use_translucency_vertex_fog", False)

    tex = _lin_sampler(mel.create_material_expression(
        material, unreal.MaterialExpressionTextureSampleParameter2D, -400, 0),
        gamma_space)
    tex.set_editor_property("parameter_name", "Tex")
    scaled = mel.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -100, 0)
    scaled.set_editor_property(
        "const_b", 2.0 if gamma_space else 2.0 ** 2.2)
    mel.connect_material_expressions(tex, "RGB", scaled, "A")
    mel.connect_material_property(
        scaled, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    mel.recompile_material(material)
    eal.save_asset(ASSET_PATH)
    return material


if __name__ == "__main__":
    result = make_mesh_modulate_master()
    unreal.log(f"MESH-MODULATE-OK {result.get_path_name()}")
