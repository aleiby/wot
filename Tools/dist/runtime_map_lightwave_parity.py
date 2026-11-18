#!/usr/bin/env python3
"""Exact oracle for the version-5 animated light-map responses.

Compares the native BSP animation trailer written by
``WoT.RetailReader.BSPWorld.RealRetailAtomicSetAndPins``
(``<native>/<Map>.anim``) against the
developer pipeline's ``<Map>_lmatlas.json`` manifest and the
``<Map>_lmatlas_response_<slot>.png`` images produced by
``Tools/convert/lightmap_atlas.py``:

* the waveform group table and the term slot order must match the manifest;
* every native rect must equal the PNG interior of that surface exactly;
* every surface the native trailer omits must be all-zero in the PNG;
* the PNG gutters must be the edge replication of the interior.

Usage: python runtime_map_lightwave_parity.py --native <dir> --conversion <dir>
       [--maps Arena_12 ...] [--report <json>]
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONVERT = REPO / "Tools" / "convert"
IMPORT = REPO / "Tools" / "import"
for directory in (CONVERT, IMPORT):
    if str(directory) not in sys.path:
        sys.path.insert(0, str(directory))

from bsp_payload import _validate_animation_trailer  # noqa: E402
from import_all import MP_MAPS  # noqa: E402

LIGHT_TYPES = {"LT_Pulse": 2, "LT_Blink": 3, "LT_SubtlePulse": 7}
EFFECTS = {"LE_TorchWaver": 1, "LE_FireWaver": 2, "LE_WateryShimmer": 3}
KINDS = {"waveform": 0, "effect": 1, "intersection": 2}


class ParityError(ValueError):
    pass


def manifest_rows(animation: dict) -> tuple[list[tuple], list[tuple]]:
    groups = [(LIGHT_TYPES[g["key"][0]], int(g["key"][1]), int(g["key"][2]),
               int(g["light_count"])) for g in animation["groups"]]
    keys = [tuple(g["key"]) for g in animation["groups"]]
    terms = []
    for term in animation["terms"]:
        group = keys.index(tuple(term["group"])) if "group" in term else None
        terms.append((KINDS[term["kind"]], group,
                      EFFECTS.get(term.get("effect"), 0), int(term["light_count"]),
                      term["response_png"]))
    return groups, terms


def compare_map(map_name: str, native_dir: Path, conversion: Path) -> dict:
    from PIL import Image  # local import: the parser stays stdlib-only

    document = json.loads((conversion / f"{map_name}_lmatlas.json").read_text())
    width, height = document["size"]
    groups, terms = manifest_rows(document["animation"])
    native_path = native_dir / f"{map_name}.anim"
    result = {"map": map_name, "groups": len(groups), "terms": len(terms)}
    if not terms:
        if native_path.exists():
            raise ParityError(f"{map_name}: native trailer present for an unanimated map")
        result["status"] = "unanimated"
        return result
    if not native_path.exists():
        raise ParityError(f"{map_name}: native trailer is missing")
    native = _validate_animation_trailer(native_path.read_bytes(), width, height)
    native_groups = [(g["key"][0], g["key"][1], g["key"][2], g["light_count"])
                     for g in native["groups"]]
    if native_groups != groups:
        raise ParityError(f"{map_name}: group table differs {native_groups} != {groups}")
    native_terms = [(t["kind"], t["group"], t["effect"], t["light_count"])
                    for t in native["terms"]]
    if native_terms != [t[:4] for t in terms]:
        raise ParityError(f"{map_name}: term table differs {native_terms} != {terms}")
    surfaces = document["surfs"]
    placements = {int(key): (rect["x"], rect["y"], rect["w"], rect["h"])
                  for key, rect in surfaces.items()}
    native_surfaces = native["surfaces"]
    for index, (x, y, w, h) in placements.items():
        if native_surfaces[index] != (x, y, w, h):
            raise ParityError(
                f"{map_name}: surface {index} placement {native_surfaces[index]} != {(x, y, w, h)}")
    checked_rects = 0
    checked_texels = 0
    zero_surfaces = 0
    for slot, (term, native_term) in enumerate(zip(terms, native["terms"])):
        image = Image.open(conversion / term[4]).convert("RGB")
        if image.size != (width, height):
            raise ParityError(f"{map_name}: response {slot} size {image.size}")
        pixels = image.tobytes()
        rects = dict(native_term["rects"])
        for index, (x, y, w, h) in placements.items():
            expected = b"".join(
                pixels[((y + row) * width + x) * 3:((y + row) * width + x + w) * 3]
                for row in range(h))
            actual = rects.get(index)
            if actual is None:
                if any(expected):
                    raise ParityError(
                        f"{map_name}: response {slot} surface {index} is non-zero in the "
                        "reference but absent natively")
                zero_surfaces += 1
                continue
            if actual != expected:
                first = next(i for i in range(len(expected)) if expected[i] != actual[i])
                raise ParityError(
                    f"{map_name}: response {slot} surface {index} differs at byte {first}: "
                    f"native {actual[first]} reference {expected[first]}")
            # Edge-replicated gutter (lightmap_atlas.py np.pad mode="edge").
            for row in range(-1, h + 1):
                source_row = min(max(row, 0), h - 1)
                for column in (-1, w):
                    source_column = min(max(column, 0), w - 1)
                    at = ((y + row) * width + x + column) * 3
                    src = ((y + source_row) * width + x + source_column) * 3
                    if pixels[at:at + 3] != pixels[src:src + 3]:
                        raise ParityError(
                            f"{map_name}: response {slot} surface {index} gutter differs")
            checked_rects += 1
            checked_texels += w * h
        if set(rects) - set(placements):
            raise ParityError(f"{map_name}: response {slot} names an unplaced surface")
    result.update(status="exact", rects=checked_rects, texels=checked_texels,
                  zero_surfaces=zero_surfaces)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--native", type=Path,
                        default=REPO / "Saved/ReleaseEvidence/mp-lightmaps/native")
    parser.add_argument("--conversion", type=Path,
                        default=REPO / "Intermediate/Conv")
    parser.add_argument("--maps", nargs="*", default=list(MP_MAPS))
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    rows = []
    failures = 0
    for map_name in args.maps:
        try:
            row = compare_map(map_name, args.native, args.conversion)
        except (ParityError, OSError, ValueError) as exc:
            row = {"map": map_name, "status": "FAIL", "error": str(exc)}
            failures += 1
        rows.append(row)
        print(json.dumps(row, sort_keys=True))
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(
            {"format": "wot-lightwave-response-parity-v1", "maps": rows},
            indent=1, sort_keys=True))
    print(f"light-wave response parity: {len(rows) - failures}/{len(rows)} maps exact")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
