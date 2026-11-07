"""Unit tests for the Steam Deck test harness (no Deck needed: SSH is faked)."""
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import deck


class FakeRunner:
    def __init__(self, outputs=None):
        self.calls, self.outputs = [], list(outputs or [])

    def run(self, argv, input_bytes=None, timeout=120):
        self.calls.append((argv, input_bytes))
        out = self.outputs.pop(0) if self.outputs else b""
        return subprocess.CompletedProcess(argv, 0, out, b"")


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
        self.assertIn("Proton 11.0/proton", script)
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
    def test_probe_batch_quotes_every_argument_with_crlf_lines(self):
        text = deck.probe_batch(["walk", r"Z:\home\deck\Games\WoT Reborn"])
        self.assertIn('"walk" "Z:\\home\\deck\\Games\\WoT Reborn"', text)
        self.assertIn(r"> C:\wot-probe\out.jsonl 2>&1", text)
        self.assertTrue(all(line.endswith("\r") for line in text.split("\n")[:-1]))

    def test_probe_batch_refuses_quotes_inside_an_argument(self):
        with self.assertRaises(deck.DeckError):
            deck.probe_batch(['walk', 'C:\\a"b'])

    def test_parse_probe_output_keeps_non_json_lines_as_raw(self):
        rows = deck.parse_probe_output(b'{"check":"env","wine":true}\r\nnot json\r\n\r\n')
        self.assertEqual(rows, [{"check": "env", "wine": True}, {"raw": "not json"}])

    def test_run_probe_runs_the_batch_under_proton_and_fetches_output(self):
        runner = FakeRunner([b"", b"", b'{"check":"env"}\n'])
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "wot_path_probe.exe"
            exe.write_bytes(b"MZ")
            rows = deck.run_probe(deck.DeckTarget("h"), runner, exe, 2524773648, ["env"])
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


if __name__ == "__main__":
    unittest.main()
