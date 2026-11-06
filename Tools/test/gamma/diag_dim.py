"""Per-shot golden-anchored scores for the under-golden maps + crops of
the worst shots (golden | gamma side-by-side with a dim-region marker)."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from wot_gamma_paths import GOLD, SCRATCH

GAMMA = SCRATCH / "ue_gamma_v1"
OUT = SCRATCH / "gamma_exp"


def luma(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def q565(a):
    a = a.astype(np.int32)
    r = (a[..., 0] >> 3); r = (r << 3) | (r >> 2)
    g = (a[..., 1] >> 2); g = (g << 2) | (g >> 4)
    b = (a[..., 2] >> 3); b = (b << 3) | (b >> 2)
    return np.stack([r, g, b], -1).astype(np.float64)


for m in (sys.argv[1:] or ["Arena_06", "Arena_07", "Arena_04"]):
    rows = []
    for f in sorted((GAMMA / m).glob("*.png")):
        ue = np.asarray(Image.open(f).convert("RGB"))
        gold = np.asarray(Image.open(GOLD / m / f.name).convert("RGB"))
        ug, gg = luma(q565(ue)), luma(gold.astype(np.float64))
        sel = gg > 8
        r = float(np.median(ug[sel] / np.maximum(gg[sel], 1e-6)))
        rows.append((r, f.name))
    rows.sort()
    print(m)
    for r, n in rows[:4]:
        print(f"  {r:.3f} {n}")
    # crop the worst shot's dimmest region
    r0, n0 = rows[0]
    ue = np.asarray(Image.open(GAMMA / m / n0).convert("RGB"))
    gold = np.asarray(Image.open(GOLD / m / n0).convert("RGB"))
    ug, gg = luma(q565(ue)), luma(gold.astype(np.float64))
    ratio = np.where(gg > 20, ug / np.maximum(gg, 1e-6), 1.0)
    # dimmest 64x64 block centroid
    from numpy.lib.stride_tricks import sliding_window_view  # noqa
    small = ratio[::8, ::8]
    yy, xx = np.unravel_index(np.argmin(
        np.where(small > 0, small, 9)), small.shape)
    cy, cx = yy * 8, xx * 8
    box = (max(0, cx - 300), max(0, cy - 220),
           min(1600, cx + 300), min(1200, cy + 220))
    g = Image.open(GOLD / m / n0).convert("RGB").crop(box).resize((500, 400))
    u = Image.open(GAMMA / m / n0).convert("RGB").crop(box).resize((500, 400))
    sbs = Image.new("RGB", (1012, 400), (40, 40, 40))
    sbs.paste(g, (0, 0))
    sbs.paste(u, (508, 0))
    sbs.save(OUT / f"dim_{m}.png")
    print(f"  crop -> dim_{m}.png (worst {n0}, dim block at {cx},{cy})")
