"""Deterministic BSP static-mesh payload and stripped-carrier helpers.

The payload consumes the exact GLB emitted by :mod:`bsp2obj`, converts its
coordinates back to final UE centimetres, and writes a small, hostile-input
validated binary contract.  It deliberately does not parse arbitrary glTF:
accepting only the converter's pinned subset keeps the bootstrap and packaged
reader contracts auditable.
"""
from __future__ import annotations

import hashlib
import json
import math
import struct
import sys
from pathlib import Path

from gltf import write_glb


BSP_MAGIC = b"WOTPBSP\0"
BSP_ENCODING = "wot-bsp-static-mesh-v1"
BSP_ENCODING_VERSION = 1
BSP_TANGENT_ALGORITHM = "ue-mikk-preserve-normals-v1"
BSP_TANGENT_ALGORITHM_ID = 1
BSP_DERIVED_FORMAT_VERSION = 1
MAX_BSP_VERTICES = 2_000_000
MAX_BSP_INDICES = 6_000_000
MAX_BSP_SECTIONS = 4_096
MAX_BSP_SLOT_NAME_BYTES = 1_024
MAX_BSP_LIGHTMAP_DIMENSION = 8_192
MAX_BSP_LIGHTMAP_PIXELS = 4_096 * 8_192
MAX_ENTRY_LIGHTMAP_PIXELS = 4 * 1_024 * 1_024
MAX_BSP_PROVENANCE_INPUTS = 256
MAX_GLB_BYTES = 256 * 1024 * 1024
MAX_BSP_COORDINATE_ABS = 10_000_000.0

_HEADER = struct.Struct("<8sHHIIIHH6f")
_SECTION = struct.Struct("<HHIIIIB3x")
_SECTION_DECODE = struct.Struct("<HHIIIIB3s")
_SECTION_IDENTITY = struct.Struct("<HHHHI")
_SECTION_MACRO = struct.Struct("<HHHHfff")


class BSPPayloadError(ValueError):
    """A deterministic conversion or structural-validation failure."""


