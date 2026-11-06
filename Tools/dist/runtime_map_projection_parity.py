#!/usr/bin/env python3
"""Exact parity oracle/comparator for the runtime-map cache projection.

The native run is intentionally an input, not an implementation detail.  A
packaging/native-test pass can dump raw ``world-v3`` and ``WOTDEF3`` payloads;
this tool independently derives the expected leaves, light identities,
friction, mover geometry, and Battle struct values from the retail/dev
artifacts and compares every retained value.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
from pathlib import Path
from typing import Any, Iterable

from Tools.dist.runtime_map_projection_cache import (
    CacheFormatError,
    parse_class_defaults_v3,
    parse_world_v3,
)


class ProjectionParityError(AssertionError):
    """An exact native/oracle value differs."""


def last_named_records(rows: Iterable[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Match zones.py's dict-comprehension rule: last export name wins."""
    result = {}
    for row in rows:
        name = row.get("name")
        if not isinstance(name, str) or not name:
            raise CacheFormatError("light export lacks a name")
        result[name.casefold()] = row
    return result


def canonical_light_runs(map_name: str, zone: dict[str, Any],
                         exports: Iterable[dict[str, Any]]) -> dict[str, list[str]]:
    last = last_named_records(exports)
    names = zone.get("light_actors")
    runs = zone.get("leaf_lights")
    if not isinstance(names, list) or not isinstance(runs, dict):
        raise CacheFormatError(f"{map_name}: invalid zone light oracle")
    resolved = []
    for name in names:
        row = last.get(str(name).casefold())
        if row is None:
            raise CacheFormatError(f"{map_name}: light {name!r} has no last export")
        resolved.append(f"{map_name}.{row['name']}")
    result = {}
    for leaf, indices in runs.items():
        if not isinstance(indices, list) or any(
                not isinstance(index, int) or isinstance(index, bool)
                or not 0 <= index < len(resolved) for index in indices):
            raise CacheFormatError(f"{map_name}: invalid light run for leaf {leaf}")
        result[str(int(leaf))] = [resolved[index] for index in indices]
    return dict(sorted(result.items(), key=lambda item: int(item[0])))


def _f32_bits(values: Iterable[float]) -> list[int]:
    result = []
    for value in values:
        value = float(value)
        if not math.isfinite(value):
            raise CacheFormatError("non-finite parity float")
        # WoTWorldFloatBits gives every serialized zero the one canonical
        # cache spelling.  Converter GLBs legitimately carry IEEE -0.0, so
        # normalize before comparing their restored UE1 streams to native.
        if value == 0.0:
            value = 0.0
        result.append(struct.unpack("<I", struct.pack("<f", value))[0])
    return result


def _read_glb(path: Path) -> tuple[dict[str, Any], bytes]:
    data = Path(path).read_bytes()
    if len(data) < 28:
        raise CacheFormatError(f"{path}: truncated GLB")
    magic, version, total = struct.unpack_from("<III", data)
    if magic != 0x46546C67 or version != 2 or total != len(data):
        raise CacheFormatError(f"{path}: invalid GLB header")
    offset = 12
    chunks = []
    while offset < len(data):
        if offset + 8 > len(data):
            raise CacheFormatError(f"{path}: truncated GLB chunk")
        size, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        end = offset + size
        if size % 4 or end > len(data):
            raise CacheFormatError(f"{path}: invalid GLB chunk range")
        chunks.append((kind, data[offset:end]))
        offset = end
    if len(chunks) != 2 or chunks[0][0] != 0x4E4F534A \
            or chunks[1][0] != 0x004E4942:
        raise CacheFormatError(f"{path}: expected JSON and BIN chunks")
    try:
        return json.loads(chunks[0][1].decode("utf-8")), chunks[1][1]
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise CacheFormatError(f"{path}: invalid GLB JSON") from exc


def _accessor(document: dict[str, Any], binary: bytes, index: Any,
              component_type: int, spelling: str, width: int,
              label: str) -> list[int | float]:
    accessors = document.get("accessors")
    views = document.get("bufferViews")
    if not isinstance(accessors, list) or not isinstance(views, list) \
            or not isinstance(index, int) or isinstance(index, bool) \
            or not 0 <= index < len(accessors):
        raise CacheFormatError(f"{label}: invalid accessor")
    row = accessors[index]
    view_index = row.get("bufferView") if isinstance(row, dict) else None
    if row.get("componentType") != component_type or row.get("type") != spelling \
            or not isinstance(row.get("count"), int) or row["count"] < 0 \
            or not isinstance(view_index, int) or not 0 <= view_index < len(views):
        raise CacheFormatError(f"{label}: unsupported accessor shape")
    view = views[view_index]
    if not isinstance(view, dict) or view.get("buffer", 0) != 0:
        raise CacheFormatError(f"{label}: unsupported buffer view")
    base = view.get("byteOffset", 0)
    length = view.get("byteLength")
    relative = row.get("byteOffset", 0)
    if any(not isinstance(value, int) or isinstance(value, bool) or value < 0
           for value in (base, length, relative)):
        raise CacheFormatError(f"{label}: invalid accessor range")
    count = row["count"] * width
    byte_count = count * 4
    start = base + relative
    if start < base or start + byte_count > base + length \
            or base + length > len(binary):
        raise CacheFormatError(f"{label}: accessor escapes buffer view")
    code = "f" if component_type == 5126 else "I"
    values = list(struct.unpack_from(f"<{count}{code}", binary, start))
    if code == "f" and not all(math.isfinite(value) for value in values):
        raise CacheFormatError(f"{label}: non-finite stream")
    return values


