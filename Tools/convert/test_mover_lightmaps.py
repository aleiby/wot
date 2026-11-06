"""Reference coverage for retail static-mover baked lightmaps."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np

import mover_lightmaps as M
from t3d import parse, typed


class _FakePackage:
    def __init__(self, _path):
        self.exports = [
            {"name": "Model1", "cls": 1},
            {"name": "Light1", "cls": 2},
        ]

    def objname(self, ref):
        return {1: "Model", 2: "Light", 3: "Model1", 4: "Light1"}.get(ref)


class MoverLightmapSyntheticTests(unittest.TestCase):
    def test_singular_unlit_retail_poly_uses_constant_base_zone_ambient(self):
        """Battle3_01 DoorMover2 poly 2 has TextureV parallel to Normal.

        Its saved light run is empty, so no spatial light operation needs the
        non-invertible basis. Retail can still contribute the polygon's base
        zone ambient uniformly.
        """
        poly = {
            "base": np.array([1.0, 2.0, 3.0]),
            "normal": np.array([0.0, 0.0, -1.0]),
            "texture_u": np.array([0.0, -1.0, 0.0]),
            "texture_v": np.array([0.0, 0.0, -1.0]),
        }
        lightmap = {
            "data_offset": 0, "pan": (0.0, 0.0, 0.0),
            "uclamp": 2, "vclamp": 2, "uscale": 1.0,
            "vscale": 1.0, "ilightactors": -1,
        }
        package = type("Package", (), {"objname": lambda _self, _ref: None})()
        with patch.object(M, "ambient_seed", return_value=(3, 4, 5)), \
                patch.object(M, "point_zone", return_value=0):
            actual = M._compose_poly(
                package, {"lights": [], "lightbits": np.array([], dtype=np.uint8)},
                {"zones": []}, {}, {}, poly, lightmap, set(), 1.0, {})
        np.testing.assert_array_equal(
            actual, np.tile(np.array([3, 4, 5], dtype=np.uint8), (2, 2, 1)))

    def test_one_static_mover_poly_one_light_matches_hand_derived_rgb7(self):
        """A missing/changed radial, palette, or shadow kernel breaks byte 34.

        Derivation: radius=25, distance=10, diffuse=10/25. UE1's vertically
        edge-replicated three-row walk produces 133 for a one-row shadow map;
        LightSqrt[654]=1.6208408, so the illumination byte is
        round(133*.4*1.6208408)=86. Hue 0/saturation 0
        gives red 0.8213098; brightness 64 makes fixed red
        floor(0.8213098*64/255*65536)=13509. The palette lookup is
        (86*13509)>>16 = 17.
        """
        mover_bsp = {
            "lightmap": [{
                "data_offset": 0, "pan": (0.0, 0.0, 0.0),
                "uclamp": 1, "vclamp": 1, "uscale": 1.0,
                "vscale": 1.0, "ilightactors": 0,
            }],
            "lightbits": np.array([1], dtype=np.uint8),
            "lights": [4, 0],
        }
        polys = [{
            "base": (0.0, 0.0, 0.0), "normal": (1.0, 0.0, 0.0),
            "texture_u": (0.0, 1.0, 0.0),
            "texture_v": (0.0, 0.0, 1.0),
            "vertices": [(0.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                         (0.0, 0.0, 1.0)],
            "flags": 0, "brush_poly": 0,
        }]
        level_bsp = {
            "nodes": [], "node_planes": [], "node_children": [],
            "zones": [],
        }
        actors = [
            {"cls": "LevelInfo", "name": "LevelInfo0",
             "location": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0],
             "props": {"Brightness": "1.0"}},
            {"cls": "Mover", "name": "Mover0",
             "location": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0],
             "props": {"Brush": "Model'Map.Model1'"}},
            {"cls": "Light", "name": "Light1",
             "location": [10.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0],
             "props": {"LightType": "LT_Steady", "LightEffect": "LE_None",
                       "LightBrightness": "64", "LightHue": "0",
                       "LightSaturation": "0", "LightRadius": "0"}},
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            actor_json = Path(temp_dir) / "Synthetic.t3d.json"
            actor_json.write_text(json.dumps({"actors": actors}))
            with patch.object(M, "Package", _FakePackage), \
                    patch.object(M, "decode_model_full",
                                 side_effect=[level_bsp, mover_bsp]), \
                    patch.object(M, "decode_brush_model", return_value=polys):
                maps = M.compose_mover("Synthetic.wot", actor_json, "Mover0")
        self.assertEqual(len(maps), 1)
        np.testing.assert_array_equal(
            maps[0], np.array([[[17, 0, 0]]], dtype=np.uint8))

    def test_pose_transform_matches_runtime_frame_then_actor_transform(self):
        """A wrong UE rotator order/sign or inverse-transpose breaks equality."""
        poly = {
            "base": (13.0, -5.0, 9.0),
            "normal": (0.0, 0.0, 1.0),
            "texture_u": (1.0, 0.0, 0.0),
            "texture_v": (0.0, 1.0, 0.0),
            "vertices": [(13.0, -5.0, 9.0)],
        }
        pose = M.MoverPose(
            location=np.array([100.0, -200.0, 50.0]),
            rotation=np.array([8192.0, 16384.0, 0.0]),
            pre_pivot=np.array([3.0, 2.0, -4.0]),
            main_scale=np.array([2.0, 0.5, 1.5]),
            post_scale=np.array([0.75, 1.25, 2.0]),
        )
        transformed = M.transform_poly(poly, pose)

        # Literal matrix independently transcribed from UE's FRotator axes
        # for Pitch=8192, Yaw=16384, Roll=0 (SinTab exact quarter/eighth
        # turns). This is the actor FTransform used after
        # WoTApplyMoverBrushFrame's mesh-local conjugation.
        rotation = np.array([
            [0.0, -1.0, 0.0],
            [2 ** -0.5, 0.0, -(2 ** -0.5)],
            [2 ** -0.5, 0.0, 2 ** -0.5],
        ])
        local = (np.array(poly["base"]) - pose.pre_pivot) * pose.main_scale
        runtime_mesh = rotation.T @ (pose.post_scale * (rotation @ local))
        runtime_world = pose.location + rotation @ runtime_mesh
        np.testing.assert_allclose(transformed["base"], runtime_world,
                                   rtol=0.0, atol=1.0e-6)
        np.testing.assert_allclose(
            transformed["base"], [102.625, -199.55805826, 105.86143571],
            rtol=0.0, atol=1.0e-6)


class MoverLightmapRetailTests(unittest.TestCase):
    @unittest.skipUnless(
        (Path(__file__).resolve().parents[2]
         / "original/Maps/Battle2_02.wot").is_file(),
        "retail original/ tree is absent")
    def test_battle202_mover6_lit_polys_pin_oracle_texels(self):
        repo = Path(__file__).resolve().parents[2]
        actors = typed(parse(repo / "exports/t3d/Battle2_02.t3d"))
        with tempfile.TemporaryDirectory() as temp_dir:
            actor_json = Path(temp_dir) / "Battle2_02.t3d.json"
            actor_json.write_text(json.dumps({"actors": actors}))
            maps = M.compose_mover(
                repo / "original/Maps/Battle2_02.wot", actor_json, "Mover6")
        samples = {
            2: {(0, 0): [0, 0, 0], (0, 5): [5, 4, 5],
                (1, 4): [21, 18, 18], (1, 5): [21, 18, 18],
                (2, 9): [4, 4, 4]},
            3: {(0, 0): [4, 5, 6], (0, 5): [20, 20, 23],
                (1, 4): [16, 15, 18], (1, 5): [16, 15, 17],
                (2, 9): [0, 1, 1]},
        }
        for poly_index, expected in samples.items():
            self.assertEqual(maps[poly_index].shape, (3, 10, 3))
            actual = {at: maps[poly_index][at].tolist() for at in expected}
            print(f"Battle2_02 Mover6 poly {poly_index} texels: {actual}")
            self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
