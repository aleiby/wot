#!/usr/bin/env python3
"""Pure evidence logic for the 19-map guarded host + join sweep.

Never launches Unreal. The PowerShell launcher (run_map_sweep.ps1) owns the
processes; map_sweep_driver.py owns the channel conversation; this module owns
every judgement over the collected evidence so it is unit-testable offline:

* heartbeat parsing (space-separated key=value details, %20-escaped values)
  and per-process slicing (every record carries the writer's pid, so one
  role profile reused across maps still yields an exact per-map record);
* per-role checks: Ready reached, world-projected for THIS map with the
  retail title, no fatal event, physics bodies created, zero failed
  textures/materials after the map world was projected, unresolved BSP slots
  within the pinned per-map allowlist (sweep_expectations.json);
* per-map driver summary checks: both roles in the map world, the HUD
  scoreboard/overview header shows the retail Level.Title (never the
  WoTMapShellNN package), and on every Arena map a projected pickup entered
  the client pawn's inventory on the server AND on the client;
* join mode: a `session` run must show the multiplayer browser listing this
  map's host and joining that row, and no role may record a `net-failure`
  (the permanent engine network/travel failure heartbeat) in any mode.

CLI:
  sweep_checks.py slice  --heartbeat F --pid N --out F
  sweep_checks.py verify --evidence RUN [--maps M ...] [--strict-skips]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterable

HERE = Path(__file__).resolve().parent
EXPECTATIONS = HERE / "sweep_expectations.json"

ARENA_MAPS = tuple(f"Arena_{index:02d}" for index in (1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12))
BATTLE_MAPS = ("Battle2_01", "Battle2_02", "Battle2_03", "Battle2_04",
               "Battle2_05", "Battle3_01", "Battle3_02", "Battle4_01")
MP_MAPS = ARENA_MAPS + BATTLE_MAPS

# A record with one of these events means the process could not serve the map.
FATAL_EVENTS = ("initialization-failed", "boot-failure", "world-rejected",
                "host-failed", "host-refused", "net-failure")
JOIN_MODES = ("direct", "session")
REQUIRED_MAP_FILES = ("summary.json", "transcript.json", "host-heartbeat.jsonl",
                      "client-heartbeat.jsonl", "host-frame.png", "client-frame.png")


class SweepError(RuntimeError):
    """Malformed evidence or expectations (as opposed to a failed check)."""


def load_expectations(path: Path | None = None) -> dict:
    data = json.loads((path or EXPECTATIONS).read_text(encoding="utf-8"))
    if data.get("format") != "wot-map-sweep-expectations-v1":
        raise SweepError("sweep expectations have an unknown format")
    maps = data.get("maps")
    if not isinstance(maps, dict) or tuple(maps) != MP_MAPS:
        raise SweepError("sweep expectations must pin exactly the 19 MP maps, in order")
    for name, row in maps.items():
        if row.get("mode") != ("arena" if name in ARENA_MAPS else "battle"):
            raise SweepError(f"{name}: wrong mode")
        if not isinstance(row.get("title"), str) or not row["title"]:
            raise SweepError(f"{name}: missing retail title")
        if not isinstance(row.get("unresolved_slots"), list):
            raise SweepError(f"{name}: missing unresolved-slot allowlist")
        if name in ARENA_MAPS and row.get("pickup") is not True:
            raise SweepError(f"{name}: every Arena map requires the pickup check")
    return data


def parse_detail(detail: str) -> dict[str, str]:
    """`key=value key=value` (values never contain spaces; spaces are %20)."""
    fields: dict[str, str] = {}
    for token in (detail or "").split(" "):
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value.replace("%20", " ")
    return fields


def read_heartbeat(path: Path) -> list[dict]:
    records = []
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except ValueError:
            continue  # a torn final line from a killed writer
        if isinstance(record, dict):
            records.append(record)
    return records


def slice_by_pid(records: Iterable[dict], pid: int) -> list[dict]:
    return [record for record in records if record.get("pid") == pid]


def _int(value: Any, default: int = -1) -> int:
    try:
        return int(str(value))
    except (TypeError, ValueError):
        return default


def _rule_counts(value: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    if not value or value == "-":
        return counts
    for item in value.split(","):
        name, _, count = item.partition(":")
        if name:
            counts[name] = _int(count, 0)
    return counts


def evaluate_role(map_name: str, role: str, records: list[dict],
                  expectations: dict, strict_skips: bool = False) -> dict:
    """Judge one process's heartbeat slice for one map."""
    row = expectations["maps"][map_name]
    failures: list[str] = []
    info: dict[str, Any] = {}
    if not records:
        return {"ok": False, "failures": [f"{role}: no heartbeat records"], "info": info}
    if not any(record.get("phase") == "Ready" for record in records):
        failures.append(f"{role}: heartbeat never reached Ready")
    for record in records:
        if record.get("event") in FATAL_EVENTS:
            failures.append(f"{role}: fatal {record.get('event')}: {record.get('detail')}")

    projected_index = None
    projected: dict[str, str] = {}
    for index, record in enumerate(records):
        if record.get("event") != "world-projected":
            continue
        fields = parse_detail(record.get("detail", ""))
        if fields.get("map") == map_name:
            projected_index, projected = index, fields
    if projected_index is None:
        failures.append(f"{role}: never projected {map_name}")
    else:
        title = projected.get("title", "")
        info["title"] = title
        if title != row["title"]:
            failures.append(f"{role}: projected title {title!r} != retail {row['title']!r}")
        allowed = list(row["unresolved_slots"])
        count = _int(projected.get("unresolved_slots"))
        names = [name for name in projected.get("unresolved_slot_names", "-").split(",")
                 if name and name != "-"]
        info.update(unresolved_slots=count, unresolved_slot_names=names,
                    default_texture_slots=_int(projected.get("default_texture_slots")),
                    sections=_int(projected.get("sections")),
                    sky_error=projected.get("sky_error", "-"),
                    projection_rules=_rule_counts(projected.get("projection_rules", "")),
                    skipped=_rule_counts(projected.get("skipped", "")))
        if count < 0:
            failures.append(f"{role}: world-projected has no unresolved_slots count")
        elif count > len(allowed):
            failures.append(f"{role}: {count} unresolved slots exceed the pinned "
                            f"allowlist {allowed}: {names}")
        stray = [name for name in names if name not in allowed]
        if stray:
            failures.append(f"{role}: unresolved slots outside the allowlist: {stray}")
        allowed_skips = row.get("skipped_rules", {})
        unexpected_skips = {rule: skipped for rule, skipped in info["skipped"].items()
                            if skipped > allowed_skips.get(rule, 0)}
        info["unexpected_skips"] = unexpected_skips
        if strict_skips and unexpected_skips:
            failures.append(f"{role}: projection skips beyond the allowlist: {unexpected_skips}")

    if not any(record.get("event") == "world-physics-completed"
               and _int(parse_detail(record.get("detail", "")).get("created"), 0) > 0
               for record in records):
        failures.append(f"{role}: no world-physics-completed record with created>0")

    # Asset telemetry of the MAP world only: summaries emitted after this
    # map's projection (the front-end world has its own earlier summaries).
    after = records[projected_index + 1:] if projected_index is not None else []
    summaries = [parse_detail(record.get("detail", "")) for record in after
                 if record.get("event") == "assets-resolved"]
    if projected_index is not None and not summaries:
        failures.append(f"{role}: no assets-resolved summary for the map world")
    for fields in summaries:
        stage = fields.get("stage", "?")
        for key in ("failed_textures", "failed_materials"):
            value = _int(fields.get(key), -1)
            if key not in fields or value < 0:
                failures.append(f"{role}: assets-resolved stage={stage} has no "
                                f"parseable {key} ({fields.get(key)!r})")
            elif value != 0:
                failures.append(f"{role}: assets-resolved stage={stage} {key}="
                                f"{fields.get(key)} ({fields.get('failures', '-')})")
        if _int(fields.get("failed_sounds"), 0) != 0:
            info.setdefault("warnings", []).append(
                f"stage={stage} failed_sounds={fields.get('failed_sounds')}")
    benign = set(expectations.get("benign_unbound_textures", ()))
    unbound = sorted({record.get("detail", "") for record in records
                      if record.get("event") == "texture-unbound"} - benign)
    if unbound:
        info.setdefault("warnings", []).append(f"texture-unbound {unbound}")
    return {"ok": not failures, "failures": failures, "info": info}


