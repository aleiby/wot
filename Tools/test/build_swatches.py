"""Build a test level with unlit constant-emissive swatches at known linear
values, to measure the output transfer function (tonemapper-off) directly."""
import unreal

EAL = unreal.EditorAssetLibrary
ATT = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary

VALUES = [0.01, 0.02, 0.05, 0.10, 0.20, 0.35, 0.50, 0.70, 1.00, 1.50, 2.00]

# materials
for i, v in enumerate(VALUES):
    path = f"/Game/WoT/Test/M_Swatch_{i:02d}"
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    m = ATT.create_asset(f"M_Swatch_{i:02d}", "/Game/WoT/Test", unreal.Material,
                         unreal.MaterialFactoryNew())
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    c = MEL.create_material_expression(m, unreal.MaterialExpressionConstant3Vector, -300, 0)
    c.set_editor_property("constant", unreal.LinearColor(v, v, v, 1.0))
    MEL.connect_material_property(c, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(m)
    EAL.save_asset(path)

les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
level_path = "/Game/WoT/Test/Swatches"
if EAL.does_asset_exist(level_path):
    if not les.load_level(level_path):
        raise RuntimeError("load failed")
    eas0 = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for a in eas0.get_all_level_actors():
        eas0.destroy_actor(a)
elif not les.new_level(level_path):
    raise RuntimeError("new_level failed")

eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
cube = unreal.load_asset("/Engine/BasicShapes/Cube")
for i, v in enumerate(VALUES):
    a = eas.spawn_actor_from_class(unreal.StaticMeshActor,
                                   unreal.Vector(0, i * 220.0, 200))
    a.static_mesh_component.set_static_mesh(cube)
    a.static_mesh_component.set_material(0, unreal.load_asset(f"/Game/WoT/Test/M_Swatch_{i:02d}"))
    a.set_actor_scale3d(unreal.Vector(0.1, 1.8, 3.0))
    a.set_actor_label(f"Swatch_{i:02d}_{v}")
eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(-1400, 1100, 200))
les.save_current_level()
unreal.log(f"SWATCH-OK {len(VALUES)} swatches")
