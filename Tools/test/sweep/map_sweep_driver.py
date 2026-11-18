#!/usr/bin/env python3
"""Drive one map of the 19-map guarded host + join sweep.

The PowerShell launcher (run_map_sweep.ps1) owns process/profile isolation
and has already started one host and one client for THIS map. This driver
reuses the seal-match channel plumbing (Tools/test/battle/seal_match_driver:
token-gated line-JSON Channel, process Watchdog, wait_for, controlled) and
never launches Unreal, so its logic is unit-testable offline.

Flow: both front ends ready -> host the map (arena/battle by prefix) -> the
host reports a listen world of this identity -> the client joins: `direct`
connects to 127.0.0.1:<game port>; `session` drives the REAL multiplayer
browser (Play screen, LAN box, Refresh -> the host's row is listed -> select
it and Join: Find -> JoinSession -> resolved connect string -> travel) ->
the client reports a client world of this identity -> both own pawns exist ->
record each role's map identity, retail title and the HUD header's map line
(the scoreboard/overview must print Level.Title, never WoTMapShellNN) ->
screenshot both -> [Arena] teleport the client's pawn (host authority) onto a
projected pickup in Pickup state and require its class to enter the client
pawn's inventory on the server AND on the client -> screenshot -> quit both.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "battle"))
sys.path.insert(0, str(HERE))
from seal_match_driver import (  # noqa: E402  pylint: disable=wrong-import-position
    WATCHDOG, Channel, controlled, dist2, wait_for)
from sweep_checks import (  # noqa: E402  pylint: disable=wrong-import-position
    ARENA_MAPS, inventory_gained, load_expectations)

JOIN_MODES = ("direct", "session")
PICKUP_ATTEMPTS = 3
# Heights (above the pickup's origin) the client's pawn is dropped at: first
# the proven direct-overlap drop of Tools/test/net/scenarios/arena_slice.py
# (z+95, the pawn lands on the item under gravity), then a capsule-overlap
# placement. Either arrival raises a fresh begin-overlap (the pawn was nowhere
# near the item before), i.e. the real Inventory.Touch path.
PICKUP_DROP_HEIGHTS = (95.0, 20.0)


def view(snapshot: dict) -> dict:
    """What one instance's UI shows for the map (snapshot fields added for
    the sweep in WoTNetTestSnapshot.cpp), plus how many classes its own
    pawn's angreal hand set maps (0 = DT_WoTAngrealHands never loaded)."""
    seen = {key: snapshot.get(key) for key in
            ("map", "map_identity", "map_title", "map_title_retail",
             "scoreboard_map", "overview_map")}
    own = next((pawn for pawn in snapshot.get("pawns", ()) if pawn.get("local")), None)
    for key in ("hand_classes", "hand_plan_classes"):
        if own is not None and key in own:
            seen[key] = own[key]
    return seen


def live_pickups(snapshot: dict) -> list[dict]:
    return [item for item in snapshot.get("pickups", ())
            if item.get("state") == "Pickup" and not item.get("hidden")]


def choose_pickups(pickups: list[dict], held: set[str], near: dict | None) -> list[dict]:
    """Deterministic order: classes the client does not hold first (a clean
    new-item signal), then nearest to the client pawn, then by name."""
    def key(item):
        distance = dist2(item, near) if near else 0.0
        return (item.get("cls") in held, distance, item.get("name", ""))
    return sorted(pickups, key=key)


def browser_row_for(status: dict, map_name: str) -> dict | None:
    """The browser row advertising this map, once its search has FINISHED.

    Only a completed search is joinable (UWoTSessionSubsystem::Join refuses
    an entry from a search generation still in flight), and the Null OSS LAN
    search runs out its full timeout before completing."""
    browser = status.get("browser") or {}
    if not browser.get("available") or browser.get("operation") != "none":
        return None
    return next((row for row in browser.get("rows", ())
                 if row.get("map") == map_name), None)


def pawn_named(snapshot: dict, name: str) -> dict | None:
    return next((pawn for pawn in snapshot.get("pawns", ()) if pawn.get("name") == name), None)


