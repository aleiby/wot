"""Exercise editor zone configuration without loading or saving content."""
import ast
import json
from pathlib import Path
import re
import tempfile
import types
import unittest


class ZoneActor:
    def __init__(self):
        self.props = {}

    def set_editor_property(self, name, value):
        self.props[name] = value

    def get_component_by_class(self, _cls):
        return self

    def set_box_extent(self, extent, _update):
        self.extent = extent

    def set_actor_label(self, label):
        self.label = label


class ZoneImportTests(unittest.TestCase):
    def place(self, props=None, defaults=None, velocity=(0, 0, 0)):
        source = Path(__file__).with_name("import_map.py")
        tree = ast.parse(source.read_text(encoding="utf-8"))
        names = {"place_zones", "_set_zone_trigger_velocity", "_sprayer_uc",
                 "_ml_class_prop", "_ml_bool"}
        functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                     and node.name in names]
        spawned = []

        def spawn(cls, _location):
            actor = cls()
            spawned.append(actor)
            return actor

        unreal = types.SimpleNamespace(
            WoTZoneVolume=ZoneActor, BoxComponent=object,
            Vector=lambda *args: args, Rotator=lambda **kwargs: kwargs,
            Name=str, log=lambda _message: None)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "zones").mkdir()
            (root / "zones" / "SpecMap.zones.json").write_text("{}")
            if props is not None:
                (root / "SpecMap.t3d.json").write_text(json.dumps(dict(actors=[
                    dict(name="Current0", cls="CurrentZone", props=props)])))
            script = root / "script" / "Fixture"
            script.mkdir(parents=True)
            for cls, (parent, fields) in (defaults or {}).items():
                assignments = "\n".join(f"    {k}={v}" for k, v in fields.items())
                (script / f"{cls}.uc").write_text(
                    f"class {cls} extends {parent};\ndefaultproperties\n{{\n"
                    + assignments + "\n}\n", encoding="latin-1")
            zone = dict(actor="Current0", aabb=[[-8, -8, -8], [8, 8, 8]],
                        velocity=list(velocity))
            scope = dict(json=json, re=re, EXPORTS=root, CONV=root, MAP="SpecMap",
                         unreal=unreal, _uc_files=None, _uc_cache={},
                         _zone_export_sections=lambda: ([zone], [], []))
            exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), "exec"), scope)
            count = scope["place_zones"](
                types.SimpleNamespace(spawn_actor_from_class=spawn))
        self.assertEqual(count, 1)
        return spawned[0]

    def test_authored_trigger_settings_reach_the_editor_zone(self):
        actor = self.place(dict(
            bTrigZoneVelo2="True", bTrigZoneVelo2InitiallyOn="True",
            MaxZoneVelocity2="(X=-80,Y=12,Z=240)",
            MinZoneVelocity2="(X=2,Y=-4,Z=6)", TransitionTime2="0.75",
            TriggerDuration="3.5", InitialState="TriggerControl", Tag="WindSwitch"))
        self.assertEqual(actor.props.get("trig_zone_velo2"), True)
        self.assertEqual(actor.props.get("trig_zone_velo2_initially_on"), True)
        self.assertEqual(actor.props.get("max_zone_velocity2"), (-80, 12, 240))
        self.assertEqual(actor.props.get("min_zone_velocity2"), (2, -4, 6))
        self.assertEqual(actor.props.get("transition_time2"), 0.75)
        self.assertEqual(actor.props.get("trigger_duration"), 3.5)
        self.assertEqual(actor.props.get("initial_state"), "TriggerControl")
        self.assertEqual(actor.props.get("tag_name"), "WindSwitch")
        # A .75 transition steps past the whole range each tick: "on" applies
        # the steady midpoint of maximum and minimum.
        self.assertEqual(actor.props.get("zone_velocity"), (-39, 4, 123))

    def test_long_transition_applies_its_two_tick_average(self):
        actor = self.place(dict(
            bTrigZoneVelo2="True", bTrigZoneVelo2InitiallyOn="True",
            MaxZoneVelocity2="(X=80,Y=-40,Z=120)", TransitionTime2="4"))
        self.assertEqual(actor.props.get("zone_velocity"), (70, -35, 105))

    def test_omitted_settings_resolve_the_class_chain(self):
        actor = self.place({}, defaults={
            "BaseCurrentZone": ("ZoneInfo", dict(
                bTrigZoneVelo2="True", bTrigZoneVelo2InitiallyOn="True",
                MaxZoneVelocity2="(X=5,Y=7,Z=90)",
                MinZoneVelocity2="(X=-2,Y=4,Z=6)", TransitionTime2="0.25",
                TriggerDuration="2", InitialState="TriggerTimed", Tag="DefaultWind")),
            "CurrentZone": ("BaseCurrentZone", dict(bTrigZoneVelo2InitiallyOn="False")),
        })
        self.assertEqual(actor.props.get("trig_zone_velo2"), True)
        self.assertEqual(actor.props.get("trig_zone_velo2_initially_on"), False)
        self.assertEqual(actor.props.get("max_zone_velocity2"), (5, 7, 90))
        self.assertEqual(actor.props.get("min_zone_velocity2"), (-2, 4, 6))
        self.assertEqual(actor.props.get("transition_time2"), 0.25)
        self.assertEqual(actor.props.get("trigger_duration"), 2)
        self.assertEqual(actor.props.get("initial_state"), "TriggerTimed")
        self.assertEqual(actor.props.get("tag_name"), "DefaultWind")
        self.assertEqual(actor.props.get("zone_velocity"), (-2, 4, 6))

    def test_instance_false_and_zero_override_class_defaults(self):
        actor = self.place(dict(bTrigZoneVelo2="False", TransitionTime2="0",
                                TriggerDuration="0", Tag="None", InitialState="None"),
                           defaults={"CurrentZone": ("ZoneInfo", dict(
                               bTrigZoneVelo2="True", TransitionTime2="2",
                               TriggerDuration="4", Tag="Wind", InitialState="TriggerToggle"))},
                           velocity=(10, 20, 30))
        self.assertEqual(actor.props.get("trig_zone_velo2"), False)
        self.assertEqual(actor.props.get("transition_time2"), 0)
        self.assertEqual(actor.props.get("trigger_duration"), 0)
        self.assertEqual(actor.props.get("tag_name"), "None")
        self.assertEqual(actor.props.get("initial_state"), "None")
        self.assertEqual(actor.props.get("zone_velocity"), (10, 20, 30))

    def test_unset_trigger_fields_use_zero_defaults(self):
        actor = self.place({})
        self.assertEqual(actor.props.get("trig_zone_velo2"), False)
        self.assertEqual(actor.props.get("trig_zone_velo2_initially_on"), False)
        self.assertEqual(actor.props.get("max_zone_velocity2"), (0, 0, 0))
        self.assertEqual(actor.props.get("min_zone_velocity2"), (0, 0, 0))
        self.assertEqual(actor.props.get("transition_time2"), 0)
        self.assertEqual(actor.props.get("trigger_duration"), 0)
        self.assertEqual(actor.props.get("initial_state"), "None")

    def test_vector_overrides_preserve_inherited_components(self):
        actor = self.place(dict(MaxZoneVelocity2="(Z=3e2)"), defaults={
            "BaseCurrentZone": ("ZoneInfo", dict(
                bTrigZoneVelo2="True", bTrigZoneVelo2InitiallyOn="True",
                **{"MaxZoneVelocity2 ": "(X=12,Y=24,Z=48)"})),
            "CurrentZone": ("BaseCurrentZone", dict(MaxZoneVelocity2="(Y=-5)")),
        })
        self.assertEqual(actor.props.get("max_zone_velocity2"), (12, -5, 300))
        self.assertEqual(actor.props.get("zone_velocity"), (6, -2.5, 150))

    def test_old_conversion_without_actor_json_keeps_static_velocity(self):
        actor = self.place(velocity=(3, 6, 9))
        self.assertEqual(actor.props.get("zone_velocity"), (3, 6, 9))


if __name__ == "__main__":
    unittest.main()
