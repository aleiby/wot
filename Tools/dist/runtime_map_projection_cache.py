#!/usr/bin/env python3
"""Strict Python twin for runtime-map world-v3/v3 and BSP-v4 records.

World and class-default records live here. BSP decoding delegates to the
canonical v1-v4 converter parser in ``Tools.convert`` so fixture generation,
exact parity, and developer conversion share one byte-level implementation.
"""
from __future__ import annotations

import json
import math
import struct
import sys
from pathlib import Path
from typing import Any, Iterable


_CONVERT_DIR = Path(__file__).resolve().parents[1] / "convert"
if str(_CONVERT_DIR) not in sys.path:
    sys.path.insert(0, str(_CONVERT_DIR))
from bsp_payload import (  # noqa: E402  pylint: disable=wrong-import-position
    BSPPayloadError as _CanonicalBSPPayloadError,
    parse_bsp_payload as _parse_canonical_bsp_payload,
)


class CacheFormatError(ValueError):
    """The projected cache record is malformed or violates a wire bound."""


_BSP_HEADER = struct.Struct("<8sHHIIIHH6f")
_BSP_SECTION = struct.Struct("<HHIIIIB3s")
_BSP_IDENTITY = struct.Struct("<HHHHI")
_WORLD_HEADER = struct.Struct("<8sHH13II")
_SURFACE = struct.Struct("<iiiiiiIhhf")
_NODE = struct.Struct("<4fQB7i3B2i")
_MAX_STRING = 1024 * 1024
_MAX_PROPERTIES = 4_000_000
_MAX_BSP_VERTICES = 2_000_000
_MAX_BSP_INDICES = 6_000_000
_MAX_BSP_SECTIONS = 4096
_MAX_BSP_TEXT = 1024
_MAX_BSP_DIMENSION = 8192
_MAX_BSP_PIXELS = 4096 * 8192
_MAX_ENTRY_PIXELS = 4 * 1024 * 1024
_MAX_COORDINATE = 10_000_000.0
_SUPPORTED_TYPES = {1, 2, 3, 4, 5, 6, 10, 13}
_NULL_SEMANTIC = 0xFFFFFFFF


def _finite(value: Any, label: str) -> float:
    result = float(value)
    if not math.isfinite(result):
        raise CacheFormatError(f"non-finite {label}")
    # The native writer's WoTWorldFloatBits canonicalizes IEEE -0.0.  Keep the
    # strict Python twin byte-identical at every float serialization boundary.
    return 0.0 if result == 0.0 else result


def _vector(values: Any, size: int, label: str) -> list[float]:
    if not isinstance(values, (list, tuple)) or len(values) != size:
        raise CacheFormatError(f"invalid {label}")
    return [_finite(value, label) for value in values]


def _bytes(value: Any, label: str) -> bytes:
    try:
        return bytes(value)
    except (TypeError, ValueError) as exc:
        raise CacheFormatError(f"invalid {label}") from exc


def _text(value: Any, label: str, *, optional: bool = True) -> str:
    if not isinstance(value, str) or (not optional and not value):
        raise CacheFormatError(f"invalid {label}")
    encoded = value.encode("utf-8")
    if len(encoded) > _MAX_STRING or b"\0" in encoded:
        raise CacheFormatError(f"unsafe {label}")
    return value


def _pack_string(value: Any, label: str, *, optional: bool = True) -> bytes:
    text = _text(value, label, optional=optional)
    encoded = text.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded


def _short_text(value: Any, label: str, *, optional: bool = False) -> str:
    result = _text(value, label, optional=optional)
    if len(result.encode("utf-8")) > _MAX_BSP_TEXT or any(
            ord(character) < 32 or ord(character) == 127
            for character in result):
        raise CacheFormatError(f"unsafe {label}")
    return result


class _Cursor:
    def __init__(self, payload: bytes | bytearray | memoryview):
        self.data = memoryview(payload)
        self.offset = 0

    def take(self, size: int, label: str) -> memoryview:
        if size < 0 or self.offset + size > len(self.data):
            raise CacheFormatError(f"truncated {label}")
        result = self.data[self.offset:self.offset + size]
        self.offset += size
        return result

    def unpack(self, spelling: str, label: str) -> tuple:
        fmt = struct.Struct(spelling)
        return fmt.unpack(self.take(fmt.size, label))

    def string(self, label: str, *, optional: bool = True) -> str:
        (size,) = self.unpack("<I", f"{label} length")
        if size > _MAX_STRING:
            raise CacheFormatError(f"oversized {label}")
        raw = bytes(self.take(size, label))
        try:
            value = raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise CacheFormatError(f"invalid UTF-8 {label}") from exc
        if value.encode("utf-8") != raw or "\0" in value or (not optional and not value):
            raise CacheFormatError(f"invalid {label}")
        return value

    def end(self, label: str) -> None:
        if self.offset != len(self.data):
            raise CacheFormatError(f"trailing {label} bytes")


def _validate_sections(sections: Any, vertex_count: int, index_count: int,
                       indices: list[int] | None = None,
                       *, expected_collision: bool | None = None,
                       short_text: bool = False,
                       allow_mover_null: bool = False) -> None:
    if not isinstance(sections, list) or not sections \
            or len(sections) > _MAX_BSP_SECTIONS:
        raise CacheFormatError("invalid section count")
    expected_first = 0
    identity_by_slot: dict[str, tuple[str, str, str, int, str]] = {}
    for section in sections:
        text = _short_text if short_text else _text
        slot = text(section.get("material_slot"), "material slot", optional=False)
        identity = (
            text(section.get("canonical_package", ""), "canonical package", optional=True),
            text(section.get("canonical_name", ""), "canonical name", optional=True),
            text(section.get("source_texture_identity", ""), "source identity", optional=True),
            int(section.get("surface_flags", -1)),
            text(section.get("pan_zone_actor_identity", ""), "pan-zone identity", optional=True),
        )
        prior = identity_by_slot.get(slot)
        if prior is not None:
            raise CacheFormatError("each material slot must have exactly one identity/flag family")
        identity_by_slot[slot] = identity
        if not 0 <= identity[3] <= 0xFFFFFFFF:
            raise CacheFormatError("surface flags outside uint32")
        static_null = not allow_mover_null and slot == "__None"
        mover_null = allow_mover_null and slot.startswith("__None__MP")
        if mover_null and (not identity[1] or not identity[2]):
            raise CacheFormatError("mover null section identity is incomplete")
        if not static_null and not mover_null and not all(identity[:3]):
            raise CacheFormatError("non-null section identity is incomplete")
        first = int(section.get("first_index", -1))
        count = int(section.get("index_count", -1))
        minimum = int(section.get("min_vertex", -1))
        maximum = int(section.get("max_vertex", -1))
        collision = section.get("collision_enabled")
        if not isinstance(collision, bool) or (expected_collision is not None
                                               and collision != expected_collision):
            raise CacheFormatError("invalid section collision role")
        if (first != expected_first or count <= 0 or count % 3
                or first + count > index_count or minimum < 0
                or maximum < minimum or maximum >= vertex_count):
            raise CacheFormatError("invalid section range")
        if indices is not None:
            subset = indices[first:first + count]
            if not subset or min(subset) != minimum or max(subset) != maximum:
                raise CacheFormatError("section min/max disagrees with indices")
        expected_first += count
    if expected_first != index_count:
        raise CacheFormatError("sections do not cover indices")


