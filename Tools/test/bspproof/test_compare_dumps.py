#!/usr/bin/env python3
"""Stdlib specs for the packaged BSP proof plan and comparator."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import compare_dumps as compare
import make_plan


def _mesh(path: str, slots: list[str], *, index_order=(0, 1, 2),
          position_delta=0.0, normal=(0.0, 0.0, 1.0),
          tangent=(1.0, 0.0, 0.0), handedness=1.0,
          collision=True):
    positions, normals, tangents, signs = [], [], [], []
    uv0, uv1, indices, sections = [], [], [], []
    for slot_index, _slot in enumerate(slots):
        base = len(positions)
        x = float(slot_index * 10)
        positions.extend([
            [x + position_delta, 0.0, 0.0],
            [x + 1.0 + position_delta, 0.0, 0.0],
            [x + position_delta, 1.0, 0.0],
        ])
        normals.extend([list(normal)] * 3)
        tangents.extend([list(tangent)] * 3)
        signs.extend([handedness] * 3)
        uv0.extend([[0.0, 0.0], [1.0, 0.0], [0.0, 1.0]])
        uv1.extend([[0.25, 0.25], [0.5, 0.25], [0.25, 0.5]])
        first = len(indices)
        indices.extend(base + corner for corner in index_order)
        sections.append({
            "material_index": slot_index,
            "first_index": first,
            "num_triangles": 1,
            "min_vertex_index": base,
            "max_vertex_index": base + 2,
            "enable_collision": collision,
        })
    minimum = [min(point[i] for point in positions) for i in range(3)]
    maximum = [max(point[i] for point in positions) for i in range(3)]
    center = [(minimum[i] + maximum[i]) / 2.0 for i in range(3)]
    radius = sum(((maximum[i] - minimum[i]) / 2.0) ** 2
                 for i in range(3)) ** .5
    return {
        "object_path": path,
        "num_lods": 1,
        "uv_channels": 2,
        "allow_cpu_access": True,
        "never_stream": True,
        "nanite_valid": False,
        "cpu_readable": True,
        "materials": [
            {"slot_name": slot, "material_path": f"/Game/Test/{slot}.{slot}"}
            for slot in slots
        ],
        "sections": sections,
        "positions": positions,
        "normals": normals,
        "tangent_x": tangents,
        "binormal_sign": signs,
        "uv0": uv0,
        "uv1": uv1,
        "indices": indices,
        "bounds": {
            "box_min": minimum,
            "box_max": maximum,
            "sphere_origin": center,
            "sphere_radius": radius,
        },
        "triangle_count": len(indices) // 3,
    }


def _render_fixture():
    main_slots = list(compare.ARENA07_MAIN_SLOTS)
    portal_slots = list(compare.ARENA07_PORTAL_SLOTS)
    main = _mesh(compare.ARENA07_MAIN_OBJECT, main_slots)
    # Interchange reference sections retain true; reference no-collision is
    # actor-level and intentionally differs from B's payload bits.
    portal_base = _mesh(
        "/Game/WoT/Maps/Geo/Arena_07_portals/Arena_07_portals.Arena_07_portals",
        [portal_slots[1]], position_delta=10.0, collision=True)
    portal_split = _mesh(
        "/Game/WoT/Maps/Geo/Arena_07_portals/"
        "Arena_07_portals_Fractal_000.Arena_07_portals_Fractal_000",
        [portal_slots[0]], collision=True)
    return (
        {"role": "main", "meshes": [main]},
        {"role": "portal", "meshes": [portal_base, portal_split]},
        {"role": "main", "meshes": [
            _mesh(compare.ARENA07_MAIN_OBJECT, main_slots)]},
        {"role": "portal", "meshes": [
            _mesh(compare.ARENA07_PORTAL_OBJECT, [portal_slots[1]],
                  position_delta=10.0, collision=False),
            _mesh(portal_split["object_path"], [portal_slots[0]],
                  collision=False),
        ]},
    )


def _hit(identifier, blocking=True):
    record = {"id": identifier, "kind": "line", "blocking_hit": blocking}
    if blocking:
        record.update({
            "actor": "Arena_07", "component": "StaticMeshComponent0",
            "material_slot": compare.ARENA07_MAIN_SLOTS[0],
            "impact_point": [0.0, 0.0, 1.0],
            "impact_normal": [0.0, 0.0, 1.0], "distance": 100.0,
        })
    return record


def _collision_fixture(plan):
    probes = []
    for index, ray in enumerate(plan["floor_rays"]):
        probes.append(_hit(ray["id"], index < 80))
    probes.extend(_hit(ray["id"]) for ray in plan["wall_rays"])
    probes.append(_hit("start_000_capsule"))
    probes.extend(_hit(f"start_000_radial_{index}", False)
                  for index in range(plan["player_start_radial_count"]))
    projectiles = []
    for shot in plan["projectiles"]:
        blocking = bool(shot["expect_block"])
        record = {
            "id": shot["id"], "portal_directed": shot["portal_directed"],
            "blocking_hit": blocking,
        }
        if blocking:
            record.update({
                "impact_point": [1.0, 2.0, 3.0],
                "impact_normal": [0.0, 1.0, 0.0],
                "actor": "Arena_07", "component": "StaticMeshComponent0",
                "material_slot": compare.ARENA07_MAIN_SLOTS[0],
            })
        projectiles.append(record)
    return {
        "probes": probes,
        "settles": [{"id": "start_000", "settled_z": 42.0,
                     "movement_mode": "Walking", "fall_through": False}],
        "projectiles": projectiles,
    }


def _seeking_fixture():
    return {
        "id": "seeking", "los_blocked": True,
        "side": "left", "side_sign": 1,
        "completion_reason": "reached", "hit_actor_class": "",
        "trajectory": [[1.0, 0.0, 0.0], [10.0, 0.0, 1.0]],
        "reached_endpoint": [10.0, 0.0, 1.0],
    }


def _nav_fixture(plan):
    return {
        "bounds": {"box_min": [-10.0, -10.0, -1.0],
                   "box_max": [10.0, 10.0, 10.0]},
        "tile_count": 4,
        "paths": [{
            "id": pair["id"], "found": True, "partial": False,
            "projected_start": [0.0, 0.0, 1.0],
            "projected_end": [10.0, 0.0, 1.0], "length": 10.0,
        } for pair in plan["nav_pairs"]],
        "seeking": _seeking_fixture(),
    }


def _travel_cycle(index, collision, nav, plan, memory, final=False):
    compact = set(plan["compact_probe_ids"])
    compact_nav = set(plan["compact_nav_ids"])
    cycle = {
        "cycle": index, "used_physical": memory,
        "probes": [deepcopy(record) for record in collision["probes"]
                   if record["id"] in compact],
        "projectiles": [deepcopy(record) for record in collision["projectiles"]
                         if record["id"] in compact],
        "nav_paths": [deepcopy(record) for record in nav["paths"]
                      if record["id"] in compact_nav],
        "nav_tiles": nav["tile_count"],
    }
    if final:
        cycle["seeking"] = deepcopy(nav["seeking"])
    return cycle


def _add_tied_centroid_triangle(mesh, section_index, duplicate_first):
    section = mesh["sections"][section_index]
    old = mesh["indices"][section["first_index"]:section["first_index"] + 3]
    base = len(mesh["positions"])
    for field in ("positions", "normals", "tangent_x", "binormal_sign"):
        mesh[field].extend(deepcopy(mesh[field][vertex]) for vertex in old)
    mesh["uv0"].extend([[value + .2 for value in mesh["uv0"][vertex]]
                        for vertex in old])
    mesh["uv1"].extend([[value + .2 for value in mesh["uv1"][vertex]]
                        for vertex in old])
    duplicate = list(range(base, base + 3))
    insertion = section["first_index"]
    replacement = duplicate + old if duplicate_first else old + duplicate
    mesh["indices"][insertion:insertion + 3] = replacement
    for later in mesh["sections"]:
        if later is not section and later["first_index"] > insertion:
            later["first_index"] += 3
    section["num_triangles"] = 2
    section["min_vertex_index"] = min(old + duplicate)
    section["max_vertex_index"] = max(old + duplicate)
    mesh["triangle_count"] += 1


class ComparatorTests(unittest.TestCase):
    def setUp(self):
        self.a_main, self.a_portal, self.b_main, self.b_portal = _render_fixture()
        self.plan = make_plan.make_plan(self.a_main, None)
        self.a_collision = _collision_fixture(self.plan)
        self.b_collision = deepcopy(self.a_collision)
        self.a_nav = _nav_fixture(self.plan)
        self.b_nav = deepcopy(self.a_nav)
        self.a_extra = {}
        self.b_extra = {}

    def artifacts(self, side):
        if side == "a":
            base = {"main": self.a_main, "portal": self.a_portal,
                    "collision": self.a_collision, "nav": self.a_nav}
            base.update(self.a_extra)
            return base
        base = {"main": self.b_main, "portal": self.b_portal,
                "collision": self.b_collision, "nav": self.b_nav}
        base.update(self.b_extra)
        return base

    def compare(self, plan=None):
        return compare.compare_artifacts(
            self.artifacts("a"), self.artifacts("b"), plan or self.plan)

    def assert_failed_section(self, section, plan=None):
        verdict = self.compare(plan)
        self.assertFalse(verdict["passed"])
        self.assertFalse(verdict["sections"][section]["passed"], verdict)

    def test_split_reference_and_split_carriers_pass(self):
        verdict = self.compare()
        self.assertTrue(verdict["passed"], verdict)
        self.assertEqual(verdict["sections"]["render_portal"]["slot_count"], 2)

    def test_cyclic_rotation_passes_but_reversed_winding_fails(self):
        for mesh in self.b_portal["meshes"]:
            for offset in range(0, len(mesh["indices"]), 3):
                a, b, c = mesh["indices"][offset:offset + 3]
                mesh["indices"][offset:offset + 3] = [b, c, a]
        self.assertTrue(self.compare()["passed"])
        a, b, c = self.b_portal["meshes"][0]["indices"][:3]
        self.b_portal["meshes"][0]["indices"][:3] = [a, c, b]
        self.assert_failed_section("render_portal")

    def test_tied_centroid_triangles_use_multiset_matching(self):
        _add_tied_centroid_triangle(self.a_portal["meshes"][1], 0, False)
        _add_tied_centroid_triangle(self.b_portal["meshes"][1], 0, True)
        self.assertTrue(self.compare()["passed"], self.compare())

    def test_section_coverage_and_metadata_failures(self):
        mutations = (
            lambda mesh: mesh["sections"][1].update(first_index=0),
            lambda mesh: mesh["sections"][0].update(min_vertex_index=1),
            lambda mesh: mesh.update(triangle_count=999),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                self.b_main = _render_fixture()[2]
                mutate(self.b_main["meshes"][0])
                self.assert_failed_section("render_main")
        self.b_main = _render_fixture()[2]
        self.a_main["meshes"][0]["sections"][0]["max_vertex_index"] = 1
        self.assert_failed_section("render_main")

    def test_B_section_bits_gate_but_A_portal_bits_do_not(self):
        self.assertTrue(self.compare()["passed"])
        self.b_portal["meshes"][0]["sections"][0]["enable_collision"] = True
        self.assert_failed_section("render_portal")
        self.b_portal = _render_fixture()[3]
        self.b_main["meshes"][0]["sections"][0]["enable_collision"] = False
        self.assert_failed_section("render_main")

    def test_render_tolerances_and_stream_failures(self):
        mesh = self.b_portal["meshes"][0]
        mesh["positions"][0][0] += 1e-4
        mesh["bounds"]["box_min"][0] += 1e-4
        self.assertTrue(self.compare()["passed"])
        mesh["positions"][0][0] += 1e-7
        self.assert_failed_section("render_portal")
        self.b_portal = _render_fixture()[3]
        self.b_portal["meshes"][0]["binormal_sign"][0] = -1.0
        self.assert_failed_section("render_portal")

    def test_normal_uv_tangent_and_carrier_identity_failures(self):
        mutations = (
            ("normals", [0.01, 0.0, 0.99994]),
            ("uv0", [0.0, 0.000002]),
            ("tangent_x", [0.05, 0.998, 0.0]),
        )
        for field, value in mutations:
            with self.subTest(field=field):
                self.b_portal = _render_fixture()[3]
                self.b_portal["meshes"][0][field][0] = value
                self.assert_failed_section("render_portal")
        self.b_main["meshes"][0]["object_path"] += "_wrong"
        self.assert_failed_section("render_main")

    def test_vacuous_collision_arrays_fail(self):
        for field in ("probes", "settles", "projectiles"):
            with self.subTest(field=field):
                self.b_collision = deepcopy(self.a_collision)
                self.a_collision[field] = []
                self.b_collision[field] = []
                self.assert_failed_section("collision")
                self.a_collision = _collision_fixture(self.plan)

    def test_plan_minimum_violations_fail(self):
        mutations = (
            lambda plan: plan.update(floor_rays=plan["floor_rays"][:127]),
            lambda plan: plan.update(wall_rays=plan["wall_rays"][:63]),
            lambda plan: plan.update(player_start_radial_count=7),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                plan = deepcopy(self.plan)
                mutate(plan)
                self.assert_failed_section("collision", plan)

    def test_missing_plan_probe_and_expect_block_miss_fail(self):
        self.b_collision["probes"] = self.b_collision["probes"][1:]
        self.assert_failed_section("collision")
        self.b_collision = deepcopy(self.a_collision)
        # The expect_block gate is a fraction (>= 80% must block in A: one
        # anchored floor shot legitimately exits via the pit's no-collision
        # lava portal) — breach it by dropping 30% of the expected shots.
        expected_ids = [shot["id"] for shot in self.plan["projectiles"]
                        if shot.get("expect_block")]
        to_miss = set(expected_ids[:max(1, (len(expected_ids) * 3) // 10 + 1)])
        for record in self.a_collision["projectiles"]:
            if record["id"] in to_miss:
                record["blocking_hit"] = False
                for field in ("impact_point", "impact_normal", "actor",
                              "component", "material_slot", "distance"):
                    record.pop(field, None)
        self.b_collision = deepcopy(self.a_collision)
        self.assert_failed_section("collision")

    def test_floor_block_ratio_and_portal_collision_fail(self):
        for record in self.a_collision["probes"]:
            if record["id"].startswith("floor_"):
                record["blocking_hit"] = False
        self.b_collision = deepcopy(self.a_collision)
        self.assert_failed_section("collision")
        self.a_collision = _collision_fixture(self.plan)
        self.b_collision = deepcopy(self.a_collision)
        self.a_collision["projectiles"][-1]["blocking_hit"] = True
        self.b_collision["projectiles"][-1]["blocking_hit"] = True
        self.assert_failed_section("collision")

    def test_collision_tolerance_and_identity_failures(self):
        mutations = (
            lambda record: record.update(actor="Other"),
            lambda record: record.update(distance=100.10001),
            lambda record: record.update(impact_normal=[0.05, 0.0, 0.998]),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                self.b_collision = deepcopy(self.a_collision)
                mutate(self.b_collision["probes"][0])
                self.assert_failed_section("collision")

    def test_nav_seeking_trajectory_and_comparison_failures(self):
        mutations = (
            lambda nav: nav.update(tile_count=0),
            lambda nav: nav["paths"][0].update(found=False),
            lambda nav: nav["seeking"].update(los_blocked=False),
            lambda nav: nav["seeking"].update(side="right", side_sign=-1),
            lambda nav: nav["seeking"].update(completion_reason="timeout"),
            lambda nav: nav["seeking"].update(trajectory=[]),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                self.b_nav = deepcopy(self.a_nav)
                mutate(self.b_nav)
                self.assert_failed_section("nav")

    def test_nav_endpoint_and_length_tolerances_fail(self):
        self.b_nav["paths"][0]["projected_end"] = [11.00001, 0.0, 1.0]
        self.assert_failed_section("nav")
        self.b_nav = deepcopy(self.a_nav)
        self.b_nav["paths"][0]["length"] = 15.00001
        self.assert_failed_section("nav")

    def test_travel_missing_mismatched_and_zero_tiles_fail(self):
        self.a_extra = {"travel": [_travel_cycle(
            0, self.a_collision, self.a_nav, self.plan, 100, True)]}
        self.b_extra = {"travel": []}
        self.assert_failed_section("travel")
        self.b_extra = {"travel": [deepcopy(self.a_extra["travel"][0]),
                                   deepcopy(self.a_extra["travel"][0])]}
        self.assert_failed_section("travel")
        self.b_extra = {"travel": [deepcopy(self.a_extra["travel"][0])]}
        self.b_extra["travel"][0]["nav_tiles"] = 0
        self.assert_failed_section("travel")

    def test_travel_memory_series_and_growth_failure(self):
        plan = deepcopy(self.plan)
        plan["travel_count"] = 4
        memories = [100, 100, 100, 106]
        self.a_extra = {"summary": {"mode": "travel"}, "travel": []}
        self.b_extra = {"summary": {"mode": "travel"}, "travel": []}
        for index, memory in enumerate(memories):
            final = index == len(memories) - 1
            cycle = _travel_cycle(index, self.a_collision, self.a_nav,
                                  plan, memory, final)
            self.a_extra["travel"].append(cycle)
            self.b_extra["travel"].append(deepcopy(cycle))
        verdict = self.compare(plan)
        travel = verdict["sections"]["travel"]
        self.assertFalse(travel["passed"], verdict)
        self.assertEqual(travel["memory_series"]["a"], memories)
        self.assertTrue(any("resource growth trend" in error
                            for error in travel["errors"]))

    def test_travel_requires_plan_count_and_final_seeking(self):
        plan = deepcopy(self.plan)
        plan["travel_count"] = 2
        cycle = _travel_cycle(0, self.a_collision, self.a_nav,
                              plan, 100, False)
        self.a_extra = {"summary": {"mode": "travel"}, "travel": [cycle]}
        self.b_extra = deepcopy(self.a_extra)
        self.assert_failed_section("travel", plan)
        second = _travel_cycle(1, self.a_collision, self.a_nav,
                               plan, 100, False)
        self.a_extra["travel"].append(second)
        self.b_extra["travel"].append(deepcopy(second))
        self.assert_failed_section("travel", plan)

    def test_cli_writes_hashes_and_requires_plan(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for label, artifacts in (("a", self.artifacts("a")),
                                     ("b", self.artifacts("b"))):
                directory = root / label
                directory.mkdir()
                for name in ("render_main", "render_portal", "collision", "nav"):
                    key = {"render_main": "main", "render_portal": "portal"}.get(
                        name, name)
                    (directory / f"{name}.json").write_text(
                        json.dumps(artifacts[key]), encoding="utf-8")
            plan_path = root / "plan.json"
            plan_path.write_text(json.dumps(self.plan), encoding="utf-8")
            verdict_path = root / "verdict.json"
            result = subprocess.run([
                sys.executable, str(HERE / "compare_dumps.py"),
                str(root / "a"), str(root / "b"), "--plan", str(plan_path),
                "--hash", "--out", str(verdict_path)], check=False)
            self.assertEqual(result.returncode, 0)
            verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
            self.assertTrue(verdict["passed"])
            self.assertEqual(set(verdict["hashes"]), {"a", "b"})
            self.assertEqual(len(verdict["hashes"]["a"]), 4)


class PlanTests(unittest.TestCase):
    def test_plan_is_deterministic_and_has_gate_minimums(self):
        render = _render_fixture()[0]
        first = make_plan.make_plan(render, None)
        second = make_plan.make_plan(deepcopy(render), None)
        self.assertEqual(first, second)
        self.assertEqual(len(first["floor_rays"]), 128)
        self.assertGreaterEqual(len(first["wall_rays"]), 64)
        self.assertEqual(len(first["nav_pairs"]), 12)
        self.assertGreaterEqual(len(first["projectiles"]), 32)
        self.assertEqual(first["travel_count"], 10)
        self.assertTrue(all(shot["expect_block"]
                            for shot in first["projectiles"]
                            if shot.get("shot_kind") == "floor"))
        self.assertTrue(any(shot["expect_block"]
                            for shot in first["projectiles"]))
        self.assertTrue(all(not shot["expect_block"]
                            for shot in first["projectiles"]
                            if shot["portal_directed"]))

    def test_collision_oracle_is_carried_without_aliasing(self):
        render = _render_fixture()[0]
        plan = make_plan.make_plan(render, None)
        oracle = _collision_fixture(plan)
        output = make_plan.make_plan(render, oracle)
        self.assertEqual(output["collision_oracle"], oracle)
        oracle["probes"][0]["id"] = "mutated"
        self.assertNotEqual(output["collision_oracle"]["probes"][0]["id"],
                            "mutated")


if __name__ == "__main__":
    unittest.main()
