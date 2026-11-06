#!/usr/bin/env python3
"""Pure (engine-free) logic of the angreal matrix: snapshot reduction and
per-observable verdicts. The driver records raw observations; the verifier
calls `judge_cast` on each record, so every verdict is reproducible offline
and covered by test_angreal_matrix.py.

Verdict values per observable: "pass", "fail", "pending-build" (the running
binary lacks the snapshot field the observable needs), "n/a" (the observable
is not visible on that instance by design, e.g. another pawn's inventory).
"""
from __future__ import annotations

import math

PASS, FAIL, PENDING, NA = "pass", "fail", "pending-build", "n/a"
PAWN_KEYS = ("name", "cls", "player", "local", "health", "dead", "x", "y", "z",
             "vx", "vy", "vz", "movemode", "mesh", "leeches", "reflectors", "inv",
             "selected")


def lite(snapshot: dict) -> dict:
    """The part of a snapshot the matrix judges (keeps records small)."""
    out = {
        "time": snapshot.get("time"),
        "matchended": snapshot.get("matchended"),
        "pawns": [{k: p[k] for k in PAWN_KEYS if k in p} for p in snapshot.get("pawns", [])],
        "projectiles": [{"cls": p.get("cls"), "name": p.get("name"),
                         "x": p.get("x"), "y": p.get("y"), "z": p.get("z")}
                        for p in snapshot.get("projectiles", [])],
    }
    if "effects" in snapshot:
        out["effects"] = [{"cls": e.get("cls"), "name": e.get("name"),
                           "instigator": e.get("instigator"), "owner": e.get("owner")}
                          for e in snapshot.get("effects", [])]
    return out


def frame_key(frame: dict) -> str:
    """Identity of a reduced frame ignoring the clock (dedupe key)."""
    import json
    return json.dumps({k: v for k, v in frame.items() if k != "time"}, sort_keys=True)


def pawn(frame: dict, name: str) -> dict | None:
    return next((p for p in frame.get("pawns", []) if p.get("name") == name), None)


def cls_match(actual: str | None, wanted: list[str]) -> bool:
    """Exact C++ class or a Blueprint/derived name that starts with it."""
    if not actual:
        return False
    return any(actual == w or actual.startswith(w) for w in wanted)


def item(pawn_row: dict | None, cls: str) -> dict | None:
    if not pawn_row:
        return None
    return next((i for i in pawn_row.get("inv", []) or [] if i.get("cls") == cls), None)


def speed(p: dict) -> float:
    return math.sqrt(sum((p.get(k) or 0.0) ** 2 for k in ("vx", "vy", "vz")))


def dist(a: dict, b: dict) -> float:
    return math.sqrt(sum(((a.get(k) or 0.0) - (b.get(k) or 0.0)) ** 2 for k in ("x", "y", "z")))


def frames(record: dict, role: str) -> list[dict]:
    return [f["lite"] for f in record.get("timeline", []) if f.get("role") == role]


def charge_series(record: dict, role: str, who: str = "caster") -> list[int | None]:
    """Charges of the granted item on `who` as `role` saw it, oldest first
    (None = the item is no longer in the inventory)."""
    name = record["names"][role][who]
    series = []
    for frame in frames(record, role):
        row = pawn(frame, name)
        if row is None or "inv" not in row:
            continue
        entry = item(row, record["cpp_class"])
        series.append(entry.get("charges") if entry else None)
    return series


def charge_delta(record: dict, role: str = "host") -> dict:
    """How many charges the cast consumed as `role` saw it."""
    granted = record.get("granted")
    series = charge_series(record, role)
    seen = [c for c in series if c is not None]
    removed = bool(series) and series[-1] is None and bool(seen or granted)
    if granted is None:
        return {"delta": None, "removed": removed, "series": series}
    low = min(seen) if seen else None
    if removed:
        low = 0
    return {"delta": None if low is None else granted - low, "removed": removed,
            "series": series[-12:]}


def _where(observable: dict) -> list[str]:
    where = observable.get("where", "both")
    return ["host", "client"] if where == "both" else [where]


def _field_missing(record: dict, role: str, field: str) -> bool:
    got = frames(record, role)
    return bool(got) and all(field not in f for f in got)


def judge_observable(record: dict, observable: dict) -> dict:
    """Judge one expectation on every instance it must be seen on."""
    kind = observable["kind"]
    per_role = {}
    for role in _where(observable):
        per_role[role] = _judge_on(record, observable, role)
    verdicts = set(v["verdict"] for v in per_role.values())
    if FAIL in verdicts:
        overall = FAIL
    elif PENDING in verdicts:
        overall = PENDING
    elif verdicts <= {NA}:
        overall = NA
    else:
        overall = PASS
    return {"kind": kind, "verdict": overall, "roles": per_role,
            "cite": observable.get("cite")}


