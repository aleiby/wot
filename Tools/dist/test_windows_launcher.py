#!/usr/bin/env python3
"""Contract tests for the stable Windows first-install launcher."""

from __future__ import annotations

import csv
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import windows_launcher
import build_stripped_pak as stripped_pak


def _repo_tmp() -> Path:
    """The ignored .tmp folder in the checkout (kept off the system temp
    volume); a fresh clone does not have it yet."""
    path = Path.cwd() / ".tmp"
    path.mkdir(exist_ok=True)
    return path


@unittest.skipUnless(os.name == "nt", "the launcher is a Win64 executable")
class WindowsLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            prefix="wot launcher tests ", dir=_repo_tmp())
        self.root = Path(self.temporary.name) / "Install Root With Spaces" / "WoT Reborn"
        staged = self.root / "Game"
        self.child = windows_launcher.build_test_stub(
            stripped_pak._guarded_main_artifact_path(
                staged, "Win64", "Test"))
        self.launcher = windows_launcher.build_launcher(
            self.root / "WoT Reborn.exe", self.child, "Test",
            test_mode=True, copy_icon=False)

    def tearDown(self):
        self.temporary.cleanup()

    def _environment(self):
        environment = os.environ.copy()
        environment["WOT_LAUNCHER_TEST_ARGS_FILE"] = str(self.root / "arguments.txt")
        environment["WOT_LAUNCHER_TEST_ERROR_FILE"] = str(self.root / "error.txt")
        return environment

    def test_raw_arguments_quotes_and_exit_code_are_forwarded(self):
        suffix = ' alpha "two words" "quote\\\\\"inside" ""'
        environment = self._environment()
        environment["WOT_LAUNCHER_TEST_EXIT_CODE"] = "37"
        completed = subprocess.run(
            f'"{self.launcher}"{suffix}', env=environment,
            capture_output=True, text=True)
        self.assertEqual(completed.returncode, 37)
        command = (self.root / "arguments.txt").read_text(encoding="utf-16")
        self.assertEqual(command, f'"{self.child}"{suffix}')

    def test_missing_game_is_reported_without_fallback(self):
        self.child.unlink()
        completed = subprocess.run([self.launcher], env=self._environment(),
                                   capture_output=True, text=True)
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(
            (self.root / "error.txt").read_text(encoding="utf-16"),
            "WoT Reborn couldn't find Game\\WoT\\Binaries\\Win64\\"
            "WoT-Win64-Test.exe. Re-download the complete WoT Reborn folder "
            "and start it again.")

    def _run(self):
        return subprocess.run([self.launcher], env=self._environment(),
                              capture_output=True, text=True, timeout=15)

    def _stage(self):
        workspace = self.root / ".wot-update"
        candidate = workspace / "staged" / "Game"
        shutil.copytree(self.root / "Game", candidate)
        (self.root / "Game" / "identity").write_text("old")
        (candidate / "identity").write_text("new")
        (workspace / "ready").write_bytes(b"2\n")
        return workspace

    def _hold(self, *, shared):
        environment = self._environment()
        environment["WOT_LAUNCHER_TEST_HOLD"] = "1"
        if shared:
            lock = self.root / "Data" / "session.lock"
            lock.parent.mkdir(parents=True, exist_ok=True)
            environment["WOT_LAUNCHER_TEST_PROFILE_LOCK"] = str(lock)
        arguments = self.root / "arguments.txt"
        arguments.unlink(missing_ok=True)
        # Outside Game: a cwd directory handle cannot provide the exclusion.
        process = subprocess.Popen([self.child], cwd=self.root, env=environment)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        deadline = time.monotonic() + 10
        while not arguments.exists() and time.monotonic() < deadline:
            self.assertIsNone(process.poll(), "fixture exited early")
            time.sleep(0.01)
        self.assertTrue(arguments.exists(), "fixture never started")
        self.assertIsNone(process.poll())
        return process

    def test_ready_at_or_below_running_sequence_is_discarded(self):
        identity = self.root / "Game" / "WoT" / "Build" / "WoTRelease" / "release-identity.json"
        identity.parent.mkdir(parents=True)
        for running in (2, 3, 18446744073709551615):
            with self.subTest(running=running):
                identity.write_text(json.dumps({
                    "format": "wot-release-identity-v1", "release_commit": "abcdef123",
                    "release_sequence": running, "version": 1,
                }, sort_keys=True, separators=(",", ":")), encoding="utf-8")
                workspace = self._stage()
                sentinel = self.root / "Data" / "Preferences" / "keep"
                sentinel.parent.mkdir(parents=True, exist_ok=True)
                sentinel.write_text("player preferences")
                self.assertEqual(self._run().returncode, 0)
                self.assertEqual((self.root / "Game" / "identity").read_text(), "old")
                self.assertFalse((workspace / "ready").exists())
                self.assertFalse((workspace / "staged").exists())
                self.assertFalse((workspace / "trial").exists())
                self.assertEqual(sentinel.read_text(), "player preferences")
                self.assertFalse((self.root / "error.txt").exists())

    def test_running_copy_shared_lock_defers_swap_until_exit(self):
        workspace = self._stage()
        process = self._hold(shared=True)
        try:
            self.assertEqual(self._run().returncode, 0)
            self.assertEqual((self.root / "Game" / "identity").read_text(), "old")
            self.assertTrue((workspace / "ready").exists())
            self.assertFalse((workspace / "trial").exists())
            self.assertFalse((self.root / "error.txt").exists())
            self.assertIsNone(process.poll())
        finally:
            process.terminate()
            process.wait(timeout=10)
        self.assertEqual(self._run().returncode, 0)
        self.assertEqual((self.root / "Game" / "identity").read_text(), "new")
        self.assertEqual((workspace / "trial").read_bytes(), b"2 pending\n")

    def test_swap_then_double_click_before_game_shared_hold_keeps_pending(self):
        workspace = self._stage()
        self.assertEqual(self._run().returncode, 0)
        self.assertEqual((workspace / "trial").read_bytes(), b"2 pending\n")
        process = self._hold(shared=False)
        try:
            self.assertEqual(self._run().returncode, 0)
            self.assertEqual((self.root / "Game" / "identity").read_text(), "new")
            self.assertEqual((workspace / "prev" / "Game" / "identity").read_text(), "old")
            self.assertEqual((workspace / "trial").read_bytes(), b"2 pending\n")
            self.assertFalse((workspace / "rolled-back").exists())
            self.assertFalse((self.root / "error.txt").exists())
        finally:
            process.terminate()
            process.wait(timeout=10)

    def test_started_with_shared_lock_skips_then_rolls_back_when_free(self):
        workspace = self._stage()
        self.assertEqual(self._run().returncode, 0)
        (workspace / "trial").write_bytes(b"2 started\n")
        process = self._hold(shared=True)
        try:
            self.assertEqual(self._run().returncode, 0)
            self.assertEqual((self.root / "Game" / "identity").read_text(), "new")
            self.assertEqual((workspace / "trial").read_bytes(), b"2 started\n")
            self.assertFalse((self.root / "error.txt").exists())
        finally:
            process.terminate()
            process.wait(timeout=10)
        self.assertEqual(self._run().returncode, 0)
        self.assertEqual((self.root / "Game" / "identity").read_text(), "old")
        self.assertEqual((workspace / "rolled-back").read_bytes(), b"2\n")
        self.assertFalse((workspace / "trial").exists())

    def test_missing_game_recovery_with_and_without_started(self):
        for started in (False, True):
            with self.subTest(started=started):
                workspace = self.root / ".wot-update"
                previous = workspace / "prev" / "Game"
                previous.parent.mkdir(parents=True, exist_ok=True)
                (self.root / "Game").rename(previous)
                if started:
                    (workspace / "trial").write_bytes(b"2 started\n")
                self.assertEqual(self._run().returncode, 0)
                self.assertTrue(self.child.exists())
                self.assertFalse((workspace / "trial").exists())
                if started:
                    self.assertEqual((workspace / "rolled-back").read_bytes(), b"2\n")

    def test_pending_without_previous_launches_without_destructive_action(self):
        workspace = self.root / ".wot-update"
        workspace.mkdir()
        (workspace / "trial").write_bytes(b"2 pending\n")
        (workspace / "ready").write_bytes(b"3\n")
        self.assertEqual(self._run().returncode, 0)
        self.assertEqual((workspace / "trial").read_bytes(), b"2 pending\n")
        self.assertEqual((workspace / "ready").read_bytes(), b"3\n")
        self.assertFalse((self.root / "error.txt").exists())

    def test_malformed_markers_and_missing_rollback_tree(self):
        # A torn marker (full disk, crash, power loss) is stale: the launcher
        # drops it and starts the current game, never refusing later launches.
        workspace = self.root / ".wot-update"
        workspace.mkdir()
        for payload in (b"", b"0\n", b"-1\n", b"01\n", b"9223372036854775808\n",
                        b"2", b"2\r\n", b"2\nextra", b"2 " + b"x" * 64,
                        b"2 pen", b"2 started"):
            with self.subTest(payload=payload):
                (workspace / "ready").write_bytes(payload)
                self.assertEqual(self._run().returncode, 0)
                self.assertFalse((workspace / "ready").exists())
                self.assertFalse((self.root / "error.txt").exists())
                (workspace / "trial").write_bytes(payload)
                self.assertEqual(self._run().returncode, 0)
                self.assertFalse((workspace / "trial").exists())
                self.assertFalse((self.root / "error.txt").exists())
                self.assertTrue(self.child.exists())
        # A started trial with no previous tree has nothing to restore: the
        # current game keeps running and the stale trial is dropped.
        (workspace / "trial").write_bytes(b"2 started\n")
        self.assertEqual(self._run().returncode, 0)
        self.assertFalse((workspace / "trial").exists())
        self.assertTrue(self.child.exists())

    def test_marker_writes_leave_no_temporary(self):
        workspace = self._stage()
        self.assertEqual(self._run().returncode, 0)
        self.assertEqual((workspace / "trial").read_bytes(), b"2 pending\n")
        self.assertEqual(sorted(path.name for path in workspace.glob("*.tmp")), [])

    def test_trial_write_failure_restores_old_tree(self):
        workspace = self._stage()
        # A held ready file prevents marker retirement after the two renames.
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32,
            ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(workspace / "ready"), 0x80000000, 1, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            self.assertEqual(self._run().returncode, 0)
        finally:
            kernel.CloseHandle(handle)
        self.assertEqual((self.root / "Game" / "identity").read_text(), "old")
        self.assertTrue((workspace / "ready").exists())
        self.assertFalse((workspace / "trial").exists())
        self.assertIn("couldn't install", (self.root / "error.txt").read_text(encoding="utf-16"))

    def test_UnwritableInstallFolderIsReported(self):
        data = self.root / "Data"
        data.mkdir()
        system = Path(os.environ["SystemRoot"]) / "System32"
        whoami = subprocess.run([system / "whoami.exe", "/user", "/fo", "csv", "/nh"],
                                check=True, capture_output=True, text=True)
        sid = next(csv.reader([whoami.stdout.strip()]))[1]
        deny = subprocess.run(
            [system / "icacls.exe", str(data), "/inheritance:r", "/deny", f"*{sid}:(W)"],
            capture_output=True, text=True)
        self.assertEqual(deny.returncode, 0, deny.stdout + deny.stderr)
        try:
            completed = subprocess.run([self.launcher], env=self._environment(),
                                       capture_output=True, text=True)
            self.assertEqual(completed.returncode, 5)
            expected = (
                f"WoT Reborn can't save its data in {data}. Move the WoT Reborn "
                "folder to a place you can write to, such as your user folder, "
                "and start it again.")
            self.assertEqual(
                (self.root / "error.txt").read_text(encoding="utf-16"), expected)
        finally:
            subprocess.run([system / "icacls.exe", str(data), "/remove:d", f"*{sid}",
                            "/inheritance:e"], capture_output=True, text=True)

    def test_launcher_build_is_reproducible(self):
        second = self.root / "second.exe"
        windows_launcher.build_launcher(
            second, self.child, "Test", test_mode=True, copy_icon=False)
        digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        self.assertEqual(digest(self.launcher), digest(second))

    def test_real_no_bootstrap_staging_layout_launches_for_each_configuration(self):
        self.assertFalse((self.root / "Game" / "WoT.exe").exists())
        for configuration in ("Test", "Shipping"):
            staged = self.root / configuration / "Game"
            child = windows_launcher.build_test_stub(
                stripped_pak._guarded_main_artifact_path(
                    staged, "Win64", configuration))
            launcher = staged.parent / "WoT Reborn.exe"
            windows_launcher.build_launcher(
                launcher, child, configuration, test_mode=True,
                copy_icon=False)
            environment = self._environment()
            environment["WOT_LAUNCHER_TEST_ARGS_FILE"] = str(
                staged.parent / "arguments.txt")
            completed = subprocess.run(
                [launcher, "package-layout"], env=environment,
                capture_output=True, text=True)
            with self.subTest(configuration=configuration):
                self.assertEqual(completed.returncode, 0)
                self.assertEqual(
                    (staged.parent / "arguments.txt").read_text(
                        encoding="utf-16"),
                    f'"{child}" package-layout')
                self.assertFalse((staged / "WoT.exe").exists())


if __name__ == "__main__":
    unittest.main()
