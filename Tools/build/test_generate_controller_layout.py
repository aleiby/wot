#!/usr/bin/env python3
"""Specs for generate_controller_layout.py and the include it committed.

The committed include must be exactly what the generator writes from the
committed CSVs, so a layout edited without regenerating fails here.
"""
from __future__ import annotations

import ast
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_controller_layout as gen  # noqa: E402

# LF-normalized, so the row edits below match on a CRLF (Windows) checkout too;
# the generator hashes LF-normalized text either way.
LAYOUT = (gen.REPO / gen.LAYOUT_RELATIVE).read_bytes().replace(b"\r\n", b"\n")
TUNING = (gen.REPO / gen.TUNING_RELATIVE).read_bytes().replace(b"\r\n", b"\n")


def _replace(data: bytes, old: str, new: str) -> bytes:
    text = data.decode("utf-8")
    assert old in text, old
    return text.replace(old, new, 1).encode("utf-8")


class GeneratorTests(unittest.TestCase):
    def test_committed_include_matches_its_sources(self):
        committed = (gen.REPO / gen.OUTPUT_RELATIVE).read_bytes().replace(b"\r\n", b"\n")
        self.assertEqual(gen.render(LAYOUT, TUNING), committed)

    def test_render_is_deterministic_and_line_ending_neutral(self):
        crlf = LAYOUT.replace(b"\n", b"\r\n")
        self.assertEqual(gen.render(LAYOUT, TUNING), gen.render(crlf, TUNING))

    def test_rows_reference_the_cpp_enums(self):
        text = gen.render(LAYOUT, TUNING).decode()
        self.assertIn("{EWoTGamepadContext::Gameplay, EWoTGamepadInput::LeftStick, "
                      "EWoTGamepadAction::Move, EWoTGamepadTrigger::Press},", text)
        self.assertIn("EWoTGamepadAction::DropTainted, EWoTGamepadTrigger::Hold},", text)

    def test_tuning_literals_are_valid_cpp_floats(self):
        text = gen.render(LAYOUT, TUNING).decode()
        self.assertIn("/*YawRate*/ 220.0f,", text)
        self.assertIn("/*MoveGain*/ 4.7f,", text)

    def test_turn_around_is_a_known_action_with_its_tuning(self):
        text = gen.render(LAYOUT, TUNING).decode()
        self.assertIn("{EWoTGamepadContext::Gameplay, EWoTGamepadInput::LeftTrigger, "
                      "EWoTGamepadAction::TurnAround, EWoTGamepadTrigger::Press},", text)
        self.assertIn("/*TurnAroundSeconds*/ 0.15f,", text)

    def test_team_select_overlays_the_dpad_with_the_four_teams(self):
        text = gen.render(LAYOUT, TUNING).decode()
        for key, team in (("DPadLeft", 1), ("DPadRight", 2), ("DPadUp", 3), ("DPadDown", 4)):
            self.assertIn("{EWoTGamepadContext::TeamSelect, EWoTGamepadInput::%s, "
                          "EWoTGamepadAction::JoinTeam%d, EWoTGamepadTrigger::Press}," % (key, team),
                          text)

    def test_the_team_select_overlay_needs_no_move_look_or_menu(self):
        # It sits on top of Gameplay, which keeps the sticks and the menu.
        layout = LAYOUT.decode()
        self.assertNotIn("TeamSelect,LeftStick", layout)
        self.assertNotIn("TeamSelect,Menu", layout)
        gen.render(LAYOUT, TUNING)

    def test_unknown_names_are_refused(self):
        for old, new in [("Gameplay,FaceTop,ItemInfo,", "Gameplay,FaceTip,ItemInfo,"),
                         ("Gameplay,FaceTop,ItemInfo,", "Gameplay,FaceTop,ItemInfos,"),
                         ("Gameplay,FaceTop,ItemInfo,", "Gamplay,FaceTop,ItemInfo,"),
                         ("Gameplay,FaceTop,ItemInfo,", "Gameplay,FaceTop,ItemInfo,Double")]:
            with self.subTest(new=new), self.assertRaises(gen.LayoutError):
                gen.render(_replace(LAYOUT, old, new), TUNING)

    def test_an_input_bound_twice_in_a_context_is_refused_unless_tap_and_hold(self):
        twice = _replace(LAYOUT, "Gameplay,DPadDown,RandomTaunt,",
                         "Gameplay,DPadDown,RandomTaunt,\nGameplay,DPadDown,ItemInfo,")
        with self.assertRaises(gen.LayoutError):
            gen.render(twice, TUNING)
        gen.render(LAYOUT, TUNING)  # FaceLeft Tap + Hold is allowed

    def test_an_action_bound_twice_in_a_context_is_refused(self):
        twice = _replace(LAYOUT, "Gameplay,DPadDown,RandomTaunt,",
                         "Gameplay,DPadDown,RandomTaunt,\nGameplay,DPadUp,RandomTaunt,")
        with self.assertRaises(gen.LayoutError):
            gen.render(twice, TUNING)

    def test_sticks_drive_only_move_and_look(self):
        for old, new in [("Gameplay,LeftStick,Move,", "Gameplay,LeftStick,Jump,"),
                         ("Gameplay,FaceBottom,Jump,", "Gameplay,FaceBottom,Move,"),
                         ("Gameplay,LeftStick,Move,", "Gameplay,LeftStick,Move,Tap")]:
            with self.subTest(new=new), self.assertRaises(gen.LayoutError):
                gen.render(_replace(LAYOUT, old, new), TUNING)

    def test_every_context_needs_move_look_and_menu(self):
        with self.assertRaises(gen.LayoutError):
            gen.render(_replace(LAYOUT, "CitadelEditor,Menu,Menu,\n", ""), TUNING)

    def test_an_action_keeps_one_trigger_across_contexts(self):
        mixed = _replace(LAYOUT, "CitadelEditor,DPadUp,ViewUp,",
                         "CitadelEditor,DPadUp,ViewUp,\nCitadelEditor,FaceLeft,Drop,")
        with self.assertRaises(gen.LayoutError):
            gen.render(mixed, TUNING)

    def test_tuning_is_complete_and_in_range(self):
        for old, new in [("OuterZone,0.95,", "OuterZone,0.3,"),
                         ("MoveGain,4.7,", "MoveGain,0.5,"),
                         ("YawRate,220,", "YawRate,0,"),
                         ("LookExponent,2.0,", "LookExponent,nan,"),
                         ("TurnAroundSeconds,0.15,", "TurnAroundSeconds,0,"),
                         ("HoldSeconds,0.5,", "HoldSeconds,0,"),
                         ("HoldSeconds,0.5,", "DropHoldSeconds,0.5,")]:
            with self.subTest(new=new), self.assertRaises(gen.LayoutError):
                gen.render(LAYOUT, _replace(TUNING, old, new))
        without_hold = "\n".join(line for line in TUNING.decode().split("\n")
                                 if not line.startswith("HoldSeconds,"))
        with self.assertRaisesRegex(gen.LayoutError, "missing HoldSeconds"):
            gen.render(LAYOUT, without_hold.encode())

    def test_dpad_up_taps_chat_and_holds_team_chat(self):
        text = gen.render(LAYOUT, TUNING).decode()
        self.assertIn("{EWoTGamepadContext::Gameplay, EWoTGamepadInput::DPadUp, "
                      "EWoTGamepadAction::Chat, EWoTGamepadTrigger::Tap},", text)
        self.assertIn("{EWoTGamepadContext::Gameplay, EWoTGamepadInput::DPadUp, "
                      "EWoTGamepadAction::TeamChat, EWoTGamepadTrigger::Hold},", text)
        # One hold time for every Tap/Hold pair (FaceLeft and D-pad up).
        self.assertIn("/*HoldSeconds*/ 0.5f,", text)
        self.assertNotIn("DropHoldSeconds", text)

    def test_team_select_and_the_editor_keep_dpad_up(self):
        text = gen.render(LAYOUT, TUNING).decode()
        self.assertIn("{EWoTGamepadContext::TeamSelect, EWoTGamepadInput::DPadUp, "
                      "EWoTGamepadAction::JoinTeam3, EWoTGamepadTrigger::Press},", text)
        self.assertIn("{EWoTGamepadContext::CitadelEditor, EWoTGamepadInput::DPadUp, "
                      "EWoTGamepadAction::ViewUp, EWoTGamepadTrigger::Press},", text)
        for context in ("CitadelEditor", "TeamSelect"):
            for action in ("Chat", "TeamChat"):
                self.assertNotIn("{EWoTGamepadContext::%s, EWoTGamepadInput::DPadUp, "
                                 "EWoTGamepadAction::%s," % (context, action), text)

    def test_dpad_up_takes_no_third_gameplay_binding(self):
        third = _replace(LAYOUT, "Gameplay,DPadUp,TeamChat,Hold",
                         "Gameplay,DPadUp,TeamChat,Hold\nGameplay,DPadUp,ViewUp,")
        with self.assertRaisesRegex(gen.LayoutError, "DPadUp is bound twice in Gameplay"):
            gen.render(third, TUNING)
        swapped = _replace(LAYOUT, "Gameplay,DPadUp,Chat,Tap", "Gameplay,DPadUp,Chat,")
        with self.assertRaises(gen.LayoutError):
            gen.render(swapped, TUNING)

    def test_sources_parse_as_python_3_9(self):
        # The Mac host pipeline runs Python 3.9: no 3.10+ syntax here.
        here = Path(__file__).resolve().parent
        for name in ("generate_controller_layout.py", "test_generate_controller_layout.py"):
            with self.subTest(name=name):
                source = (here / name).read_text(encoding="utf-8")
                ast.parse(source, filename=name, feature_version=(3, 9))

    def test_check_detects_a_stale_include(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel in (gen.LAYOUT_RELATIVE, gen.TUNING_RELATIVE, gen.OUTPUT_RELATIVE):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(gen.REPO / rel, root / rel)
            self.assertTrue(gen.check(root))
            layout = root / gen.LAYOUT_RELATIVE
            layout.write_bytes(_replace(layout.read_bytes(), "Gameplay,DPadDown,RandomTaunt",
                                        "Gameplay,DPadDown,ViewDown"))
            self.assertFalse(gen.check(root))


if __name__ == "__main__":
    unittest.main()
