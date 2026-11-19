"""Room reverb in zones.py's ambient_zones rows, from the real retail maps.

Every row carries its zone actor's ZoneInfo reverb properties (bReverbZone,
MasterGain, CutoffHz, Delay[6], Gain[6]; WOTPlayer.uc LogReverb names the
set): the value the actor authors, else its class's default. The retail
ZoneInfo class defaults are MasterGain 100, CutoffHz 6000, Delay (20,34,0,0,0,0),
Gain (150,70,0,0,0,0), and a zone is not a reverb zone unless it says so.
The reference maps come from WOT_REFERENCE_ROOT; without them the real-map
tests skip.
"""
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import zones  # noqa: E402
from ue1pkg import Package, PayloadReader, read_property_tags  # noqa: E402
from wot_config import original_dir  # noqa: E402

MULTIPLAYER_MAPS = (
    "Arena_01", "Arena_02", "Arena_03", "Arena_04", "Arena_05", "Arena_06",
    "Arena_07", "Arena_08", "Arena_09", "Arena_11", "Arena_12",
    "Battle2_01", "Battle2_02", "Battle2_03", "Battle2_04", "Battle2_05",
    "Battle3_01", "Battle3_02", "Battle4_01")

RETAIL_DEFAULTS = dict(reverb_zone=False, master_gain=100, cutoff_hz=6000,
                       delay=[20, 34, 0, 0, 0, 0], gain=[150, 70, 0, 0, 0, 0])

REVERB_NAMES = {"breverbzone", "mastergain", "cutoffhz", "delay", "gain"}


def _map(name):
    return original_dir(required=False) / "Maps" / f"{name}.wot"


def _have_reference():
    root = original_dir(required=False)
    return (root / "System" / "Core.u").is_file() and _map("Arena_01").is_file()


class _EveryPackage:
    """Texture manifests are irrelevant to the reverb rows."""
    def __contains__(self, _name):
        return True


def _extract(name):
    with mock.patch.object(zones, "_texture_frictions",
                           return_value=({}, {}, _EveryPackage())):
        return zones.extract(_map(name), None)


def _actor(name, actor):
    package = Package(_map(name))
    export = next(e for e in package.exports if e["name"] == actor)
    authored = {tag["name"].casefold()
                for tag in read_property_tags(PayloadReader(package, export))}
    reverb = zones.read_reverb(package, export,
                               zones.reverb_resolver(_map(name)))
    return reverb, authored


@unittest.skipUnless(_have_reference(),
                     "retail reference maps absent (WOT_REFERENCE_ROOT)")
class RealMapReverbTests(unittest.TestCase):
    def test_unauthored_properties_come_from_the_class_defaults(self):
        # Arena_01's zone 1 (WOTZoneInfo9) authors its six pairs and the flag,
        # but neither MasterGain nor CutoffHz.
        reverb, authored = _actor("Arena_01", "WOTZoneInfo9")
        self.assertNotIn("mastergain", authored)
        self.assertNotIn("cutoffhz", authored)
        self.assertEqual(reverb, dict(
            reverb_zone=True, master_gain=100, cutoff_hz=6000,
            delay=[19, 48, 94, 124, 151, 207], gain=[54, 87, 40, 44, 18, 7]))

    def test_a_zone_authoring_nothing_takes_every_retail_default(self):
        reverb, authored = _actor("Arena_01", "LevelInfo0")
        self.assertFalse(authored & REVERB_NAMES)
        self.assertEqual(reverb, RETAIL_DEFAULTS)

    def test_authored_numbers_without_the_flag_are_not_a_reverb_zone(self):
        reverb, _ = _actor("Arena_08", "WaterZone0")
        self.assertFalse(reverb["reverb_zone"])
        self.assertEqual((reverb["master_gain"], reverb["delay"][:2]), (90, [30, 50]))
        reverb, _ = _actor("Arena_08", "ZoneInfo0")
        self.assertTrue(reverb["reverb_zone"])
        self.assertEqual((reverb["master_gain"], reverb["cutoff_hz"]), (90, 8000))

    def test_rows_follow_each_compiled_zones_actor(self):
        rows = _extract("Arena_01")["ambient_zones"]
        self.assertEqual(len(rows), 11)
        self.assertEqual([row["zone"] for row in rows if row["reverb_zone"]],
                         [1, 6, 7, 9])
        self.assertEqual(rows[0]["actor"], "LevelInfo0")
        self.assertEqual({key: rows[0][key] for key in RETAIL_DEFAULTS},
                         RETAIL_DEFAULTS)
        self.assertEqual(rows[6]["delay"], [5, 14, 24, 33, 51, 118])
        self.assertEqual(rows[6]["gain"], [153, 31, 44, 14, 7, 1])
        table = zones.zone_reverb_rows(rows)
        self.assertEqual(len(table), 11)
        self.assertEqual(table[9], {key: rows[9][key] for key in RETAIL_DEFAULTS})

    def test_reverb_zone_census_of_the_multiplayer_maps(self):
        expected = dict(zip(MULTIPLAYER_MAPS, (
            [1, 6, 7, 9], [], [1, 2], [], [1, 2, 3, 4, 5], [0], [2], [2],
            [1, 2, 3, 4, 5, 6], [1, 3], [1, 4, 7],
            [1, 12, 17, 18, 19, 20], [], [], [],
            [1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 19, 20, 21, 33, 34],
            [1, 2, 3, 14, 15, 16, 17],
            [1, 2, 3, 4, 14, 15, 16, 17, 18, 19, 21],
            [1, 2, 3, 4, 6, 8, 10, 13, 14, 15, 16, 30, 31])))
        for name in MULTIPLAYER_MAPS:
            with self.subTest(map=name):
                rows = _extract(name)["ambient_zones"]
                self.assertEqual(
                    [row["zone"] for row in rows if row["reverb_zone"]],
                    expected[name])
                if name == "Battle2_05":
                    # Zone 0 follows the first LevelInfo, not the stray
                    # second one that authors a reverb.
                    self.assertEqual(rows[0]["actor"], "LevelInfo0")
                if name in ("Arena_07", "Arena_08"):
                    self.assertEqual((rows[2]["master_gain"], rows[2]["cutoff_hz"]),
                                     (90, 8000))


class ZoneReverbRowsTests(unittest.TestCase):
    def test_an_export_without_reverb_fields_gives_no_rows(self):
        self.assertEqual(zones.zone_reverb_rows([{"zone": 0}]), [])
        self.assertEqual(zones.zone_reverb_rows([]), [])

    def test_rows_are_indexed_by_zone_number(self):
        row = dict(zone=2, reverb_zone=True, master_gain=90, cutoff_hz=8000,
                   delay=[30, 50, 0, 0, 0, 0], gain=[44, 140, 0, 0, 0, 0])
        table = zones.zone_reverb_rows([row])
        self.assertEqual(len(table), 3)
        self.assertFalse(table[0]["reverb_zone"])
        self.assertEqual(table[2]["delay"], [30, 50, 0, 0, 0, 0])

    def test_a_row_without_six_pairs_is_refused(self):
        row = dict(zone=0, reverb_zone=True, master_gain=90, cutoff_hz=8000,
                   delay=[30, 50], gain=[44, 140])
        with self.assertRaises(ValueError):
            zones.zone_reverb_rows([row])


if __name__ == "__main__":
    unittest.main()
