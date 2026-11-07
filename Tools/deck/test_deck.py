"""Unit tests for the Steam Deck test harness (no Deck needed: SSH is faked)."""
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import deck


class FakeRunner:
    """Each output is stdout bytes, or (returncode, stdout) for a failing call."""

    def __init__(self, outputs=None):
        self.calls, self.outputs = [], list(outputs or [])

    def run(self, argv, input_bytes=None, timeout=120):
        self.calls.append((argv, input_bytes))
        out = self.outputs.pop(0) if self.outputs else b""
        rc, out = out if isinstance(out, tuple) else (0, out)
        return subprocess.CompletedProcess(argv, rc, out, b"err")


class DeckHarnessTests(unittest.TestCase):
    def test_load_target_reads_local_json_and_refuses_missing_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "local.json"
            path.write_text(json.dumps({"deck_host": "deckhost.local"}))
            self.assertEqual(deck.load_target(path), deck.DeckTarget("deckhost.local"))
            path.write_text("{}")
            with self.assertRaises(deck.DeckError):
                deck.load_target(path)

    def test_ssh_is_batch_mode_with_timeout(self):
        runner = FakeRunner()
        deck.ssh(deck.DeckTarget("h"), runner, "echo hi")
        argv, stdin = runner.calls[0]
        self.assertEqual(argv[:3], ["ssh", "-o", "BatchMode=yes"])
        self.assertIn("ConnectTimeout=8", " ".join(argv))
        self.assertEqual(argv[-1], "deck@h")
        self.assertEqual(stdin, b"echo hi")

    def test_launch_hidden_uses_systemd_run_with_runtime_and_proton(self):
        runner = FakeRunner()
        deck.launch_hidden(deck.DeckTarget("h"), runner, "/srv/games/WoT Reborn",
                           unit="wotdeck", app_id=2524773648)
        script = runner.calls[0][1].decode()
        self.assertIn("systemd-run --user --unit=wotdeck --collect", script)
        self.assertIn("SteamLinuxRuntime_4/_v2-entry-point", script)
        self.assertIn("'Proton 11.0'/proton", script)
        self.assertIn("SteamGameId=2524773648", script)
        self.assertIn("'WoT Reborn.exe'", script)

    def test_heartbeat_events_parse_jsonl_and_skip_garbage(self):
        lines = (b'{"event":"phase-begin","phase":"Ready"}\nnot json\n'
                 b'{"event":"frontend-handoff"}\n')
        runner = FakeRunner([lines])
        events = deck.heartbeat_events(deck.DeckTarget("h"), runner, "/x/Data")
        self.assertEqual([e["event"] for e in events], ["phase-begin", "frontend-handoff"])

    def test_wait_for_event_times_out_with_the_events_seen(self):
        runner = FakeRunner([b'{"event":"phase-begin"}\n'] * 3)
        with self.assertRaises(deck.DeckTimeout) as caught:
            deck.wait_for_event(deck.DeckTarget("h"), runner, "/x/Data",
                                "frontend-handoff", timeout=0.0, poll=0.0)
        self.assertEqual(caught.exception.events[0]["event"], "phase-begin")


