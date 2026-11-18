#!/usr/bin/env python3
"""Tests for prune_stale_module_intermediates.py.

The receipt gate (record_build_receipt.py) must read exactly the Definitions.h
files a clean build would leave. These tests pin that the current link
response file is the authority, that stale module directories -- including a
stale Definitions.h that would otherwise satisfy the gate -- are removed, and
that anything unprovable fails closed.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prune_stale_module_intermediates as prune  # noqa: E402
import record_build_receipt as recorder  # noqa: E402


class PruneTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.project = Path(self._tmp.name) / "BuildRoot"
        self.config = (self.project / "Intermediate" / "Build" / "Win64"
                       / "x64" / "WoT" / "Test")

    def module(self, name: str, definitions: str | None = None) -> Path:
        directory = self.config / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / f"Module.{name}.cpp.obj").write_bytes(b"obj")
        if definitions is not None:
            (directory / "Definitions.h").write_text(definitions)
        return directory

    def link(self, modules, name="WoT-Win64-Test.exe.rsp") -> Path:
        self.config.mkdir(parents=True, exist_ok=True)
        lines = ["/NOLOGO", "/DEBUG:FULL",
                 '/LIBPATH:"C:/Program Files/x/lib"']
        for module in modules:
            path = (self.config / module / f"Module.{module}.cpp.obj")
            lines.append(f'"{path.as_posix()}"')
        lines.append('"G:/UnrealEngine/Engine/Lib/other/Test/Nope/x.lib"')
        lines.append('/OUT:"G:/x/Binaries/Win64/WoT-Win64-Test.exe"')
        response = self.config / name
        response.write_text("\n".join(lines) + "\n")
        # Resource response files sit beside the link one and are ignored.
        (self.config / "WoT-Win64-Test-Default.rc2.res.rsp").write_text("x")
        return response

    def test_stale_module_and_its_definitions_are_removed(self):
        self.module("Core", "#define WOT_GUARDED_BOOTSTRAP 1\n")
        self.module("WoT")
        stale = self.module(
            "OldModule",
            "#define WOT_GUARDED_BOOTSTRAP 1\n"
            "#define WOT_BOOTSTRAP_TEST_IDENTITIES 1\n")
        self.link(["Core", "WoT"])
        summary = prune.prune(self.project, "WoT", "Win64", "Test")
        self.assertFalse(stale.exists())
        self.assertTrue((self.config / "Core" / "Definitions.h").is_file())
        row = summary["Intermediate/Build/Win64/x64/WoT/Test"]
        self.assertEqual(row["linked_modules"], 2)
        self.assertEqual(row["pruned"], ["OldModule"])
        # The gate now sees only the current build's evidence.
        definitions, count = recorder.collect_definitions(
            self.project, "WoT", "Win64", "Test")
        self.assertEqual(count, 1)
        self.assertNotIn("WOT_BOOTSTRAP_TEST_IDENTITIES", definitions)

    def test_a_clean_build_prunes_nothing(self):
        for name in ("Core", "Engine", "WoT"):
            self.module(name)
        self.link(["Core", "Engine", "WoT"])
        summary = prune.prune(self.project, "WoT", "Win64", "Test")
        self.assertEqual(
            summary["Intermediate/Build/Win64/x64/WoT/Test"]["pruned"], [])

    def test_windows_backslash_and_relative_paths_are_understood(self):
        self.module("Core")
        self.module("Stale")
        self.config.mkdir(parents=True, exist_ok=True)
        (self.config / "WoT-Mac-Test.rsp").unlink(missing_ok=True)
        response = self.config / "WoT-Win64-Test.exe.rsp"
        response.write_text(
            '"..\\..\\..\\BuildRoot\\Intermediate\\Build\\Win64\\x64\\WoT\\'
            'Test\\Core\\Module.Core.cpp.obj"\n')
        prune.prune(self.project, "WoT", "Win64", "Test")
        self.assertTrue((self.config / "Core").is_dir())
        self.assertFalse((self.config / "Stale").exists())

    def test_mac_link_response_name(self):
        config = (self.project / "Intermediate" / "Build" / "Mac" / "arm64"
                  / "WoT" / "Shipping")
        (config / "Core").mkdir(parents=True)
        (config / "Gone").mkdir()
        (config / "WoT-Mac-Shipping.rsp").write_text(
            f'"{(config / "Core" / "Module.Core.cpp.o").as_posix()}"\n'
            '-o "Binaries/Mac/WoT-Mac-Shipping"\n')
        prune.prune(self.project, "WoT", "Mac", "Shipping")
        self.assertTrue((config / "Core").is_dir())
        self.assertFalse((config / "Gone").exists())

    def test_unprovable_module_sets_fail_closed(self):
        with self.subTest("no intermediates"):
            with self.assertRaisesRegex(prune.PruneRejected, "build first"):
                prune.prune(self.project, "WoT", "Win64", "Test")
        self.module("Core")
        with self.subTest("no link response file"):
            with self.assertRaisesRegex(prune.PruneRejected, "exactly one"):
                prune.prune(self.project, "WoT", "Win64", "Test")
        with self.subTest("ambiguous link response files"):
            self.link(["Core"])
            self.link(["Core"], name="WoT-Win64-Test.rsp")
            with self.assertRaisesRegex(prune.PruneRejected, "exactly one"):
                prune.prune(self.project, "WoT", "Win64", "Test")
            (self.config / "WoT-Win64-Test.rsp").unlink()
        with self.subTest("link names no module"):
            self.link([])
            with self.assertRaisesRegex(prune.PruneRejected, "no module"):
                prune.prune(self.project, "WoT", "Win64", "Test")
        with self.subTest("link names a module with no directory"):
            self.link(["Core", "Missing"])
            with self.assertRaisesRegex(prune.PruneRejected, "Missing"):
                prune.prune(self.project, "WoT", "Win64", "Test")
        self.assertTrue((self.config / "Core").is_dir())

    def test_main_reports_and_rejects(self):
        self.module("Core")
        self.link(["Core"])
        arguments = ["--project", str(self.project / "WoT.uproject"),
                     "--target", "WoT", "--platform", "Win64",
                     "--configuration", "Test"]
        self.assertEqual(prune.main(arguments), 0)
        arguments[-1] = "Shipping"
        self.assertEqual(prune.main(arguments), 1)


if __name__ == "__main__":
    unittest.main()
