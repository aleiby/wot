"""Decompose every remaining gate-sweep mismatch row: is the disc delta a
CORONA mismatch or LOCAL SURFACE-PARITY pollution?

Per row: project the light (sweep math), then over
  core   = disc r <= 40 (the sweep's window)
  ann    = annulus outside the sprite: r in [1.1*HALF, 1.4*HALF],
           HALF = 640*DrawScale px (sprite half-width at 1600w)
compute gold/ours/nocorona means. Surface pollution = (gold-ours) over ann
(no sprite there in either render). Corona terms (surface-corrected):
  ours_c   = (ours - noc)_core   - (ours - noc)_ann      [~0 expected in ann]
  retail_c = (gold - noc)_core   - (gold - noc)_ann
"""
import csv
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mask_pickups import CX, CY, FX, FY, U2D, basis  # noqa: E402
from scoreboard import GOLD, luma, q565  # noqa: E402
from wot_gamma_paths import CONVERSION, SCRATCH  # noqa: E402

CONV = CONVERSION
OURS = SCRATCH / "lostrace_v1"
NOC = SCRATCH / "nocorona"

rows = list(csv.DictReader((SCRATCH / "gate_lostrace.csv").open()))
t3ds, mans = {}, {}
print(f"{'map':11s} {'pose':24s} {'light':9s} {'verd':10s} "
      f"{'discD':>6} {'annD':>6} {'oursC':>6} {'retlC':>6} class")

for row in rows:
    m, f, L = row["map"], row["pose"], row["corona"]
    if m not in t3ds:
        t3ds[m] = {a["name"]: a for a in json.loads(
            (CONV / f"{m}.t3d.json").read_text())["actors"]}
        mans[m] = json.loads((GOLD / m / "manifest.json").read_text())
    man = mans[m]
    eye = float(man.get("eye_height", 29.4))
    shot = next(s for s in man["shots"] if s["file"] == f)
    cam = np.array([shot["pawn"]["x"], shot["pawn"]["y"],
                    shot["pawn"]["z"] + eye])
    Fw, R, U = basis(shot["rot"]["pitch"] * U2D, shot["rot"]["yaw"] * U2D,
                     shot["rot"]["roll"] * U2D)
    a = t3ds[m][L]
    rel = np.array(a["location"], float) - cam
    xc = rel @ Fw
    sx = CX + FX * (rel @ R) / xc
    sy = CY - FY * (rel @ U) / xc
    ds = float(a.get("props", {}).get("DrawScale", 1.0))
    half = 640.0 * ds

    g = luma(np.asarray(Image.open(GOLD / m / f).convert("RGB")).astype(float))
    o = luma(q565(np.asarray(Image.open(OURS / m / f).convert("RGB"))))
    n = luma(q565(np.asarray(Image.open(NOC / m / f).convert("RGB"))))
    yy, xx = np.ogrid[:g.shape[0], :g.shape[1]]
    r = np.sqrt((xx - sx) ** 2 + (yy - sy) ** 2)
    core = r <= 40
    ann = (r >= 1.1 * half) & (r <= 1.4 * half)
    if not ann.any() or not core.any():
        continue
    discD = g[core].mean() - o[core].mean()
    annD = g[ann].mean() - o[ann].mean()
    ours_c = (o[core].mean() - n[core].mean()) - (o[ann].mean() - n[ann].mean())
    retl_c = (g[core].mean() - n[core].mean()) - (g[ann].mean() - n[ann].mean())
    cls = "SURFACE" if abs(annD) > 8 else (
        "CORONA" if abs(ours_c - retl_c) > 8 else "match?")
    print(f"{m:11s} {f[len(m)+1:]:24s} {L:9s} {row['verdict']:10s} "
          f"{discD:+6.1f} {annD:+6.1f} {ours_c:+6.1f} {retl_c:+6.1f} {cls}")