def encode_bsp_v4(geometry: dict[str, Any]) -> bytes:
    positions = [_finite(value, "position") for value in geometry.get("positions", [])]
    normals = [_finite(value, "normal") for value in geometry.get("normals", [])]
    uv0 = [_finite(value, "UV0") for value in geometry.get("uv0", [])]
    uv1 = [_finite(value, "UV1") for value in geometry.get("uv1", [])]
    if len(positions) == 0 or len(positions) % 3 \
            or len(positions) // 3 > _MAX_BSP_VERTICES:
        raise CacheFormatError("invalid position stream")
    vertex_count = len(positions) // 3
    if len(normals) != vertex_count * 3 or len(uv0) != vertex_count * 2 \
            or len(uv1) != vertex_count * 2:
        raise CacheFormatError("BSP vertex streams disagree")
    for at in range(0, len(normals), 3):
        length = sum(value * value for value in normals[at:at + 3])
        if abs(length - 1.0) > 0.02:
            raise CacheFormatError("non-unit normal")
    indices = [int(value) for value in geometry.get("indices", [])]
    if not indices or len(indices) > _MAX_BSP_INDICES or len(indices) % 3 \
            or any(value < 0 or value >= vertex_count for value in indices):
        raise CacheFormatError("invalid BSP indices")
    role = geometry.get("role")
    if role not in {"main", "portal"}:
        raise CacheFormatError("invalid BSP role")
    sections = geometry.get("sections")
    _validate_sections(sections, vertex_count, len(indices), indices,
                       expected_collision=role == "main", short_text=True)
    bounds_min = _vector(geometry.get("bounds_min"), 3, "bounds minimum")
    bounds_max = _vector(geometry.get("bounds_max"), 3, "bounds maximum")
    computed_min = [min(positions[axis::3]) for axis in range(3)]
    computed_max = [max(positions[axis::3]) for axis in range(3)]
    if bounds_min != computed_min or bounds_max != computed_max:
        raise CacheFormatError("BSP bounds disagree with positions")
    if any(abs(value) > _MAX_COORDINATE for value in bounds_min + bounds_max):
        raise CacheFormatError("BSP coordinate exceeds bound")
    lightmap = _bytes(geometry.get("lightmap_bgra", b""), "lightmap")
    trailer = _bytes(geometry.get("entry_trailer", b""), "entry trailer")
    if trailer and not lightmap:
        raise CacheFormatError("entry trailer requires a lightmap")
    flags = 2 | (1 if role == "portal" else 0)
    if lightmap:
        flags |= 4
    if trailer:
        flags |= 8
    payload = bytearray(_BSP_HEADER.pack(
        b"WOTPBSP\0", 4, flags, vertex_count, len(indices), len(sections),
        2, 1, *(bounds_min + bounds_max)))
    for section in sections:
        slot = section["material_slot"].encode("utf-8")
        identity_values = [section.get(name, "").encode("utf-8") for name in (
            "canonical_package", "canonical_name", "source_texture_identity",
            "pan_zone_actor_identity")]
        if any(len(value) > _MAX_BSP_TEXT for value in [slot, *identity_values]):
            raise CacheFormatError("section identity exceeds native bound")
        payload += _BSP_SECTION.pack(
            len(slot), 0, section["first_index"], section["index_count"],
            section["min_vertex"], section["max_vertex"],
            int(section["collision_enabled"]), b"\0\0\0")
        payload += slot
        payload += _BSP_IDENTITY.pack(
            *(len(value) for value in identity_values), section["surface_flags"])
        for value in identity_values:
            payload += value
    for stream in (positions, normals, uv0, uv1):
        payload += struct.pack(f"<{len(stream)}f", *stream)
    payload += struct.pack(f"<{len(indices)}I", *indices)
    if lightmap:
        width = int(geometry.get("lightmap_width", 0))
        height = int(geometry.get("lightmap_height", 0))
        pixels = width * height
        if width <= 0 or width > _MAX_BSP_DIMENSION \
                or height <= 0 or height > _MAX_BSP_DIMENSION \
                or pixels > _MAX_BSP_PIXELS \
                or trailer and pixels > _MAX_ENTRY_PIXELS \
                or len(lightmap) != pixels * 4 \
                or any(lightmap[index] != 255 for index in range(3, len(lightmap), 4)):
            raise CacheFormatError("invalid lightmap dimensions")
        payload += struct.pack("<III", width, height, len(lightmap)) + lightmap
    payload += trailer
    return bytes(payload)


def parse_bsp_v4(payload: bytes | bytearray | memoryview) -> dict[str, Any]:
    try:
        result = _parse_canonical_bsp_payload(bytes(payload))
    except _CanonicalBSPPayloadError as exc:
        raise CacheFormatError(str(exc)) from exc
    # Version 10 is the current exact-macro multiplayer format and requires
    # triangle provenance. Legacy fixture formats 4-6 remain readable; the
    # superseded production formats 7-9 are intentionally stale.
    if result["version"] not in (4, 5, 6, 10):
        raise CacheFormatError("BSP payload is not version 4")
    return result


def _property_defaults(value: dict[str, Any]) -> dict[str, Any]:
    return {
        "name": value.get("name", ""), "type": value.get("type", 0),
        "array_index": value.get("array_index", 0),
        "struct_name": value.get("struct_name", ""),
        "name_value": value.get("name_value", ""),
        "object_identity": value.get("object_identity", ""),
        "object_class_identity": value.get("object_class_identity", ""),
        "value": value.get("value", b""), "members": value.get("members", []),
    }


