"""Metric v3 re-baseline: write exports/reference/compare/baseline_stats.csv
+ SUMMARY.txt from the gamma render set.

Metric v3 = the PAIR of per-shot medians:
  gamG  = median(UE/golden) over pixels where GOLDEN luma > 8  (anchored on
          original content — the honest fidelity measure)
  gamUE = 1/median(golden/UE) over pixels where UE luma > 8    (legacy v2
          gate; >1 means we are bright where golden is dark)
UE frames are 565-floor quantized first (goldens carry that floor).
Single-gate medians are NOT trustworthy alone: the v2 UE-gate hid a ~30%
deficit in the linear scheme by ignoring pixels our render left dark.

Usage: python rebaseline.py <render_root> [out_dir]
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from wot_gamma_paths import EXPORTS, GOLD, SCRATCH

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else SCRATCH / "ue_gamma_v1"
OUT = Path(sys.argv[2] if len(sys.argv) > 2
           else EXPORTS / "reference" / "compare")


def luma(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def q565(a):
    a = a.astype(np.int32)
    r = (a[..., 0] >> 3); r = (r << 3) | (r >> 2)
    g = (a[..., 1] >> 2); g = (g << 2) | (g >> 4)
    b = (a[..., 2] >> 3); b = (b << 3) | (b >> 2)
    return np.stack([r, g, b], -1).astype(np.float64)


rows = []
for mdir in sorted(d for d in ROOT.iterdir() if d.is_dir()):
    for f in sorted(mdir.glob("*.png")):
        ue = np.asarray(Image.open(f).convert("RGB"))
        gold = np.asarray(Image.open(GOLD / mdir.name / f.name).convert("RGB"))
        ug, gg = luma(q565(ue)), luma(gold.astype(np.float64))
        sg = gg > 8
        su = ug > 8
        gamg = float(np.median(ug[sg] / np.maximum(gg[sg], 1e-6))) if sg.any() else 0.0
        if su.any():
            m = float(np.median(gg[su] / np.maximum(ug[su], 1e-6)))
            gamu = 1.0 / m if m > 1e-6 else 0.0
        else:
            gamu = 0.0
        rows.append((mdir.name, f.name, gamg, gamu))

OUT.mkdir(parents=True, exist_ok=True)
with open(OUT / "baseline_stats.csv", "w") as fh:
    fh.write("map,shot,goldgate_median,uegate_median\n")
    for m, s, a, b in rows:
        fh.write(f"{m},{s},{a:.4f},{b:.4f}\n")

lines = ["metric v3 baseline (gamma-space scheme)",
         f"render set: {ROOT}", "",
         f"{'map':12s} {'shots':>5s} {'goldG.med':>9s} {'goldG.p10':>9s} "
         f"{'goldG.p90':>9s} {'ueG.med':>8s}"]
allg = []
for m in sorted({r[0] for r in rows}):
    g = np.array([r[2] for r in rows if r[0] == m])
    u = np.array([r[3] for r in rows if r[0] == m])
    allg += list(g)
    lines.append(f"{m:12s} {len(g):5d} {np.median(g):9.3f} "
                 f"{np.percentile(g, 10):9.3f} {np.percentile(g, 90):9.3f} "
                 f"{np.median(u):8.3f}")
allg = np.array(allg)
lines += ["", f"GLOBAL golden-gated: median {np.median(allg):.3f}, "
          f"p10-p90 {np.percentile(allg, 10):.3f}-{np.percentile(allg, 90):.3f}, "
          f"shots {len(allg)}"]
(OUT / "SUMMARY.txt").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
