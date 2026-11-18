#!/usr/bin/env python3
"""Drive the packaged two-instance angreal matrix + match-rules run.

The PowerShell launcher (run_angreal_matrix.ps1) owns process/profile
isolation and has already started one listen host and one client. This
driver owns only the token-gated line-JSON conversation (the seal harness's
Channel/Watchdog/wait_for), writes an auditable transcript, the raw per-cast
observation records (observations.jsonl) and a summary. Verdicts are
computed offline by verify_angreal_matrix.py from those records and
angreal_expectations.json, so the judgement is re-runnable and unit-tested.

Phases:
  matrix - for every obtainable class (angreal_expectations.json): grant it to
           the caster on the authority, select it, aim at the other player,
           press/release fire through the real input path, poll BOTH
           instances for the observation window, clean up (kill + restart-fire
           respawn away from the test spot) - once host->client, once
           client->host.
  rules  - giMPArena match rules against the shipped values: restart-fire
           respawn, frag limit 20 ends the match (both instances), restart
           fire is refused inside PlayerRestartGameDelay and accepted after,
           and the restart rotates to the next Arena map.
  timelimit - host a TimeLimit=1 match (pending-build `host` fields, else a
           guarded `servertravel` of the same shell) and wait for the clock.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "battle"))
sys.path.insert(0, str(HERE))
from seal_match_driver import (  # noqa: E402  pylint: disable=wrong-import-position
    WATCHDOG, Channel, FatalRun, controlled, dist2, wait_for)
import matrix_logic as logic  # noqa: E402  pylint: disable=wrong-import-position

EXPECTATIONS = HERE / "angreal_expectations.json"


class Session:
    def __init__(self, map_name, host, client, transcript, screens, evidence):
        self.map = map_name
        self.host = host
        self.client = client
        self.chan = {"host": host, "client": client}
        self.transcript = transcript
        self.screens = screens
        self.evidence = evidence
        self.events = []
        self.frames = {"host": [], "client": []}
        self.observations = evidence / "observations.jsonl"
        self.spot = None
        self.graveyard = None
        self.client_faction = None
        self.host_timelimit = None
        self.host_fraglimit = None

    # -- bookkeeping ----------------------------------------------------------
    def note(self, what, **detail):
        entry = {"utc": time.time(), "event": what, **detail}
        self.events.append(entry)
        self.transcript.append({"utc": entry["utc"], "role": "driver", "event": what,
                                "detail": detail})
        print(f"[{self.map}] {what} {json.dumps(detail, sort_keys=True, default=str)[:600]}",
              flush=True)

    def record(self, row):
        with self.observations.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(row, default=str) + "\n")

    def shot(self, label):
        """Request a screenshot (HUD included) on both instances WITHOUT waiting
        for the PNG: a blocking wait would stall the poll loop and delay a
        hold-to-cast release (and so its charge count). The launcher copies
        every PNG afterwards; the n-th request of a role is its n-th PNG
        (ScreenShotNNNNN order), recorded here with its label."""
        for role in ("host", "client"):
            try:
                self.chan[role].command("screenshot")
                self.frames[role].append({"label": label, "seq": len(self.frames[role]),
                                          "utc": time.time()})
            except RuntimeError:
                pass
        self.note("screenshot", label=label)

    # -- snapshots --------------------------------------------------------------
    def snap(self, role):
        return self.chan[role].snapshot()

    def both(self):
        return {"host": self.snap("host"), "client": self.snap("client")}

    def players(self, snapshot):
        return [p for p in snapshot.get("pawns", []) if p.get("player") is not None]

    def own(self, role, snapshot=None):
        return controlled(snapshot or self.snap(role))

    def other_player(self, snapshot):
        """The remote player's pawn as this instance sees it."""
        others = [p for p in self.players(snapshot) if not p.get("local")]
        if not others:
            raise RuntimeError("no remote player pawn in snapshot")
        # A corpse keeps its PlayerState briefly; prefer the living one.
        others.sort(key=lambda p: (bool(p.get("dead")), p.get("name", "")))
        return others[0]

    def names(self):
        """Host-side (authority) names of both players' pawns."""
        hs = self.snap("host")
        return {"host": self.own("host", hs)["name"], "client": self.other_player(hs)["name"]}

    # -- flow -------------------------------------------------------------------
    def join(self, game_port):
        wait_for("host front end ready", lambda: self.host.status().get("front_end_ready"), 480, 1)
        wait_for("client front end ready",
                 lambda: self.client.status().get("front_end_ready"), 480, 1)
        self.note("front-ends-ready")
        # Optional match rules (pending-build `host` fields; the shipped binary
        # ignores unknown fields and hosts with DefaultHostRequest's values).
        rules = {k: v for k, v in (("timelimit", self.host_timelimit),
                                   ("fraglimit", self.host_fraglimit)) if v is not None}
        wait_for("host request accepted",
                 lambda: self.host.command("host", map=self.map, game="arena", **rules), 120, 1)

        def host_listening():
            status = self.host.status()
            return status if (status.get("world") == self.map
                              and status.get("net_mode") == "listen") else None
        self.note("host-listening",
                  status=wait_for(f"host listen world {self.map}", host_listening, 300, 1))
        if self.client_faction:
            # Disguise needs a target of another player class. The `faction`
            # verb is pending build; the shipped binary answers "unknown cmd".
            try:
                self.client.command("faction", name=self.client_faction)
                self.note("client-faction", faction=self.client_faction)
            except RuntimeError as exc:
                self.note("client-faction-unavailable", error=str(exc)[:200])
        wait_for("join request accepted", lambda: self.client.command(
            "join", address=f"127.0.0.1:{game_port}"), 60, 1)

        def client_joined():
            status = self.client.status()
            return status if (status.get("world") == self.map
                              and status.get("net_mode") == "client") else None
        self.note("client-joined",
                  status=wait_for(f"client joined {self.map}", client_joined, 300, 1))
        for role in ("host", "client"):
            wait_for(f"{role} own pawn", lambda role=role: controlled(self.snap(role)), 180)
        wait_for("both players on both instances",
                 lambda: len(self.players(self.snap("host"))) >= 2
                 and len(self.players(self.snap("client"))) >= 2, 60)
        snaps = self.both()
        self.note("joined", host_pawns=[(p["name"], p["cls"]) for p in self.players(snaps["host"])],
                  client_pawns=[(p["name"], p["cls"]) for p in self.players(snaps["client"])])


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", default="Arena_02")
    parser.add_argument("--host-port", required=True, type=int)
    parser.add_argument("--client-port", required=True, type=int)
    parser.add_argument("--game-port", required=True, type=int)
    parser.add_argument("--token", default="wotangreal")
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--host-screens")
    parser.add_argument("--client-screens")
    parser.add_argument("--host-pid", type=int, default=0)
    parser.add_argument("--client-pid", type=int, default=0)
    parser.add_argument("--phases", default="matrix,rules")
    parser.add_argument("--classes", default="")
    parser.add_argument("--directions", default="both")
    parser.add_argument("--hold-on-failure", type=float, default=0.0)
    parser.add_argument("--probe-seconds", type=float, default=0.0)
    parser.add_argument("--client-faction", default="Forsaken")
    parser.add_argument("--host-timelimit", type=int)
    parser.add_argument("--host-fraglimit", type=int)
    args = parser.parse_args(argv)
    WATCHDOG.watch("host", args.host_pid, None)
    WATCHDOG.watch("client", args.client_pid, None)
    evidence = Path(args.evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    transcript = []
    summary = {"map": args.map, "status": "failed", "phases": args.phases.split(","),
               "classes": [c for c in args.classes.split(",") if c], "directions": args.directions}
    host = client = session = None
    started = time.time()
    return_code = 1
    try:
        host = Channel(args.host_port, args.token, transcript, "host")
        client = Channel(args.client_port, args.token, transcript, "client")
        screens = {"host": Path(args.host_screens) if args.host_screens else None,
                   "client": Path(args.client_screens) if args.client_screens else None}
        session = Session(args.map, host, client, transcript, screens, evidence)
        session.client_faction = args.client_faction or None
        session.host_timelimit = args.host_timelimit
        session.host_fraglimit = args.host_fraglimit
        if args.map.startswith("Battle"):
            # giMPBattle: the seal harness's proven flow (host, join, teams,
            # every citadel edit session readied, fire-to-start) so seals sit
            # in the level and play has begun (damage/targeting rules live).
            from seal_match_driver import Match  # noqa: E402
            match = Match(args.map, host, client, transcript, screens)
            match.prepare(args.game_port)
            match.open_battlefield()
            session.events.extend(match.events)
        else:
            session.join(args.game_port)
        if args.probe_seconds > 0:
            session.note("probe-hold", seconds=args.probe_seconds)
            deadline = time.time() + args.probe_seconds
            while time.time() < deadline:
                WATCHDOG.check()
                time.sleep(2)
            summary["status"] = "probe"
            return_code = 0
        else:
            import matrix_phases as phases  # noqa: E402
            expectations = json.loads(EXPECTATIONS.read_text(encoding="utf-8"))
            results = phases.run(session, expectations, args, summary)
            summary.update(results)
            summary["status"] = "completed"
            return_code = 0
    except FatalRun as exc:
        summary["error"] = f"instance died: {exc}"
        summary["contention_or_crash"] = True
    except Exception as exc:  # the evidence must survive a failed pass
        summary["error"] = f"{type(exc).__name__}: {exc}"
        if session is not None:
            for role in ("host", "client"):
                try:
                    (evidence / f"failure-{role}-snapshot.json").write_text(
                        json.dumps(session.snap(role), indent=1), encoding="utf-8")
                except Exception:
                    pass
            try:
                session.shot("failure")
            except Exception:
                pass
        if args.hold_on_failure > 0:
            print(f"holding instances {args.hold_on_failure}s", flush=True)
            time.sleep(args.hold_on_failure)
    finally:
        summary["duration_s"] = round(time.time() - started, 1)
        if session is not None:
            summary.update(events=session.events, frames=session.frames)
        for channel in (client, host):
            if channel:
                try:
                    channel.command("quit")
                except Exception:
                    pass
        (evidence / "transcript.json").write_text(json.dumps(transcript, indent=1, default=str),
                                                  encoding="utf-8")
        (evidence / "summary.json").write_text(json.dumps(summary, indent=1, default=str),
                                               encoding="utf-8")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
