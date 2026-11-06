import importlib.util
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image


SCRIPT = Path(__file__).with_name("compare_lightmap_atlas.py")


class CompareLightmapAtlasTests(unittest.TestCase):
    def _module(self):
        self.assertTrue(SCRIPT.exists(), "supported atlas comparator is missing")
        spec = importlib.util.spec_from_file_location(
            "compare_lightmap_atlas", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_synthetic_bgra_exact_and_first_surface_difference(self):
        module = self._module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            oracle = root / "synthetic.png"
            rgba = np.array([[[10, 20, 30, 255], [40, 50, 60, 255]]],
                            dtype=np.uint8)
            Image.fromarray(rgba, "RGBA").save(oracle)
            native = root / "synthetic.bgra"
            native.write_bytes(rgba[:, :, [2, 1, 0, 3]].tobytes())
            metadata = {"surfs": {
                "7": {"x": 0, "y": 0, "w": 1, "h": 1},
                "9": {"x": 1, "y": 0, "w": 1, "h": 1},
            }}

            exact = module.compare_atlas(native, oracle, metadata)
            self.assertTrue(exact["exact"])
            self.assertEqual(exact["changed_pixels"], 0)

            changed = bytearray(native.read_bytes())
            changed[4 + 2] = 41  # second pixel's R byte in BGRA order
            native.write_bytes(changed)
            report = module.compare_atlas(native, oracle, metadata)
            self.assertFalse(report["exact"])
            self.assertEqual(report["changed_pixels"], 1)
            self.assertEqual(report["first_difference"]["surface"], 9)
            self.assertEqual(report["first_difference"]["native_rgba"],
                             [41, 50, 60, 255])
            self.assertEqual(report["first_difference"]["oracle_rgba"],
                             [40, 50, 60, 255])


if __name__ == "__main__":
    unittest.main()