def _validate_resolved_struct(
        prop: dict[str, Any], *, world_resolved_schema: bool) -> None:
    if int(prop["type"]) != 10:
        return
    members = prop["members"]
    if not isinstance(members, list) or any(
            not isinstance(member, dict) for member in members):
        raise CacheFormatError("resolved struct members are not records")
    if not world_resolved_schema:
        return
    schemas = {
        "resource": (("ResourcePackageName", 6, None),
                     ("ResourceAdapterName", 6, None),
                     ("ItemCount", 2, 4)),
        "soundevent": (("Delay", 4, 4), ("Event", 6, None),
                       ("SoundSourceTag", 6, None), ("Sound", 5, None),
                       ("SoundTableTag", 6, None),
                       ("SoundTableSlot", 6, None)),
    }
    expected = schemas.get(str(prop["struct_name"]).casefold())
    if expected is None:
        if members:
            raise CacheFormatError("unsupported resolved struct-member schema")
        return
    actual = tuple((member.get("name"), int(member.get("type", 0)),
                    len(_bytes(member.get("value", b""), "member value")))
                   for member in members)
    for_schema = tuple((name, kind, byte_count if byte_count is not None else 0)
                       for name, kind, byte_count in expected)
    if actual != for_schema or any(int(member.get("array_index", 0)) != 0
                                   for member in members):
        raise CacheFormatError(
            f"{prop['struct_name']} member schema is not canonical")


def _encode_property(value: dict[str, Any], depth: int = 0,
                     descendant_budget: list[int] | None = None,
                     descendant: bool = False, *,
                     world_resolved_schema: bool = False) -> bytes:
    if depth > 8:
        raise CacheFormatError("property member nesting too deep")
    if descendant_budget is None:
        descendant_budget = [4096]
    if descendant:
        descendant_budget[0] -= 1
        if descendant_budget[0] < 0:
            raise CacheFormatError("property descendant-member budget exceeded")
    prop = _property_defaults(value)
    _text(prop["name"], "property name", optional=False)
    prop_type = int(prop["type"])
    if prop_type not in _SUPPORTED_TYPES or not 0 <= int(prop["array_index"]) <= 0xFFFFFFFF:
        raise CacheFormatError("unsupported property type/index")
    raw = _bytes(prop["value"], "property value")
    scalar_sizes = {1: 1, 2: 4, 3: 1, 4: 4}
    if prop_type in scalar_sizes and len(raw) != scalar_sizes[prop_type]:
        raise CacheFormatError("scalar property byte count is invalid")
    if prop_type == 3 and raw[0] > 1:
        raise CacheFormatError("boolean scalar value is invalid")
    members = prop["members"]
    if not isinstance(members, list) or len(members) > _MAX_PROPERTIES:
        raise CacheFormatError("invalid property member count")
    if prop_type != 10 and members:
        raise CacheFormatError("only struct properties may have members")
    if prop_type == 10 and not prop["struct_name"]:
        raise CacheFormatError("struct property lacks a struct name")
    if prop_type != 10 and prop["struct_name"]:
        raise CacheFormatError("non-struct property has a struct name")
    if prop_type in (5, 6) and raw:
        raise CacheFormatError("resolved name/object property retains package bytes")
    if prop_type == 6 and not prop["name_value"]:
        raise CacheFormatError("name property lacks resolved value")
    if prop_type != 6 and prop["name_value"]:
        raise CacheFormatError("non-name property has a resolved name value")
    if prop_type == 5 and bool(prop["object_identity"]) != bool(prop["object_class_identity"]):
        raise CacheFormatError("object binding is incomplete")
    if prop_type != 5 and (prop["object_identity"] or prop["object_class_identity"]):
        raise CacheFormatError("non-object property has object binding")
    member_keys = [(str(member.get("name", "")).casefold(),
                    int(member.get("array_index", 0))) for member in members]
    if len(member_keys) != len(set(member_keys)):
        raise CacheFormatError("duplicate struct-member property key")
    _validate_resolved_struct(
        prop, world_resolved_schema=world_resolved_schema)
    result = bytearray()
    result += _pack_string(prop["name"], "property name", optional=False)
    result += struct.pack("<BI", prop_type, int(prop["array_index"]))
    for name in ("struct_name", "name_value", "object_identity", "object_class_identity"):
        result += _pack_string(prop[name], name)
    result += struct.pack("<I", len(raw)) + raw + struct.pack("<I", len(members))
    for member in members:
        result += _encode_property(
            member, depth + 1, descendant_budget, descendant=True,
            world_resolved_schema=world_resolved_schema)
    return bytes(result)


def _decode_property(cursor: _Cursor, depth: int = 0,
                     descendant_budget: list[int] | None = None,
                     descendant: bool = False, *,
                     world_resolved_schema: bool = False) -> dict[str, Any]:
    if depth > 8:
        raise CacheFormatError("property member nesting too deep")
    if descendant_budget is None:
        descendant_budget = [4096]
    if descendant:
        descendant_budget[0] -= 1
        if descendant_budget[0] < 0:
            raise CacheFormatError("property descendant-member budget exceeded")
    name = cursor.string("property name", optional=False)
    prop_type, array_index = cursor.unpack("<BI", "property type/index")
    prop = {
        "name": name, "type": prop_type, "array_index": array_index,
        "struct_name": cursor.string("struct name"),
        "name_value": cursor.string("resolved name"),
        "object_identity": cursor.string("object identity"),
        "object_class_identity": cursor.string("object class"),
    }
    (size,) = cursor.unpack("<I", "property byte count")
    if size > 256 * 1024 * 1024:
        raise CacheFormatError("property value over bound")
    prop["value"] = bytes(cursor.take(size, "property value"))
    (member_count,) = cursor.unpack("<I", "property member count")
    if member_count > _MAX_PROPERTIES:
        raise CacheFormatError("property member count over bound")
    prop["members"] = [
        _decode_property(
            cursor, depth + 1, descendant_budget, descendant=True,
            world_resolved_schema=world_resolved_schema)
        for _ in range(member_count)]
    # Reuse the writer's strict shape checks without changing the decoded data.
    _encode_property(
        prop, depth, world_resolved_schema=world_resolved_schema)
    return prop


def _pack_properties(
        properties: Any, *, world_resolved_schema: bool = False) -> bytes:
    if not isinstance(properties, list) or len(properties) > _MAX_PROPERTIES:
        raise CacheFormatError("invalid property list")
    keys = [(item.get("name", "").lower(), int(item.get("array_index", 0)),
             item.get("name", "")) for item in properties]
    if keys != sorted(keys) or any(
            keys[index] <= keys[index - 1]
            for index in range(1, len(keys))):
        raise CacheFormatError("properties are not in canonical order")
    return struct.pack("<I", len(properties)) + b"".join(
        _encode_property(
            item, world_resolved_schema=world_resolved_schema)
        for item in properties)


def _read_properties(
        cursor: _Cursor, *, world_resolved_schema: bool = False) -> list[dict[str, Any]]:
    (count,) = cursor.unpack("<I", "property count")
    if count > _MAX_PROPERTIES:
        raise CacheFormatError("property count over bound")
    result = [
        _decode_property(cursor, world_resolved_schema=world_resolved_schema)
        for _ in range(count)]
    keys = [(item["name"].lower(), item["array_index"], item["name"])
            for item in result]
    if keys != sorted(keys) or any(
            keys[index] <= keys[index - 1]
            for index in range(1, len(keys))):
        raise CacheFormatError("property canonical order violated")
    return result


