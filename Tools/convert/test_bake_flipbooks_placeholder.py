"""Regression coverage for path-preserving fractal sheet placeholders."""
import base64
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bake_flipbooks  # noqa: E402


class _PlaceholderFixturePackage:
    name = "SyntheticPkg"
    exports = [
        {"cls": "Palette", "group": 0, "name": "TinyPalette"},
        {"cls": "FireTexture", "group": 0, "name": "TinyFire"},
    ]

    def __init__(self, _path):
        pass

    @staticmethod
    def objname(ref):
        return ref

    def objpath(self, ref):
        return self.exports[ref - 1]["name"]


def _placeholder_fixture_palette():
    return [(index, 255 - index, index // 2, 255) for index in range(256)]


def _placeholder_fixture_fractal():
    return {
        "name": "TinyFire",
        "cls": "FireTexture",
        "props": {
            "Palette": 1,
            "MinFrameRate": 12.5,
            "MaxFrameRate": 34.5,
            "PrimeCount": 3,
            "WaveAmp": 17,
            "BumpMapLight": 18,
            "BumpMapAngle": 19,
            "PhongRange": 20,
            "PhongSize": 21,
            "RenderHeat": 22,
            "bRising": True,
            "SparksLimit": 7,
            "NumDrops": 1,
            "PanningStyle": 2,
            "TimeMethod": 3,
            "HorizPanSpeed": 4,
            "VertPanSpeed": 5,
            "Frequency": 6,
            "Amplitude": 7,
            "MoveIce": True,
            "MasterCount": 8.5,
            "UDisplace": 9.5,
            "VDisplace": 10.5,
            "UPosition": 11.5,
            "VPosition": 12.5,
            "_structs": {
                "Drops": {0: bytes((1, 2, 3, 4, 5, 6, 7, 8))},
            },
        },
        "mips": [(2, 2, bytes((0, 1, 2, 3)))],
        "sparks": [bytes((0, 250, 1, 1, 4, 5, 6, 7))],
    }


class FlipbookPlaceholderTests(unittest.TestCase):
    def test_no_sheet_png_is_emitted_and_runtime_record_is_unchanged(self):
        palette = _placeholder_fixture_palette()
        fractal = _placeholder_fixture_fractal()
        expected_palette = bytes(
            channel
            for color in palette
            for channel in (color[0], color[1], color[2], 255)
        )
        expected_prechange_runtime = {
            "class_name": "FireTexture",
            "texture_name": "TinyFire",
            "object_path": "SyntheticPkg.TinyFire",
            "width": 2,
            "height": 2,
            "min_frame_rate": 12.5,
            "max_frame_rate": 34.5,
            "prime_count": 3,
            "wave_amp": 17,
            "bump_map_light": 18,
            "bump_map_angle": 19,
            "phong_range": 20,
            "phong_size": 21,
            "render_heat": 22,
            "rising": True,
            "sparks_limit": 7,
            "palette_rgba": base64.b64encode(expected_palette).decode("ascii"),
            "initial_bitmap": base64.b64encode(bytes((0, 1, 2, 3))).decode("ascii"),
            "sparks": [[0, 250, 1, 1, 4, 5, 6, 7]],
            "drops": [[1, 2, 3, 4, 5, 6, 7, 8]],
            "source_texture": "",
            "source_is_fractal": False,
            "source_width": 0,
            "source_height": 0,
            "source_indices": "",
            "glass_texture": "",
            "glass_is_fractal": False,
            "glass_width": 0,
            "glass_height": 0,
            "glass_indices": "",
            "panning_style": 2,
            "time_method": 3,
            "horiz_pan_speed": 4,
            "vert_pan_speed": 5,
            "frequency": 6,
            "amplitude": 7,
            "move_ice": True,
            "master_count": 8.5,
            "u_displace": 9.5,
            "v_displace": 10.5,
            "u_position": 11.5,
            "v_position": 12.5,
        }

        with tempfile.TemporaryDirectory() as temporary:
            output_root = Path(temporary)
            with (
                mock.patch.object(
                    bake_flipbooks, "Package", _PlaceholderFixturePackage
                ),
                mock.patch.object(
                    bake_flipbooks, "decode_palette", return_value=palette
                ),
                mock.patch.object(
                    bake_flipbooks, "decode_fractal", return_value=fractal
                ),
            ):
                baked, skipped = bake_flipbooks.bake_package(
                    "SyntheticPkg.utx", output_root
                )

            self.assertEqual((baked, skipped), (1, 0))
            package_root = output_root / "SyntheticPkg"
            # Sheet-scrub retirement (flipbook phase 3): the bake emits NO
            # sheet PNG — nothing reads one — while the manifest keeps the
            # load-bearing grid/frame dims (import_map pan frames).
            self.assertFalse(
                (package_root / "TinyFire_sheet.png").exists())

            manifest = json.loads(
                (package_root / "_flipbooks.json").read_text(encoding="utf-8")
            )
            runtime_record = manifest["runtime"]["TinyFire"]
            self.assertEqual(
                json.dumps(runtime_record, separators=(",", ":")).encode(),
                json.dumps(
                    expected_prechange_runtime, separators=(",", ":")
                ).encode(),
            )
            self.assertEqual(
                base64.b64decode(
                    runtime_record["initial_bitmap"]
                ),
                bytes((0, 1, 2, 3)),
            )
            self.assertEqual(
                runtime_record["drops"],
                [[1, 2, 3, 4, 5, 6, 7, 8]],
            )


if __name__ == "__main__":
    unittest.main()
