"""Emit pixel-exact brush textures from the shipped UWindow atlases.

Usage:
    python stitch_uwindow_textures.py <UWindow PNG directory> <output directory>

Every output texel is copied from Misc.png or ActiveFrameS.png. The region
table is UWindowWin95LookAndFeel.uc defaultproperties:791-863.
"""
import sys
from pathlib import Path

from PIL import Image


def crop(source, box):
    x, y, width, height = box
    return source.crop((x, y, x + width, y + height))


def paste(output, source, box, position):
    output.paste(crop(source, box), position)


def tile(output, source, box, destination):
    x, y, width, height = destination
    sample = crop(source, box)
    for destination_y in range(y, y + height, sample.height):
        for destination_x in range(x, x + width, sample.width):
            remaining_width = min(sample.width, x + width - destination_x)
            remaining_height = min(sample.height, y + height - destination_y)
            output.paste(
                sample.crop((0, 0, remaining_width, remaining_height)),
                (destination_x, destination_y),
            )


def save_crop(source, box, output_path):
    crop(source, box).save(output_path)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: stitch_uwindow_textures.py "
            "<UWindow PNG directory> <output directory>")

    source_dir = Path(sys.argv[1])
    output_dir = Path(sys.argv[2])
    output_dir.mkdir(parents=True, exist_ok=True)
    misc = Image.open(source_dir / "Misc.png").convert("RGBA")
    active_frame_s = Image.open(
        source_dir / "ActiveFrameS.png").convert("RGBA")

    # BevelUp*: 2px corners, 1px edge/area samples -> a true 5x5 box.
    raised = Image.new("RGBA", (5, 5), (0, 0, 0, 0))
    for box, position in (
        ((4, 16, 2, 2), (0, 0)),    # BevelUpTL
        ((10, 16, 1, 2), (2, 0)),   # BevelUpT
        ((18, 16, 2, 2), (3, 0)),   # BevelUpTR
        ((4, 20, 2, 1), (0, 2)),    # BevelUpL
        ((8, 20, 1, 1), (2, 2)),    # BevelUpArea
        ((18, 20, 2, 1), (3, 2)),   # BevelUpR
        ((4, 30, 2, 2), (0, 3)),    # BevelUpBL
        ((10, 30, 1, 2), (2, 3)),   # BevelUpB
        ((18, 30, 2, 2), (3, 3)),   # BevelUpBR
    ):
        paste(raised, misc, box, position)
    raised.save(output_dir / "RaisedBevel.png")

    # MiscBevel[1] is one authored contiguous 122x17 frame. MiscBevelArea(2)
    # is the parent-adjudicated contiguous 9x9 sunken frame.
    save_crop(misc, (0, 0, 122, 17), output_dir / "RecessedBevel.png")
    save_crop(misc, (2, 35, 9, 9), output_dir / "SunkenFrame.png")

    # ActiveFrameS with status bar:
    #   L=2, center=1, R=16; T=16, center=1, B=16 -> 19x33.
    # FrameTR/FrameR are only 2px wide. Fill the other 14px of their 16px
    # box column with authored FrameT/transparent-center samples, leaving the
    # full 16x16 FrameSBR resize-grip region intact.
    frame = Image.new("RGBA", (19, 33), (0, 0, 0, 0))
    paste(frame, active_frame_s, (0, 0, 2, 16), (0, 0))       # FrameTL
    tile(frame, active_frame_s, (32, 0, 1, 16), (2, 0, 15, 16))
    paste(frame, active_frame_s, (126, 0, 2, 16), (17, 0))   # FrameTR
    paste(frame, active_frame_s, (0, 32, 2, 1), (0, 16))     # FrameL
    tile(frame, active_frame_s, (32, 32, 1, 1), (2, 16, 15, 1))
    paste(frame, active_frame_s, (126, 32, 2, 1), (17, 16))  # FrameR
    paste(frame, active_frame_s, (0, 112, 2, 16), (0, 17))   # FrameSBL
    paste(frame, active_frame_s, (32, 112, 1, 16), (2, 17))  # FrameSB
    paste(frame, active_frame_s, (112, 112, 16, 16), (3, 17))  # FrameSBR
    frame.save(output_dir / "WindowFrame.png")

    # Only currently-consumed scrollbar regions are emitted. Tabs remain
    # census-only because the current one-list browser has no tab widget.
    for name, box in (
        ("ScrollUp", (20, 16, 12, 10)),
        ("ScrollDown", (20, 26, 12, 10)),
        ("ScrollLeft", (20, 48, 10, 12)),
        ("ScrollRight", (20, 36, 10, 12)),
        ("ScrollBackground", (4, 79, 1, 1)),
    ):
        save_crop(misc, box, output_dir / f"{name}.png")


if __name__ == "__main__":
    main()
