#!/usr/bin/env python3
"""Socket-free specs for editing an already-published notes entry."""
from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_formats as formats  # noqa: E402
import update_release_notes as editor  # noqa: E402


GH = "gh-never-runs"


DEFAULT_BODIES = {
    "v1": "other release sequence 1",
    "v2": ("WoT 2 (release sequence 2, commit abc)\n\n"
           f"{editor.WHATS_NEW_START}\nold body\n"
           f"{editor.WHATS_NEW_END}\n\nInstall steps stay.\n"),
}


class FakeRunner:
    def __init__(self, asset: bytes, bodies=None):
        self.asset = asset
        self.bodies = dict(DEFAULT_BODIES if bodies is None else bodies)
        self.calls = []
        self.uploaded = None
        self.edited_body = None
        self.edited_tag = None

    def __call__(self, command):
        command = list(command)
        self.calls.append(command)

        def done(code=0, stdout="", stderr=""):
            return subprocess.CompletedProcess(command, code, stdout, stderr)

        if command[0:3] == [GH, "release", "download"]:
            directory = Path(command[command.index("--dir") + 1])
            (directory / formats.RELEASE_NOTES_ASSET).write_bytes(self.asset)
            return done()
        if (command[0:3] == [GH, "release", "view"]
                and command[3] == "--repo"):
            self._assert_json_fields(command, "tagName")
            return done(stdout=json.dumps({"tagName": "v-latest"}))
        if command[0:3] == [GH, "release", "list"]:
            self._assert_json_fields(command, "tagName")
            return done(stdout=json.dumps(
                [{"tagName": tag} for tag in self.bodies]))
        if command[0:3] == [GH, "release", "view"] and command[3] in self.bodies:
            self._assert_json_fields(command, "tagName,body")
            return done(stdout=json.dumps({
                "tagName": command[3], "body": self.bodies[command[3]]}))
        if command[0:3] == [GH, "release", "upload"]:
            self.uploaded = Path(command[4]).read_bytes()
            return done()
        if command[0:3] == [GH, "release", "edit"]:
            path = Path(command[command.index("--notes-file") + 1])
            self.edited_body = path.read_text(encoding="utf-8")
            self.edited_tag = command[3]
            return done()
        raise AssertionError(f"unscripted command: {command}")

    @staticmethod
    def _assert_json_fields(command, expected):
        assert command[command.index("--json") + 1] == expected


