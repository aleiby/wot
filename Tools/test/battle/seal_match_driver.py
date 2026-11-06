#!/usr/bin/env python3
"""Drive an already-launched two-process packaged Battle evidence run.

The PowerShell launcher owns process/profile isolation. This helper owns only the
token-gated line-JSON conversation and writes an auditable transcript + summary.
It never launches Unreal, so its plan/fixture tests remain engine-free.

Flow (task-6-2-brief deliverable 4): front end -> host the map (listen) -> join
-> both players start neutral -> team selection -> every team's citadel edit
session opens in its edit zone and readies up (budget spent, seals placed on
the team altar) -> a teamed player's fire runs AssessBattlefield ->
EnableTeleporters (phase flip seen on BOTH channels, per-player teleporting
re-enabled) -> [Battle2_01] the host captures enemy seals through the real
altar pickup and banks them on its own altar until the altar goal is met and
the match ends (seen on both channels).
"""
import argparse
import json
import os
import socket
import time
from pathlib import Path


class FatalRun(Exception):
    """A launched instance died or its bootstrap failed: stop waiting at once."""


def process_alive(pid):
    if not pid:
        return True
    if os.name == "nt":
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        code = ctypes.c_ulong()
        ok = kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        kernel32.CloseHandle(handle)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


class Watchdog:
    """Fails the run fast when an instance exits, instead of waiting out every
    channel timeout. Read the exited process's plain diagnostic file."""
    FATAL_EVENTS = ("initialization-failed", "boot-failure", "world-rejected",
                    "host-failed")

    def __init__(self):
        self.roles = {}

    def watch(self, role, pid, heartbeat):
        self.roles[role] = (pid, Path(heartbeat) if heartbeat else None)

    def check(self):
        for role, (pid, heartbeat) in self.roles.items():
            if process_alive(pid):
                continue
            detail = "no failure event recorded"
            if heartbeat and heartbeat.is_dir():
                heartbeat = next(iter(sorted(
                    heartbeat.glob(f"bootstrap-heartbeat-v1.{pid}.*.jsonl"))), None)
            if heartbeat and heartbeat.exists():
                try:
                    lines = heartbeat.read_text(encoding="utf-8", errors="replace").splitlines()
                except OSError:
                    lines = []
                for line in lines:
                    try:
                        record = json.loads(line)
                    except ValueError:
                        continue
                    if record.get("event") in self.FATAL_EVENTS:
                        detail = f"{record.get('event')}: {record.get('detail')}"
                        break
            raise FatalRun(f"{role} process {pid} exited ({detail})")


WATCHDOG = Watchdog()


class Channel:
    def __init__(self, port, token, transcript, role, boot_timeout=420):
        deadline = time.time() + boot_timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                WATCHDOG.check()
                if time.time() >= deadline:
                    raise RuntimeError(f"timeout waiting for {role} channel on {port}")
                time.sleep(1)
        self.sock.settimeout(5)
        self.response_timeout = 300
        self.buf = b""
        self.ident = 0
        self.transcript = transcript
        self.role = role
        self._send({"token": token})

    def _send(self, value):
        self.sock.sendall((json.dumps(value) + "\n").encode("utf-8"))

    def command(self, command, quiet=False, **fields):
        """Send one request and return its response. `quiet` keeps high-rate
        polls (snapshots/status) out of the transcript; every state-changing
        request is recorded."""
        self.ident += 1
        request = {"id": self.ident, "cmd": command, **fields}
        self._send(request)
        # The channel is pumped on the game thread, which can stall for long
        # stretches (content build, map load, world materialization): keep
        # waiting for this request's response up to a generous deadline.
        deadline = time.time() + self.response_timeout
        while True:
            while b"\n" not in self.buf:
                try:
                    chunk = self.sock.recv(65536)
                except socket.timeout:
                    WATCHDOG.check()
                    if time.time() >= deadline:
                        raise RuntimeError(f"{self.role} did not answer {command} "
                                           f"within {self.response_timeout}s")
                    continue
                if not chunk:
                    raise RuntimeError(f"{self.role} closed during {command}")
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            if not line.strip():
                continue
            response = json.loads(line)
            if response.get("id") == self.ident:
                if not quiet:
                    self.transcript.append({"utc": time.time(), "role": self.role,
                                            "request": request, "response": response})
                if not response.get("ok"):
                    raise RuntimeError(f"{self.role} {command}: {response}")
                return response

    def snapshot(self):
        return self.command("snapshot", quiet=True)["data"]

    def status(self):
        return self.command("status", quiet=True)["data"]