def parse_class_defaults_v3(
        payload: bytes | bytearray | memoryview) -> list[dict[str, Any]]:
    """Decode the reflection-complete WOTDEF3 v3 payload strictly."""
    cursor = _Cursor(payload)
    magic = bytes(cursor.take(8, "class-default magic"))
    version, flags, count = cursor.unpack("<HHI", "class-default header")
    if magic != b"WOTDEF3\0" or version != 3 or flags != 0 \
            or count > _MAX_PROPERTIES:
        raise CacheFormatError("invalid WOTDEF3-v3 header")
    result = []
    identities = set()
    for index in range(count):
        identity = cursor.string("class-default identity", optional=False)
        (export_index,) = cursor.unpack("<I", "class-default export index")
        superclass = cursor.string("class-default superclass")
        (placeholder,) = cursor.unpack("<B", "class-default placeholder")
        if export_index == 0 or placeholder > 1:
            raise CacheFormatError(
                f"class-default record {index} is malformed")
        folded = identity.casefold()
        if folded in identities:
            raise CacheFormatError("duplicate class-default identity")
        identities.add(folded)
        result.append({
            "identity": identity,
            "export_index": export_index,
            "superclass_identity": superclass,
            "placeholder": bool(placeholder),
            "properties": _read_properties(
                cursor, world_resolved_schema=False),
        })
    cursor.end("class-default payload")
    return result


def _counts(document: dict[str, Any]) -> tuple[int, ...]:
    actors = document.get("actors", [])
    return (
        len(actors), sum(bool(item.get("parent_identity")) for item in actors),
        len(document.get("vectors", [])), len(document.get("points", [])),
        len(document.get("nodes", [])), len(document.get("surfaces", [])),
        len(document.get("vertices", [])), len(document.get("zones", [])),
        len(document.get("semantic_objects", [])), len(document.get("reach_specs", [])),
        len(document.get("leaves", [])), len(document.get("model_lights", [])),
        len(document.get("mover_models", [])),
    )


def _validate_world_counts(counts: tuple[int, ...]) -> None:
    limits = (1_000_000, 1_000_000, 2_000_000, 2_000_000,
              2_000_000, 2_000_000, 6_000_000, 64,
              4_000_000, 10_000_000, 2_000_000, 4_000_000,
              1_000_000)
    if len(counts) != len(limits) or any(
            count < 0 or count > limit
            for count, limit in zip(counts, limits)) \
            or counts[12] > counts[0]:
        raise CacheFormatError("world table count over native bound")


