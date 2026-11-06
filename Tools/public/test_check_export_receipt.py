"""Tests for check_export_receipt; every fixture is a synthetic Git repository."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import check_export_receipt  # noqa: E402

FILES = {
    "README.md": (b"hello\n", "100644"),
    "Tools/run.sh": (b"#!/bin/sh\necho run\n", "100755"),
}


class CheckExportReceiptTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.repo = Path(self._temp.name)
        self.git("init", "-q")
        self.git("config", "core.autocrlf", "false")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "test")

    def tearDown(self):
        self._temp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args],
                              capture_output=True, check=True).stdout

    def receipt(self, files=FILES, **overrides):
        document = {
            "file_count": len(files),
            "files": [{"mode": mode, "path": path,
                       "sha256": hashlib.sha256(data).hexdigest()}
                      for path, (data, mode) in sorted(files.items())],
        }
        document.update(overrides)
        return json.dumps(document, indent=2).encode("utf-8")

    def commit(self, files, receipt):
        for path, (data, mode) in files.items():
            target = self.repo / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            self.git("add", "--", path)
            # Set the mode in the index explicitly: the test must not depend
            # on the host filesystem having executable bits.
            self.git("update-index", "--chmod=" + ("+x" if mode == "100755" else "-x"),
                     "--", path)
        if receipt is not None:
            (self.repo / check_export_receipt.RECEIPT).write_bytes(receipt)
            self.git("add", "--", check_export_receipt.RECEIPT)
        self.git("commit", "-q", "-m", "export")

    def test_a_tree_that_matches_its_receipt_passes(self):
        self.commit(FILES, self.receipt())
        self.assertEqual(check_export_receipt.check(self.repo), [])
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(check_export_receipt.main(["--tree", str(self.repo)]), 0)
        self.assertIn("matches its export receipt", out.getvalue())

    def test_a_dropped_executable_bit_fails(self):
        files = dict(FILES)
        files["Tools/run.sh"] = (FILES["Tools/run.sh"][0], "100644")
        self.commit(files, self.receipt())
        self.assertEqual(check_export_receipt.check(self.repo),
                         ["Tools/run.sh: mode 100644 in the tree, 100755 in the receipt"])
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(check_export_receipt.main(["--tree", str(self.repo)]), 1)

    def test_changed_content_fails(self):
        files = dict(FILES)
        files["README.md"] = (b"hello\r\n", "100644")
        self.commit(files, self.receipt())
        self.assertEqual(check_export_receipt.check(self.repo),
                         ["README.md: content sha256 differs from the receipt"])

    def test_missing_and_extra_files_fail(self):
        files = {"README.md": FILES["README.md"],
                 "notes.txt": (b"not exported\n", "100644")}
        self.commit(files, self.receipt())
        self.assertEqual(check_export_receipt.check(self.repo), [
            "Tools/run.sh: listed in the receipt but missing from the tree",
            "notes.txt: in the tree but not in the receipt",
        ])

    def test_a_wrong_file_count_fails(self):
        self.commit(FILES, self.receipt(file_count=3))
        self.assertEqual(check_export_receipt.check(self.repo), [
            "PUBLIC_EXPORT_RECEIPT.json file_count 3 does not match its 2 entries "
            "(2 distinct paths)"])

    def test_a_tree_without_a_receipt_fails(self):
        self.commit(FILES, None)
        self.assertEqual(check_export_receipt.check(self.repo),
                         ["PUBLIC_EXPORT_RECEIPT.json is not in the tree"])

    def test_export_commits_only_skips_contributor_commits(self):
        self.commit(FILES, self.receipt())
        self.assertTrue(check_export_receipt.is_export_commit(self.repo))
        files = dict(FILES)
        files["README.md"] = (b"a contributor's edit\n", "100644")
        self.commit(files, None)
        self.assertFalse(check_export_receipt.is_export_commit(self.repo))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(check_export_receipt.main(
                ["--tree", str(self.repo), "--export-commits-only"]), 0)
        self.assertIn("skipped", out.getvalue())
        # The next export sync rewrites the receipt, so it is checked again.
        self.commit(files, self.receipt(files))
        self.assertTrue(check_export_receipt.is_export_commit(self.repo))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(check_export_receipt.main(
                ["--tree", str(self.repo), "--export-commits-only"]), 0)
        self.assertIn("matches its export receipt", out.getvalue())

    def test_an_export_commit_with_a_dropped_bit_still_fails_when_scoped(self):
        files = dict(FILES)
        files["Tools/run.sh"] = (FILES["Tools/run.sh"][0], "100644")
        self.commit(files, self.receipt())
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(check_export_receipt.main(
                ["--tree", str(self.repo), "--export-commits-only"]), 1)

    def test_an_unreadable_receipt_is_an_error(self):
        self.commit(FILES, b"{not json")
        with contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(check_export_receipt.main(["--tree", str(self.repo)]), 2)
        self.assertIn("unreadable", err.getvalue())


if __name__ == "__main__":
    unittest.main()
