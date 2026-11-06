"""Replicate skybake's intersection on Arena_04's sky-zone nodes and
report per-node diagnostics (plane hits, polygon-inside counts)."""
import json
import sys
from pathlib import Path

import numpy as np

TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS / "convert"))
sys.path.insert(0, str(TOOLS))
from wot_config import local_path, original_dir  # noqa: E402
import lightmaps as L

S = local_path("conversion_root")
name = "Arena_04"
pkg = L.Package(original_dir() / "Maps" / f"{name}.wot")
models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
bsp = L.decode_model_full(pkg, max(models, key=lambda e: e["size"]))
actors = json.load(open(S / f"{name}.t3d.json"))["actors"]
skies = [a for a in actors if a["cls"].lower() == "skyzoneinfo"]
winner = skies[-1]
zone_names = [str(pkg.objname(z)) if z else None for z in (bsp["zones"] or [])]
sky_zone = next(zi for zi, zn in enumerate(zone_names)
                if zn and zn.lower() == winner["name"].lower())
origin = np.array(winner["location"], dtype=np.float64)
pts = np.array(bsp["points"])
vecs = np.array(bsp["vectors"])

W, H = 64, 32
us = (np.arange(W) + 0.5) / W
vs = (np.arange(H) + 0.5) / H
phi = (us - 0.5) * 2 * np.pi
theta = vs * np.pi
st, ct = np.sin(theta), np.cos(theta)
d = np.empty((H, W, 3))
d[..., 0] = st[:, None] * np.cos(phi)[None, :]
d[..., 1] = st[:, None] * np.sin(phi)[None, :]
d[..., 2] = ct[:, None]
d = d.reshape(-1, 3)

for node in bsp["nodes"]:
    ivp, isurf, nv = node[0], node[1], node[2]
    iz = node[4] if len(node) > 4 else (0, 0)
    if nv < 3 or sky_zone not in tuple(iz):
        continue
    s = bsp["surfs"][isurf]
    n_ = vecs[s["vnorm"]]
    base = pts[s["pbase"]]
    tex = pkg.objname(s["tex"])
    denom = d @ n_
    with np.errstate(divide="ignore", invalid="ignore"):
        t = ((base - origin) @ n_) / denom
    ok = np.isfinite(t) & (t > 1.0)
    poly = pts[[bsp["verts"][ivp + k][0] for k in range(nv)]]
    ax = int(np.argmax(np.abs(n_)))
    keep = [i for i in range(3) if i != ax]
    hitp = origin[None, :] + d * t[:, None]
    p2 = poly[:, keep]
    h2 = hitp[:, keep]
    inside = ok.copy()
    sign = None
    for i in range(nv):
        e = p2[(i + 1) % nv] - p2[i]
        w_ = h2 - p2[i][None, :]
        cr = e[0] * w_[:, 1] - e[1] * w_[:, 0]
        if sign is None:
            sign = np.where(cr >= 0, 1.0, -1.0)
        inside &= (cr * sign) >= -1e-6
    print(f"node isurf={isurf} nv={nv} tex={tex} flags={hex(s['flags'])} "
          f"iz={tuple(iz)} plane_ok={int(ok.sum())} inside={int(inside.sum())} "
          f"poly_z={poly[:, 2].min():.0f}..{poly[:, 2].max():.0f}")
