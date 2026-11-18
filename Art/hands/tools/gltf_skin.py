"""Minimal glTF 2.0 binary (.glb) writer for one skinned, textured mesh.

Everything is given in glTF space (right-handed, +Y up, metres). Joint transforms are
GLOBAL rest transforms (4x4, numpy); local node transforms and inverse bind matrices are
derived here, so callers think only in global rest poses.
"""
import json
import struct

import numpy as np


def _quat_from_matrix(m):
    """xyzw quaternion from a 3x3 rotation matrix."""
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        w, x, y, z = 0.25 * s, (m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        w, x, y, z = (m[2, 1] - m[1, 2]) / s, 0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        w, x, y, z = (m[0, 2] - m[2, 0]) / s, (m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        w, x, y, z = (m[1, 0] - m[0, 1]) / s, (m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s
    q = np.array([x, y, z, w])
    return q / np.linalg.norm(q)


class _Bin:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def view(self, raw, target=None):
        while len(self.data) % 4:
            self.data += b"\0"
        v = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(raw)}
        if target:
            v["target"] = target
        self.data += raw
        self.views.append(v)
        return len(self.views) - 1

    def accessor(self, array, ctype, atype, target=None, minmax=False):
        array = np.ascontiguousarray(array)
        acc = {"bufferView": self.view(array.tobytes(), target), "componentType": ctype,
               "count": int(array.shape[0]), "type": atype}
        if minmax:
            acc["min"] = [float(v) for v in array.min(axis=0)]
            acc["max"] = [float(v) for v in array.max(axis=0)]
        self.accessors.append(acc)
        return len(self.accessors) - 1


def write_glb(path, *, positions, normals, uvs, indices, joint_names, joint_parents,
              joint_globals, joints, weights, png_bytes, material_name="Hand"):
    """positions/normals (N,3) f32, uvs (N,2) f32 (glTF v-down convention), indices (M,) u32,
    joint_parents: index or -1, joint_globals: list of 4x4 global rest transforms,
    joints (N,4) u16, weights (N,4) f32 (rows sum to 1)."""
    b = _Bin()
    pos = b.accessor(positions.astype(np.float32), 5126, "VEC3", 34962, minmax=True)
    nor = b.accessor(normals.astype(np.float32), 5126, "VEC3", 34962)
    tex = b.accessor(uvs.astype(np.float32), 5126, "VEC2", 34962)
    jnt = b.accessor(joints.astype(np.uint16), 5123, "VEC4", 34962)
    wgt = b.accessor(weights.astype(np.float32), 5126, "VEC4", 34962)
    idx = b.accessor(indices.astype(np.uint32), 5125, "SCALAR", 34963)
    inverse_binds = np.stack([np.linalg.inv(g).T for g in joint_globals]).astype(np.float32)  # column-major
    ibm = b.accessor(inverse_binds.reshape(len(joint_globals), 16), 5126, "MAT4")
    image_view = b.view(png_bytes)

    nodes = []
    children = {i: [] for i in range(len(joint_names))}
    for i, parent in enumerate(joint_parents):
        if parent >= 0:
            children[parent].append(i)
    for i, name in enumerate(joint_names):
        parent = joint_parents[i]
        local = joint_globals[i] if parent < 0 else np.linalg.inv(joint_globals[parent]) @ joint_globals[i]
        node = {"name": name, "translation": [float(v) for v in local[:3, 3]],
                "rotation": [float(v) for v in _quat_from_matrix(local[:3, :3])]}
        if children[i]:
            node["children"] = children[i]
        nodes.append(node)
    root = joint_parents.index(-1)
    mesh_node = len(nodes)
    nodes.append({"name": "HandMesh", "mesh": 0, "skin": 0})
    gltf = {
        "asset": {"version": "2.0", "generator": "wot hand-art gltf_skin.py"},
        "scene": 0, "scenes": [{"nodes": [root, mesh_node]}],
        "nodes": nodes,
        "skins": [{"joints": list(range(len(joint_names))), "inverseBindMatrices": ibm, "skeleton": root}],
        "meshes": [{"name": "HandMesh", "primitives": [{
            "attributes": {"POSITION": pos, "NORMAL": nor, "TEXCOORD_0": tex, "JOINTS_0": jnt, "WEIGHTS_0": wgt},
            "indices": idx, "material": 0}]}],
        "materials": [{"name": material_name, "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 1.0}}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"bufferView": image_view, "mimeType": "image/png"}],
        "buffers": [{"byteLength": 0}],
        "bufferViews": b.views, "accessors": b.accessors,
    }
    while len(b.data) % 4:
        b.data += b"\0"
    gltf["buffers"][0]["byteLength"] = len(b.data)
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    total = 12 + 8 + len(js) + 8 + len(b.data)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(b.data), 0x004E4942) + bytes(b.data))
