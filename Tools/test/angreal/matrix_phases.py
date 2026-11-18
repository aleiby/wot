#!/usr/bin/env python3
"""The phases of the angreal matrix driver (see angreal_matrix_driver.py).

Only harness choreography lives here; expected observables come from
angreal_expectations.json and are judged offline (matrix_logic)."""
from __future__ import annotations

import json
import math
import time

from seal_match_driver import wait_for
import matrix_logic as logic

# Proven two-player test lane per map (both nodes are exports/t3d PathNodes on
# the same floor; a host Dart from A reached B and hurt the target, probe run
# 20260929T050341Z). GRAVE is a far lower-level PathNode where cleanup kills
# happen, so the dropped BagHolding never lands in the test lane.
LANES = {
    "Arena_02": {"a": (-376.0, 223.0, 60.0), "b": (169.0, 249.0, 60.0),
                 "grave_host": (11.0, -2084.0, -280.0), "grave_client": (137.0, -2169.0, -280.0),
                 "cite": "exports/t3d/Arena_02.t3d PathNode24/PathNode27; PathNode92/PathNode1"},
}
# Classes whose frames are kept as the representative visual subset.
SCREENSHOT = {"Fireball", "Balefire", "Lightning", "Heal", "Shield", "FireShield", "Levitate",
              "Disguise", "Whirlwind", "Minion", "WallOfAir", "SwapPlaces", "Ice", "Seeker",
              "EarthTremor", "AirBurst", "Decay"}


def fire(channel):
    """One fire tap through the real input path. A dead player has no pawn to
    release on (WoTNetTest::CastRelease fails), which is expected there."""
    channel.command("castpress")
    try:
        channel.command("castrelease")
    except RuntimeError:
        pass


