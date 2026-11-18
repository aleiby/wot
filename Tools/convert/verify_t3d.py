#!/usr/bin/env python3
"""Verify pure-Python .wot Level T3D exports against the UCC oracle.

Actor semantics are compared through t3d.parse()+typed() as multisets, so text
ordering and irrelevant whitespace are not requirements.  Nested brush blocks
are compared separately through t3d.parse_brushes(), including model name,
polygon metadata, basis vectors, pan, and every vertex.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import sys
import tempfile
from pathlib import Path

import t3d
from ue1pkg import export_level

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import exports_dir, original_dir  # noqa: E402


REPO = Path(__file__).resolve().parents[2]
MP_MAPS = (
    *(f"Arena_{index:02d}" for index in (1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12)),
    *(f"Battle2_{index:02d}" for index in range(1, 6)),
    "Battle3_01", "Battle3_02", "Battle4_01",
)


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _counter(values):
    return collections.Counter(_canonical(value) for value in values)


def _identity_counter(actors):
    return collections.Counter((actor["cls"], actor["name"]) for actor in actors)


def _difference(left, right):
    missing = list((left - right).elements())
    extra = list((right - left).elements())
    return {
        "missing": missing[:3], "missing_count": len(missing),
        "extra": extra[:3], "extra_count": len(extra),
    }


def verify_map(name, maps_root, oracle_root, out_root, system_dir):
    source = maps_root / f"{name}.wot"
    oracle = oracle_root / f"{name}.t3d"
    generated = out_root / f"{name}.t3d"
    if not source.is_file():
        raise ValueError(f"missing map package {source}")
    if not oracle.is_file():
        raise ValueError(f"missing T3D oracle {oracle}")
    export_level(source, generated, system_dir)

    oracle_parsed = t3d.parse(oracle)
    generated_parsed = t3d.parse(generated)
    oracle_typed = t3d.typed(oracle_parsed)
    generated_typed = t3d.typed(generated_parsed)
    oracle_identities = _identity_counter(oracle_typed)
    generated_identities = _identity_counter(generated_typed)
    oracle_semantics = _counter(oracle_typed)
    generated_semantics = _counter(generated_typed)

    oracle_brush_actors = [
        actor for actor in t3d.parse_brushes(oracle) if actor["brushes"]]
    generated_brush_actors = [
        actor for actor in t3d.parse_brushes(generated) if actor["brushes"]]
    oracle_brushes = _counter(oracle_brush_actors)
    generated_brushes = _counter(generated_brush_actors)

    failures = {}
    if oracle_identities != generated_identities:
        failures["actor_multiset"] = _difference(
            oracle_identities, generated_identities)
    if oracle_semantics != generated_semantics:
        failures["actor_semantics"] = _difference(
            oracle_semantics, generated_semantics)
    if oracle_brushes != generated_brushes:
        failures["brush_geometry"] = _difference(
            oracle_brushes, generated_brushes)

    oracle_bytes = oracle.read_bytes()
    generated_bytes = generated.read_bytes()
    brush_count = sum(len(actor["brushes"]) for actor in oracle_brush_actors)
    polygon_count = sum(
        len(brush["polygons"])
        for actor in oracle_brush_actors for brush in actor["brushes"])
    result = {
        "map": name,
        "actors": len(oracle_typed),
        "classes": len({actor["cls"] for actor in oracle_typed}),
        "properties": sum(len(actor["props"]) for actor in oracle_parsed),
        "brushes": brush_count,
        "polygons": polygon_count,
        "byte_exact": oracle_bytes == generated_bytes,
        "oracle_sha256": hashlib.sha256(oracle_bytes).hexdigest(),
        "generated_sha256": hashlib.sha256(generated_bytes).hexdigest(),
        "failures": failures,
    }
    return result


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("maps", nargs="*", help="map stems (default: all 19 MP maps)")
    result.add_argument("--maps-root", type=Path)
    result.add_argument("--system-dir", type=Path)
    result.add_argument("--oracle-root", type=Path)
    result.add_argument(
        "--out-dir", type=Path,
        help="retain generated T3Ds here (default: a temporary directory)")
    result.add_argument("--json", type=Path, help="write the complete report as JSON")
    return result


def main(argv=None):
    arguments = parser().parse_args(argv)
    retail = (original_dir()
              if arguments.maps_root is None or arguments.system_dir is None
              else None)
    arguments.maps_root = arguments.maps_root or retail / "Maps"
    arguments.system_dir = arguments.system_dir or retail / "System"
    arguments.oracle_root = arguments.oracle_root or exports_dir() / "t3d"
    names = tuple(arguments.maps) if arguments.maps else MP_MAPS
    unknown = [name for name in names if name not in MP_MAPS]
    if unknown:
        parser().error(
            f"unknown MP map {unknown[0]!r}; choices: {', '.join(MP_MAPS)}")

    temporary = None
    if arguments.out_dir is None:
        temporary = tempfile.TemporaryDirectory(prefix="wot-t3d-verify-")
        out_root = Path(temporary.name)
    else:
        out_root = arguments.out_dir
        out_root.mkdir(parents=True, exist_ok=True)

    reports = []
    failed = False
    try:
        for name in names:
            try:
                report = verify_map(
                    name, arguments.maps_root, arguments.oracle_root,
                    out_root, arguments.system_dir)
                reports.append(report)
                status = "PASS" if not report["failures"] else "FAIL"
                byte_status = "exact" if report["byte_exact"] else "diff"
                print(
                    f"{name}: {status} actors={report['actors']} "
                    f"classes={report['classes']} props={report['properties']} "
                    f"brushes={report['brushes']} polygons={report['polygons']} "
                    f"bytes={byte_status}")
                if report["failures"]:
                    failed = True
                    print(json.dumps(report["failures"], indent=1))
            except Exception as exc:
                failed = True
                reports.append({"map": name, "error": str(exc)})
                print(f"{name}: ERROR {exc}", file=sys.stderr)
    finally:
        if temporary is not None:
            temporary.cleanup()

    summary = {
        "maps": len(reports),
        "passed": sum(not report.get("failures") and "error" not in report
                      for report in reports),
        "byte_exact": sum(report.get("byte_exact", False) for report in reports),
        "actors": sum(report.get("actors", 0) for report in reports),
        "properties": sum(report.get("properties", 0) for report in reports),
        "brushes": sum(report.get("brushes", 0) for report in reports),
        "polygons": sum(report.get("polygons", 0) for report in reports),
    }
    output = {"summary": summary, "maps": reports}
    if arguments.json:
        arguments.json.parent.mkdir(parents=True, exist_ok=True)
        arguments.json.write_text(json.dumps(output, indent=1) + "\n")
    print(
        f"TOTAL: maps={summary['passed']}/{summary['maps']} "
        f"byte_exact={summary['byte_exact']}/{summary['maps']} "
        f"actors={summary['actors']} properties={summary['properties']} "
        f"brushes={summary['brushes']} "
        f"polygons={summary['polygons']}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