class UpdateReleaseNotesTests(unittest.TestCase):
    def run_main(self, runner, notes: str, *extra):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "notes.md"
            path.write_bytes(notes.encode("utf-8"))
            with contextlib.redirect_stdout(io.StringIO()) as stdout, \
                    contextlib.redirect_stderr(io.StringIO()) as stderr:
                code = editor.main([
                    "--sequence", "2", "--notes-file", str(path),
                    "--gh", GH, *extra], runner=runner)
            return code, stdout.getvalue() + stderr.getvalue()

    def test_dry_run_replaces_existing_entry_and_prints_writes(self):
        original = formats.merge_release_notes(None, 1, "one")
        original = formats.merge_release_notes(original, 2, "old")
        runner = FakeRunner(original)
        code, text = self.run_main(runner, "new text  \r\nsecond line\r\n")
        self.assertEqual(code, 0, text)
        self.assertIn("DRY RUN", text)
        self.assertIn("release upload v-latest", text)
        self.assertEqual([call[1:3] for call in runner.calls], [
            ["release", "download"], ["release", "view"],
            ["release", "list"], ["release", "view"],
            ["release", "view"],
        ])
        self.assertEqual(runner.calls[1][3], "--repo")
        self.assertIsNone(runner.uploaded)

    def test_publish_uploads_asset_and_rewrites_only_marked_body_section(self):
        original = formats.merge_release_notes(None, 1, "one")
        original = formats.merge_release_notes(original, 2, "old")
        runner = FakeRunner(original)
        code, text = self.run_main(runner, "new text", "--publish")
        self.assertEqual(code, 0, text)
        document = formats.parse_release_notes(runner.uploaded)
        self.assertEqual(document["notes"], [
            {"release_sequence": 1, "text": "one"},
            {"release_sequence": 2, "text": "new text"},
        ])
        self.assertIn(f"{editor.WHATS_NEW_START}\nnew text\n{editor.WHATS_NEW_END}",
                      runner.edited_body)
        self.assertIn("Install steps stay.", runner.edited_body)
        upload = runner.calls[-2]
        self.assertEqual(upload[3], "v-latest")
        self.assertIn("--clobber", upload)

    def test_missing_sequence_is_refused_before_any_write(self):
        runner = FakeRunner(formats.merge_release_notes(None, 1, "one"))
        code, text = self.run_main(runner, "new text", "--publish")
        self.assertEqual(code, 1)
        self.assertIn("does not contain release sequence 2", text)
        self.assertIsNone(runner.uploaded)

    def test_edit_that_no_longer_fits_is_refused_and_keeps_every_entry(self):
        # Fill the asset until one more full entry would not fit, so growing
        # entry 2 to the per-entry maximum must exceed the file limit. The
        # edit must be refused, never satisfied by trimming other releases.
        filler = "x" * 16000
        notes = [{"release_sequence": 1, "text": filler},
                 {"release_sequence": 2, "text": "old"}]
        sequence = 3
        while True:
            candidate = notes + [{"release_sequence": sequence, "text": filler}]
            document = {"format": formats.RELEASE_NOTES_FORMAT, "notes": candidate}
            if (len(candidate) > formats.RELEASE_NOTES_MAX_ENTRIES
                    or len(formats.canonical_bytes(document))
                    > formats.RELEASE_NOTES_MAX_BYTES):
                break
            notes = candidate
            sequence += 1
        original = formats.release_notes_bytes(
            {"format": formats.RELEASE_NOTES_FORMAT, "notes": notes})
        runner = FakeRunner(original)
        code, text = self.run_main(
            runner, "y" * formats.RELEASE_NOTES_MAX_ENTRY_BYTES, "--publish")
        self.assertEqual(code, 1, text)
        self.assertIn("limit", text)
        self.assertIsNone(runner.uploaded)
        self.assertIsNone(runner.edited_body)

    def test_prose_naming_the_sequence_never_selects_a_release(self):
        # A newer release whose notes mention sequence 2, even as a copy of the
        # identity line inside its marked section, is not sequence 2's page.
        original = formats.merge_release_notes(None, 1, "one")
        original = formats.merge_release_notes(original, 2, "old")
        bodies = dict(DEFAULT_BODIES)
        bodies["v3"] = (
            "Fixes a crash introduced in release sequence 2.\n\n"
            f"{editor.WHATS_NEW_START}\n"
            "WoT 3 (release sequence 2, commit abc)\n"
            f"{editor.WHATS_NEW_END}\n\n"
            "WoT 3 (release sequence 3, commit def)\n")
        runner = FakeRunner(original, bodies)
        code, text = self.run_main(runner, "new text", "--publish")
        self.assertEqual(code, 0, text)
        self.assertEqual(runner.edited_tag, "v2")

    def test_a_listing_that_fills_the_limit_is_refused(self):
        original = formats.merge_release_notes(None, 1, "one")
        original = formats.merge_release_notes(original, 2, "old")
        runner = FakeRunner(original)
        limit = editor.RELEASE_LIST_LIMIT
        editor.RELEASE_LIST_LIMIT = len(DEFAULT_BODIES)
        self.addCleanup(setattr, editor, "RELEASE_LIST_LIMIT", limit)
        code, text = self.run_main(runner, "new text", "--publish")
        self.assertEqual(code, 1, text)
        self.assertIn("may be truncated", text)
        self.assertIsNone(runner.uploaded)


if __name__ == "__main__":
    unittest.main()
