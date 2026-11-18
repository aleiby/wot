#!/usr/bin/env python3
"""Specs for generate_setup_backdrop.py and the include it committed.

The committed include must be exactly what the generator writes from the
committed PNG: an image updated without regenerating (or an include edited by
hand) fails here rather than shipping a backdrop nobody reviewed.
"""
from __future__ import annotations

import hashlib
import re
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_setup_backdrop as backdrop  # noqa: E402

PNG = backdrop.REPO / backdrop.PNG_RELATIVE
INCLUDE = backdrop.REPO / backdrop.OUTPUT_RELATIVE


def _chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))


def _png(width: int, height: int) -> bytes:
    """A minimal valid RGBA PNG of the given size (every pixel zero)."""
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    rows = b"".join(b"\0" + b"\0" * (4 * width) for _ in range(height))
    return (backdrop.PNG_SIGNATURE + _chunk(b"IHDR", header)
            + _chunk(b"IDAT", zlib.compress(rows)) + _chunk(b"IEND", b""))


def _embedded_bytes(text: str) -> bytes:
    body = text.split("static const uint8 Png[] = {", 1)[1].split("};", 1)[0]
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", body))


class CommittedIncludeTests(unittest.TestCase):
    def test_committed_include_matches_committed_png(self):
        self.assertEqual(backdrop.render(PNG.read_bytes()), INCLUDE.read_bytes(),
                         "regenerate with Tools/build/generate_setup_backdrop.py")

    def test_committed_include_embeds_the_png_bytes_size_and_digest(self):
        data = PNG.read_bytes()
        text = INCLUDE.read_bytes().decode("ascii")
        self.assertEqual(_embedded_bytes(text), data)
        self.assertIn(f"static constexpr int32 PngSize = {len(data)};", text)
        self.assertIn(f"// SHA-256: {hashlib.sha256(data).hexdigest()}", text)

    def test_committed_png_is_a_backdrop_the_game_accepts(self):
        data = PNG.read_bytes()
        backdrop.validate(data)
        self.assertEqual((backdrop.WIDTH, backdrop.HEIGHT), backdrop.png_size(data))

    def test_check_mode_accepts_the_committed_pair(self):
        self.assertEqual(0, backdrop.main(
            ["--check", "--png", str(PNG), "--output", str(INCLUDE)]))


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.png = self.root / "backdrop.png"
        self.output = self.root / "out" / "backdrop.inl"

    def test_render_is_deterministic_lf_ascii_with_bounded_lines(self):
        data = _png(backdrop.WIDTH, backdrop.HEIGHT)
        first = backdrop.render(data)
        self.assertEqual(first, backdrop.render(data))
        self.assertNotIn(b"\r", first)
        self.assertTrue(first.endswith(b"}\n"))
        self.assertLessEqual(max(len(line) for line in first.split(b"\n")), 100)
        self.assertEqual(_embedded_bytes(first.decode("ascii")), data)

    def test_generate_then_check_round_trips(self):
        self.png.write_bytes(_png(backdrop.WIDTH, backdrop.HEIGHT))
        arguments = ["--png", str(self.png), "--output", str(self.output)]
        self.assertEqual(0, backdrop.main(arguments))
        self.assertEqual(0, backdrop.main(["--check", *arguments]))

    def test_check_fails_on_a_stale_or_missing_include(self):
        arguments = ["--check", "--png", str(self.png), "--output", str(self.output)]
        self.png.write_bytes(_png(backdrop.WIDTH, backdrop.HEIGHT))
        self.assertEqual(1, backdrop.main(arguments))
        self.output.parent.mkdir(parents=True)
        self.output.write_bytes(backdrop.render(self.png.read_bytes()).replace(
            b"0x89", b"0x88", 1))
        self.assertEqual(1, backdrop.main(arguments))

    def test_a_png_the_game_would_refuse_is_rejected(self):
        with self.assertRaisesRegex(backdrop.BackdropError, "must be 640x360"):
            backdrop.render(_png(320, 180))
        with self.assertRaisesRegex(backdrop.BackdropError, "not a PNG"):
            backdrop.render(b"GIF89a" + b"\0" * 40)
        with self.assertRaisesRegex(backdrop.BackdropError, "empty"):
            backdrop.render(b"")
        oversized = _png(backdrop.WIDTH, backdrop.HEIGHT) + b"\0" * backdrop.MAX_BYTES
        with self.assertRaisesRegex(backdrop.BackdropError, "refuses more than"):
            backdrop.render(oversized)
        self.png.write_bytes(_png(4, 4))
        self.assertEqual(2, backdrop.main(
            ["--png", str(self.png), "--output", str(self.output)]))
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
