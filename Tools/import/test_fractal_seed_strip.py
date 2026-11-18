#!/usr/bin/env python3
"""Stdlib checks for the dormant stripped-pak fractal-seed strip gate.

The runtime fractal SEED (retail p8 palette indices + initial bitmaps) is
embedded in DA_WoTFractalCatalog and is the sole remaining stripped-pak Audit-1
leak. The strip is opt-in and off by default, coordinated with the Windows
WoTFractalSubsystem cache-rehydration; these tests pin that gate.
"""
import base64
import sys
import unittest
from unittest import mock

# import_flipbooks makes module-level `unreal` calls; stub the whole module so
# the pure seed-gate helpers can be exercised without an editor.
sys.modules["unreal"] = mock.MagicMock()

import import_flipbooks  # noqa: E402


class FractalSeedStripTests(unittest.TestCase):
    def test_dormant_by_default(self):
        self.assertFalse(import_flipbooks.fractal_seed_stripped({}))

    def test_opt_in_flag_toggles(self):
        env = import_flipbooks.FRACTAL_SEED_STRIP_ENV
        self.assertTrue(import_flipbooks.fractal_seed_stripped({env: "1"}))
        self.assertFalse(import_flipbooks.fractal_seed_stripped({env: "0"}))
        self.assertFalse(import_flipbooks.fractal_seed_stripped({env: ""}))

    def _retail(self):
        raw = base64.b64encode(bytes(range(8))).decode()
        return {"source_texture": "Pkg.Grp.Tex", "source_is_fractal": False,
                "source_indices": raw, "initial_bitmap": raw,
                "glass_indices": raw, "palette_rgba": raw}

    def test_seed_embedded_verbatim_when_dormant(self):
        # Even a retail-sourced record keeps its seed while the flag is off.
        self.assertEqual(
            import_flipbooks.fractal_seed_property_value(
                self._retail(), "source_indices", {}),
            list(bytes(range(8))))

    def test_retail_sourced_seed_omitted_when_opted_in(self):
        env = {import_flipbooks.FRACTAL_SEED_STRIP_ENV: "1"}
        info = self._retail()
        for prop in import_flipbooks._FRACTAL_SEED_PROPS:
            self.assertEqual(
                import_flipbooks.fractal_seed_property_value(info, prop, env),
                [])

    def test_fire_baker_seed_kept_even_when_opted_in(self):
        # No source_texture (fire/wave): baker-generated seed is not a leak and
        # cannot be rehydrated, so it stays embedded even with the flag set.
        env = {import_flipbooks.FRACTAL_SEED_STRIP_ENV: "1"}
        raw = base64.b64encode(bytes(range(8))).decode()
        info = {"source_texture": "", "palette_rgba": raw, "initial_bitmap": raw}
        self.assertEqual(
            import_flipbooks.fractal_seed_property_value(
                info, "initial_bitmap", env),
            list(bytes(range(8))))

    def test_live_fractal_source_seed_kept(self):
        env = {import_flipbooks.FRACTAL_SEED_STRIP_ENV: "1"}
        raw = base64.b64encode(bytes(range(8))).decode()
        info = {"source_texture": "Pkg.Grp.LiveFire",
                "source_is_fractal": True, "source_indices": raw}
        self.assertEqual(
            import_flipbooks.fractal_seed_property_value(
                info, "source_indices", env),
            list(bytes(range(8))))


if __name__ == "__main__":
    unittest.main()
