import sys
import struct
import tempfile
import unittest
from pathlib import Path

import numpy as np


sys.path.insert(0, str(Path(__file__).resolve().parent))

import entry_weather_oracle as oracle  # noqa: E402


def _native_overlap_payload():
    identities = (b"Fixture.White", b"Fixture.Red")
    header = b"WOTPBSP\0" + struct.pack(
        "<HHIIIHH6f", 3, 2, 3, 3, 1, 2, 1, *([0.0] * 6))
    section = struct.pack("<HHIIIIB3x", 1, 0, 0, 3, 0, 2, 1) + b"M"
    geometry = bytes(3 * (12 + 12 + 8 + 8) + 3 * 4)
    base = bytes([
        2, 4, 6, 255, 8, 10, 12, 255, 14, 16, 18, 255,
        20, 22, 24, 255, 60, 40, 20, 255, 26, 28, 30, 255,
        32, 34, 36, 255, 38, 42, 44, 255, 46, 48, 50, 255,
    ])
    atlas = struct.pack("<III", 3, 3, len(base)) + base
    trailer_body = bytearray()
    trailer_body.extend(struct.pack("<5I", 1, 1, 2, 2, 0))
    trailer_body.extend(struct.pack("<I", 0))
    trailer_body.extend(struct.pack("<5I2f", 0, 1, 1, 1, 1, 0.0, 0.0))
    light_values = ((255, 0, 255, 1.0), (255, 0, 0, 1.0))
    for identity, (brightness, hue, saturation, level) in zip(
            identities, light_values):
        trailer_body.extend(struct.pack("<HH", len(identity), 0))
        trailer_body.extend(identity)
        trailer_body.extend(struct.pack(
            "<BBBBf", brightness, hue, saturation, 0, level))
    trailer_body.extend(struct.pack("<III", 0, 0, 1))
    trailer_body.extend(b"\x40")
    trailer_body.extend(struct.pack("<III", 1, 0, 1))
    trailer_body.extend(b"\x20")
    return (header + section + geometry + atlas
            + struct.pack("<I", len(trailer_body) + 4) + trailer_body), base


def _native_disjoint_payload(odd_inactive=False):
    identities = (b"Fixture.Red", b"Fixture.Blue")
    header = b"WOTPBSP\0" + struct.pack(
        "<HHIIIHH6f", 3, 2, 3, 3, 1, 2, 1, *([0.0] * 6))
    section = struct.pack("<HHIIIIB3x", 1, 0, 0, 3, 0, 2, 1) + b"M"
    geometry = bytes(3 * (12 + 12 + 8 + 8) + 3 * 4)
    base = bytearray([4, 6, 8, 255] * (7 * 3))
    for y in range(3):
        offset = (y * 7 + 3) * 4
        base[offset:offset + 4] = bytes([33, 35, 37, 17])
    if odd_inactive:
        base[(1 * 7 + 5) * 4] = 5
    atlas = struct.pack("<III", 7, 3, len(base)) + base
    trailer_body = bytearray()
    trailer_body.extend(struct.pack("<5I", 1, 2, 2, 2, 0))
    trailer_body.extend(struct.pack("<I", 0))
    trailer_body.extend(struct.pack("<5I2f", 0, 1, 1, 1, 1, 0.0, 0.0))
    trailer_body.extend(struct.pack("<5I2f", 1, 5, 1, 1, 1, 0.0, 0.0))
    light_values = ((255, 0, 0, 1.0), (255, 170, 0, 1.0))
    for identity, (brightness, hue, saturation, level) in zip(
            identities, light_values):
        trailer_body.extend(struct.pack("<HH", len(identity), 0))
        trailer_body.extend(identity)
        trailer_body.extend(struct.pack(
            "<BBBBf", brightness, hue, saturation, 0, level))
    trailer_body.extend(struct.pack("<III", 0, 0, 1))
    trailer_body.extend(b"\x40")
    trailer_body.extend(struct.pack("<III", 1, 1, 1))
    trailer_body.extend(b"\x20")
    payload = (header + section + geometry + atlas
               + struct.pack("<I", len(trailer_body) + 4) + trailer_body)
    return payload