class Sweep:
    def __init__(self, map_name, host, client, transcript, screens):
        self.map = map_name
        self.host = host
        self.client = client
        self.transcript = transcript
        self.screens = screens
        self.events = []
        self.frames = {"host": [], "client": []}
        self.views = {}
        self.pickup = None
        self.browser_row = None

    def note(self, what, **detail):
        entry = {"utc": time.time(), "event": what, **detail}
        self.events.append(entry)
        self.transcript.append({"utc": entry["utc"], "role": "driver", "event": what,
                                "detail": detail})
        print(f"[{self.map}] {what} {json.dumps(detail, sort_keys=True)}", flush=True)

    def shot(self, label):
        before = {}
        for role in ("host", "client"):
            folder = self.screens.get(role)
            before[role] = set(p.name for p in folder.rglob("*.png")) \
                if folder and folder.exists() else set()
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
                fresh = [p for p in folder.rglob("*.png")
                         if p.name not in before[role] and p.stat().st_size > 0]
                return fresh[0].name if fresh else None
            try:
                landed[role] = wait_for(f"{role} screenshot {label}", new_png, 30, .5)
                self.frames[role].append({"label": label, "file": landed[role]})
            except RuntimeError:
                landed[role] = None
        self.note("screenshot", label=label, **{f"{r}_file": f for r, f in landed.items()})

    def join_browser(self):
        """Multiplayer -> LAN -> Refresh until the host's row is listed, then
        select that row and press Join. A refresh can finish before the
        host's beacon answers, so an idle browser without the row is
        refreshed again."""
        def listed():
            status = self.client.status()
            row = browser_row_for(status, self.map)
            if row is not None:
                return row
            if (status.get("browser") or {}).get("operation") == "none":
                self.client.command("find", lan=True)
            return None
        self.browser_row = wait_for(f"browser lists the {self.map} host",
                                    listed, 120, 1)
        self.note("browser-listed", row=self.browser_row)
        wait_for("browser join accepted", lambda: self.client.command(
            "join", row=self.browser_row["row"]), 60, 1)
        self.note("browser-joined", row=self.browser_row["row"])

    def join(self, game_port, join_mode="direct"):
        wait_for("host front end ready",
                 lambda: self.host.status().get("front_end_ready"), 420, 1)
        wait_for("client front end ready",
                 lambda: self.client.status().get("front_end_ready"), 420, 1)
        self.note("front-ends-ready")
        game = "arena" if self.map in ARENA_MAPS else "battle"
        wait_for("host request accepted",
                 lambda: self.host.command("host", map=self.map, game=game), 120, 1)

        def host_listening():
            status = self.host.status()
            return status if (status.get("world") == self.map
                              and status.get("net_mode") == "listen") else None
        self.note("host-listening",
                  status=wait_for(f"host listen world {self.map}", host_listening, 300, 1))
        if join_mode == "session":
            self.join_browser()
        else:
            wait_for("join request accepted", lambda: self.client.command(
                "join", address=f"127.0.0.1:{game_port}"), 60, 1)

        def client_joined():
            status = self.client.status()
            return status if (status.get("world") == self.map
                              and status.get("net_mode") == "client") else None
        self.note("client-joined",
                  status=wait_for(f"client joined {self.map}", client_joined, 300, 1))
        for channel, role in ((self.host, "host"), (self.client, "client")):
            wait_for(f"{role} own pawn",
                     lambda channel=channel: controlled(channel.snapshot()), 180)

    def check_titles(self, expected_title):
        # Right after travel a client can briefly report no local pawn; wait
        # (bounded) until each side's own pawn reports its hand plan.
        for channel, role in ((self.host, "host"), (self.client, "client")):
            def own_view(channel=channel):
                seen = view(channel.snapshot())
                return seen if "hand_plan_classes" in seen else None
            try:
                self.views[role] = wait_for(f"{role} local pawn view", own_view, 60, 1)
            except Exception:
                self.views[role] = view(channel.snapshot())
        mismatches = {role: seen for role, seen in self.views.items()
                      if seen.get("map_identity") != self.map
                      or seen.get("map_title") != expected_title
                      or seen.get("scoreboard_map") != expected_title}
        self.note("titles-checked", expected=expected_title, views=self.views,
                  mismatches=sorted(mismatches))
        if mismatches:
            raise RuntimeError(f"map name/title surfaces disagree: {mismatches}")

    def client_pawn_on_host(self):
        me = controlled(self.client.snapshot())
        pawns = [p for p in self.host.snapshot().get("pawns", [])
                 if p.get("ctrl") and not p.get("local")]
        if not pawns:
            raise RuntimeError("host snapshot has no remote player pawn")
        return min(pawns, key=lambda p: dist2(p, me))

    def take_pickup(self):
        client_me = controlled(self.client.snapshot())
        pickups = wait_for("a projected pickup in Pickup state",
                           lambda: live_pickups(self.host.snapshot()) or None, 90, 1)
        held = {item.get("cls") for item in client_me.get("inv", ())}
        ordered = choose_pickups(pickups, held, client_me)
        self.note("pickups-seen", count=len(pickups),
                  classes=sorted({item.get("cls") for item in pickups}))
        failures = []
        for attempt, target in enumerate(ordered[:PICKUP_ATTEMPTS], 1):
            pawn = self.client_pawn_on_host()
            server_before = pawn.get("inv", [])
            client_before = controlled(self.client.snapshot()).get("inv", [])
            cls = target.get("cls")

            def server_gain(name=pawn["name"], before=server_before, wanted=cls):
                seen = pawn_named(self.host.snapshot(), name) or {}
                after = seen.get("inv", [])
                return after if inventory_gained(before, after, wanted) else None

            def client_gain(before=client_before, wanted=cls):
                after = controlled(self.client.snapshot()).get("inv", [])
                return after if inventory_gained(before, after, wanted) else None

            server_after = None
            for height in PICKUP_DROP_HEIGHTS:
                self.host.command("teleport", pawn=pawn["name"], x=target["x"],
                                  y=target["y"], z=target["z"] + height)
                try:
                    server_after = wait_for(f"server inventory gains {cls}",
                                            server_gain, 10, .25)
                    break
                except RuntimeError as exc:
                    failures.append(f"attempt {attempt} {target.get('name')} "
                                    f"z+{height:.0f}: {exc}")
            if server_after is None:
                self.note("pickup-attempt-failed", attempt=attempt,
                          pickup=target.get("name"), cls=cls)
                continue
            try:
                client_after = wait_for(f"client inventory gains {cls}", client_gain, 15, .25)
            except RuntimeError as exc:
                # The server took it: a client that never sees the item is the
                # replication defect this check exists for - fail, do not retry.
                raise RuntimeError(f"{cls} entered the server inventory but never "
                                   f"the client's: {exc}") from exc
            left = next((item for item in self.host.snapshot().get("pickups", ())
                         if item.get("name") == target.get("name")), None)
            self.pickup = {
                "name": target.get("name"), "cls": cls, "attempt": attempt,
                "at": [target.get("x"), target.get("y"), target.get("z")],
                "pickup_state_after": left.get("state") if left else "destroyed",
                "server_before": server_before, "server_after": server_after,
                "client_before": client_before, "client_after": client_after,
            }
            self.note("pickup-taken", **{k: v for k, v in self.pickup.items()
                                         if not k.endswith(("_before", "_after"))})
            return self.pickup
        raise RuntimeError(f"no projected pickup entered the client inventory: {failures}")


