"""Render one hand GLB once per base-colour texture (same camera), for variant review.
usage: blender -b --factory-startup --python render_variants.py -- in.glb out_prefix tex1.png [tex2.png ...]
"""
import sys

import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
src, prefix, textures = args[0], args[1], args[2:]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
obj = next(o for o in bpy.context.scene.objects if o.type == "MESH")
tex_node = next(n for n in obj.active_material.node_tree.nodes if n.type == "TEX_IMAGE")
corners = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
lo = Vector(map(min, *corners))
hi = Vector(map(max, *corners))
center, radius = (lo + hi) / 2, (hi - lo).length / 2
scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 24
scene.cycles.use_denoising = False
scene.render.resolution_x = scene.render.resolution_y = 384
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.45, 0.45, 0.45, 1)
world.node_tree.nodes["Background"].inputs[1].default_value = 1.4
scene.world = world
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
scene.camera = cam
d = Vector((0.9, -0.8, 0.5)).normalized()
cam.location = center + d * radius * 2.6
cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
for i, tex in enumerate(textures):
    # The GLB's own image is packed, so re-pointing its filepath does nothing; load each one.
    tex_node.image = bpy.data.images.load(tex, check_existing=False)
    scene.render.filepath = f"{prefix}-{i}.png"
    bpy.ops.render.render(write_still=True)
print("RENDERED", len(textures))
