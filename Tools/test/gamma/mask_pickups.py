"""Per-shot exclusion mask for pickup/angreal/spawn actors, so random item
spawns (AngrealSpawnPool uses Rand()) and our mis-rendered static pickups
don't pollute pixel-parity. The mask excludes the SAME pixels from both
golden and our render (unbiased). Impact on the per-shot MEDIAN metric is
small (<=0.006/map, measured) -- the median already absorbs a few % of item
pixels -- so this is mainly for clean visual diffs and explicit exclusion.

Camera model = Source/WoT/WoTShotSubsystem.cpp: camera Loc = pawn+(0,0,42),
FRotator(pitch,yaw,roll * 360/65536) from the manifest (UE1 rotator units),
FOV 90 TRUE horizontal, constrained 4:3 @1600x1200 -> fx=fy=800, center
(800,600). T3D/UE1 world coords land 1:1 in UE5 (PORT_NOTES).

Usage: masks_for_map("Arena_06") -> {shot_filename: (bool_mask HxW, hits)}.
CLI:   python mask_pickups.py Arena_06   # prints per-shot coverage.
"""
import json
import re
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "Tools" / "convert"))
sys.path.insert(0, str(REPO / "Tools"))
import t3d as T  # noqa: E402
from wot_config import exports_dir, original_dir  # noqa: E402

EXPORTS = exports_dir()
ORIGINAL = original_dir()

W, H = 1600, 1200
CX, CY = W / 2, H / 2
FX = FY = (W / 2) / np.tan(np.radians(90) / 2)   # 800 (HFOV 90)
# Eye height comes from the manifest (corrected 42 -> 29.4, 2026-07-06: the
# ghost/fly capture pawn used the jump/fall EyeHeight = 0.7*42, WOTPlayer.uc:2261).
U2D = 360.0 / 65536.0
# actor classes whose location can show a pickup item in EITHER capture:
# static placements we render (AndilayRoot, AngrealInv*) AND the random pools
# golden spawns from (Common/RareAngrealSpawnPool, InventorySpot).
PICKUP_RE = re.compile(r"angreal|andilay|inventoryspot|spawnpool|pickup", re.I)
WORLD_R = 48.0        # world radius incl. mesh-above-origin (uu)
PIX_PAD = 8
MAX_R = 150.0         # cap so a near pickup can't swallow the frame


def basis(pitch, yaw, roll):
    """UE FRotationMatrix axes (deg in)."""
    p, y, r = np.radians([pitch, yaw, roll])
    sp, cp = np.sin(p), np.cos(p); sy, cy = np.sin(y), np.cos(y)
    sr, cr = np.sin(r), np.cos(r)
    F = np.array([cp * cy, cp * sy, sp])
    R = np.array([sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp])
    U = np.array([-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp])
    return F, R, U


def _pickup_positions(mapname):
    t3d = EXPORTS / "t3d" / f"{mapname}.t3d"
    actors = T.typed(T.parse(str(t3d)))
    return [(a["cls"], np.array(a["location"], float)) for a in actors
            if PICKUP_RE.search(a["cls"])]


def _level_tris(mapname):
    """Level-model triangles for occlusion rays (camera -> pickup). Verified
    2026-07-06: without this, an occluded pickup masks innocent wall pixels —
    Arena_07 PS4's circles sat exactly on a REAL golden-vs-ours lighting delta
    (a lit band we render dark) and hid part of it from the metric."""
    from ue1pkg import Package
    from lightmaps import decode_model_full
    pkg = Package(str(ORIGINAL / "Maps" / f"{mapname}.wot"))
    lvl = max((e for e in pkg.exports if e["name"].startswith("Model")),
              key=lambda e: e["size"])
    bsp = decode_model_full(pkg, lvl)
    tris = []
    for nd in bsp["nodes"]:
        ivp, isurf, nv = nd[0], nd[1], nd[2]
        if nv < 3:
            continue
        pts = [np.array(bsp["points"][bsp["verts"][ivp + i][0]])
               for i in range(nv)]
        for i in range(1, nv - 1):
            tris.append((pts[0], pts[i], pts[i + 1]))
    v0 = np.array([t[0] for t in tris])
    e1 = np.array([t[1] for t in tris]) - v0
    e2 = np.array([t[2] for t in tris]) - v0
    return v0, e1, e2


def _occluded(cam, target, geo, slack=32.0):
    """True if the level BSP blocks cam->target before the pickup (vectorized
    Moller-Trumbore; `slack` uu spares the pickup's own extent)."""
    v0, e1, e2 = geo
    d = target - cam
    seg = np.linalg.norm(d)
    if seg < 1e-3:
        return False
    d = d / seg
    p = np.cross(d, e2)
    det = np.einsum("ij,ij->i", e1, p)
    ok = np.abs(det) > 1e-9
    inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
    t = cam - v0
    u = np.einsum("ij,ij->i", t, p) * inv
    q = np.cross(t, e1)
    v = np.einsum("j,ij->i", d, q) * inv
    dist = np.einsum("ij,ij->i", e2, q) * inv
    hit = ok & (u >= 0) & (u <= 1) & (v >= 0) & (u + v <= 1) \
        & (dist > 1.0) & (dist < seg - slack)
    return bool(hit.any())


def masks_for_map(mapname):
    man = json.loads((EXPORTS / "reference/shots" / mapname
                      / "manifest.json").read_text())
    eye = float(man.get("eye_height", 29.4))
    pk = _pickup_positions(mapname)
    pts = np.array([p for _, p in pk]) if pk else np.zeros((0, 3))
    geo = _level_tris(mapname) if len(pts) else None
    yy, xx = np.ogrid[:H, :W]
    out = {}
    for shot in man["shots"]:
        cam = np.array([shot["pawn"]["x"], shot["pawn"]["y"],
                        shot["pawn"]["z"]], float) + np.array([0, 0, eye])
        F, R, U = basis(shot["rot"]["pitch"] * U2D, shot["rot"]["yaw"] * U2D,
                        shot["rot"]["roll"] * U2D)
        mask = np.zeros((H, W), bool)
        hits = []
        if len(pts):
            rel = pts - cam
            xc = rel @ F; sx = CX + FX * (rel @ R) / xc; sy = CY - FY * (rel @ U) / xc
            for i in range(len(pts)):
                if xc[i] <= 1:
                    continue
                rad = min(FX * WORLD_R / xc[i] + PIX_PAD, MAX_R)
                if sx[i] < -rad or sx[i] > W + rad or sy[i] < -rad or sy[i] > H + rad:
                    continue
                # A pickup hidden behind level geometry shows in NEITHER
                # capture — masking it would only hide real deltas.
                if _occluded(cam, pts[i], geo):
                    continue
                mask |= (xx - sx[i]) ** 2 + (yy - sy[i]) ** 2 <= rad ** 2
                hits.append((pk[i][0], int(sx[i]), int(sy[i]), int(xc[i]), int(rad)))
        out[shot["file"]] = (mask, hits)
    return out


if __name__ == "__main__":
    for m in (sys.argv[1:] or ["Arena_06"]):
        res = masks_for_map(m)
        cov = [100 * mask.mean() for mask, _ in res.values()]
        print(f"{m}: {len(res)} shots, mask mean {np.mean(cov):.1f}% "
              f"max {np.max(cov):.1f}% "
              f"({sum(len(h) for _, h in res.values())} pickup projections)")
