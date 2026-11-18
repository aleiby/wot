#!/usr/bin/env python3
"""Mark sleeve texels by geometry when colour can't separate them from the hand (e.g. a
black glove and a near-black sleeve): faces on the elbow side of the wrist trim become
label 100. The wrist is the median long-axis position of faces showing the trim colour.
usage: geometry_sleeve.py faces.json labels.npy trim_cluster [hand_side: +1|-1]
(faces.json from dump_faces.py; labels.npy is updated in place, original kept as labels-colour.npy)
"""
import json
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw

SLEEVE = 100

faces_path, labels_path, trim = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = json.load(open(faces_path))
labels = np.load(labels_path)
shutil.copy(labels_path, labels_path.replace(".npy", "-colour.npy"))
size = labels.shape[0]


def texel(u, v):
    # dump_faces.py records Blender UVs (V up); image rows run down, as in glTF.
    return labels[min(size - 1, max(0, int((1 - v) * size))), min(size - 1, max(0, int(u * size)))]


trim_a = [t["a"] for t in d["tris"] if texel(*np.mean(t["uv"], axis=0)) == trim]
wrist = float(np.median(trim_a))
# The hand is the side with more triangles (fingers); the sleeve is the other side.
below = sum(1 for t in d["tris"] if t["a"] < wrist)
hand_below = below > len(d["tris"]) - below
margin = 0.0105 * (d["hi"] - d["lo"])  # ~2% of the hand+forearm half-length
img = Image.new("L", (size, size), 0)
draw = ImageDraw.Draw(img)
for t in d["tris"]:
    sleeve = t["a"] > wrist + margin if hand_below else t["a"] < wrist - margin
    if sleeve:
        draw.polygon([(u * size, (1 - v) * size) for u, v in t["uv"]], fill=255)
geo = np.asarray(img) > 0
out = labels.copy()
out[geo & (labels != trim) & (labels >= 0)] = SLEEVE
np.save(labels_path, out)
print(f"wrist at {wrist:.4f} on the {d['axis']} axis; sleeve texels {int((out == SLEEVE).sum())}")
