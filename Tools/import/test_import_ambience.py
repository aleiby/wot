"""The ambience pass stamps fields the placed ambient actor actually has.

Runs without the editor: plan_map reads a temporary T3D, and main() runs
against a recording stand-in for the `unreal` module. The actor's property
names come from the C++ header, so a stale importer field fails here.
"""
import re
import sys
import tempfile
import types
import unittest
from pathlib import Path

import import_ambience as ambience

REPO = Path(__file__).resolve().parents[2]
AMBIENT_HEADER = REPO / "Source" / "WoT" / "WoTAmbientSound.h"


def _t3d(*actors):
    lines = ["Begin Map"]
    for index, (cls, name, props) in enumerate(actors):
        lines.append(f"Begin Actor Class={cls} Name={name}")
        lines.append(f"    Location=(X={index * 100},Y=0,Z=0)")
        lines.extend(f"    {key}={value}" for key, value in props.items())
        lines.append("End Actor")
    lines.append("End Map")
    return "\n".join(lines) + "\n"


SPEC_MAP = _t3d(
    ("Light", "PulseLight0", {"AmbientSound": "Sound'AmbientA.Fire17'",
                              "LightType": "LT_SubtlePulse",
                              "LightBrightness": "200", "LightPeriod": "40",
                              "LightPhase": "7"}),
    ("Light", "PlainLight0", {"AmbientSound": "Sound'AmbientA.Fire17'"}),
    ("Decoration", "Unlit0", {"AmbientSound": "Sound'AmbientA.Wind3'"}),
)


def _snake(name):
    """UE's Python name for a C++ property ("LightType" -> "light_type")."""
    if re.match(r"b[A-Z]", name):
        name = name[1:]
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def _ambient_actor_properties():
    text = AMBIENT_HEADER.read_text(encoding="utf-8")
    start = text.index("class AWoTAmbientSound : public AActor")
    body = text[start:text.index("};", start)]
    names = re.findall(r"UPROPERTY\([^)]*\)\s*(?:[\w:<>]+\s+)+(\w+)\s*(?:=|;)", body)
    return {_snake(name) for name in names}


class _Recorder:
    def __init__(self, cls):
        self.cls = cls
        self.props = {}

    def set_actor_label(self, _label):
        pass

    def get_actor_label(self):
        return ""

    def set_editor_property(self, name, value):
        self.props[name] = value


def _fake_unreal(spawned):
    module = types.ModuleType("unreal")

    class WoTAmbientSound:
        pass

    class WoTDynamicAmbientSound:
        pass

    class WoTMover:
        pass

    class WoTDecoration:
        pass

    class _LevelEditor:
        def load_level(self, _asset):
            return True

        def save_current_level(self):
            return True

    class _Actors:
        def get_all_level_actors(self):
            return []

        def destroy_actor(self, _actor):
            return True

        def spawn_actor_from_class(self, cls, _location):
            actor = _Recorder(cls)
            spawned.append(actor)
            return actor

    module.WoTAmbientSound = WoTAmbientSound
    module.WoTDynamicAmbientSound = WoTDynamicAmbientSound
    module.WoTMover = WoTMover
    module.WoTDecoration = WoTDecoration
    module.LevelEditorSubsystem = _LevelEditor
    module.EditorActorSubsystem = _Actors
    module.EditorAssetLibrary = types.SimpleNamespace(
        does_asset_exist=lambda _asset: True)
    module.get_editor_subsystem = lambda cls: cls()
    module.Vector = lambda *xyz: xyz
    module.Name = str
    module.log = lambda _message: None
    return module


class AmbienceLightStampTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        (root / "SpecMap.t3d").write_text(SPEC_MAP, encoding="latin-1")
        self._saved = (ambience.T3D_DIR, ambience._CLASSES)
        ambience.T3D_DIR = root
        ambience._CLASSES = {}   # engine defaults only; no game script needed

    def tearDown(self):
        ambience.T3D_DIR, ambience._CLASSES = self._saved
        self._tmp.cleanup()

    def _emitters(self):
        return {e["name"]: e for e in ambience.plan_map("SpecMap")["emitters"]}

    def test_authored_light_fields_use_the_shipped_enum_order(self):
        emitter = self._emitters()["PulseLight0"]
        self.assertEqual(emitter["light_type"], 7)   # LT_SubtlePulse
        self.assertEqual(emitter["light_brightness"], 200)
        self.assertEqual(emitter["light_period"], 40)
        self.assertEqual(emitter["light_phase"], 7)

    def test_light_class_defaults(self):
        emitter = self._emitters()["PlainLight0"]
        self.assertEqual(emitter["light_type"], 1)   # LT_Steady
        self.assertEqual(emitter["light_brightness"], 64)
        self.assertEqual(emitter["light_period"], 32)
        self.assertEqual(emitter["light_phase"], 0)

    def test_unlit_carrier(self):
        emitter = self._emitters()["Unlit0"]
        self.assertEqual((emitter["light_type"], emitter["light_brightness"],
                          emitter["light_period"], emitter["light_phase"]),
                         (0, 0, 0, 0))

    def test_every_stamped_property_exists_on_the_ambient_actor(self):
        spawned = []
        saved_argv = sys.argv
        sys.modules["unreal"] = _fake_unreal(spawned)
        try:
            sys.argv = ["import_ambience.py", "SpecMap"]
            ambience.main()
        finally:
            sys.argv = saved_argv
            del sys.modules["unreal"]
        emitters = [a for a in spawned if a.cls.__name__ == "WoTAmbientSound"]
        self.assertEqual(len(emitters), 3)
        known = _ambient_actor_properties()
        self.assertIn("light_type", known)
        for emitter in emitters:
            self.assertLessEqual(set(emitter.props), known)
        pulse = next(e for e in emitters if e.props.get("light_period") == 40)
        self.assertEqual((pulse.props["light_type"], pulse.props["light_brightness"],
                          pulse.props["light_phase"]), (7, 200, 7))


if __name__ == "__main__":
    unittest.main()