def inventory_gained(before: list[dict] | None, after: list[dict] | None,
                     cls: str) -> bool:
    """A pickup of `cls` entered the chain: a new item, or more charges on the
    existing one (retail PickupQuery lets a held angreal absorb the copy)."""
    held_before = [item for item in before or () if item.get("cls") == cls]
    held_after = [item for item in after or () if item.get("cls") == cls]
    if len(held_after) > len(held_before):
        return True
    if held_before and held_after:
        return _int(held_after[0].get("charges"), 0) > _int(held_before[0].get("charges"), 0)
    return False


def evaluate_summary(map_name: str, summary: dict, expectations: dict,
                     join_mode: str | None = None) -> list[str]:
    row = expectations["maps"][map_name]
    failures: list[str] = []
    if summary.get("map") != map_name:
        failures.append(f"summary names {summary.get('map')!r}, not {map_name}")
    if summary.get("status") != "passed":
        failures.append(f"driver status {summary.get('status')!r}: {summary.get('error', '-')}")
    # Summaries written before the session mode existed joined directly.
    used = summary.get("join_mode", "direct")
    if used not in JOIN_MODES:
        failures.append(f"unknown join mode {used!r}")
    if join_mode is not None and used != join_mode:
        failures.append(f"joined by {used!r}, not the required {join_mode!r}")
    events = [event.get("event") for event in summary.get("events", ())]
    required_phases = ["front-ends-ready", "host-listening", "client-joined", "titles-checked"]
    if used == "session":
        required_phases[2:2] = ["browser-listed", "browser-joined"]
        listed = summary.get("browser_row") or {}
        if listed.get("map") != map_name:
            failures.append(f"browser row {listed!r} does not advertise {map_name}")
    for required in required_phases:
        if required not in events:
            failures.append(f"summary lacks the {required} phase")
    positions = [events.index(phase) for phase in required_phases if phase in events]
    if positions != sorted(positions):
        failures.append(f"summary phases out of order: {events}")
    for role in ("host", "client"):
        view = (summary.get("views") or {}).get(role) or {}
        if view.get("map_identity") != map_name:
            failures.append(f"{role} map identity {view.get('map_identity')!r}")
        for key in ("map_title", "scoreboard_map"):
            if view.get(key) != row["title"]:
                failures.append(f"{role} {key} {view.get(key)!r} != {row['title']!r}")
        shown = " ".join(str(view.get(key, "")) for key in
                         ("map_title", "scoreboard_map", "overview_map"))
        if "WoTMapShell" in shown:
            failures.append(f"{role} UI shows the neutral shell name: {shown!r}")
        # Guarded cooks clear DirectoriesToAlwaysCook (build_stripped_pak.py
        # _configure_guarded_cook); a hand set mapping no class means the
        # angreal hand table never loaded and the hands HUD/keys are dead.
        # hand_plan_classes = planned cells of the local pawn's angreal hand
        # set (WOTPlayer.AngrealHandOrder: 42). hand_classes (held cells) is
        # informational: a Battle player holds no angreal at the start.
        expected_plan = expectations.get("hand_plan_classes", 42)
        if "hand_plan_classes" not in view:
            failures.append(f"{role} snapshot reports no hand plan for its local pawn")
        elif _int(view.get("hand_plan_classes"), -1) != expected_plan:
            failures.append(f"{role} angreal hand plan has "
                            f"{view.get('hand_plan_classes')!r} cells, not {expected_plan}")
    if row.get("pickup"):
        pickup = summary.get("pickup") or {}
        cls = pickup.get("cls", "")
        if not cls:
            failures.append("no projected pickup was taken")
        else:
            if not inventory_gained(pickup.get("server_before"), pickup.get("server_after"), cls):
                failures.append(f"server inventory never gained {cls}")
            if not inventory_gained(pickup.get("client_before"), pickup.get("client_after"), cls):
                failures.append(f"client inventory never gained {cls}")
    return failures