def _mover_glb_exact(path: Path) -> list[dict[str, Any]]:
    document, binary = _read_glb(path)
    meshes = document.get("meshes")
    materials = document.get("materials")
    if not isinstance(meshes, list) or len(meshes) != 1 \
            or not isinstance(materials, list):
        raise CacheFormatError(f"{path}: invalid mover mesh/material list")
    primitives = meshes[0].get("primitives", [])
    if not isinstance(primitives, list) or not primitives:
        raise CacheFormatError(f"{path}: empty mover primitive list")
    result = []
    for ordinal, primitive in enumerate(primitives):
        attributes = primitive.get("attributes", {})
        material_index = primitive.get("material")
        if primitive.get("mode", 4) != 4 or not isinstance(attributes, dict) \
                or not isinstance(material_index, int) \
                or not 0 <= material_index < len(materials):
            raise CacheFormatError(f"{path}: primitive {ordinal} binding is invalid")
        position = _accessor(document, binary, attributes.get("POSITION"),
                             5126, "VEC3", 3, "position")
        normal = _accessor(document, binary, attributes.get("NORMAL"),
                           5126, "VEC3", 3, "normal")
        uv0 = _accessor(document, binary, attributes.get("TEXCOORD_0"),
                        5126, "VEC2", 2, "UV0")
        indices = _accessor(document, binary, primitive.get("indices"),
                            5125, "SCALAR", 1, "indices")
        if not position or len(normal) != len(position) \
                or len(uv0) * 3 != len(position) * 2 \
                or not indices or len(indices) % 3 \
                or any(index >= len(position) // 3 for index in indices):
            raise CacheFormatError(f"{path}: primitive {ordinal} streams disagree")
        plane = (primitive.get("extras") or {}).get("wotMoverPlane")
        if not isinstance(plane, dict) or set(plane) != {"origin", "normal"} \
                or not all(isinstance(plane.get(name), list)
                           and len(plane[name]) == 3
                           for name in ("origin", "normal")):
            raise CacheFormatError(f"{path}: primitive {ordinal} has no exact plane")
        # movers2glb spells glTF=(UE.x, UE.z, UE.y)/100.  Compare the native
        # cache's serialized float32 values.  _f32_bits canonicalizes either
        # IEEE zero spelling to the native cache's positive-zero spelling.
        ue_positions = []
        ue_normals = []
        for at in range(0, len(position), 3):
            ue_positions.extend((position[at] * 100.0,
                                 position[at + 2] * 100.0,
                                 position[at + 1] * 100.0))
            ue_normals.extend((normal[at], normal[at + 2], normal[at + 1]))
        origin = plane["origin"]
        plane_normal = plane["normal"]
        result.append({
            "material_slot": materials[material_index].get("name"),
            "plane_origin_f32": _f32_bits(
                (origin[0] * 100.0, origin[2] * 100.0, origin[1] * 100.0)),
            "plane_normal_f32": _f32_bits(
                (plane_normal[0], plane_normal[2], plane_normal[1])),
            "positions_f32": _f32_bits(ue_positions),
            "normals_f32": _f32_bits(ue_normals),
            "uv0_f32": _f32_bits(uv0),
            "indices": indices,
        })
    return result


def _mover_source_exact(map_path: Path, model_name: str,
                        texture_root: Path) -> list[dict[str, Any]]:
    """Re-run the read-only bsp2obj math and narrow once at cache serialization."""
    convert = Path(__file__).resolve().parents[1] / "convert"
    sys.path.insert(0, str(convert))
    try:
        from bsp2obj import (SKIP, build_groups, flag_suffix, load_tex_sizes,
                             resolve_tex, split_mover_groups_by_plane)  # type: ignore
        from ue1pkg import Package, decode_model  # type: ignore
    finally:
        sys.path.pop(0)
    package = Package(map_path)
    exports = {row["name"]: row for row in package.exports}
    if model_name not in exports:
        raise CacheFormatError(f"{map_path}: missing mover model {model_name}")
    bsp = decode_model(package, exports[model_name])
    texture_sizes = load_tex_sizes(texture_root)
    groups, texture_flags, stats = build_groups(
        package, bsp, texture_sizes, include_planes=True)
    if not stats["tris"]:
        raise CacheFormatError(f"{map_path}:{model_name}: mover has no triangles")
    source_by_material = {}
    for vertex_pool, surface_index, vertex_count, _plane in bsp["nodes"]:
        del vertex_pool
        if vertex_count < 3:
            continue
        surface = bsp["surfs"][surface_index]
        if surface["flags"] & SKIP:
            continue
        canonical_package, canonical_name, *_ = resolve_tex(
            package, surface["tex"], texture_sizes)
        material = Package.qualkey(canonical_package, canonical_name) \
            + flag_suffix(surface["flags"])
        source_identity = package.objpath(surface["tex"]) \
            if surface["tex"] else "None"
        if not source_identity:
            source_identity = "None"
        prior = source_by_material.setdefault(material, source_identity)
        if prior != source_identity:
            raise CacheFormatError(
                f"{map_path}:{model_name}: material aliases source identities")
    groups, texture_flags, surfaces, _extras = split_mover_groups_by_plane(
        groups, texture_flags, bsp["points"])
    surface_by_slot = {row["slot"]: row for row in surfaces}
    result = []
    for slot in sorted(groups):
        metadata = surface_by_slot[slot]
        origin = [float(value) for value in metadata["origin"]]
        plane_normal = [float(value) for value in metadata["normal"]]
        positions = []
        normals = []
        uv0 = []
        for triangle in groups[slot]:
            indices, uvs, normal = triangle[:3]
            for point_index, uv in zip(reversed(indices), reversed(uvs)):
                point = [float(value) for value in bsp["points"][point_index]]
                distance = sum((point[axis] - origin[axis])
                               * plane_normal[axis] for axis in range(3))
                positions.extend(point[axis] - distance * plane_normal[axis]
                                 for axis in range(3))
                normals.extend(float(value) for value in normal)
                uv0.extend(float(value) for value in uv)
        canonical_package, canonical_name, flags = texture_flags[slot]
        result.append({
            "material_slot": slot,
            "canonical_package": canonical_package,
            "canonical_name": canonical_name,
            "source_texture_identity": source_by_material[
                metadata["source_material"]],
            "surface_flags": flags,
            "collision_enabled": True,
            "plane_origin_f32": _f32_bits(origin),
            "plane_normal_f32": _f32_bits(plane_normal),
            "positions_f32": _f32_bits(positions),
            "normals_f32": _f32_bits(normals),
            "uv0_f32": _f32_bits(uv0),
            "indices": list(range(len(positions) // 3)),
        })
    return result


def collect_exact_mover_oracle(root: Path, maps: Iterable[str],
                               retail_root: Path | None = None) \
        -> dict[str, list[dict[str, Any]]]:
    result = {}
    for map_name in maps:
        directory = Path(root) / map_name
        document = json.loads((directory / "movers.json").read_text(encoding="utf-8"))
        movers = document.get("movers")
        if not isinstance(movers, list):
            raise CacheFormatError(f"{map_name}: invalid movers.json")
        rows = []
        for mover in movers:
            actor = mover.get("name")
            model = mover.get("model")
            if not isinstance(actor, str) or not actor or not isinstance(model, str) or not model:
                raise CacheFormatError(f"{map_name}: invalid mover identity")
            sections = _mover_glb_exact(directory / f"{model}.glb")
            if retail_root is not None:
                source_rows = _mover_source_exact(
                    Path(retail_root) / "Maps" / f"{map_name}.wot", model,
                    Path(root).parent / "tex")
                source_by_slot = {row["material_slot"]: row
                                  for row in source_rows}
                if len(source_by_slot) != len(source_rows) or set(source_by_slot) != {
                        row["material_slot"] for row in sections}:
                    raise CacheFormatError(
                        f"{map_name}.{actor}: GLB/source mover slots disagree")
                metadata_fields = (
                    "canonical_package", "canonical_name",
                    "source_texture_identity", "surface_flags",
                    "collision_enabled")
                for section in sections:
                    source = source_by_slot[section["material_slot"]]
                    section.update({key: source[key] for key in metadata_fields})
            declared = mover.get("surfaces")
            if not isinstance(declared, list) or [row.get("slot") for row in declared] != [
                    row["material_slot"] for row in sections]:
                # GLB primitives/materials are canonical-sorted while the
                # metadata list is source ordered, so compare as an exact key set.
                by_slot = {row.get("slot"): row for row in declared or []}
                if len(by_slot) != len(sections) or set(by_slot) != {
                        row["material_slot"] for row in sections}:
                    raise CacheFormatError(f"{map_name}.{actor}: mover sections disagree")
            rows.append({
                "actor_identity": f"{map_name}.{actor}",
                "model_identity": f"{map_name}.{model}",
                "sections": sections,
            })
        result[map_name] = rows
    return result


def collect_exact_zone_oracle(zone_root: Path, retail_root: Path,
                              maps: Iterable[str]) -> dict[str, dict[str, Any]]:
    convert = Path(__file__).resolve().parents[1] / "convert"
    sys.path.insert(0, str(convert))
    try:
        from lightmaps import decode_model_full  # type: ignore
        from ue1pkg import Package  # type: ignore
    finally:
        sys.path.pop(0)
    result = {}
    for map_name in maps:
        zone = json.loads((Path(zone_root) / f"{map_name}.zones.json").read_text(
            encoding="utf-8"))
        package = Package(Path(retail_root) / "Maps" / f"{map_name}.wot")
        models = [row for row in package.exports
                  if package.objname(row["cls"]) == "Model"]
        if not models:
            raise CacheFormatError(f"{map_name}: retail package has no Model")
        bsp = decode_model_full(package, max(models, key=lambda row: row["size"]))
        exports = [{"name": row["name"], "ordinal": ordinal}
                   for ordinal, row in enumerate(package.exports)]
        leaf_bsp = zone.get("leaf_bsp")
        if not isinstance(leaf_bsp, dict) \
                or leaf_bsp.get("num_leaves") != len(bsp["leaves"]):
            raise CacheFormatError(f"{map_name}: leaf oracle disagrees with retail")
        result[map_name] = {
            "leaves": [list(row) for row in bsp["leaves"]],
            "leaf_bsp": {
                "root_outside": bool(leaf_bsp.get("root_outside")),
                "nodes": leaf_bsp.get("nodes"),
            },
            "leaf_light_identities": canonical_light_runs(map_name, zone, exports),
            "friction_nodes": zone.get("friction_nodes"),
        }
    return result


_BUDGET_LINE = re.compile(
    r"Budget\((\d+)\)=\((?:ResourcePackageName=(\w+),"
    r"ResourceAdapterName=(\w+),)?ItemCount=(\d+)\)")
_INDEXED_PROPERTY = re.compile(r"^(Budget|EventList)\((\d+)\)$")


def _tuple_fields(value: Any) -> dict[str, str]:
    text = str(value or "").strip()
    if text.startswith("(") and text.endswith(")"):
        text = text[1:-1]
    result = {}
    for field in text.split(","):
        if "=" in field:
            name, field_value = field.split("=", 1)
            result[name.strip()] = field_value.strip()
    return result


def _canonical_ue1_name(value: Any) -> str:
    """Use the parity schema's empty spelling for UE1's NAME_None."""
    result = str(value or "")
    return "" if result.casefold() == "none" else result


def _budget_defaults(script_root: Path, class_name: str) -> list[dict[str, Any]]:
    source = (Path(script_root) / f"{class_name}.uc").read_text(
        encoding="utf-8", errors="replace")
    rows = []
    for match in _BUDGET_LINE.finditer(source):
        index, package, adapter, count = match.groups()
        rows.append({"array_index": int(index),
                     "resource_package_name": _canonical_ue1_name(package),
                     "resource_adapter_name": _canonical_ue1_name(adapter),
                     "item_count": int(count)})
    if not rows:
        raise CacheFormatError(f"{class_name}: no Resource defaults")
    return rows


def _sound_event(index: int, value: Any) -> dict[str, Any]:
    fields = _tuple_fields(value)
    sound = fields.get("Sound", "")
    match = re.fullmatch(r"Sound'([^']+)'", sound)
    if sound and match is None:
        raise CacheFormatError(f"unresolved SoundEvent object {sound!r}")
    return {
        "array_index": index,
        "delay": struct.unpack(
            "<f", struct.pack("<f", float(fields.get("Delay", 0.0))))[0],
        "event": _canonical_ue1_name(fields.get("Event", "")),
        "sound_source_tag": _canonical_ue1_name(
            fields.get("SoundSourceTag", "")),
        "sound_identity": match.group(1) if match else "",
        "sound_table_tag": _canonical_ue1_name(
            fields.get("SoundTableTag", "")),
        "sound_table_slot": _canonical_ue1_name(
            fields.get("SoundTableSlot", "")),
    }


def collect_battle_struct_oracle(script_root: Path, t3d_root: Path,
                                 maps: Iterable[str]) -> dict[str, dict[str, Any]]:
    result = {}
    for map_name in maps:
        document = json.loads((Path(t3d_root) / f"{map_name}.t3d.json").read_text(
            encoding="utf-8"))
        actors = document.get("actors")
        if not isinstance(actors, list):
            raise CacheFormatError(f"{map_name}: invalid T3D actor list")
        budgets = []
        dispatchers = []
        for actor in actors:
            class_name = actor.get("cls", "")
            props = actor.get("props", {})
            if not isinstance(props, dict):
                raise CacheFormatError(f"{map_name}: invalid actor properties")
            if class_name.endswith("Budget"):
                defaults = _budget_defaults(script_root, class_name)
                overrides = []
                effective = {row["array_index"]: dict(row) for row in defaults}
                for property_name, value in props.items():
                    match = _INDEXED_PROPERTY.match(property_name)
                    if not match or match.group(1) != "Budget":
                        continue
                    index = int(match.group(2))
                    fields = _tuple_fields(value)
                    row = {"array_index": index}
                    if "ResourcePackageName" in fields:
                        row["resource_package_name"] = _canonical_ue1_name(
                            fields["ResourcePackageName"])
                    if "ResourceAdapterName" in fields:
                        row["resource_adapter_name"] = _canonical_ue1_name(
                            fields["ResourceAdapterName"])
                    if "ItemCount" in fields:
                        row["item_count"] = int(fields["ItemCount"])
                    overrides.append(row)
                    if index not in effective:
                        effective[index] = {"array_index": index,
                                            "resource_package_name": "",
                                            "resource_adapter_name": "",
                                            "item_count": 0}
                    effective[index].update(row)
                budgets.append({
                    "actor_identity": f"{map_name}.{actor.get('name', '')}",
                    "class_identity": f"WOT.{class_name}",
                    # Retail BudgetInfo has no Team default; zero is required.
                    "team": int(props.get("Team", 0)),
                    "class_defaults": defaults,
                    "instance_overrides": sorted(overrides,
                                                 key=lambda row: row["array_index"]),
                    "effective": [effective[index] for index in sorted(effective)],
                })
            elif class_name == "SoundDispatcher":
                events = []
                for property_name, value in props.items():
                    match = _INDEXED_PROPERTY.match(property_name)
                    if match and match.group(1) == "EventList":
                        events.append(_sound_event(int(match.group(2)), value))
                dispatchers.append({
                    "actor_identity": f"{map_name}.{actor.get('name', '')}",
                    "event_list": sorted(events, key=lambda row: row["array_index"]),
                })
        result[map_name] = {"budgets": budgets,
                            "sound_dispatchers": dispatchers}
    return result


def class_struct_roundtrip_oracle(
        battle: dict[str, dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    """Flatten both resolved schemas into stable, exact comparison rows."""
    resources = []
    sound_events = []
    for map_name, row in battle.items():
        for budget in row["budgets"]:
            resources.append({
                "map_identity": map_name,
                "actor_identity": budget["actor_identity"],
                "class_identity": budget["class_identity"],
                "class_defaults": budget["class_defaults"],
                "instance_overrides": budget["instance_overrides"],
                "effective": budget["effective"],
            })
        for dispatcher in row["sound_dispatchers"]:
            for event in dispatcher["event_list"]:
                sound_events.append({
                    "map_identity": map_name,
                    "actor_identity": dispatcher["actor_identity"],
                    **event,
                })
    return {"Resource": resources, "SoundEvent": sound_events}


def build_exact_projection_oracle(zone_root: Path, retail_root: Path,
                                  mover_root: Path, script_root: Path,
                                  t3d_root: Path, maps: Iterable[str]) \
        -> dict[str, Any]:
    maps = tuple(maps)
    zone_rows = collect_exact_zone_oracle(zone_root, retail_root, maps)
    mover_rows = collect_exact_mover_oracle(mover_root, maps, retail_root)
    for map_name in maps:
        zone_rows[map_name]["mover_models"] = mover_rows[map_name]
    battle_maps = tuple(name for name in maps if name.startswith("Battle"))
    battle = collect_battle_struct_oracle(script_root, t3d_root, battle_maps)
    return {
        "format": "runtime-map-projection-exact-v1",
        "maps": zone_rows,
        "class_struct_round_trips": class_struct_roundtrip_oracle(battle),
        "battle": battle,
    }


def canonical_native_world(map_name: str, document: dict[str, Any]) \
        -> dict[str, Any]:
    nodes = document.get("nodes", [])
    leaves = document.get("leaves", [])
    semantics = document.get("semantic_objects", [])
    lights = document.get("model_lights", [])
    light_runs = {}
    for leaf_index, leaf in enumerate(leaves):
        start = int(leaf["permeating"])
        if start < 0:
            continue
        run = []
        cursor = start
        while cursor < len(lights) and lights[cursor] is not None:
            semantic = semantics[lights[cursor]]
            run.append(semantic["identity"])
            cursor += 1
        light_runs[str(leaf_index)] = run
    friction = []
    surfaces = document.get("surfaces", [])
    for node_index, node in enumerate(nodes):
        surface = int(node["surface"])
        if 0 <= surface < len(surfaces) and surfaces[surface]["friction"] != 1.0:
            friction.append([node_index, surfaces[surface]["friction"]])
    mover_rows = []
    for mover in document.get("mover_models", []):
        section_rows = []
        for section in mover["sections"]:
            minimum = section["min_vertex"]
            maximum = section["max_vertex"] + 1
            first = section["first_index"]
            end = first + section["index_count"]
            positions = [value for row in mover["positions"][minimum:maximum]
                         for value in row]
            normals = [value for row in mover["normals"][minimum:maximum]
                       for value in row]
            uv0 = [value for row in mover["uv0"][minimum:maximum] for value in row]
            section_rows.append({
                "material_slot": section["material_slot"],
                "canonical_package": section["canonical_package"],
                "canonical_name": section["canonical_name"],
                "source_texture_identity": section["source_texture_identity"],
                "surface_flags": section["surface_flags"],
                "collision_enabled": section["collision_enabled"],
                "plane_origin_f32": _f32_bits(section["plane_origin"]),
                "plane_normal_f32": _f32_bits(section["plane_normal"]),
                "positions_f32": _f32_bits(positions),
                "normals_f32": _f32_bits(normals),
                "uv0_f32": _f32_bits(uv0),
                "indices": [index - minimum for index in mover["indices"][first:end]],
            })
        mover_rows.append({"actor_identity": mover["actor_identity"],
                           "model_identity": mover["model_identity"],
                           "sections": section_rows})
    return {
        "leaves": [[row["zone"], row["permeating"], row["volumetric"]]
                   for row in leaves],
        "leaf_bsp": {
            "root_outside": bool(document.get("root_outside")),
            "nodes": [[row["leaves"][0], row["leaves"][1],
                       int(row["vertex_count"] > 0 and not row["flags"] & 0x21)]
                      for row in nodes],
        },
        "leaf_light_identities": light_runs,
        "friction_nodes": friction,
        "mover_models": mover_rows,
    }


def collect_native_world_snapshot(root: Path, maps: Iterable[str]) \
        -> dict[str, dict[str, Any]]:
    result = {}
    for map_name in maps:
        payload = (Path(root) / f"{map_name}.world-v3.bin").read_bytes()
        result[map_name] = canonical_native_world(map_name, parse_world_v3(payload))
    return result


def _native_class_records(path: Path) -> dict[str, dict[str, Any]]:
    path = Path(path)
    payloads = sorted(path.rglob("*.bin")) if path.is_dir() else [path]
    if not payloads:
        raise CacheFormatError(f"{path}: no WOTDEF3-v3 payloads")
    records: dict[str, dict[str, Any]] = {}
    aliases: dict[str, str] = {}
    alias_counts: dict[str, int] = {}
    for payload in payloads:
        for record in parse_class_defaults_v3(payload.read_bytes()):
            key = record["identity"].casefold()
            if key in records:
                raise CacheFormatError("class-default identities collide")
            records[key] = record
            marker = key.rfind("#export-")
            if marker > 0:
                base = key[:marker]
                alias_counts[base] = alias_counts.get(base, 0) + 1
                aliases[base] = key
    aliases = {base: key for base, key in aliases.items()
               if alias_counts[base] == 1 and base not in records}

    def find(identity: str) -> dict[str, Any] | None:
        key = identity.casefold()
        return records.get(key) or records.get(aliases.get(key, ""))

    for record in records.values():
        seen = set()
        current = record
        while current["superclass_identity"]:
            key = current["identity"].casefold()
            if key in seen:
                raise CacheFormatError("class-default superclass graph has a cycle")
            seen.add(key)
            current = find(current["superclass_identity"])
            if current is None:
                raise CacheFormatError(
                    f"class-default superclass {record['superclass_identity']!r} is absent")
    records["__aliases__"] = aliases
    return records


def _class_record(records: dict[str, dict[str, Any]],
                  identity: str) -> dict[str, Any] | None:
    key = identity.casefold()
    direct = records.get(key)
    if direct is not None:
        return direct
    return records.get(records["__aliases__"].get(key, ""))


def _effective_class_properties(records: dict[str, dict[str, Any]],
                                identity: str) -> list[dict[str, Any]]:
    chain = []
    current = _class_record(records, identity)
    if current is None:
        raise CacheFormatError(f"class defaults are absent for {identity}")
    while current is not None:
        chain.append(current)
        superclass = current["superclass_identity"]
        current = _class_record(records, superclass) if superclass else None
    effective: dict[tuple[str, int], dict[str, Any]] = {}
    for record in reversed(chain):
        for prop in record["properties"]:
            effective[(prop["name"].casefold(), prop["array_index"])] = prop
    return list(effective.values())


def _resource_property(prop: dict[str, Any]) -> dict[str, Any]:
    if prop.get("type") != 10 or str(prop.get("struct_name", "")).casefold() \
            != "resource":
        raise CacheFormatError("Budget property is not a Resource struct")
    members = {member["name"]: member for member in prop["members"]}
    try:
        package = _canonical_ue1_name(
            members["ResourcePackageName"]["name_value"])
        adapter = _canonical_ue1_name(
            members["ResourceAdapterName"]["name_value"])
        count_member = members["ItemCount"]
        if count_member["type"] != 2 or len(count_member["value"]) != 4:
            raise KeyError("ItemCount")
        (count,) = struct.unpack("<i", count_member["value"])
    except KeyError as exc:
        raise CacheFormatError("decoded Resource member is corrupt") from exc
    return {"array_index": int(prop["array_index"]),
            "resource_package_name": package,
            "resource_adapter_name": adapter, "item_count": count}


def _sound_event_property(prop: dict[str, Any]) -> dict[str, Any]:
    if prop.get("type") != 10 or str(prop.get("struct_name", "")).casefold() \
            != "soundevent":
        raise CacheFormatError("EventList property is not a SoundEvent struct")
    members = {member["name"]: member for member in prop["members"]}
    try:
        delay_member = members["Delay"]
        sound_member = members["Sound"]
        if delay_member["type"] != 4 or len(delay_member["value"]) != 4 \
                or sound_member["type"] != 5:
            raise KeyError("typed member")
        (delay,) = struct.unpack("<f", delay_member["value"])
        if not math.isfinite(delay):
            raise CacheFormatError("decoded SoundEvent delay is non-finite")
        return {
            "array_index": int(prop["array_index"]), "delay": delay,
            "event": _canonical_ue1_name(members["Event"]["name_value"]),
            "sound_source_tag": _canonical_ue1_name(
                members["SoundSourceTag"]["name_value"]),
            "sound_identity": sound_member["object_identity"],
            "sound_table_tag": _canonical_ue1_name(
                members["SoundTableTag"]["name_value"]),
            "sound_table_slot": _canonical_ue1_name(
                members["SoundTableSlot"]["name_value"]),
        }
    except KeyError as exc:
        raise CacheFormatError("decoded SoundEvent member is corrupt") from exc


def _team_value(properties: Iterable[dict[str, Any]]) -> int:
    for prop in properties:
        if prop["name"].casefold() != "team" or prop["array_index"] != 0:
            continue
        if prop["type"] == 1 and len(prop["value"]) == 1:
            return prop["value"][0]
        if prop["type"] == 2 and len(prop["value"]) == 4:
            return struct.unpack("<i", prop["value"])[0]
        raise CacheFormatError("decoded Team property is corrupt")
    return 0


def collect_native_battle_snapshot(
        world_root: Path, class_defaults: Path, maps: Iterable[str]) \
        -> dict[str, dict[str, Any]]:
    """Decode raw world-v3 and WOTDEF3-v3 values into the Battle schema."""
    records = _native_class_records(class_defaults)
    result = {}
    for map_name in maps:
        world = parse_world_v3(
            (Path(world_root) / f"{map_name}.world-v3.bin").read_bytes())
        budgets = []
        dispatchers = []
        for actor in world["actors"]:
            class_identity = actor["class_identity"]
            public_class = class_identity.split("#export-", 1)[0]
            class_chain = []
            current = _class_record(records, class_identity)
            while current is not None:
                class_chain.append(current["identity"].split("#export-", 1)[0].casefold())
                parent = current["superclass_identity"]
                current = _class_record(records, parent) if parent else None
            is_budget = public_class.casefold().endswith("budget")
            is_dispatcher = "wot.sounddispatcher" in class_chain
            if not is_budget and not is_dispatcher:
                continue
            class_properties = _effective_class_properties(records, class_identity)
            if is_budget:
                defaults = [_resource_property(prop) for prop in class_properties
                            if prop["name"].casefold() == "budget"]
                effective = {row["array_index"]: dict(row) for row in defaults}
                overrides = []
                empty_resource = {"resource_package_name": "",
                                  "resource_adapter_name": "", "item_count": 0}
                for prop in actor["properties"]:
                    if prop["name"].casefold() != "budget":
                        continue
                    decoded = _resource_property(prop)
                    index = decoded["array_index"]
                    base = effective.get(index, {
                        "array_index": index, **empty_resource})
                    delta = {"array_index": index}
                    for field in empty_resource:
                        if decoded[field] != base[field]:
                            delta[field] = decoded[field]
                    overrides.append(delta)
                    effective[index] = dict(decoded)
                team_props = list(actor["properties"])
                team = _team_value(team_props) if any(
                    prop["name"].casefold() == "team" and prop["array_index"] == 0
                    for prop in team_props) else _team_value(class_properties)
                budgets.append({
                    "actor_identity": actor["identity"],
                    "class_identity": public_class, "team": team,
                    "class_defaults": sorted(
                        defaults, key=lambda row: row["array_index"]),
                    "instance_overrides": sorted(
                        overrides, key=lambda row: row["array_index"]),
                    "effective": [effective[index] for index in sorted(effective)],
                })
            elif is_dispatcher:
                events = {prop["array_index"]: _sound_event_property(prop)
                          for prop in class_properties
                          if prop["name"].casefold() == "eventlist"}
                for prop in actor["properties"]:
                    if prop["name"].casefold() == "eventlist":
                        events[prop["array_index"]] = _sound_event_property(prop)
                dispatchers.append({
                    "actor_identity": actor["identity"],
                    "event_list": [events[index] for index in sorted(events)],
                })
        result[map_name] = {"budgets": budgets,
                            "sound_dispatchers": dispatchers}
    return result


def _compare(expected: Any, actual: Any, path: str) -> None:
    if type(expected) is not type(actual):
        raise ProjectionParityError(
            f"{path}: type {type(actual).__name__} != {type(expected).__name__}")
    if isinstance(expected, dict):
        if set(expected) != set(actual):
            missing = sorted(set(expected) - set(actual))
            extra = sorted(set(actual) - set(expected))
            raise ProjectionParityError(f"{path}: keys missing={missing} extra={extra}")
        for key in expected:
            _compare(expected[key], actual[key], f"{path}.{key}" if path else key)
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            raise ProjectionParityError(
                f"{path}: length {len(actual)} != {len(expected)}")
        for index, value in enumerate(expected):
            _compare(value, actual[index], f"{path}[{index}]")
    elif expected != actual:
        raise ProjectionParityError(f"{path}: {actual!r} != {expected!r}")


def assert_exact_projection_parity(expected: dict[str, Any],
                                   actual: dict[str, Any]) -> None:
    _compare(expected, actual, "")


def _json(path: Path) -> Any:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    compare = commands.add_parser("compare")
    compare.add_argument("--expected", required=True, type=Path)
    compare.add_argument("--native", required=True, type=Path)
    verify = commands.add_parser("verify-worlds")
    verify.add_argument("--zone-root", required=True, type=Path)
    verify.add_argument("--retail-root", required=True, type=Path)
    verify.add_argument("--mover-root", required=True, type=Path)
    verify.add_argument("--native-world-root", required=True, type=Path)
    verify.add_argument("--maps", required=True, nargs="+")
    exact = commands.add_parser("verify-exact")
    exact.add_argument("--zone-root", required=True, type=Path)
    exact.add_argument("--retail-root", required=True, type=Path)
    exact.add_argument("--mover-root", required=True, type=Path)
    exact.add_argument("--script-root", required=True, type=Path)
    exact.add_argument("--t3d-root", required=True, type=Path)
    exact.add_argument("--native-world-root", required=True, type=Path)
    exact.add_argument("--native-class-defaults", required=True, type=Path)
    exact.add_argument("--maps", required=True, nargs="+")
    args = parser.parse_args(argv)
    try:
        if args.command == "compare":
            assert_exact_projection_parity(_json(args.expected), _json(args.native))
        elif args.command == "verify-worlds":
            expected = collect_exact_zone_oracle(
                args.zone_root, args.retail_root, args.maps)
            movers = collect_exact_mover_oracle(
                args.mover_root, args.maps, args.retail_root)
            for map_name in args.maps:
                expected[map_name]["mover_models"] = movers[map_name]
            actual = collect_native_world_snapshot(args.native_world_root, args.maps)
            assert_exact_projection_parity(expected, actual)
        else:
            expected = build_exact_projection_oracle(
                args.zone_root, args.retail_root, args.mover_root,
                args.script_root, args.t3d_root, args.maps)
            battle_maps = tuple(
                name for name in args.maps if name.startswith("Battle"))
            native_battle = collect_native_battle_snapshot(
                args.native_world_root, args.native_class_defaults, battle_maps)
            actual = {
                "format": "runtime-map-projection-exact-v1",
                "maps": collect_native_world_snapshot(
                    args.native_world_root, args.maps),
                "class_struct_round_trips": class_struct_roundtrip_oracle(
                    native_battle),
                "battle": native_battle,
            }
            assert_exact_projection_parity(expected, actual)
    except (OSError, ValueError, ProjectionParityError) as exc:
        print(f"runtime-map exact parity FAILED: {exc}", file=sys.stderr)
        return 1
    print("runtime-map exact parity OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
