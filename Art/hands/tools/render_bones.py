"""Render a rigged hand with its bones showing: the mesh semi-transparent, each bone a thin
emissive cylinder (from joints.json, UE space) and each joint a small sphere. Top + side views.
usage: blender -b --factory-startup --python render_bones.py -- in.glb joints.json out_prefix
"""
import json
import sys

import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
src, joints_path, prefix = args
PARENTS = {"wrist_r": "forearm_r", "palm_r": "wrist_r", "grip_r": "wrist_r"}
for chain, segs in (("thumb", ("metacarpal", "proximal", "distal", "tip")),) + tuple(
        (f, ("metacarpal", "proximal", "intermediate", "distal", "tip")) for f in ("index", "middle", "ring", "little")):
    parent = "wrist_r"
    for seg in segs:
        PARENTS[f"{chain}_{seg}_r"] = parent
        parent = f"{chain}_{seg}_r"


def ue_to_blender(p):
    # UE cm (X fwd, Y right, Z up) -> glTF m via the measured mapping -> Blender (Z up).
    gx, gy, gz = p[0] / 100.0, p[2] / 100.0, p[1] / 100.0
    return Vector((gx, -gz, gy))


bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
for o in list(bpy.context.scene.objects):
    if o.type == "ARMATURE":
        o.hide_render = True
mesh = next(o for o in bpy.context.scene.objects if o.type == "MESH")
mat = mesh.active_material
bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
bsdf.inputs["Alpha"].default_value = 0.35
mat.blend_method = "BLEND"
J = {k: ue_to_blender(v) for k, v in json.load(open(joints_path)).items()}


def emissive(name, rgb):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nodes = m.node_tree.nodes
    nodes.clear()
    em = nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = (*rgb, 1)
    em.inputs["Strength"].default_value = 3.0
    out = nodes.new("ShaderNodeOutputMaterial")
    m.node_tree.links.new(em.outputs[0], out.inputs[0])
    return m


bone_mat, joint_mat, tip_mat = emissive("bone", (0.1, 0.8, 1.0)), emissive("joint", (1.0, 0.9, 0.2)), emissive("tip", (1.0, 0.3, 0.3))
for name, parent in PARENTS.items():
    a, b = J[parent], J[name]
    if name in ("palm_r", "grip_r"):
        continue
    d = b - a
    bpy.ops.mesh.primitive_cylinder_add(radius=0.0012, depth=d.length, location=(a + b) / 2)
    cyl = bpy.context.object
    cyl.rotation_euler = d.to_track_quat("Z", "Y").to_euler()
    cyl.data.materials.append(bone_mat)
for name, p in J.items():
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.0028 if name not in ("palm_r", "grip_r") else 0.004, location=p)
    bpy.context.object.data.materials.append(tip_mat if name.endswith("tip_r") or name == "grip_r" else joint_mat)

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 32
scene.cycles.use_denoising = False
scene.render.resolution_x, scene.render.resolution_y = 900, 520
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.18, 0.18, 0.2, 1)
scene.world = world
pts = list(J.values())
lo = Vector(map(min, *pts))
hi = Vector(map(max, *pts))
center = (lo + hi) / 2
cam_data = bpy.data.cameras.new("cam")
cam_data.type = "ORTHO"
cam_data.ortho_scale = (hi - lo).length * 1.15
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
# Blender views of UE +Z (back of hand, top) and UE -Y (thumb side).
for view, direction, up in (("top", Vector((0, 0, 1)), "Y"), ("side", Vector((0, -1, 0)), "Y")):
    d = ue_to_blender([direction.x, direction.y, direction.z])
    d = Vector((d.x, d.y, d.z)).normalized() if d.length else Vector((0, 0, 1))
    cam.location = center + d * 0.6
    cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
    scene.render.filepath = f"{prefix}-{view}.png"
    bpy.ops.render.render(write_still=True)
print("RENDERED bones", prefix)
