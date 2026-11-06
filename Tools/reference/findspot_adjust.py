"""Bake the golden-capture FindSpot camera-height correction into the manifests.

The capture rig (capture_original_shots.ps1) recorded the raw `JumpTo` target as
`pawn`, but retail UE1 moved the pawn before each screenshot:
    JumpTo x y z -> SetLocation -> ULevel::FarMoveActor(test=0, bNoCheck=0)
                 -> FindSpot(cylinder 17x17x46, loc, bCheckActors=0, bAssumeFit=0)
The capture pawn is in state CheatFlying (rig sends `ghost` then `fly`; Fly()
sets SetCollision(true..)+bCollideWorld=true in UE1's PlayerPawn), so FindSpot
shoves the cylinder off the floor/walls. Our render used the un-nudged target,
leaving the camera low per-pose. This writes the FindSpot spot into `pawn`.

Model (validated 2026-07-07 on B2_02/B2_04/Arena_04, PORT_NOTES
"Golden-capture height residual"):
    pawn_adjusted.z = pawn_jumpto.z + (FindSpot_dz - GLOBAL_MEAN_DZ)
    (x,y likewise get the FindSpot dx,dy; those are ~0 off open floor.)
GLOBAL_MEAN_DZ de-degenerates the nudge from eye_height: the original parallax
fit that produced eye_height=29.4 had silently folded the AVERAGE nudge into it,
so applying the full nudge double-counts. Subtracting the global mean applies
only the physical per-pose DEVIATION; eye_height stays 29.4 (a single global
constant handles the degeneracy; the mechanism explains all per-map variation).

Reimplements the original engine's FindSpot/AdjustSpot/FarMoveActor.
Geometry is the ORIGINAL retail BSP (original/Maps/<map>.wot),
the exact model retail traced during capture. bCheckActors=0 so actors are not
consulted (matches FarMoveActor's FindSpot call), level BSP only.

Idempotent: preserves the raw target as `pawn_jumpto` and re-reads it on reruns.

Usage:
    python Tools/reference/findspot_adjust.py            # adjust all 19 maps
    python Tools/reference/findspot_adjust.py --dry-run  # report, write nothing
    python Tools/reference/findspot_adjust.py Battle2_02 # a subset
"""
import json
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools" / "test" / "gamma"))
sys.path.insert(0, str(REPO / "Tools" / "convert"))
sys.path.insert(0, str(REPO / "Tools"))
from mask_pickups import _level_tris  # noqa: E402  (retail-BSP triangle reader)
from wot_config import exports_dir  # noqa: E402

SHOTS = exports_dir() / "reference" / "shots"
EXT = np.array([17.0, 17.0, 46.0])   # WOTPlayer CDO CollisionRadius/Height
MAPS = ["Arena_01", "Arena_02", "Arena_03", "Arena_04", "Arena_05", "Arena_06",
        "Arena_07", "Arena_08", "Arena_09", "Arena_11", "Arena_12",
        "Battle2_01", "Battle2_02", "Battle2_03", "Battle2_04", "Battle2_05",
        "Battle3_01", "Battle3_02", "Battle4_01"]


class Geo:
    def __init__(self, mapname):
        v0, e1, e2 = _level_tris(mapname)
        self.v0, self.e1, self.e2 = v0, e1, e2
        n = np.cross(e1, e2)
        ln = np.linalg.norm(n, axis=1, keepdims=True)
        ln[ln == 0] = 1.0
        self.n = n / ln
        self.lo = np.minimum(np.minimum(v0, v0 + e1), v0 + e2)
        self.hi = np.maximum(np.maximum(v0, v0 + e1), v0 + e2)


def trace(geo, start, delta):
    """Nearest hit along the segment; UE1 zero-extent SingleLineCheck.
    Returns (pulled-back time fraction, normal) or None."""
    L = float(np.linalg.norm(delta))
    if L < 1e-6:
        return None
    d = delta / L
    p = np.cross(d, geo.e2)
    det = np.einsum("ij,ij->i", geo.e1, p)
    ok = np.abs(det) > 1e-9
    inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
    t = start - geo.v0
    u = np.einsum("ij,ij->i", t, p) * inv
    q = np.cross(t, geo.e1)
    v = np.einsum("j,ij->i", d, q) * inv
    dist = np.einsum("ij,ij->i", geo.e2, q) * inv
    hit = ok & (u >= -1e-6) & (u <= 1 + 1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6) \
        & (dist > 1e-4) & (dist <= L + 1e-6)
    if not hit.any():
        return None
    idx = np.where(hit)[0]
    i = idx[np.argmin(dist[idx])]
    tf = float(dist[i]) / L
    tf = max(0.0, tf - 0.5 / L)          # UE1 zero-extent BSP pullback
    n = geo.n[i]
    if np.dot(n, d) > 0:
        n = -n
    return tf, n


def adjust_spot(geo, adj, dest, tracelen):
    """ULevel::AdjustSpot: SingleLineCheck adj->dest, push out by
    Normal*(1.05-Time)*TraceLen on a hit."""
    r = trace(geo, adj, dest - adj)
    if r is not None:
        tf, n = r
        if tf < 1.0:
            adj = adj + n * (1.05 - tf) * tracelen
    return adj