def _judge_on(record: dict, obs: dict, role: str) -> dict:
    kind = obs["kind"]
    names = record["names"][role]
    if obs.get("requires") == "distinct_player_classes":
        pre_host = record["pre"]["host"]
        caster_cls = (pawn(pre_host, record["names"]["host"]["caster"]) or {}).get("cls")
        target_cls = (pawn(pre_host, record["names"]["host"]["target"]) or {}).get("cls")
        if caster_cls == target_cls:
            return {"verdict": PENDING, "why": f"both players are {caster_cls}; needs the "
                                               "`faction` net-test verb (pending build)"}
    pre = record["pre"][role]
    got = frames(record, role) + [record["post"][role]]
    caster, target = names["caster"], names["target"]
    wanted = obs.get("cls", [])

    def verdict(ok, **evidence):
        return {"verdict": PASS if ok else FAIL, **evidence}

    if kind == "projectile":
        seen = sorted({p["cls"] for f in got for p in f.get("projectiles", [])
                       if cls_match(p.get("cls"), wanted)})
        return verdict(bool(seen), seen=seen)
    if kind == "effect_actor":
        if _field_missing(record, role, "effects"):
            return {"verdict": PENDING, "why": "snapshot has no `effects` field"}
        before = {e["name"] for e in pre.get("effects", [])}
        seen = sorted({e["cls"] for f in got for e in f.get("effects", [])
                       if e["name"] not in before and cls_match(e.get("cls"), wanted)})
        return verdict(bool(seen), seen=seen)
    if kind in ("target_health_drop", "target_dead", "caster_health_rise",
                "caster_health_drop"):
        who = target if kind.startswith("target") else caster
        start = (pawn(pre, who) or {}).get("health")
        rows = [pawn(f, who) for f in got]
        rows = [r for r in rows if r]
        healths = [r.get("health") for r in rows if r.get("health") is not None]
        died = any(r.get("dead") for r in rows)
        if start is None or not healths:
            return {"verdict": FAIL, "why": f"{who} not visible", "start": start}
        if kind == "target_dead":
            return verdict(died, start=start, low=min(healths))
        if kind.endswith("drop"):
            low = min(healths)
            return verdict(died or start - low >= obs.get("min", 1), start=start, low=low,
                           died=died)
        high = max(healths)
        return verdict(high - start >= obs.get("min", 1), start=start, high=high)
    if kind in ("caster_reflector", "target_reflector", "caster_leech", "target_leech"):
        who = caster if kind.startswith("caster") else target
        field = "reflectors" if kind.endswith("reflector") else "leeches"
        seen = sorted({c for f in got for c in ((pawn(f, who) or {}).get(field) or [])
                       if cls_match(c, wanted)})
        return verdict(bool(seen), seen=seen)
    if kind in ("caster_reflector_absent", "target_reflector_absent",
                "caster_leech_absent", "target_leech_absent"):
        who = caster if kind.startswith("caster") else target
        field = "reflectors" if "reflector" in kind else "leeches"
        seen = sorted({c for f in got for c in ((pawn(f, who) or {}).get(field) or [])
                       if cls_match(c, wanted)})
        return verdict(not seen, seen=seen)
    if kind in ("caster_leech_removed", "target_leech_removed"):
        who = caster if kind.startswith("caster") else target
        before = [c for c in (pawn(pre, who) or {}).get("leeches", []) or []
                  if cls_match(c, wanted)]
        if not before:
            return {"verdict": FAIL, "why": "precondition leech absent before the cast"}
        after = [c for c in ((pawn(record["post"][role], who) or {}).get("leeches") or [])
                 if cls_match(c, wanted)]
        return verdict(len(after) < len(before), before=before, after=after)
    if kind in ("caster_moved", "target_moved"):
        who = caster if kind.startswith("caster") else target
        start = pawn(pre, who)
        rows = [pawn(f, who) for f in got]
        moved = max((dist(start, r) for r in rows if r), default=0.0) if start else 0.0
        return verdict(moved >= obs.get("min", 100), moved=round(moved, 1))
    if kind == "positions_swapped":
        c0, t0 = pawn(pre, caster), pawn(pre, target)
        tol = obs.get("tol", 120)
        hit = False
        for f in got:
            c1, t1 = pawn(f, caster), pawn(f, target)
            if c0 and t0 and c1 and t1 and dist(c1, t0) <= tol and dist(t1, c0) <= tol:
                hit = True
                break
        return verdict(hit)
    if kind in ("caster_movemode", "target_movemode"):
        who = caster if kind.startswith("caster") else target
        modes = sorted({(pawn(f, who) or {}).get("movemode") for f in got} - {None})
        return verdict(obs.get("mode") in modes, modes=modes)
    if kind in ("caster_mesh_changed", "caster_mesh"):
        start = (pawn(pre, caster) or {}).get("mesh")
        meshes = sorted({(pawn(f, caster) or {}).get("mesh") for f in got} - {None})
        if kind == "caster_mesh":
            return verdict(any(obs.get("contains", "") in m for m in meshes), meshes=meshes)
        return verdict(any(m != start for m in meshes), start=start, meshes=meshes)
    if kind == "pawn_spawned":
        before = {p["name"] for p in pre.get("pawns", [])}
        seen = sorted({p["cls"] for f in got for p in f.get("pawns", [])
                       if p["name"] not in before and not p.get("player")
                       and any(w.lower() in (p.get("cls") or "").lower() for w in wanted)})
        return verdict(bool(seen), seen=seen)
    if kind == "target_speed":
        rows = [pawn(f, target) for f in got]
        top = max((speed(r) for r in rows if r), default=0.0)
        return verdict(top >= obs.get("min", 100), top=round(top, 1))
    if kind in ("caster_inventory_gain", "target_inventory_loss"):
        who = caster if kind.startswith("caster") else target
        start_row = pawn(pre, who)
        if start_row is None or "inv" not in start_row:
            return {"verdict": NA, "why": "inventory not replicated to this instance"}
        before = {i["cls"] for i in start_row.get("inv", [])}
        after_row = pawn(record["post"][role], who) or {}
        after = {i["cls"] for i in after_row.get("inv", []) or []}
        if kind == "caster_inventory_gain":
            return verdict(any(cls_match(c, wanted) for c in after - before))
        return verdict(any(cls_match(c, wanted) for c in before - after))
    if kind == "item_tainted":
        row = pawn(record["post"][role], target)
        if row is None or "inv" not in row:
            return {"verdict": NA, "why": "inventory not replicated to this instance"}
        rows = [pawn(f, target) for f in got]
        items = [i for r in rows if r for i in (r.get("inv") or [])]
        if items and all("tainted" not in i for i in items):
            return {"verdict": PENDING, "why": "snapshot inventory has no `tainted` field"}
        return verdict(any(i.get("tainted") for i in items))
    return {"verdict": FAIL, "why": f"unknown observable kind {kind}"}


