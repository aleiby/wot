"""Golden vs UE transfer-curve analysis.

For every registered pair (golden capture, UE render at identical pose):
downsample to kill registration noise, then per channel:
  - binned empirical LUT: golden value -> median UE value
  - free power-law fit  ue = a * g^gamma  (log-log least squares)
Aggregates per map + global; compares candidate curves by RMSE on the LUT.
Writes lut_data.json for plotting.
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from wot_config import exports_dir, local_path  # noqa: E402
SCRATCH = local_path("scratch_root")
GOLDEN = exports_dir() / "reference" / "shots"
UE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else SCRATCH / "ue_baseline"
OUT = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else SCRATCH / "lut_data.json"

DOWN = (200, 150)          # registration-noise averaging
LO, HI = 4, 250            # exclude dither-noise blacks and clipped whites
BINS = 64

def load(path):
    img = Image.open(path).convert("RGB").resize(DOWN, Image.BILINEAR)
    return np.asarray(img, dtype=np.float64)

def fit_power(g, u):
    """ue = a * g^gamma on [0,1] scale; returns (a, gamma, n)."""
    gn, un = g / 255.0, u / 255.0
    m = (g >= LO) & (g <= HI) & (u >= LO) & (u <= HI)
    if m.sum() < 500:
        return None
    lg, lu = np.log(gn[m]), np.log(un[m])
    gamma, loga = np.polyfit(lg, lu, 1)
    return float(np.exp(loga)), float(gamma), int(m.sum())

maps = {}
all_g = {c: [] for c in range(3)}
all_u = {c: [] for c in range(3)}

for map_dir in sorted(UE.iterdir()):
    if not map_dir.is_dir():
        continue
    name = map_dir.name
    fits = []
    for ue_png in sorted(map_dir.glob("*.png")):
        gpath = GOLDEN / name / ue_png.name
        if not gpath.exists():
            continue
        gi, ui = load(gpath), load(ue_png)
        for c in range(3):
            all_g[c].append(gi[..., c].ravel())
            all_u[c].append(ui[..., c].ravel())
        f = fit_power(gi.ravel(), ui.ravel())
        if f:
            fits.append(f)
    if fits:
        gammas = [f[1] for f in fits]
        scales = [f[0] for f in fits]
        maps[name] = dict(
            n_pairs=len(fits),
            gamma_median=float(np.median(gammas)),
            gamma_iqr=[float(np.percentile(gammas, 25)), float(np.percentile(gammas, 75))],
            scale_median=float(np.median(scales)),
        )
        print(f"{name}: pairs={len(fits)} gamma={np.median(gammas):.3f} "
              f"[{np.percentile(gammas,25):.3f},{np.percentile(gammas,75):.3f}] "
              f"scale={np.median(scales):.3f}")

# global per-channel fits + binned LUT
channels = {}
for c, cname in enumerate("RGB"):
    g = np.concatenate(all_g[c])
    u = np.concatenate(all_u[c])
    a, gamma, n = fit_power(g, u)
    # binned LUT (median UE per golden bin)
    edges = np.linspace(0, 255, BINS + 1)
    centers, med, q1, q3, cnt = [], [], [], [], []
    for i in range(BINS):
        m = (g >= edges[i]) & (g < edges[i + 1]) & (u > 0)
        if m.sum() > 200:
            centers.append(float((edges[i] + edges[i + 1]) / 2))
            med.append(float(np.median(u[m])))
            q1.append(float(np.percentile(u[m], 25)))
            q3.append(float(np.percentile(u[m], 75)))
            cnt.append(int(m.sum()))
    channels[cname] = dict(a=a, gamma=gamma, n=n, lut_x=centers, lut_med=med,
                           lut_q1=q1, lut_q3=q3, lut_n=cnt)
    print(f"channel {cname}: gamma={gamma:.3f} a={a:.3f} n={n}")

# candidate-curve RMSE against the luminance-channel binned LUT (use G channel)
x = np.array(channels["G"]["lut_x"]) / 255.0
y = np.array(channels["G"]["lut_med"]) / 255.0
def rmse(pred): return float(np.sqrt(np.mean((pred - y) ** 2)))
cands = {}
for label, fn in {
    "linear (best a)": lambda x, a: a * x,
    "pow 2.2": lambda x, a: a * x ** 2.2,
    "pow 1/2.2": lambda x, a: a * x ** (1 / 2.2),
    "pow 1.4 (glide ramp)": lambda x, a: a * x ** 1.4,
    "pow 1/1.4": lambda x, a: a * x ** (1 / 1.4),
}.items():
    # least-squares scale for each candidate shape
    base = fn(x, 1.0)
    a = float(np.dot(base, y) / np.dot(base, base))
    cands[label] = dict(a=a, rmse=rmse(fn(x, a)))
    print(f"candidate {label:22s} a={a:.3f} rmse={cands[label]['rmse']:.4f}")
free = channels["G"]
base = free["a"] * x ** free["gamma"]
cands["free power fit"] = dict(a=free["a"], gamma=free["gamma"], rmse=rmse(base))
print(f"candidate free power (gamma={free['gamma']:.3f})    rmse={cands['free power fit']['rmse']:.4f}")

OUT.write_text(json.dumps(dict(maps=maps, channels=channels, candidates=cands), indent=1))
print("WROTE", OUT)
