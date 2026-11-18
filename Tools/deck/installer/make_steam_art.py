#!/usr/bin/env python3
"""Render WoT Reborn's Steam library artwork for the SteamOS installer.

Owner rulings: "our own simple art" (2026-11-08), the blurred main-menu
backdrop that already ships in the game; and (2026-11-09) the title in the
game's large menu lettering, "cleaned up and high res". Nothing from the
original game is used: the wordmark is new lettering drawn in that style.

Inputs:
  backdrop  Source/WoT/UI/Resources/WoTSetupBackdropDefault.png, the blurred
            setup backdrop that ships in the download.
  wordmark  Tools/deck/installer/art/wordmark.png, "WoT Reborn" as a
            transparent PNG (generated once with an image model from a crop of
            the menu's title lettering, then cropped to its bounds).

Outputs, committed under Tools/deck/installer/art/ (Steam grid names use the
shortcut's appid; the installer renames):
  capsule_portrait.jpg  600x900   -> <appid>p.jpg
  capsule_wide.jpg      920x430   -> <appid>.jpg
  hero.jpg              3840x1240 -> <appid>_hero.jpg
  logo.png              1280x360  -> <appid>_logo.png (transparent)
  logo.json                       -> <appid>.json

Dev-side only (Pillow). Deterministic for a given Pillow version and inputs.
    python3 Tools/deck/installer/make_steam_art.py \\
        --backdrop Source/WoT/UI/Resources/WoTSetupBackdropDefault.png \\
        --wordmark Tools/deck/installer/art/wordmark.png \\
        --out Tools/deck/installer/art
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

GOLD = (196, 150, 74)            # a brighter take on GameAccentColor for small rules
CHARCOAL = (18, 17, 16)
TITLE = "WoT Reborn"


def cover(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    """Scale to cover size, centre-crop. The backdrop is already blurred."""
    w, h = size
    scale = max(w / img.width, h / img.height)
    scaled = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
    left = (scaled.width - w) // 2
    top = (scaled.height - h) // 2
    return scaled.crop((left, top, left + w, top + h))


def base(backdrop: Image.Image, size: tuple[int, int], darken: float) -> Image.Image:
    img = cover(backdrop.convert("RGB"), size)
    img = img.filter(ImageFilter.GaussianBlur(radius=max(size) / 400))
    img = ImageEnhance.Brightness(img).enhance(darken)
    # A soft vignette towards charcoal keeps the title legible on any crop.
    vignette = Image.new("L", size, 0)
    d = ImageDraw.Draw(vignette)
    steps = 40
    for i in range(steps):
        inset_x = size[0] * i / (2.6 * steps)
        inset_y = size[1] * i / (2.6 * steps)
        d.rectangle([inset_x, inset_y, size[0] - inset_x, size[1] - inset_y],
                    fill=round(255 * i / steps))
    vignette = vignette.filter(ImageFilter.GaussianBlur(radius=max(size) / 20))
    return Image.composite(img, Image.new("RGB", size, CHARCOAL), vignette)


def title_layer(size: tuple[int, int], wordmark: Image.Image, fraction: float,
                rule: bool) -> Image.Image:
    """Transparent layer: the wordmark centred at fraction * width, a gold rule below."""
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    tw = round(size[0] * fraction)
    th = round(wordmark.height * tw / wordmark.width)
    mark = wordmark.resize((tw, th), Image.LANCZOS)
    rule_gap = max(6, th // 6) if rule else 0
    thickness = max(2, th // 28) if rule else 0
    x = (size[0] - tw) // 2
    y = (size[1] - (th + rule_gap + thickness)) // 2
    layer.alpha_composite(mark, (x, y))
    if rule:
        rule_w = int(tw * 0.62)
        rule_y = y + th + rule_gap
        ImageDraw.Draw(layer).rectangle(
            [(size[0] - rule_w) // 2, rule_y, (size[0] + rule_w) // 2, rule_y + thickness],
            fill=GOLD + (255,))
    return layer


def capsule(backdrop, wordmark, size, fraction):
    img = base(backdrop, size, 0.78).convert("RGBA")
    img = Image.alpha_composite(img, title_layer(size, wordmark, fraction, True))
    return img.convert("RGB")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--backdrop", type=Path, required=True)
    p.add_argument("--wordmark", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    backdrop = Image.open(a.backdrop)
    wordmark = Image.open(a.wordmark).convert("RGBA")
    a.out.mkdir(parents=True, exist_ok=True)
    jpeg = dict(quality=88, optimize=True, progressive=False, subsampling=0)
    capsule(backdrop, wordmark, (600, 900), 0.86).save(a.out / "capsule_portrait.jpg", **jpeg)
    capsule(backdrop, wordmark, (920, 430), 0.74).save(a.out / "capsule_wide.jpg", **jpeg)
    base(backdrop, (3840, 1240), 1.25).save(a.out / "hero.jpg", **jpeg)
    logo_size = (1280, 360)
    title_layer(logo_size, wordmark, 0.92, True).save(a.out / "logo.png", optimize=True)
    (a.out / "logo.json").write_text(json.dumps(
        {"nVersion": 1, "logoPosition": {"pinnedPosition": "BottomLeft",
                                          "nWidthPct": 42, "nHeightPct": 50}}) + "\n", encoding="utf-8")
    for f in sorted(a.out.iterdir()):
        print(f.name, f.stat().st_size)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
