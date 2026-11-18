"""Dump a hand GLB's triangles (UVs plus centroid along the mesh's longest axis) to JSON,
for geometry-based texture masks (e.g. a sleeve vs a glove of the same colour).
usage: blender -b --factory-startup --python dump_faces.py -- in.glb out.json
"""
import json
import sys

import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=args[0])
obj = next(o for o in bpy.context.scene.objects if o.type == "MESH")
mesh = obj.data
mesh.calc_loop_triangles()
uv = mesh.uv_layers.active.data
world = [obj.matrix_world @ v.co for v in mesh.vertices]
lo = Vector(map(min, *world))
hi = Vector(map(max, *world))
axis = max(range(3), key=lambda i: hi[i] - lo[i])
tris = []
for t in mesh.loop_triangles:
    centroid = sum((world[v] for v in t.vertices), Vector()) / 3
    tris.append({"uv": [list(uv[l].uv) for l in t.loops], "a": round(centroid[axis], 5)})
json.dump({"axis": "xyz"[axis], "lo": lo[axis], "hi": hi[axis], "tris": tris}, open(args[1], "w"))
print("DUMPED", len(tris), "axis", "xyz"[axis], round(lo[axis], 3), round(hi[axis], 3))
