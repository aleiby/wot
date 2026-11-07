"""Unit tests for judging Steam Deck cells from the game's bootstrap heartbeat."""
import unittest

import cells

# Shapes taken from a real first run under Proton 11 on a Steam Deck.
FIRST_RUN = [
    {"event": "phase-begin", "phase": "EstablishingIdentity"},
    {"event": "release-identity", "phase": "EstablishingIdentity",
     "detail": "sequence=2 stamped=1"},
    {"event": "original-game-detected", "phase": "CheckingOriginalGameData",
     "detail": "source=downloads"},
    {"event": "phase-begin", "phase": "BuildingContent"},
    {"event": "frontend-handoff", "phase": "Ready",
     "detail": "widget=1 authorized=1 ready=1 world_authority=1"},
    {"event": "update-check-begin", "phase": "Ready"},
    {"event": "update-check-result", "phase": "Ready", "detail": "none"},
]


def without(events, name):
    return [e for e in events if e.get("event") != name]


def replace_detail(events, name, detail):
    return [dict(e, detail=detail) if e.get("event") == name else e for e in events]


class CellJudgingTests(unittest.TestCase):
    def test_first_run_needs_the_frontend_handoff(self):
        self.assertTrue(cells.first_run(FIRST_RUN).ok)
        verdict = cells.first_run(without(FIRST_RUN, "frontend-handoff"))
        self.assertFalse(verdict.ok)
        self.assertIn("frontend-handoff", verdict.reason)

    def test_installer_must_come_from_downloads_without_browse(self):
        self.assertTrue(cells.installer_from_downloads(FIRST_RUN).ok)
        picked = replace_detail(FIRST_RUN, "original-game-detected", "source=picked")
        self.assertFalse(cells.installer_from_downloads(picked).ok)
        self.assertFalse(cells.installer_from_downloads(
            without(FIRST_RUN, "original-game-detected")).ok)

    def test_update_check_must_not_fail(self):
        self.assertTrue(cells.update_check(FIRST_RUN).ok)
        failed = replace_detail(FIRST_RUN, "update-check-result", "failed")
        verdict = cells.update_check(failed)
        self.assertFalse(verdict.ok)
        self.assertIn("failed", verdict.reason)
        self.assertFalse(cells.update_check(without(FIRST_RUN, "update-check-result")).ok)

    def test_release_sequence_reads_the_identity_event(self):
        self.assertEqual(cells.release_sequence(FIRST_RUN), 2)
        self.assertIsNone(cells.release_sequence(without(FIRST_RUN, "release-identity")))

    def test_staged_update_applies_when_the_sequence_rises(self):
        after = replace_detail(FIRST_RUN, "release-identity", "sequence=3 stamped=1")
        self.assertTrue(cells.staged_update_applies(FIRST_RUN, after).ok)
        self.assertFalse(cells.staged_update_applies(FIRST_RUN, FIRST_RUN).ok)


class FakeRunner:
    def __init__(self, outputs):
        self.calls, self.outputs = [], list(outputs)

    def run(self, argv, input_bytes=None, timeout=120):
        import subprocess
        self.calls.append((argv, input_bytes))
        return subprocess.CompletedProcess(argv, 0, self.outputs.pop(0) if self.outputs else b"", b"")


class FirstRunCellsTests(unittest.TestCase):
    def test_run_first_run_cells_launches_waits_judges_and_stops(self):
        import json
        import deck
        jsonl = "".join(json.dumps(e) + "\n" for e in FIRST_RUN).encode()
        # clear old heartbeats, launch, poll (handoff + result present), final read, stop
        runner = FakeRunner([b"", b"", jsonl, jsonl, b""])
        verdicts = cells.run_first_run_cells(deck.DeckTarget("h"), runner,
                                             "/srv/games/WoT Reborn", app_id=7,
                                             timeout=0.0, poll=0.0)
        self.assertEqual([name for name, _ in verdicts],
                         ["first-run", "installer-from-downloads", "update-check"])
        self.assertTrue(all(v.ok for _, v in verdicts))
        scripts = [call[1].decode() for call in runner.calls]
        # Heartbeats accumulate across launches; a cell judges only its own run.
        self.assertIn("rm -f '/srv/games/WoT Reborn/Data'/UpdaterState/bootstrap-heartbeat-v1",
                      scripts[0])
        self.assertIn("systemd-run", scripts[1])
        self.assertIn("'/srv/games/WoT Reborn/Data'/UpdaterState", scripts[2])
        self.assertIn("systemctl --user stop", scripts[-1])

    def test_a_run_that_never_reaches_the_menu_is_judged_not_raised(self):
        import json
        import deck
        stuck = "".join(json.dumps(e) + "\n" for e in FIRST_RUN[:3]).encode()
        runner = FakeRunner([b"", b"", stuck, stuck, b""])
        verdicts = dict(cells.run_first_run_cells(deck.DeckTarget("h"), runner,
                                                  "/srv/g", app_id=7, timeout=0.0, poll=0.0))
        self.assertFalse(verdicts["first-run"].ok)
        self.assertIn("systemctl --user stop", runner.calls[-1][1].decode())


if __name__ == "__main__":
    unittest.main()
