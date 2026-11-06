"""Stdlib specification for deterministic curved-surface subdivision."""
import json
import math
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import curvy
from curvy import eval_curvy, plan_curvy
from ue3d import MTT_FLAT, MTT_PLACEHOLDER
from wot_config import original_dir


def _triangle(indices, *, triangle_type=0, uvs=None, texture=0):
    return {
        "idx": tuple(indices),
        "uvs": tuple(uvs or ((0, 0), (255, 0), (0, 255))),
        "type": triangle_type,
        "tex": texture,
    }


def _octahedron():
    positions = [
        (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
        (-1.0, 0.0, 0.0), (0.0, -1.0, 0.0),
        (0.0, 0.0, 1.0), (0.0, 0.0, -1.0),
    ]
    triangles = [
        _triangle(face) for face in (
            (0, 1, 4), (1, 2, 4), (2, 3, 4), (3, 0, 4),
            (1, 0, 5), (2, 1, 5), (3, 2, 5), (0, 3, 5),
        )
    ]
    return positions, list(positions), triangles


class CurvySubdivisionTests(unittest.TestCase):
    def test_reference_inputs_follow_wot_reference_root(self):
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(
                os.environ, {"WOT_REFERENCE_ROOT": temporary}):
            (Path(temporary) / "original").mkdir()
            (Path(temporary) / "exports" / "script").mkdir(parents=True)
            original, scripts = curvy.reference_inputs()
        self.assertEqual(original, Path(temporary).resolve() / "original")
        self.assertEqual(scripts, Path(temporary).resolve() / "exports" / "script")

    def test_normalization_matches_fvector_reciprocal_multiply_order(self):
        vector = (123.0, 456.0, 789.0)
        length = math.sqrt(sum(component * component for component in vector))
        reciprocal = 1.0 / length

        self.assertEqual(
            curvy._normalized(vector),
            tuple(component * reciprocal for component in vector))

    def test_base_normals_match_fvector_reciprocal_multiply_order(self):
        positions = [
            (0.0, 0.0, 0.0),
            (123.0, 0.0, 0.0),
            (0.0, 456.0, 789.0),
            (-222.0, 333.0, -444.0),
        ]
        triangles = [(0, 1, 2), (0, 3, 1), (0, 2, 3), (1, 3, 2)]

        normals = curvy.smooth_normals(positions, triangles)

        self.assertEqual(normals[0], (
            -0.41446921548650584,
            -0.9089416212904394,
            0.045170770415256245,
        ))

    def test_octahedron_counts_and_hand_computed_midpoint(self):
        positions, normals, triangles = _octahedron()
        level_one = plan_curvy(triangles, 1)
        self.assertEqual(level_one.vertex_count, 18)  # V + E = 6 + 12
        self.assertEqual(len(level_one.triangles), 32)
        level_two = plan_curvy(triangles, 2)
        self.assertEqual(level_two.vertex_count, 66)
        self.assertEqual(len(level_two.triangles), 128)

        evaluated_positions, evaluated_normals = eval_curvy(
            level_one, positions, normals)
        midpoint = level_one.base_vertex_count + level_one.parents.index((0, 1))
        self.assertAlmostEqual(evaluated_positions[midpoint][0], 0.65, places=7)
        self.assertAlmostEqual(evaluated_positions[midpoint][1], 0.65, places=7)
        self.assertEqual(evaluated_positions[midpoint][2], 0.0)
        expected = math.sqrt(0.5)
        self.assertAlmostEqual(evaluated_normals[midpoint][0], expected, places=7)
        self.assertAlmostEqual(evaluated_normals[midpoint][1], expected, places=7)
        self.assertEqual(evaluated_normals[midpoint][2], 0.0)

    def test_each_active_level_quadruples_triangles(self):
        _positions, _normals, triangles = _octahedron()
        for levels in range(3):
            plan = plan_curvy(triangles, levels)
            self.assertEqual(len(plan.triangles), len(triangles) * 4 ** levels)

    def test_flat_and_placeholder_triangles_remain_unsplit(self):
        triangles = [
            _triangle((0, 1, 2), triangle_type=MTT_FLAT),
            _triangle((0, 2, 3), triangle_type=MTT_PLACEHOLDER),
            _triangle((0, 3, 1)),
        ]
        plan = plan_curvy(triangles, 1)
        self.assertEqual(len(plan.triangles), 6)
        self.assertEqual(plan.triangles[0]["idx"], (0, 1, 2))
        self.assertEqual(plan.triangles[0]["type"] & MTT_FLAT, MTT_FLAT)
        self.assertEqual(plan.triangles[1]["idx"], (0, 2, 3))
        self.assertEqual(plan.triangles[1]["type"] & 0x0f, MTT_PLACEHOLDER)

    def test_plan_is_deterministic_and_inherits_material_flags(self):
        _positions, _normals, triangles = _octahedron()
        triangles[0]["type"] = 0x90
        triangles[0]["tex"] = 7
        first = plan_curvy(triangles, 2)
        second = plan_curvy(triangles, 2)
        self.assertEqual(first, second)
        self.assertEqual(first.parents[:3], ((0, 1), (1, 4), (0, 4)))
        for child in first.triangles[:4]:
            self.assertEqual(child["type"], 0x90)
            self.assertEqual(child["tex"], 7)

    def test_uv_splits_share_one_position_midpoint_without_cracks(self):
        triangles = [
            _triangle((0, 1, 2), uvs=((0, 0), (255, 0), (0, 255))),
            _triangle((1, 0, 3), uvs=((32, 64), (224, 64), (255, 255))),
        ]
        plan = plan_curvy(triangles, 1)
        shared_index = plan.base_vertex_count + plan.parents.index((0, 1))
        occurrences = [
            uv for triangle in plan.triangles
            for index, uv in zip(triangle["idx"], triangle["uvs"])
            if index == shared_index
        ]
        self.assertGreaterEqual(len(occurrences), 4)
        self.assertIn((127.5, 0.0), occurrences)
        self.assertIn((128.0, 64.0), occurrences)
        self.assertEqual(plan.parents.count((0, 1)), 1)


class CurvyRetailHashTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        retail_root = original_dir(required=False)
        if not any((retail_root / "System").glob("*.u")):
            raise unittest.SkipTest("retail reference packages are unavailable")
        intermediate = Path(__file__).resolve().parents[2] / "Intermediate"
        intermediate.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(
            prefix="TestCurvyHashes-", dir=str(intermediate))
        cls.output = Path(cls.temporary.name) / "curvy_hashes.json"
        curvy.main(["--write-hashes", str(cls.output)])
        cls.payload = cls.output.read_bytes()
        cls.document = json.loads(cls.payload.decode("ascii"))

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temporary"):
            cls.temporary.cleanup()

    def test_writer_is_canonical_and_matches_committed_fixture(self):
        canonical = json.dumps(
            self.document, sort_keys=True, separators=(",", ":"),
            ensure_ascii=True).encode("ascii")
        self.assertEqual(self.payload, canonical)
        self.assertFalse(self.payload.endswith(b"\n"))
        committed = Path(__file__).with_name("curvy_hashes.json").read_bytes()
        self.assertEqual(self.payload, committed)

    def test_retail_hashes_cover_every_curvy_mesh_and_runtime_shield_value(self):
        hashes = self.document["meshes"]
        self.assertEqual(set(hashes), {
            "AMApearl",
            "AngrealShieldPickup",
        })
        self.assertEqual({name: len(frames) for name, frames in hashes.items()}, {
            "AMApearl": 1,
            "AngrealShieldPickup": 1,
        })
        self.assertEqual(
            hashes["AngrealShieldPickup"][0],
            "51a3dc02656a433c5a8eea97425660ceee48488d01accdb3b3bf47d441bdcb29")


if __name__ == "__main__":
    unittest.main()