def _validate_world_semantics(document: dict[str, Any], counts: tuple[int, ...]) -> None:
    semantic_rows = document.get("semantic_objects", [])
    semantic_by_identity: dict[str, dict[str, Any]] = {}
    for row in semantic_rows:
        identity = _text(row.get("identity", ""), "semantic identity", optional=False)
        _text(row.get("class_identity", ""), "semantic class", optional=False)
        folded = identity.casefold()
        if folded in semantic_by_identity:
            raise CacheFormatError("duplicate semantic identity")
        semantic_by_identity[folded] = row
    lights = document.get("model_lights", [])
    for value in lights:
        if value is None:
            continue
        index = int(value)
        if not 0 <= index < len(semantic_rows):
            raise CacheFormatError("model light semantic index outside registry")
        if not bool(semantic_rows[index].get("actor")):
            raise CacheFormatError("model light is not an actor semantic object")
    reaches_sentinel = [False] * len(lights)
    has_sentinel = False
    for index in range(len(lights) - 1, -1, -1):
        has_sentinel = has_sentinel or lights[index] is None
        reaches_sentinel[index] = has_sentinel
    for leaf in document.get("leaves", []):
        zone = int(leaf.get("zone", -1))
        if not 0 <= zone < 64:
            raise CacheFormatError("leaf zone is outside the fixed table")
        for field in ("permeating", "volumetric"):
            start = int(leaf.get(field, -1))
            if start == -1:
                continue
            if not 0 <= start < len(lights):
                raise CacheFormatError("leaf light-run offset outside table")
            if not reaches_sentinel[start]:
                raise CacheFormatError("leaf light run lacks null sentinel")
    leaf_count = len(document.get("leaves", []))
    for node in document.get("nodes", []):
        leaves = node.get("leaves", [-1, -1])
        if not isinstance(leaves, (list, tuple)) or len(leaves) != 2 or any(
                int(value) < -1 or int(value) >= leaf_count
                for value in leaves):
            raise CacheFormatError("node leaf index is outside decoded leaves")
    actors = document.get("actors", [])
    if sum(bool(row.get("actor")) for row in semantic_rows) != len(actors):
        raise CacheFormatError("semantic actor census disagrees with actors")
    actor_identities: set[str] = set()
    actor_index_by_identity: dict[str, int] = {}
    for actor_index, actor in enumerate(actors):
        identity = _text(actor.get("identity", ""), "actor identity", optional=False)
        folded = identity.casefold()
        semantic = semantic_by_identity.get(folded)
        if folded in actor_identities or semantic is None or not semantic.get("actor") \
                or semantic.get("class_identity") != actor.get("class_identity"):
            raise CacheFormatError("actor/semantic identity binding is malformed")
        actor_identities.add(folded)
        actor_index_by_identity[folded] = actor_index
        pending = list(actor.get("properties", []))
        while pending:
            prop = pending.pop()
            pending.extend(prop.get("members", []))
            object_identity = str(prop.get("object_identity", ""))
            if not object_identity:
                continue
            semantic_object = semantic_by_identity.get(object_identity.casefold())
            if semantic_object is None or semantic_object.get("class_identity") != \
                    prop.get("object_class_identity"):
                raise CacheFormatError(
                    "property object binding is outside the semantic registry")
    parents = [-1] * len(actors)
    for actor_index, actor in enumerate(actors):
        parent = str(actor.get("parent_identity", "")).casefold()
        if not parent:
            continue
        parent_semantic = semantic_by_identity.get(parent)
        if parent_semantic is None or not parent_semantic.get("actor") \
                or parent not in actor_index_by_identity:
            raise CacheFormatError("attachment parent is not an actor identity")
        parents[actor_index] = actor_index_by_identity[parent]
    states = [0] * len(actors)
    for start in range(len(actors)):
        current = start
        chain = []
        while current >= 0:
            if states[current] == 1:
                raise CacheFormatError("attachment cycle")
            if states[current] == 2:
                break
            states[current] = 1
            chain.append(current)
            current = parents[current]
        for actor_index in chain:
            states[actor_index] = 2

    nodes = document.get("nodes", [])
    vertices = document.get("vertices", [])
    surfaces = document.get("surfaces", [])
    points = document.get("points", [])
    vectors = document.get("vectors", [])
    reachable_vertices = [False] * len(vertices)
    for node in nodes:
        vertex_pool = int(node.get("vertex_pool", -1))
        vertex_count = int(node.get("vertex_count", 0))
        surface = int(node.get("surface", -1))
        optional_nodes = [int(node.get(name, -1))
                          for name in ("back", "front", "plane_node")]
        zones = node.get("zones", [])
        if vertex_pool < 0 or vertex_count < 0 \
                or vertex_pool + vertex_count > len(vertices) \
                or not 0 <= surface < len(surfaces) \
                or any(value < -1 or value >= len(nodes)
                       for value in optional_nodes) \
                or int(node.get("collision_bound", -1)) < -1 \
                or int(node.get("render_bound", -1)) < -1 \
                or not isinstance(zones, (list, tuple)) or len(zones) != 2 \
                or any(int(zone) < 0 or int(zone) >= 64 for zone in zones):
            raise CacheFormatError("node reference is outside decoded tables")
        for vertex_index in range(vertex_pool, vertex_pool + vertex_count):
            reachable_vertices[vertex_index] = True
    for surface in surfaces:
        texture_identity = str(surface.get("texture_identity", "")).casefold()
        actor_identity = str(surface.get("actor_identity", "")).casefold()
        texture = semantic_by_identity.get(texture_identity) if texture_identity else None
        actor = semantic_by_identity.get(actor_identity) if actor_identity else None
        if not 0 <= int(surface.get("base", -1)) < len(points) \
                or any(not 0 <= int(surface.get(name, -1)) < len(vectors)
                       for name in ("normal", "texture_u", "texture_v")) \
                or int(surface.get("light_map", -1)) < -1 \
                or int(surface.get("brush_poly", -1)) < -1 \
                or texture_identity and (texture is None or texture.get("actor")) \
                or actor_identity and (actor is None or not actor.get("actor")):
            raise CacheFormatError("surface reference is outside decoded tables")
    for vertex_index, vertex in enumerate(vertices):
        if int(vertex.get("side", -1)) < -1 \
                or reachable_vertices[vertex_index] and not (
                    0 <= int(vertex.get("point", -1)) < len(points)):
            raise CacheFormatError("vertex reference is outside decoded tables")
    for zone in document.get("zones", []):
        identity = str(zone.get("actor_identity", "")).casefold()
        semantic = semantic_by_identity.get(identity) if identity else None
        if identity and (semantic is None or not semantic.get("actor")):
            raise CacheFormatError("zone actor is outside actor registry")
    for reach in document.get("reach_specs", []):
        for name in ("start_identity", "end_identity"):
            identity = str(reach.get(name, "")).casefold()
            semantic = semantic_by_identity.get(identity) if identity else None
            if identity and (semantic is None or not semantic.get("actor")):
                raise CacheFormatError("reach-spec endpoint is not an actor identity")
    seen_mover_actors: set[str] = set()
    seen_mover_models: set[str] = set()
    for mover in document.get("mover_models", []):
        actor = str(mover.get("actor_identity", "")).casefold()
        model_identity = str(mover.get("model_identity", "")).casefold()
        model = semantic_by_identity.get(model_identity)
        if actor not in actor_identities or actor in seen_mover_actors \
                or model_identity in seen_mover_models or model is None \
                or model.get("actor") \
                or str(model.get("class_identity", "")).casefold() != "engine.model":
            raise CacheFormatError("mover model semantic binding is malformed")
        actor_row = next(row for row in actors
                         if str(row.get("identity", "")).casefold() == actor)
        brushes = [prop for prop in actor_row.get("properties", [])
                   if str(prop.get("name", "")).casefold() == "brush"
                   and int(prop.get("array_index", 0)) == 0]
        if len(brushes) != 1 or int(brushes[0].get("type", 0)) != 5 \
                or str(brushes[0].get("object_identity", "")).casefold() != model_identity \
                or str(brushes[0].get("object_class_identity", "")).casefold() != "engine.model":
            raise CacheFormatError("mover actor Brush property does not match mover model")
        seen_mover_actors.add(actor)
        seen_mover_models.add(model_identity)
    if counts[1] != sum(bool(actor.get("parent_identity")) for actor in actors):
        raise CacheFormatError("attachment census disagrees with actors")