class DeckProbeTests(unittest.TestCase):
    def test_probe_batch_is_exact_crlf_and_keeps_the_probe_exit_status(self):
        text = deck.probe_batch(["walk", r"Z:\games\WoT Reborn"])
        crlf = "\r\n"
        self.assertEqual(text.splitlines(keepends=True), [
            "@echo off" + crlf,
            r'C:\wot-probe\wot_path_probe.exe "walk" "Z:\games\WoT Reborn" '
            + r"> C:\wot-probe\out.jsonl 2>&1" + crlf,
            "set WOT_PROBE_STATUS=%ERRORLEVEL%" + crlf,
            r"echo done > C:\wot-probe\done.txt" + crlf,
            "exit /b %WOT_PROBE_STATUS%" + crlf,
        ])

    def test_probe_batch_escapes_percent_and_refuses_line_breaks(self):
        self.assertIn('"C:\\%%TEMP%%\\x"', deck.probe_batch([r"C:\%TEMP%\x"]))
        for bad in ["a\nb", "a\rb"]:
            with self.assertRaises(deck.DeckError):
                deck.probe_batch([bad])

    def test_probe_batch_refuses_quotes_inside_an_argument(self):
        with self.assertRaises(deck.DeckError):
            deck.probe_batch(['walk', 'C:\\a"b'])

    def test_parse_probe_output_keeps_non_json_lines_as_raw(self):
        rows = deck.parse_probe_output(b'{"check":"env","wine":true}\r\nnot json\r\n\r\n')
        self.assertEqual(rows, [{"check": "env", "wine": True}, {"raw": "not json"}])

    def _run_probe(self, outputs, args=("env",)):
        runner = FakeRunner(outputs)
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "wot_path_probe.exe"
            exe.write_bytes(b"MZ")
            result = deck.run_probe(deck.DeckTarget("h"), runner, exe, 2524773648, list(args))
        return runner, result

    def test_run_probe_reports_the_probe_exit_status_with_its_rows(self):
        _, result = self._run_probe([b"", (3, b""), b'{"check":"x","ok":false}\n'])
        self.assertEqual(result.exit_status, 3)
        self.assertEqual(result.rows, [{"check": "x", "ok": False}])

    def test_run_probe_raises_on_transport_failures(self):
        for outputs in ([(1, b"")], [b"", (255, b"")], [b"", b"", (1, b"")]):
            with self.assertRaises(deck.DeckError):
                self._run_probe(outputs)

    def test_run_probe_script_fails_fast_and_kills_on_timeout(self):
        runner, _ = self._run_probe([b"", b"", b""])
        script = runner.calls[1][1].decode()
        self.assertTrue(script.startswith("set -e\n"))
        self.assertIn("timeout --kill-after=30 900", script)

    def test_run_probe_runs_the_batch_under_proton_and_fetches_output(self):
        runner, result = self._run_probe([b"", b"", b'{"check":"env"}\n'])
        rows = result.rows
        scripts = [call[1] for call in runner.calls]
        self.assertEqual(scripts[0], b"MZ")  # exe copied over stdin
        run_script = runner.calls[1][1].decode()
        self.assertIn("SteamLinuxRuntime_4/_v2-entry-point", run_script)
        self.assertIn("cmd.exe", run_script)
        self.assertIn("wot-probe", run_script)
        # Wine's background server inherits the command's stdio; unless it is
        # redirected, the SSH session never ends.
        self.assertRegex(run_script, r"run\.bat' < /dev/null > \S*proton\.log 2>&1")
        # Proton stalls during Wine startup without a display, even for a console program.
        self.assertIn("DISPLAY=:0", run_script)
        self.assertEqual(rows, [{"check": "env"}])


class DeckRobustnessTests(unittest.TestCase):
    def test_launch_hidden_refuses_unsafe_unit_names(self):
        for unit in ["ok; echo hi #", "a b", "it's", ""]:
            with self.assertRaises(deck.DeckError):
                deck.launch_hidden(deck.DeckTarget("h"), FakeRunner(), "/x", unit=unit, app_id=1)

    def test_launch_game_direct_refuses_unsafe_unit_names(self):
        for unit in ["ok; echo hi #", "a b", ""]:
            with self.assertRaises(deck.DeckError):
                deck.launch_game_direct(deck.DeckTarget("h"), FakeRunner(), "/x",
                                        unit=unit, app_id=1)

    def test_launch_game_direct_starts_the_game_exe_from_its_directory(self):
        runner = FakeRunner()
        deck.launch_game_direct(deck.DeckTarget("h"), runner, "/srv/games/WoT Reborn",
                                unit="wotgame", app_id=7)
        script = runner.calls[0][1].decode()
        self.assertTrue(script.startswith("cd '/srv/games/WoT Reborn'/Game && "))
        self.assertIn("WoT/Binaries/Win64/WoT-Win64-*.exe", script)
        self.assertIn("--unit=wotgame", script)
        self.assertIn("SteamGameId=7", script)

    def test_launch_hidden_single_quotes_proton_and_runtime_names(self):
        runner = FakeRunner()
        deck.launch_hidden(deck.DeckTarget("h"), runner, "/x", unit="wotdeck", app_id=1,
                           proton='GE"$(echo PWNED)', runtime="SteamLinuxRuntime_4")
        script = runner.calls[0][1].decode()
        self.assertIn("""'GE"$(echo PWNED)'""", script)
        self.assertIn("timeout --kill-after=30 3600", script)

    def test_heartbeat_and_stop_raise_when_the_deck_is_unreachable(self):
        with self.assertRaises(deck.DeckError):
            deck.heartbeat_events(deck.DeckTarget("h"), FakeRunner([(255, b"")]), "/x")
        with self.assertRaises(deck.DeckError):
            deck.stop(deck.DeckTarget("h"), FakeRunner([(255, b"")]), "wotdeck")

    def test_heartbeat_with_no_file_yet_is_an_empty_list(self):
        self.assertEqual(deck.heartbeat_events(deck.DeckTarget("h"), FakeRunner([b""]), "/x"), [])


if __name__ == "__main__":
    unittest.main()