class EntryWeatherOracleTests(unittest.TestCase):
    def test_one_row_shadow_filter_keeps_retail_asymmetric_184(self):
        actual = oracle.filtered_shadow_bytes(bytes([0xFF]), 1, 1)
        self.assertEqual(actual, bytes([184] * 8))

    def test_palette_merge_uses_fixed_point_floor_and_saturates(self):
        steady = np.array([[[120, 126, 127]]], dtype=np.uint8)
        illumination = np.array([[20]], dtype=np.uint8)
        fixed_color = np.array([32768, 65536, 32768], dtype=np.int64)
        actual = oracle.merge_palette(steady, illumination, fixed_color)
        np.testing.assert_array_equal(
            actual, np.array([[[127, 127, 127]]], dtype=np.uint8))

    def test_palette_merge_floors_unsaturated_fractional_contribution(self):
        steady = np.array([[[10, 20, 30]]], dtype=np.uint8)
        illumination = np.array([[3]], dtype=np.uint8)
        fixed_color = np.array([32768, 0, 65535], dtype=np.int64)
        actual = oracle.merge_palette(steady, illumination, fixed_color)
        np.testing.assert_array_equal(
            actual, np.array([[[11, 20, 32]]], dtype=np.uint8))

    def test_dynamic_palette_uses_original_float32_multiplication_grouping(self):
        actual = oracle.dynamic_fixed_color(
            brightness=240, hue=0, saturation=185,
            level_brightness=1.1, sample=0.25)
        np.testing.assert_array_equal(
            actual, np.array([13931, 10107, 10107], dtype=np.int64))
        contribution = (actual * 214) >> 16
        np.testing.assert_array_equal(
            contribution, np.array([45, 33, 33], dtype=np.int64))

    def test_supplied_samples_compose_all_pixels_and_reset_exactly(self):
        payload, base = _native_overlap_payload()
        self.assertEqual(
            oracle.compose_native_samples(payload, [0.0, 0.0]), base)
        intermediate = bytes([86, 66, 84, 255] * 9)
        self.assertEqual(
            oracle.compose_native_samples(payload, [0.25, 0.75]),
            intermediate)
        full = bytes([164, 144, 176, 255] * 9)
        self.assertEqual(
            oracle.compose_native_samples(payload, [1.0, 1.0]), full)
        self.assertEqual(
            oracle.compose_native_samples(payload, [0.0, 0.0]), base)

    def test_supplied_samples_reject_wrong_count_and_nonfinite_values(self):
        payload, _base = _native_overlap_payload()
        with self.assertRaisesRegex(ValueError, "sample count"):
            oracle.compose_native_samples(payload, [1.0])
        with self.assertRaisesRegex(ValueError, "finite"):
            oracle.compose_native_samples(payload, [0.0, float("nan")])

    def test_paired_inactive_and_zero_sample_rectangles_still_validate_base(self):
        payload = _native_disjoint_payload(odd_inactive=True)
        with self.assertRaisesRegex(ValueError, "RGBA7"):
            oracle.compose_native_samples(payload, [1.0, 0.0])
        with self.assertRaisesRegex(ValueError, "RGBA7"):
            oracle.compose_native_samples(payload, [0.0, 0.0])

    def test_supplied_sample_comparison_checks_every_atlas_pixel(self):
        payload, _base = _native_overlap_payload()
        samples = [0.25, 0.75]
        expected = oracle.compose_native_samples(payload, samples)
        equal = oracle.compare_native_samples(payload, samples, expected)
        self.assertTrue(equal["all_atlas_bytes_equal"])
        self.assertEqual(equal["differing_pixel_count"], 0)
        changed = bytearray(expected)
        changed[0] ^= 2
        unequal = oracle.compare_native_samples(payload, samples, changed)
        self.assertFalse(unequal["all_atlas_bytes_equal"])
        self.assertEqual(unequal["differing_pixel_count"], 1)
        self.assertEqual(unequal["first_differing_pixels"], [[0, 0]])

    def test_sample_case_parser_requires_a_finite_labeled_vector(self):
        self.assertEqual(
            oracle.parse_sample_case("flash=0.25,0,1"),
            ("flash", [0.25, 0.0, 1.0]))
        with self.assertRaisesRegex(ValueError, "LABEL"):
            oracle.parse_sample_case("0.25,0,1")
        with self.assertRaisesRegex(ValueError, "finite"):
            oracle.parse_sample_case("flash=0,nan,1")

    def test_native_payload_comparison_checks_every_pair_byte(self):
        identity = b"WheelOfTime.Light1"
        header = b"WOTPBSP\0" + struct.pack(
            "<HHIIIHH6f", 3, 2, 3, 3, 1, 2, 1, *([0.0] * 6))
        section = struct.pack("<HHIIIIB3x", 1, 0, 0, 3, 0, 2, 1) + b"M"
        geometry = bytes(3 * (12 + 12 + 8 + 8) + 3 * 4)
        atlas = struct.pack("<III", 3, 3, 36) + bytes([20, 40, 60, 255]) * 9
        trailer_body = bytearray()
        trailer_body.extend(struct.pack("<5I", 1, 1, 1, 1, 0))
        trailer_body.extend(struct.pack("<I", 0))
        trailer_body.extend(struct.pack("<5I2f", 0, 1, 1, 1, 1, 0.0, 0.0))
        trailer_body.extend(struct.pack("<HH", len(identity), 0))
        trailer_body.extend(identity)
        trailer_body.extend(struct.pack("<BBBBf", 64, 0, 255, 0, 1.0))
        trailer_body.extend(struct.pack("<III", 0, 0, 1))
        trailer_body.extend(b"\x7b")
        payload = header + section + geometry + atlas \
            + struct.pack("<I", len(trailer_body) + 4) + trailer_body
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pair.bin").write_bytes(b"\x7b")
            manifest = {
                "lights": [{"identity": identity.decode("ascii")}],
                "pairs": [{
                    "light_index": 0,
                    "surface_index": 0,
                    "byte_count": 1,
                    "file": "pair.bin",
                }],
            }
            equal = oracle.compare_native_payload(payload, manifest, root)
            self.assertTrue(equal["all_pair_bytes_equal"])
            self.assertEqual(equal["differing_pairs"], [])
            (root / "pair.bin").write_bytes(b"\x7c")
            unequal = oracle.compare_native_payload(payload, manifest, root)
            self.assertFalse(unequal["all_pair_bytes_equal"])
            self.assertEqual(unequal["differing_pairs"], [[0, 0]])


if __name__ == "__main__":
    unittest.main()