def encode_world_v3(document: dict[str, Any]) -> bytes:
    counts = _counts(document)
    _validate_world_counts(counts)
    _validate_world_semantics(document, counts)
    payload = bytearray(_WORLD_HEADER.pack(
        b"WOTWRL3\0", 3, 0, *counts, int(bool(document.get("root_outside")))))
    for table_name in ("vectors", "points"):
        for row in document.get(table_name, []):
            payload += struct.pack("<3f", *_vector(row, 3, table_name))
    for row in document.get("nodes", []):
        plane = _vector(row.get("plane"), 4, "node plane")
        payload += _NODE.pack(
            *plane, int(row.get("zone_mask", 0)), int(row.get("flags", 0)),
            *(int(row.get(key, -1)) for key in (
                "vertex_pool", "surface", "back", "front", "plane_node",
                "collision_bound", "render_bound")),
            int(row.get("zones", [0, 0])[0]), int(row.get("zones", [0, 0])[1]),
            int(row.get("vertex_count", 0)),
            int(row.get("leaves", [-1, -1])[0]), int(row.get("leaves", [-1, -1])[1]))
    for row in document.get("surfaces", []):
        friction = _finite(row.get("friction", 1.0), "surface friction")
        payload += _SURFACE.pack(
            *(int(row.get(key, -1)) for key in (
                "base", "normal", "texture_u", "texture_v", "light_map", "brush_poly")),
            int(row.get("flags", 0)), int(row.get("pan_u", 0)),
            int(row.get("pan_v", 0)), friction)
        payload += _pack_string(row.get("texture_identity", ""), "surface texture")
        payload += _pack_string(row.get("actor_identity", ""), "surface actor")
    for row in document.get("vertices", []):
        payload += struct.pack("<ii", int(row.get("point", -1)), int(row.get("side", -1)))
    for row in document.get("zones", []):
        payload += _pack_string(row.get("actor_identity", ""), "zone actor")
        payload += struct.pack("<QQ", int(row.get("connectivity", 0)),
                               int(row.get("visibility", 0)))
    for row in document.get("leaves", []):
        payload += struct.pack("<iii", int(row.get("zone", -1)),
                               int(row.get("permeating", -1)),
                               int(row.get("volumetric", -1)))
    semantic_count = counts[8]
    for value in document.get("model_lights", []):
        index = _NULL_SEMANTIC if value is None else int(value)
        if index != _NULL_SEMANTIC and not 0 <= index < semantic_count:
            raise CacheFormatError("model light semantic index outside registry")
        payload += struct.pack("<I", index)
    for row in document.get("semantic_objects", []):
        payload += _pack_string(row.get("identity", ""), "semantic identity", optional=False)
        payload += _pack_string(row.get("class_identity", ""), "semantic class", optional=False)
        payload += struct.pack("<B", int(bool(row.get("actor"))))
    for row in document.get("actors", []):
        payload += _pack_string(row.get("identity", ""), "actor identity", optional=False)
        payload += _pack_string(row.get("class_identity", ""), "actor class", optional=False)
        payload += _pack_properties(
            row.get("properties", []), world_resolved_schema=True)
        payload += _pack_string(row.get("parent_identity", ""), "parent identity")
    for row in document.get("reach_specs", []):
        payload += struct.pack("<iiiIB", int(row.get("distance", 0)),
                               int(row.get("collision_radius", 0)),
                               int(row.get("collision_height", 0)),
                               int(row.get("reach_flags", 0)),
                               int(bool(row.get("pruned"))))
        payload += _pack_string(row.get("start_identity", ""), "reach start")
        payload += _pack_string(row.get("end_identity", ""), "reach end")
    for mover in document.get("mover_models", []):
        positions = [_vector(item, 3, "mover position") for item in mover.get("positions", [])]
        normals = [_vector(item, 3, "mover normal") for item in mover.get("normals", [])]
        uv0 = [_vector(item, 2, "mover UV0") for item in mover.get("uv0", [])]
        indices = [int(value) for value in mover.get("indices", [])]
        if not positions or len(normals) != len(positions) or len(uv0) != len(positions) \
                or not indices or len(indices) % 3 or any(
                    value < 0 or value >= len(positions) for value in indices):
            raise CacheFormatError("invalid mover vertex/index streams")
        if len(positions) > _MAX_BSP_VERTICES or len(indices) > _MAX_BSP_INDICES \
                or any(abs(value) > _MAX_COORDINATE
                       for position in positions for value in position):
            raise CacheFormatError("mover position stream is outside native bound")
        if any(abs(sum(value * value for value in normal) - 1.0) > 0.02
               for normal in normals):
            raise CacheFormatError("mover normal stream is not unit")
        sections = mover.get("sections", [])
        _validate_sections(sections, len(positions), len(indices), indices,
                           allow_mover_null=True)
        payload += _pack_string(mover.get("actor_identity", ""), "mover actor", optional=False)
        payload += _pack_string(mover.get("model_identity", ""), "mover model", optional=False)
        payload += struct.pack("<III", len(sections), len(positions), len(indices))
        for section in sections:
            origin = _vector(section.get("plane_origin"), 3, "mover plane origin")
            normal = _vector(section.get("plane_normal"), 3, "mover plane normal")
            if abs(sum(value * value for value in normal) - 1.0) > 0.02:
                raise CacheFormatError("mover plane normal is not unit")
            first, count = int(section["first_index"]), int(section["index_count"])
            for vertex in {indices[at] for at in range(first, first + count)}:
                distance = sum((positions[vertex][axis] - origin[axis]) * normal[axis]
                               for axis in range(3))
                if abs(distance) > 0.05:
                    raise CacheFormatError("mover vertex is outside section plane")
            payload += struct.pack("<IIIIB3sI6f", first, count,
                                   int(section["min_vertex"]), int(section["max_vertex"]),
                                   int(section["collision_enabled"]), b"\0\0\0",
                                   int(section["surface_flags"]), *(origin + normal))
            mover_null = section.get("material_slot", "").startswith(
                "__None__MP")
            for key, label in (("material_slot", "mover slot"),
                               ("canonical_package", "mover canonical package"),
                               ("canonical_name", "mover canonical name"),
                               ("source_texture_identity", "mover source identity")):
                payload += _pack_string(section.get(key, ""), label,
                                        optional=(mover_null
                                                  and key == "canonical_package"))
        for rows, width in ((positions, 3), (normals, 3), (uv0, 2)):
            flat = [value for row in rows for value in row]
            payload += struct.pack(f"<{len(flat)}f", *flat)
        payload += struct.pack(f"<{len(indices)}I", *indices)
    return bytes(payload)


