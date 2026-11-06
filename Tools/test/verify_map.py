"""Headless check: load built level, assert actor/mesh sanity."""
import sys
import unreal

MAP = sys.argv[1] if len(sys.argv) > 1 else "Arena_01"
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
assert les.load_level(f"/Game/WoT/Maps/{MAP}"), "level load failed"
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = eas.get_all_level_actors()
sm = [a for a in actors if isinstance(a, unreal.StaticMeshActor)]
ps = [a for a in actors if isinstance(a, unreal.PlayerStart)]
assert sm and ps, f"missing actors: sm={len(sm)} ps={len(ps)}"
mesh = sm[0].static_mesh_component.static_mesh
b = sm[0].get_actor_bounds(False)
ext = b[1]
unreal.log(f"VERIFY-MAP-OK actors={len(actors)} sections={mesh.get_num_sections(0)} "
           f"tris~{mesh.get_num_triangles(0)} extent=({ext.x:.0f},{ext.y:.0f},{ext.z:.0f})")