class Matrix:
    def __init__(self, session, expectations, args):
        self.s = session
        self.rows = {row["class"]: row for row in expectations["classes"]}
        self.order = expectations.get("order") or list(self.rows)
        self.lane = LANES.get(session.map)
        # Battle maps: casts happen where the battle flow left the players
        # (no lane teleports, no kill cleanup - only lane-independent classes).
        self.lane_less = self.lane is None
        self.context = "battle" if session.map.startswith("Battle") else "arena"
        self.args = args
        self.results = []

    # -- players ----------------------------------------------------------------
    def alive_names(self, timeout=25):
        """Host-side names of both living players; respawns dead ones with a
        restart-fire (WOTPlayer.Dying.Fire -> ServerReStartPlayer)."""
        deadline = time.time() + timeout
        fired = {}
        while True:
            # A dead player's controller is unpossessed: its corpse keeps no
            # PlayerState/local flag, so "no living player pawn" means dead.
            hs = self.s.snap("host")
            living = [p for p in self.s.players(hs) if not p.get("dead")]
            me = next((p for p in living if p.get("local")), None)
            other = next((p for p in living if not p.get("local")), None)
            dead = {"host": me is None, "client": other is None}
            if not any(dead.values()):
                cs = self.s.snap("client")
                mine = next((p for p in self.s.players(cs) if p.get("local")
                             and not p.get("dead")), None)
                if mine and mine.get("health", 0) > 0:
                    return {"host": me["name"], "client": other["name"]}
            for role, is_dead in dead.items():
                if is_dead and time.time() - fired.get(role, 0) > 1.2:
                    fire(self.s.chan[role])
                    fired[role] = time.time()
            if time.time() > deadline:
                raise RuntimeError(f"players did not come back alive: {dead}")
            time.sleep(.25)

    def client_view_names(self):
        """The client's names for both players. On a large Battle map the host
        pawn can be net-irrelevant to the client (no actor channel), so its
        client-side name may be absent (observables there judge as not seen)."""
        cs = self.s.snap("client")
        try:
            other = self.s.other_player(cs)["name"]
        except RuntimeError:
            other = None
        return {"client": self.s.own("client", cs)["name"], "host": other}

    def settle(self, names, timeout=4.0):
        def still():
            hs = self.s.snap("host")
            rows = [logic.pawn(hs, names[r]) for r in ("host", "client")]
            return all(r and not r.get("dead") and r.get("movemode") == 1
                       and abs(r.get("vz") or 0) < 5 for r in rows)
        try:
            wait_for("pawns settled", still, timeout, .1)
        except RuntimeError:
            pass

    def place(self, names, caster_role, distance=None):
        a, b = self.lane["a"], self.lane["b"]
        if distance:
            span = math.dist(a[:2], b[:2])
            f = min(1.0, max(0.1, distance / span))
            b = (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, b[2])
        target_role = "client" if caster_role == "host" else "host"
        self.s.host.command("teleport", pawn=names[caster_role], x=a[0], y=a[1], z=a[2])
        self.s.host.command("teleport", pawn=names[target_role], x=b[0], y=b[1], z=b[2])
        time.sleep(.3)
        self.settle(names)

    def cleanup(self, names):
        """Fresh players for the next cast: kill both away from the lane (their
        bags drop there) and restart-fire them; kill summoned NPCs too."""
        hs = self.s.snap("host")
        for p in hs.get("pawns", []):
            if not p.get("player") and not p.get("dead"):
                try:
                    self.s.host.command("damage", pawn=p["name"], amount=5000)
                except RuntimeError:
                    pass
        for role, grave in (("host", self.lane["grave_host"]), ("client", self.lane["grave_client"])):
            try:
                self.s.host.command("teleport", pawn=names[role], x=grave[0], y=grave[1], z=grave[2])
            except RuntimeError:
                pass
        time.sleep(.4)
        for role in ("host", "client"):
            try:
                self.s.host.command("damage", pawn=names[role], amount=5000)
            except RuntimeError:
                pass
        # World weaves outlive a cast: an untripped AMA mine never expires (retail
        # LifeSpan 0, trips on any pawn or projectile touching its 12uu collision)
        # and EarthTremor rocks live 15 s. Clear them so the next cast starts clean
        # (a binary without the verb answers "unknown cmd": tolerated).
        try:
            self.s.host.command("clearprojectiles")
        except RuntimeError:
            pass
        time.sleep(1.15)   # WOTPlayer.RespawnDelaySecs = 1.0 (WOTPlayer.uc:7126)
        return self.alive_names()

    # -- casting -----------------------------------------------------------------
    def grant(self, names, caster_role, cls, charges):
        """Grant + select on the authority; wait until the caster owns and
        selects it (server view, and the owning client's replicated view)."""
        before = logic.pawn(self.s.snap("host"), names[caster_role]) or {}
        held = logic.item(before, cls)
        self.s.host.command("give", pawn=names[caster_role], **{"class": cls}, charges=charges)

        def server_has():
            row = logic.pawn(self.s.snap("host"), names[caster_role]) or {}
            entry = logic.item(row, cls)
            return entry if entry and row.get("selected") == cls else None
        entry = wait_for(f"server grants {cls}", server_has, 8, .1)
        if caster_role == "client":
            def client_has():
                row = self.s.own("client")
                got = logic.item(row, cls)
                return got if got and row.get("selected") == cls else None
            wait_for(f"client sees {cls} selected", client_has, 8, .1)
        return entry.get("charges"), held is not None

    def cast(self, caster_role, hold_s, window_s, target_names, shot_label=None):
        """Press/release fire through the real input path while polling both
        instances; returns the timeline."""
        chan = self.s.chan[caster_role]
        timeline = []
        start = time.time()
        chan.command("castpress")
        released = hold_s <= 0
        if released:
            chan.command("castrelease")
        shot_taken = shot_label is None
        last = {}
        while time.time() - start < window_s:
            for role in ("host", "client"):
                frame = logic.lite(self.s.snap(role))
                # Keep only frames that changed (records stay small; the
                # verdicts only look at state transitions).
                key = logic.frame_key(frame)
                if key != last.get(role):
                    last[role] = key
                    timeline.append({"t": round(time.time() - start, 3), "role": role,
                                     "lite": frame})
            time.sleep(.02)
            if not released and time.time() - start >= hold_s:
                chan.command("castrelease")
                released = True
            if not shot_taken and time.time() - start >= min(0.35, window_s / 3):
                self.s.shot(shot_label)
                shot_taken = True
        if not released:
            chan.command("castrelease")
        return timeline

    def setup(self, row, names, caster_role, errors):
        """Harness preconditions the expectation row names (e.g. a damaged
        caster for Heal, a curse on the caster for RemoveCurse)."""
        target_role = "client" if caster_role == "host" else "host"
        for step in row.get("setup", []):
            if "damage" in step:
                who = caster_role if step["damage"]["who"] == "caster" else target_role
                self.s.host.command("damage", pawn=names[who], amount=step["damage"]["amount"])
                time.sleep(.3)
            elif "precast" in step:
                spec = step["precast"]
                pre_row = self.rows.get(spec["class"], {})
                by = caster_role if spec.get("by") == "caster" else target_role
                at = target_role if by == caster_role else caster_role
                cls = pre_row.get("cpp_class") or f"WoTAngrealInv{spec['class']}"
                try:
                    self.grant(names, by, cls, spec.get("charges", 5))
                    self.aim(by, at)
                    self.cast(by, spec.get("hold_s", .15), spec.get("wait_s", 2.0), None)
                except RuntimeError as exc:
                    errors.append(f"setup precast {spec['class']}: {exc}")
            elif "give" in step:
                spec = step["give"]
                who = caster_role if spec["who"] == "caster" else target_role
                try:
                    self.s.host.command("give", pawn=names[who], **{"class": spec["class"]},
                                        charges=spec.get("charges", 5))
                except RuntimeError as exc:
                    errors.append(f"setup give {spec['class']}: {exc}")
            elif "seal" in step:
                who = caster_role if step["seal"]["who"] == "caster" else target_role
                try:
                    self.s.host.command("give_seal", pawn=names[who])
                except RuntimeError as exc:
                    errors.append(f"setup seal: {exc}")
            elif "wait" in step:
                time.sleep(step["wait"])

    def aim(self, caster_role, target_role, mode="target"):
        """Aim the caster's view (yaw+pitch, WoTNetTest::AimTarget/AimPoint) as
        the CASTER's instance sees the lane: at the other player's centre, level
        toward it (Shift), or at the floor ~150 uu toward it (ExpWard)."""
        del target_role  # two-player lane: the other player IS the target
        if mode == "none":
            return   # the artifact ignores the view (e.g. Tracer seeks a Seal)
        snap = self.s.snap(caster_role)
        other = self.s.other_player(snap)
        if mode == "target":
            self.s.chan[caster_role].command("aim", pawn=other["name"])
            return
        me = self.s.own(caster_role, snap)
        dx, dy = other["x"] - me["x"], other["y"] - me["y"]
        span = math.hypot(dx, dy) or 1.0
        if mode == "level":
            far = 100000.0 / span
            point = (me["x"] + dx * far, me["y"] + dy * far, me["z"] + 40.0)
        else:  # floor
            point = (me["x"] + dx * 150.0 / span, me["y"] + dy * 150.0 / span, me["z"] - 46.0)
        self.s.chan[caster_role].command("aim", x=point[0], y=point[1], z=point[2])

    def run_one(self, row, direction):
        caster_role = "host" if direction == "h2c" else "client"
        target_role = "client" if caster_role == "host" else "host"
        errors = []
        names = self.alive_names()
        rng = row.get("range") or {}
        distance = row.get("lane_distance") or (None if not rng else
                                               max(rng.get("min", 0), min(rng.get("max", 545), 545)))
        if not self.lane_less:
            self.place(names, caster_role, distance if distance and distance < 545 else None)
        self.setup(row, names, caster_role, errors)
        names = {"host": names["host"], "client": names["client"]}
        view = self.client_view_names()
        record_names = {
            "host": {"caster": names[caster_role], "target": names[target_role]},
            "client": {"caster": view[caster_role], "target": view[target_role]},
        }
        cls = row["cpp_class"]
        granted, pre_held = None, False
        grant_ok = True
        try:
            granted, pre_held = self.grant(names, caster_role, cls, row.get("grant_charges", 5))
        except RuntimeError as exc:
            grant_ok = False
            errors.append(f"grant: {exc}")
        if grant_ok:
            aim_mode = row.get("aim_mode") or (
                "none" if (row.get("cast") or {}).get("aim") == "none" and self.lane_less
                else "target")
            self.aim(caster_role, target_role, aim_mode)
            time.sleep(.15)
        pre = {role: logic.lite(self.s.snap(role)) for role in ("host", "client")}
        timeline = []
        if grant_ok:
            cast = row.get("cast") or {}
            label = f"{row['class']}-{direction}" if row["class"] in SCREENSHOT else None
            try:
                timeline = self.cast(caster_role, cast.get("hold_s", .2),
                                     row.get("window_s", 3.0), record_names, label)
            except RuntimeError as exc:
                errors.append(f"cast: {exc}")
        post = {role: logic.lite(self.s.snap(role)) for role in ("host", "client")}
        record = {"class": row["class"], "cpp_class": cls, "direction": direction,
                  "caster_role": caster_role, "target_role": target_role,
                  "names": record_names, "granted": granted, "pre_held": pre_held,
                  "grant_ok": grant_ok, "pre": pre, "timeline": timeline, "post": post,
                  "errors": errors, "utc": time.time()}
        self.s.record(record)
        verdict = logic.judge_cast(record, row)
        self.s.note("cast", cls=row["class"], direction=direction, verdict=verdict["verdict"],
                    failed=[c["kind"] for c in verdict["checks"] if c["verdict"] == "fail"])
        self.results.append({"class": row["class"], "direction": direction,
                             "verdict": verdict["verdict"]})
        if self.lane_less:
            return record
        try:
            self.cleanup(names)
        except RuntimeError as exc:
            self.s.note("cleanup-failed", error=str(exc))
            raise
        if row.get("post_wait_s"):
            time.sleep(row["post_wait_s"])
        return record

    def run(self, classes, directions):
        wanted = [c for c in self.order if (not classes or c in classes) and c in self.rows]
        dirs = ["h2c", "c2h"] if directions == "both" else [directions]
        names = self.alive_names()
        if not self.lane_less:
            self.cleanup(names)
        for cls in wanted:
            row = self.rows[cls]
            if not row.get("obtainable", True):
                continue
            if row.get("context", "arena") != self.context:
                continue
            for direction in dirs:
                self.run_one(row, direction)
        return self.results


