"""Split long edges on the HAND side of the wrist (fingers need vertices to bend), keeping
UVs and the texture; the sleeve/forearm is left alone. Meshy's remesh can leave fingers
with 2-6 cm triangles, which tear when a joint inside them bends.
usage: blender -b --factory-startup --python subdivide_hand.py -- in.glb out.glb params.json
params: {"point": [x,y,z], "axis": [x,y,z], "max_edge": L}  (Blender space, metres)
"""
import json
import sys

import bmesh
import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
src, dst, params = args[0], args[1], json.load(open(args[2]))
point, axis, max_edge = Vector(params["point"]), Vector(params["axis"]).normalized(), params["max_edge"]

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
obj = next(o for o in bpy.context.scene.objects if o.type == "MESH")
mw = obj.matrix_world
bm = bmesh.new()
bm.from_mesh(obj.data)
before = len(bm.faces)
for _ in range(4):
    bm.verts.ensure_lookup_table()
    long_edges = []
    for e in bm.edges:
        a, b = mw @ e.verts[0].co, mw @ e.verts[1].co
        if (a - b).length > max_edge and ((a - point).dot(axis) > 0 or (b - point).dot(axis) > 0):
            long_edges.append(e)
    if not long_edges:
        break
    bmesh.ops.subdivide_edges(bm, edges=long_edges, cuts=1, use_grid_fill=True)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
bm.to_mesh(obj.data)
bm.free()
obj.data.update()
bpy.ops.export_scene.gltf(filepath=dst, export_format="GLB", use_selection=False)
print(f"SUBDIVIDED faces {before} -> {len(obj.data.polygons)}")
