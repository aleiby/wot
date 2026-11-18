"""Judge Steam Deck test cells from the game's bootstrap heartbeat events.

Every cell here launches the game with its LAN installer receiver switched off
(deck.LAN_RECEIVER_OFF_ENV), so a WoT Reborn sharing the installer elsewhere on
the network cannot feed a cell that judges where the installer came from. Only
Test builds read that switch, so these cells need a Test build.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from typing import List, Optional, Tuple

# The existing cells all judge installs fed from Downloads or already set up.
CELLS_RECEIVER_OFF = True


@dataclass(frozen=True)
class Verdict:
    ok: bool
    reason: str


def _find(events: List[dict], name: str) -> List[dict]:
    return [e for e in events if e.get("event") == name]


def first_run(events: List[dict]) -> Verdict:
    """The game got from a fresh install through setup to the main menu."""
    if _find(events, "frontend-handoff"):
        return Verdict(True, "reached the main menu (frontend-handoff)")
    return Verdict(False, "no frontend-handoff: the game never reached the main menu")


def installer_from_downloads(events: List[dict]) -> Verdict:
    """The original installer was found automatically in Downloads, not picked."""
    sources = [str(e.get("detail", "")) for e in _find(events, "original-game-detected")]
    if any(re.search(r"\bsource=downloads\b", s) for s in sources):
        return Verdict(True, "installer found in Downloads without Browse")
    if sources:
        return Verdict(False, "installer found, but not in Downloads: " + "; ".join(sources))
    return Verdict(False, "no original-game-detected event")


def update_check(events: List[dict]) -> Verdict:
    """The update check completed; 'none' and 'available' both count."""
    results = [str(e.get("detail", "")) for e in _find(events, "update-check-result")]
    if not results:
        return Verdict(False, "no update-check-result event")
    if results[-1] == "failed":
        failures = [str(e.get("detail", "")) for e in _find(events, "update-check-failed")]
        return Verdict(False, "update check failed: " + ("; ".join(failures) or "no detail"))
    return Verdict(True, "update check completed: " + results[-1])


def release_sequence(events: List[dict]) -> Optional[int]:
    """The running build's release sequence from its release-identity event."""
    for event in _find(events, "release-identity"):
        match = re.search(r"\bsequence=(\d+)\b", str(event.get("detail", "")))
        if match:
            return int(match.group(1))
    return None


def staged_update_applies(before: List[dict], after: List[dict]) -> Verdict:
    """A relaunch after staging runs a newer release sequence."""
    old, new = release_sequence(before), release_sequence(after)
    if old is None or new is None:
        return Verdict(False, f"release sequence unknown (before={old}, after={new})")
    if new > old:
        return Verdict(True, f"release sequence rose {old} -> {new}")
    return Verdict(False, f"release sequence did not rise ({old} -> {new})")


def run_first_run_cells(target, runner, remote_dir: str, app_id: int,
                        unit: str = "wotdeck", timeout: float = 900.0,
                        poll: float = 5.0) -> List[Tuple[str, Verdict]]:
    """Launch a fresh install, wait for its update check, judge, always stop.

    The update check runs after the main menu appears, so waiting for its
    result covers the whole first run. A run that stalls is judged, not raised.
    """
    import deck  # deck's CLI imports this module

    import shlex

    data_dir = remote_dir.rstrip("/") + "/Data"
    # Heartbeats accumulate across launches; judge only this run's events.
    cleared = deck.ssh(target, runner,
                       f"rm -f {shlex.quote(data_dir)}/UpdaterState/bootstrap-heartbeat-v1*.jsonl")
    if cleared.returncode != 0:
        raise deck.DeckError("could not clear old heartbeats: "
                             + cleared.stderr.decode(errors="replace"))
    deck.launch_hidden(target, runner, remote_dir, unit, app_id,
                       lan_receiver_off=CELLS_RECEIVER_OFF)
    try:
        try:
            deck.wait_for_event(target, runner, data_dir, "update-check-result",
                                timeout, poll)
        except deck.DeckTimeout:
            pass
        events = deck.heartbeat_events(target, runner, data_dir)
    finally:
        deck.stop(target, runner, unit)
    return [
        ("first-run", first_run(events)),
        ("installer-from-downloads", installer_from_downloads(events)),
        ("update-check", update_check(events)),
    ]


# The launcher's update workspace, read on the Deck: file identities (inodes)
# show whether a tree was swapped, the markers show the swap's state.
_SNAPSHOT = r"""
import json, os, sys
root = sys.argv[1]
work = os.path.join(root, ".wot-update")
def inode(path):
    try:
        return os.lstat(path).st_ino
    except OSError:
        return None
def marker(name):
    try:
        with open(os.path.join(work, name)) as f:
            return f.read().strip()
    except OSError:
        return None
print(json.dumps({
    "game": inode(os.path.join(root, "Game")),
    "ready": marker("ready"),
    "ready_inode": inode(os.path.join(work, "ready")),
    "trial": marker("trial"),
    "rolled_back": marker("rolled-back"),
    "staged": inode(os.path.join(work, "staged", "Game")),
    "prev": inode(os.path.join(work, "prev", "Game")),
}))
"""


def update_snapshot(target, runner, remote_dir: str) -> dict:
    import json
    import shlex

    import deck
    result = deck.ssh(target, runner, f"python3 -c {shlex.quote(_SNAPSHOT)} "
                                      f"{shlex.quote(remote_dir)}")
    if result.returncode != 0:
        raise deck.DeckError("update snapshot failed: "
                             + result.stderr.decode(errors="replace"))
    return json.loads(result.stdout.decode("utf-8", "replace"))


