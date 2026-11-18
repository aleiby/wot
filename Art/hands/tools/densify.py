#!/usr/bin/env python3
"""Per class: wrist plane + edge threshold (Blender space) for subdivide_hand.py.
usage: densify.py <class> <in.glb> <labels.npy> <covering> <params.json> [max_edge_cm]"""
import json
import sys

import numpy as np

sys.path.insert(0, __import__("os").path.dirname(__file__))
import hand_rig as hr  # noqa: E402

cls, glb, labels, covering, out = sys.argv[1:6]
max_cm = float(sys.argv[6]) if len(sys.argv) > 6 else 1.1
plane_cm = float(sys.argv[7]) if len(sys.argv) > 7 else 4.5  # only the bending part: knuckles, fingers, thumb joints
mesh, local, _ = hr.analyse(cls, glb, labels, [int(i) for i in covering.split(",")], None)
v, J = hr.landmarks(local, mesh["indices"])
k = np.linalg.norm(v) / np.linalg.norm(local)
to_blender = np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], float)  # glTF (Y up) -> Blender (Z up)
wrist_raw = mesh["frame"]["wrist_raw"] + mesh["frame"]["axis_raw"] * (plane_cm / k)
wrist_g = hr.P @ wrist_raw / 100.0
axis_g = hr.P @ mesh["frame"]["axis_raw"]
json.dump({"point": (to_blender @ wrist_g).tolist(), "axis": (to_blender @ axis_g).tolist(),
           "max_edge": max_cm / k / 100.0}, open(out, "w"))
print(f"{cls}: scale k={k:.4f}, max edge {max_cm} cm = {max_cm / k / 100.0:.5f} m, from x={plane_cm} cm")
