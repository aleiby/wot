"""Isolation: place the (red) re-centered sky mesh at PS34's camera via a PLAIN
StaticMeshActor scale 12 — no WoTSkyBox, no per-tick. If it renders, the mesh is
fine at the camera scaled and the WoTSkyBox tick path is the bug."""
import unreal
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
les.load_level("/Game/WoT/Maps/Battle2_02")
# remove the WoTSkyBox so only the control is at the camera
for a in eas.get_all_level_actors():
    if a.get_actor_label() == "SkyBox_Battle2_02" or a.get_actor_label() == "SkyCtrl":
        eas.destroy_actor(a)
sm = unreal.load_asset("/Game/WoT/Maps/Sky/Battle2_02/Battle2_02_skygeo")
PS34 = unreal.Vector(14864.45, 11192.92, -1359.73)   # PS34 camera (from log)
ctrl = eas.spawn_actor_from_class(unreal.StaticMeshActor, PS34)
ctrl.set_actor_label("SkyCtrl")
ctrl.static_mesh_component.set_static_mesh(sm)
ctrl.static_mesh_component.set_mobility(unreal.ComponentMobility.MOVABLE)
ctrl.set_actor_scale3d(unreal.Vector(12, 12, 12))
les.save_current_level()
b = ctrl.static_mesh_component.bounds
unreal.log(f"[dbg] control at {PS34} scale12 boundsR={b.sphere_radius}")
