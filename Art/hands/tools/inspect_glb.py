"""Headless Blender check of a hand GLB: triangle count, bounds, materials and texture
sizes, plus two Cycles CPU preview renders (top and side).
usage: blender -b --factory-startup --python inspect_glb.py -- in.glb out_prefix
"""
import json
import math
import sys

import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
src, prefix = args[0], args[1]

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
tris = 0
lo = Vector((1e9, 1e9, 1e9))
hi = Vector((-1e9, -1e9, -1e9))
for o in meshes:
    o.data.calc_loop_triangles()
    tris += len(o.data.loop_triangles)
    for corner in o.bound_box:
        w = o.matrix_world @ Vector(corner)
        lo = Vector(map(min, lo, w))
        hi = Vector(map(max, hi, w))
images = {i.name: list(i.size) for i in bpy.data.images}
report = {
    "file": src, "meshes": len(meshes), "triangles": tris,
    "bounds_min": [round(v, 4) for v in lo], "bounds_max": [round(v, 4) for v in hi],
    "size": [round(v, 4) for v in (hi - lo)],
    "materials": [m.name for m in bpy.data.materials], "images": images,
}
print("INSPECT " + json.dumps(report))

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 24
scene.cycles.use_denoising = False
scene.render.resolution_x = scene.render.resolution_y = 640
scene.render.film_transparent = False
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.45, 0.45, 0.45, 1)
world.node_tree.nodes["Background"].inputs[1].default_value = 1.4
scene.world = world
center = (lo + hi) / 2
radius = max((hi - lo).length / 2, 1e-3)
cam_data = bpy.data.cameras.new("cam")
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
for name, direction in (("top", Vector((0.25, -0.35, 1.0))), ("side", Vector((1.0, -0.6, 0.35)))):
    d = direction.normalized()
    cam.location = center + d * radius * 3.2
    cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
    cam_data.lens = 50
    scene.render.filepath = f"{prefix}-{name}.png"
    bpy.ops.render.render(write_still=True)
print("RENDERED " + prefix)
