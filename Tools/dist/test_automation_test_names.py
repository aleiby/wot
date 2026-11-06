"""UE automation silently skips a test whose full name is also a group.

When `WoT.A.B` is registered and so is `WoT.A.B.C`, the automation tree turns
`WoT.A.B` into a group node and never runs the test registered under that
name, in a filtered run or the full suite (found 2026-10-07: three specs had
not run since they gained children). Every registered name must be a leaf.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TEST_NAME = re.compile(
    r"IMPLEMENT_(?:CUSTOM_)?(?:SIMPLE|COMPLEX)_AUTOMATION_TEST\s*\(\s*\w+\s*,"
    r"\s*(?:\w+\s*,\s*)?\"([^\"]+)\"",
    re.S)
SPEC_NAME = re.compile(r"BEGIN_DEFINE_SPEC\s*\(\s*\w+\s*,\s*\"([^\"]+)\"", re.S)


def registered_names(root: Path) -> dict[str, Path]:
    names: dict[str, Path] = {}
    for path in sorted(root.rglob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in TEST_NAME.finditer(text):
            names[match.group(1)] = path
    return names


def spec_names(root: Path) -> set[str]:
    names: set[str] = set()
    for path in sorted(root.rglob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        names.update(match.group(1) for match in SPEC_NAME.finditer(text))
    return names


def shadowed(names: dict[str, Path], groups: set[str]) -> list[str]:
    prefixes = set(names) | groups
    return sorted(
        name for name in names
        if any(other.startswith(name + ".") for other in prefixes))


class AutomationTestNamesTests(unittest.TestCase):
    def test_detector_finds_a_shadowed_name(self):
        names = {"WoT.A.B": Path("a.cpp"), "WoT.A.B.C": Path("b.cpp"),
                 "WoT.A.BC": Path("c.cpp")}
        self.assertEqual(shadowed(names, set()), ["WoT.A.B"])
        self.assertEqual(shadowed({"WoT.X": Path("x.cpp")}, {"WoT.X.Y"}),
                         ["WoT.X"])

    def test_every_registered_test_is_a_leaf(self):
        names = registered_names(REPO / "Source")
        self.assertGreater(len(names), 1000)
        found = shadowed(names, spec_names(REPO / "Source"))
        self.assertEqual(
            found, [],
            "these tests never run because another test sits under their "
            "name; give each a leaf name: "
            + ", ".join(f"{name} ({names[name].relative_to(REPO)})"
                        for name in found))

    def test_retired_original_game_download_literals_are_absent_from_source(self):
        forbidden = (
            "DownloadingOriginalGame",
            "OriginalGameDownloadBytes",
            "InstallerUrl",
            "WOT_BOOTSTRAP_TEST_INSTALLER_URL",
            "WoTAcquirePinnedInstaller",
            "twot_gog_classic",
            "archive.org",
        )
        found = []
        for path in sorted((REPO / "Source").rglob("*")):
            if path.suffix not in {".h", ".cpp", ".mm"}:
                continue
            source = path.read_text(encoding="utf-8", errors="replace")
            for literal in forbidden:
                if literal in source:
                    found.append(f"{literal}: {path.relative_to(REPO)}")
        self.assertEqual(
            found, [],
            "retired original-game download literals remain in shipped source: "
            + "; ".join(found))


if __name__ == "__main__":
    unittest.main()
