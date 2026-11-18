#!/usr/bin/env python3
"""Pin canonical Python BSP streams for native migration parity tests.

Inputs are the direct outputs of ``split_bsp_glb_parts``/``split_world_glb``
retained by the stripped-pak build. The generator calls the one canonical GLB
decoder and payload encoder; it contains no geometry, UV, winding, material, or
splitting algorithm of its own. The compact JSON pins exact structural-block
hashes plus head/end stream samples. A whole-payload hash is deliberately
absent: float samples use the documented native migration tolerance.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Optional


REPO = Path(__file__).resolve().parents[2]
CONVERT = REPO / "Tools" / "convert"
IMPORT = REPO / "Tools" / "import"
DIST = REPO / "Tools" / "dist"
TOOLS = REPO / "Tools"
sys.path[:0] = [str(CONVERT), str(IMPORT), str(DIST), str(TOOLS)]
from wot_config import original_dir  # noqa: E402

from bsp_payload import (  # noqa: E402
    decode_bsp_glb,
    split_bsp_glb_parts,
)
from import_all import MP_MAPS  # noqa: E402
from bsp2obj import (  # noqa: E402
    SKY_SUFFIX,
    base_surface_material_slot,
    load_tex_macros,
    load_tex_sizes,
    macro_pan_material_slot,
    macro_pan_split_slots,
    sky_shared_split,
)
from ue1pkg import PF_INVISIBLE, PF_PORTAL, Package, decode_model  # noqa: E402
from runtime_map_projection_cache import (  # noqa: E402
    _BSP_HEADER,
    _BSP_IDENTITY,
    _BSP_SECTION,
    encode_bsp_v4,
)


def _require_tex_provenance(path) -> None:
    """Refuse a GLB converted without texture-size records.

    bsp2obj stamps asset.extras.wot_tex_sizes with the number of texture-size
    records it loaded. Zero, or an unstamped pre-provenance GLB, means the
    conversion ran without its tex root: every DrawScale silently fell back
    to 1.0 and the UVs baked into this fixture would be exactly wrong for any
    DrawScale-2 texture — the corruption behind the 2026-07-26 six-round
    parity hunt. Reconvert first:
    python3 Tools/convert/bsp2obj.py <map>.wot <conv-root> <conv-root>/tex
    """
    from bsp_payload import _read_glb
    document, _binary = _read_glb(Path(path))
    stamp = (document.get("asset", {}).get("extras") or {}).get("wot_tex_sizes")
    if not isinstance(stamp, int) or stamp <= 0:
        raise SystemExit(
            f"{path}: conversion provenance missing or zero texture-size "
            f"records (asset.extras.wot_tex_sizes={stamp!r}); the GLB was "
            "converted without its tex root and its UVs cannot be trusted.")


def _payload_block_sha256(payload: bytes, geometry: dict) -> dict[str, str]:
    """Hash exact-compared blocks as compact mismatch diagnostics."""
    vertex_count = geometry["vertex_count"]
    index_count = geometry["index_count"]
    section_table_end = _BSP_HEADER.size + sum(
        _BSP_SECTION.size + len(section["material_slot"].encode("utf-8"))
        + _BSP_IDENTITY.size
        + sum(len(section[field].encode("utf-8")) for field in (
            "canonical_package", "canonical_name", "source_texture_identity",
            "pan_zone_actor_identity"))
        for section in geometry["sections"])
    positions_end = section_table_end + vertex_count * 3 * 4
    normals_end = positions_end + vertex_count * 3 * 4
    uv0_end = normals_end + vertex_count * 2 * 4
    uv1_end = uv0_end + vertex_count * 2 * 4
    indices_end = uv1_end + index_count * 4
    if indices_end != len(payload):
        raise ValueError("canonical BSP payload block ranges do not cover payload")
    blocks = {
        "section_table": payload[_BSP_HEADER.size:section_table_end],
        "indices": payload[uv1_end:indices_end],
    }
    return {name: hashlib.sha256(block).hexdigest()
            for name, block in blocks.items()}


def _float_bits(values: list[float]) -> list[int]:
    # Samples describe encoded payload spelling, including its one zero form.
    return [struct.unpack("<I", struct.pack("<f", 0.0 if value == 0.0 else value))[0]
            for value in values]


def _head_end(values: list, width: int, rows: int = 3) -> dict[str, list]:
    count = min(len(values), width * rows)
    return {"end": values[len(values) - count:], "head": values[:count]}


def _payload_samples(geometry: dict) -> dict:
    return {
        "bounds": _float_bits(
            list(geometry["bounds_min"]) + list(geometry["bounds_max"])),
        "indices": _head_end(list(geometry["indices"]), 1, 6),
        "normals": _head_end(_float_bits(geometry["normals"]), 3),
        "positions": _head_end(_float_bits(geometry["positions"]), 3),
        "sections": [{
            "collision_enabled": section["collision_enabled"],
            "first_index": section["first_index"],
            "index_count": section["index_count"],
            "material_slot": section["material_slot"],
            "max_vertex": section["max_vertex"],
            "min_vertex": section["min_vertex"],
            "canonical_package": section["canonical_package"],
            "canonical_name": section["canonical_name"],
            "source_texture_identity": section["source_texture_identity"],
            "surface_flags": section["surface_flags"],
            "pan_zone_actor_identity": section["pan_zone_actor_identity"],
            "triangle_count": section["triangle_count"],
        } for section in geometry["sections"]],
        "uv0": _head_end(_float_bits(geometry["uv0"]), 2),
        "uv1": _head_end(_float_bits(geometry["uv1"]), 2),
    }


def _retail_section_identities(retail_root: Path, conversion_root: Path,
                               map_name: str, role: str,
                               sizes: dict | None = None,
                               macro_lookups: dict | None = None) -> dict:
    """Rebuild the converter's slot keys from the authoritative UE1 refs.

    ``bsp.json`` intentionally stores only the canonical material binding.  It
    cannot reconstruct a grouped source path after ``resolve_tex`` falls back
    to a flat leaf, so v4's source identity must come from the retail package.
    """
    package = Package(retail_root / "Maps" / f"{map_name}.wot")
    models = [row for row in package.exports
              if package.objname(row["cls"]) == "Model"]
    if not models:
        raise ValueError(f"{map_name}: retail package has no Model export")
    bsp = decode_model(package, max(models, key=lambda row: row["size"]))
    sizes = sizes if sizes is not None else load_tex_sizes(conversion_root / "tex")
    macro_lookups = macro_lookups if macro_lookups is not None \
        else load_tex_macros(conversion_root / "tex", sizes)
    sky_ids, split_materials = sky_shared_split(
        package, bsp, sizes, conversion_root / f"{map_name}.t3d.json")
    macro_splits = macro_pan_split_slots(
        package, bsp, sizes, sky_ids, split_materials, macro_lookups)
    rows = defaultdict(list)
    for node in bsp["nodes"]:
        _vertex_pool, surface_index, vertex_count = node[:3]
        if vertex_count < 3:
            continue
        surface = bsp["surfs"][surface_index]
        if surface["flags"] & PF_INVISIBLE:
            continue
        is_portal = bool(surface["flags"] & PF_PORTAL)
        if is_portal != (role == "portal"):
            continue
        canonical_package, canonical_name, *_unused, slot = \
            base_surface_material_slot(
                package, surface, surface_index, sizes,
                sky_ids, split_materials)
        if (is_portal, slot) in macro_splits:
            slot = macro_pan_material_slot(
                slot, surface["panu"], surface["panv"])
        source_identity = package.objpath(surface["tex"])
        if not source_identity and canonical_name == "None":
            source_identity = "None"
        if not source_identity:
            raise ValueError(
                f"{map_name}:{slot}: retail texture source provenance is absent")
        identity = {
            "canonical_package": canonical_package,
            "canonical_name": canonical_name,
            "source_texture_identity": source_identity,
            "surface_flags": int(surface["flags"]),
            "pan_zone_actor_identity": "",
        }
        if identity not in rows[slot]:
            rows[slot].append(identity)
    return dict(rows)


def generate(conversion_root: Path, retail_root: Optional[Path] = None) -> bytes:
    retail_root = retail_root or original_dir()
    maps = []
    sizes = load_tex_sizes(conversion_root / "tex")
    macro_lookups = load_tex_macros(conversion_root / "tex", sizes)
    with tempfile.TemporaryDirectory() as temporary:
        split_root = Path(temporary)
        for map_name in MP_MAPS:
            bsp_document = json.loads(
                (conversion_root / f"{map_name}.bsp.json").read_text(
                    encoding="utf-8"))
            rows = split_bsp_glb_parts(
                conversion_root, map_name, split_root / map_name)
            parts = []
            for row in rows:
                _require_tex_provenance(row["path"])
                geometry = decode_bsp_glb(row["path"], row["role"])
                retail_identities = _retail_section_identities(
                    retail_root, conversion_root, map_name, row["role"], sizes,
                    macro_lookups)
                for section in geometry["sections"]:
                    json_table = bsp_document[
                        "portal_materials" if row["role"] == "portal"
                        else "materials"]
                    json_identity = json_table.get(section["material_slot"])
                    if json_identity is None and section["material_slot"].endswith(
                            "__SkyZone"):
                        json_identity = json_table.get(
                            section["material_slot"].removesuffix("__SkyZone"))
                    candidates = retail_identities.get(
                        section["material_slot"], [])
                    if not candidates and section["material_slot"].endswith(
                            "__SkyZone"):
                        candidates = retail_identities.get(
                            section["material_slot"].removesuffix("__SkyZone"), [])
                    matching = [candidate for candidate in candidates
                                if json_identity is not None and (
                                    candidate["canonical_package"],
                                    candidate["canonical_name"],
                                    candidate["surface_flags"]) == (
                                        json_identity.get("package"),
                                        json_identity.get("texture"),
                                        int(json_identity.get("flags", -1)))]
                    source_paths = {candidate["source_texture_identity"]
                                    for candidate in matching}
                    if len(source_paths) != 1:
                        raise ValueError(
                            f"{map_name}:{section['material_slot']}: retail and "
                            f"bsp.json material identities disagree: "
                            f"json={json_identity!r} retail={candidates!r}")
                    identity = dict(matching[0])
                    pan = json_identity.get("pan_zone_actor") \
                        or json_identity.get("pan_zone", "")
                    identity["pan_zone_actor_identity"] = (
                        f"{map_name}.{pan}" if pan and "." not in pan else pan)
                    section.update(identity)
                payload = encode_bsp_v4(geometry)
                sections = geometry["sections"]
                procedural_slot = (
                    sections[0]["material_slot"]
                    if row["material_slot"] is not None and len(sections) == 1
                    else "")
                parts.append({
                    "index_count": geometry["index_count"],
                    "part_identity": row["asset_name"],
                    "payload_block_sha256": _payload_block_sha256(
                        payload, geometry),
                    "payload_byte_length": len(payload),
                    "payload_samples": _payload_samples(geometry),
                    "procedural_material_slot": procedural_slot,
                    "role": row["role"],
                    "section_count": len(sections),
                    "tangent_algorithm": geometry["tangent_algorithm"],
                    "triangle_count": geometry["triangle_count"],
                    "vertex_count": geometry["vertex_count"],
                })
            if not parts:
                raise ValueError(f"{map_name}: canonical split produced no BSP parts")
            maps.append({"identity": map_name, "parts": parts})
    document = {
        "format": "wot-native-bsp-parity-v3",
        "maps": maps,
    }
    # NO trailing newline: ParseCanonicalJson rejects every trailing byte.
    encoded = json.dumps(document, sort_keys=True,
                         separators=(",", ":")).encode("ascii")
    return encoded


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--conversion-root", required=True, type=Path)
    parser.add_argument("--retail-root", type=Path,
                        help="retail tree (default: WOT_REFERENCE_ROOT/original)")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        expected = generate(args.conversion_root, args.retail_root)
        if args.check:
            if args.output.read_bytes() != expected:
                print(f"native BSP parity fixture is stale: {args.output}",
                      file=sys.stderr)
                return 1
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(expected)
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(f"native BSP parity fixture OK: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
