"""Unit tests for the light-wave response parity oracle."""
from __future__ import annotations

import json
import struct
import tempfile
import unittest
from pathlib import Path

from Tools.dist import runtime_map_lightwave_parity as oracle


def _trailer(surfaces, groups, terms) -> bytes:
    body = bytearray(struct.pack("<I", len(surfaces)))
    for rect in surfaces:
        body += struct.pack("<IIII", *rect)
    body += struct.pack("<I", len(groups))
    for light_type, period, phase, count in groups:
        body += struct.pack("<BBBBI", light_type, period, phase, 0, count)
    body += struct.pack("<I", len(terms))
    for kind, group, effect, count, rects in terms:
        body += struct.pack("<BBBBII", kind, group, effect, 0, count, len(rects))
        for surface, rgb in rects:
            body += struct.pack("<I", surface) + rgb
    return struct.pack("<I", len(body) + 4) + bytes(body)


class LightWaveParityOracleTests(unittest.TestCase):
    WIDTH = 8
    HEIGHT = 8
    PLACEMENT = (1, 1, 2, 2)
    INTERIOR = bytes([10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.native = root / "native"
        self.conversion = root / "conv"
        self.native.mkdir()
        self.conversion.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def _write_reference(self, map_name: str, interior: bytes = INTERIOR,
                         animated: bool = True,
                         light_type: str = "LT_Pulse",
                         period: int = 64, phase: int = 0) -> None:
        from PIL import Image

        x, y, w, h = self.PLACEMENT
        document = {
            "size": [self.WIDTH, self.HEIGHT],
            "surfs": {"0": {"x": x, "y": y, "w": w, "h": h}},
            "animation": {"groups": [], "terms": []},
        }
        if animated:
            document["animation"] = {
                "groups": [{"key": [light_type, period, phase],
                            "light_count": 1}],
                "terms": [{"kind": "waveform",
                           "group": [light_type, period, phase],
                           "light_count": 1,
                           "response_png": f"{map_name}_lmatlas_response_0.png"}],
            }
            pixels = bytearray(self.WIDTH * self.HEIGHT * 3)
            # Interior plus the edge-replicated one-texel gutter
            # (lightmap_atlas.py np.pad mode="edge").
            for row in range(-1, h + 1):
                source_row = min(max(row, 0), h - 1)
                for column in range(-1, w + 1):
                    source_column = min(max(column, 0), w - 1)
                    at = ((y + row) * self.WIDTH + x + column) * 3
                    src = (source_row * w + source_column) * 3
                    pixels[at:at + 3] = interior[src:src + 3]
            Image.frombytes("RGB", (self.WIDTH, self.HEIGHT), bytes(pixels)).save(
                self.conversion / f"{map_name}_lmatlas_response_0.png")
        (self.conversion / f"{map_name}_lmatlas.json").write_text(json.dumps(document))

    def _write_native(self, map_name: str, interior: bytes = INTERIOR,
                      rects=None, light_type: int = 2,
                      period: int = 64, phase: int = 0) -> None:
        if rects is None:
            rects = [(0, interior)]
        (self.native / f"{map_name}.anim").write_bytes(_trailer(
            [self.PLACEMENT], [(light_type, period, phase, 1)],
            [(0, 0, 0, 1, rects)]))

    @staticmethod
    def _blink(period: int, phase: int, time_seconds: float) -> int:
        units = (time_seconds * 35.0 * 65536.0 / (period + 1)
                 + (phase << 8))
        return 0 if int(units) & 1 else 1

    def test_exact_match_reports_rects_and_texels(self):
        self._write_reference("Arena_T")
        self._write_native("Arena_T")
        row = oracle.compare_map("Arena_T", self.native, self.conversion)
        self.assertEqual(row["status"], "exact")
        self.assertEqual((row["rects"], row["texels"], row["zero_surfaces"]), (1, 4, 0))
        self.assertEqual((row["groups"], row["terms"]), (1, 1))

    def test_blink_wire_id_and_period_plus_one_fixture(self):
        period = 7
        self._write_reference(
            "Blink_T", light_type="LT_Blink", period=period)
        self._write_native(
            "Blink_T", light_type=3, period=period)
        row = oracle.compare_map("Blink_T", self.native, self.conversion)
        self.assertEqual(row["status"], "exact")
        unit_seconds = (period + 1) / (35.0 * 65536.0)
        self.assertEqual(self._blink(period, 0, 0.5 * unit_seconds), 1)
        self.assertEqual(self._blink(period, 0, 1.5 * unit_seconds), 0)
        self.assertEqual(self._blink(period, 0, 2.5 * unit_seconds), 1)

    def test_interior_byte_difference_is_a_failure(self):
        self._write_reference("Arena_T")
        wrong = bytearray(self.INTERIOR)
        wrong[7] ^= 1
        self._write_native("Arena_T", bytes(wrong))
        with self.assertRaisesRegex(oracle.ParityError, "differs at byte 7"):
            oracle.compare_map("Arena_T", self.native, self.conversion)

    def test_missing_native_rect_with_nonzero_reference_fails(self):
        self._write_reference("Arena_T")
        self._write_native("Arena_T", rects=[])
        with self.assertRaisesRegex(oracle.ParityError, "absent natively"):
            oracle.compare_map("Arena_T", self.native, self.conversion)

    def test_gutter_must_replicate_the_interior_edge(self):
        from PIL import Image

        self._write_reference("Arena_T")
        self._write_native("Arena_T")
        png = self.conversion / "Arena_T_lmatlas_response_0.png"
        image = Image.open(png).convert("RGB")
        image.putpixel((0, 0), (1, 2, 3))
        image.save(png)
        with self.assertRaisesRegex(oracle.ParityError, "gutter differs"):
            oracle.compare_map("Arena_T", self.native, self.conversion)

    def test_missing_native_trailer_fails_for_an_animated_map(self):
        self._write_reference("Arena_T")
        with self.assertRaisesRegex(oracle.ParityError, "missing"):
            oracle.compare_map("Arena_T", self.native, self.conversion)

    def test_unanimated_map_must_have_no_native_trailer(self):
        self._write_reference("Arena_S", animated=False)
        row = oracle.compare_map("Arena_S", self.native, self.conversion)
        self.assertEqual(row["status"], "unanimated")
        self._write_native("Arena_S")
        with self.assertRaisesRegex(oracle.ParityError, "unanimated"):
            oracle.compare_map("Arena_S", self.native, self.conversion)

    def test_main_writes_the_report_and_returns_failure_count(self):
        self._write_reference("Arena_T")
        self._write_native("Arena_T")
        self._write_reference("Arena_S", animated=False)
        report = Path(self.temp.name) / "report.json"
        status = oracle.main(["--native", str(self.native), "--conversion",
                              str(self.conversion), "--maps", "Arena_T", "Arena_S",
                              "--report", str(report)])
        self.assertEqual(status, 0)
        document = json.loads(report.read_text())
        self.assertEqual(document["format"], "wot-lightwave-response-parity-v1")
        self.assertEqual([row["status"] for row in document["maps"]],
                         ["exact", "unanimated"])
        status = oracle.main(["--native", str(self.native), "--conversion",
                              str(self.conversion), "--maps", "Arena_T", "Arena_X"])
        self.assertEqual(status, 1)


if __name__ == "__main__":
    unittest.main()
