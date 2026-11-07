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


if __name__ == "__main__":
    unittest.main()
