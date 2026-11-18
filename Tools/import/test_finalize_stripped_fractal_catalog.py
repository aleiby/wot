#!/usr/bin/env python3
"""Stdlib checks for the guarded fractal-catalog finalizer."""
from __future__ import annotations

import sys
import runpy
import unittest
import json
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stripped_fractal_catalog as finalizer  # noqa: E402


class Record:
    def __init__(self, *, source="", source_is_fractal=False, seed=b"seed"):
        self.values = {
            "source_texture_ref": source,
            "source_is_fractal": source_is_fractal,
            "object_path": "Fixture.Group.Texture",
            **{prop: list(seed) for prop in finalizer.SEED_PROPERTIES},
        }

    def get_editor_property(self, name):
        return self.values[name]

    def set_editor_property(self, name, value):
        self.values[name] = value


class Catalog:
    def __init__(self, records):
        self.records = records

    def get_editor_property(self, name):
        if name != "textures":
            raise KeyError(name)
        return self.records

    def set_editor_property(self, name, value):
        if name != "textures":
            raise KeyError(name)
        self.records = value


class FinalizeStrippedFractalCatalogTests(unittest.TestCase):
    def test_retail_source_record_is_emptied(self):
        record = Record(source="MordethT.SurfaceWall.BlokWall4060")

        records, count, byte_count = finalizer.strip_retail_source_seeds(
            [record])

        self.assertEqual(records, [record])
        self.assertEqual(count, 1)
        self.assertEqual(byte_count, 4 * len(b"seed"))
        for prop in finalizer.SEED_PROPERTIES:
            self.assertEqual(record.get_editor_property(prop), [])
        finalizer.verify_retail_source_seeds_empty(records)

    def test_baker_and_live_fractal_seeds_are_preserved(self):
        baker = Record(source="")
        live = Record(source="WarrenT.Fire.Live", source_is_fractal=True)

        records, count, byte_count = finalizer.strip_retail_source_seeds(
            [baker, live])

        self.assertEqual(count, 0)
        self.assertEqual(byte_count, 0)
        for record in records:
            for prop in finalizer.SEED_PROPERTIES:
                self.assertEqual(record.get_editor_property(prop), list(b"seed"))

    def test_verify_rejects_a_surviving_retail_seed(self):
        with self.assertRaisesRegex(RuntimeError, "seed survived"):
            finalizer.verify_retail_source_seeds_empty(
                [Record(source="ScottT.Water.S_RainTex03")])

    def test_commandlet_main_saves_and_emits_its_own_marker(self):
        catalog = Catalog([
            Record(source="MordethT.SurfaceWall.BlokWall4060"),
            Record(source=""),
        ])
        logs = []
        with tempfile.TemporaryDirectory() as saved:
            fake_unreal = SimpleNamespace(
                load_asset=lambda path: catalog,
                EditorAssetLibrary=SimpleNamespace(save_asset=lambda path: True),
                Paths=SimpleNamespace(project_saved_dir=lambda: saved),
                log=logs.append,
            )

            with mock.patch.dict(sys.modules, {"unreal": fake_unreal}):
                runpy.run_path(
                    str(Path(__file__).with_name(
                        "finalize_stripped_fractal_catalog.py")),
                    run_name="unreal_commandlet_fixture")
            receipt = json.loads((
                Path(saved) / "ImportReceipts" /
                "finalize-stripped-fractal-catalog.json").read_text())

        for prop in finalizer.SEED_PROPERTIES:
            self.assertEqual(catalog.records[0].get_editor_property(prop), [])
            self.assertEqual(
                catalog.records[1].get_editor_property(prop), list(b"seed"))
        self.assertTrue(any(
            line.startswith("FINALIZE-STRIPPED-FRACTAL-CATALOG OK ")
            for line in logs))
        self.assertEqual(receipt["verdict"], "pass")
        self.assertEqual(receipt["retail_source_records"], 1)
        self.assertEqual(receipt["seed_bytes_removed"], 16)


if __name__ == "__main__":
    unittest.main()
