"""Per-pose parity scoreboard vs golden — the measured baseline for the
lighting audit (2026-07-06). Median-ratio alone can read 1.00 while whole
regions are broken, so each pose gets localizing metrics as well, all under
the occlusion-culled pickup masks (mask_pickups):

  medR   median luma ratio ours/golden (luma>8 gate, 565-quantized ours) —
         continuity with the historical 0.95-0.97 numbers
  mae    mean |luma delta| over comparable pixels (both > the 565 noise floor)
  bad%   fraction of comparable pixels with |luma delta| > 16 — localized
         breakage a median never sees
  n      comparable-pixel fraction of the frame

Usage: python scoreboard.py <renders_root> [map ...] [--align]
(CSV to stdout; summary + worst poses to stderr so the CSV stays
machine-readable.)

--align: per-pose integer registration search (+/-2px, min-MAE over the
comparable gate) applied to OUR render before diffing, reported as dx,dy
CSV columns. The +/-1uu eye-height fit noise renders as 1-2px edge
doubling on high-frequency content (Battle2_02 maze); the render is
right, so the alignment belongs in the metric. Never compare an aligned
number against an unaligned baseline.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mask_pickups import masks_for_map  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from wot_config import exports_dir  # noqa: E402

GOLD = exports_dir() / "reference" / "shots"


def q565(a):
    a = a.astype(np.int32)
    r = (a[..., 0] >> 3); r = (r << 3) | (r >> 2)
    g = (a[..., 1] >> 2); g = (g << 2) | (g >> 4)
    b = (a[..., 2] >> 3); b = (b << 3) | (b >> 2)
    return np.stack([r, g, b], -1).astype(np.float64)


def luma(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def shift2d(a, dy, dx):
    """Translate without wrap; vacated border = 0 (falls below the >8 gate)."""
    out = np.zeros_like(a)
    h, w = a.shape
    ys, yd = (dy, 0) if dy >= 0 else (0, -dy)
    xs, xd = (dx, 0) if dx >= 0 else (0, -dx)
    out[ys:h - yd, xs:w - xd] = a[yd:h - ys, xd:w - xs]
    return out


def pose_stats(ours, gold, mask, align=0):
    """mask: True = EXCLUDE (pickup). align: registration search radius, px."""
    uo0, go = luma(q565(ours)), luma(gold.astype(np.float64))
    # A golden-dark pose (a handful of _rev captures where retail rendered
    # black) carries no signal — skip it rather than score 100% bad.
    if float((go > 8).mean()) < 0.005:
        return None
    ok = ~mask
    best, uo, sdy, sdx = None, uo0, 0, 0
    for dy in range(-align, align + 1):
        for dx in range(-align, align + 1):
            u = shift2d(uo0, dy, dx) if (dy or dx) else uo0
            g = ok & (go > 8) & (u > 8)
            m = float(np.abs(u[g] - go[g]).mean()) if g.any() else 255.0
            if best is None or m < best:
                best, uo, sdy, sdx = m, u, dy, dx
    gate = ok & (go > 8) & (uo > 8)         # both above the 565 noise floor
    n = float(gate.mean())
    if not gate.any():
        return dict(medR=0.0, mae=255.0, bad=100.0, n=0.0, dy=sdy, dx=sdx)
    ratio = float(np.median(uo[gate] / np.maximum(go[gate], 1e-6)))
    d = np.abs(uo[gate] - go[gate])
    return dict(medR=ratio, mae=float(d.mean()),
                bad=float((d > 16).mean() * 100), n=n, dy=sdy, dx=sdx)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--align")]
    align = 0
    for a in sys.argv[1:]:
        if a.startswith("--align"):
            align = int(a.partition("=")[2]) if "=" in a else 2
    root = Path(args[0])
    maps = args[1:] or sorted(d.name for d in root.iterdir() if d.is_dir())
    rows = []
    print("map,pose,medR,mae,bad_pct,cmp_frac" + (",dx,dy" if align else ""))
    for m in maps:
        try:
            masks = masks_for_map(m)
        except Exception as e:
            print(f"{m}: mask fail {e}", file=sys.stderr)
            masks = {}
        for f in sorted((root / m).glob("*.png")):
            gf = GOLD / m / f.name
            if not gf.exists():
                continue
            try:
                ours = np.asarray(Image.open(f).convert("RGB"))
                gold = np.asarray(Image.open(gf).convert("RGB"))
            except Exception:
                print(f"{m}/{f.name}: unreadable", file=sys.stderr)
                continue
            mask = masks.get(f.name, (np.zeros(gold.shape[:2], bool), None))[0]
            s = pose_stats(ours, gold, mask, align=align)
            if s is None:
                print(f"{m}/{f.name}: golden-dark, skipped", file=sys.stderr)
                continue
            rows.append((m, f.name, s))
            print(f"{m},{f.name},{s['medR']:.4f},{s['mae']:.2f},"
                  f"{s['bad']:.2f},{s['n']:.3f}"
                  + (f",{s['dx']},{s['dy']}" if align else ""))
    if not rows:
        return
    med = np.median([s["medR"] for _, _, s in rows])
    mae = np.mean([s["mae"] for _, _, s in rows])
    bad = np.mean([s["bad"] for _, _, s in rows])
    print(f"\nALL: poses={len(rows)} medR(med)={med:.4f} "
          f"mae(mean)={mae:.2f} bad%(mean)={bad:.2f}", file=sys.stderr)
    worst = sorted(rows, key=lambda r: -r[2]["bad"])[:20]
    print("worst poses by bad%:", file=sys.stderr)
    for m, f, s in worst:
        print(f"  {m}/{f}: bad={s['bad']:.1f}% mae={s['mae']:.1f} "
              f"medR={s['medR']:.3f}", file=sys.stderr)


if __name__ == "__main__":
    main()
