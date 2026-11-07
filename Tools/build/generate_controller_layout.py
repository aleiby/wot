#!/usr/bin/env python3
"""Generate the controller layout include from its two CSV sources.

The default controller layout and its tuning live in
Source/WoT/Gamepad/Resources/ControllerLayout.csv and ControllerTuning.csv.
The guarded build cooks no data assets, so this script compiles them into
Source/WoT/Gamepad/Generated/WoTControllerLayout.inl, which
WoTGamepadLayout.cpp includes. Unknown names, duplicate bindings and
out-of-range tuning fail here, before a C++ build. ``--check`` (and
test_generate_controller_layout.py) fails when the committed include is stale.

    python3 Tools/build/generate_controller_layout.py           # write
    python3 Tools/build/generate_controller_layout.py --check   # verify only
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import math
import sys
from pathlib import Path
from typing import Dict, List, Tuple

REPO = Path(__file__).resolve().parents[2]
LAYOUT_RELATIVE = "Source/WoT/Gamepad/Resources/ControllerLayout.csv"
TUNING_RELATIVE = "Source/WoT/Gamepad/Resources/ControllerTuning.csv"
OUTPUT_RELATIVE = "Source/WoT/Gamepad/Generated/WoTControllerLayout.inl"

# TeamSelect overlays Gameplay while a Battle player has no team.
CONTEXTS = ("Gameplay", "CitadelEditor", "TeamSelect")
FULL_CONTEXTS = ("Gameplay", "CitadelEditor")
STICKS = ("LeftStick", "RightStick")
INPUTS = STICKS + ("LeftTrigger", "RightTrigger", "FaceBottom", "FaceRight",
                   "FaceLeft", "FaceTop", "LeftShoulder", "RightShoulder",
                   "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
                   "LeftThumb", "RightThumb", "View", "Menu")
STICK_ACTIONS = ("Move", "Look")
ACTIONS = STICK_ACTIONS + ("Fire", "Jump", "EditCitadel", "Drop", "DropTainted",
                           "ItemInfo", "PrevItem", "NextItem", "PrevHand",
                           "NextHand", "ViewUp", "ViewDown", "RandomTaunt",
                           "WalkToggle", "CenterView", "Scoreboard", "Menu",
                           "TurnAround", "JoinTeam1", "JoinTeam2", "JoinTeam3",
                           "JoinTeam4")
TRIGGERS = {"": "Press", "Tap": "Tap", "Hold": "Hold"}
REQUIRED_ACTIONS = ("Move", "Look", "Menu")
# FWoTGamepadTuning's field order.
TUNING = ("OuterZone", "MoveGain", "YawRate", "PitchRate", "LookExponent",
          "DropHoldSeconds", "TurnAroundSeconds")
# The largest dead zone the settings accept (FWoTGameSettings::Validate).
MAX_DEAD_ZONE = 0.35


class LayoutError(ValueError):
    """A source row the game would not accept."""


def _text(data: bytes) -> str:
    """Source text with LF line endings, so a CRLF checkout renders the same."""
    return data.decode("utf-8").replace("\r\n", "\n")


def _rows(text: str, header: Tuple[str, ...], name: str) -> List[Dict[str, str]]:
    rows = [row for row in csv.reader(io.StringIO(text))
            if row and any(cell.strip() for cell in row)]
    if not rows or tuple(cell.strip() for cell in rows[0]) != header:
        raise LayoutError(f"{name}: the header must be {','.join(header)}")
    out = []
    for number, row in enumerate(rows[1:], start=2):
        if len(row) != len(header):
            raise LayoutError(f"{name}:{number}: expected {len(header)} columns")
        out.append({key: cell.strip() for key, cell in zip(header, row)})
    return out


def parse_layout(text: str) -> List[Tuple[str, str, str, str]]:
    bindings: List[Tuple[str, str, str, str]] = []
    input_triggers: Dict[Tuple[str, str], List[str]] = {}
    actions = set()
    action_trigger: Dict[str, str] = {}
    rows = _rows(text, ("Context", "Input", "Action", "Trigger"), "layout")
    for number, row in enumerate(rows, start=2):
        where = f"layout:{number}"
        context, key, action, trigger = (row["Context"], row["Input"],
                                         row["Action"], row["Trigger"])
        if context not in CONTEXTS:
            raise LayoutError(f"{where}: unknown context {context!r}")
        if key not in INPUTS:
            raise LayoutError(f"{where}: unknown input {key!r}")
        if action not in ACTIONS:
            raise LayoutError(f"{where}: unknown action {action!r}")
        if trigger not in TRIGGERS:
            raise LayoutError(f"{where}: unknown trigger {trigger!r}")
        if (key in STICKS) != (action in STICK_ACTIONS):
            raise LayoutError(f"{where}: sticks drive Move and Look only, and only sticks do")
        if key in STICKS and trigger:
            raise LayoutError(f"{where}: a stick takes no trigger")
        if (context, action) in actions:
            raise LayoutError(f"{where}: {action} is bound twice in {context}")
        actions.add((context, action))
        if action_trigger.setdefault(action, trigger) != trigger:
            raise LayoutError(f"{where}: {action} must use one trigger in every context")
        triggers = input_triggers.setdefault((context, key), [])
        triggers.append(trigger)
        if len(triggers) > 1 and sorted(triggers) != ["Hold", "Tap"]:
            raise LayoutError(f"{where}: {key} is bound twice in {context}")
        bindings.append((context, key, action, TRIGGERS[trigger]))
    for context in FULL_CONTEXTS:
        for action in REQUIRED_ACTIONS:
            if (context, action) not in actions:
                raise LayoutError(f"layout: {context} has no {action}")
    return bindings


def parse_tuning(text: str) -> Dict[str, float]:
    values: Dict[str, float] = {}
    for number, row in enumerate(_rows(text, ("Name", "Value", "Note"), "tuning"), start=2):
        name = row["Name"]
        if name not in TUNING:
            raise LayoutError(f"tuning:{number}: unknown name {name!r}")
        if name in values:
            raise LayoutError(f"tuning:{number}: {name} appears twice")
        try:
            value = float(row["Value"])
        except ValueError:
            raise LayoutError(f"tuning:{number}: {row['Value']!r} is not a number") from None
        if not math.isfinite(value):
            raise LayoutError(f"tuning:{number}: {name} must be finite")
        values[name] = value
    missing = [name for name in TUNING if name not in values]
    if missing:
        raise LayoutError("tuning: missing " + ", ".join(missing))
    if not MAX_DEAD_ZONE < values["OuterZone"] <= 1.0:
        raise LayoutError(f"tuning: OuterZone must be above {MAX_DEAD_ZONE} and at most 1")
    if values["MoveGain"] < 1.0 or values["LookExponent"] < 1.0:
        raise LayoutError("tuning: MoveGain and LookExponent must be at least 1")
    for name in ("YawRate", "PitchRate", "DropHoldSeconds", "TurnAroundSeconds"):
        if values[name] <= 0.0:
            raise LayoutError(f"tuning: {name} must be positive")
    return values


def render(layout_data: bytes, tuning_data: bytes) -> bytes:
    """The include for the two sources: identical input always gives identical bytes."""
    layout_text, tuning_text = _text(layout_data), _text(tuning_data)
    bindings = parse_layout(layout_text)
    tuning = parse_tuning(tuning_text)
    lines = [
        "// Generated by Tools/build/generate_controller_layout.py; do not edit.",
        f"// Source: {LAYOUT_RELATIVE}",
        f"// SHA-256: {hashlib.sha256(layout_text.encode('utf-8')).hexdigest()}",
        f"// Source: {TUNING_RELATIVE}",
        f"// SHA-256: {hashlib.sha256(tuning_text.encode('utf-8')).hexdigest()}",
        "//",
        "// The default controller layout and its tuning (hashes cover LF-normalized",
        "// text). Included once, by WoTGamepadLayout.cpp.",
        "#pragma once",
        "",
        "namespace WoTControllerLayoutGenerated",
        "{",
        "static const FWoTGamepadBinding Bindings[] = {",
    ]
    for context, key, action, trigger in bindings:
        lines.append(f"\t{{EWoTGamepadContext::{context}, EWoTGamepadInput::{key}, "
                     f"EWoTGamepadAction::{action}, EWoTGamepadTrigger::{trigger}}},")
    lines += ["};", "", "static const FWoTGamepadTuning Tuning = {"]
    for name in TUNING:
        lines.append(f"\t/*{name}*/ {float(tuning[name])!r}f,")
    lines += ["};", "} // namespace WoTControllerLayoutGenerated", ""]
    return "\n".join(lines).encode("utf-8")


def _render_root(root: Path) -> bytes:
    return render((root / LAYOUT_RELATIVE).read_bytes(), (root / TUNING_RELATIVE).read_bytes())


def check(root: Path = REPO) -> bool:
    output = root / OUTPUT_RELATIVE
    if not output.exists():
        return False
    return output.read_bytes().replace(b"\r\n", b"\n") == _render_root(root)


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="verify the committed include instead of writing it")
    args = parser.parse_args(argv)
    try:
        expected = _render_root(REPO)
    except LayoutError as error:
        print(f"controller layout rejected: {error}", file=sys.stderr)
        return 1
    if args.check:
        if not check(REPO):
            print(f"controller layout include is stale: {OUTPUT_RELATIVE}", file=sys.stderr)
            return 1
        print(f"controller layout include OK: {OUTPUT_RELATIVE}")
        return 0
    output = REPO / OUTPUT_RELATIVE
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(expected)
    print(f"wrote {OUTPUT_RELATIVE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
