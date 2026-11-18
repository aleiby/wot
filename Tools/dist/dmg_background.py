#!/usr/bin/env python3
"""Generate the deterministic 1x/2x background for the Mac DMG."""
from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Callable, Sequence

from PIL import Image, ImageColor, ImageDraw, ImageFont

WINDOW = (660, 400)
HINT = "Drag WoT to Applications to install."
TOP_COLOUR = "#EDE6D6"
BOTTOM_COLOUR = "#E2D8C3"
ARROW_COLOUR = "#5A4A36"
HINT_COLOUR = "#4A3F33"
ARROW_START_X = 250
ARROW_END_X = 410
ARROW_Y = 190
ARROW_SHAFT_WIDTH = 6
ARROW_HEAD_LENGTH = 26
HINT_Y = 330
HINT_SIZE = 15

Runner = Callable[..., subprocess.CompletedProcess]


def _run(command: Sequence[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(list(command), capture_output=True, text=True, **kwargs)


def _interpolate(start: int, end: int, step: int, maximum: int) -> int:
    return round(start + ((end - start) * step / maximum))


def render_background(scale: int) -> Image.Image:
    """Return the Finder background rendered at an integer display scale."""
    if not isinstance(scale, int) or isinstance(scale, bool) or scale <= 0:
        raise ValueError("background scale must be a positive integer")

    width, height = (dimension * scale for dimension in WINDOW)
    top = ImageColor.getrgb(TOP_COLOUR)
    bottom = ImageColor.getrgb(BOTTOM_COLOUR)
    image = Image.new("RGB", (width, height), top)
    pixels = image.load()
    maximum = max(height - 1, 1)
    for y in range(height):
        colour = tuple(_interpolate(start, end, y, maximum)
                       for start, end in zip(top, bottom))
        for x in range(width):
            pixels[x, y] = colour

    draw = ImageDraw.Draw(image)
    centre_y = ARROW_Y * scale
    head_base = (ARROW_END_X - ARROW_HEAD_LENGTH) * scale
    head_half_height = ARROW_HEAD_LENGTH * scale // 2
    draw.line(((ARROW_START_X * scale, centre_y), (head_base, centre_y)),
              fill=ARROW_COLOUR, width=ARROW_SHAFT_WIDTH * scale)
    draw.polygon(((head_base, centre_y - head_half_height),
                  (ARROW_END_X * scale, centre_y),
                  (head_base, centre_y + head_half_height)),
                 fill=ARROW_COLOUR)

    font = ImageFont.load_default(size=HINT_SIZE * scale)
    draw.text((width // 2, HINT_Y * scale), HINT, font=font,
              fill=HINT_COLOUR, anchor="mm")
    return image


def build_background_tiff(out_dir: Path, runner: Runner = _run) -> Path:
    """Write 1x/2x PNGs and combine them into a HiDPI TIFF."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    one_x = out_dir / "background.png"
    two_x = out_dir / "background@2x.png"
    output = out_dir / "background.tiff"
    render_background(1).save(one_x, format="PNG")
    render_background(2).save(two_x, format="PNG")
    command = ["tiffutil", "-cathidpicheck", str(one_x), str(two_x),
               "-out", str(output)]
    result = runner(command)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(
            "tiffutil HiDPI background creation failed "
            f"({result.returncode}): {detail}")
    if not output.is_file():
        raise RuntimeError("tiffutil did not create the background TIFF")
    return output