def parse_world_v3(payload: bytes | bytearray | memoryview) -> dict[str, Any]:
    cursor = _Cursor(payload)
    fields = _WORLD_HEADER.unpack(cursor.take(_WORLD_HEADER.size, "world header"))
    magic, version, flags = fields[:3]
    counts = fields[3:16]
    root_outside = fields[16]
    if magic != b"WOTWRL3\0" or version != 3 or flags != 0 or root_outside > 1:
        raise CacheFormatError("invalid world-v3 header")
    _validate_world_counts(counts)
    (actor_count, attachment_count, vector_count, point_count, node_count,
     surface_count, vertex_count, zone_count, semantic_count, reach_count,
     leaf_count, light_count, mover_count) = counts
    def vectors(count: int, width: int, label: str) -> list[list[float]]:
        result = []
        for _ in range(count):
            row = list(cursor.unpack(f"<{width}f", label))
            if not all(math.isfinite(value) for value in row):
                raise CacheFormatError(f"non-finite {label}")
            result.append(row)
        return result
    result: dict[str, Any] = {
        "vectors": vectors(vector_count, 3, "vectors"),
        "points": vectors(point_count, 3, "points"),
        "root_outside": bool(root_outside),
    }
    nodes = []
    for _ in range(node_count):
        row = _NODE.unpack(cursor.take(_NODE.size, "node"))
        if not all(math.isfinite(value) for value in row[:4]):
            raise CacheFormatError("non-finite node plane")
        nodes.append({
            "plane": list(row[:4]), "zone_mask": row[4], "flags": row[5],
            **dict(zip(("vertex_pool", "surface", "back", "front", "plane_node",
                        "collision_bound", "render_bound"), row[6:13])),
            "zones": list(row[13:15]), "vertex_count": row[15],
            "leaves": list(row[16:18]),
        })
    result["nodes"] = nodes
    surfaces = []
    for _ in range(surface_count):
        row = _SURFACE.unpack(cursor.take(_SURFACE.size, "surface"))
        if not math.isfinite(row[9]):
            raise CacheFormatError("non-finite surface friction")
        surfaces.append({
            **dict(zip(("base", "normal", "texture_u", "texture_v", "light_map",
                        "brush_poly", "flags", "pan_u", "pan_v", "friction"), row)),
            "texture_identity": cursor.string("surface texture"),
            "actor_identity": cursor.string("surface actor"),
        })
    result["surfaces"] = surfaces
    result["vertices"] = [dict(zip(("point", "side"), cursor.unpack("<ii", "vertex")))
                          for _ in range(vertex_count)]
    result["zones"] = [{"actor_identity": cursor.string("zone actor"),
                         **dict(zip(("connectivity", "visibility"),
                                    cursor.unpack("<QQ", "zone masks")))}
                        for _ in range(zone_count)]
    result["leaves"] = [dict(zip(("zone", "permeating", "volumetric"),
                                  cursor.unpack("<iii", "leaf")))
                        for _ in range(leaf_count)]
    model_lights = []
    for _ in range(light_count):
        (index,) = cursor.unpack("<I", "model light")
        if index != _NULL_SEMANTIC and index >= semantic_count:
            raise CacheFormatError("model light semantic index outside registry")
        model_lights.append(None if index == _NULL_SEMANTIC else index)
    result["model_lights"] = model_lights
    result["semantic_objects"] = []
    for _ in range(semantic_count):
        identity = cursor.string("semantic identity", optional=False)
        class_identity = cursor.string("semantic class", optional=False)
        (actor_flag,) = cursor.unpack("<B", "semantic actor flag")
        if actor_flag > 1:
            raise CacheFormatError("invalid semantic actor flag")
        result["semantic_objects"].append({
            "identity": identity, "class_identity": class_identity,
            "actor": bool(actor_flag),
        })
    actors = []
    for _ in range(actor_count):
        actors.append({
            "identity": cursor.string("actor identity", optional=False),
            "class_identity": cursor.string("actor class", optional=False),
            "properties": _read_properties(
                cursor, world_resolved_schema=True),
            "parent_identity": cursor.string("parent identity"),
        })
    if sum(bool(actor["parent_identity"]) for actor in actors) != attachment_count:
        raise CacheFormatError("world attachment count disagrees with actors")
    result["actors"] = actors
    result["reach_specs"] = []
    for _ in range(reach_count):
        distance, radius, height, reach_flags, pruned = cursor.unpack("<iiiIB", "reach spec")
        if pruned > 1:
            raise CacheFormatError("invalid ReachSpec pruned flag")
        result["reach_specs"].append({
            "distance": distance, "collision_radius": radius,
            "collision_height": height, "reach_flags": reach_flags,
            "pruned": bool(pruned), "start_identity": cursor.string("reach start"),
            "end_identity": cursor.string("reach end"),
        })
    movers = []
    for _ in range(mover_count):
        actor = cursor.string("mover actor", optional=False)
        model = cursor.string("mover model", optional=False)
        section_count, count, index_count = cursor.unpack("<III", "mover counts")
        if not 0 < section_count <= _MAX_BSP_SECTIONS \
                or not 0 < count <= _MAX_BSP_VERTICES \
                or not 0 < index_count <= _MAX_BSP_INDICES or index_count % 3:
            raise CacheFormatError("invalid mover counts")
        sections = []
        for _ in range(section_count):
            row = cursor.unpack("<IIIIB3sI6f", "mover section")
            if row[5] != b"\0\0\0":
                raise CacheFormatError("mover section reserved padding is nonzero")
            if row[4] > 1 or not all(math.isfinite(value) for value in row[7:]):
                raise CacheFormatError("invalid mover section")
            sections.append({
                "first_index": row[0], "index_count": row[1],
                "min_vertex": row[2], "max_vertex": row[3],
                "collision_enabled": bool(row[4]), "surface_flags": row[6],
                "plane_origin": list(row[7:10]), "plane_normal": list(row[10:13]),
                "material_slot": cursor.string("mover slot", optional=False),
                "canonical_package": cursor.string("mover canonical package"),
                "canonical_name": cursor.string("mover canonical name"),
                "source_texture_identity": cursor.string("mover source identity"),
            })
        positions = vectors(count, 3, "mover positions")
        normals = vectors(count, 3, "mover normals")
        uv0 = vectors(count, 2, "mover UV0")
        indices = list(cursor.unpack(f"<{index_count}I", "mover indices"))
        if any(value >= count for value in indices):
            raise CacheFormatError("mover index outside vertex stream")
        if any(abs(value) > _MAX_COORDINATE
               for position in positions for value in position):
            raise CacheFormatError("mover position stream is outside native bound")
        if any(abs(sum(value * value for value in normal) - 1.0) > 0.02
               for normal in normals):
            raise CacheFormatError("mover normal stream is not unit")
        _validate_sections(sections, count, index_count, indices,
                           allow_mover_null=True)
        for section in sections:
            normal = section["plane_normal"]
            if abs(sum(value * value for value in normal) - 1.0) > 0.02:
                raise CacheFormatError("mover plane normal is not unit")
            for vertex in {indices[at] for at in range(
                    section["first_index"], section["first_index"] + section["index_count"])}:
                distance = sum((positions[vertex][axis] - section["plane_origin"][axis])
                               * normal[axis] for axis in range(3))
                if abs(distance) > 0.05:
                    raise CacheFormatError("mover vertex is outside section plane")
        movers.append({"actor_identity": actor, "model_identity": model,
                       "sections": sections, "positions": positions,
                       "normals": normals, "uv0": uv0, "indices": indices})
    result["mover_models"] = movers
    _validate_world_semantics(result, counts)
    cursor.end("world")
    return result


def section_identities_from_bsp_json(document: dict[str, Any], role: str,
                                     map_identity: str = "") -> dict[str, dict[str, Any]]:
    if role not in {"main", "portal"}:
        raise CacheFormatError("invalid BSP identity role")
    source = document.get("portal_materials" if role == "portal" else "materials")
    if not isinstance(source, dict):
        raise CacheFormatError("bsp.json material table is absent")
    result = {}
    for slot, row in source.items():
        if not isinstance(row, dict):
            raise CacheFormatError("bsp.json material row is not an object")
        package = _text(row.get("package", ""), "bsp.json package")
        name = _text(row.get("texture", ""), "bsp.json texture", optional=False)
        source_identity = row.get("source_identity")
        if not source_identity:
            raise CacheFormatError(
                "bsp.json source provenance is absent; exact package object path is required")
        pan = row.get("pan_zone_actor") or row.get("pan_zone", "")
        if pan and map_identity and "." not in pan:
            pan = f"{map_identity}.{pan}"
        result[_text(slot, "bsp.json slot", optional=False)] = {
            "canonical_package": package, "canonical_name": name,
            "source_texture_identity": _text(source_identity, "bsp.json source"),
            "surface_flags": int(row.get("flags", 0)),
            "pan_zone_actor_identity": _text(pan, "bsp.json pan zone"),
        }
    return result