def point_check(geo, center, half):
    """ULevel::SinglePointCheck: True if any tri overlaps the AABB (SAT)."""
    m = np.all(geo.hi >= center - half, 1) & np.all(geo.lo <= center + half, 1)
    idx = np.where(m)[0]
    if idx.size == 0:
        return False
    for i in idx:
        a = geo.v0[i] - center
        b = a + geo.e1[i]
        c = a + geo.e2[i]
        tri = np.array([a, b, c])
        n = geo.n[i]
        r = np.dot(half, np.abs(n))
        if abs(np.dot(n, a)) > r:
            continue
        sep = False
        for e in (b - a, c - b, a - c):
            for ax in range(3):
                axis = np.zeros(3)
                axis[ax] = 1.0
                cr = np.cross(axis, e)
                if np.linalg.norm(cr) < 1e-9:
                    continue
                pr = tri @ cr
                rr = np.dot(half, np.abs(cr))
                if pr.min() > rr or pr.max() < -rr:
                    sep = True
                    break
            if sep:
                break
        if not sep:
            return True
    return False


def find_spot(geo, loc):
    """ULevel::FindSpot(Extent, loc, bCheckActors=0, bAssumeFit=0). Returns
    (adjusted_location, status). bAssumeFit=0 -> always run the axis passes."""
    adj = loc.copy()
    for i in (-1, 1):
        adj = adjust_spot(geo, adj, adj + np.array([i * EXT[0], 0, 0]), EXT[0])
        adj = adjust_spot(geo, adj, adj + np.array([0, i * EXT[1], 0]), EXT[1])
        adj = adjust_spot(geo, adj, adj + np.array([0, 0, i * EXT[2]]), EXT[2])
    if not point_check(geo, adj, EXT):
        return adj, "ok"
    tl = float(np.linalg.norm(EXT)) + 2.0
    for i in (-1, 1):
        for j in (-1, 1):
            for k in (-1, 1):
                adj = adjust_spot(geo, adj,
                                  adj + np.array([i * EXT[0], j * EXT[1], k * EXT[2]]), tl)
    if np.sum((adj - loc) ** 2) > 1.5 * float(np.sum(EXT ** 2)):
        return loc, "FAIL_far"          # retail SetLocation() would return false
    if point_check(geo, adj, EXT):
        return loc, "FAIL_fit"
    return adj, "ok_corner"


def raw_pawn(shot):
    """The un-nudged JumpTo target: pawn_jumpto if already adjusted, else pawn."""
    src = shot.get("pawn_jumpto", shot["pawn"])
    return np.array([src["x"], src["y"], src["z"]], float)


def compute_nudges(maps):
    """{(map, source): (dvec, status)} over unique sources. One Geo load/map."""
    out = {}
    for m in maps:
        man = json.loads((SHOTS / m / "manifest.json").read_text())
        geo = Geo(m)
        seen = {}
        for s in man["shots"]:
            src = s["source"]
            if src in seen:
                continue
            loc = raw_pawn(s)
            adj, status = find_spot(geo, loc)
            seen[src] = (adj - loc, status)
            out[(m, src)] = seen[src]
        print(f"  {m}: {len(seen)} sources", flush=True)
    return out


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    maps = argv or MAPS

    print("computing FindSpot nudges (retail BSP)...", flush=True)
    nudges = compute_nudges(maps)

    good = [d[2] for (d, st) in nudges.values() if st in ("ok", "ok_corner")]
    global_mean = float(np.mean(good))
    print(f"\nGLOBAL_MEAN_DZ = {global_mean:.4f}  (mean over {len(good)} "
          f"ok sources; FAIL count={sum(1 for _, st in nudges.values() if st.startswith('FAIL'))})")

    for m in maps:
        man = json.loads((SHOTS / m / "manifest.json").read_text())
        moved = 0
        for s in man["shots"]:
            dvec, status = nudges[(m, s["source"])]
            jt = raw_pawn(s)
            if status.startswith("FAIL"):
                applied = np.zeros(3)            # no confident model; keep raw
            else:
                applied = dvec - np.array([0, 0, global_mean])
            s.setdefault("pawn_jumpto", {"x": jt[0], "y": jt[1], "z": jt[2]})
            new = jt + applied
            s["pawn"] = {"x": round(float(new[0]), 3),
                         "y": round(float(new[1]), 3),
                         "z": round(float(new[2]), 3)}
            s["findspot"] = {"dx": round(float(dvec[0]), 3),
                             "dy": round(float(dvec[1]), 3),
                             "dz": round(float(dvec[2]), 3),
                             "applied_dz": round(float(applied[2]), 3),
                             "status": status}
            if np.any(np.abs(applied) > 0.05):
                moved += 1
        man["eye_height_note"] = (
            f"pawn = FindSpot-adjusted (JumpTo + FindSpot_dz - {global_mean:.3f} "
            f"global-mean); eye_height unchanged. Tools/reference/findspot_adjust.py")
        tag = "(dry-run) " if dry else ""
        print(f"  {tag}{m}: {moved}/{len(man['shots'])} shots adjusted")
        if not dry:
            (SHOTS / m / "manifest.json").write_text(json.dumps(man, indent=1))


if __name__ == "__main__":
    main()
