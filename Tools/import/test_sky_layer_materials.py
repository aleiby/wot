"""Offline tests for the stacked child-sky layer masters."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sky_layer_materials as sl  # noqa: E402


class SkyLayerMaterialTests(unittest.TestCase):
    def test_two_passes_equal_the_retail_glide_translucent_blend(self):
        for dst, tex, m in (((0.5, 0.2, 0.9), (0.5, 0.5, 0.5), (1.0, 1.0, 1.0)),
                            ((0.1, 0.7, 0.3), (0.25, 0.9, 0.0), (1.4, 0.6, 1.99))):
            for got, want in zip(sl.two_pass(dst, tex, m),
                                 sl.scene_blend(dst, tex, m)):
                self.assertAlmostEqual(got, want, places=12)

    def test_two_passes_stack_where_scene_color_could_not(self):
        sky = (0.502, 0.502, 0.502)
        tex, m = (0.502, 0.502, 0.502), (1.004, 1.004, 1.004)
        one = sl.two_pass(sky, tex, m)
        two = sl.two_pass(one, tex, m)
        # SceneColor reads the pre-translucency sky for both layers, so the
        # nearer layer would yield `one` again; the passes compound instead.
        self.assertGreater(two[0], one[0] + 0.1)

    def test_specs_name_the_two_cooked_masters(self):
        self.assertEqual(sl.material_specs(), (
            ("M_WoT_EntrySkyModulate", "modulate", "M*(1-Tex)"),
            ("M_WoT_EntrySkyAdd", "additive", "Tex*M")))
        self.assertEqual(sl.SOURCE_NAME, "M_WoT_EntryTranslucent_SceneBlend")
        self.assertEqual(set(sl.PARAMETERS), {
            "Tex", "Lightmap", "PanU", "PanV", "WoTSceneTime", "DynLightEnable"})

    def test_output_root_is_closed(self):
        self.assertEqual(sl.validate_output_root("/Game/WoT/Materials"),
                         "/Game/WoT/Materials")
        with self.assertRaises(ValueError):
            sl.validate_output_root("/Game/Other")


if __name__ == "__main__":
    unittest.main()
