#!/usr/bin/env python3
"""Rig a Meshy hand to the first-person hand skeleton contract v1 and write a skinned .glb.

Pipeline (all numpy; the glb is written by gltf_skin.write_glb):
  1. Read the baked GLB (glTF space) and move it into UE space (cm; X fwd, Y right, Z up),
     using the measured Interchange mapping UE = 100 * (gx, gz, gy).
  2. Find the contract frame from the geometry: arm axis (PCA, +X toward the fingers),
     wrist (distal edge of the class's cuff, from texture clusters), palm normal (thin axis
     of the hand), thumb side -> -Y, so +Z is the back of the hand. Origin = wrist joint;
     scale so wrist -> middle fingertip = TARGET_LENGTH cm.
  3. Landmarks: fingertips and finger webs from the silhouette; knuckles and finger joints by
     standard proportions along each finger's own axis; thumb chain likewise.
  4. Bones with contract axes, generated skin weights (<= 4 influences), export.
usage: hand_rig.py analyse <class> <in.glb> <labels.npy> <covering ids> <outdir>
"""
import json
import pathlib
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

TARGET_LENGTH = 18.5  # cm, wrist joint to middle fingertip (contract: 18-19)
P = np.array([[1, 0, 0], [0, 0, 1], [0, 1, 0]], dtype=float)  # swap Y/Z (its own inverse)
FINGERS = ["index", "middle", "ring", "little"]


def read_glb(path):
    data = pathlib.Path(path).read_bytes()
    jlen = struct.unpack("<I", data[12:16])[0]
    gltf = json.loads(data[20:20 + jlen])
    blen = struct.unpack("<I", data[20 + jlen:24 + jlen])[0]
    binary = data[28 + jlen:28 + jlen + blen]
    comp = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}
    width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}

    def acc(i):
        a = gltf["accessors"][i]
        v = gltf["bufferViews"][a["bufferView"]]
        off = v.get("byteOffset", 0) + a.get("byteOffset", 0)
        n = a["count"] * width[a["type"]]
        arr = np.frombuffer(binary, comp[a["componentType"]], n, off)
        return arr.reshape(a["count"], width[a["type"]]) if width[a["type"]] > 1 else arr
    prim = gltf["meshes"][0]["primitives"][0]
    image = gltf["images"][0]
    view = gltf["bufferViews"][image["bufferView"]]
    png = binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]]
    node = next((n for n in gltf["nodes"] if "mesh" in n), {})
    return {"positions": acc(prim["attributes"]["POSITION"]).astype(float),
            "uvs": acc(prim["attributes"]["TEXCOORD_0"]).astype(float),
            "indices": acc(prim["indices"]).astype(np.uint32).reshape(-1, 3),
            "png": png, "node": node}