def main(argv=None):
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
    parser.add_argument("--expectations")
    parser.add_argument("--hold-on-failure", type=float, default=0.0)
    parser.add_argument("--join-mode", choices=JOIN_MODES, default="direct")
    args = parser.parse_args(argv)
    expectations = load_expectations(Path(args.expectations) if args.expectations else None)
    row = expectations["maps"][args.map]
    # Heartbeats are read only by the launcher after the writers exit (a
    # concurrent reader can latch the game's heartbeat off; seal-match run 8).
    WATCHDOG.watch("host", args.host_pid, None)
    WATCHDOG.watch("client", args.client_pid, None)
    evidence = Path(args.evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    transcript = []
    summary = {"map": args.map, "mode": row["mode"], "status": "failed",
               "expected_title": row["title"], "pickup_required": bool(row.get("pickup")),
               "join_mode": args.join_mode}
    host = client = sweep = None
    started = time.time()
    try:
        host = Channel(args.host_port, args.token, transcript, "host")
        client = Channel(args.client_port, args.token, transcript, "client")
        screens = {"host": Path(args.host_screens) if args.host_screens else None,
                   "client": Path(args.client_screens) if args.client_screens else None}
        sweep = Sweep(args.map, host, client, transcript, screens)
        sweep.join(args.game_port, args.join_mode)
        sweep.check_titles(row["title"])
        sweep.shot("world")
        if row.get("pickup"):
            sweep.take_pickup()
            sweep.shot("pickup")
        summary["status"] = "passed"
        return_code = 0
    except Exception as exc:  # the evidence must survive a failed pass
        summary["error"] = str(exc)
        return_code = 1
        if sweep is not None:
            for channel, role in ((host, "host"), (client, "client")):
                try:
                    (evidence / f"failure-{role}-snapshot.json").write_text(
                        json.dumps(channel.snapshot(), indent=2), encoding="utf-8")
                except Exception:
                    pass
            try:
                sweep.shot("failure")
            except Exception:
                pass
        if args.hold_on_failure > 0:
            print(f"[{args.map}] holding instances {args.hold_on_failure}s", flush=True)
            time.sleep(args.hold_on_failure)
    finally:
        summary["duration_s"] = round(time.time() - started, 1)
        if sweep is not None:
            summary.update(events=sweep.events, frames=sweep.frames,
                           views=sweep.views, pickup=sweep.pickup,
                           browser_row=sweep.browser_row)
        for channel in (client, host):
            if channel:
                try:
                    channel.command("quit")
                except Exception:
                    pass
        (evidence / "transcript.json").write_text(json.dumps(transcript, indent=2),
                                                  encoding="utf-8")
        (evidence / "summary.json").write_text(json.dumps(summary, indent=2),
                                               encoding="utf-8")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
