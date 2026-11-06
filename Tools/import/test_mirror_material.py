"""Pure-Python pins for the PF_Mirrored master and its runtime contract."""

import math
import random
import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import mirror_material as mm  # noqa: E402

REPO = HERE.parents[1]
MIRROR_HEADER = REPO / "Source" / "WoT" / "World" / "WoTRuntimeMirror.h"


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _cross(a, b):
    # UE (left-handed, X forward, Y right, Z up): Y = Z ^ X uses this formula.
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _normalize(v):
    length = math.sqrt(_dot(v, v))
    return tuple(c / length for c in v)


def _reflect(v, n):
    d = _dot(v, n)
    return tuple(c - 2.0 * d * k for c, k in zip(v, n))


class MirrorProjectionTests(unittest.TestCase):
    def test_capture_lookup_is_the_horizontally_mirrored_main_view(self):
        # The capture (viewer reflected through the plane, forward/up
        # reflected, same FOV/aspect) sees a plane point P at the main view's
        # pixel flipped left/right: u' = 1 - u, v' = v.  This is the identity
        # the projected lookup relies on; a handedness slip breaks it.
        rng = random.Random(1999)
        for _ in range(200):
            normal = _normalize((rng.uniform(-1, 1), rng.uniform(-1, 1),
                                 rng.uniform(-1, 1)))
            distance = rng.uniform(-3000, 3000)
            eye = tuple(rng.uniform(-2000, 2000) for _ in range(3))
            height = _dot(normal, eye) - distance
            if height < 1.0:
                eye = tuple(e + (1.0 - height + rng.uniform(0, 500)) * k
                            for e, k in zip(eye, normal))
            forward = _normalize((rng.uniform(-1, 1), rng.uniform(-1, 1),
                                  rng.uniform(-1, 1)))
            helper = (0.0, 0.0, 1.0) if abs(forward[2]) < 0.9 else (1.0, 0.0, 0.0)
            right = _normalize(_cross(helper, forward))
            up = _cross(forward, right)
            tan_x = math.tan(math.radians(rng.uniform(60, 110)) / 2)
            tan_y = tan_x * rng.choice((600 / 800, 9 / 16, 1.0))
            # A point on the plane in front of the eye.
            point = None
            for _attempt in range(50):
                u = rng.uniform(-0.9, 0.9)
                v = rng.uniform(-0.9, 0.9)
                ray = _normalize(tuple(f + u * tan_x * r + v * tan_y * w
                                       for f, r, w in zip(forward, right, up)))
                denom = _dot(ray, normal)
                if denom < -1e-3:
                    t = (distance - _dot(normal, eye)) / denom
                    point = tuple(e + t * r for e, r in zip(eye, ray))
                    break
            if point is None:
                continue
            main_u, main_v = mm.project_to_capture(point, eye, forward, right,
                                                   up, (tan_x, tan_y))
            origin = tuple(e - 2.0 * height_c * k for e, k, height_c in
                           zip(eye, normal, [_dot(normal, eye) - distance] * 3))
            f2 = _reflect(forward, normal)
            u2 = _reflect(up, normal)
            y2 = _normalize(_cross(u2, f2))
            z2 = _cross(f2, y2)
            cap_u, cap_v = mm.project_to_capture(point, origin, f2, y2, z2,
                                                 (tan_x, tan_y))
            self.assertAlmostEqual(cap_u, 1.0 - main_u, places=6)
            self.assertAlmostEqual(cap_v, main_v, places=6)

    def test_hlsl_matches_the_python_twin(self):
        hlsl = mm.PROJECTION_HLSL
        for token in ("WP - Origin.xyz", "dot(D, AxisX.xyz)", "1e-4",
                      "0.5 + 0.5 * dot(D, AxisY.xyz) / (X * Tan.x)",
                      "0.5 - 0.5 * dot(D, AxisZ.xyz) / (X * Tan.y)"):
            self.assertIn(token, hlsl)


class MirrorCompositeTests(unittest.TestCase):
    def test_single_tmu_translucent_pair(self):
        # FB1 = Tex + Dst*(1-Tex) (ONE, ONE_MINUS_SRC_COLOR), then the light
        # map pass DST_COLOR/SRC_COLOR doubles FB1 by the half-scale map.
        texel = (0.25, 0.5, 0.75)
        reflection = (0.8, 0.1, 0.4)
        half_map = (0.3, 0.45, 0.5)
        fb1 = tuple(t + d * (1 - t) for t, d in zip(texel, reflection))
        expected = tuple(2.0 * m * c for m, c in zip(half_map, fb1))
        light = tuple(mm.LIGHTMAP_MULTIPLIER * m for m in half_map)
        for got, want in zip(mm.composite(texel, reflection, light), expected):
            self.assertAlmostEqual(got, want, places=12)

    def test_black_reflection_is_the_plain_lit_sheet(self):
        self.assertEqual(mm.composite((0.5, 0.25, 1.0), (0, 0, 0), (1, 1, 1)),
                         (0.5, 0.25, 1.0))


class MirrorContractTests(unittest.TestCase):
    def test_contract(self):
        contract = mm.graph_contract()
        self.assertEqual(contract["material"], "M_WoT_EntryTranslucent_Mirror")
        self.assertEqual(contract["blend"], "opaque")
        self.assertEqual(contract["shading_model"], "unlit")
        self.assertFalse(contract["two_sided"])
        self.assertEqual(contract["lightmap_multiplier"], 2.0)
        self.assertEqual(contract["dynamic_light_default"], 0.0)
        self.assertEqual(contract["texture_defaults"],
                         "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral")

    def test_output_root_is_closed(self):
        self.assertEqual(mm.validate_output_root("/Game/WoT/Materials"),
                         "/Game/WoT/Materials")
        with self.assertRaises(ValueError):
            mm.validate_output_root("/Game/Elsewhere")

    def test_names_agree_with_the_runtime_header(self):
        header = MIRROR_HEADER.read_text(encoding="utf-8")
        constants = dict(re.findall(
            r'inline constexpr TCHAR (WoTRuntimeMirror\w+)\[\] =\s*TEXT\("([^"]*)"\)',
            header))
        self.assertEqual(
            constants["WoTRuntimeMirrorMasterPath"],
            f"{mm.MATERIAL_PATH}.{mm.MATERIAL_NAME}")
        self.assertEqual(constants["WoTRuntimeMirrorReflectionParameter"],
                         mm.REFLECTION_PARAMETER)
        self.assertEqual(
            [constants["WoTRuntimeMirrorOriginParameter"],
             constants["WoTRuntimeMirrorAxisXParameter"],
             constants["WoTRuntimeMirrorAxisYParameter"],
             constants["WoTRuntimeMirrorAxisZParameter"],
             constants["WoTRuntimeMirrorTanParameter"]],
            list(mm.VIEW_VECTORS))


if __name__ == "__main__":
    unittest.main()
