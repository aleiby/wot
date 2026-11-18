"""Display-domain fit: best achievable K for stock UE5 lights, honestly scored.

display_UE(K) = (K * sum_L linear_contrib)^(1/2.2)  vs  M (UE1 display mult).
Also scores single-light vs multi-light texels separately to isolate the
display-space-additivity divergence.
"""
import json
import sys
from pathlib import Path

import numpy as np

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "convert"))
from wot_config import local_path  # noqa: E402
SCRATCH = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else local_path("scratch_root")
from lightmaps import fget_hsv

E = 5.3

def build_model(name):
    z = np.load(SCRATCH / "lmcal" / f"{name}.npz", allow_pickle=True)
    pos, rgb, surf_id = z["texel_pos"], z["texel_rgb"], z["surf_id"]
    inv = np.full(int(z["surf_index"].max()) + 1, -1, dtype=np.int64)
    inv[z["surf_index"]] = np.arange(len(z["surf_index"]))
    normals = z["surf_normal"][inv[surf_id]]

    t3d = json.loads((SCRATCH / "wotconv" / f"{name}.t3d.json").read_text())
    N = len(pos)
    L5 = np.zeros((N, 3))
    nlights = np.zeros(N, dtype=np.int32)
    for a in t3d["actors"]:
        if a["cls"] != "Light":
            continue
        p = a["props"]
        if p.get("LightType", "LT_Steady") != "LT_Steady":
            continue
        R = 25.0 * (int(p.get("LightRadius", 64)) + 1)
        b = int(p.get("LightBrightness", 64)) / 255.0
        col = np.array(fget_hsv(int(p.get("LightHue", 0)),
                                int(p.get("LightSaturation", 255)), 255)) ** 2.2
        loc = np.array(a["location"])
        d = pos - loc
        dist2 = np.einsum("ij,ij->i", d, d)
        s2 = dist2 / (R * R)
        m = s2 < 1.0
        if not m.any():
            continue
        dist = np.sqrt(dist2[m]) + 1e-6
        cos = np.abs(np.einsum("ij,ij->i", -d[m], normals[m])) / dist
        contrib = (b ** 2.2) * ((1.0 - s2[m]) ** E) * cos
        L5[m] += contrib[:, None] * col[None, :]
        nlights[m] += (contrib > 1e-4)
    M = np.clip(rgb * 2.0 / 255.0, 0, 1)
    return L5, M, nlights

def score(L5, M, K):
    disp = np.clip(K * L5, 0, None) ** (1 / 2.2)
    disp = np.clip(disp, 0, 1)
    return disp

for name in ("Arena_01", "Arena_03", "Arena_05"):
    L5, M, nl = build_model(name)
    lit = (M.max(axis=1) > 0.03) | (L5.max(axis=1) > 1e-5)
    x, y = L5[lit], M[lit]
    # 1D optimize K in log space
    Ks = np.exp(np.linspace(np.log(0.05), np.log(50), 120))
    errs = [np.sqrt(np.mean((score(x, y, K) - y) ** 2)) for K in Ks]
    Kbest = float(Ks[int(np.argmin(errs))])
    best = min(errs)
    # single vs multi light residual at Kbest
    nlit = nl[lit]
    for label, msk in (("single", nlit == 1), ("multi", nlit >= 2)):
        if msk.sum():
            e = np.sqrt(np.mean((score(x[msk], y[msk], Kbest) - y[msk]) ** 2))
            print(f"  {name} {label:6s} texels={msk.sum():7d} rmse_display={e:.4f}")
    print(f"{name}: Kbest={Kbest:.2f} overall display-RMSE={best:.4f} "
          f"(display units 0-1; 0.05 ~= 13/255)")
