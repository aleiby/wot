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



def snapshot(**overrides):
    """An update-folder snapshot as update_snapshot returns it."""
    base = {"game": 100, "ready": "2", "ready_inode": 200, "trial": None,
            "rolled_back": None, "staged": 300, "prev": None}
    base.update(overrides)
    return base


class UpdateJudgingTests(unittest.TestCase):
    def test_launcher_lock_passes_only_when_nothing_moved(self):
        self.assertTrue(cells.launcher_skips_while_running(snapshot(), snapshot()).ok)
        verdict = cells.launcher_skips_while_running(
            snapshot(), snapshot(ready=None, ready_inode=None))
        self.assertFalse(verdict.ok)
        self.assertIn("ready", verdict.reason)
        swapped = cells.launcher_skips_while_running(snapshot(), snapshot(game=300, prev=100))
        self.assertFalse(swapped.ok)
        self.assertIn("game", swapped.reason)

    def test_launcher_lock_needs_a_staged_update_to_protect(self):
        verdict = cells.launcher_skips_while_running(
            snapshot(ready=None, staged=None), snapshot(ready=None, staged=None))
        self.assertFalse(verdict.ok)
        self.assertIn("no staged update", verdict.reason)

    def test_update_applies_when_swapped_reported_and_retired(self):
        staged = snapshot(ready="1")
        events = [{"event": "release-identity", "detail": "sequence=1 stamped=1"}]
        after = snapshot(game=300, ready="2", staged=400)
        self.assertTrue(cells.update_applied(staged, after, events).ok)

    def test_update_fails_when_the_running_sequence_is_not_the_staged_one(self):
        events = [{"event": "release-identity", "detail": "sequence=0 stamped=1"}]
        verdict = cells.update_applied(snapshot(ready="1"), snapshot(game=300), events)
        self.assertFalse(verdict.ok)
        self.assertIn("sequence", verdict.reason)

    def test_update_fails_when_the_trial_is_not_retired(self):
        events = [{"event": "release-identity", "detail": "sequence=1 stamped=1"}]
        verdict = cells.update_applied(
            snapshot(ready="1"), snapshot(game=300, trial="1 started", prev=100), events)
        self.assertFalse(verdict.ok)
        self.assertIn("trial", verdict.reason)

    def test_update_fails_when_the_staged_tree_was_not_swapped_in(self):
        events = [{"event": "release-identity", "detail": "sequence=1 stamped=1"}]
        verdict = cells.update_applied(snapshot(ready="1"), snapshot(game=100), events)
        self.assertFalse(verdict.ok)
        self.assertIn("swap", verdict.reason)


def snap_bytes(**overrides):
    import json
    return (json.dumps(snapshot(**overrides)) + "\n").encode()


class UpdateCellDriverTests(unittest.TestCase):
    def test_lock_cell_runs_the_game_directly_then_a_second_launcher(self):
        import deck
        handoff = b'{"event":"frontend-handoff"}\n'
        # snapshot, clear, start game, poll, second launcher, snapshot, stop x2
        runner = FakeRunner([snap_bytes(), b"", b"", handoff, b"", snap_bytes(), b"", b""])
        verdict = cells.run_launcher_lock_cell(deck.DeckTarget("h"), runner,
                                               "/srv/g/WoT Reborn", app_id=7,
                                               settle=0.0, timeout=0.0, poll=0.0)
        self.assertTrue(verdict.ok, verdict.reason)
        scripts = [call[1].decode() for call in runner.calls]
        self.assertIn("--unit=wotgame", scripts[2])
        self.assertIn("WoT-Win64-", scripts[2])
        self.assertNotIn("'WoT Reborn.exe'", scripts[2])  # not through the launcher
        self.assertIn("--unit=wotlaunch2", scripts[4])
        self.assertIn("'WoT Reborn.exe'", scripts[4])
        self.assertEqual(scripts[-2:], ["systemctl --user stop wotlaunch2 || true",
                                        "systemctl --user stop wotgame || true"])

    def test_lock_cell_without_a_staged_update_launches_nothing(self):
        import deck
        runner = FakeRunner([snap_bytes(ready=None, staged=None)])
        verdict = cells.run_launcher_lock_cell(deck.DeckTarget("h"), runner, "/srv/g",
                                               app_id=7, settle=0.0, timeout=0.0, poll=0.0)
        self.assertFalse(verdict.ok)
        self.assertEqual(len(runner.calls), 1)

    def test_lock_cell_stops_both_units_when_the_game_never_starts(self):
        import deck
        runner = FakeRunner([snap_bytes(), b"", b"", b"", b"", b""])
        verdict = cells.run_launcher_lock_cell(deck.DeckTarget("h"), runner, "/srv/g",
                                               app_id=7, settle=0.0, timeout=0.0, poll=0.0)
        self.assertFalse(verdict.ok)
        self.assertIn("frontend-handoff", verdict.reason)
        scripts = [call[1].decode() for call in runner.calls]
        self.assertIn("systemctl --user stop wotgame || true", scripts)

    def test_update_cell_stages_the_candidate_and_judges_the_next_launch(self):
        import deck
        events = (b'{"event":"release-identity","detail":"sequence=1 stamped=1"}\n'
                  b'{"event":"frontend-handoff"}\n')
        # stage (prints the sequence), snapshot, launch, poll, snapshot, final read, stop
        runner = FakeRunner([b"1\n", snap_bytes(ready="1"), b"", events,
                             snap_bytes(game=300, staged=None, ready=None), events, b""])
        verdict = cells.run_staged_update_cell(deck.DeckTarget("h"), runner,
                                               "/srv/g/WoT Reborn", app_id=7,
                                               candidate_zip="/srv/z/s1.zip",
                                               settle=0.0, timeout=0.0, poll=0.0)
        self.assertTrue(verdict.ok, verdict.reason)
        stage = runner.calls[0][1].decode()
        self.assertIn("'WoT Reborn/Game/*'", stage)
        self.assertIn("/srv/z/s1.zip", stage)
        self.assertIn("release_sequence", stage)
        self.assertIn(".wot-update/ready", stage)
        self.assertIn("systemctl --user stop wotdeck", runner.calls[-1][1].decode())

    def test_update_cell_refuses_a_candidate_without_a_positive_sequence(self):
        import deck
        runner = FakeRunner([b"0\n"])
        with self.assertRaises(deck.DeckError):
            cells.run_staged_update_cell(deck.DeckTarget("h"), runner, "/srv/g", app_id=7,
                                         candidate_zip="/srv/z/s0.zip",
                                         settle=0.0, timeout=0.0, poll=0.0)

if __name__ == "__main__":
    unittest.main()
