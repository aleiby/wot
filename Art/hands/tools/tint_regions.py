#!/usr/bin/env python3
"""Colour regions of a baked hand texture, for the team-colour mask.

`clusters`: k-means the used texels (Lab-ish, fixed seed) and print each cluster's mean
colour and coverage, writing a cluster preview PNG, so a person can pick the tintable ids.
`variants`: given the tintable cluster ids, write mask.png (soft edges) and recoloured
textures for each team colour. Hue and saturation come from the target, and the texel's
own value is kept, so the hand-painted shading survives.
usage: tint_regions.py clusters tex.png outdir [k]
       tint_regions.py variants tex.png outdir ids(comma) colours.json
"""
import colorsys
import json
import pathlib
import sys

import numpy as np
from PIL import Image, ImageFilter


def used_mask(rgb):
    return rgb.max(axis=2) > 6  # bake background is pure black


def kmeans(x, k, iters=40, seed=7):
    rng = np.random.default_rng(seed)
    centers = x[rng.choice(len(x), k, replace=False)]
    for _ in range(iters):
        labels = np.argmin(((x[:, None, :] - centers[None]) ** 2).sum(-1), axis=1)
        new = np.array([x[labels == i].mean(0) if np.any(labels == i) else centers[i] for i in range(k)])
        if np.allclose(new, centers):
            break
        centers = new
    return labels, centers


def features(rgb):
    # Luma plus chroma, with luma weighted down so shading doesn't split one material.
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    return np.stack([0.5 * y, r - y, b - y, (r - g)], axis=-1)


def cmd_clusters(tex, out, k):
    rgb = np.asarray(Image.open(tex).convert("RGB")).astype(np.float32)
    used = used_mask(rgb)
    labels, _ = kmeans(features(rgb)[used], k)
    full = np.full(used.shape, -1)
    full[used] = labels
    np.save(out / "labels.npy", full)
    palette = []
    preview = np.zeros_like(rgb)
    for i in range(k):
        sel = full == i
        mean = rgb[sel].mean(0)
        palette.append({"id": i, "mean": "#%02x%02x%02x" % tuple(int(v) for v in mean),
                        "coverage": round(float(sel.sum()) / float(used.sum()), 3)})
        preview[sel] = mean
    Image.fromarray(preview.astype(np.uint8)).save(out / "clusters.png")
    print(json.dumps(palette, indent=1))


def recolour(rgb, target_hex, value_scale):
    tr, tg, tb = (int(target_hex[i:i + 2], 16) / 255 for i in (1, 3, 5))
    th, ts, tv = colorsys.rgb_to_hsv(tr, tg, tb)
    flat = rgb.reshape(-1, 3) / 255.0
    out = np.empty_like(flat)
    for i, (r, g, b) in enumerate(flat):
        _, s, v = colorsys.rgb_to_hsv(r, g, b)
        out[i] = colorsys.hsv_to_rgb(th, min(1.0, ts * (0.6 + 0.8 * s)), min(1.0, v * value_scale))
    return (out.reshape(rgb.shape) * 255).astype(np.float32)


def cmd_variants(tex, out, ids, colours):
    rgb = np.asarray(Image.open(tex).convert("RGB")).astype(np.float32)
    labels = np.load(out / "labels.npy")
    hard = np.isin(labels, ids).astype(np.uint8) * 255
    # Close small holes (light painted strokes that clustered with skin) before softening.
    closed = Image.fromarray(hard).filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))
    soft = closed.filter(ImageFilter.GaussianBlur(1.2))
    soft.save(out / "mask.png")
    m = (np.asarray(soft).astype(np.float32) / 255.0)[..., None]
    for name, spec in colours.items():
        tinted = recolour(rgb, spec["hex"], spec.get("value_scale", 1.0))
        mixed = rgb * (1 - m) + tinted * m
        Image.fromarray(np.clip(mixed, 0, 255).astype(np.uint8)).save(out / f"basecolor-{name}.png")
        print(f"wrote basecolor-{name}.png")


if __name__ == "__main__":
    cmd, tex, out = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    if cmd == "clusters":
        cmd_clusters(tex, out, int(sys.argv[4]) if len(sys.argv) > 4 else 6)
    else:
        cmd_variants(tex, out, [int(v) for v in sys.argv[4].split(",")], json.loads(pathlib.Path(sys.argv[5]).read_text()))