def launcher_skips_while_running(before: dict, during: dict) -> Verdict:
    """A launcher run while the game holds its lock changes nothing."""
    if before.get("ready") is None or before.get("staged") is None:
        return Verdict(False, "no staged update to protect")
    keys = ("game", "ready", "ready_inode", "staged", "prev")
    changed = [k for k in keys if during.get(k) != before.get(k)]
    if changed:
        return Verdict(False, "the launcher changed: " + ", ".join(changed))
    return Verdict(True, "the staged update was left untouched")


def update_applied(staged: dict, after: dict, events: List[dict]) -> Verdict:
    """The staged tree became the game, ran as its sequence, and was retired."""
    expected = int(staged["ready"])
    running = release_sequence(events)
    if running != expected:
        return Verdict(False, f"running sequence {running}, staged {expected}")
    if after.get("game") != staged.get("staged"):
        return Verdict(False, "the staged tree was not swapped in")
    if after.get("trial") is not None or after.get("prev") is not None:
        return Verdict(False, f"trial not retired (trial={after.get('trial')!r}, "
                              f"prev={'present' if after.get('prev') else 'absent'})")
    return Verdict(True, f"sequence {expected} swapped in, trial and prev retired")


def _clear_heartbeats(data_dir: str) -> str:
    import shlex
    return f"rm -f {shlex.quote(data_dir)}/UpdaterState/bootstrap-heartbeat-v1*.jsonl"


def run_launcher_lock_cell(target, runner, remote_dir: str, app_id: int,
                           settle: float = 45.0, timeout: float = 600.0,
                           poll: float = 5.0) -> Verdict:
    """With the game running, a second launcher must not touch a staged update.

    Needs an install whose .wot-update already holds a staged update (the
    game's own update check stages one). The game is started directly so the
    launcher does not apply that update first.
    """
    import time

    import deck
    data_dir = remote_dir.rstrip("/") + "/Data"
    before = update_snapshot(target, runner, remote_dir)
    if before.get("ready") is None or before.get("staged") is None:
        return launcher_skips_while_running(before, before)
    _require(deck, deck.ssh(target, runner, _clear_heartbeats(data_dir)), "clear heartbeats")
    try:
        deck.launch_game_direct(target, runner, remote_dir, "wotgame", app_id,
                                lan_receiver_off=CELLS_RECEIVER_OFF)
        try:
            deck.wait_for_event(target, runner, data_dir, "frontend-handoff", timeout, poll)
        except deck.DeckTimeout:
            return Verdict(False, "the game never reached frontend-handoff")
        deck.launch_hidden(target, runner, remote_dir, "wotlaunch2", app_id,
                           lan_receiver_off=CELLS_RECEIVER_OFF)
        time.sleep(settle)
        during = update_snapshot(target, runner, remote_dir)
    finally:
        deck.stop(target, runner, "wotlaunch2")
        deck.stop(target, runner, "wotgame")
    return launcher_skips_while_running(before, during)


def run_staged_update_cell(target, runner, remote_dir: str, app_id: int,
                           candidate_zip: str, settle: float = 20.0,
                           timeout: float = 600.0, poll: float = 5.0) -> Verdict:
    """Stage a newer release tree, launch, and judge the swap and its trial.

    candidate_zip is a release zip already on the Deck whose stamped sequence
    is above the install's; it must be the same build configuration, because
    the launcher only applies a tree carrying its own game executable.
    """
    import shlex
    import time

    import deck
    data_dir = remote_dir.rstrip("/") + "/Data"
    q, z = shlex.quote(remote_dir), shlex.quote(candidate_zip)
    stage = (
        "set -e\n"
        f"cd {q}\n"
        "rm -rf .wot-update .wot-candidate\n"
        "mkdir -p .wot-update/staged .wot-candidate\n"
        f"unzip -q {z} 'WoT Reborn/Game/*' -d .wot-candidate\n"
        "mv '.wot-candidate/WoT Reborn/Game' .wot-update/staged/Game\n"
        "rm -rf .wot-candidate\n"
        "seq=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))"
        "[\"release_sequence\"])' .wot-update/staged/Game/WoT/Build/WoTRelease/"
        "release-identity.json)\n"
        "printf '%s\\n' \"$seq\" > .wot-update/ready\n"
        + _clear_heartbeats(data_dir) + "\n"
        "echo \"$seq\"\n"
    )
    result = deck.ssh(target, runner, stage, 900)
    _require(deck, result, "stage the candidate")
    sequence = result.stdout.decode().strip()
    if not sequence.isdigit() or int(sequence) < 1:
        raise deck.DeckError(f"the candidate's release_sequence must be positive: {sequence!r}")
    staged = update_snapshot(target, runner, remote_dir)
    try:
        deck.launch_hidden(target, runner, remote_dir, "wotdeck", app_id,
                           lan_receiver_off=CELLS_RECEIVER_OFF)
        try:
            deck.wait_for_event(target, runner, data_dir, "frontend-handoff", timeout, poll)
        except deck.DeckTimeout:
            pass
        time.sleep(settle)  # the front end retires the trial after the handoff
        after = update_snapshot(target, runner, remote_dir)
        events = deck.heartbeat_events(target, runner, data_dir)
    finally:
        deck.stop(target, runner, "wotdeck")
    return update_applied(staged, after, events)


def _require(deck, result, what: str):
    if result.returncode != 0:
        raise deck.DeckError(f"{what} failed: " + result.stderr.decode(errors="replace"))
