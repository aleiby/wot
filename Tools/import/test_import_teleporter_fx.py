import tempfile
import unittest
from pathlib import Path

import import_teleporter_fx as teleporter_fx


class TeleporterFxPlanTests(unittest.TestCase):
    def test_plan_map_preserves_independent_effect_and_sound_axes(self):
        text = """Begin Map
Begin Actor Class=TeleporterWithEffects Name=TeleporterWithEffects0
    EnterEffectClass=Class'WOT.WOTSparks'
    EnterEffectSound=Sound'AmbientA.Portal.Portal5'
    ExitEffectClass=Class'WOT.WOTSparks'
    ExitEffectSound=Sound'AmbientA.Portal.Portal5'
    Name=TeleporterWithEffects0
End Actor
Begin Actor Class=TeleporterWithEffects Name=TeleporterWithEffects1
    ExitEffectSound=Sound'AmbientA.Portal.Portal5'
    Name=TeleporterWithEffects1
End Actor
End Map
"""
        with tempfile.TemporaryDirectory() as tmp:
            old = teleporter_fx.T3D_DIR
            try:
                teleporter_fx.T3D_DIR = Path(tmp)
                (Path(tmp) / "SpecMap.t3d").write_text(text)
                plan = teleporter_fx.plan_map("SpecMap")
            finally:
                teleporter_fx.T3D_DIR = old

        self.assertEqual(plan["TeleporterWithEffects0"], {
            "enter_effect_config": "WOTSparks",
            "exit_effect_config": "WOTSparks",
            "enter_effect_sound_path": "AmbientA/Portal5",
            "exit_effect_sound_path": "AmbientA/Portal5",
        })
        self.assertEqual(plan["TeleporterWithEffects1"], {
            "enter_effect_config": "",
            "exit_effect_config": "",
            "enter_effect_sound_path": "",
            "exit_effect_sound_path": "AmbientA/Portal5",
        })

    def test_restamp_writes_empty_axes_and_counts_only_payload_or_change(self):
        class Actor:
            def __init__(self, values):
                self.values = dict(values)
                self.writes = []

            def get_editor_property(self, prop):
                return self.values.get(prop, "")

            def set_editor_property(self, prop, value):
                self.writes.append((prop, value))
                self.values[prop] = value

        empty = {
            "enter_effect_config": "",
            "exit_effect_config": "",
            "enter_effect_sound_path": "",
            "exit_effect_sound_path": "",
        }
        clean = Actor(empty)
        self.assertFalse(teleporter_fx.restamp_actor(clean, empty))
        self.assertEqual(clean.writes, list(empty.items()))

        stale = Actor({**empty, "exit_effect_config": "WOTSparks"})
        self.assertTrue(teleporter_fx.restamp_actor(stale, empty))
        self.assertEqual(stale.values, empty)


if __name__ == "__main__":
    unittest.main()
