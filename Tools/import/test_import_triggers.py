"""Exercise trigger placement without loading the editor or saving content."""
import ast
import json
from pathlib import Path
import tempfile
import types
import unittest


class Actor:
    def __init__(self):
        self.props = {}

    def set_actor_label(self, label):
        self.label = label

    def set_editor_property(self, name, value):
        self.props[name] = value

    def get_editor_property(self, name):
        return self

    def set_capsule_size(self, radius, height):
        self.size = (radius, height)

    def set_ue1_collision_size(self, radius, height):
        self.props["collision_radius"] = radius
        self.props["collision_height"] = height


class TriggerImportTests(unittest.TestCase):
    def place(self, props):
        source = Path(__file__).with_name("import_map.py")
        tree = ast.parse(source.read_text(encoding="utf-8"))
        functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                     and node.name in ("place_movers", "_set_trigger_properties")]
        enums = types.SimpleNamespace(PLAYER_PROXIMITY=0, PAWN_PROXIMITY=1,
                                      CLASS_PROXIMITY=2, ANY_PROXIMITY=3, SHOOT=4)
        unreal = types.SimpleNamespace(WoTTrigger=Actor, WoTTriggerType=enums,
                                       Vector=lambda *args: args,
                                       WoTDoorDirection=types.SimpleNamespace(FORWARD=0, BACKWARD=1))
        spawned = []

        def spawn(cls, location):
            actor = cls()
            spawned.append(actor)
            return actor

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = root / "movers" / "SpecMap"
            folder.mkdir(parents=True)
            (folder / "movers.json").write_text(json.dumps(dict(
                movers=[], triggers=[dict(name="Target", location=[0, 0, 0],
                                          tag="", event="Wall", once_only=True,
                                          retrigger=0, radius=48, height=150)],
                dispatchers=[], counters=[])), encoding="utf-8")
            (root / "SpecMap.t3d.json").write_text(json.dumps(dict(actors=[
                dict(name="Target", cls="Trigger", props=props)])), encoding="utf-8")
            scope = dict(json=json, CONV=root, MAP="SpecMap", GAMMA_SPACE=False,
                         unreal=unreal)
            exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), "exec"), scope)
            counts = scope["place_movers"](types.SimpleNamespace(spawn_actor_from_class=spawn), {}, {})
        self.assertEqual(counts["triggers"], 1)
        return spawned[0]

    def test_authored_shoot_settings_reach_editor_actor(self):
        actor = self.place(dict(TriggerType="TT_Shoot", DamageThreshold="10.000000",
                                bInitiallyActive="False", Message="Target hit",
                                RepeatTriggerTime="0.5"))
        self.assertEqual(actor.props.get("trigger_type"), 4)
        self.assertEqual(actor.props.get("damage_threshold"), 10.0)
        self.assertEqual(actor.props.get("initially_active"), False)
        self.assertEqual(actor.props.get("message"), "Target hit")
        self.assertEqual(actor.props.get("repeat_trigger_time"), 0.5)

    def test_omitted_values_use_trigger_defaults(self):
        actor = self.place({})
        self.assertEqual(actor.props.get("trigger_type"), 0)
        self.assertEqual(actor.props.get("damage_threshold"), 0.0)
        self.assertEqual(actor.props.get("initially_active"), True)
        self.assertEqual(actor.props.get("message"), "")
        self.assertEqual(actor.props.get("repeat_trigger_time"), 0.0)

    def test_message_decodes_t3d_string_literal(self):
        actor = self.place(dict(Message=r'"Target says \"ready\""'))
        self.assertEqual(actor.props.get("message"), 'Target says "ready"')

    def test_authored_cylinder_reaches_runtime_touch_filter(self):
        actor = self.place(dict(TriggerType="TT_Shoot"))
        self.assertEqual(actor.props.get("collision_radius"), 48)
        self.assertEqual(actor.props.get("collision_height"), 150)


if __name__ == "__main__":
    unittest.main()
