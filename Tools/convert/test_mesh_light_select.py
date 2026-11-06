"""Retail actor-light filtering, priority, threshold, and cap tests."""
import unittest

from mesh_light_select import lighting_tag, select_mesh_lights


def _light(name, x, *, radius=100.0, brightness=64, special_lit=False,
           emits=True):
    return {
        "name": name,
        "location": (float(x), 0.0, 0.0),
        "radius": float(radius),
        "brightness": int(brightness),
        "special_lit": bool(special_lit),
        "emits": bool(emits),
    }


class RetailMeshLightSelectionTests(unittest.TestCase):
    def test_lighting_tag_uses_raw_brightness_and_retail_rounding(self):
        self.assertEqual(lighting_tag((0.0, 0.0, 0.0),
                                      _light("half", 50, brightness=1)), 512)

    def test_sqrt_approx_threshold_membership_witnesses(self):
        arena_light = {
            "location": (-4915.652344, -11787.421875, -7664.330566),
            "radius": 625.0,
            "brightness": 96,
        }
        self.assertEqual(lighting_tag(
            (-4969.651855, -11269.277344, -7863.899414), arena_light),
            10584)

        battle_light = {
            "location": (-16602.650391, -7637.359375, 21302.890625),
            "radius": 1225.0,
            "brightness": 32,
        }
        battle_tag = lighting_tag(
            (-17688.714844, -7321.080078, 21048.101563), battle_light)
        self.assertEqual(battle_tag, 1763)
        self.assertGreater(battle_tag, 1762)

    def test_filter_visibility_threshold_and_special_lit(self):
        rows = [
            _light("blocked_high", 10, brightness=100),
            _light("bright", 20, brightness=80),
            _light("below_threshold", 95, brightness=4),
            _light("special", 5, brightness=255, special_lit=True),
            _light("outside", 101, brightness=255),
        ]
        by_name = {row["name"]: row for row in rows}
        selected = select_mesh_lights(
            actor_location=(0.0, 0.0, 0.0),
            actor_special_lit=False,
            max_lights=6,
            permeating_names=[row["name"] for row in rows],
            lights_by_name=by_name,
            line_check=lambda _start, end: end != (10.0, 0.0, 0.0),
        )
        self.assertEqual([row["name"] for row in selected], ["bright"])

    def test_equal_tags_preserve_permeating_order_and_cap(self):
        rows = [_light(f"light{i}", 25) for i in range(4)]
        selected = select_mesh_lights(
            actor_location=(0.0, 0.0, 0.0),
            actor_special_lit=False,
            max_lights=2,
            permeating_names=[row["name"] for row in rows],
            lights_by_name={row["name"]: row for row in rows},
            line_check=lambda _start, _end: True,
        )
        self.assertEqual([row["name"] for row in selected],
                         ["light0", "light1"])

    def test_non_emitting_visible_lights_raise_threshold_and_consume_cap(self):
        rows = [
            _light("bright", 10, brightness=128),
            _light("lightning_off", 10, brightness=100, emits=False),
            _light("otherwise_selected", 10, brightness=16),
        ]
        selected = select_mesh_lights(
            actor_location=(0.0, 0.0, 0.0),
            actor_special_lit=False,
            max_lights=2,
            permeating_names=[row["name"] for row in rows],
            lights_by_name={row["name"]: row for row in rows},
            line_check=lambda _start, _end: True,
        )
        self.assertEqual([row["name"] for row in selected], ["bright"])

    def test_missing_light_name_is_skipped(self):
        selected = select_mesh_lights(
            actor_location=(0.0, 0.0, 0.0),
            actor_special_lit=False,
            max_lights=6,
            permeating_names=["not_in_t3d"],
            lights_by_name={},
            line_check=lambda _start, _end: True,
        )
        self.assertEqual(selected, [])


if __name__ == "__main__":
    unittest.main()
