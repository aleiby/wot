import json
import tempfile
import unittest
from pathlib import Path

import import_path_nodes as path_nodes


class PathNodePlanTests(unittest.TestCase):
    def _plan(self, tmp, t3d, reachspecs):
        root = Path(tmp)
        (root / "SpecMap.t3d").write_text(t3d, encoding="latin-1")
        (root / "SpecMap.json").write_text(
            json.dumps({"reachspecs": reachspecs}), encoding="utf-8")
        old_t3d = path_nodes.T3D_DIR
        old_reachspec = path_nodes.REACHSPEC_DIR
        try:
            path_nodes.T3D_DIR = root
            path_nodes.REACHSPEC_DIR = root
            return path_nodes.plan_map("SpecMap")
        finally:
            path_nodes.T3D_DIR = old_t3d
            path_nodes.REACHSPEC_DIR = old_reachspec

    def test_plan_map_stamps_full_navigation_list_with_class_heights(self):
        actors = [
            ("PathNode", "Path0", "", 50.0, False),
            ("WotPathNode", "Wot0", "    bDark=True\n    bSnipe=True\n", 50.0, True),
            ("MashadarPathNode", "Mash0", "", 50.0, False),
            ("InventorySpot", "Inv0", "", 40.0, False),
            ("PlayerStart", "Start0", "    CollisionHeight=44.5\n", 44.5, False),
            ("Teleporter", "Tele0", "", 40.0, False),
            ("TeleporterWithEffects", "TeleFx0", "", 40.0, False),
        ]
        body = ["Begin Map"]
        for index, (cls, name, extra, _height, _dark) in enumerate(actors):
            body.extend([
                f"Begin Actor Class={cls} Name={name}",
                f"    Location=(X={index * 100},Y=0,Z=100)",
                extra.rstrip(),
                f"    Name={name}",
                "End Actor",
            ])
        body.append("End Map")

        with tempfile.TemporaryDirectory() as tmp:
            plan = self._plan(tmp, "\n".join(body), [])

        self.assertEqual([stamp["label"] for stamp in plan["nodes"]],
                         [actor[1] for actor in actors])
        self.assertEqual([stamp["source_class"] for stamp in plan["nodes"]],
                         [actor[0] for actor in actors])
        self.assertEqual([stamp["dark"] for stamp in plan["nodes"]],
                         [actor[4] for actor in actors])
        self.assertEqual([stamp["snipe"] for stamp in plan["nodes"]],
                         [actor[1] == "Wot0" for actor in actors])
        self.assertEqual([stamp["collision_height"] for stamp in plan["nodes"]],
                         [actor[3] for actor in actors])
        self.assertEqual(
            [stamp["extra_cost"] for stamp in plan["nodes"]],
            [0] * len(actors))

    def test_plan_map_ignores_non_navigation_actors(self):
        text = """Begin Map
Begin Actor Class=Decoration Name=Decoration0
    Location=(X=0,Y=0,Z=100)
    CollisionHeight=99
    Name=Decoration0
End Actor
End Map
"""
        with tempfile.TemporaryDirectory() as tmp:
            plan = self._plan(tmp, text, [])

        self.assertEqual(plan["nodes"], [])

    def test_plan_map_resolves_order_direction_drops_and_extra_cost(self):
        text = """Begin Map
Begin Actor Class=PathNode Name=NodeA
    Location=(X=0,Y=0,Z=100)
    Paths(1)=1
    Paths(3)=3
    Paths(0)=0
    ExtraCost=17
    Name=NodeA
End Actor
Begin Actor Class=PathNode Name=NodeB
    Location=(X=100,Y=0,Z=100)
    upstreamPaths(1)=2
    upstreamPaths(0)=0
    Name=NodeB
End Actor
Begin Actor Class=WotPathNode Name=NodeC
    Location=(X=200,Y=0,Z=100)
    Name=NodeC
End Actor
Begin Actor Class=Decoration Name=Decoration0
    Location=(X=300,Y=0,Z=100)
    Name=Decoration0
End Actor
End Map
"""
        reachspecs = [
            {"start": "NodeA", "end": "NodeB", "distance": 10},
            {"start": "NodeA", "end": "NodeC", "distance": 20},
            {"start": "Decoration0", "end": "NodeB", "distance": 30},
            {"start": "NodeA", "end": "Decoration0", "distance": 40},
        ]
        with tempfile.TemporaryDirectory() as tmp:
            plan = self._plan(tmp, text, reachspecs)

        by_label = {node["label"]: node for node in plan["nodes"]}
        self.assertEqual(
            by_label["NodeA"]["out_edges"],
            [("NodeB", 10), ("NodeC", 20)])
        self.assertEqual(by_label["NodeB"]["in_edges"], [("NodeA", 10)])
        self.assertEqual(by_label["NodeA"]["extra_cost"], 17)
        self.assertEqual(by_label["NodeB"]["extra_cost"], 0)
        self.assertEqual(plan["dropped_edges"], {"out": 1, "in": 1})

    def test_plan_map_rejects_slots_beyond_the_retail_16_cap(self):
        # UE1 Paths/upstreamPaths are fixed 16-slot arrays; slot 16 cannot
        # come from a faithful export.
        for prop in ("Paths", "upstreamPaths"):
            text = f"""Begin Map
Begin Actor Class=PathNode Name=NodeA
    Location=(X=0,Y=0,Z=100)
    {prop}(16)=0
    Name=NodeA
End Actor
End Map
"""
            with tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(ValueError, "16-slot"):
                    self._plan(tmp, text, [
                        {"start": "NodeA", "end": "NodeA", "distance": 1}])

    def test_plan_map_requires_reachspec_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            old_t3d = path_nodes.T3D_DIR
            old_reachspec = path_nodes.REACHSPEC_DIR
            try:
                path_nodes.T3D_DIR = Path(tmp)
                path_nodes.REACHSPEC_DIR = Path(tmp)
                (Path(tmp) / "SpecMap.t3d").write_text(
                    "Begin Map\nEnd Map\n", encoding="latin-1")
                with self.assertRaisesRegex(
                        FileNotFoundError, "missing ReachSpec JSON"):
                    path_nodes.plan_map("SpecMap")
            finally:
                path_nodes.T3D_DIR = old_t3d
                path_nodes.REACHSPEC_DIR = old_reachspec

    def test_single_actor_preserves_same_label_gameplay_twin(self):
        class GameplayActor:
            def __init__(self, label):
                self.label = label

            def get_actor_label(self):
                return self.label

        class DataActor(GameplayActor):
            def set_actor_label(self, label):
                self.label = label

            def set_actor_location(self, location, _sweep, _teleport):
                self.location = location

        class EditorActorSubsystem:
            def __init__(self):
                self.spawned = []
                self.destroyed = []

            def spawn_actor_from_class(self, actor_class, location):
                actor = actor_class("spawned")
                actor.location = location
                self.spawned.append(actor)
                return actor

            def destroy_actor(self, actor):
                self.destroyed.append(actor)

        gameplay_twin = GameplayActor("Teleporter0")
        actors = [gameplay_twin]
        eas = EditorActorSubsystem()
        node = path_nodes._single_actor(
            eas, actors, "NavList_Teleporter0", DataActor, (1, 2, 3),
            legacy_label="Teleporter0")

        self.assertEqual(node.get_actor_label(), "NavList_Teleporter0")
        self.assertNotIn(gameplay_twin, eas.destroyed)
        self.assertIs(path_nodes._single_actor(
            eas, actors, "NavList_Teleporter0", DataActor, (4, 5, 6),
            legacy_label="Teleporter0"), node)
        self.assertEqual(len(eas.spawned), 1)


if __name__ == "__main__":
    unittest.main()
