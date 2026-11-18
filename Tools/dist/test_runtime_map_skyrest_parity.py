"""Sky-rest parity oracle: native v6 trailer vs <Map>_skyrest.json."""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_map_skyrest_parity as parity  # noqa: E402


def trailer(zone=2, rays=100, covered=40, surfaces=((252, 60, 0, 0), (887, 0, 3, 0))):
    body = bytearray(struct.pack("<IIII", zone, rays, covered, len(surfaces)))
    for row in surfaces:
        body += struct.pack("<IIII", *row)
    body += struct.pack("<I", 1)
    raw = b"NatureT__SkyMod442"
    body += struct.pack("<BBH", 1, 0, len(raw)) + raw + struct.pack("<I", 1)
    body += struct.pack("<fff", 0.0, 0.0, 1.0)
    for corner in range(3):
        body += struct.pack("<fffffff", float(corner), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
    return struct.pack("<I", len(body) + 4) + bytes(body)


class SkyRestParityTests(unittest.TestCase):
    development = {"sky_zone": 2, "covered_frac": 0.4,
                   "visible": {"252": 60}, "blockers": {"887": 3}, "fb": {}}

    def test_exact_census_matches(self):
        row = parity.compare_map("Arena_04", trailer(), self.development)
        self.assertEqual(row["status"], "exact", row["problems"])
        self.assertEqual(row["groups"], [("visible", "NatureT__SkyMod442", 1)])

    def test_count_zone_and_fraction_drift_are_mismatches(self):
        for label, native in (
                ("counts", trailer(surfaces=((252, 61, 0, 0), (887, 0, 3, 0)))),
                ("zone", trailer(zone=3)),
                ("fraction", trailer(covered=41))):
            with self.subTest(label=label):
                self.assertEqual(parity.compare_map(
                    "Arena_04", native, self.development)["status"], "mismatch")

    def test_sealed_and_skyless_maps(self):
        sealed = {"sky_zone": 2, "covered_frac": 1.0}
        self.assertEqual(parity.compare_map(
            "Arena_01", trailer(rays=100, covered=100, surfaces=()), sealed)["status"],
            "exact")
        self.assertEqual(parity.compare_map(
            "Arena_01", trailer(rays=100, covered=99, surfaces=()), sealed)["status"],
            "mismatch")
        self.assertEqual(parity.compare_map(
            "Arena_03", None, {"reason": "no sky zones"})["status"], "no-sky")
        self.assertEqual(parity.compare_map(
            "Arena_03", trailer(), {"reason": "no sky zones"})["status"], "mismatch")
        self.assertEqual(parity.compare_map(
            "Arena_04", None, self.development)["status"], "mismatch")


if __name__ == "__main__":
    unittest.main()
