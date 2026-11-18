#!/usr/bin/env python3
"""Specs for steamos_installer_asset.py: the SteamOS installer is rebuilt from
git at a commit, with that commit's own builder, never from the worktree."""
from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_formats as formats  # noqa: E402
import steamos_installer_asset as steamos  # noqa: E402

INSTALLER = formats.REPOSITORY_ROOT / "Tools" / "deck" / "installer"


def _load_builder(directory: Path):
    spec = importlib.util.spec_from_file_location(
        "steamos_installer_builder_under_test", directory / "build_desktop_entry.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Repository:
    def __init__(self, root: Path):
        self.root = root
        root.mkdir(parents=True)
        self.git("init", "-q")
        self.git("config", "user.email", "spec@example.invalid")
        self.git("config", "user.name", "spec")

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.root), *args], check=True,
                              capture_output=True, text=True).stdout.strip()

    def commit(self, message: str) -> str:
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)
        return self.git("rev-parse", "HEAD")

    def copy_installer(self) -> Path:
        destination = self.root / steamos.SOURCE_DIRECTORY
        shutil.copytree(INSTALLER, destination,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        return destination


class BuildAtCommitTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.repository = Repository(Path(self.temporary.name) / "repo")

    def tearDown(self):
        self.temporary.cleanup()

    def test_the_asset_name_is_a_plain_release_asset_name(self):
        self.assertEqual(steamos.ASSET_NAME, "Install-WoT-Reborn.desktop")
        self.assertEqual(formats.validate_asset_name(steamos.ASSET_NAME), steamos.ASSET_NAME)

    def test_builds_the_committed_installer_with_its_own_builder(self):
        copied = self.repository.copy_installer()
        commit = self.repository.commit("installer")
        expected = _load_builder(INSTALLER).build_entry(INSTALLER)
        self.assertEqual(steamos.build_at_commit(self.repository.root, commit), expected)

        # The worktree never matters: only the commit's objects do.
        program = copied / "install_wot_reborn.py"
        program.write_bytes(program.read_bytes() + b"\n# changed after the commit\n")
        (copied / "art" / "hero.jpg").unlink()
        self.assertEqual(steamos.build_at_commit(self.repository.root, commit), expected)

        # A later commit missing an art file cannot build; one that changes the
        # program builds different bytes; the first commit still builds the same.
        missing_art = self.repository.commit("art removed")
        with self.assertRaisesRegex(steamos.SteamOSInstallerError, "failed"):
            steamos.build_at_commit(self.repository.root, missing_art)
        shutil.copyfile(INSTALLER / "art" / "hero.jpg", copied / "art" / "hero.jpg")
        changed = self.repository.commit("program changed")
        self.assertNotEqual(steamos.build_at_commit(self.repository.root, changed), expected)
        self.assertEqual(steamos.build_at_commit(self.repository.root, commit[:12]), expected)

    def test_the_builder_at_the_commit_is_the_one_that_runs(self):
        directory = self.repository.root / steamos.SOURCE_DIRECTORY
        directory.mkdir(parents=True)
        (directory / steamos.BUILDER).write_text(
            "import argparse, pathlib\n"
            "parser = argparse.ArgumentParser()\n"
            "parser.add_argument('--out', type=pathlib.Path, required=True)\n"
            "args = parser.parse_args()\n"
            "args.out.write_bytes(b'[Desktop Entry]\\nName=from the commit\\n')\n",
            encoding="utf-8")
        commit = self.repository.commit("a different builder")
        self.assertEqual(steamos.build_at_commit(self.repository.root, commit),
                         b"[Desktop Entry]\nName=from the commit\n")

    def test_a_failing_builder_is_reported(self):
        directory = self.repository.root / steamos.SOURCE_DIRECTORY
        directory.mkdir(parents=True)
        (directory / steamos.BUILDER).write_text(
            "import sys\nsys.stderr.write('art/hero.jpg is missing')\nsys.exit(2)\n",
            encoding="utf-8")
        commit = self.repository.commit("broken builder")
        with self.assertRaisesRegex(steamos.SteamOSInstallerError,
                                    r"failed \(exit status 2\): art/hero.jpg is missing"):
            steamos.build_at_commit(self.repository.root, commit)

    def test_a_commit_before_the_installer_existed_fails_plainly(self):
        (self.repository.root / "README.md").write_text("x\n", encoding="utf-8")
        commit = self.repository.commit("before the installer")
        with self.assertRaisesRegex(steamos.SteamOSInstallerError, "predates"):
            steamos.build_at_commit(self.repository.root, commit)

    def test_has_source_tells_older_build_commits_from_newer_ones(self):
        (self.repository.root / "README.md").write_text("x\n", encoding="utf-8")
        older = self.repository.commit("before the installer")
        directory = self.repository.root / steamos.SOURCE_DIRECTORY
        directory.mkdir(parents=True)
        (directory / "vdf.py").write_text("# not the builder\n", encoding="utf-8")
        still_older = self.repository.commit("installer folder without its builder")
        (directory / steamos.BUILDER).write_text("# builder\n", encoding="utf-8")
        newer = self.repository.commit("the builder")
        self.assertFalse(steamos.has_source(self.repository.root, older))
        self.assertFalse(steamos.has_source(self.repository.root, still_older))
        self.assertTrue(steamos.has_source(self.repository.root, newer))
        self.assertTrue(steamos.has_source(self.repository.root, newer[:9]))
        # An unknown commit is an error, never "older".
        with self.assertRaisesRegex(steamos.SteamOSInstallerError, "git ls-tree failed"):
            steamos.has_source(self.repository.root, "f" * 40)
        with self.assertRaisesRegex(steamos.SteamOSInstallerError, "lowercase hex"):
            steamos.has_source(self.repository.root, "HEAD")

    def test_unknown_or_malformed_commits_are_refused(self):
        self.repository.commit("empty")
        with self.assertRaisesRegex(steamos.SteamOSInstallerError, "git ls-tree failed"):
            steamos.build_at_commit(self.repository.root, "f" * 40)
        for commit in ("HEAD", "--output=x", "ABCDEF1", "abc", ""):
            with self.subTest(commit=commit), \
                    self.assertRaisesRegex(steamos.SteamOSInstallerError, "lowercase hex"):
                steamos.build_at_commit(self.repository.root, commit)

    def test_it_is_a_format_error_for_the_release_tools(self):
        self.assertTrue(issubclass(steamos.SteamOSInstallerError, formats.FormatError))


if __name__ == "__main__":
    unittest.main()
