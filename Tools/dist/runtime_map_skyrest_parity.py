#!/usr/bin/env python3
"""Sky-rest parity oracle: native census vs the development bake.

The native cache builder ports Tools/convert/skybake.py bake_rest exactly and
carries its census in the version-6 BSP sky-rest trailer (flag 32). The
real-retail native spec WoT.RetailReader.BSPWorld.RealRetailAtomicSetAndPins
writes each map's trailer bytes as <evidence>/<Map>.skyrest; this oracle
decodes them with the canonical Python parser and compares, map by map, with
the development pipeline's own <conversion>/<Map>_skyrest.json:

* the winning sky zone;
* the covered fraction (the JSON rounds it to 6 decimals; SEALED maps write
  1.0 with no lists and must be covered on every ray);
* the per-surface visible / blocker / FakeBackdrop ray counts, exactly.

A map without a sky zone ({"reason": ...}) must have no trailer.

  python Tools/dist/runtime_map_skyrest_parity.py --native-evidence DIR \\
      --conversion-root Intermediate/Conv [--report OUT.json]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools" / "convert"))
sys.path.insert(0, str(REPO / "Tools" / "import"))
from bsp_payload import BSPPayloadError, _validate_sky_rest_trailer  # noqa: E402


def _dev_surfaces(document: dict) -> dict[int, tuple[int, int, int]]:
    rows: dict[int, list[int]] = {}
    for column, key in enumerate(("visible", "blockers", "fb")):
        for surface, count in (document.get(key) or {}).items():
            rows.setdefault(int(surface), [0, 0, 0])[column] = int(count)
    return {surface: tuple(values) for surface, values in rows.items()}


def compare_map(name: str, native: bytes | None, development: dict) -> dict:
    row: dict = {"map": name, "status": "exact", "problems": []}
    if "reason" in development:
        if native is not None:
            row["problems"].append("a sky-less map carries a sky-rest trailer")
        row["status"] = "no-sky" if not row["problems"] else "mismatch"
        return row
    if native is None:
        row["problems"].append("the native trailer is missing")
        row["status"] = "mismatch"
        return row
    try:
        census = _validate_sky_rest_trailer(native)
    except BSPPayloadError as exc:
        row["problems"].append(f"the native trailer does not parse: {exc}")
        row["status"] = "mismatch"
        return row
    covered = census["covered_rays"] / census["ray_count"]
    row.update(sky_zone=census["sky_zone"], covered_rays=census["covered_rays"],
               ray_count=census["ray_count"], covered_fraction=round(covered, 6),
               surfaces=len(census["surfaces"]),
               groups=[(group["role"], group["slot"], len(group["triangles"]))
                       for group in census["groups"]])
    if census["sky_zone"] != int(development.get("sky_zone", -1)):
        row["problems"].append(
            f"sky zone {census['sky_zone']} != {development.get('sky_zone')}")
    sealed = "visible" not in development
    if sealed and census["covered_rays"] != census["ray_count"]:
        row["problems"].append("the development bake is SEALED but rays are uncovered")
    if abs(covered - float(development.get("covered_frac", -1))) > 5e-7:
        row["problems"].append(
            f"covered fraction {covered:.6f} != {development.get('covered_frac')}")
    native_rows = {item["surface"]: (item["visible"], item["blocker"], item["fakebackdrop"])
                   for item in census["surfaces"]}
    if native_rows != _dev_surfaces(development):
        row["problems"].append("per-surface ray counts differ from skyrest.json")
    if row["problems"]:
        row["status"] = "mismatch"
    return row


def run(native_evidence: Path, conversion_root: Path, maps: list[str]) -> dict:
    rows = []
    for name in maps:
        dev_path = conversion_root / f"{name}_skyrest.json"
        development = json.loads(dev_path.read_text(encoding="utf-8"))
        native_path = native_evidence / f"{name}.skyrest"
        native = native_path.read_bytes() if native_path.is_file() else None
        rows.append(compare_map(name, native, development))
    return {"format": "wot-skyrest-parity-v1", "maps": rows,
            "ok": all(row["status"] != "mismatch" for row in rows)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-evidence", type=Path, required=True)
    parser.add_argument("--conversion-root", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    from import_all import MP_MAPS  # pylint: disable=import-outside-toplevel
    report = run(args.native_evidence, args.conversion_root, list(MP_MAPS))
    text = json.dumps(report, indent=1, sort_keys=True)
    if args.report:
        args.report.write_text(text + "\n", encoding="utf-8")
    for row in report["maps"]:
        detail = "; ".join(row["problems"]) if row["problems"] else ""
        print(f"{row['map']}: {row['status']} {detail}".rstrip())
    print("SKYREST PARITY", "OK" if report["ok"] else "FAILED")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