def judge_charges(record: dict, expectation: dict) -> dict:
    """Charges decrement by the shipped ChargeCost (server view; the owning
    client's replicated view must agree when the client is the caster)."""
    bounds = expectation.get("expected_charge_delta") or {}
    lo, hi = bounds.get("min", 1), bounds.get("max", bounds.get("min", 1))
    roles = ["host"] + (["client"] if record["caster_role"] == "client" else [])
    per_role = {}
    for role in roles:
        got = charge_delta(record, role)
        ok = got["delta"] is not None and lo <= got["delta"] <= hi
        per_role[role] = {"verdict": PASS if ok else FAIL, **got, "expected": [lo, hi]}
    overall = PASS if all(v["verdict"] == PASS for v in per_role.values()) else FAIL
    return {"kind": "charges", "verdict": overall, "roles": per_role,
            "cite": expectation.get("charge_cite")}


def judge_cast(record: dict, expectation: dict) -> dict:
    """Every verdict for one (class, direction) cast."""
    checks = []
    if record.get("errors"):
        checks.append({"kind": "no-errors", "verdict": FAIL, "errors": record["errors"]})
    else:
        checks.append({"kind": "no-errors", "verdict": PASS})
    if not record.get("grant_ok"):
        checks.append({"kind": "grant", "verdict": FAIL,
                       "why": record.get("grant_detail", "grant/select failed")})
    checks.append(judge_charges(record, expectation))
    for observable in expectation.get("observables", []):
        checks.append(judge_observable(record, observable))
    pending = [judge_observable(record, o) for o in
               expectation.get("pending_build_observables", [])]
    verdicts = [c["verdict"] for c in checks]
    overall = FAIL if FAIL in verdicts else PASS
    return {"class": record["class"], "direction": record["direction"], "verdict": overall,
            "checks": checks, "pending_build": pending}
