"""Pins for palette-zero manifests and mesh-only keyed texture variants."""

import json
import os
import unittest
from pathlib import Path
from uuid import uuid4

from mesh_texture_variants import (
    is_keyed_texture_name,
    key0_texture_names,
    keyed_texture_name,
    palette0_rgb,
)
from utx2png import _texture_image


ROOT = Path(__file__).resolve().parents[2]


class MeshTextureVariantTests(unittest.TestCase):
    def test_key0_census_comes_only_from_masked_slots(self):
        temp = ROOT / "Intermediate" / f"TestMeshTextureVariants-{os.getpid()}-{uuid4().hex}"
        temp.mkdir(parents=True)
        try:
            (temp / "One.meta.json").write_text(json.dumps({
                "slot_types": {
                    "JBoxyLamp1_M": {"base_type": 3, "flags": 0},
                    "JHangingLight1": {"base_type": 0, "flags": 0},
                    "Aes_Base_2S": {"base_type": 1, "flags": 0},
                },
            }))
            self.assertEqual(key0_texture_names(temp), {"JBoxyLamp1"})
        finally:
            (temp / "One.meta.json").unlink(missing_ok=True)
            temp.rmdir()

    def test_keyed_image_alpha_is_exactly_palette_index_zero(self):
        texture = {
            "w": 2, "h": 2, "data": bytes((0, 1, 2, 0)), "masked": False,
        }
        palette = [(255, 0, 255, 255), (10, 20, 30, 255),
                   (40, 50, 60, 255)]
        ordinary = _texture_image(texture, palette)
        keyed = _texture_image(texture, palette, key_index_zero=True)
        self.assertEqual([pixel[3] for pixel in ordinary.getdata()], [255] * 4)
        self.assertEqual([pixel[3] for pixel in keyed.getdata()], [0, 255, 255, 0])

    def test_variant_naming_and_palette_validation(self):
        self.assertEqual(keyed_texture_name("JMinion2"), "JMinion2_key0")
        self.assertTrue(is_keyed_texture_name("JMinion2_key0"))
        self.assertFalse(is_keyed_texture_name("JMinion2"))
        self.assertEqual(palette0_rgb({"palette0": [42, 42, 42]}), (42, 42, 42))
        with self.assertRaises(ValueError):
            palette0_rgb({"palette0": [256, 0, 0]})


if __name__ == "__main__":
    unittest.main()
