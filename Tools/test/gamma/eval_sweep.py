"""Metric-v2 stats for the 19-map gamma sweep vs golden, with the linear
baseline (ue_v29 under scratch_root) alongside. PIL-verifies every render
(a full disk truncates renders). Usage: python eval_sweep.py [map ...]"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from wot_gamma_paths import GOLD, SCRATCH

GAMMA = SCRATCH / "ue_gamma_v1"
LINEAR = SCRATCH / "ue_v29"


def q565(a):
    a = a.astype(np.int32)
    r = (a[..., 0] >> 3); r = (r << 3) | (r >> 2)
    g = (a[..., 1] >> 2); g = (g << 2) | (g >> 4)
    b = (a[..., 2] >> 3); b = (b << 3) | (b >> 2)
    return np.stack([r, g, b], -1).astype(np.float64)


def luma(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def med(ue, gold, gate="ue"):
    ug, gg = luma(q565(ue)), luma(gold.astype(np.float64))
    sel = (ug > 8) if gate == "ue" else (gg > 8)
    if not sel.any():
        return 0.0
    if gate == "ue":
        m = float(np.median(gg[sel] / np.maximum(ug[sel], 1e-6)))
        return 1.0 / m if m > 1e-6 else 0.0
    return float(np.median(ug[sel] / np.maximum(gg[sel], 1e-6)))


maps = sys.argv[1:] or sorted(d.name for d in GAMMA.iterdir() if d.is_dir())
print(f"{'map':12s} {'shots':>5s} {'linG':>7s} {'gamG':>7s} "
      f"{'gamG p10':>8s} {'gamG p90':>8s} {'gamUE':>7s} corrupt")
allg = []
for m in maps:
    gg_, lg_, gu_, bad = [], [], [], 0
    for f in sorted((GAMMA / m).glob("*.png")):
        try:
            ue = np.asarray(Image.open(f).convert("RGB"))
        except Exception:
            bad += 1
            continue
        gold = np.asarray(Image.open(GOLD / m / f.name).convert("RGB"))
        gg_.append(med(ue, gold, "gold"))
        gu_.append(med(ue, gold, "ue"))
        lf = LINEAR / m / f.name
        if lf.exists():
            try:
                lg_.append(med(np.asarray(Image.open(lf).convert("RGB")),
                               gold, "gold"))
            except Exception:
                pass
    if not gg_:
        print(f"{m:12s} {'0':>5s}  (no renders)")
        continue
    allg += gg_
    print(f"{m:12s} {len(gg_):5d} {np.median(lg_) if lg_ else 0:7.3f} "
          f"{np.median(gg_):7.3f} {np.percentile(gg_, 10):8.3f} "
          f"{np.percentile(gg_, 90):8.3f} {np.median(gu_):7.3f} {bad}")
if allg:
    print(f"\nGLOBAL gamma (golden-gated): median {np.median(allg):.3f}, "
          f"p10-p90 {np.percentile(allg, 10):.3f}-{np.percentile(allg, 90):.3f}, "
          f"shots {len(allg)}")