class Rules:
    """giMPArena match rules against the shipped script values."""

    def __init__(self, session, expectations):
        self.s = session
        self.rules = expectations.get("rules", {})
        self.m = Matrix.__new__(Matrix)
        self.m.s = session
        self.m.lane = LANES[session.map]
        self.m.lane_less = False
        self.out = {}

    def players_row(self, role="host"):
        return {p["name"]: p for p in self.s.snap(role).get("players", [])}

    def score(self, snapshot, local=True):
        """CalcScore = Kills - Suicides (the port's AWoTPlayerState::CalcScore)."""
        pawn = self.s.own("host", snapshot) if local else self.s.other_player(snapshot)
        row = next(p for p in snapshot.get("players", []) if p["name"] == pawn.get("player"))
        return row["kills"] - row["suicides"], row

    def living_local(self, role):
        return next((p for p in self.s.players(self.s.snap(role))
                     if p.get("local") and not p.get("dead")), None)

    def respawn_gate(self):
        """WOTPlayer.Dying.Fire respawns only once Level.TimeSeconds >=
        OKToFireTime = death + RespawnDelaySecs (1.0; WOTPlayer.uc:3620,3738,7126)."""
        names = self.m.alive_names()
        self.s.host.command("damage", pawn=names["client"], amount=5000)
        wait_for("client dead", lambda: self.living_local("client") is None, 5, .02)
        died = time.time()
        fire(self.s.client)
        early_fire = round(time.time() - died, 3)
        time.sleep(.5)
        early_dead = self.living_local("client") is None
        time.sleep(max(0.0, 1.2 - (time.time() - died)))
        fire(self.s.client)
        late_fire = round(time.time() - died, 3)
        respawned = wait_for("client respawned by fire", lambda: self.living_local("client"),
                             6, .1)
        host_view = self.s.other_player(self.s.snap("host"))
        self.out["respawn"] = {"early_fire_s": early_fire, "still_dead_after_early_fire": early_dead,
                               "late_fire_s": late_fire, "respawned_name": respawned["name"],
                               "respawned_health": respawned.get("health"),
                               "host_sees_alive": not host_view.get("dead"),
                               "cite": "exports/script/WOT/WOTPlayer.uc:3607-3640,3738,7126"}
        self.s.note("respawn-gate", **self.out["respawn"])

    def frag_limit(self, limit):
        """Host frags the client with its real Dart until CalcScore reaches the
        limit; the match must end exactly then, on both instances."""
        self.s.shot("rules-before-frags")
        timeline = []
        attempts = 0
        while attempts < limit * 6:
            hs = self.s.snap("host")
            if hs.get("matchended"):
                break
            names = self.m.alive_names()
            self.m.place(names, "host")
            before, _ = self.score(self.s.snap("host"))
            # Leave the client at 1 health (plain damage, no instigator, no
            # death) so a real host Dart is the killing, credited blow.
            health = (logic.pawn(self.s.snap("host"), names["client"]) or {}).get("health", 100)
            if health > 1:
                self.s.host.command("damage", pawn=names["client"], amount=health - 1)
            self.s.host.command("give", pawn=names["host"], **{"class": "WoTAngrealInvDart"},
                                charges=25)
            killed = False
            for _ in range(4):
                attempts += 1
                self.s.host.command("aim", pawn=self.s.other_player(self.s.snap("host"))["name"])
                self.s.host.command("castpress")
                time.sleep(.1)
                self.s.host.command("castrelease")
                try:
                    wait_for("client killed by the host dart", lambda: (
                        logic.pawn(self.s.snap("host"), names["client"]) or {}).get("dead"),
                        1.5, .05)
                    killed = True
                    break
                except RuntimeError:
                    continue
            hs = self.s.snap("host")
            new_score, row = self.score(hs)
            victim = next((p for p in hs.get("players", []) if p["name"] != row["name"]), {})
            ended_host = bool(hs.get("matchended"))
            ended_client = bool(self.s.snap("client").get("matchended"))
            timeline.append({"score_before": before, "score": new_score, "kills": row["kills"],
                             "suicides": row["suicides"], "killed": killed,
                             "victim_deaths": victim.get("deaths"),
                             "ended_host": ended_host, "ended_client": ended_client})
            self.s.note("frag", score=new_score, killed=killed, ended_host=ended_host,
                        ended_client=ended_client)
            if ended_host:
                break
            time.sleep(1.15)
        client_seen = None
        try:
            client_seen = wait_for("client sees match end",
                                   lambda: self.s.snap("client").get("matchended") is True, 10, .1)
        except RuntimeError:
            pass
        self.out["fraglimit"] = {"limit": limit, "timeline": timeline,
                                 "host_ended": bool(self.s.snap("host").get("matchended")),
                                 "client_ended": bool(client_seen),
                                 "cite": "exports/script/WOT/giMPArena.uc:20; giCombatBase.uc:188-196"}
        self.s.shot("rules-fraglimit-ended")

    def restart_fire(self, delay):
        """GameEnded.Fire: refused while frozen (PlayerRestartGameDelay 15 s,
        User.ini:322 / WOTPlayer.uc:7251), then ServerReStartGame ->
        giCombatBase.RestartGame -> NextMap + ServerTravel (bChangeLevels)."""
        if not self.s.snap("host").get("matchended"):
            self.out["restart"] = {"error": "match did not end; restart-fire not exercised"}
            self.s.note("restart-skipped")
            return
        ended_at = time.time()
        world0 = self.s.host.status().get("world")
        fire(self.s.client)
        early = round(time.time() - ended_at, 2)
        time.sleep(4)
        early_world = self.s.host.status().get("world")
        early_ended = bool(self.s.snap("host").get("matchended"))
        time.sleep(max(0.0, delay + 1.5 - (time.time() - ended_at)))
        fire(self.s.client)
        late = round(time.time() - ended_at, 2)
        travelled = {}

        def fresh_match():
            try:
                status = self.s.host.status()
                snap = self.s.snap("host")
            except RuntimeError:
                return None
            if not snap.get("matchended") and status.get("net_mode") == "listen" \
                    and (snap.get("time") or 99) < 30:
                return status
            return None
        try:
            travelled["host"] = wait_for("host restarted the match", fresh_match, 120, 1)
        except RuntimeError as exc:
            travelled["host_error"] = str(exc)

        def client_back():
            try:
                status = self.s.client.status()
                snap = self.s.snap("client")
            except RuntimeError:
                return None
            if status.get("net_mode") == "client" and not snap.get("matchended") \
                    and any(p.get("local") for p in snap.get("pawns", [])):
                return status
            return None
        try:
            travelled["client"] = wait_for("client followed the restart", client_back, 120, 1)
        except RuntimeError as exc:
            travelled["client_error"] = str(exc)
        self.out["restart"] = {"world_before": world0, "early_fire_s": early,
                               "world_after_early_fire": early_world,
                               "still_ended_after_early_fire": early_ended,
                               "late_fire_s": late, "after": travelled,
                               "expected_next_map": self.rules.get("expected_next_map", {}).get(world0),
                               "cite": "exports/script/WOT/WOTPlayer.uc:3837-3860; "
                                       "giCombatBase.uc:207-227; giWOT.uc:57-67"}
        self.s.note("restart", **self.out["restart"])
        try:
            wait_for("both pawns after restart",
                     lambda: len(self.s.players(self.s.snap("host"))) >= 2
                     and len(self.s.players(self.s.snap("client"))) >= 2, 60, 1)
            self.s.shot("rules-after-restart")
        except RuntimeError:
            pass

    def teleport_sync(self):
        """How long an IDLE owning client keeps a stale view of its own pawn
        after the authority moves it (a server teleport, then a SwapPlaces
        swap), and whether its next input (a view change) re-syncs it. The
        matrix saw SwapPlaces move the client pawn on the host but never on
        the client itself (observations: SwapPlaces h2c/c2h)."""
        result = {}

        def client_track(seconds, who_host_name):
            frames = []
            t0 = time.time()
            while time.time() - t0 < seconds:
                hs, cs = self.s.snap("host"), self.s.snap("client")
                h = logic.pawn(hs, who_host_name) or {}
                c = self.living_local("client") or {}
                frames.append({"t": round(time.time() - t0, 2),
                               "host_view": [h.get("x"), h.get("y"), h.get("z")],
                               "client_view": [c.get("x"), c.get("y"), c.get("z")]})
                time.sleep(.25)
            return frames

        def gap(frame):
            hv, cv = frame["host_view"], frame["client_view"]
            if None in hv or None in cv:
                return None
            return round(math.dist(hv, cv), 1)

        # 1. Harness (authority) teleport of the live, idle client pawn.
        names = self.m.alive_names()
        self.m.place(names, "host")
        time.sleep(1.0)
        a = self.m.lane["b"]
        self.s.host.command("teleport", pawn=names["client"], x=a[0], y=a[1] - 200.0, z=a[2])
        idle = client_track(6.0, names["client"])
        self.s.client.command("aim", pawn=self.s.other_player(self.s.snap("client"))["name"])
        after_input = client_track(2.0, names["client"])
        result["server_teleport"] = {
            "idle_gap_first": gap(idle[1]) if len(idle) > 1 else None,
            "idle_gap_last": gap(idle[-1]), "after_input_gap_last": gap(after_input[-1]),
            "idle": idle[::4], "after_input": after_input[::2]}
        # 2. SwapPlaces cast by the host at the idle client.
        names = self.m.cleanup(names)
        self.m.place(names, "host")
        time.sleep(1.0)
        self.s.host.command("give", pawn=names["host"], **{"class": "WoTAngrealInvSwapPlaces"},
                            charges=3)
        self.s.host.command("aim", pawn=self.s.other_player(self.s.snap("host"))["name"])
        self.s.host.command("castpress")
        time.sleep(.2)
        self.s.host.command("castrelease")
        idle = client_track(8.0, names["client"])
        self.s.client.command("aim", pawn=self.s.other_player(self.s.snap("client"))["name"])
        after_input = client_track(2.0, names["client"])
        # Then real movement input (the held-axis verb: AddMovementInput each
        # frame on the client pawn) for 0.4 s.
        self.s.client.command("move", x=1.0, y=0.0)
        time.sleep(.4)
        self.s.client.command("stop")
        after_move = client_track(3.0, names["client"])
        result["swap"] = {"idle_gap_last": gap(idle[-1]),
                          "after_input_gap_last": gap(after_input[-1]),
                          "after_move_gap_last": gap(after_move[-1]),
                          "idle": idle[::4], "after_input": after_input[::2],
                          "after_move": after_move[::2]}
        self.out["teleport_sync"] = result
        self.s.note("teleport-sync", server_teleport=result["server_teleport"]["idle_gap_last"],
                    swap=result["swap"]["idle_gap_last"],
                    swap_after_input=result["swap"]["after_input_gap_last"])
        self.m.cleanup(names)

    def time_limit(self, minutes):
        """giCombatBase: RemainingTime = 60*TimeLimit; Timer ends the match at
        0 ('timelimit') and restarts it once RemainingTime < -GameRestartDelay."""
        world0 = self.s.host.status().get("world")
        route = None
        clock = (self.s.snap("host").get("gamestate") or {}).get("remaining") or 0
        if 0 < clock <= 60 * minutes:
            # Hosted with ?TimeLimit= through the production host request (the
            # pending-build `host` timelimit field): no travel needed.
            route = {"route": "host request TimeLimit", "remaining_at_start": clock}
        if route is None:
            # The shipped binary's `host` verb has no TimeLimit: the same URL
            # option through a same-shell servertravel (InitGame parses it).
            shell = self.s.snap("host").get("map")
            try:
                response = self.s.host.command(
                    "exec", c=f"servertravel /WoTNeutralShells/{shell}?TimeLimit={minutes}")
                route = {"route": "exec servertravel", "shell": shell, "response": response}
            except RuntimeError as exc:
                route = {"route": "exec servertravel", "error": str(exc)}
        self.out["timelimit_route"] = route
        self.s.note("timelimit-route", **route)

        def seeded():
            try:
                g = self.s.snap("host").get("gamestate") or {}
            except RuntimeError:
                return None
            return g if 0 < (g.get("remaining") or 0) <= 60 * minutes else None
        try:
            first = wait_for("TimeLimit clock seeded", seeded, 180, 1)
        except RuntimeError as exc:
            self.out["timelimit"] = {"error": str(exc), "world": world0}
            return
        wait_for("client rejoined", lambda: any(
            p.get("local") for p in self.s.snap("client").get("pawns", [])), 120, 1)
        samples = []
        t0 = time.time()
        ended_at = None
        while time.time() - t0 < 60 * minutes + 40:
            hs, cs = self.s.snap("host"), self.s.snap("client")
            sample = {"t": round(time.time() - t0, 1),
                      "host_remaining": (hs.get("gamestate") or {}).get("remaining"),
                      "client_remaining": (cs.get("gamestate") or {}).get("remaining"),
                      "host_ended": hs.get("matchended"), "client_ended": cs.get("matchended")}
            samples.append(sample)
            if sample["host_ended"] and ended_at is None:
                ended_at = sample["t"]
                self.s.shot("rules-timelimit-ended")
            if ended_at is not None and sample["t"] - ended_at > 25:
                break
            time.sleep(2)
        self.out["timelimit"] = {"minutes": minutes, "first": first, "samples": samples,
                                 "ended_at_s": ended_at, "world_before": world0,
                                 "cite": "exports/script/WOT/giCombatBase.uc:30-35,166-185,229-235"}
        try:
            after = wait_for("auto restart after the delay", lambda: (
                lambda st, sn: st if not sn.get("matchended") and (sn.get("time") or 99) < 30
                else None)(self.s.host.status(), self.s.snap("host")), 90, 1)
            self.out["timelimit"]["after"] = after
        except RuntimeError as exc:
            self.out["timelimit"]["after_error"] = str(exc)
        self.s.note("timelimit", ended_at_s=ended_at, after=self.out["timelimit"].get("after"),
                    error=self.out["timelimit"].get("after_error"))


def run(session, expectations, args, summary):
    phases = [p for p in args.phases.split(",") if p]
    classes = [c for c in args.classes.split(",") if c]
    out = {}
    if "matrix" in phases:
        matrix = Matrix(session, expectations, args)
        out["matrix"] = matrix.run(classes, args.directions)
    if "rules" in phases or "timelimit" in phases or "sync" in phases:
        rules = Rules(session, expectations)
        if "sync" in phases:
            rules.teleport_sync()
        if "rules" in phases:
            rules.respawn_gate()
            rules.frag_limit(expectations.get("rules", {}).get("frag_limit", 20))
            rules.restart_fire(expectations.get("rules", {}).get("restart_delay_s", 15))
        if "timelimit" in phases:
            rules.time_limit(1)
        out["rules"] = rules.out
        (session.evidence / "rules.json").write_text(json.dumps(rules.out, indent=1, default=str),
                                                     encoding="utf-8")
    return out
