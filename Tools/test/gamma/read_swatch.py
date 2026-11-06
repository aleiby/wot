"""Read the swatch render: patch centers via pinhole projection.
Camera: pawn(0,0,58)+eye42 -> (0,0,100), yaw 0 (+X), 1600x1200 FOV90
(f = 800 px). Patch (300, y, z): sx = 800*(1 + y/300), sy = 600 -
800*(z-100)/300. Prints D_in -> byte_out."""
import sys

import numpy as np
from PIL import Image

img = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=np.int32)
VALUES = list(range(8, 256, 8))
print("D_in  out  (pow22_pred  srgb_pred)")
for idx, val in enumerate(VALUES):
    r, c = divmod(idx, 8)
    y = (c - 3.5) * 60.0
    z = 200.0 - r * 60.0
    sx = int(800 * (1 + y / 300.0))
    sy = int(600 - 800 * (z - 100.0) / 300.0)
    patch = img[max(0, sy - 5):sy + 5, max(0, sx - 5):sx + 5]
    out = int(np.median(patch[..., 1]))
    d = val / 255.0
    pow22 = round(255 * (d ** (1 / 2.2)))
    srgb = round(255 * (1.055 * d ** (1 / 2.4) - 0.055 if d > 0.0031308
                        else 12.92 * d))
    print(f"{val:4d} {out:4d}   ({pow22:4d} {srgb:4d})")