def wait_for(description, predicate, timeout=45, interval=.25):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        WATCHDOG.check()
        try:
            last = predicate()
            if last:
                return last
        except (KeyError, StopIteration, RuntimeError, IndexError, TypeError):
            pass
        time.sleep(interval)
    raise RuntimeError(f"timeout: {description}; last={last!r}")


def controlled(snapshot):
    """The snapshotting instance's OWN pawn. On a listen host every player's
    pawn has a controller (`ctrl`), so the locally-controlled flag decides."""
    pawns = snapshot.get("pawns", [])
    if any("local" in pawn for pawn in pawns):
        return next(pawn for pawn in pawns if pawn.get("local"))
    return next(pawn for pawn in pawns if pawn.get("ctrl"))


def battle(channel):
    return channel.snapshot().get("battle") or {}


def altar(snapshot, team):
    return next(item for item in snapshot["battle"]["altars"]
                if item.get("team") == team)


def dist2(a, b):
    return (a["x"] - b["x"]) ** 2 + (a["y"] - b["y"]) ** 2 + (a["z"] - b["z"]) ** 2


class Match:
    def __init__(self, map_name, host, client, transcript, screens):
        self.map = map_name
        self.host = host
        self.client = client
        self.transcript = transcript
        self.screens = screens
        self.events = []
        self.frames = {"host": [], "client": []}

    def note(self, what, **detail):
        entry = {"utc": time.time(), "event": what, **detail}
        self.events.append(entry)
        self.transcript.append({"utc": entry["utc"], "role": "driver", "event": what,
                                "detail": detail})
        print(f"[{self.map}] {what} {json.dumps(detail, sort_keys=True)}", flush=True)

    # -- screenshots: request on both instances, wait for each PNG to land ----
    def shot(self, label):
        before = {}
        for role in ("host", "client"):
            folder = self.screens.get(role)
            before[role] = set(p.name for p in folder.rglob("*.png")) if folder and folder.exists() else set()
        self.host.command("screenshot")
        self.client.command("screenshot")
        landed = {}
        for role in ("host", "client"):
            folder = self.screens.get(role)
            if not folder:
                continue

            def new_png(folder=folder, role=role):
                if not folder.exists():
                    return None
                fresh = [p for p in folder.rglob("*.png") if p.name not in before[role]
                         and p.stat().st_size > 0]
                return fresh[0].name if fresh else None
            try:
                landed[role] = wait_for(f"{role} screenshot {label}", new_png, 30, .5)
                self.frames[role].append({"label": label, "file": landed[role]})
            except RuntimeError:
                landed[role] = None
        self.note("screenshot", label=label, **{f"{r}_file": f for r, f in landed.items()})

    # -- the host-side (authority) name of the client's pawn -------------------
    def client_pawn_on_host(self):
        me = controlled(self.client.snapshot())
        pawns = [p for p in self.host.snapshot().get("pawns", [])
                 if p.get("ctrl") and not p.get("local")]
        if not pawns:
            raise RuntimeError("host snapshot has no remote player pawn")
        return min(pawns, key=lambda p: dist2(p, me))

    def own_team(self, channel):
        return controlled(channel.snapshot()).get("team", 255)

    def prepare(self, game_port):
        """Drive the guarded front end through the production session seams."""
        wait_for("host front end ready", lambda: self.host.status().get("front_end_ready"), 420, 1)
        wait_for("client front end ready", lambda: self.client.status().get("front_end_ready"), 420, 1)
        self.note("front-ends-ready")
        wait_for("host request accepted", lambda: self.host.command(
            "host", map=self.map, game="battle"), 120, 1)

        def host_listening():
            status = self.host.status()
            return status if (status.get("world") == self.map and
                              status.get("net_mode") == "listen") else None

        status = wait_for(f"host listen world {self.map}", host_listening, 300, 1)
        self.note("host-listening", status=status)
        wait_for("join request accepted", lambda: self.client.command(
            "join", address=f"127.0.0.1:{game_port}"), 60, 1)

        def client_joined():
            status = self.client.status()
            return status if (status.get("world") == self.map and
                              status.get("net_mode") == "client") else None

        status = wait_for(f"client joined {self.map}", client_joined, 300, 1)
        self.note("client-joined", status=status)

    def ready_team(self, channel, team):
        """Stand the player in its team's edit zone (at the team altar), open the
        citadel edit session, end it: PlaceSeals + ready (retail CitadelEditor.EndState)."""
        snap = self.host.snapshot()
        place = altar(snap, team)
        if channel is self.client:
            pawn_name = self.client_pawn_on_host()["name"]
        else:
            pawn_name = controlled(snap)["name"]
        for attempt in range(2):
            self.host.command("teleport", pawn=pawn_name, x=place["x"] + 150,
                              y=place["y"], z=place["z"] + 95)
            time.sleep(.8)
            channel.command("exec", c="CitadelBeginEdit 0")
            try:
                wait_for(f"team {team} editing",
                         lambda: battle(self.host)["editing"][team] == 1, timeout=12)
                break
            except RuntimeError:
                if attempt:
                    raise
                self.note("edit-session-restage", team=team)
        self.note("edit-session-open", team=team, pawn=pawn_name)
        channel.command("exec", c="CitadelEndEdit")
        wait_for(f"team {team} ready", lambda: battle(self.host)["ready"][team] == 1, 20)
        state = battle(self.host)
        budget = next((b for b in state.get("budgets", []) if b.get("team") == team), {})
        self.note("team-ready", team=team, numseals=state.get("numseals"),
                  budget_remaining=budget.get("remaining"))
        if state["numseals"][team] <= 0:
            raise RuntimeError(f"team {team} placed no seals on ready-up: {state}")
        if budget.get("remaining", 0) != 0:
            raise RuntimeError(f"team {team} seal budget not spent on ready-up: {budget}")

    def hop(self, team):
        self.client.command("changeteam", team=team)
        wait_for(f"client team {team}", lambda: self.own_team(self.client) == team, 20)
        self.note("client-team", team=team)

    def open_battlefield(self):
        state = battle(self.host)
        self.note("battle-info", numteams=state.get("numteams"),
                  altars=state.get("altars"), budgets=state.get("budgets"),
                  teleportersdisabled=state.get("teleportersdisabled"))
        if state.get("teleportersdisabled") is not True:
            raise RuntimeError("edit phase must start with teleporters disabled")
        if len(state.get("altars", [])) != state.get("numteams"):
            raise RuntimeError(f"one altar per team expected: {state}")
        for channel, role in ((self.host, "host"), (self.client, "client")):
            wait_for(f"{role} own pawn", lambda channel=channel: controlled(channel.snapshot()), 180)
            wait_for(f"{role} starts neutral",
                     lambda channel=channel: self.own_team(channel) == 255, 30)
        self.note("both-neutral")

        # Team selection (Battle honours the request; retail JoinTeam).
        self.host.command("changeteam", team=0)
        self.client.command("changeteam", team=1)
        wait_for("host team 0", lambda: self.own_team(self.host) == 0, 20)
        wait_for("client team 1", lambda: self.own_team(self.client) == 1, 20)
        self.note("teams-selected", host=0, client=1)
        for channel, role in ((self.host, "host"), (self.client, "client")):
            wait_for(f"{role} frozen for the edit phase",
                     lambda channel=channel: controlled(channel.snapshot()).get("teleportdisabled") is True, 20)

        # Editing phase for EVERY team (retail opens the battlefield only when
        # every BudgetInfo is spent, including citadels without players).
        self.ready_team(self.host, 0)
        numteams = state.get("numteams", 2)
        for team in range(1, min(numteams, 4)):
            if self.own_team(self.client) != team:
                self.hop(team)
            self.ready_team(self.client, team)
        if self.own_team(self.client) != 1:
            self.hop(1)
        self.shot("editing-complete")

        time.sleep(1.0)
        if battle(self.host).get("teleportersdisabled") is False:
            raise RuntimeError("play started without a fire (ready-up must not assess)")
        # AssessBattlefield: a teamed player's fire while frozen
        # (WOTPlayer.UseAngreal -> ServerAssessBattlefield) -> EnableTeleporters.
        self.host.command("castpress")
        time.sleep(.1)
        self.host.command("castrelease")
        wait_for("host sees EnableTeleporters",
                 lambda: battle(self.host).get("teleportersdisabled") is False, 30)
        wait_for("client sees EnableTeleporters (replicated BattleInfo)",
                 lambda: battle(self.client).get("teleportersdisabled") is False, 30)
        for channel, role in ((self.host, "host"), (self.client, "client")):
            wait_for(f"{role} teleporting re-enabled",
                     lambda channel=channel: controlled(channel.snapshot()).get("teleportdisabled") is False, 30)
        self.note("battlefield-open", numseals=battle(self.host).get("numseals"))
        self.shot("play")

    def seal_objective(self):
        """Capture enemy seals through the real altar pickup and bank them on
        the host team's altar until the altar goal ends the match."""
        snap = self.host.snapshot()
        own, enemy = altar(snap, 0), altar(snap, 1)
        goal = own.get("goal")
        host_pawn = controlled(snap)["name"]
        self.note("objective-start", goal=goal, numseals=snap["battle"].get("numseals"))
        provisioned = 0

        def counts():
            return battle(self.host).get("numseals", [0, 0, 0, 0])

        def touch(where, label):
            # Leave any altar first so each touch is a fresh begin-overlap, then
            # drop the host pawn onto the altar (it lands on the altar's flat
            # UE1 cylinder).
            self.host.command("teleport", pawn=host_pawn, x=where["x"] + 400,
                              y=where["y"], z=where["z"] + 120)
            time.sleep(.3)
            self.host.command("teleport", pawn=host_pawn, x=where["x"],
                              y=where["y"], z=where["z"] + 60)
            time.sleep(.5)
            me = controlled(self.host.snapshot())
            self.note("touch", altar=label, pawn_at=[me.get("x"), me.get("y"), me.get("z")],
                      altar_at=[where["x"], where["y"], where["z"]])

        def ended():
            return all(a.get("ended") for a in battle(self.host).get("altars", []))

        def carried_items():
            return controlled(self.host.snapshot()).get("invcount", 0)

        for _ in range(goal + 2):
            if ended():
                break
            before = counts()
            items = carried_items()
            if before[1] > 0:
                # SealAltar.Active.Touch: an empty-handed player takes the last
                # seal on the altar (PickupQuery permits it once play began).
                # Team counts only move when a seal is PLACED (SetControllingTeam).
                touch(enemy, "enemy")
                wait_for("enemy seal carried", lambda: carried_items() > items, 10)
                self.note("seal-captured", numseals=counts(), invcount=carried_items())
            else:
                # Only when the map's seal census cannot reach the goal: a real
                # AWoTSeal routed through TryPickupBy (guarded test verb).
                self.host.command("give_seal", pawn=host_pawn)
                provisioned += 1
                wait_for("provisioned seal carried", lambda: carried_items() > items, 10)
                self.note("seal-provisioned", count=provisioned)
                if provisioned > goal:
                    raise RuntimeError("seal goal did not advance after guarded provisioning")
            time.sleep(1.1)  # SealAltar Inactive debounce (Sleep(1.0)) on the altar just used
            touch(own, "own")
            wait_for("seal banked on own altar",
                     lambda: counts()[0] > before[0] or ended(), 10)
            self.note("seal-banked", numseals=counts(), ended=ended())
        wait_for("altar goal ended the match", ended, 20)
        wait_for("host match-ended", lambda: self.host.snapshot().get("matchended") is True, 30)
        wait_for("client match-ended", lambda: self.client.snapshot().get("matchended") is True, 30)
        final = battle(self.host)
        self.note("objective-complete", numseals=final.get("numseals"),
                  altars=final.get("altars"), provisioned=provisioned)
        self.shot("objective")
        return final, provisioned


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", required=True)
    parser.add_argument("--host-port", required=True, type=int)
    parser.add_argument("--client-port", required=True, type=int)
    parser.add_argument("--game-port", required=True, type=int)
    parser.add_argument("--token", default="wotnet")
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--host-screens")
    parser.add_argument("--client-screens")
    parser.add_argument("--host-pid", type=int, default=0)
    parser.add_argument("--client-pid", type=int, default=0)
    parser.add_argument("--host-heartbeat")
    parser.add_argument("--client-heartbeat")
    # Debug aid: keep both instances up this long after a failure so a second
    # channel connection can probe them (the channel accepts several clients).
    parser.add_argument("--hold-on-failure", type=float, default=0.0)
    args = parser.parse_args()
    WATCHDOG.watch("host", args.host_pid, args.host_heartbeat)
    WATCHDOG.watch("client", args.client_pid, args.client_heartbeat)
    evidence = Path(args.evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    transcript = []
    full = args.map == "Battle2_01"
    summary = {"map": args.map, "mode": "full" if full else "smoke", "status": "failed",
               "phase_started": False, "objective_complete": False,
               "test_seals_provisioned": 0}
    host = client = None
    match = None
    started = time.time()
    try:
        host = Channel(args.host_port, args.token, transcript, "host")
        client = Channel(args.client_port, args.token, transcript, "client")
        screens = {"host": Path(args.host_screens) if args.host_screens else None,
                   "client": Path(args.client_screens) if args.client_screens else None}
        match = Match(args.map, host, client, transcript, screens)
        match.prepare(args.game_port)
        match.open_battlefield()
        summary["phase_started"] = True
        if full:
            final, provisioned = match.seal_objective()
            summary["objective_complete"] = True
            summary["final_numseals"] = final.get("numseals")
            summary["test_seals_provisioned"] = provisioned
        summary["status"] = "passed"
        return_code = 0
    except Exception as exc:  # evidence must survive a failed later pass
        summary["error"] = str(exc)
        return_code = 1
        if match is not None:
            # Failure evidence: both instances' full snapshots at the moment
            # of failure (pawns, pickups/seals, altars, phase flags).
            for channel, role in ((host, "host"), (client, "client")):
                try:
                    (evidence / f"failure-{role}-snapshot.json").write_text(
                        json.dumps(channel.snapshot(), indent=2), encoding="utf-8")
                except Exception:
                    pass
            try:
                match.shot("failure")
            except Exception:
                pass
        if args.hold_on_failure > 0:
            print(f"[{args.map}] holding instances {args.hold_on_failure}s for probing", flush=True)
            time.sleep(args.hold_on_failure)
    finally:
        summary["duration_s"] = round(time.time() - started, 1)
        if match is not None:
            summary["events"] = match.events
            summary["frames"] = match.frames
        for channel in (client, host):
            if channel:
                try:
                    channel.command("quit")
                except Exception:
                    pass
        (evidence / "transcript.json").write_text(
            json.dumps(transcript, indent=2), encoding="utf-8")
        (evidence / "summary.json").write_text(
            json.dumps(summary, indent=2), encoding="utf-8")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
