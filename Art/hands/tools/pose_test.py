#!/usr/bin/env python3
"""Deformation check: curl the rigged hand with linear blend skinning, flexing each bone
about its LOCAL +Y exactly as the game code will (+angle moves local +X toward local -Z),
and write one static GLB per pose for rendering.
usage: pose_test.py <class> <stage3 class dir> <basecolor.png> <source.glb> <labels.npy> <covering>
"""
import io
import json
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import hand_rig as hr  # noqa: E402
from gltf_skin import write_glb  # noqa: E402

POSES = {
    "relaxed": {"proximal": 15, "intermediate": 20, "distal": 10, "thumb": (5, 10, 10)},
    "grip": {"proximal": 55, "intermediate": 75, "distal": 45, "thumb": (20, 30, 35)},
    "point": {"proximal": 60, "intermediate": 85, "distal": 50, "thumb": (25, 35, 40), "straight": ["index"]},
}


def roty(deg):
    a = np.radians(deg)
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def main():
    cls, d, png, src, labels, covering = sys.argv[1:7]
    d = pathlib.Path(d)
    mesh, local, _ = hr.analyse(cls, src, labels, [int(i) for i in covering.split(",")], d)
    v, J = hr.landmarks(local, mesh["indices"])
    R = hr.bone_frames(J)
    names, joints, weights = hr.skin_weights(v, J)
    parent = dict(hr.BONES)
    rest = {}
    for n in names:
        m = np.eye(4)
        m[:3, :3] = R[n]
        m[:3, 3] = J[n]
        rest[n] = m
    for pose, spec in POSES.items():
        posed = {}
        for n in names:  # BONES is parent-first
            local_rest = rest[n] if parent[n] is None else np.linalg.inv(rest[parent[n]]) @ rest[n]
            angle = 0.0
            chain, seg = n[:-2].split("_", 1) if n.count("_") >= 2 else (n, "")
            if chain == "thumb" and seg in ("metacarpal", "proximal", "distal"):
                angle = spec["thumb"][("metacarpal", "proximal", "distal").index(seg)]
            elif chain in hr.FINGERS and seg in ("proximal", "intermediate", "distal"):
                angle = 0.0 if chain in spec.get("straight", []) else spec[seg]
            flex = np.eye(4)
            flex[:3, :3] = roty(angle)
            local_posed = local_rest @ flex
            posed[n] = local_posed if parent[n] is None else posed[parent[n]] @ local_posed
        skin = np.stack([posed[n] @ np.linalg.inv(rest[n]) for n in names])
        vh = np.c_[v, np.ones(len(v))]
        out = np.zeros_like(v)
        for k in range(4):
            out += weights[:, k:k + 1] * np.einsum("nij,nj->ni", skin[joints[:, k]], vh)[:, :3]
        pos_g = (hr.P @ out.T).T / 100.0
        nrm_g = hr.smooth_normals(pos_g, mesh["indices"])
        write_glb(str(d / f"pose-{pose}.glb"), positions=pos_g, normals=nrm_g, uvs=mesh["uvs"].astype(np.float32),
                  indices=mesh["indices"].ravel(), joint_names=["root"], joint_parents=[-1], joint_globals=[np.eye(4)],
                  joints=np.zeros((len(v), 4), np.uint16), weights=np.tile([1, 0, 0, 0], (len(v), 1)).astype(np.float32),
                  png_bytes=pathlib.Path(png).read_bytes())
        print("wrote", pose)


if __name__ == "__main__":
    main()