def collect_zone_parity(root: Path, maps: Iterable[str]) -> dict[str, dict[str, Any]]:
    result = {}
    for map_name in maps:
        document = json.loads((Path(root) / f"{map_name}.zones.json").read_text(
            encoding="utf-8"))
        leaf_bsp = document.get("leaf_bsp", {})
        if not isinstance(leaf_bsp, dict):
            raise CacheFormatError(f"{map_name}: invalid leaf_bsp oracle")
        light_runs = document.get("leaf_lights", {})
        if isinstance(light_runs, dict):
            runs = list(light_runs.values())
        elif isinstance(light_runs, list):
            runs = light_runs
        else:
            raise CacheFormatError(f"{map_name}: invalid leaf-light oracle")
        if any(not isinstance(run, list) for run in runs):
            raise CacheFormatError(f"{map_name}: invalid leaf-light run")
        result[map_name] = {
            "num_leaves": leaf_bsp.get("num_leaves", document.get("num_leaves")),
            "root_outside": bool(leaf_bsp.get(
                "root_outside", document.get("root_outside", False))),
            "leaf_lights": light_runs,
            "friction_nodes": document.get("friction_nodes", []),
            "light_reference_count": sum(len(run) for run in runs),
            "longest_light_run": max((len(run) for run in runs), default=0),
        }
    return result


def _mover_glb_summary(path: Path) -> list[dict[str, Any]]:
    data = path.read_bytes()
    if len(data) < 28:
        raise CacheFormatError(f"{path}: truncated mover GLB")
    magic, version, total, json_bytes, json_kind = struct.unpack_from("<IIIII", data)
    if magic != 0x46546C67 or version != 2 or total != len(data) \
            or json_kind != 0x4E4F534A or 20 + json_bytes > len(data):
        raise CacheFormatError(f"{path}: malformed mover GLB framing")
    try:
        document = json.loads(data[20:20 + json_bytes].decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise CacheFormatError(f"{path}: malformed mover GLB JSON") from exc
    meshes = document.get("meshes", [])
    materials = document.get("materials", [])
    accessors = document.get("accessors", [])
    if len(meshes) != 1 or not isinstance(meshes[0], dict):
        raise CacheFormatError(f"{path}: mover GLB mesh census is malformed")
    primitives = meshes[0].get("primitives", [])
    if not isinstance(primitives, list) or not primitives:
        raise CacheFormatError(f"{path}: mover GLB has no primitives")
    result = []
    for index, primitive in enumerate(primitives):
        if not isinstance(primitive, dict):
            raise CacheFormatError(f"{path}: mover primitive is malformed")
        material_index = primitive.get("material")
        accessor_index = primitive.get("indices")
        if not isinstance(material_index, int) or not 0 <= material_index < len(materials) \
                or not isinstance(accessor_index, int) or not 0 <= accessor_index < len(accessors):
            raise CacheFormatError(f"{path}: mover primitive binding is malformed")
        slot = materials[material_index].get("name")
        index_count = accessors[accessor_index].get("count")
        if not isinstance(slot, str) or not slot or not isinstance(index_count, int) \
                or index_count <= 0 or index_count % 3:
            raise CacheFormatError(f"{path}: mover primitive census is malformed")
        plane = (primitive.get("extras", {}) or {}).get("wotMoverPlane")
        if not isinstance(plane, dict) or set(plane) != {"origin", "normal"}:
            raise CacheFormatError(f"{path}: mover primitive lacks its plane")
        result.append({"slot": slot, "triangle_count": index_count // 3,
                       "plane": plane})
    return result


def collect_mover_parity(root: Path, maps: Iterable[str]) -> dict[str, Any]:
    movers = sections = triangles = planes = 0
    models = []
    map_census = {}
    for map_name in maps:
        mover_start, section_start, plane_start, triangle_start = (
            movers, sections, planes, triangles)
        document = json.loads((Path(root) / map_name / "movers.json").read_text(
            encoding="utf-8"))
        rows = document.get("movers", document if isinstance(document, list) else [])
        if not isinstance(rows, list):
            raise CacheFormatError(f"{map_name}: invalid mover list")
        movers += len(rows)
        for mover in rows:
            mover_sections = mover.get("sections")
            if mover_sections is None:
                surfaces = mover.get("surfaces", [])
                if not isinstance(surfaces, list) or not surfaces:
                    raise CacheFormatError(f"{map_name}: mover lacks surface planes")
                glb_sections = _mover_glb_summary(
                    Path(root) / map_name / f"{mover.get('model', '')}.glb")
                surface_by_slot = {row.get("slot"): row for row in surfaces}
                if len(surface_by_slot) != len(surfaces) or set(surface_by_slot) != {
                        row["slot"] for row in glb_sections}:
                    raise CacheFormatError(
                        f"{map_name}:{mover.get('name')}: GLB/surface slots disagree")
                for glb in glb_sections:
                    surface = surface_by_slot[glb["slot"]]
                    origin = _vector(surface.get("origin"), 3, "mover surface origin")
                    normal = _vector(surface.get("normal"), 3, "mover surface normal")
                    expected_origin = [origin[0] / 100.0, origin[2] / 100.0,
                                       origin[1] / 100.0]
                    expected_normal = [normal[0], normal[2], normal[1]]
                    actual_origin = _vector(
                        glb["plane"].get("origin"), 3, "mover GLB plane origin")
                    actual_normal = _vector(
                        glb["plane"].get("normal"), 3, "mover GLB plane normal")
                    if any(not math.isclose(left, right, rel_tol=0.0, abs_tol=1e-9)
                           for left, right in zip(
                               expected_origin + expected_normal,
                               actual_origin + actual_normal)):
                        raise CacheFormatError(
                            f"{map_name}:{mover.get('name')}: GLB plane disagrees")
                mover_sections = [{**surface_by_slot[glb["slot"]],
                                   "triangle_count": glb["triangle_count"]}
                                  for glb in glb_sections]
            if not isinstance(mover_sections, list) or not mover_sections:
                raise CacheFormatError(f"{map_name}: invalid mover sections")
            sections += len(mover_sections)
            plane_count = sum(int("plane" in section or (
                "origin" in section and "normal" in section))
                              for section in mover_sections)
            planes += plane_count
            triangle_count = sum(int(section.get(
                "triangles", section.get("triangle_count", 0)))
                                 for section in mover_sections)
            triangles += triangle_count
            models.append({"map_identity": map_name,
                           "actor_identity": mover.get("name", ""),
                           "model_identity": mover.get("model", ""),
                           "section_count": len(mover_sections),
                           "plane_count": plane_count,
                           "triangle_count": triangle_count})
        map_census[map_name] = {
            "mover_count": movers - mover_start,
            "section_count": sections - section_start,
            "plane_count": planes - plane_start,
            "triangle_count": triangles - triangle_start,
        }
    return {"mover_count": movers, "section_count": sections,
            "plane_count": planes, "triangle_count": triangles,
            "models": models, "maps": map_census}
