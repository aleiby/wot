"""Bake a Meshy hand GLB's base colour down to a 512x512 texture (the era budget) on its
own UVs. The bake margin dilates island edges so the downsized atlas doesn't bleed.
(A smart-project re-unwrap was tried and rejected: the remeshed triangles' noisy normals
shattered it into hundreds of tiny islands.)
usage: blender -b --factory-startup --python rebake_512.py -- in.glb out.glb out_basecolor.png [size]
"""
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:]
src, out_glb, out_png = args[0], args[1], args[2]
size = int(args[3]) if len(args) > 3 else 512

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
obj = next(o for o in bpy.context.scene.objects if o.type == "MESH")
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
mesh = obj.data
old_uv = mesh.uv_layers.active.name

material = obj.active_material
nodes, links = material.node_tree.nodes, material.node_tree.links
src_tex = next(n for n in nodes if n.type == "TEX_IMAGE")
uv_old = nodes.new("ShaderNodeUVMap")
uv_old.uv_map = old_uv
links.new(uv_old.outputs["UV"], src_tex.inputs["Vector"])
target = bpy.data.images.new("BaseColor512", width=size, height=size, alpha=False)
target_node = nodes.new("ShaderNodeTexImage")
target_node.image = target
uv_new = nodes.new("ShaderNodeUVMap")
uv_new.uv_map = old_uv
links.new(uv_new.outputs["UV"], target_node.inputs["Vector"])
# Bake the source colour exactly: route it through an emission shader for the bake.
output = next(n for n in nodes if n.type == "OUTPUT_MATERIAL")
original_surface = output.inputs["Surface"].links[0].from_socket if output.inputs["Surface"].links else None
emit = nodes.new("ShaderNodeEmission")
links.new(src_tex.outputs["Color"], emit.inputs["Color"])
links.new(emit.outputs["Emission"], output.inputs["Surface"])
nodes.active = target_node

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 16
scene.cycles.use_denoising = False
scene.render.bake.margin = 6
bpy.ops.object.bake(type="EMIT")
target.filepath_raw = out_png
target.file_format = "PNG"
target.save()

# Final material: the baked 512 texture on the new UVs only.
principled = next(n for n in nodes if n.type == "BSDF_PRINCIPLED")
links.new(target_node.outputs["Color"], principled.inputs["Base Color"])
if original_surface is not None:
    links.new(original_surface, output.inputs["Surface"])
for n in (emit, uv_old, src_tex):
    nodes.remove(n)
bpy.ops.export_scene.gltf(filepath=out_glb, export_format="GLB", use_selection=False)
print(f"REBAKED {out_glb} {out_png} {size} tris={sum(len(p.vertices) - 2 for p in mesh.polygons)}")
