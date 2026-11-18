"""Pure-Python pins for the sky-window master and runtime names."""

import math
import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import sky_window_material as sw  # noqa: E402

REPO = HERE.parents[1]
RUNTIME_HEADER = (REPO / "Source" / "WoT" / "World"
                  / "WoTRuntimeBSPRenderer.h")


class SkyWindowMaterialTests(unittest.TestCase):
    def test_contract_is_opaque_unlit_two_sided_identity_transport(self):
        contract = sw.graph_contract()
        self.assertEqual(contract["material"], "M_WoT_SkyWindow")
        self.assertEqual(contract["blend"], "opaque")
        self.assertEqual(contract["shading_model"], "unlit")
        self.assertTrue(contract["two_sided"])
        self.assertEqual(contract["cube_parameter"], "SkyCube")
        color = (2 / 255, 127 / 255, 251 / 255, 1.0)
        self.assertEqual(sw.transport_color(color), color)

    def test_lookup_is_pixel_world_position_minus_camera(self):
        got = sw.view_direction((13.0, -2.0, 9.0), (10.0, -6.0, -3.0))
        length = math.sqrt(3.0 ** 2 + 4.0 ** 2 + 12.0 ** 2)
        self.assertEqual(got, (3.0 / length, 4.0 / length, 12.0 / length))
        self.assertEqual(sw.view_direction((1, 2, 3), (1, 2, 3)),
                         (0.0, 0.0, 0.0))

    def test_output_root_is_closed(self):
        self.assertEqual(sw.validate_output_root("/Game/WoT/Materials"),
                         "/Game/WoT/Materials")
        with self.assertRaises(ValueError):
            sw.validate_output_root("/Game/Elsewhere")

    def test_names_agree_with_runtime(self):
        header = RUNTIME_HEADER.read_text(encoding="utf-8")
        constants = dict(re.findall(
            r'inline constexpr TCHAR (WoTRuntimeSkyWindow\w+)\[\] =\s*'
            r'TEXT\("([^"]*)"\)', header))
        self.assertEqual(constants["WoTRuntimeSkyWindowMasterPath"],
                         f"{sw.MATERIAL_PATH}.{sw.MATERIAL_NAME}")
        self.assertEqual(constants["WoTRuntimeSkyWindowCubeParameter"],
                         sw.CUBE_PARAMETER)


if __name__ == "__main__":
    unittest.main()
