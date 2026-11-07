"""Judge Steam Deck test cells from the game's bootstrap heartbeat events."""
from __future__ import annotations

import re
from dataclasses import dataclass
from typing import List, Optional, Tuple


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
    deck.launch_hidden(target, runner, remote_dir, unit, app_id)
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
