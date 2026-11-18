#!/usr/bin/env python3
"""Judge an angreal-matrix evidence run offline.

    python verify_angreal_matrix.py --evidence <evidence-root>/<run>

Reads observations.jsonl (raw per-cast records), rules.json and the role
heartbeats; writes verdicts.json and prints the per-class host->client /
client->host table. Exit codes: 0 = complete evidence and every check
passed, 2 = complete evidence with failed game checks (findings), 1 = the
evidence is incomplete or unusable (a harness problem)."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import matrix_logic as logic  # noqa: E402

FATAL_EVENTS = ("initialization-failed", "boot-failure", "world-rejected", "host-failed",
                "host-refused")


def load_jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except ValueError:
            continue
    return rows


def judge_rules(rules: dict, expectations: dict) -> dict:
    out = {}
    spec = expectations.get("rules", {})
    if "respawn" in rules:
        r = rules["respawn"]
        ok = bool(r.get("still_dead_after_early_fire")) and r.get("respawned_health", 0) > 0 \
            and r.get("host_sees_alive") is True
        out["restart_fire_respawn"] = {"verdict": logic.PASS if ok else logic.FAIL, **r,
                                       "expected": f"refused before RespawnDelaySecs "
                                                   f"{spec.get('respawn_delay_s')} s, respawn after"}
    if "fraglimit" in rules:
        f = rules["fraglimit"]
        limit = f.get("limit")
        tl = f.get("timeline", [])
        early_end = [t for t in tl if t.get("ended_host") and t.get("score", 0) < limit]
        final = tl[-1] if tl else {}
        ok = bool(tl) and not early_end and final.get("score") == limit \
            and final.get("ended_host") and f.get("client_ended")
        out["frag_limit"] = {"verdict": logic.PASS if ok else logic.FAIL, "limit": limit,
                             "final": final, "frags": len(tl), "client_ended": f.get("client_ended"),
                             "early_end": early_end, "cite": spec.get("frag_limit_cite")}
    if "restart" in rules:
        r = rules["restart"]
        if "error" in r:
            out["restart_fire_game"] = {"verdict": logic.FAIL, **r}
        else:
            after = r.get("after", {})
            refused = r.get("still_ended_after_early_fire") is True
            restarted = "host" in after
            out["restart_fire_game"] = {
                "verdict": logic.PASS if refused and restarted else logic.FAIL,
                "refused_inside_delay": refused, "restarted_after_delay": restarted,
                "early_fire_s": r.get("early_fire_s"), "late_fire_s": r.get("late_fire_s"),
                "cite": spec.get("restart_delay_cite")}
            expected = r.get("expected_next_map")
            got_host = after.get("host", {}).get("world")
            got_client = after.get("client", {}).get("world")
            out["map_rotation"] = {
                "verdict": logic.PASS if restarted and got_host == expected
                and got_client == expected else logic.FAIL,
                "world_before": r.get("world_before"), "expected": expected,
                "host_after": got_host, "client_after": got_client,
                "cite": spec.get("rotation_cite")}
    if "teleport_sync" in rules:
        sync = rules["teleport_sync"]
        for key in ("server_teleport", "swap"):
            row = sync.get(key) or {}
            idle_gap = row.get("idle_gap_last")
            out[f"owner_sync_{key}"] = {
                "verdict": logic.PASS if idle_gap is not None and idle_gap <= 30 else logic.FAIL,
                "idle_gap_last_uu": idle_gap, "after_input_gap_last_uu": row.get("after_input_gap_last"),
                "after_move_gap_last_uu": row.get("after_move_gap_last"),
                "expected": "the owning client's own pawn follows an authority move while idle "
                            "(UE1 PlayerPawn sends ServerMove every tick and is corrected by "
                            "ClientAdjustPosition; "
                            "SwapPlaces moves both pawns, AngrealSwapPlacesProjectile.uc)"}
    if "timelimit" in rules:
        t = rules["timelimit"]
        if "error" in t:
            out["time_limit"] = {"verdict": logic.FAIL, **t}
        else:
            minutes = t.get("minutes", 1)
            first = (t.get("first") or {}).get("remaining", 0)
            ended = t.get("ended_at_s")
            samples = t.get("samples", [])
            client_ended = any(s.get("client_ended") for s in samples)
            client_counts = any(s.get("client_remaining") is not None
                                and 0 < s["client_remaining"] < 60 * minutes for s in samples)
            ok = 55 * minutes <= first <= 60 * minutes and ended is not None \
                and 60 * minutes - 6 <= ended + (60 * minutes - first) <= 60 * minutes + 6 \
                and client_ended and client_counts
            out["time_limit"] = {"verdict": logic.PASS if ok else logic.FAIL, "minutes": minutes,
                                 "seeded_remaining": first, "ended_at_s": ended,
                                 "client_ended": client_ended, "client_clock": client_counts,
                                 "route": rules.get("timelimit_route"),
                                 "auto_restart": t.get("after"),
                                 "auto_restart_error": t.get("after_error"),
                                 "cite": spec.get("time_limit_cite")}
            expected = spec.get("expected_next_map", {}).get(t.get("world_before"))
            got = (t.get("after") or {}).get("world")
            out["map_rotation_after_timelimit"] = {
                "verdict": logic.PASS if got == expected else logic.FAIL,
                "world_before": t.get("world_before"), "expected": expected, "host_after": got,
                "cite": spec.get("rotation_cite")}
    return out


def verify(evidence: Path, expectations_path: Path) -> tuple[int, dict]:
    expectations = json.loads(expectations_path.read_text(encoding="utf-8"))
    rows = {r["class"]: r for r in expectations["classes"]}
    summary_path = evidence / "summary.json"
    summary = json.loads(summary_path.read_text(encoding="utf-8")) if summary_path.exists() else {}
    records = load_jsonl(evidence / "observations.jsonl")
    problems = []
    if not summary:
        problems.append("summary.json missing")
    casts = []
    for record in records:
        row = rows.get(record["class"])
        if row is None:
            problems.append(f"no expectation row for {record['class']}")
            continue
        casts.append(logic.judge_cast(record, row))
    # Completeness: every planned class x direction of this run has a record.
    planned_classes = summary.get("classes") or [c for c in expectations.get("order", rows)
                                                 if rows[c].get("obtainable", True)]
    context = "battle" if str(summary.get("map", "")).startswith("Battle") else "arena"
    planned_classes = [c for c in planned_classes
                       if rows.get(c, {}).get("context", "arena") == context]
    directions = ["h2c", "c2h"] if summary.get("directions", "both") == "both" \
        else [summary.get("directions")]
    if "matrix" in summary.get("phases", []):
        got = {(c["class"], c["direction"]) for c in casts}
        missing = [(c, d) for c in planned_classes for d in directions if (c, d) not in got]
        if missing:
            problems.append(f"missing casts: {missing}")
    rules_path = evidence / "rules.json"
    rules = judge_rules(json.loads(rules_path.read_text(encoding="utf-8")), expectations) \
        if rules_path.exists() else {}
    health = {}
    for role in ("host", "client"):
        records_hb = load_jsonl(evidence / f"{role}-heartbeat.jsonl")
        fatal = [r for r in records_hb if r.get("event") in FATAL_EVENTS]
        health[role] = {"records": len(records_hb), "fatal": fatal[:5]}
    if summary.get("error"):
        problems.append(f"driver error: {summary['error']}")
    table = {}
    for c in casts:
        table.setdefault(c["class"], {})[c["direction"]] = {
            "verdict": c["verdict"],
            "failed": [k["kind"] for k in c["checks"] if k["verdict"] == logic.FAIL],
            "pending_build": [(k["kind"], k["verdict"]) for k in c["pending_build"]]}
    # The n-th screenshot request of a role is its n-th PNG (ScreenShotNNNNN).
    frames_index = {}
    for role in ("host", "client"):
        pngs = sorted(p.name for p in (evidence / f"{role}-frames").glob("*.png")) \
            if (evidence / f"{role}-frames").exists() else []
        for i, frame in enumerate((summary.get("frames") or {}).get(role, [])):
            frames_index.setdefault(frame.get("label"), {})[role] = \
                f"{role}-frames/{pngs[i]}" if i < len(pngs) else None
    failed_game = [f"{c['class']} {c['direction']}" for c in casts if c["verdict"] == logic.FAIL]
    failed_game += [k for k, v in rules.items() if v["verdict"] == logic.FAIL]
    failed_game += [f"{role} fatal heartbeat" for role, h in health.items() if h["fatal"]]
    result = {"schema": "wot-angreal-verdicts-v1", "evidence": str(evidence),
              "map": summary.get("map"), "table": table, "casts": casts, "rules": rules,
              "process_health": health, "frames": frames_index, "harness_problems": problems,
              "failed_game_checks": failed_game}
    code = 1 if problems else (2 if failed_game else 0)
    return code, result


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--expectations", type=Path, default=HERE / "angreal_expectations.json")
    args = parser.parse_args(argv)
    code, result = verify(args.evidence, args.expectations)
    (args.evidence / "verdicts.json").write_text(json.dumps(result, indent=1, default=str),
                                                 encoding="utf-8")
    for cls, dirs in result["table"].items():
        cells = []
        for direction in ("h2c", "c2h"):
            cell = dirs.get(direction)
            if cell:
                note = f" ({','.join(cell['failed'])})" if cell["failed"] else ""
                cells.append(f"{direction}={cell['verdict']}{note}")
        print(f"{cls:14s} " + "  ".join(cells))
    for key, value in result["rules"].items():
        print(f"rule {key}: {value['verdict']}")
    if result["harness_problems"]:
        print("HARNESS PROBLEMS: " + "; ".join(result["harness_problems"]))
    print(f"verdict: {'complete, all pass' if code == 0 else 'complete, findings' if code == 2 else 'incomplete'}")
    return code


if __name__ == "__main__":
    sys.exit(main())
