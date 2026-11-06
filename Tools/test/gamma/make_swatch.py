"""Build /Game/WoT/Maps/GammaSwatch: a wall of unlit emissive patches at
known values D = i/255 for i in 8..248 step 8, NO gamma-boundary PPV.
Rendering it and reading bytes measures the output transform E directly:
out_byte = 255*E(D). A second render with the boundary MI in a PPV then
measures the net chain. Kills all encode speculation."""
import unreal

EAL = unreal.EditorAssetLibrary
ATT = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary

VALUES = list(range(8, 256, 8))   # 31 patches

# one shared master with a Value vector param
mp = "/Game/WoT/Materials/M_WoT_Swatch"
if not EAL.does_asset_exist(mp):
    m = ATT.create_asset("M_WoT_Swatch", "/Game/WoT/Materials",
                         unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    v = MEL.create_material_expression(
        m, unreal.MaterialExpressionVectorParameter, -300, 0)
    v.set_editor_property("parameter_name", "Value")
    MEL.connect_material_property(v, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(m)
    EAL.save_asset(mp)
master = unreal.load_asset(mp)

les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
lp = "/Game/WoT/Maps/GammaSwatch"
if EAL.does_asset_exist(lp):
    les.load_level(lp)
    eas0 = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for a in eas0.get_all_level_actors():
        eas0.destroy_actor(a)
elif not les.new_level(lp):
    raise RuntimeError("new_level failed")
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

plane = unreal.load_asset("/Engine/BasicShapes/Plane")
for idx, val in enumerate(VALUES):
    mic_path = f"/Game/WoT/Materials/Instances/MI_Swatch_{val}"
    if not EAL.does_asset_exist(mic_path):
        mic = ATT.create_asset(f"MI_Swatch_{val}",
                               "/Game/WoT/Materials/Instances",
                               unreal.MaterialInstanceConstant,
                               unreal.MaterialInstanceConstantFactoryNew())
        mic.set_editor_property("parent", master)
        MEL.set_material_instance_vector_parameter_value(
            mic, "Value", unreal.LinearColor(val / 255.0, val / 255.0,
                                             val / 255.0, 1.0))
        EAL.save_asset(mic_path)
    mic = unreal.load_asset(mic_path)
    # grid: 8 cols, 60uu pitch, wall at x=+300 facing -x toward the camera
    r, c = divmod(idx, 8)
    a = eas.spawn_actor_from_class(
        unreal.StaticMeshActor,
        unreal.Vector(300.0, (c - 3.5) * 60.0, 200.0 - r * 60.0))
    a.static_mesh_component.set_static_mesh(plane)
    a.static_mesh_component.set_material(0, mic)
    a.set_actor_rotation(unreal.Rotator(0.0, 90.0, 90.0), False)
    a.set_actor_scale3d(unreal.Vector(0.5, 0.5, 0.5))
    a.set_actor_label(f"Swatch_{val}")

ps = eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(0, 0, 100))
ps.set_actor_label("PlayerStart0")
les.save_current_level()
unreal.log(f"SWATCH-OK patches={len(VALUES)}")
