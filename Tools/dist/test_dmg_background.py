#!/usr/bin/env python3
"""Specs for the generated Finder background used by the Mac DMG."""
from __future__ import annotations

import importlib
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from PIL import Image


def _background_module():
    try:
        return importlib.import_module("dmg_background")
    except ModuleNotFoundError:
        raise AssertionError("dmg_background module is missing")


class DmgBackgroundTests(unittest.TestCase):
    def test_renders_exact_one_x_and_two_x_rgb_dimensions(self):
        background = _background_module()
        self.assertEqual(background.render_background(1).mode, "RGB")
        self.assertEqual(background.render_background(1).size, (660, 400))
        self.assertEqual(background.render_background(2).mode, "RGB")
        self.assertEqual(background.render_background(2).size, (1320, 800))

    def test_render_is_byte_deterministic(self):
        background = _background_module()
        first = background.render_background(2)
        second = background.render_background(2)
        self.assertEqual(first.tobytes(), second.tobytes())

    def test_arrow_midpoint_is_darker_than_background_corner(self):
        background = _background_module()
        for scale in (1, 2):
            with self.subTest(scale=scale):
                image = background.render_background(scale)
                arrow = image.getpixel((330 * scale, 190 * scale))
                corner = image.getpixel((0, 0))
                self.assertLess(sum(arrow), sum(corner))

    def test_build_writes_pngs_and_invokes_tiffutil_for_hidpi_tiff(self):
        background = _background_module()
        calls = []

        def runner(command):
            command = list(command)
            calls.append(command)
            with Image.open(command[2]) as one_x_image:
                one_x = one_x_image.copy()
            with Image.open(command[3]) as two_x_image:
                two_x = two_x_image.copy()
            one_x.save(command[-1], format="TIFF", save_all=True,
                       append_images=[two_x])
            return subprocess.CompletedProcess(command, 0, "", "")

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = background.build_background_tiff(root, runner=runner)
            one_x = root / "background.png"
            two_x = root / "background@2x.png"
            expected = root / "background.tiff"
            self.assertEqual(result, expected)
            self.assertEqual(calls, [[
                "tiffutil", "-cathidpicheck", str(one_x), str(two_x),
                "-out", str(expected),
            ]])
            with Image.open(one_x) as one_x_image:
                self.assertEqual(one_x_image.size, (660, 400))
            with Image.open(two_x) as two_x_image:
                self.assertEqual(two_x_image.size, (1320, 800))
            with Image.open(expected) as combined:
                self.assertEqual(combined.n_frames, 2)
                self.assertEqual(combined.size, (660, 400))
                combined.seek(1)
                self.assertEqual(combined.size, (1320, 800))

    @unittest.skipUnless(
        platform.system() == "Darwin" and shutil.which("tiffutil"),
        "requires the real macOS tiffutil",
    )
    def test_real_tiffutil_output_contains_one_x_and_two_x_representations(self):
        background = _background_module()
        with tempfile.TemporaryDirectory() as temporary:
            result = background.build_background_tiff(Path(temporary))
            with Image.open(result) as combined:
                self.assertEqual(combined.n_frames, 2)
                self.assertEqual(combined.size, (660, 400))
                combined.seek(1)
                self.assertEqual(combined.size, (1320, 800))


if __name__ == "__main__":
    unittest.main()
