"""Atlas-clamp probe over the composed flat arrays."""
import sys
from pathlib import Path
import numpy as np
from wot_gamma_paths import SCRATCH

WIN = {1745, 1746, 1752, 1753, 1784, 6318, 1949, 1961, 1973}
source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else SCRATCH / "b202_composed.npz"
z = np.load(source, allow_pickle=True)
rgb = z["texel_rgb"]          # composed merged values (RGBA7 domain 0..127?)
sid = z["surf_id"]
print("texel_rgb shape/dtype:", rgb.shape, rgb.dtype,
      "min/max:", rgb.min(), rgb.max())
print("surf_id shape:", sid.shape, "unique surfs:", len(np.unique(sid)))

CLAMP = 63.75
for name, mask in [("ALL surfs", np.ones(len(sid), bool)),
                   ("WIN surfs", np.isin(sid, list(WIN)))]:
    r = rgb[mask].astype(np.float64)
    mx = r.max(axis=1)                     # brightest channel per texel
    over = mx > CLAMP
    frac = float(over.mean())
    # predicted ours/retail ratio where clamped (per brightest channel)
    ratio = np.minimum(mx, CLAMP) / np.maximum(mx, 1e-6)
    print(f"{name}: texels={len(r)} over-ceiling={frac:.4f} "
          f"({int(over.sum())} texels)")
    if over.any():
        print(f"   over-texels: max total={mx[over].max():.1f} "
              f"median={np.median(mx[over]):.1f} "
              f"predicted ratio median={np.median(ratio[over]):.3f} "
              f"P10={np.percentile(ratio[over], 10):.3f}")
    # luma-weighted overall predicted dimming on this surf set
    lum = 0.299 * r[:, 0] + 0.587 * r[:, 1] + 0.114 * r[:, 2]
    lum_c = 0.299 * np.minimum(r[:, 0], CLAMP) \
        + 0.587 * np.minimum(r[:, 1], CLAMP) \
        + 0.114 * np.minimum(r[:, 2], CLAMP)
    bright = lum > 40      # bright texels (top of range)
    if bright.any():
        print(f"   bright texels (merged luma>40): {int(bright.sum())} "
              f"({float(bright.mean()):.3f}) "
              f"clamped-luma/unclamped ratio mean="
              f"{float((lum_c[bright] / lum[bright]).mean()):.3f} "
              f"P10={np.percentile(lum_c[bright] / lum[bright], 10):.3f}")