def evaluate_run(run: Path, maps: Iterable[str], expectations: dict,
                 strict_skips: bool = False, join_mode: str | None = None) -> dict:
    report: dict[str, Any] = {"format": "wot-map-sweep-report-v1", "maps": {}}
    ok = True
    for map_name in maps:
        if map_name not in expectations["maps"]:
            raise SweepError(f"{map_name} is not a pinned MP map")
        directory = run / map_name
        failures: list[str] = []
        entry: dict[str, Any] = {}
        for name in REQUIRED_MAP_FILES:
            path = directory / name
            if not path.is_file():
                failures.append(f"missing {name}")
            elif path.stat().st_size == 0:
                failures.append(f"empty {name}")
        summary_path = directory / "summary.json"
        if summary_path.is_file():
            summary = json.loads(summary_path.read_text(encoding="utf-8-sig"))
            failures += evaluate_summary(map_name, summary, expectations, join_mode)
            entry["pickup"] = summary.get("pickup")
            entry["join_mode"] = summary.get("join_mode", "direct")
            if summary.get("browser_row") is not None:
                entry["browser_row"] = summary.get("browser_row")
        for role in ("host", "client"):
            beat = directory / f"{role}-heartbeat.jsonl"
            if beat.is_file() and beat.stat().st_size:
                result = evaluate_role(map_name, role, read_heartbeat(beat),
                                       expectations, strict_skips)
                failures += result["failures"]
                entry[role] = result["info"]
        entry["failures"] = failures
        entry["ok"] = not failures
        ok = ok and entry["ok"]
        report["maps"][map_name] = entry
    report["ok"] = ok
    return report


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="map sweep evidence checks")
    sub = parser.add_subparsers(dest="command", required=True)
    cut = sub.add_parser("slice")
    cut.add_argument("--heartbeat", required=True, type=Path)
    cut.add_argument("--pid", required=True, type=int)
    cut.add_argument("--out", required=True, type=Path)
    check = sub.add_parser("verify")
    check.add_argument("--evidence", required=True, type=Path)
    check.add_argument("--maps", nargs="*")
    check.add_argument("--expectations", type=Path)
    check.add_argument("--strict-skips", action="store_true")
    check.add_argument("--join-mode", choices=JOIN_MODES)
    args = parser.parse_args(argv)
    if args.command == "slice":
        records = slice_by_pid(read_heartbeat(args.heartbeat), args.pid)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text("".join(json.dumps(record, sort_keys=True) + "\n"
                                    for record in records), encoding="utf-8")
        print(f"{len(records)} records for pid {args.pid}")
        return 0 if records else 1
    expectations = load_expectations(args.expectations)
    maps = args.maps or [path.name for path in sorted(args.evidence.iterdir())
                         if path.is_dir() and path.name in MP_MAPS]
    if not maps:
        raise SweepError("the evidence root holds no map directories")
    report = evaluate_run(args.evidence, maps, expectations, args.strict_skips,
                          args.join_mode)
    (args.evidence / "sweep-report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    for map_name, entry in report["maps"].items():
        print(f"{map_name}: {'ok' if entry['ok'] else 'FAILED'}")
        for failure in entry["failures"]:
            # stdout, not stderr: Windows PowerShell 5.1 turns a redirected
            # native stderr line into a terminating NativeCommandError.
            print(f"  - {map_name}: {failure}")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