def split_bsp_glb_parts(conversion_dir: Path, map_name: str,
                        output_dir: Path) -> list[dict]:
    """Split a map's main/portal GLBs exactly like canonical world import.

    Record-bearing fractal slots become independent parts.  Each result carries
    the splitter-authored ``asset_name`` plus its main/portal role, which is the
    complete semantic identity used by both cache keys and stripped carriers.
    """
    conversion_dir = Path(conversion_dir)
    info_path = conversion_dir / f"{map_name}.bsp.json"
    try:
        info = json.loads(info_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BSPPayloadError(f"cannot read BSP material manifest {info_path}: {exc}") from exc

    import_dir = Path(__file__).resolve().parents[1] / "import"
    added_path = str(import_dir) not in sys.path
    if added_path:
        sys.path.insert(0, str(import_dir))
    try:
        from world_fractal_split import (  # pylint: disable=import-outside-toplevel
            record_bearing_slots,
            runtime_fractal_texture_keys,
            split_world_glb,
        )
        runtime_keys = runtime_fractal_texture_keys(conversion_dir / "flipbooks")
        parts = []
        for role, suffix, material_field in (
                ("main", "", "materials"),
                ("portal", "_portals", "portal_materials")):
            source = conversion_dir / f"{map_name}{suffix}.glb"
            if role == "main" and not source.is_file():
                raise BSPPayloadError(f"missing main BSP GLB: {source}")
            if not source.is_file():
                continue
            materials = info.get(material_field, {})
            if not isinstance(materials, dict):
                raise BSPPayloadError(
                    f"{info_path}: {material_field} must be an object")
            asset_name = f"{map_name}{suffix}"
            split_slots = record_bearing_slots(materials, runtime_keys)
            for part in split_world_glb(
                    source, Path(output_dir) / asset_name,
                    split_slots, asset_name):
                parts.append({**part, "role": role, "source_path": source})
        return parts
    except BSPPayloadError:
        raise
    except (OSError, ValueError) as exc:
        raise BSPPayloadError(f"cannot split BSP GLBs for {map_name}: {exc}") from exc
    finally:
        if added_path:
            sys.path.remove(str(import_dir))


def _checked_product(*values: int, limit: int = MAX_GLB_BYTES) -> int:
    result = 1
    for value in values:
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise BSPPayloadError(f"invalid allocation count {value!r}")
        result *= value
        if result > limit:
            raise BSPPayloadError(f"allocation size exceeds {limit} bytes")
    return result


def _finite(values, label):
    if any(not isinstance(value, (int, float)) or isinstance(value, bool)
           or not math.isfinite(value) for value in values):
        raise BSPPayloadError(f"{label} contains a non-finite value")


def _canonical_float(value: float) -> float:
    """Use the one canonical IEEE spelling for zero in proof payloads."""
    return 0.0 if value == 0.0 else value


def _read_glb(path: Path) -> tuple[dict, bytes]:
    path = Path(path)
    size = path.stat().st_size
    if not 28 <= size <= MAX_GLB_BYTES:
        raise BSPPayloadError(f"{path}: invalid GLB byte length {size}")
    data = path.read_bytes()
    magic, version, total = struct.unpack_from("<III", data)
    if magic != 0x46546C67 or version != 2 or total != len(data):
        raise BSPPayloadError(f"{path}: invalid GLB 2 header/length")
    offset = 12
    chunks = []
    while offset < len(data):
        if offset + 8 > len(data):
            raise BSPPayloadError(f"{path}: truncated GLB chunk header")
        length, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        end = offset + length
        if length % 4 or end < offset or end > len(data):
            raise BSPPayloadError(f"{path}: invalid GLB chunk range")
        chunks.append((kind, data[offset:end]))
        offset = end
    if offset != len(data) or len(chunks) != 2 \
            or chunks[0][0] != 0x4E4F534A or chunks[1][0] != 0x004E4942:
        raise BSPPayloadError(f"{path}: expected exactly JSON then BIN chunks")
    try:
        document = json.loads(chunks[0][1].decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BSPPayloadError(f"{path}: invalid GLB JSON: {exc}") from exc
    return document, chunks[1][1]


def _exact_list(value, count, label):
    if not isinstance(value, list) or len(value) != count:
        raise BSPPayloadError(f"GLB requires exactly {count} {label}")
    return value


def _read_accessor(document: dict, binary: bytes, accessor_index: object,
                   component_type: int, accessor_type: str,
                   components: int, label: str) -> tuple:
    accessors = document.get("accessors")
    views = document.get("bufferViews")
    if (not isinstance(accessors, list) or not isinstance(views, list)
            or not isinstance(accessor_index, int) or isinstance(accessor_index, bool)
            or not 0 <= accessor_index < len(accessors)):
        raise BSPPayloadError(f"{label}: invalid accessor index")
    accessor = accessors[accessor_index]
    if (not isinstance(accessor, dict)
            or set(accessor) - {"bufferView", "byteOffset", "componentType",
                                "count", "type", "min", "max"}
            or accessor.get("componentType") != component_type
            or accessor.get("type") != accessor_type
            or not isinstance(accessor.get("count"), int)
            or isinstance(accessor.get("count"), bool)
            or accessor["count"] < 0
            or "sparse" in accessor or accessor.get("normalized", False)):
        raise BSPPayloadError(f"{label}: unsupported accessor shape")
    view_index = accessor.get("bufferView")
    if (not isinstance(view_index, int) or isinstance(view_index, bool)
            or not 0 <= view_index < len(views)):
        raise BSPPayloadError(f"{label}: invalid buffer view")
    view = views[view_index]
    if (not isinstance(view, dict)
            or set(view) - {"buffer", "byteOffset", "byteLength", "target"}
            or view.get("buffer") != 0
            or not isinstance(view.get("byteOffset", 0), int)
            or not isinstance(view.get("byteLength"), int)
            or view.get("byteOffset", 0) < 0 or view["byteLength"] < 0):
        raise BSPPayloadError(f"{label}: unsupported buffer-view shape")
    count = accessor["count"]
    item_size = 4 * components
    byte_length = _checked_product(count, item_size)
    accessor_offset = accessor.get("byteOffset", 0)
    if not isinstance(accessor_offset, int) or isinstance(accessor_offset, bool) \
            or accessor_offset < 0:
        raise BSPPayloadError(f"{label}: invalid accessor byte offset")
    start = view.get("byteOffset", 0) + accessor_offset
    end = start + byte_length
    view_end = view.get("byteOffset", 0) + view["byteLength"]
    if start < view.get("byteOffset", 0) or end < start \
            or end > view_end or view_end > len(binary):
        raise BSPPayloadError(f"{label}: accessor escapes its buffer view")
    code = "f" if component_type == 5126 else "I"
    return struct.unpack_from(f"<{count * components}{code}", binary, start)


def decode_bsp_glb(path: Path, role: str) -> dict:
    """Decode the pinned bsp2obj GLB subset to final UE-centimetre streams."""
    if role not in ("main", "portal"):
        raise BSPPayloadError(f"invalid BSP role {role!r}")
    document, binary = _read_glb(path)
    asset = document.get("asset")
    if not isinstance(asset, dict) or asset.get("version") != "2.0" \
            or asset.get("generator") != "wot bsp2glb":
        raise BSPPayloadError("GLB was not emitted by the pinned BSP converter")
    buffers = _exact_list(document.get("buffers"), 1, "buffer")
    if buffers[0] != {"byteLength": len(binary)}:
        raise BSPPayloadError("GLB buffer declaration disagrees with BIN bytes")
    meshes = _exact_list(document.get("meshes"), 1, "mesh")
    primitives = meshes[0].get("primitives") if isinstance(meshes[0], dict) else None
    materials = document.get("materials")
    if (not isinstance(primitives, list) or not primitives
            or len(primitives) > MAX_BSP_SECTIONS
            or not isinstance(materials, list)):
        raise BSPPayloadError("GLB has an invalid primitive/material list")
    if (not isinstance(document.get("accessors"), list)
            or not isinstance(document.get("bufferViews"), list)
            or len(document["accessors"]) != len(document["bufferViews"])
            or len(document["accessors"]) < len(primitives) * 5
            or len(document["accessors"]) > MAX_BSP_SECTIONS * 5):
        raise BSPPayloadError("GLB contains unexpected accessor/buffer-view data")

    positions: list[float] = []
    normals: list[float] = []
    uv0: list[float] = []
    uv1: list[float] = []
    indices: list[int] = []
    sections: list[dict] = []
    used_materials = set()
    used_slots = set()
    used_accessors = set()
    for section_index, primitive in enumerate(primitives):
        if not isinstance(primitive, dict) or set(primitive) != {
                "attributes", "indices", "material", "mode"} \
                or primitive.get("mode") != 4:
            raise BSPPayloadError(f"section {section_index}: unsupported primitive")
        attrs = primitive.get("attributes")
        if not isinstance(attrs, dict) or set(attrs) != {
                "POSITION", "NORMAL", "TEXCOORD_0", "TEXCOORD_1"}:
            raise BSPPayloadError(
                f"section {section_index}: proof payload requires exactly two UV channels")
        section_accessor_list = [
            attrs["POSITION"], attrs["NORMAL"], attrs["TEXCOORD_0"],
            attrs["TEXCOORD_1"], primitive["indices"]]
        if any(not isinstance(index, int) or isinstance(index, bool)
               for index in section_accessor_list):
            raise BSPPayloadError(
                f"section {section_index}: invalid/reused accessor")
        section_accessors = set(section_accessor_list)
        if (len(section_accessors) != 5
                or used_accessors.intersection(section_accessors)):
            raise BSPPayloadError(
                f"section {section_index}: invalid/reused accessor")
        used_accessors.update(section_accessors)
        material_index = primitive.get("material")
        if (not isinstance(material_index, int) or isinstance(material_index, bool)
                or not 0 <= material_index < len(materials)
                or material_index != section_index
                or material_index in used_materials):
            raise BSPPayloadError(f"section {section_index}: invalid/reused material")
        used_materials.add(material_index)
        material = materials[material_index]
        slot = material.get("name") if isinstance(material, dict) else None
        if not isinstance(slot, str) or not slot:
            raise BSPPayloadError(f"section {section_index}: missing material slot")
        slot_bytes = slot.encode("utf-8")
        if (len(slot_bytes) > MAX_BSP_SLOT_NAME_BYTES or b"\0" in slot_bytes
                or any(byte < 32 or byte == 127 for byte in slot_bytes)):
            raise BSPPayloadError(f"section {section_index}: unsafe material slot")
        if slot in used_slots:
            raise BSPPayloadError(f"section {section_index}: duplicate material slot")
        used_slots.add(slot)

        local_positions = _read_accessor(
            document, binary, attrs["POSITION"], 5126, "VEC3", 3, "position")
        local_normals = _read_accessor(
            document, binary, attrs["NORMAL"], 5126, "VEC3", 3, "normal")
        local_uv0 = _read_accessor(
            document, binary, attrs["TEXCOORD_0"], 5126, "VEC2", 2, "uv0")
        local_uv1 = _read_accessor(
            document, binary, attrs["TEXCOORD_1"], 5126, "VEC2", 2, "uv1")
        local_indices = _read_accessor(
            document, binary, primitive["indices"], 5125, "SCALAR", 1, "index")
        count = len(local_positions) // 3
        if (count == 0 or len(local_normals) != count * 3
                or len(local_uv0) != count * 2 or len(local_uv1) != count * 2
                or len(local_indices) == 0 or len(local_indices) % 3):
            raise BSPPayloadError(f"section {section_index}: inconsistent stream counts")
        if len(positions) // 3 + count > MAX_BSP_VERTICES \
                or len(indices) + len(local_indices) > MAX_BSP_INDICES:
            raise BSPPayloadError("BSP GLB exceeds compiled geometry caps")
        _finite(local_positions, "positions")
        _finite(local_normals, "normals")
        _finite(local_uv0, "uv0")
        _finite(local_uv1, "uv1")
        for offset in range(0, len(local_normals), 3):
            squared = sum(value * value for value in local_normals[offset:offset + 3])
            if not 0.98 <= squared <= 1.02:
                raise BSPPayloadError(f"section {section_index}: non-unit normal")
        if any(index >= count for index in local_indices):
            raise BSPPayloadError(f"section {section_index}: out-of-range index")

        base_vertex = len(positions) // 3
        first_index = len(indices)
        # bsp2obj wrote glTF=(UE.x, UE.z, UE.y)/100; undo it here.
        for offset in range(0, len(local_positions), 3):
            x, y, z = local_positions[offset:offset + 3]
            positions.extend((x * 100.0, z * 100.0, y * 100.0))
            nx, ny, nz = local_normals[offset:offset + 3]
            normals.extend((nx, nz, ny))
        uv0.extend(local_uv0)
        uv1.extend(local_uv1)
        indices.extend(base_vertex + index for index in local_indices)
        sections.append({
            "material_slot": slot,
            "first_index": first_index,
            "index_count": len(local_indices),
            "triangle_count": len(local_indices) // 3,
            "min_vertex": base_vertex + min(local_indices),
            "max_vertex": base_vertex + max(local_indices),
            "collision_enabled": role == "main",
        })

    if used_materials != set(range(len(materials))):
        raise BSPPayloadError("GLB contains unused or out-of-order materials")
    axes = [positions[index::3] for index in range(3)]
    if any(abs(value) > MAX_BSP_COORDINATE_ABS for value in positions):
        raise BSPPayloadError("BSP position exceeds the proof coordinate cap")
    bounds_min = [min(axis) for axis in axes]
    bounds_max = [max(axis) for axis in axes]
    return {
        "role": role,
        "positions": positions,
        "normals": normals,
        "uv0": uv0,
        "uv1": uv1,
        "indices": indices,
        "sections": sections,
        "bounds_min": bounds_min,
        "bounds_max": bounds_max,
        "vertex_count": len(positions) // 3,
        "index_count": len(indices),
        "triangle_count": len(indices) // 3,
        "uv_channels": 2,
        "tangent_algorithm": BSP_TANGENT_ALGORITHM,
    }


def encode_bsp_payload(geometry: dict) -> bytes:
    """Encode already-decoded geometry after revalidating the public contract."""
    role = geometry.get("role")
    vertex_count = geometry.get("vertex_count")
    index_count = geometry.get("index_count")
    sections = geometry.get("sections")
    if (role not in ("main", "portal") or not isinstance(vertex_count, int)
            or not 1 <= vertex_count <= MAX_BSP_VERTICES
            or not isinstance(index_count, int) or not 3 <= index_count <= MAX_BSP_INDICES
            or index_count % 3 or not isinstance(sections, list)
            or not 1 <= len(sections) <= MAX_BSP_SECTIONS
            or geometry.get("uv_channels") != 2
            or geometry.get("tangent_algorithm") != BSP_TANGENT_ALGORITHM):
        raise BSPPayloadError("invalid BSP geometry header contract")
    streams = (("positions", 3), ("normals", 3), ("uv0", 2), ("uv1", 2))
    for label, width in streams:
        values = geometry.get(label)
        if not isinstance(values, list) or len(values) != vertex_count * width:
            raise BSPPayloadError(f"{label} count disagrees with vertices")
        _finite(values, label)
    if any(abs(value) > MAX_BSP_COORDINATE_ABS
           for value in geometry["positions"]):
        raise BSPPayloadError("BSP position exceeds the proof coordinate cap")
    indices = geometry.get("indices")
    if (not isinstance(indices, list) or len(indices) != index_count
            or any(not isinstance(index, int) or isinstance(index, bool)
                   or not 0 <= index < vertex_count for index in indices)):
        raise BSPPayloadError("invalid BSP index stream")
    bounds = list(geometry.get("bounds_min", [])) + list(geometry.get("bounds_max", []))
    if len(bounds) != 6:
        raise BSPPayloadError("invalid BSP bounds")
    _finite(bounds, "bounds")
    flags = (1 if role == "portal" else 0) | 2  # proof v1 always carries UV1
    payload = bytearray(_HEADER.pack(
        BSP_MAGIC, BSP_ENCODING_VERSION, flags, vertex_count, index_count,
        len(sections), 2, BSP_TANGENT_ALGORITHM_ID,
        *(_canonical_float(value) for value in bounds)))
    expected_first = 0
    for section_index, section in enumerate(sections):
        required = {"material_slot", "first_index", "index_count", "triangle_count",
                    "min_vertex", "max_vertex", "collision_enabled"}
        if not isinstance(section, dict) or set(section) != required:
            raise BSPPayloadError(f"section {section_index}: invalid metadata fields")
        name = section["material_slot"].encode("utf-8")
        first = section["first_index"]
        count = section["index_count"]
        if (not 0 < len(name) <= MAX_BSP_SLOT_NAME_BYTES or b"\0" in name
                or not isinstance(first, int) or first != expected_first
                or not isinstance(count, int) or count <= 0 or count % 3
                or section["triangle_count"] != count // 3
                or first + count > index_count
                or not isinstance(section["min_vertex"], int)
                or not isinstance(section["max_vertex"], int)
                or not 0 <= section["min_vertex"] <= section["max_vertex"] < vertex_count
                or type(section["collision_enabled"]) is not bool
                or section["collision_enabled"] != (role == "main")):
            raise BSPPayloadError(f"section {section_index}: invalid range/role metadata")
        actual = indices[first:first + count]
        if min(actual) != section["min_vertex"] or max(actual) != section["max_vertex"]:
            raise BSPPayloadError(f"section {section_index}: min/max vertex mismatch")
        payload.extend(_SECTION.pack(
            len(name), 0, first, count, section["min_vertex"],
            section["max_vertex"], int(section["collision_enabled"])))
        payload.extend(name)
        expected_first += count
    if expected_first != index_count:
        raise BSPPayloadError("sections do not exactly cover the index stream")
    for label, _width in streams:
        values = geometry[label]
        payload.extend(struct.pack(
            f"<{len(values)}f",
            *(_canonical_float(value) for value in values)))
    payload.extend(struct.pack(f"<{index_count}I", *indices))
    return bytes(payload)


def _decode_bsp_text_at(payload: bytes, offset: int, size: int, label: str,
                        *, optional: bool = False) -> tuple[str, int]:
    if size > MAX_BSP_SLOT_NAME_BYTES or offset + size > len(payload):
        raise BSPPayloadError(f"truncated or oversized {label}")
    raw = payload[offset:offset + size]
    try:
        value = raw.decode("utf-8")
    except UnicodeError as exc:
        raise BSPPayloadError(f"invalid UTF-8 {label}") from exc
    if (value.encode("utf-8") != raw or "\0" in value
            or (not optional and not value)
            or any(ord(character) < 32 or ord(character) == 127
                   for character in value)):
        raise BSPPayloadError(f"unsafe {label}")
    return value, offset + size


def _validate_entry_trailer(payload: bytes, triangle_count: int,
                            lightmap_width: int, lightmap_height: int) -> dict:
    offset = 0

    def unpack(spelling: str, label: str) -> tuple:
        nonlocal offset
        layout = struct.Struct(spelling)
        if offset + layout.size > len(payload):
            raise BSPPayloadError(f"truncated entry trailer {label}")
        values = layout.unpack_from(payload, offset)
        offset += layout.size
        return values

    def take(size: int, label: str) -> bytes:
        nonlocal offset
        if size < 0 or offset + size > len(payload):
            raise BSPPayloadError(f"truncated entry trailer {label}")
        value = payload[offset:offset + size]
        offset += size
        return value

    (byte_count, triangles, surface_count, light_count, pair_count,
     root_outside) = unpack("<6I", "header")
    if (byte_count != len(payload) or byte_count < 24
            or triangles != triangle_count or not 0 < surface_count <= 65_536
            or light_count > 8 or pair_count > 4_096 or root_outside > 1):
        raise BSPPayloadError("invalid entry trailer counts or length")
    triangle_surfaces = unpack(
        f"<{triangles}I", "triangle surfaces") if triangles else ()
    if any(value >= surface_count for value in triangle_surfaces):
        raise BSPPayloadError("invalid entry trailer triangle surface")
    surfaces = []
    intervals: list[list[tuple[int, int]]] = [
        [] for _ in range(lightmap_height)]
    padded_area = 0
    for expected in range(surface_count):
        index, x, y, width, height, pan_u, pan_v = unpack(
            "<IIIIIff", "surface")
        if (index != expected or not math.isfinite(pan_u)
                or not math.isfinite(pan_v) or abs(pan_u) > 1_024.0
                or abs(pan_v) > 1_024.0):
            raise BSPPayloadError("invalid entry trailer surface")
        empty = x == y == width == height == 0
        if not empty:
            right, bottom = x + width, y + height
            padded_area += (width + 2) * (height + 2)
            if (not x or not y or not width or not height
                    or right >= lightmap_width or bottom >= lightmap_height
                    or padded_area > lightmap_width * lightmap_height):
                raise BSPPayloadError("invalid entry trailer surface rectangle")
            for row in range(y - 1, bottom + 1):
                intervals[row].append((x - 1, right + 1))
        surfaces.append((width, height))
    for row in intervals:
        row.sort()
        if any(row[index][0] < row[index - 1][1]
               for index in range(1, len(row))):
            raise BSPPayloadError("overlapping entry trailer surfaces")
    previous = None
    folded = set()
    for _ in range(light_count):
        identity_bytes, reserved = unpack("<HH", "light header")
        identity_raw = take(identity_bytes, "light identity")
        try:
            identity = identity_raw.decode("utf-8")
        except UnicodeError as exc:
            raise BSPPayloadError("invalid entry trailer light identity") from exc
        brightness, hue, saturation, reserved8, level = unpack(
            "<BBBBf", "light values")
        del brightness, hue, saturation
        if (reserved or reserved8 or not identity
                or identity.encode("utf-8") != identity_raw
                or len(identity_raw) > 1_024 or b"\0" in identity_raw
                or any(ord(character) < 32 or ord(character) == 127
                       for character in identity)
                or not math.isfinite(level) or not 0.0 <= level <= 1_024.0
                or (previous is not None and identity_raw <= previous)
                or identity.casefold() in folded):
            raise BSPPayloadError("invalid entry trailer light row")
        previous = identity_raw
        folded.add(identity.casefold())
    previous_pair = None
    total_pair_bytes = 0
    for _ in range(pair_count):
        light, surface, size = unpack("<III", "illumination")
        pair = (light, surface)
        if (light >= light_count or surface >= surface_count
                or (previous_pair is not None and pair <= previous_pair)
                or size != surfaces[surface][0] * surfaces[surface][1]):
            raise BSPPayloadError("invalid entry trailer illumination")
        total_pair_bytes += size
        if total_pair_bytes > 4 * 1_024 * 1_024:
            raise BSPPayloadError("entry trailer illumination over bound")
        take(size, "illumination bytes")
        previous_pair = pair
    if offset != len(payload):
        raise BSPPayloadError("trailing entry trailer bytes")
    return {"triangle_surfaces": list(triangle_surfaces),
            "surface_count": surface_count}


def _validate_triangle_surface_trailer(payload: bytes,
                                       triangle_count: int) -> dict:
    if len(payload) < 12:
        raise BSPPayloadError("truncated triangle-surface trailer")
    byte_count, triangles, surface_count = struct.unpack_from("<III", payload)
    if (byte_count != len(payload) or triangles != triangle_count
            or not 0 < surface_count <= 65_536
            or byte_count != 12 + triangles * 4):
        raise BSPPayloadError("invalid triangle-surface trailer header")
    surfaces = list(struct.unpack_from(f"<{triangles}I", payload, 12))
    if any(surface >= surface_count for surface in surfaces):
        raise BSPPayloadError("invalid triangle surface index")
    return {"triangle_surfaces": surfaces, "surface_count": surface_count}




MAX_ANIMATION_SURFACES = 65_536
MAX_ANIMATION_GROUPS = 4
MAX_ANIMATION_TERMS = 6
MAX_ANIMATION_BYTES = 64 * 1024 * 1024


def _validate_animation_trailer(payload: bytes, lightmap_width: int,
                                lightmap_height: int) -> dict:
    """Version-5 animated light-map responses (native BSPWorld.cpp writer):
    every model surface's atlas placement, the waveform group table, and the
    response terms with their sparse per-surface unsigned scale-2 RGB rects.
    Returns the decoded structure; raises on any bound violation."""
    offset = 0

    def unpack(spelling: str, label: str) -> tuple:
        nonlocal offset
        layout = struct.Struct(spelling)
        if offset + layout.size > len(payload):
            raise BSPPayloadError(f"truncated animation trailer {label}")
        values = layout.unpack_from(payload, offset)
        offset += layout.size
        return values

    byte_count, surface_count = unpack("<II", "header")
    if (byte_count != len(payload) or byte_count < 16
            or not 0 < surface_count <= MAX_ANIMATION_SURFACES):
        raise BSPPayloadError("invalid animation trailer counts or length")
    surfaces = []
    intervals: list[list[tuple[int, int]]] = [
        [] for _ in range(lightmap_height)]
    padded_area = 0
    for _ in range(surface_count):
        x, y, width, height = unpack("<IIII", "surface")
        empty = x == y == width == height == 0
        if not empty:
            right, bottom = x + width, y + height
            padded_area += (width + 2) * (height + 2)
            if (not x or not y or not width or not height
                    or right >= lightmap_width or bottom >= lightmap_height
                    or padded_area > lightmap_width * lightmap_height):
                raise BSPPayloadError("invalid animation surface rectangle")
            for row in range(y - 1, bottom + 1):
                intervals[row].append((x - 1, right + 1))
        surfaces.append((x, y, width, height))
    for row in intervals:
        row.sort()
        if any(row[index][0] < row[index - 1][1]
               for index in range(1, len(row))):
            raise BSPPayloadError("overlapping animation atlas allocations")
    (group_count,) = unpack("<I", "group count")
    if group_count > MAX_ANIMATION_GROUPS:
        raise BSPPayloadError("invalid animation group table")
    groups = []
    for _ in range(group_count):
        light_type, period, phase, reserved, light_count = unpack(
            "<BBBBI", "group")
        if reserved or light_type not in (2, 3, 7) or not light_count:
            raise BSPPayloadError("invalid animation group row")
        key = (light_type, period, phase)
        if groups and key <= groups[-1]["key"]:
            raise BSPPayloadError("animation groups are not sorted and unique")
        groups.append({"key": key, "light_count": light_count})
    (term_count,) = unpack("<I", "term count")
    if not 0 < term_count <= MAX_ANIMATION_TERMS:
        raise BSPPayloadError("invalid animation term table")
    terms = []
    response_bytes = 0
    waveform_terms = 0
    for _ in range(term_count):
        kind, group, effect, reserved, light_count, rect_count = unpack(
            "<BBBBII", "term")
        if (reserved or kind > 2 or not light_count or rect_count > surface_count
                or (kind == 0 and (group >= group_count or effect != 0))
                or (kind == 1 and (group != 255 or not 1 <= effect <= 3))
                or (kind == 2 and (group >= group_count or not 1 <= effect <= 3))):
            raise BSPPayloadError("invalid animation term row")
        order = (kind, effect) if kind == 1 else (kind, group, effect)
        if terms and order <= terms[-1]["order"]:
            raise BSPPayloadError("animation terms are not in material slot order")
        if kind == 0:
            if group != waveform_terms:
                raise BSPPayloadError(
                    "animation waveform slots do not match the group table")
            waveform_terms += 1
        rects = []
        previous_surface = None
        for _ in range(rect_count):
            (surface_index,) = unpack("<I", "rect surface")
            if (surface_index >= surface_count
                    or (previous_surface is not None
                        and surface_index <= previous_surface)):
                raise BSPPayloadError("invalid animation rect surface")
            _x, _y, width, height = surfaces[surface_index]
            size = width * height * 3
            if not width or not height or offset + size > len(payload):
                raise BSPPayloadError("invalid animation rect bytes")
            response_bytes += size
            if response_bytes > MAX_ANIMATION_BYTES:
                raise BSPPayloadError("animation responses over bound")
            rects.append((surface_index, payload[offset:offset + size]))
            offset += size
            previous_surface = surface_index
        terms.append({"order": order, "kind": kind,
                      "group": None if group == 255 else group,
                      "effect": effect, "light_count": light_count,
                      "rects": rects})
    if waveform_terms != group_count:
        raise BSPPayloadError("animation group table has no waveform term")
    if offset != len(payload):
        raise BSPPayloadError("trailing animation trailer bytes")
    for term in terms:
        del term["order"]
    return {"surfaces": surfaces, "groups": groups, "terms": terms}


MAX_SKY_REST_SURFACES = 65536
MAX_SKY_REST_GROUPS = 4096
MAX_SKY_REST_TRIANGLES = 2_000_000
SKY_REST_ROLES = {1: "visible", 2: "blocker", 3: "fakebackdrop"}
_SKY_REST_TRIANGLE = struct.Struct("<" + "f" * (3 + 3 * 7))


def _validate_sky_rest_trailer(payload: bytes) -> dict:
    """Version-6 sky-rest trailer (flag 32; native BSPWorld.cpp
    WoTWorldBuildSkyRestTrailer): the Tools/convert/skybake.py bake_rest census
    (sky zone, ray count, covered rays, ascending per-surface visible/blocker/
    fakebackdrop ray counts - exactly <Map>_skyrest.json) and the skygeo.py
    rest groups ("SkyRest__<slot>" visible, "SkyRestBlock", "SkyRestFB"),
    three world-space vertices per triangle with the encoder's UV0/UV1."""
    offset = 0

    def unpack(layout: struct.Struct, label: str) -> tuple:
        nonlocal offset
        if offset + layout.size > len(payload):
            raise BSPPayloadError(f"truncated sky-rest trailer {label}")
        values = layout.unpack_from(payload, offset)
        offset += layout.size
        return values

    byte_count, sky_zone, ray_count, covered, surface_count = unpack(
        struct.Struct("<IIIII"), "header")
    if (byte_count != len(payload) or byte_count < 24 or not 0 < sky_zone <= 255
            or not ray_count or covered > ray_count
            or surface_count > MAX_SKY_REST_SURFACES
            or surface_count * 16 > len(payload) - offset):
        raise BSPPayloadError("invalid sky-rest trailer header")
    surfaces = []
    for _ in range(surface_count):
        surface, visible, blocker, fakebackdrop = unpack(
            struct.Struct("<IIII"), "surface")
        if (surfaces and surface <= surfaces[-1]["surface"]) \
                or not visible + blocker + fakebackdrop:
            raise BSPPayloadError("invalid sky-rest surface row")
        surfaces.append({"surface": surface, "visible": visible,
                         "blocker": blocker, "fakebackdrop": fakebackdrop})
    (group_count,) = unpack(struct.Struct("<I"), "group count")
    if group_count > MAX_SKY_REST_GROUPS or group_count * 8 > len(payload) - offset:
        raise BSPPayloadError("invalid sky-rest group count")
    groups = []
    seen = set()
    triangles_total = 0
    for _ in range(group_count):
        role, pad, slot_size = unpack(struct.Struct("<BBH"), "group header")
        if pad or role not in SKY_REST_ROLES or slot_size > MAX_BSP_SLOT_NAME_BYTES:
            raise BSPPayloadError("invalid sky-rest group header")
        slot, offset = _decode_bsp_text_at(
            payload, offset, slot_size, "sky-rest slot", optional=True)
        (triangle_count,) = unpack(struct.Struct("<I"), "triangle count")
        key = (role, slot)
        triangles_total += triangle_count
        if ((role == 1) != bool(slot) or key in seen or not triangle_count
                or triangles_total > MAX_SKY_REST_TRIANGLES
                or triangle_count * _SKY_REST_TRIANGLE.size > len(payload) - offset):
            raise BSPPayloadError("invalid sky-rest group")
        seen.add(key)
        values = []
        for _ in range(triangle_count):
            row = unpack(_SKY_REST_TRIANGLE, "triangle")
            _finite(row, "sky-rest triangle")
            if any(abs(value) > MAX_BSP_COORDINATE_ABS
                   for corner in range(3) for value in row[3 + corner * 7:6 + corner * 7]):
                raise BSPPayloadError("sky-rest position exceeds the coordinate cap")
            values.append(row)
        groups.append({"role": SKY_REST_ROLES[role], "slot": slot,
                       "triangles": values})
    if offset != len(payload):
        raise BSPPayloadError("trailing sky-rest trailer bytes")
    return {"sky_zone": sky_zone, "ray_count": ray_count, "covered_rays": covered,
            "surfaces": surfaces, "groups": groups}


def parse_bsp_payload(payload: bytes) -> dict:
    """Hostile-input parser used by cache verification and stdlib specs."""
    payload = bytes(payload)
    if not _HEADER.size <= len(payload) <= MAX_GLB_BYTES:
        raise BSPPayloadError("truncated or oversized BSP payload")
    (magic, version, flags, vertex_count, index_count, section_count,
     uv_channels, tangent_id, *bounds) = _HEADER.unpack_from(payload)
    # Version 10 retains v9 provenance and adds exact macro PanU/PanV terms.
    # Superseded production v7-v9 bytes are rejected.
    is_v10 = version == 10
    is_v4 = version in (4, 5, 6, 10)
    is_v5 = version in (5, 6, 10)
    is_v6 = version in (6, 10)
    has_macro_record = is_v10
    allowed_flags = (0x7F if is_v10 else
                     (0x3F if is_v6 else
                      (0x1F if is_v5 else (0xF if is_v4 else 0x3))))
    if (magic != BSP_MAGIC or version not in (1, 2, 3, 4, 5, 6, 10)
            or flags & ~allowed_flags or not flags & 2
            or is_v10 and not flags & 64
            or (is_v4 and flags & 8 and not flags & 4)
            or (is_v5 and flags & 16 and not flags & 4)
            or (is_v6 and flags & 32 and not flags & 4)
            or not 1 <= vertex_count <= MAX_BSP_VERTICES
            or not 3 <= index_count <= MAX_BSP_INDICES or index_count % 3
            or not 1 <= section_count <= MAX_BSP_SECTIONS
            or uv_channels != 2 or tangent_id != BSP_TANGENT_ALGORITHM_ID):
        raise BSPPayloadError("invalid BSP payload header")
    _finite(bounds, "bounds")
    if (any(abs(value) > MAX_BSP_COORDINATE_ABS for value in bounds)
            or any(bounds[axis] > bounds[axis + 3] for axis in range(3))):
        raise BSPPayloadError("invalid BSP bounds ordering or coordinate")
    role = "portal" if flags & 1 else "main"
    offset = _HEADER.size
    sections = []
    used_slots = set()
    expected_first = 0
    for section_index in range(section_count):
        if offset + _SECTION_DECODE.size > len(payload):
            raise BSPPayloadError(f"section {section_index}: truncated header")
        (name_size, reserved, first, count, minimum, maximum,
         collision, padding) = _SECTION_DECODE.unpack_from(payload, offset)
        offset += _SECTION_DECODE.size
        end = offset + name_size
        if reserved != 0 or collision not in (0, 1) or padding != b"\0\0\0":
            raise BSPPayloadError(
                f"section {section_index}: invalid flags or reserved padding")
        if (not 1 <= name_size <= MAX_BSP_SLOT_NAME_BYTES
                or end > len(payload) or first != expected_first or count == 0
                or count % 3 or first + count > index_count
                or not 0 <= minimum <= maximum < vertex_count
                or bool(collision) != (role == "main")):
            raise BSPPayloadError(f"section {section_index}: invalid header/range")
        try:
            name = payload[offset:end].decode("utf-8")
        except UnicodeError as exc:
            raise BSPPayloadError(f"section {section_index}: invalid UTF-8 slot") from exc
        if not name or "\0" in name or any(ord(char) < 32 or ord(char) == 127
                                           for char in name):
            raise BSPPayloadError(f"section {section_index}: unsafe slot name")
        if name in used_slots:
            raise BSPPayloadError(f"section {section_index}: duplicate slot name")
        used_slots.add(name)
        section = {
            "material_slot": name, "first_index": first, "index_count": count,
            "min_vertex": minimum, "max_vertex": maximum,
            "collision_enabled": bool(collision),
        }
        if not is_v4:
            section["triangle_count"] = count // 3
        offset = end
        if is_v4:
            if offset + _SECTION_IDENTITY.size > len(payload):
                raise BSPPayloadError(
                    f"section {section_index}: truncated section identity header")
            package_size, texture_size, source_size, pan_size, surface_flags = \
                _SECTION_IDENTITY.unpack_from(payload, offset)
            offset += _SECTION_IDENTITY.size
            package, offset = _decode_bsp_text_at(
                payload, offset, package_size, "canonical package", optional=True)
            texture, offset = _decode_bsp_text_at(
                payload, offset, texture_size, "canonical name", optional=True)
            source, offset = _decode_bsp_text_at(
                payload, offset, source_size, "source identity", optional=True)
            pan, offset = _decode_bsp_text_at(
                payload, offset, pan_size, "pan-zone identity", optional=True)
            if name != "__None" and not all((package, texture, source)):
                raise BSPPayloadError(
                    f"section {section_index}: incomplete canonical material identity")
            section.update({
                "canonical_package": package, "canonical_name": texture,
                "source_texture_identity": source,
                "surface_flags": surface_flags,
                "pan_zone_actor_identity": pan,
            })
        if has_macro_record:
            if offset + _SECTION_MACRO.size > len(payload):
                raise BSPPayloadError(
                    f"section {section_index}: truncated macro identity header")
            macro_package_size, macro_texture_size, macro_source_size, \
                macro_reserved, macro_ratio, macro_offset_u, macro_offset_v = \
                _SECTION_MACRO.unpack_from(payload, offset)
            offset += _SECTION_MACRO.size
            macro_package, offset = _decode_bsp_text_at(
                payload, offset, macro_package_size, "macro canonical package",
                optional=True)
            macro_texture, offset = _decode_bsp_text_at(
                payload, offset, macro_texture_size, "macro canonical name",
                optional=True)
            macro_source, offset = _decode_bsp_text_at(
                payload, offset, macro_source_size, "macro source identity",
                optional=True)
            has_macro = bool(macro_package or macro_texture or macro_source
                             or macro_ratio != 0.0 or macro_offset_u != 0.0
                             or macro_offset_v != 0.0)
            if (macro_reserved != 0 or has_macro and (
                    not all((macro_package, macro_texture, macro_source))
                    or not all(math.isfinite(value) for value in
                               (macro_ratio, macro_offset_u, macro_offset_v))
                    or macro_ratio <= 0.0)):
                raise BSPPayloadError(
                    f"section {section_index}: invalid macro identity")
            section.update({
                "macro_canonical_package": macro_package,
                "macro_canonical_name": macro_texture,
                "macro_source_texture_identity": macro_source,
                "macro_ratio": macro_ratio,
                "macro_offset_u": macro_offset_u,
                "macro_offset_v": macro_offset_v,
            })
        sections.append(section)
        expected_first += count
    if expected_first != index_count:
        raise BSPPayloadError("sections do not exactly cover indices")
    float_count = vertex_count * 10
    arrays_size = _checked_product(float_count, 4) + _checked_product(index_count, 4)
    if offset + arrays_size > len(payload):
        raise BSPPayloadError("BSP stream byte lengths disagree with header")
    result = {}
    for label, count in (("positions", vertex_count * 3),
                         ("normals", vertex_count * 3),
                         ("uv0", vertex_count * 2),
                         ("uv1", vertex_count * 2)):
        result[label] = list(struct.unpack_from(f"<{count}f", payload, offset))
        _finite(result[label], label)
        offset += count * 4
    result["indices"] = list(struct.unpack_from(f"<{index_count}I", payload, offset))
    offset += index_count * 4
    if any(abs(value) > MAX_BSP_COORDINATE_ABS for value in result["positions"]):
        raise BSPPayloadError("BSP position exceeds the proof coordinate cap")
    for section_index, section in enumerate(sections):
        actual = result["indices"][section["first_index"]:
                                   section["first_index"] + section["index_count"]]
        if (any(index >= vertex_count for index in actual)
                or min(actual) != section["min_vertex"]
                or max(actual) != section["max_vertex"]):
            raise BSPPayloadError(f"section {section_index}: invalid index data")
    # NO zero-area rejection here: collinear triangles with distinct indices
    # are LEGITIMATE retail geometry (bsp2obj's sliver removal misses
    # float-rounding-collinear ones; the cooked reference ships them; the
    # runtime rebuild sanitizes their basis and
    # Chaos skips them). Rejecting them killed the first all-map cache build
    # on Arena_01 triangle 4150. Repeated-INDEX triangles stay fail-closed.
    for triangle in range(0, index_count, 3):
        a, b, c = result["indices"][triangle:triangle + 3]
        if len({a, b, c}) != 3:
            raise BSPPayloadError(f"triangle {triangle // 3}: repeated vertex")
    for index in range(vertex_count):
        normal = result["normals"][index * 3:index * 3 + 3]
        squared = sum(value * value for value in normal)
        if not 0.98 <= squared <= 1.02:
            raise BSPPayloadError(f"vertex {index}: non-unit normal")
    axes = [result["positions"][index::3] for index in range(3)]
    computed_min = [min(axis) for axis in axes]
    computed_max = [max(axis) for axis in axes]
    if any(struct.pack("<f", actual) != struct.pack("<f", declared)
           for actual, declared in zip(computed_min + computed_max, bounds)):
        raise BSPPayloadError("declared bounds disagree with position stream")
    has_lightmap = version in (2, 3) or (is_v4 and bool(flags & 4))
    has_entry_trailer = version == 3 or (is_v4 and bool(flags & 8))
    has_animation = is_v5 and bool(flags & 16)
    lightmap_width = lightmap_height = 0
    lightmap = b""
    if has_lightmap:
        if offset + 12 > len(payload):
            raise BSPPayloadError("truncated lightmap header")
        lightmap_width, lightmap_height, byte_count = struct.unpack_from(
            "<III", payload, offset)
        offset += 12
        pixels = lightmap_width * lightmap_height
        if (not lightmap_width or lightmap_width > MAX_BSP_LIGHTMAP_DIMENSION
                or not lightmap_height or lightmap_height > MAX_BSP_LIGHTMAP_DIMENSION
                or pixels > MAX_BSP_LIGHTMAP_PIXELS
                or has_entry_trailer and pixels > MAX_ENTRY_LIGHTMAP_PIXELS
                or byte_count != pixels * 4 or offset + byte_count > len(payload)):
            raise BSPPayloadError("invalid lightmap header")
        lightmap = payload[offset:offset + byte_count]
        offset += byte_count
        if any(lightmap[index] != 255 for index in range(3, len(lightmap), 4)):
            raise BSPPayloadError("non-opaque lightmap")
    has_sky_rest = is_v6 and bool(flags & 32)
    has_triangle_surfaces = is_v10 and bool(flags & 64)
    animation = None
    sky_rest = None
    provenance = None
    entry_end = len(payload)
    if has_animation or has_sky_rest or has_triangle_surfaces:
        # Trailers follow the atlas in flag order (entry 8, animation 16, sky
        # rest 32), each prefixed by its own byte count; the last ends the
        # payload. The entry trailer is never emitted with a map trailer by
        # the native builder, but the walk admits it.
        cursor = offset

        def block(minimum: int, label: str) -> tuple[int, int]:
            if cursor + 4 > len(payload):
                raise BSPPayloadError(f"truncated {label} byte count")
            (count,) = struct.unpack_from("<I", payload, cursor)
            if count < minimum or cursor + count > len(payload):
                raise BSPPayloadError(f"invalid {label} byte count")
            return cursor, cursor + count

        if has_entry_trailer:
            _start, cursor = block(24, "entry trailer before a map trailer")
            entry_end = cursor
        if has_animation:
            start, cursor = block(16, "animation trailer")
            animation = _validate_animation_trailer(
                payload[start:cursor], lightmap_width, lightmap_height)
        if has_sky_rest:
            start, cursor = block(24, "sky-rest trailer")
            sky_rest = _validate_sky_rest_trailer(payload[start:cursor])
        if has_triangle_surfaces:
            start, cursor = block(12, "triangle-surface trailer")
            provenance = _validate_triangle_surface_trailer(
                payload[start:cursor], index_count // 3)
        if cursor != len(payload):
            raise BSPPayloadError("BSP trailers do not account for the payload")
    entry_trailer = payload[offset:entry_end] if has_entry_trailer else b""
    entry_provenance = None
    if has_entry_trailer:
        entry_provenance = _validate_entry_trailer(
            entry_trailer, index_count // 3, lightmap_width, lightmap_height)
        offset = entry_end
    if has_animation or has_sky_rest or has_triangle_surfaces:
        offset = len(payload)
    if offset != len(payload):
        raise BSPPayloadError("trailing BSP payload bytes")
    result.update({
        "version": version, "role": role, "sections": sections,
        "bounds_min": list(bounds[:3]), "bounds_max": list(bounds[3:]),
        "vertex_count": vertex_count, "index_count": index_count,
        "triangle_count": index_count // 3, "section_count": section_count,
        "uv_channels": uv_channels,
        "tangent_algorithm": BSP_TANGENT_ALGORITHM,
        "lightmap_width": lightmap_width, "lightmap_height": lightmap_height,
        "lightmap_bgra": lightmap, "entry_trailer": entry_trailer,
        "animation": animation, "sky_rest": sky_rest,
        "triangle_surfaces": (provenance or entry_provenance or {}).get(
            "triangle_surfaces", []),
        "surface_count": (provenance or entry_provenance or {}).get(
            "surface_count", 0),
    })
    return result


def bsp_manifest_metadata(geometry: dict, provenance: dict) -> dict:
    """Return the exact cache-v2 metadata mirrored by the packaged reader."""
    return {
        "role": geometry["role"],
        "vertex_count": geometry["vertex_count"],
        "index_count": geometry["index_count"],
        "triangle_count": geometry["triangle_count"],
        "section_count": len(geometry["sections"]),
        "uv_channels": geometry["uv_channels"],
        "bounds_min": geometry["bounds_min"],
        "bounds_max": geometry["bounds_max"],
        "tangent_algorithm": geometry["tangent_algorithm"],
        "sections": geometry["sections"],
        "provenance": provenance,
    }


def bsp_derived_cache_key(payload_sha256: str, engine_build: str,
                          platform: str, *,
                          tangent_algorithm: str = BSP_TANGENT_ALGORITHM,
                          format_version: int = BSP_DERIVED_FORMAT_VERSION) -> str:
    """Return the framed key mirrored by the packaged runtime.

    The raw payload hash is necessary but deliberately insufficient: cooked
    render buffers and Chaos objects are engine/platform products.  Changing
    any build identity or pinned construction contract must miss the old file.
    """
    if (not isinstance(payload_sha256, str) or len(payload_sha256) != 64
            or any(char not in "0123456789abcdef" for char in payload_sha256)
            or not isinstance(engine_build, str) or not engine_build
            or not isinstance(platform, str) or not platform
            or tangent_algorithm != BSP_TANGENT_ALGORITHM
            or format_version != BSP_DERIVED_FORMAT_VERSION):
        raise BSPPayloadError("invalid BSP derived-cache key input")
    digest = hashlib.sha256()
    digest.update(b"WOT-BSP-DERIVED-KEY\0")
    for value in (str(format_version), payload_sha256, engine_build, platform,
                  tangent_algorithm):
        encoded = value.encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "little"))
        digest.update(encoded)
    return digest.hexdigest()


def write_stripped_carrier(full_glb: Path, output_glb: Path, role: str) -> dict:
    """Write synthetic one-triangle-per-slot geometry with no retail vertices."""
    geometry = decode_bsp_glb(full_glb, role)
    groups = {}
    for section_index, section in enumerate(geometry["sections"]):
        # Separate tiny triangles avoid Interchange merging empty/duplicate
        # primitives while carrying no retail-derived position or index stream.
        # 2D GRID, not a line: Interchange scales GLB units x100, so a linear
        # 0.0001/section layout reaches 2.27 UU across Battle2_02's 227 slots
        # and trips StampCarrier's 1.0 UU tiny-contract bounds cap. A 32-wide
        # grid keeps any realistic slot count under 0.35 UU.
        x = (section_index % 32) * 0.0001
        y = (section_index // 32) * 0.0001
        groups[section["material_slot"]] = (
            [x, y, 0.0, x + 0.00001, y, 0.0, x, y + 0.00001, 0.0],
            [0.0, 0.0, 1.0] * 3,
            [0.0, 0.0, 1.0, 0.0, 0.0, 1.0],
            [0, 1, 2],
            [0.0, 0.0, 1.0, 0.0, 0.0, 1.0],
        )
    output_glb = Path(output_glb)
    output_glb.parent.mkdir(parents=True, exist_ok=True)
    # The filename is deliberately distinct, but the node/object name must stay
    # identical to the canonical asset referenced by the copied level.
    write_glb(output_glb, Path(full_glb).stem, groups)
    return geometry


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def texture_dimensions_sha256(conversion_dir: Path) -> str:
    """Hash the canonical texture-size table that controlled BSP UV0."""
    from bsp2obj import load_tex_sizes

    values = load_tex_sizes(Path(conversion_dir) / "tex")
    canonical = [[package, name, folder, source_name, width, height, draw_scale]
                 for (package, name), (folder, source_name, width, height, draw_scale)
                 in sorted(values.items())]
    return hashlib.sha256(json.dumps(
        canonical, sort_keys=True, separators=(",", ":"),
        ensure_ascii=True).encode("utf-8")).hexdigest()
