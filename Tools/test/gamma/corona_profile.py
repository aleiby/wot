"""Radial-profile fit for the lantern corona (Battle2_04 PS12 Light187).

Retail model (display space, screen blend): g(r) = S(r) + B(r)*(1-S(r)/255)
with S(r) = Tex(r/scale) * Color * Bright * RZ (all display-space).
Extract S_golden(r) = (g - B) / (1 - B/255) and compare against:
  - candidate curves from the corona05 texture: T*C (display multiply),
    (T*C)^2.2 (linear-crush), T^2.2*C (texture-only crush)
  - our measured add contribution A(r) - B(r).
Sprite half-width at PS12: Scale = 512*0.2*1600/640 / 2 = 128 px.
"""
from pathlib import Path

import numpy as np
from PIL import Image
from wot_gamma_paths import CONVERSION, GOLD, SCRATCH

SX, SY = 1288, 92
F = "Battle2_04_PlayerStart12_fwd.png"
HALF = 128.0        # sprite half-width in px (512*DrawScale*W/640 / 2)
CB = np.array([0.8529, 0.4438, 0.2941])   # hue18/sat75 color, Bright*RZ=1


def load(root):
    img = Image.open(Path(root) / "Battle2_04" / F).convert("RGB")
    return np.asarray(img).astype(np.float64)


G = load(GOLD)
A = load(SCRATCH / "b204_addonly")
B = load(SCRATCH / "nocorona")

tex = np.asarray(Image.open(
    CONVERSION / "tex" / "JamesT" / "corona05.png").convert("RGB")
).astype(np.float64)
th, tw = tex.shape[:2]

yy, xx = np.ogrid[:G.shape[0], :G.shape[1]]
r = np.sqrt((xx - SX) ** 2 + (yy - SY) ** 2)

# luma helpers
def luma(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


GL, AL, BL = luma(G), luma(A), luma(B)

# radial texture profile (texture is a radial gradient; sample along +x)
cy, cx = th // 2, tw // 2
tprof = luma(tex)[cy, cx:]                      # 0..half-width of texture
tex_r = np.arange(len(tprof)) / (len(tprof) - 1)  # normalized 0..1

print(f"{'r_px':>5} {'T':>6} {'S_pred':>7} {'S_gold':>7} {'ourAdd':>7} "
      f"{'S_pred^2.2':>10} {'B':>6}")
for rpx in [0, 8, 16, 24, 32, 48, 64, 80, 96, 112, 126]:
    ring = (np.abs(r - rpx) < 2.0) & (yy >= 0)
    ring = ring & (yy < G.shape[0]) & (xx < G.shape[1])
    if not ring.any():
        continue
    g, a, b = GL[ring].mean(), AL[ring].mean(), BL[ring].mean()
    tn = min(rpx / HALF, 1.0)
    T = np.interp(tn, tex_r, tprof)             # texture luma 0..255
    Slum = T * (0.299 * CB[0] + 0.587 * CB[1] + 0.114 * CB[2])
    s_gold = (g - b) / max(1e-6, 1 - b / 255.0)
    s_ours = a - b
    print(f"{rpx:5d} {T:6.1f} {Slum:7.1f} {s_gold:7.1f} {s_ours:7.1f} "
          f"{255*(Slum/255)**2.2:10.1f} {b:6.1f}")