def node_matrix(node):
    m = np.eye(4)
    if "matrix" in node:
        return np.array(node["matrix"], dtype=float).reshape(4, 4).T
    if "scale" in node:
        m = np.diag(list(node["scale"]) + [1.0]) @ m
    if "rotation" in node:
        x, y, z, w = node["rotation"]
        r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                      [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                      [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        rm = np.eye(4); rm[:3, :3] = r
        m = rm @ m
    if "translation" in node:
        tm = np.eye(4); tm[:3, 3] = node["translation"]
        m = tm @ m
    return m


def vertex_labels(uvs, labels):
    size = labels.shape[0]
    x = np.clip((uvs[:, 0] * size).astype(int), 0, size - 1)
    y = np.clip((uvs[:, 1] * size).astype(int), 0, size - 1)  # glTF uv: v down = image row
    return labels[y, x]


def analyse(cls, glb, labels_path, covering, outdir):
    mesh = read_glb(glb)
    world = node_matrix(mesh["node"])
    pg = (world[:3, :3] @ mesh["positions"].T).T + world[:3, 3]
    v = (P @ pg.T).T * 100.0  # UE space, cm (Meshy units are arbitrary until rescaled)
    labels = vertex_labels(mesh["uvs"], np.load(labels_path))

    # Arm axis: PCA long axis, pointing toward the denser (finger) end.
    c = v.mean(0)
    _, _, vt = np.linalg.svd(v - c, full_matrices=False)
    ax = vt[0]
    t = (v - c) @ ax
    # The hand is the end with LESS covering (sleeve/cuff/bracer); triangle density is not
    # reliable (a quilted bracer can out-number the fingers).
    cover_all = np.isin(labels, covering)
    if cover_all[t > 0].mean() > cover_all[t < 0].mean():
        ax, t = -ax, -t
    # Wrist: distal edge of the covering (cuff/band/trim/bracer) along the arm axis.
    # Scan slices from the elbow end; the wrist is where the covering stops being the
    # majority (stray covering-coloured texels inside the hand don't move it).
    cover = np.isin(labels, covering)
    edges = np.linspace(t.min(), t.max(), 81)
    frac = [cover[(t >= a) & (t < b)].mean() if ((t >= a) & (t < b)).any() else 0.0
            for a, b in zip(edges[:-1], edges[1:])]
    # The wrist is the distal edge of the LAST covering-majority slice (a strap or plate gap
    # inside a vambrace can dip below half before the real end of the covering).
    last_cover = max(i for i, f in enumerate(frac) if f > 0.5)
    wrist_t = float(edges[last_cover + 1])
    hand = t > wrist_t
    # Palm normal: thinnest axis of the hand region, orthogonal to the arm axis.
    h = v[hand] - v[hand].mean(0)
    h_perp = h - np.outer(h @ ax, ax)
    _, _, vt2 = np.linalg.svd(h_perp, full_matrices=False)
    width_dir, thin_dir = vt2[0], vt2[1]
    # Thumb: the hand vertex reaching farthest sideways; it sets -Y.
    side = h @ width_dir
    thumb_sign = np.sign(side[np.argmax(np.abs(side))])
    y_axis = -thumb_sign * width_dir
    x_axis = ax
    z_axis = np.cross(x_axis, y_axis)
    if np.dot(z_axis, thin_dir) < 0:
        thin_dir = -thin_dir
    rot = np.stack([x_axis, y_axis, z_axis])  # rows: new axes
    wrist_point = c + ax * wrist_t
    local = (rot @ (v - wrist_point).T).T
    # Centre the wrist on the arm's cross-section just past the covering.
    ring = (local[:, 0] > 0) & (local[:, 0] < 1.5 * np.ptp(local[:, 0]) / 50)
    shift = np.array([0, local[ring, 1].mean(), local[ring, 2].mean()])
    local -= shift
    mesh["frame"] = {"wrist_raw": wrist_point + rot.T @ shift, "axis_raw": ax}
    return mesh, local, labels


def tips_and_webs(local):
    """Fingertips (index..little), thumb tip, and finger webs, from the hand silhouette."""
    hand = local[local[:, 0] > 0]
    y = hand[:, 1]
    bins = np.linspace(y.min(), y.max(), 121)
    prof = np.full(len(bins) - 1, -np.inf)
    which = np.digitize(y, bins) - 1
    for i in range(len(prof)):
        sel = which == i
        if sel.any():
            prof[i] = hand[sel, 0].max()
    smooth = np.convolve(np.where(np.isfinite(prof), prof, prof[np.isfinite(prof)].min()), np.ones(3) / 3, "same")
    peaks = [i for i in range(2, len(smooth) - 2) if smooth[i] >= smooth[i - 1] and smooth[i] >= smooth[i + 1]
             and smooth[i] >= smooth[max(0, i - 6):i + 7].max() - 1e-9]
    return bins, prof, smooth, peaks


def _pca_line(pts, toward):
    c = pts.mean(0)
    _, _, vt = np.linalg.svd(pts - c, full_matrices=False)
    d = vt[0] if np.dot(vt[0], toward) > 0 else -vt[0]
    return c, d


RES = 0.05  # cm per pixel for the top-view silhouette


def _silhouette(v, faces):
    import cv2
    lo = v[:, :2].min(0) - 1.0
    hi = v[:, :2].max(0) + 1.0
    w, h = (np.ceil((hi - lo) / RES)).astype(int)
    mask = np.zeros((h, w), np.uint8)
    pix = ((v[:, :2] - lo) / RES).astype(np.int32)
    for f in faces:
        cv2.fillPoly(mask, [pix[f]], 255)
    return mask, lo


def _px(p, lo):
    return (p[:2] - lo) / RES


def _cm(px, lo):
    return np.asarray(px, float) * RES + lo


def _mid_z(v, xy, radius=0.5):
    near = v[np.hypot(v[:, 0] - xy[0], v[:, 1] - xy[1]) < radius]
    return 0.5 * (near[:, 2].min() + near[:, 2].max()) if len(near) else 0.0


def _centerline(mask, lo, tip_xy, direction, length):
    """Midpoints of perpendicular silhouette cross-sections, walking back from the tip."""
    d = direction / np.linalg.norm(direction)
    n = np.array([-d[1], d[0]])
    pts = []
    centre = np.asarray(tip_xy, float)
    for s in np.arange(0.4, length, 0.1):
        base = tip_xy - d * s
        offs = np.arange(-1.6, 1.6, RES / 2)
        samples = base[None] + offs[:, None] * n[None]
        px = np.clip(((samples - lo) / RES).astype(int), 0, [mask.shape[1] - 1, mask.shape[0] - 1])
        inside = mask[px[:, 1], px[:, 0]] > 0
        k0 = int(np.argmin(np.abs(offs - (centre - base) @ n)))
        if not inside[k0]:
            break
        a = k0
        while a > 0 and inside[a - 1]:
            a -= 1
        b = k0
        while b < len(offs) - 1 and inside[b + 1]:
            b += 1
        centre = base + n * 0.5 * (offs[a] + offs[b])
        pts.append(centre.copy())
    return np.array(pts)


def landmarks(local, faces):
    """Scale the hand so wrist -> middle tip = TARGET_LENGTH and place all contract joints.
    Returns (scaled vertices, {joint name: UE-space position in cm})."""
    # Scale from the farthest fingertip, then refine once from the measured middle tip.
    v = local * (TARGET_LENGTH / local[:, 0].max())
    v, J = _landmarks_scaled(v, faces)
    k = TARGET_LENGTH / np.linalg.norm(J["middle_tip_r"])
    if abs(k - 1.0) > 0.003:
        v, J = _landmarks_scaled(v * k, faces)
    return v, J


def _landmarks_scaled(v, faces):
    import cv2
    mask, lo = _silhouette(v[v[:, 0] > -1.0], faces_hand(v, faces))
    contour = max(cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)[0], key=cv2.contourArea)
    hull = cv2.convexHull(contour, returnPoints=False)
    defects = cv2.convexityDefects(contour, hull)
    # A finger valley's hull edge spans two digit tips, so both hull endpoints lie far from
    # the wrist; concavities beside the wrist/cuff have one endpoint near it. Keep the 4 deepest.
    cands = []
    for s0, e0, f0, depth in defects[:, 0]:
        a, b = _cm(contour[s0, 0], lo), _cm(contour[e0, 0], lo)
        if depth / 256.0 * RES > 0.8 and min(a[0], b[0]) > 0.4 * TARGET_LENGTH:
            cands.append((depth, int(f0)))
    vidx = [f for _, f in sorted(cands, reverse=True)[:4]]
    if len(vidx) != 4:
        raise SystemExit(f"expected 4 finger valleys, found {len(vidx)}")
    pts = contour[:, 0].astype(float) * RES + lo
    n = len(pts)
    # Valleys sit near the palm where splay is small, so their y order is reliable:
    # thumb-index, index-middle, middle-ring, ring-little. Walk the short way between them.
    seq = sorted(vidx, key=lambda i: pts[i, 1])
    step = 1 if (seq[1] - seq[0]) % n < n // 2 else -1

    def walk(a, b):
        out, i = [], a
        while i != b:
            out.append(i)
            i = (i + step) % n
        return out
    valleys = [pts[i] for i in seq]
    palm_c = np.array([0.35 * TARGET_LENGTH, float(np.mean([p[1] for p in valleys]))])
    segs = [walk(seq[0], seq[1]), walk(seq[1], seq[2]), walk(seq[2], seq[3])]
    # Little finger: past the ring-little valley until the outline falls back toward the wrist.
    little, i = [], seq[3]
    while pts[i, 0] > valleys[3][0] - 2.5 or len(little) < 20:
        little.append(i)
        i = (i + step) % n
    segs.append(little)
    J = {"wrist_r": np.zeros(3)}
    for i, name in enumerate(FINGERS):
        seg = pts[segs[i]]
        tip_xy = seg[np.argmax(np.linalg.norm(seg - palm_c, axis=1))]
        if i == 0:
            web_xy = valleys[1]
        elif i == 3:
            web_xy = valleys[3]
        else:
            web_xy = 0.5 * (valleys[i] + valleys[i + 1])
        line = _centerline(mask, lo, tip_xy, tip_xy - web_xy, np.linalg.norm(tip_xy - web_xy))
        c = line.mean(0)
        _, _, vt = np.linalg.svd(line - c)
        d = vt[0] if np.dot(vt[0], tip_xy - web_xy) > 0 else -vt[0]
        tip2 = c + d * np.dot(tip_xy - c, d)
        web2 = c + d * np.dot(web_xy - c, d)
        # The knuckle sits a fixed anatomical distance behind the web (Meshy's fingers can be
        # long and stylised, so a fraction of finger length would misplace it).
        mcp2 = web2 - d * (0.1 * TARGET_LENGTH)
        z_tip = _mid_z(v, tip2 - d * 0.4, 0.4)
        z_mcp = _mid_z(v, mcp2, 0.6)
        tip = np.array([tip2[0], tip2[1], z_tip])
        mcp = np.array([mcp2[0], mcp2[1], z_mcp])
        J[f"{name}_proximal_r"] = mcp
        for frac, joint in ((0.45, "intermediate"), (0.73, "distal")):
            p2 = mcp2 + frac * (tip2 - mcp2)
            J[f"{name}_{joint}_r"] = np.array([p2[0], p2[1], _mid_z(v, p2, 0.45)])
        J[f"{name}_tip_r"] = tip
        J[f"{name}_metacarpal_r"] = np.array([0.15 * mcp[0], 0.55 * mcp[1], 0.5 * mcp[2]])
    thumb_seg, i = [], seq[0]
    while pts[i, 0] > 2.0 or len(thumb_seg) < 20:
        thumb_seg.append(i)
        i = (i - step) % n
    # Thumb: the outline point farthest along ~45 deg toward -Y beyond the thumb valley.
    d0 = np.array([np.cos(np.radians(45)), -np.sin(np.radians(45))])
    cand = pts[thumb_seg]
    tip_xy = cand[np.argmax(np.linalg.norm(cand - palm_c, axis=1))]
    line = _centerline(mask, lo, tip_xy, d0, 4.5)
    c = line.mean(0)
    _, _, vt = np.linalg.svd(line - c)
    d = vt[0] if np.dot(vt[0], d0) > 0 else -vt[0]
    tip2 = c + d * np.dot(tip_xy - c, d)
    tip = np.array([tip2[0], tip2[1], _mid_z(v, tip2 - d * 0.4, 0.4)])
    cmc_line2 = tip2 - d * (0.57 * TARGET_LENGTH)
    cmc2 = 0.5 * cmc_line2 + 0.5 * np.array([0.12, -0.14]) * TARGET_LENGTH
    for frac, joint in ((0.44, "proximal"), (0.74, "distal")):
        p2 = cmc2 + frac * (tip2 - cmc2)
        J[f"thumb_{joint}_r"] = np.array([p2[0], p2[1], _mid_z(v, p2, 0.5)])
    # The CMC sits deep in the thenar base; the wrist cuff would skew a local mid-thickness.
    J["thumb_metacarpal_r"] = np.array([cmc2[0], cmc2[1], 0.5 * J["thumb_proximal_r"][2]])
    J["thumb_tip_r"] = tip
    palm = 0.5 * (J["middle_metacarpal_r"] + J["middle_proximal_r"])
    col = v[np.hypot(v[:, 0] - palm[0], v[:, 1] - palm[1]) < 1.5]
    palm[2] = 0.5 * (col[:, 2].min() + col[:, 2].max())
    J["palm_r"] = palm
    J["grip_r"] = np.array([palm[0], palm[1], col[:, 2].min() - 4.0])
    end = v[v[:, 0] < v[:, 0].min() + 1.0]
    J["forearm_r"] = np.array([v[:, 0].min(), end[:, 1].mean(), end[:, 2].mean()])
    print("valleys (x,y cm):", [np.round(p, 2).tolist() for p in valleys])
    return v, J


def faces_hand(v, faces):
    keep = (v[faces][:, :, 0] > -1.0).all(axis=1)
    sub = faces[keep]
    # Re-index onto the hand-side vertex subset used for the silhouette.
    idx = np.nonzero(v[:, 0] > -1.0)[0]
    remap = -np.ones(len(v), int)
    remap[idx] = np.arange(len(idx))
    return remap[sub]


def debug_image(local, joints, path, faces, width_px=900):
    pts = np.vstack([local] + [np.array(list(joints.values()))]) if joints else local
    lo, hi = pts.min(0) - 1, pts.max(0) + 1
    scale = width_px / (hi[0] - lo[0])
    w = int((hi[0] - lo[0]) * scale)
    img = Image.new("RGB", (w * 2 + 20, int(max(hi[1] - lo[1], hi[2] - lo[2]) * scale) + 10), (40, 40, 40))
    d = ImageDraw.Draw(img)
    for view, (axis, off) in enumerate(((1, 0), (2, w + 20))):
        for f in faces:
            d.polygon([(off + (local[i, 0] - lo[0]) * scale, (hi[axis] - local[i, axis]) * scale) for i in f],
                      fill=(150, 150, 150), outline=(110, 110, 110))
        for name, p in joints.items():
            px, py = off + (p[0] - lo[0]) * scale, (hi[axis] - p[axis]) * scale
            color = (255, 80, 80) if "tip" in name else (80, 220, 255)
            d.ellipse((px - 4, py - 4, px + 4, py + 4), outline=color, width=2)
    img.save(path)


def main():
    cmd = sys.argv[1]
    if cmd == "analyse":
        cls, glb, labels_path, covering, outdir = sys.argv[2:7]
        out = pathlib.Path(outdir); out.mkdir(parents=True, exist_ok=True)
        mesh, local, labels = analyse(cls, glb, labels_path, [int(i) for i in covering.split(",")], out)
        bins, prof, smooth, peaks = tips_and_webs(local)
        print(cls, "extent cm", np.round(np.ptp(local, 0), 2), "hand verts", int((local[:, 0] > 0).sum()))
        print("profile peaks (y, max x):", [(round(float((bins[i] + bins[i + 1]) / 2), 2), round(float(smooth[i]), 2)) for i in peaks])
        np.save(out / "local.npy", local)
        v, J = landmarks(local, mesh["indices"])
        np.save(out / "scaled.npy", v)
        json.dump({k: [round(float(x), 3) for x in p] for k, p in J.items()}, open(out / "joints.json", "w"), indent=1)
        debug_image(v, J, out / "joints.png", mesh["indices"])
        print("wrist->middle tip", round(float(np.linalg.norm(J["middle_tip_r"])), 2), "cm; forearm end x", round(float(J["forearm_r"][0]), 2))
        for k in ("thumb_metacarpal_r", "thumb_proximal_r", "thumb_tip_r", "index_proximal_r", "middle_proximal_r", "little_proximal_r", "palm_r", "grip_r"):
            print(" ", k, np.round(J[k], 2))
        if len(sys.argv) > 7:
            report = export(mesh, v, J, sys.argv[7], out / f"{cls}.glb")
            json.dump(report, open(out / "rig-report.json", "w"), indent=1)
            print("EXPORTED", json.dumps({k: report[k] for k in ("triangles", "bones", "max_influences", "wrist_to_middle_tip_cm")}),
                  "weighted:", len(report["weighted_bones"]))


# ---------------------------------------------------------------- rig + export ----------
BONES = [("forearm_r", None), ("wrist_r", "forearm_r"), ("palm_r", "wrist_r"), ("grip_r", "wrist_r")]
for _chain, _segs in (("thumb", ("metacarpal", "proximal", "distal", "tip")),
                      ("index", ("metacarpal", "proximal", "intermediate", "distal", "tip")),
                      ("middle", ("metacarpal", "proximal", "intermediate", "distal", "tip")),
                      ("ring", ("metacarpal", "proximal", "intermediate", "distal", "tip")),
                      ("little", ("metacarpal", "proximal", "intermediate", "distal", "tip"))):
    _parent = "wrist_r"
    for _seg in _segs:
        BONES.append((f"{_chain}_{_seg}_r", _parent))
        _parent = f"{_chain}_{_seg}_r"
UNWEIGHTED = {"palm_r", "grip_r"} | {n for n, _ in BONES if n.endswith("_tip_r")}


def bone_frames(J):
    """Global rest rotations (columns = local X, Y, Z in UE space) per the contract."""
    R = {}
    children = {}
    for name, parent in BONES:
        children.setdefault(parent, []).append(name)
    for name, parent in BONES:
        if name in ("forearm_r", "wrist_r", "palm_r", "grip_r"):
            R[name] = np.eye(3)
            continue
        if name.endswith("_tip_r"):
            continue
        child = children[name][0]
        x = J[child] - J[name]
        x /= np.linalg.norm(x)
        zref = np.array([0.0, -0.7071, 0.7071]) if name.startswith("thumb") else np.array([0.0, 0.0, 1.0])
        z = zref - np.dot(zref, x) * x
        z /= np.linalg.norm(z)
        y = np.cross(z, x)
        R[name] = np.stack([x, y, z], axis=1)
    for name, parent in BONES:
        if name.endswith("_tip_r"):
            R[name] = R[parent]
    return R


def _smooth(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3 - 2 * t)


def _seg_param(p, a, b):
    ab = b - a
    t = np.clip(((p - a) @ ab) / (ab @ ab), 0.0, 1.0)
    d = np.linalg.norm(p - (a + np.outer(t, ab)), axis=1)
    return t, d


def skin_weights(v, J):
    """Generated weights per contract section 5; returns dict bone -> (N,) array."""
    n = len(v)
    W = {name: np.zeros(n) for name, _ in BONES}
    chains = {"thumb": ["thumb_metacarpal_r", "thumb_proximal_r", "thumb_distal_r", "thumb_tip_r"]}
    for f in FINGERS:
        chains[f] = [f"{f}_proximal_r", f"{f}_intermediate_r", f"{f}_distal_r", f"{f}_tip_r"]
    # Distance from each vertex to each finger's free chain (knuckle -> tip; thumb CMC -> tip).
    chain_d = {}
    for f, names in chains.items():
        best = np.full(n, np.inf)
        for a, b in zip(names[:-1], names[1:]):
            _, d = _seg_param(v, J[a], J[b])
            best = np.minimum(best, d)
        chain_d[f] = best
    # Forearm -> wrist blend over the last few cm before the wrist.
    arm = 1.0 - _smooth((v[:, 0] + 3.0) / 3.5)
    W["forearm_r"] = arm
    hand = 1.0 - arm
    # Palm share: wrist plus the two nearest metacarpals (by distance to base->knuckle lines).
    meta_d = {}
    for f in FINGERS:
        _, d = _seg_param(v, J[f"{f}_metacarpal_r"], J[f"{f}_proximal_r"])
        meta_d[f] = d
    knuckle_x = np.mean([J[f"{f}_proximal_r"][0] for f in FINGERS])
    u = np.clip(v[:, 0] / knuckle_x, 0.0, 1.0)
    wrist_share = 1.0 - 0.75 * u
    md = np.stack([meta_d[f] for f in FINGERS], 1)
    inv = 1.0 / (md + 0.3) ** 2
    order = np.argsort(md, axis=1)
    keep = np.zeros_like(inv)
    rows = np.arange(n)
    keep[rows, order[:, 0]] = inv[rows, order[:, 0]]
    keep[rows, order[:, 1]] = inv[rows, order[:, 1]]
    keep /= keep.sum(1, keepdims=True)
    palm_w = {"wrist_r": wrist_share}
    for k, f in enumerate(FINGERS):
        palm_w[f"{f}_metacarpal_r"] = (1.0 - wrist_share) * keep[:, k]
    # Finger ownership: nearest free chain, faded in across the knuckle so the palm blends.
    owner = np.stack([chain_d[f] for f in chains], 1)
    names = list(chains)
    nearest = owner.argmin(1)
    finger_w = np.zeros(n)
    per_bone = {}
    for k, f in enumerate(names):
        mine = nearest == k
        seq = chains[f]
        if f == "thumb":
            base, start = J["thumb_metacarpal_r"], J["thumb_metacarpal_r"]
            fade_from = J["thumb_metacarpal_r"]
            reach = 2.2
        else:
            base, start = J[f"{f}_metacarpal_r"], J[f"{f}_proximal_r"]
            fade_from = J[f"{f}_proximal_r"]
            reach = 1.4
        axis = J[seq[1]] - fade_from
        axis /= np.linalg.norm(axis)
        along = (v - fade_from) @ axis
        # 0 well behind the knuckle (palm), 1 past it (finger); thumb base counts as thumb.
        fade = _smooth((along + (2.0 if f == "thumb" else 0.8)) / 1.6)
        near = _smooth((reach + 0.8 - owner[:, k]) / 0.8)
        w_f = np.where(mine, fade * near, 0.0)
        finger_w = np.maximum(finger_w, w_f)
        # Split this finger's share across its segments, blending over +-0.5 cm at joints.
        joints = seq
        params = [(J[a], J[b]) for a, b in zip(joints[:-1], joints[1:])]
        cum = [0.0]
        for a, b in params:
            cum.append(cum[-1] + np.linalg.norm(b - a))
        best_t = np.zeros(n)
        best_d = np.full(n, np.inf)
        for i, (a, b) in enumerate(params):
            t, d = _seg_param(v, a, b)
            s_along = cum[i] + t * np.linalg.norm(b - a)
            better = d < best_d
            best_d = np.where(better, d, best_d)
            best_t = np.where(better, s_along, best_t)
        bones = joints[:-1]
        seg_w = np.zeros((n, len(bones)))
        for i in range(len(bones)):
            lo = _smooth((best_t - (cum[i] - 0.5)) / 1.0) if i > 0 else np.ones(n)
            hi = 1.0 - _smooth((best_t - (cum[i + 1] - 0.5)) / 1.0) if i < len(bones) - 1 else np.ones(n)
            seg_w[:, i] = lo * hi
        seg_w /= np.maximum(seg_w.sum(1, keepdims=True), 1e-6)
        for i, b in enumerate(bones):
            per_bone[b] = per_bone.get(b, 0) + w_f * seg_w[:, i]
    # Compose: forearm | hand = finger share + palm share.
    for b, w in per_bone.items():
        W[b] += hand * w
    rest = hand * (1.0 - finger_w)
    for b, w in palm_w.items():
        W[b] += rest * w
    # Ring and little metacarpals keep some weight for cupping; nothing on unweighted bones.
    for b in UNWEIGHTED:
        W[b][:] = 0.0
    names_all = [nm for nm, _ in BONES]
    M = np.stack([W[nm] for nm in names_all], 1)
    top = np.argsort(-M, axis=1)[:, :4]
    vals = np.take_along_axis(M, top, 1)
    vals /= np.maximum(vals.sum(1, keepdims=True), 1e-9)
    return names_all, top.astype(np.uint16), vals.astype(np.float32)


def smooth_normals(v, faces):
    key = np.round(v, 4)
    _, inverse = np.unique(key, axis=0, return_inverse=True)
    inverse = inverse.ravel()
    fn = np.cross(v[faces[:, 1]] - v[faces[:, 0]], v[faces[:, 2]] - v[faces[:, 0]])
    acc = np.zeros((inverse.max() + 1, 3))
    for c in range(3):
        np.add.at(acc, inverse[faces[:, c]], fn)
    nrm = acc[inverse]
    return nrm / np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)


def export(mesh, v, J, png_path, out_glb):
    import sys as _sys
    _sys.path.insert(0, str(pathlib.Path(__file__).parent))
    from gltf_skin import write_glb
    R = bone_frames(J)
    names, joints, weights = skin_weights(v, J)
    faces = mesh["indices"]
    pos_g = (P @ v.T).T / 100.0
    # Normals come from glTF-space geometry: UE-space coordinates are mirrored (P is a
    # reflection), so face cross products there would point inward.
    nrm_g = smooth_normals(pos_g, faces)
    # Winding: UE-space vertices came from glTF by a proper similarity, so faces keep their order.
    globals_g = []
    for nm in names:
        m = np.eye(4)
        m[:3, :3] = P @ R[nm] @ P
        m[:3, 3] = P @ J[nm] / 100.0
        globals_g.append(m)
    parents = [names.index(p) if p else -1 for _, p in BONES]
    write_glb(out_glb, positions=pos_g, normals=nrm_g, uvs=mesh["uvs"].astype(np.float32),
              indices=faces.ravel(), joint_names=names, joint_parents=parents, joint_globals=globals_g,
              joints=joints, weights=weights, png_bytes=pathlib.Path(png_path).read_bytes())
    used = sorted({names[j] for j, w in zip(joints.ravel(), weights.ravel()) if w > 1e-4})
    return {"triangles": int(len(faces)), "vertices": int(len(v)), "bones": len(names),
            "weighted_bones": used, "max_influences": int((weights > 1e-4).sum(1).max()),
            "wrist_to_middle_tip_cm": round(float(np.linalg.norm(J["middle_tip_r"] - J["wrist_r"])), 2)}


if __name__ == "__main__":
    main()
