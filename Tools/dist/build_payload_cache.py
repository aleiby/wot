#!/usr/bin/env python3
"""Build or verify the player-local WoT Reborn payload cache.

Usage:
  python3 Tools/dist/build_payload_cache.py original/ /path/to/PayloadCache \
      --contract-version 2
  python3 Tools/dist/build_payload_cache.py original/ /path/to/PayloadCache \
      --contract-version 2 --verify
  python3 Tools/dist/build_payload_cache.py original/ /path/to/PayloadCache \
      --contract-version 2 --types textures,sounds
  python3 Tools/dist/build_payload_cache.py original/ /path/to/PayloadCache \
      --contract-version 2 --types bsps --bsp-conv-dir /path/to/current/conversion
  python3 Tools/dist/build_payload_cache.py --contract-version 3 \
      --emit-synthetic-fixtures Tools/dist/fixtures

The cache is deterministic and stdlib-only.  It reuses the verified decoders in
``Tools/convert/ue1pkg.py`` and stores loose content-addressed blobs plus a
canonical manifest.  Package markers are written only after all of a package's
blobs exist, so interrupted builds resume without trusting partial outputs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import struct
import sys
import tempfile
import time
from array import array
from collections import Counter
from pathlib import Path, PurePosixPath
from typing import Sequence


REPO = Path(__file__).resolve().parents[2]
CONVERT_DIR = REPO / "Tools" / "convert"
IMPORT_DIR = REPO / "Tools" / "import"
TOOLS_DIR = REPO / "Tools"
sys.path.insert(0, str(CONVERT_DIR))
sys.path.insert(0, str(IMPORT_DIR))
sys.path.insert(0, str(TOOLS_DIR))
from wot_config import original_dir  # noqa: E402
from import_all import MP_MAPS  # noqa: E402 - shared shipped multiplayer scope
from ue1pkg import (  # noqa: E402 - the standalone tool owns this path setup
    FRACTAL_CLASSES,
    Package,
    PayloadReader,
    _ScriptResolver,
    _read_class_defaults,
    _read_property_tag,
    decode_font,
    decode_fractal,
    decode_model,
    decode_mesh,
    decode_palette,
    decode_script_text,
    decode_sound,
    decode_texture,
    read_property_tags,
)
from bsp_payload import (  # noqa: E402
    BSP_ENCODING,
    BSP_TANGENT_ALGORITHM,
    MAX_BSP_COORDINATE_ABS,
    MAX_BSP_INDICES,
    MAX_BSP_PROVENANCE_INPUTS,
    MAX_BSP_SECTIONS,
    MAX_BSP_SLOT_NAME_BYTES,
    MAX_BSP_VERTICES,
    bsp_manifest_metadata,
    decode_bsp_glb,
    encode_bsp_payload,
    parse_bsp_payload,
    sha256_file,
    split_bsp_glb_parts,
    texture_dimensions_sha256,
)


CACHE_FORMAT = "wot-payload-cache"
CACHE_VERSION = 2
MARKER_VERSION = 2
HASH_ALGORITHM = "sha256"
TYPE_ORDER = ("textures", "sounds", "meshes", "bsps")
# BSPs remain opt-in so the legacy three-type bootstrap keeps working without
# requiring the converted outputs shared by all multiplayer maps.
DEFAULT_TYPES = ("textures", "sounds", "meshes")
TYPE_PREFIX = {
    "textures": "texture", "sounds": "sound", "meshes": "mesh", "bsps": "bsp",
}

TEXTURE_MAGIC = b"WOTPTEX"
TEXTURE_ENCODING = "ue1-p8-mips-rgba-palette-v1"
MESH_MAGIC = b"WOTPMESH"
MESH_ENCODING = "ue1-packed-verts-v1"
SOUND_ENCODING = "wav-v1"

# Cache-v3 typed-object framing.  The payload following the canonical header is
# deliberately still the frozen cache-v2 texture/WAV byte stream.
CACHE_V3_VERSION = 3
CACHE_V3_OBJECT_MAGIC = b"WOTOBJ3\0"
CACHE_V3_IMPORTER_VERSION = 1
MAX_CACHE_V3_HEADER_BYTES = 1024 * 1024
WORLD_SET_FORMAT = "wot-map-set-v3"
WORLD_SET_ROOT_DOMAIN = b"WOT-WORLD-SET-V3\0"
WORLD_MAP_ROOT_DOMAIN = b"WOT-WORLD-MAP-V3\0"
WORLD_MAP_SOURCE_ROOT_DOMAIN = b"WOT-WORLD-MAP-SOURCE-V3\0"
MAX_WORLD_ACTORS = 1_000_000
MAX_WORLD_REFERENCES = 4_000_000
MAX_WORLD_BSP_NODES = 2_000_000
MAX_WAV_SAMPLER_LOOPS = 16
MAX_CACHE_V3_MESH_VERTICES = 1_000_000
# A topology is expanded into canonical triangle, UV, render-vertex, section,
# collision, and JSON records.  Reserve 32 KiB for non-topology header members;
# the remaining 1 MiB header budget fits at most 1,520 worst-case triangles.
MAX_CACHE_V3_MESH_HEADER_BASE_BYTES = 32 * 1024
MAX_CACHE_V3_MESH_HEADER_BYTES_PER_TRIANGLE = 668
MAX_CACHE_V3_MESH_PAYLOAD_BYTES = 256 * 1024 * 1024
MAX_CACHE_V3_MESH_PAYLOAD_BASE_BYTES = 104
MAX_CACHE_V3_MESH_PAYLOAD_BYTES_PER_TRIANGLE = 122
MAX_CACHE_V3_MESH_WORKING_BYTES = 512 * 1024 * 1024
# Includes Python source/derived dictionaries, tuples, render-map keys, and
# temporary basis accumulators; intentionally much larger than native structs.
MAX_CACHE_V3_MESH_WORKING_BYTES_PER_TRIANGLE = 16 * 1024
MAX_CACHE_V3_MESH_TRIANGLES = min(
    (MAX_CACHE_V3_HEADER_BYTES - MAX_CACHE_V3_MESH_HEADER_BASE_BYTES)
    // MAX_CACHE_V3_MESH_HEADER_BYTES_PER_TRIANGLE,
    (MAX_CACHE_V3_MESH_PAYLOAD_BYTES - MAX_CACHE_V3_MESH_PAYLOAD_BASE_BYTES)
    // MAX_CACHE_V3_MESH_PAYLOAD_BYTES_PER_TRIANGLE,
    MAX_CACHE_V3_MESH_WORKING_BYTES
    // MAX_CACHE_V3_MESH_WORKING_BYTES_PER_TRIANGLE,
)
MAX_CACHE_V3_MESH_FRAMES = 1_000_000
MAX_CACHE_V3_MESH_SEQUENCES = 65_536
MAX_CACHE_V3_MESH_NOTIFIES = 65_536
MAX_CACHE_V3_PACKED_VERTICES = 64_000_000
MAX_CACHE_V3_FONT_PAGES = 256
MAX_CACHE_V3_FONT_GLYPHS = 65_536
MAX_CACHE_V3_FONT_PAGE_DIMENSION = 4096
MAX_CACHE_V3_FONT_ATLAS_PIXELS = 16 * 1024 * 1024
MAX_CACHE_V3_FONT_ATLAS_EXPANSION_BYTES = 64 * 1024 * 1024
MAX_CACHE_V3_FONT_GLYPH_PIXELS = 16 * 1024 * 1024
MAX_CACHE_V3_FONT_BDF_BYTES = 64 * 1024 * 1024
MAX_CACHE_V3_PROCEDURAL_STATES = 65_536
MAX_CACHE_V3_REGISTRY_ITEMS = 1_000_000
MAX_CACHE_V3_LOCALIZATION_ROWS = 1_000_000
PROCEDURAL_RANDOM_SEED = 1
MESH_TANGENT_RULE = "unweighted-unit-face-all-triangles-corner-uv-v1"
MESH_SUPPORTED_POLY_FLAGS = (
    0x00000001 | 0x00000002 | 0x00000004 | 0x00000010 |
    0x00000040 | 0x00000100 | 0x00000800 | 0x00004000 |
    0x00400000
)
MESH_INVISIBLE_POLY_FLAG = 0x00000001

# Version-2 allocation/preflight limits. These exact values are recorded in the
# manifest and are part of the reader contract; they are not caller-controlled.
MAX_ENTRIES = 10_000
MAX_BLOB_SIZE = 256 * 1024 * 1024
MAX_TEXTURE_DIMENSION = 8_192
MAX_TOTAL_EXPANDED_BYTES = 2 * 1024 * 1024 * 1024
MAX_TEXTURE_MIPS = 32
CACHE_LIMITS = {
    "max_blob_size": MAX_BLOB_SIZE,
    "max_entries": MAX_ENTRIES,
    "max_texture_dimension": MAX_TEXTURE_DIMENSION,
    "max_total_expanded_bytes": MAX_TOTAL_EXPANDED_BYTES,
    "max_bsp_vertices": MAX_BSP_VERTICES,
    "max_bsp_indices": MAX_BSP_INDICES,
    "max_bsp_sections": MAX_BSP_SECTIONS,
    "max_bsp_slot_name_bytes": MAX_BSP_SLOT_NAME_BYTES,
}
TYPE_ENCODINGS = {
    "textures": TEXTURE_ENCODING,
    "sounds": SOUND_ENCODING,
    "meshes": MESH_ENCODING,
    "bsps": BSP_ENCODING,
}

# Shared scope with Tools/import/import_all.py and Tools/dist/bootstrap.py.
# Three of these packages legitimately contain no Sound export; successful
# empty package markers are still useful resumability evidence.
SYSTEM_SOUND_PACKAGES = (
    "Angreal", "Environment", "Fire", "IpServer", "Legend", "MachinShin",
    "ParticleSystems", "UBrowser", "UWindow", "WOT", "WoTBase",
    "WoTBrowser", "WoTDecorations", "WoTPawns", "WoTTraps",
)


class CacheError(RuntimeError):
    """A deterministic cache build or verification failure."""


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _pretty_json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True)
            + "\n").encode("utf-8")


def _canonical_json_bytes(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    try:
        with temporary.open("wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _type_sort_key(kind: str) -> int:
    try:
        return TYPE_ORDER.index(kind)
    except ValueError as exc:
        raise CacheError(f"unknown payload type {kind!r}") from exc


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_sha256(value: object) -> bool:
    return (isinstance(value, str) and len(value) == 64
            and all(char in "0123456789abcdef" for char in value))


def parse_types(value: str) -> tuple[str, ...]:
    requested = {part.strip().lower() for part in value.split(",") if part.strip()}
    unknown = requested - set(TYPE_ORDER)
    if unknown:
        raise CacheError(
            "unknown --types value(s): " + ", ".join(sorted(unknown)))
    if not requested:
        raise CacheError("--types must select at least one payload type")
    return tuple(kind for kind in TYPE_ORDER if kind in requested)


def _validate_component(value: str, label: str, *, allow_separator: bool = False) -> None:
    if (not isinstance(value, str) or not value
            or (not allow_separator and "__" in value)
            or any(char in value for char in "/\\\0\r\n")
            or any(ord(char) < 32 or ord(char) == 127 for char in value)
            or value in (".", "..")):
        raise CacheError(f"unsafe {label} {value!r}")


def _entry_key(kind: str, package: str, name: str) -> str:
    if kind not in TYPE_PREFIX:
        raise CacheError(f"unknown payload type {kind!r}")
    _validate_component(package, "package name")
    # The separator is parsed at its first occurrence. Retail sound names such
    # as Stop__5 may contain it; package names may not.
    _validate_component(name, "export name", allow_separator=True)
    return f"{TYPE_PREFIX[kind]}:{Package.qualkey(package, name)}"


def _blob_relative_path(sha256: str) -> str:
    if not _is_sha256(sha256):
        raise CacheError(f"invalid blob SHA-256 {sha256!r}")
    return f"blobs/{sha256[:2]}/{sha256}.blob"


def _relative_cache_path(cache_dir: Path, relative: str) -> Path:
    if not isinstance(relative, str):
        raise CacheError(f"unsafe cache-relative path {relative!r}")
    path = PurePosixPath(relative)
    if (path.is_absolute() or not path.parts or path.as_posix() != relative
            or any(part in ("", ".", "..") for part in path.parts)):
        raise CacheError(f"unsafe cache-relative path {relative!r}")
    return cache_dir.joinpath(*path.parts)


def _validate_source_path(relative: object, label: str) -> None:
    if not isinstance(relative, str):
        raise CacheError(f"invalid {label} {relative!r}")
    path = PurePosixPath(relative)
    if (path.is_absolute() or not path.parts or path.as_posix() != relative
            or any(part in ("", ".", "..") for part in path.parts)
            or any("\\" in part or any(ord(char) < 32 or ord(char) == 127
                                        for char in part)
                   for part in path.parts)):
        raise CacheError(f"invalid {label} {relative!r}")


def _source_relative(retail_root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(retail_root.resolve()).as_posix()
    except ValueError as exc:
        raise CacheError(f"source package escaped retail root: {path}") from exc


def _source_record(retail_root: Path, kind: str, path: Path) -> dict:
    return {
        "type": kind,
        "path": _source_relative(retail_root, path),
        "size": path.stat().st_size,
        "sha256": _sha256_file(path),
    }


def source_packages(retail_root: Path, kind: str) -> list[Path]:
    if kind == "bsps":
        maps = [retail_root / "Maps" / f"{name}.wot" for name in MP_MAPS]
        missing = [path for path in maps if not path.is_file()]
        if missing:
            raise CacheError(f"missing multiplayer BSP source: {missing[0]}")
        return maps

    system = retail_root / "System"
    if not system.is_dir():
        raise CacheError(f"missing retail System directory: {system}")

    if kind == "textures":
        textures = retail_root / "Textures"
        if not textures.is_dir():
            raise CacheError(f"missing retail Textures directory: {textures}")
        packages = list(textures.glob("*.utx")) + list(system.glob("*.u"))
    elif kind == "sounds":
        sounds = retail_root / "Sounds"
        if not sounds.is_dir():
            raise CacheError(f"missing retail Sounds directory: {sounds}")
        embedded = [system / f"{name}.u" for name in SYSTEM_SOUND_PACKAGES]
        missing = [path for path in embedded if not path.is_file()]
        if missing:
            raise CacheError(f"missing embedded-sound package: {missing[0]}")
        packages = list(sounds.glob("*.uax")) + embedded
    elif kind == "meshes":
        packages = list(system.glob("*.u"))
    else:
        raise CacheError(f"unknown payload type {kind!r}")

    if not packages:
        raise CacheError(f"no retail packages found for {kind}")
    return sorted(packages, key=lambda path: (
        _source_relative(retail_root, path).casefold(),
        _source_relative(retail_root, path),
    ))


def encode_texture_payload(texture: dict, palette: Sequence[Sequence[int]]) -> bytes:
    mips = texture.get("mips")
    if not isinstance(mips, list) or not mips:
        raise CacheError(f"{texture.get('name', '<texture>')}: no serialized mips")
    if len(palette) != 256:
        raise CacheError(
            f"{texture.get('name', '<texture>')}: palette has {len(palette)} colors")

    flags = 1 if texture.get("masked") else 0
    payload = bytearray(struct.pack(
        "<8sHHII", TEXTURE_MAGIC, 1, flags, len(palette), len(mips)))
    for color in palette:
        if len(color) != 4 or any(not isinstance(channel, int) or not 0 <= channel <= 255
                                  for channel in color):
            raise CacheError(
                f"{texture.get('name', '<texture>')}: invalid RGBA palette entry")
        payload.extend(bytes(color))

    for mip_index, mip in enumerate(mips):
        if not isinstance(mip, tuple) or len(mip) != 3:
            raise CacheError(
                f"{texture.get('name', '<texture>')}: malformed mip {mip_index}")
        width, height, indices = mip
        if (not isinstance(width, int) or not isinstance(height, int)
                or isinstance(width, bool) or isinstance(height, bool)
                or width <= 0 or height <= 0
                or width > MAX_TEXTURE_DIMENSION
                or height > MAX_TEXTURE_DIMENSION):
            raise CacheError(
                f"{texture.get('name', '<texture>')}: invalid mip {mip_index} "
                f"dimensions {width}x{height}")
        indices = bytes(indices)
        if len(indices) != width * height:
            raise CacheError(
                f"{texture.get('name', '<texture>')}: mip {mip_index} has "
                f"{len(indices)} indices, expected {width * height}")
        payload.extend(struct.pack("<III", width, height, len(indices)))
        payload.extend(indices)
    return bytes(payload)


def encode_mesh_payload(mesh: dict) -> bytes:
    frame_verts = mesh["frame_verts"]
    anim_frames = mesh["anim_frames"]
    vertices = mesh["packed_vertices"]
    expected = frame_verts * anim_frames
    if frame_verts < 0 or anim_frames < 0 or len(vertices) != expected:
        raise CacheError(
            f"{mesh.get('name', '<mesh>')}: {len(vertices)} packed vertices, "
            f"expected {frame_verts}*{anim_frames}={expected}")
    packed = array("I", vertices)
    if packed.itemsize != 4:
        raise CacheError("host unsigned-int array is not 32 bits")
    if sys.byteorder != "little":
        packed.byteswap()
    return (struct.pack(
        "<8sHHIII", MESH_MAGIC, 1, 0, frame_verts, anim_frames, len(vertices))
            + packed.tobytes())


def parse_wav_metadata(payload: bytes, label: str = "WAV") -> dict:
    """Parse and validate the PCM fields needed by the version-1 reader."""
    payload = bytes(payload)
    if len(payload) > MAX_BLOB_SIZE:
        raise CacheError(f"{label}: WAV exceeds max blob size")
    if len(payload) < 12 or payload[:4] != b"RIFF" or payload[8:12] != b"WAVE":
        raise CacheError(f"{label}: invalid WAV RIFF header")
    riff_size = struct.unpack_from("<I", payload, 4)[0]
    if riff_size != len(payload) - 8:
        raise CacheError(
            f"{label}: RIFF length {riff_size + 8} does not match {len(payload)} bytes")

    offset = 12
    fmt_fields = None
    data_length = None
    while offset < len(payload):
        if offset + 8 > len(payload):
            raise CacheError(f"{label}: truncated WAV chunk header")
        chunk_id = payload[offset:offset + 4]
        chunk_size = struct.unpack_from("<I", payload, offset + 4)[0]
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_size
        padded_end = chunk_end + (chunk_size & 1)
        if chunk_end < chunk_start or padded_end > len(payload):
            raise CacheError(f"{label}: truncated {chunk_id!r} WAV chunk")
        if chunk_id == b"fmt ":
            if fmt_fields is not None:
                raise CacheError(f"{label}: duplicate fmt WAV chunk")
            if chunk_size < 16:
                raise CacheError(f"{label}: short fmt WAV chunk")
            fmt_fields = struct.unpack_from("<HHIIHH", payload, chunk_start)
        elif chunk_id == b"data":
            if data_length is not None:
                raise CacheError(f"{label}: duplicate data WAV chunk")
            data_length = chunk_size
        offset = padded_end

    if fmt_fields is None or data_length is None:
        raise CacheError(f"{label}: WAV requires one fmt and one data chunk")
    (format_tag, channels, sample_rate, byte_rate,
     block_align, bits_per_sample) = fmt_fields
    if format_tag != 1:
        raise CacheError(f"{label}: WAV format tag {format_tag} is not PCM")
    if not 1 <= channels <= 8:
        raise CacheError(f"{label}: invalid PCM channel count {channels}")
    if not 1 <= sample_rate <= 384_000:
        raise CacheError(f"{label}: invalid PCM sample rate {sample_rate}")
    if bits_per_sample not in (8, 16):
        raise CacheError(f"{label}: unsupported PCM bit depth {bits_per_sample}")
    expected_align = channels * (bits_per_sample // 8)
    if block_align != expected_align:
        raise CacheError(
            f"{label}: PCM block align {block_align} != {expected_align}")
    if byte_rate != sample_rate * block_align:
        raise CacheError(
            f"{label}: PCM byte rate {byte_rate} != {sample_rate * block_align}")
    if data_length % block_align:
        raise CacheError(
            f"{label}: PCM data length {data_length} is not frame-aligned")
    frame_count = data_length // block_align
    return {
        "pcm_format_tag": format_tag,
        "channels": channels,
        "sample_rate": sample_rate,
        "bits_per_sample": bits_per_sample,
        "frame_count": frame_count,
        "data_byte_length": data_length,
        "duration_seconds": frame_count / sample_rate,
    }


def _parse_wav_loops(payload: bytes, frame_count: int,
                     label: str = "WAV") -> list[dict]:
    """Return bounded PCM sampler loops in canonical half-open frame form."""
    offset = 12
    loops = []
    saw_smpl = False
    while offset < len(payload):
        chunk_id = payload[offset:offset + 4]
        chunk_size = struct.unpack_from("<I", payload, offset + 4)[0]
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_size
        if chunk_id == b"smpl":
            if saw_smpl:
                raise CacheError(f"{label}: duplicate smpl WAV chunk")
            saw_smpl = True
            if chunk_size < 36:
                raise CacheError(f"{label}: short smpl WAV chunk")
            loop_count = struct.unpack_from("<I", payload, chunk_start + 28)[0]
            sampler_bytes = struct.unpack_from("<I", payload, chunk_start + 32)[0]
            if loop_count > MAX_WAV_SAMPLER_LOOPS:
                raise CacheError(
                    f"{label}: PCM sampler loop count {loop_count} exceeds "
                    f"{MAX_WAV_SAMPLER_LOOPS}")
            required = 36 + loop_count * 24 + sampler_bytes
            if required > chunk_size:
                raise CacheError(f"{label}: truncated PCM sampler loop data")
            for loop_index in range(loop_count):
                loop_offset = chunk_start + 36 + loop_index * 24
                (_cue_id, loop_type, start, end, _fraction,
                 play_count) = struct.unpack_from("<6I", payload, loop_offset)
                if loop_type != 0:
                    raise CacheError(
                        f"{label}: PCM sampler loop {loop_index} is not forward")
                if start > end or end >= frame_count:
                    raise CacheError(
                        f"{label}: PCM loop {loop_index} {start}..{end} is "
                        f"outside {frame_count} frames")
                loops.append({
                    "end_frame_exclusive": end + 1,
                    "play_count": play_count,
                    "start_frame": start,
                })
        offset = chunk_end + (chunk_size & 1)
    return loops


def encode_cache_v3_object(header: dict, payload: bytes = b"") -> bytes:
    """Frame one canonical typed-object header and its already-bounded body."""
    payload = bytes(payload)
    header_bytes = _canonical_json_bytes(header)
    if not header_bytes or len(header_bytes) > MAX_CACHE_V3_HEADER_BYTES:
        raise CacheError("cache-v3 object header exceeds its byte bound")
    if len(payload) > MAX_BLOB_SIZE:
        raise CacheError("cache-v3 object payload exceeds its byte bound")
    return (CACHE_V3_OBJECT_MAGIC + struct.pack("<I", len(header_bytes))
            + header_bytes + payload)


def _cache_v3_source(source_path: str, source_export: str,
                     source_bytes: bytes) -> dict:
    _validate_source_path(source_path, "cache-v3 source path")
    if not isinstance(source_export, str) or not source_export:
        raise CacheError("cache-v3 source export must be non-empty")
    if any(not component for component in source_export.split(".")):
        raise CacheError(
            "cache-v3 source export has an empty identity component")

    path_components = source_path.split("/")
    leaf = path_components[-1]
    stem, separator, suffix = leaf.rpartition(".")
    if not separator:
        stem = leaf
    elif not stem or not suffix:
        raise CacheError("cache-v3 source path has an invalid extension")
    identity_components = path_components[:-1] + [stem]
    if any("." in component for component in identity_components):
        raise CacheError(
            "cache-v3 source path has a dotted package identity component")
    return {
        "export": source_export,
        "package": source_path,
        "sha256": _sha256_bytes(bytes(source_bytes)),
    }


def encode_cache_v3_texture_object(texture: dict,
                                   palette: Sequence[Sequence[int]], *,
                                   fact_id: str, source_path: str,
                                   source_export: str,
                                   source_bytes: bytes) -> bytes:
    payload = encode_texture_payload(texture, palette)
    mips = []
    for width, height, indices in texture["mips"]:
        mips.append({
            "expanded_rgba_byte_length": width * height * 4,
            "height": height,
            "index_byte_length": len(indices),
            "width": width,
        })
    header = {
        "anim_next": str(texture.get("anim_next_identity", "")),
        "color": {
            "alpha": "index-zero-if-masked",
            "channel_order": "rgba",
            "palette_colors": len(palette),
            "stored_palette_alpha": "preserved-but-non-semantic",
        },
        "decoded_layout": {
            "mip_count": len(mips),
            "mips": mips,
            "texel_format": "p8",
        },
        "encoding": TEXTURE_ENCODING,
        "fact_id": fact_id,
        "importer": "texture-v2",
        "importer_version": 2,
        "kind": "texture",
        "masked": bool(texture.get("masked")),
        "max_frame_rate_bits": struct.unpack(
            "<I", struct.pack("<f", float(texture.get("max_frame_rate", 0.0))))[0],
        "min_frame_rate_bits": struct.unpack(
            "<I", struct.pack("<f", float(texture.get("min_frame_rate", 0.0))))[0],
        "payload_byte_length": len(payload),
        "prime_count": int(texture.get("prime_count", 0)),
        "source": _cache_v3_source(
            source_path, source_export, source_bytes),
    }
    return encode_cache_v3_object(header, payload)


def encode_cache_v3_sound_object(wav: bytes, *, fact_id: str,
                                 source_path: str, source_export: str,
                                 source_bytes: bytes) -> bytes:
    wav = bytes(wav)
    metadata = parse_wav_metadata(wav, source_export)
    loops = _parse_wav_loops(wav, metadata["frame_count"], source_export)
    header = {
        "audio": {
            "bits_per_sample": metadata["bits_per_sample"],
            "channels": metadata["channels"],
            "format_tag": metadata["pcm_format_tag"],
            "sample_rate": metadata["sample_rate"],
        },
        "decoded_layout": {
            "data_byte_length": metadata["data_byte_length"],
            "frame_count": metadata["frame_count"],
            "interleaved": True,
        },
        "duration": {
            "frames": metadata["frame_count"],
            "sample_rate": metadata["sample_rate"],
        },
        "encoding": SOUND_ENCODING,
        "fact_id": fact_id,
        "importer": "sound-v1",
        "importer_version": CACHE_V3_IMPORTER_VERSION,
        "kind": "sound",
        "loops": loops,
        "payload_byte_length": len(wav),
        "source": _cache_v3_source(
            source_path, source_export, source_bytes),
    }
    return encode_cache_v3_object(header, wav)


def _mesh_float_bits(value: float, label: str) -> int:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise CacheError(f"{label} is not numeric")
    value = float(value)
    if not math.isfinite(value):
        raise CacheError(f"{label} is not finite")
    try:
        bits = struct.unpack("<I", struct.pack("<f", value))[0]
    except OverflowError as exc:
        raise CacheError(f"{label} is outside finite float32") from exc
    # Canonicalize negative zero: IEEE-754 spells zero two ways and -0.0 ==
    # +0.0, so which one a bound carries depends only on the arithmetic path
    # taken. The native encoder reached +0.0 for a bound this one reached as
    # -0.0 and byte parity failed over a value that is not numerically
    # different. Canonical JSON already refuses "-0" for integers; same rule.
    return 0 if bits == 0x80000000 else bits


def _mesh_f32(value: float, label: str) -> float:
    return struct.unpack("<f", struct.pack(
        "<I", _mesh_float_bits(value, label)))[0]


def _mesh_sign_extend(value: int, bits: int) -> int:
    value &= (1 << bits) - 1
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def _mesh_topology_preflight(triangle_count: int, label: str) -> None:
    if not _is_int(triangle_count) or triangle_count < 0:
        raise CacheError(f"{label} triangle count is invalid")
    worst_header = (MAX_CACHE_V3_MESH_HEADER_BASE_BYTES
                    + triangle_count
                    * MAX_CACHE_V3_MESH_HEADER_BYTES_PER_TRIANGLE)
    worst_payload = (MAX_CACHE_V3_MESH_PAYLOAD_BASE_BYTES
                     + triangle_count
                     * MAX_CACHE_V3_MESH_PAYLOAD_BYTES_PER_TRIANGLE)
    worst_working = (triangle_count
                     * MAX_CACHE_V3_MESH_WORKING_BYTES_PER_TRIANGLE)
    if (triangle_count > MAX_CACHE_V3_MESH_TRIANGLES
            or worst_header > MAX_CACHE_V3_HEADER_BYTES
            or worst_payload > MAX_CACHE_V3_MESH_PAYLOAD_BYTES
            or worst_working > MAX_CACHE_V3_MESH_WORKING_BYTES):
        raise CacheError(
            f"{label} topology exceeds canonical header/payload/working-set bounds")


def _mesh_rotation_sin_cos(units: int) -> tuple[float, float]:
    """UE1 serialized rotation units, with exact cardinal quadrants."""
    wrapped = units & 0xffff
    quadrants = {
        0: (0.0, 1.0),
        16384: (1.0, 0.0),
        32768: (0.0, -1.0),
        49152: (-1.0, 0.0),
    }
    if wrapped in quadrants:
        return quadrants[wrapped]
    angle = wrapped * (2.0 * math.pi / 65536.0)
    return math.sin(angle), math.cos(angle)


def _mesh_rotate_origin(point: Sequence[float],
                        rotation: Sequence[int]) -> tuple[float, float, float]:
    """Match mesh2glb.rot_origin: roll, then pitch, then yaw."""
    pitch, yaw, roll = rotation
    x, y, z = point
    if roll:
        sine, cosine = _mesh_rotation_sin_cos(roll)
        y, z = y * cosine + z * sine, -y * sine + z * cosine
    if pitch:
        sine, cosine = _mesh_rotation_sin_cos(pitch)
        x, z = x * cosine - z * sine, x * sine + z * cosine
    if yaw:
        sine, cosine = _mesh_rotation_sin_cos(yaw)
        x, y = x * cosine - y * sine, x * sine + y * cosine
    return tuple(_mesh_f32(value, f"rotated vertex axis {axis}")
                 for axis, value in enumerate((x, y, z)))


def _mesh_position(packed: int, origin: Sequence[float],
                   scale: Sequence[float],
                   rotation: Sequence[int] = (0, 0, 0)
                   ) -> tuple[float, float, float]:
    raw = (
        _mesh_sign_extend(packed, 11),
        _mesh_sign_extend(packed >> 11, 11),
        _mesh_sign_extend(packed >> 22, 10),
    )
    result = []
    for axis in range(3):
        # Spell out float32 operations so the native and Python bounds use the
        # same arithmetic domain instead of Python's implicit float64 domain.
        difference = _mesh_f32(
            _mesh_f32(raw[axis], f"packed vertex axis {axis}")
            - _mesh_f32(origin[axis], f"mesh origin axis {axis}"),
            f"transformed vertex difference axis {axis}")
        result.append(_mesh_f32(
            difference * _mesh_f32(scale[axis], f"mesh scale axis {axis}"),
            f"transformed vertex axis {axis}"))
    return (_mesh_rotate_origin(result, rotation)
            if any(rotation) else tuple(result))


def _mesh_smooth_normals(
        points: Sequence[Sequence[float]],
        triangle_indices: Sequence[Sequence[int]],
        label: str) -> list[tuple[float, float, float]]:
    """Exact mesh2glb accumulation: normalized faces, including attachments."""
    accumulators = [[0.0, 0.0, 0.0] for _point in points]
    for triangle_index, indices in enumerate(triangle_indices):
        a, b, c = (points[index] for index in indices)
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        fx, fy, fz = (uy * vz - uz * vy, uz * vx - ux * vz,
                      ux * vy - uy * vx)
        length = math.sqrt(fx * fx + fy * fy + fz * fz + 0.001)
        face = (fx / length, fy / length, fz / length)
        if not all(math.isfinite(value) for value in face):
            raise CacheError(f"{label}: triangle {triangle_index} normal is not finite")
        for index in indices:
            for axis in range(3):
                accumulators[index][axis] += face[axis]
    normals = []
    for vertex_index, value in enumerate(accumulators):
        length = math.sqrt(sum(component * component for component in value)) or 1.0
        normal = tuple(component / length for component in value)
        if not all(math.isfinite(component) for component in normal):
            raise CacheError(f"{label}: vertex {vertex_index} normal is not finite")
        normals.append(normal)
    return normals


def _mesh_corner_uv_tangents(
        points: Sequence[Sequence[float]],
        normals: Sequence[Sequence[float]],
        triangles: Sequence[dict],
        uvs: Sequence[dict],
        render_vertices: Sequence[dict],
        label: str) -> list[tuple[float, float, float, float]]:
    """UV-derivative tangents, Gram-Schmidt orthogonalized per render vertex."""
    tangent_sum = [[0.0, 0.0, 0.0] for _render in render_vertices]
    bitangent_sum = [[0.0, 0.0, 0.0] for _render in render_vertices]
    for triangle_index, triangle in enumerate(triangles):
        uv_indices = triangle["uv_indices"]
        if uv_indices is None:
            continue
        a, b, c = (points[index] for index in triangle["indices"])
        uv_a, uv_b, uv_c = (
            (uvs[index]["u"] / 255.0, uvs[index]["v"] / 255.0)
            for index in uv_indices)
        edge1 = tuple(b[axis] - a[axis] for axis in range(3))
        edge2 = tuple(c[axis] - a[axis] for axis in range(3))
        du1, dv1 = uv_b[0] - uv_a[0], uv_b[1] - uv_a[1]
        du2, dv2 = uv_c[0] - uv_a[0], uv_c[1] - uv_a[1]
        determinant = du1 * dv2 - du2 * dv1
        if determinant == 0.0:
            continue
        reciprocal = 1.0 / determinant
        tangent = tuple(
            (edge1[axis] * dv2 - edge2[axis] * dv1) * reciprocal
            for axis in range(3))
        bitangent = tuple(
            (edge2[axis] * du1 - edge1[axis] * du2) * reciprocal
            for axis in range(3))
        if not all(math.isfinite(value) for value in tangent + bitangent):
            raise CacheError(
                f"{label}: triangle {triangle_index} tangent is not finite")
        for render_index in triangle["render_indices"]:
            for axis in range(3):
                tangent_sum[render_index][axis] += tangent[axis]
                bitangent_sum[render_index][axis] += bitangent[axis]

    result = []
    for render_index, render in enumerate(render_vertices):
        normal = normals[render["source_vertex"]]
        tangent = tangent_sum[render_index]
        projection = sum(normal[axis] * tangent[axis] for axis in range(3))
        tangent = [tangent[axis] - normal[axis] * projection for axis in range(3)]
        length = math.sqrt(sum(value * value for value in tangent))
        if length == 0.0:
            if sum(value * value for value in normal) == 0.0:
                tangent = [1.0, 0.0, 0.0]
            else:
                axis = ((1.0, 0.0, 0.0) if abs(normal[0]) < 0.9
                        else (0.0, 1.0, 0.0))
                tangent = [
                    axis[1] * normal[2] - axis[2] * normal[1],
                    axis[2] * normal[0] - axis[0] * normal[2],
                    axis[0] * normal[1] - axis[1] * normal[0],
                ]
                length = math.sqrt(sum(value * value for value in tangent))
                tangent = [value / length for value in tangent]
        else:
            tangent = [value / length for value in tangent]
        cross = (
            normal[1] * tangent[2] - normal[2] * tangent[1],
            normal[2] * tangent[0] - normal[0] * tangent[2],
            normal[0] * tangent[1] - normal[1] * tangent[0],
        )
        handedness = (-1.0 if sum(
            cross[axis] * bitangent_sum[render_index][axis]
            for axis in range(3)) < 0.0 else 1.0)
        basis = (*tangent, handedness)
        if not all(math.isfinite(value) for value in basis):
            raise CacheError(f"{label}: render tangent {render_index} is not finite")
        result.append(basis)
    return result


def _mesh_skin(package: Package, reference: int, slot: int) -> dict:
    if not isinstance(reference, int) or isinstance(reference, bool):
        raise CacheError(f"mesh skin slot {slot} has a non-integer object reference")
    identity = package.objref(reference)
    if identity is None:
        return {"logical_id": None, "name": None, "package": None}
    package_name, object_name = identity
    if (not package_name or not object_name or "." in package_name
            or "." in object_name or "__" in package_name):
        raise CacheError(f"mesh skin slot {slot} has an unsafe semantic identity")
    return {
        "logical_id": Package.qualkey(package_name, object_name),
        "name": object_name,
        "package": package_name,
    }


def _derive_cache_v3_mesh(mesh: dict, package: Package) -> dict:
    """Validate and canonicalize only facts already decoded by decode_mesh."""
    name = mesh.get("name", "<mesh>")
    class_name = mesh.get("class_name")
    if class_name not in ("Mesh", "LodMesh"):
        raise CacheError(f"{name}: unsupported vertex-mesh class {class_name!r}")
    vertex_count = mesh.get("frame_verts")
    frame_count = mesh.get("anim_frames")
    packed_vertices = mesh.get("packed_vertices")
    triangles_source = mesh.get("triangles")
    sequences_source = mesh.get("sequences")
    textures = mesh.get("textures")
    if (not _is_int(vertex_count) or not 0 < vertex_count <= MAX_CACHE_V3_MESH_VERTICES
            or not _is_int(frame_count) or not 0 < frame_count <= MAX_CACHE_V3_MESH_FRAMES
            or not isinstance(packed_vertices, list)
            or not isinstance(triangles_source, list)
            or not triangles_source
            or not isinstance(sequences_source, list)
            or len(sequences_source) > MAX_CACHE_V3_MESH_SEQUENCES
            or not isinstance(textures, list)):
        raise CacheError(f"{name}: mesh dimensions or decoded arrays exceed bounds")
    _mesh_topology_preflight(len(triangles_source), name)
    expected_vertices = vertex_count * frame_count
    if (expected_vertices > MAX_CACHE_V3_PACKED_VERTICES
            or len(packed_vertices) != expected_vertices
            or any(not _is_int(value) or not 0 <= value <= 0xffffffff
                   for value in packed_vertices)):
        raise CacheError(f"{name}: packed frame stream is inconsistent or oversized")

    origin = mesh.get("origin")
    scale = mesh.get("scale")
    rotation_origin = mesh.get("rotation_origin")
    if (not isinstance(origin, tuple) or len(origin) != 3
            or not isinstance(scale, tuple) or len(scale) != 3
            or not isinstance(rotation_origin, tuple) or len(rotation_origin) != 3
            or any(not _is_int(value) or not -0x80000000 <= value <= 0x7fffffff
                   for value in rotation_origin)):
        raise CacheError(f"{name}: origin/scale/rotation vectors are malformed")
    origin_bits = [_mesh_float_bits(value, f"{name} origin") for value in origin]
    scale_bits = [_mesh_float_bits(value, f"{name} scale") for value in scale]

    skins = [_mesh_skin(package, reference, slot)
             for slot, reference in enumerate(textures)]
    triangle_parts = []
    section_keys = []
    for triangle_index, triangle in enumerate(triangles_source):
        if not isinstance(triangle, dict):
            raise CacheError(f"{name}: triangle {triangle_index} is malformed")
        indices = triangle.get("indices")
        uvs = triangle.get("uvs")
        discarded = bool(triangle.get("discarded_import_fields", False))
        lod_special = bool(triangle.get("lod_special", False))
        if (not isinstance(indices, tuple) or len(indices) != 3
                or any(not _is_int(index) or not 0 <= index < vertex_count
                       for index in indices)):
            raise CacheError(f"{name}: triangle {triangle_index} vertex is out of range")
        if discarded:
            if class_name != "LodMesh" or not lod_special:
                raise CacheError(
                    f"{name}: only LodMesh attachment faces may omit import fields")
            uv_values = None
            poly_flags = None
            texture_index = None
            section_key = (None, None, True)
        else:
            poly_flags = triangle.get("poly_flags")
            texture_index = triangle.get("texture_index")
            if (not _is_int(poly_flags) or not 0 <= poly_flags <= 0xffffffff
                    or poly_flags & ~MESH_SUPPORTED_POLY_FLAGS):
                raise CacheError(
                    f"{name}: triangle {triangle_index} has unsupported polygon flags")
            if (not _is_int(texture_index)
                    or not 0 <= texture_index < len(skins)):
                raise CacheError(
                    f"{name}: triangle {triangle_index} texture slot is out of range")
            if (not isinstance(uvs, tuple) or len(uvs) != 3
                    or any(not isinstance(uv, tuple) or len(uv) != 2
                           or any(not _is_int(value) or not 0 <= value <= 255
                                  for value in uv)
                           for uv in uvs)):
                raise CacheError(f"{name}: triangle {triangle_index} UVs are malformed")
            uv_values = tuple(uvs)
            section_key = (poly_flags, texture_index, False)
        triangle_parts.append({
            "indices": tuple(indices),
            "lod_attachment": lod_special,
            "poly_flags": poly_flags,
            "texture_index": texture_index,
            "uv_values": uv_values,
        })
        section_keys.append(section_key)

    sections = []
    triangle_sections = []
    for triangle_index, section_key in enumerate(section_keys):
        if not sections or sections[-1]["_key"] != section_key:
            poly_flags, texture_index, lod_attachment = section_key
            sections.append({
                "_key": section_key,
                "first_triangle": triangle_index,
                "lod_attachment": lod_attachment,
                "poly_flags": poly_flags,
                "skin_index": texture_index,
                "triangle_count": 0,
            })
        triangle_sections.append(len(sections) - 1)
        sections[-1]["triangle_count"] += 1
    for section in sections:
        del section["_key"]

    uvs = []
    render_vertices = []
    render_lookup = {}
    triangles = []
    for triangle_index, part in enumerate(triangle_parts):
        uv_indices = None
        if part["uv_values"] is not None:
            uv_indices = []
            for uv in part["uv_values"]:
                uv_indices.append(len(uvs))
                uvs.append({"u": uv[0], "v": uv[1]})
        render_indices = []
        for corner, source_vertex in enumerate(part["indices"]):
            uv_index = None if uv_indices is None else uv_indices[corner]
            # Material/flag boundaries deliberately split render vertices;
            # source indices remain the collision/animation identity.
            key = (source_vertex, None if uv_index is None else tuple(
                part["uv_values"][corner]), part["texture_index"],
                part["poly_flags"], part["lod_attachment"])
            render_index = render_lookup.get(key)
            if render_index is None:
                render_index = len(render_vertices)
                render_lookup[key] = render_index
                render_vertices.append({
                    "skin_index": part["texture_index"],
                    "source_vertex": source_vertex,
                    "uv_index": uv_index,
                })
            render_indices.append(render_index)
        triangles.append({
            "indices": list(part["indices"]),
            "lod_attachment": part["lod_attachment"],
            "poly_flags": part["poly_flags"],
            "render_indices": render_indices,
            "section_index": triangle_sections[triangle_index],
            "texture_index": part["texture_index"],
            "uv_indices": uv_indices,
        })

    collision_triangles = [
        index for index, part in enumerate(triangle_parts)
        if part["poly_flags"] is not None
        and not (part["poly_flags"] & MESH_INVISIBLE_POLY_FLAG)
    ]

    minimum = [float("inf")] * 3
    maximum = [float("-inf")] * 3
    for packed in packed_vertices:
        point = _mesh_position(packed, origin, scale, rotation_origin)
        for axis in range(3):
            minimum[axis] = min(minimum[axis], point[axis])
            maximum[axis] = max(maximum[axis], point[axis])
    bounds = {
        "max_bits": [_mesh_float_bits(value, f"{name} maximum bound")
                     for value in maximum],
        "min_bits": [_mesh_float_bits(value, f"{name} minimum bound")
                     for value in minimum],
    }
    base_points = [
        _mesh_position(packed, origin, scale, rotation_origin)
        for packed in packed_vertices[:vertex_count]
    ]
    base_normals = _mesh_smooth_normals(
        base_points, [part["indices"] for part in triangle_parts], name)
    _mesh_corner_uv_tangents(
        base_points, base_normals, triangles, uvs, render_vertices, name)

    sequences = []
    seen_sequences = set()
    notify_total = 0
    for sequence_index, sequence in enumerate(sequences_source):
        if not isinstance(sequence, dict):
            raise CacheError(f"{name}: sequence {sequence_index} is malformed")
        sequence_name = sequence.get("name")
        group = sequence.get("group")
        first = sequence.get("start")
        count = sequence.get("num")
        notifications = sequence.get("notifications")
        if (not isinstance(sequence_name, str) or not sequence_name
                or not isinstance(group, str) or not group
                or not _is_int(first) or not _is_int(count)
                or first < 0 or count <= 0 or first >= frame_count
                or count > frame_count + 1 - first
                or not isinstance(notifications, list)):
            raise CacheError(
                f"{name}: sequence {sequence_index} range/metadata is invalid")
        folded_name = sequence_name.casefold()
        if folded_name in seen_sequences:
            raise CacheError(
                f"{name}: overlapping sequence identity {sequence_name!r}")
        seen_sequences.add(folded_name)
        rate_bits = _mesh_float_bits(sequence.get("rate"),
                                     f"{name} sequence {sequence_name} rate")
        rate = struct.unpack("<f", struct.pack("<I", rate_bits))[0]
        if rate <= 0:
            raise CacheError(f"{name}: sequence {sequence_name} rate is not positive")
        notify_total += len(notifications)
        if notify_total > MAX_CACHE_V3_MESH_NOTIFIES:
            raise CacheError(f"{name}: animation notifications exceed their bound")
        notifies = []
        for notify_index, notify in enumerate(notifications):
            function = notify.get("function") if isinstance(notify, dict) else None
            time_bits = _mesh_float_bits(
                notify.get("time") if isinstance(notify, dict) else None,
                f"{name} sequence {sequence_name} notify {notify_index} time")
            time = struct.unpack("<f", struct.pack("<I", time_bits))[0]
            if not isinstance(function, str) or not function or not 0 <= time <= 1:
                raise CacheError(
                    f"{name}: sequence {sequence_name} notify {notify_index} is invalid")
            notifies.append({"function": function, "time_bits": time_bits})
        sequences.append({
            "first_frame": first,
            "group": group,
            "name": sequence_name,
            "notifies": notifies,
            "num_frames": count,
            "rate_bits": rate_bits,
        })

    lod_source = mesh.get("lod")
    if class_name == "Mesh":
        if lod_source is not None:
            raise CacheError(f"{name}: plain Mesh unexpectedly carries LodMesh facts")
        lod = None
    else:
        if not isinstance(lod_source, dict):
            raise CacheError(f"{name}: LodMesh facts are absent")
        model_vertices = lod_source.get("model_verts")
        special_vertices = lod_source.get("special_verts")
        old_frame_vertices = lod_source.get("old_frame_verts")
        remap = lod_source.get("remap_anim_verts")
        if (not _is_int(model_vertices) or model_vertices < 0
                or not _is_int(special_vertices) or not 0 <= special_vertices <= vertex_count
                or not _is_int(old_frame_vertices) or old_frame_vertices < 0
                or not isinstance(remap, list)):
            raise CacheError(f"{name}: LodMesh dimensions are invalid")
        lod = {
            "model_vertices": model_vertices,
            "old_frame_vertices": old_frame_vertices,
            "remap_present": bool(remap),
            "special_vertices": special_vertices,
        }

    return {
        "bounds": bounds,
        "class_name": class_name,
        "collision_triangles": collision_triangles,
        "frame_count": frame_count,
        "lod": lod,
        "origin_bits": origin_bits,
        "packed_vertices": packed_vertices,
        "render_vertices": render_vertices,
        "rotation_origin": list(rotation_origin),
        "scale_bits": scale_bits,
        "sections": sections,
        "sequences": sequences,
        "skins": skins,
        "triangles": triangles,
        "uvs": uvs,
        "vertex_count": vertex_count,
    }


def _encode_cache_v3_mesh_payload(derived: dict) -> bytes:
    flags = 1 if derived["class_name"] == "LodMesh" else 0
    payload = bytearray(b"WOTMSH3\0")
    payload += struct.pack(
        "<2H8I", 2, flags, derived["vertex_count"],
        len(derived["triangles"]), len(derived["uvs"]),
        len(derived["sections"]), len(derived["skins"]),
        len(derived["render_vertices"]),
        len(derived["collision_triangles"]), derived["frame_count"])
    payload += struct.pack(
        "<6I3i6I", *derived["origin_bits"], *derived["scale_bits"],
        *derived["rotation_origin"], *derived["bounds"]["min_bits"],
        *derived["bounds"]["max_bits"])
    payload += struct.pack(
        f"<{derived['vertex_count']}I",
        *derived["packed_vertices"][:derived["vertex_count"]])
    for uv in derived["uvs"]:
        payload += bytes((uv["u"], uv["v"]))
    for triangle in derived["triangles"]:
        uv_indices = triangle["uv_indices"] or (-1, -1, -1)
        available = ((1 if triangle["uv_indices"] is not None else 0)
                     | (2 if triangle["poly_flags"] is not None else 0)
                     | (4 if triangle["texture_index"] is not None else 0)
                     | (8 if triangle["lod_attachment"] else 0))
        payload += struct.pack(
            "<3I3i3I4Ii", *triangle["indices"], *uv_indices,
            *triangle["render_indices"], triangle["section_index"], available,
            triangle["poly_flags"] or 0, 0,
            triangle["texture_index"] or 0)
    for section in derived["sections"]:
        available = ((1 if section["poly_flags"] is not None else 0)
                     | (2 if section["skin_index"] is not None else 0)
                     | (4 if section["lod_attachment"] else 0))
        payload += struct.pack(
            "<4Ii", section["first_triangle"], section["triangle_count"],
            available, section["poly_flags"] or 0,
            section["skin_index"] or 0)
    for render in derived["render_vertices"]:
        payload += struct.pack(
            "<I2i", render["source_vertex"],
            -1 if render["uv_index"] is None else render["uv_index"],
            -1 if render["skin_index"] is None else render["skin_index"])
    if derived["collision_triangles"]:
        payload += struct.pack(
            f"<{len(derived['collision_triangles'])}I",
            *derived["collision_triangles"])
    return bytes(payload)


def _encode_cache_v3_animation_payload(derived: dict) -> bytes:
    packed = derived["packed_vertices"]
    payload = bytearray(b"WOTANI3\0")
    payload += struct.pack(
        "<2H4I", 1, 0, derived["vertex_count"], derived["frame_count"],
        len(packed), derived["vertex_count"] * 4)
    payload += struct.pack(f"<{len(packed)}I", *packed)
    return bytes(payload)


def encode_cache_v3_mesh_object(mesh: dict, package: Package, *, fact_id: str,
                                source_path: str, source_export: str,
                                source_bytes: bytes) -> bytes:
    derived = _derive_cache_v3_mesh(mesh, package)
    payload = _encode_cache_v3_mesh_payload(derived)
    header = {
        "bounds": derived["bounds"],
        "class_name": derived["class_name"],
        "collision": {
            "triangle_indices": derived["collision_triangles"],
            "uses_base_frame": True,
        },
        "encoding": "ue1-vertex-mesh-le-v2",
        "fact_id": fact_id,
        "frame_count": derived["frame_count"],
        "importer": "mesh-v1",
        "importer_version": CACHE_V3_IMPORTER_VERSION,
        "kind": "mesh",
        "lod": derived["lod"],
        "origin_bits": derived["origin_bits"],
        "payload_byte_length": len(payload),
        "render_vertices": derived["render_vertices"],
        "rotation_origin": derived["rotation_origin"],
        "scale_bits": derived["scale_bits"],
        "sections": derived["sections"],
        "skins": derived["skins"],
        "source": _cache_v3_source(
            source_path, source_export, source_bytes),
        "tangent_rule": MESH_TANGENT_RULE,
        "triangle_count": len(derived["triangles"]),
        "triangles": derived["triangles"],
        "uv_count": len(derived["uvs"]),
        "uvs": derived["uvs"],
        "vertex_count": derived["vertex_count"],
    }
    return encode_cache_v3_object(header, payload)


def encode_cache_v3_animation_object(
        mesh: dict, package: Package, *, fact_id: str, source_path: str,
        source_export: str, source_bytes: bytes) -> bytes:
    derived = _derive_cache_v3_mesh(mesh, package)
    payload = _encode_cache_v3_animation_payload(derived)
    header = {
        "encoding": "ue1-packed-vertex-frames-le-v1",
        "fact_id": fact_id,
        "frame_byte_length": derived["vertex_count"] * 4,
        "frame_count": derived["frame_count"],
        "importer": "animation-v1",
        "importer_version": CACHE_V3_IMPORTER_VERSION,
        "kind": "animation",
        "payload_byte_length": len(payload),
        "sequence_count": len(derived["sequences"]),
        "sequences": derived["sequences"],
        "source": _cache_v3_source(
            source_path, source_export, source_bytes),
        "vertex_count": derived["vertex_count"],
    }
    return encode_cache_v3_object(header, payload)


def _append_v3_string(payload: bytearray, value: str, label: str,
                      *, optional: bool = False) -> None:
    if not isinstance(value, str) or (not optional and not value):
        raise CacheError(f"{label} is not a non-empty string")
    encoded = value.encode("utf-8")
    if len(encoded) > 1024 * 1024:
        raise CacheError(f"{label} exceeds its UTF-8 bound")
    payload.extend(struct.pack("<I", len(encoded)))
    payload.extend(encoded)


def _raw_package_identity(package: Package, reference: int,
                          label: str, *, optional: bool = False) -> str:
    if not _is_int(reference):
        raise CacheError(f"{label} is not an integer object reference")
    if reference == 0:
        if optional:
            return ""
        raise CacheError(f"{label} is a null object reference")
    if (reference > len(package.exports)
            or reference < -len(package.imports)):
        raise CacheError(f"{label} is outside the complete package registry")
    path = package.objpath(reference)
    if not path:
        raise CacheError(f"{label} lacks a semantic object identity")
    if reference > 0:
        path = f"{package.name}.{path}"
    components = path.split(".")
    if any(not component for component in components):
        raise CacheError(f"{label} has an empty identity component")
    return path


def _canonical_identity_table(package: Package) -> dict:
    """Return the one reference-indexed identity/registry view for a package.

    UE1 permits distinct exports to serialize the same dotted object path.  A
    semantic identity therefore belongs to the signed package-table reference,
    not to the path alone.  All semantic encoders consume this table so collision
    suffixes cannot diverge between registry rows and their references.
    """
    raw = {}
    placeholders = {}
    for import_index in range(1, len(package.imports) + 1):
        reference = -import_index
        raw[reference] = _raw_package_identity(
            package, reference, "canonical registry import")
    for export_index, export in enumerate(package.exports, 1):
        placeholder = (export["cls"] == 0 and export["size"] == 0
                       and export["name"] == "None")
        raw[export_index] = (f"{package.name}.#export-{export_index:08d}"
                             if placeholder else _raw_package_identity(
                                 package, export_index,
                                 "canonical registry export"))
        placeholders[export_index] = placeholder

    counts = Counter(identity.casefold() for identity in raw.values())
    identities = {}
    for reference, identity in raw.items():
        if counts[identity.casefold()] > 1:
            kind = "export" if reference > 0 else "import"
            ordinal = reference if reference > 0 else -reference
            identity = f"{identity}#{kind}-{ordinal:08d}"
        identities[reference] = identity

    classes = {}
    rows = []
    for import_index, imported in enumerate(package.imports, 1):
        reference = -import_index
        classes[reference] = f"{imported[0]}.{imported[1]}"
        rows.append({"class": classes[reference],
                     "identity": identities[reference],
                     "reference": reference})
    for export_index, export in enumerate(package.exports, 1):
        reference = export["cls"]
        if reference == 0:
            classes[export_index] = "Core.Class"
        elif reference in identities:
            classes[export_index] = identities[reference]
        else:
            raise CacheError(
                f"{identities[export_index]} class is outside the complete registry")
        rows.append({"class": classes[export_index],
                     "identity": identities[export_index],
                     "reference": export_index})

    folded = Counter(row["identity"].casefold() for row in rows)
    duplicate = next((row["identity"] for row in rows
                      if folded[row["identity"].casefold()] != 1), None)
    if duplicate is not None:
        raise CacheError(
            f"canonical registry identity is not injective: {duplicate}")
    references = {identity.casefold(): reference
                  for reference, identity in identities.items()}
    return {"classes": classes, "identities": identities,
            "placeholders": placeholders, "references": references,
            "rows": rows}


def _package_identity(package: Package, reference: int, label: str, *,
                      optional: bool = False,
                      identity_table: dict | None = None) -> str:
    if reference == 0:
        if optional:
            return ""
        raise CacheError(f"{label} is a null object reference")
    if identity_table is None:
        identity_table = _canonical_identity_table(package)
    try:
        return identity_table["identities"][reference]
    except (KeyError, TypeError) as exc:
        raise CacheError(
            f"{label} is outside the complete package registry") from exc


def _bind_semantic_reference(package: Package, reference: int, label: str, *,
                             optional: bool = False,
                             identity_table: dict) -> tuple[str, str]:
    identity = _package_identity(
        package, reference, label, optional=optional,
        identity_table=identity_table)
    if not identity:
        return "", ""
    try:
        return identity, identity_table["classes"][reference]
    except KeyError as exc:
        raise CacheError(f"{label} has no canonical class binding") from exc


def _resolve_source_export(
        package: Package, source_export: str, label: str, *,
        expected_classes: Sequence[str], identity_table: dict
        ) -> tuple[int, str, dict]:
    """Resolve one fact identity through the canonical reference table.

    The inverse index is derived from ``identities`` only after that table is
    proven injective.  Class is deliberately checked after identity lookup so
    a collision suffix can never be interpreted inside a class-filtered scan.
    """
    if not isinstance(source_export, str) or not source_export:
        raise CacheError(f"{label} is not a non-empty canonical identity")
    reference = identity_table["references"].get(source_export.casefold())
    if (not _is_int(reference) or reference <= 0
            or reference > len(package.exports)):
        raise CacheError(
            f"{source_export}: {label} does not resolve to an export registry row")
    export = package.exports[reference - 1]
    class_name = package.objname(export["cls"]) if export["cls"] else "Class"
    if class_name not in expected_classes:
        expected = "/".join(expected_classes)
        raise CacheError(
            f"{source_export}: {label} resolves to {class_name}, not {expected}")
    return reference, identity_table["identities"][reference], export


def _require_semantic_reference_closure(
        identity_table: dict, references: Sequence[tuple[str, str, str]]) -> None:
    rows_by_identity = {}
    for row in identity_table["rows"]:
        rows_by_identity.setdefault(row["identity"].casefold(), []).append(row)
    for identity, expected_class, label in references:
        if not identity:
            continue
        matches = rows_by_identity.get(identity.casefold(), ())
        if (len(matches) != 1
                or matches[0]["class"].casefold() != expected_class.casefold()):
            raise CacheError(
                f"{label} does not resolve to exactly one registry row "
                f"with class {expected_class}: {identity}")


def _semantic_reference_closure_counts(
        identity_table: dict,
        references: Sequence[tuple[str, str, str]]) -> dict[str, int]:
    rows_by_identity = {}
    for row in identity_table["rows"]:
        rows_by_identity.setdefault(row["identity"].casefold(), []).append(row)
    result = {"ambiguous": 0, "references": len(references),
              "unresolved": 0, "wrong_class": 0}
    for identity, expected_class, _label in references:
        matches = rows_by_identity.get(identity.casefold(), ())
        if not matches:
            result["unresolved"] += 1
        elif len(matches) != 1:
            result["ambiguous"] += 1
        elif matches[0]["class"].casefold() != expected_class.casefold():
            result["wrong_class"] += 1
    return result


def _decode_compact_value(value: bytes, label: str) -> int:
    reader = PayloadReader.__new__(PayloadReader)
    reader.d = bytes(value)
    reader.o = 0
    reader.end = len(value)
    try:
        result = reader.cindex()
    except (IndexError, ValueError) as exc:
        raise CacheError(f"{label} has an invalid compact index") from exc
    if reader.o != reader.end:
        raise CacheError(f"{label} compact index has trailing bytes")
    return result


def _canonical_properties_from_tags(
        package: Package, export: dict, tags: Sequence[dict], *,
        identity_table: dict, context: str,
        script_resolver: _ScriptResolver | None = None,
        class_ref: tuple[Package, int] | None = None) -> list[dict]:
    """Translate raw UE1 tags to the one Task-23 canonical property shape."""
    properties = []
    seen = set()
    for tag in tags:
        property_type = tag["type"]
        if property_type not in (1, 2, 3, 4, 5, 6, 10, 13):
            raise CacheError(
                f"{context}.{tag['name']}: "
                f"unknown property type {property_type}")
        key = (tag["name"].casefold(), tag["array_index"])
        if key in seen:
            raise CacheError(
                f"{context}: duplicate property "
                f"{tag['name']}[{tag['array_index']}]")
        seen.add(key)
        value = bytes(tag["value"])
        name_value = ""
        object_identity = ""
        object_class = ""
        if property_type == 3:
            value = bytes(((tag["info"] >> 7) & 1,))
        elif property_type == 4 and value == b"\0\0\0\x80":
            value = bytes(4)
        elif property_type == 5:
            reference = _decode_compact_value(
                value, f"{context}.{tag['name']}")
            object_identity, object_class = _bind_semantic_reference(
                package, reference, f"{context}.{tag['name']}",
                optional=True, identity_table=identity_table)
            value = b""
        elif property_type == 6:
            name_index = _decode_compact_value(
                value, f"{context}.{tag['name']}")
            if not 0 <= name_index < len(package.names):
                raise CacheError(
                    f"{context}.{tag['name']}: "
                    "name reference is outside the complete registry")
            name_value = package.names[name_index]
            value = b""
        members = []
        if property_type == 10:
            member_context = f"{context}.{tag['name']}"
            if script_resolver is None or class_ref is None:
                members = _resolved_known_struct_members(
                    package, tag["struct"] or "", bytes(tag["value"]),
                    identity_table=identity_table, context=member_context)
            else:
                members = _resolved_reflected_struct_members(
                    package, class_ref, tag["name"], tag["array_index"],
                    tag["struct"] or "",
                    bytes(tag["value"]), identity_table=identity_table,
                    resolver=script_resolver, context=member_context)
        properties.append({
            "array_index": tag["array_index"],
            "name": tag["name"],
            "name_value": name_value,
            "object_class": object_class,
            "object_identity": object_identity,
            "struct": tag["struct"] or "",
            "type": property_type,
            "value": value,
            "members": members,
        })
    properties.sort(key=lambda item: (
        item["name"].casefold(), item["array_index"], item["name"]))
    return properties


def _resolved_known_struct_members(
        package: Package, struct_name: str, value: bytes, *,
        identity_table: dict, context: str) -> list[dict]:
    """Decode the two UE1 structs whose declaration order is runtime data."""
    schema = struct_name.casefold()
    if schema not in ("resource", "soundevent"):
        return []
    reader = PayloadReader.__new__(PayloadReader)
    reader.d = bytes(value)
    reader.o = 0
    reader.end = len(value)

    def member(name: str, property_type: int, *, raw: bytes = b"",
               name_value: str = "", object_identity: str = "",
               object_class: str = "") -> dict:
        return {
            "array_index": 0, "name": name, "name_value": name_value,
            "object_class": object_class, "object_identity": object_identity,
            "struct": "", "type": property_type, "value": raw, "members": [],
        }

    def resolved_name(name: str) -> dict:
        index = reader.cindex()
        if not 0 <= index < len(package.names):
            raise CacheError(f"{context}.{name}: name reference is outside the registry")
        return member(name, 6, name_value=package.names[index])

    def scalar(name: str, property_type: int, size: int) -> dict:
        reader._require(size)
        raw = bytes(reader.d[reader.o:reader.o + size])
        reader.o += size
        return member(name, property_type, raw=raw)

    def resolved_object(name: str) -> dict:
        reference = reader.cindex()
        identity, object_class = _bind_semantic_reference(
            package, reference, f"{context}.{name}", optional=True,
            identity_table=identity_table)
        return member(name, 5, object_identity=identity,
                      object_class=object_class)

    try:
        if schema == "resource":
            result = [
                resolved_name("ResourcePackageName"),
                resolved_name("ResourceAdapterName"),
                scalar("ItemCount", 2, 4),
            ]
        else:
            result = [
                scalar("Delay", 4, 4), resolved_name("Event"),
                resolved_name("SoundSourceTag"), resolved_object("Sound"),
                resolved_name("SoundTableTag"),
                resolved_name("SoundTableSlot"),
            ]
    except (IndexError, ValueError) as exc:
        raise CacheError(f"{context}: malformed {struct_name} value") from exc
    if reader.o != reader.end:
        raise CacheError(f"{context}: {struct_name} value has trailing bytes")
    return result


def _resolved_reflected_struct_members(
        value_package: Package, class_ref: tuple[Package, int],
        property_name: str, array_index: int, struct_name: str, value: bytes, *,
        identity_table: dict, resolver: _ScriptResolver,
        context: str) -> list[dict]:
    """Decode one class-default struct using its declaring package schema."""
    declaration = resolver.find_property(class_ref, property_name)
    if declaration is None or declaration["kind"] != "struct":
        raise CacheError(
            f"{context}: reflected declaration is absent or is not a struct")
    if not 0 <= array_index < declaration["array_dim"]:
        raise CacheError(
            f"{context}: tag array index exceeds reflected ArrayDim")
    struct_ref = resolver.resolve(
        declaration["package"], declaration["struct_ref"])
    if struct_ref is None:
        raise CacheError(f"{context}: reflected struct cannot be resolved")
    struct_package, struct_index = struct_ref
    reflected_name = struct_package.exports[struct_index - 1]["name"]
    if reflected_name.casefold() != struct_name.casefold():
        raise CacheError(
            f"{context}: tag struct {struct_name!r} differs from "
            f"reflection {reflected_name!r}")

    reader = PayloadReader.__new__(PayloadReader)
    reader.d = bytes(value)
    reader.o = 0
    reader.end = len(value)
    reader.pkg = value_package
    reader.export = {"name": context}

    def member(name: str, property_type: int, array_index: int, *,
               raw: bytes = b"", nested_struct: str = "",
               members: list[dict] | None = None, name_value: str = "",
               object_identity: str = "", object_class: str = "") -> dict:
        return {
            "array_index": array_index, "name": name,
            "name_value": name_value, "object_class": object_class,
            "object_identity": object_identity, "struct": nested_struct,
            "type": property_type, "value": raw,
            "members": [] if members is None else members,
        }

    def decode_field(prop: dict, name: str, array_index: int,
                     depth: int, stack: set[tuple[str, int]],
                     active_properties: set[tuple[str, int]],
                     fixed_array_depth: int) -> list[dict]:
        kind = prop["kind"]
        if kind in ("array", "map"):
            raise CacheError(
                f"{context}.{name}: unsupported reflected {kind} member")
        if kind == "fixed_array":
            if fixed_array_depth >= 4:
                raise CacheError(
                    f"{context}.{name}: fixed-array wrappers exceed depth 4")
            property_key = resolver.key((prop["package"], prop["index"]))
            if property_key in active_properties:
                raise CacheError(
                    f"{context}.{name}: fixed-array property reference cycle")
            inner_ref = resolver.resolve(prop["package"], prop["inner"])
            if inner_ref is None:
                raise CacheError(
                    f"{context}.{name}: fixed-array inner cannot be resolved")
            inner = resolver.property_info(inner_ref)
            return [item
                    for inner_index in range(prop["count"])
                    for item in decode_field(
                        inner, name, array_index * prop["count"] + inner_index,
                        depth, stack, active_properties | {property_key},
                        fixed_array_depth + 1)]
        start = reader.o
        if kind == "byte":
            resolver.enum_names(prop)
            raw = bytes((reader.u8(),))
            return [member(name, 1, array_index, raw=raw)]
        if kind == "int":
            reader._require(4)
            reader.o += 4
            return [member(name, 2, array_index,
                           raw=bytes(reader.d[start:reader.o]))]
        if kind == "bool":
            raw = bytes((reader.u8(),))
            return [member(name, 3, array_index, raw=raw)]
        if kind == "float":
            reader._require(4)
            reader.o += 4
            return [member(name, 4, array_index,
                           raw=bytes(reader.d[start:reader.o]))]
        if kind == "object":
            reference = reader.cindex()
            identity, object_class = _bind_semantic_reference(
                value_package, reference, f"{context}.{name}", optional=True,
                identity_table=identity_table)
            return [member(name, 5, array_index, object_identity=identity,
                           object_class=object_class)]
        if kind == "name":
            name_index = reader.cindex()
            if not 0 <= name_index < len(value_package.names):
                raise CacheError(
                    f"{context}.{name}: name index is outside the value package")
            return [member(name, 6, array_index,
                           name_value=value_package.names[name_index])]
        if kind == "string":
            count = reader.cindex()
            if count:
                character_count = abs(count)
                byte_count = character_count if count > 0 else character_count * 2
                reader._require(byte_count)
                encoded = bytes(reader.d[reader.o:reader.o + byte_count])
                reader.o += byte_count
                terminator = b"\0" if count > 0 else b"\0\0"
                if not encoded.endswith(terminator):
                    raise CacheError(
                        f"{context}.{name}: reflected string is not terminated")
            return [member(name, 13, array_index,
                           raw=bytes(reader.d[start:reader.o]))]
        if kind == "struct":
            if depth > 8:
                raise CacheError(f"{context}.{name}: struct nesting exceeds 8")
            nested_ref = resolver.resolve(prop["package"], prop["struct_ref"])
            if nested_ref is None:
                raise CacheError(
                    f"{context}.{name}: nested struct cannot be resolved")
            key = resolver.key(nested_ref)
            if key in stack:
                raise CacheError(
                    f"{context}.{name}: nested struct resolution cycle")
            nested_package, nested_index = nested_ref
            nested_name = nested_package.exports[nested_index - 1]["name"]
            nested_members = decode_struct(nested_ref, depth, stack | {key})
            return [member(
                name, 10, array_index, raw=bytes(reader.d[start:reader.o]),
                nested_struct=nested_name, members=nested_members)]
        raise CacheError(f"{context}.{name}: unsupported reflected kind {kind}")

    def decode_struct(resolved: tuple[Package, int], depth: int,
                      stack: set[tuple[str, int]]) -> list[dict]:
        if depth > 8:
            raise CacheError(f"{context}: struct nesting exceeds 8")
        result = []
        fields = resolver.fields(resolved)
        if len(fields) > 256:
            raise CacheError(f"{context}: struct has more than 256 fields")
        for field in fields:
            if field["flags"] & (0x1000 | 0x2000):
                continue
            for array_index in range(field["array_dim"]):
                result.extend(decode_field(
                    field, field["name"], array_index, depth + 1, stack,
                    set(), 0))
        return result

    try:
        root_key = resolver.key(struct_ref)
        result = decode_struct(struct_ref, 0, {root_key})
    except (IndexError, KeyError, TypeError, ValueError) as exc:
        if isinstance(exc, CacheError):
            raise
        raise CacheError(f"{context}: malformed reflected struct value") from exc
    if reader.o != reader.end:
        raise CacheError(f"{context}: reflected struct value has trailing bytes")
    return result


def _decoded_property_tags(package: Package, export: dict, *,
                           identity_table: dict | None = None) -> list[dict]:
    """Return canonical properties using ue1pkg's shared raw tag reader."""
    if identity_table is None:
        identity_table = _canonical_identity_table(package)
    reader = PayloadReader(package, export)
    return _canonical_properties_from_tags(
        package, export, read_property_tags(reader),
        identity_table=identity_table,
        context=f"{package.name}.{export['name']}")


def _append_v3_properties(payload: bytearray, properties: Sequence[dict], *,
                          include_struct_members: bool = False,
                          depth: int = 0) -> None:
    if depth > 8:
        raise CacheError("property member nesting exceeds its bound")
    payload.extend(struct.pack("<I", len(properties)))
    for prop in properties:
        _append_v3_string(payload, prop["name"], "property name")
        payload.extend(bytes((prop["type"],)))
        payload.extend(struct.pack("<I", prop["array_index"]))
        for field in ("struct", "name_value", "object_identity", "object_class"):
            _append_v3_string(payload, prop[field], f"property {field}", optional=True)
        payload.extend(struct.pack("<I", len(prop["value"])))
        payload.extend(prop["value"])
        if include_struct_members:
            members = prop.get("members", [])
            payload.extend(struct.pack("<I", len(members)))
            for nested in members:
                _append_v3_property(payload, nested, depth=depth + 1)


def _append_v3_property(payload: bytearray, prop: dict, *, depth: int) -> None:
    if depth > 8:
        raise CacheError("property member nesting exceeds its bound")
    _append_v3_string(payload, prop["name"], "property name")
    payload.extend(bytes((prop["type"],)))
    payload.extend(struct.pack("<I", prop["array_index"]))
    for field in ("struct", "name_value", "object_identity", "object_class"):
        _append_v3_string(payload, prop[field], f"property {field}", optional=True)
    payload.extend(struct.pack("<I", len(prop["value"])))
    payload.extend(prop["value"])
    members = prop.get("members", [])
    payload.extend(struct.pack("<I", len(members)))
    for nested in members:
        _append_v3_property(payload, nested, depth=depth + 1)


def _font_codepoint(page_index: int, characters_per_page: int,
                    local_index: int) -> int:
    values = (page_index, characters_per_page, local_index)
    if (any(not _is_int(value) or value < 0 or value > 0xffffffff
            for value in values)
            or characters_per_page == 0
            or local_index >= characters_per_page):
        raise CacheError("font codepoint operands are outside their range")
    if page_index > (0xffffffff - local_index) // characters_per_page:
        raise CacheError("font codepoint arithmetic overflow")
    codepoint = page_index * characters_per_page + local_index
    if 0xd800 <= codepoint <= 0xdfff:
        raise CacheError("font codepoint is a surrogate")
    if codepoint > 0x10ffff:
        raise CacheError("font codepoint exceeds Unicode")
    return codepoint


def _font_checked_add(left: int, right: int, context: str) -> int:
    if (not _is_int(left) or not _is_int(right) or left < 0 or right < 0
            or left > 0xffffffffffffffff - right):
        raise CacheError(f"font {context} arithmetic overflow")
    return left + right


def _font_checked_multiply(left: int, right: int, context: str) -> int:
    if (not _is_int(left) or not _is_int(right) or left < 0 or right < 0
            or left != 0 and right > 0xffffffffffffffff // left):
        raise CacheError(f"font {context} arithmetic overflow")
    return left * right


def _font_v2_materialization_plan(font: dict) -> dict[str, int]:
    pages = font.get("pages")
    characters = font.get("characters")
    characters_per_page = font.get("characters_per_page")
    name = font.get("name", "<font>")
    if (not isinstance(pages, list)
            or not 1 <= len(pages) <= MAX_CACHE_V3_FONT_PAGES
            or not isinstance(characters, list)
            or len(characters) > MAX_CACHE_V3_FONT_GLYPHS
            or not _is_int(characters_per_page)
            or characters_per_page <= 0
            or characters_per_page > MAX_CACHE_V3_FONT_GLYPHS
            or characters_per_page & (characters_per_page - 1)):
        raise CacheError(f"{name}: invalid font page layout")

    atlas_pixels = 0
    glyph_pixels = 0
    bdf_bytes = 4096
    declared_glyphs = 0
    # UE1's stock LargeFont/BigFont are 2x2 placeholder pages whose 256
    # character rectangles are all empty. They still need a valid, inert v2
    # face rather than publishing forbidden zero line metrics.
    line_height = 1
    page_pixels_by_page: list[int] = []
    aliases: set[tuple[int, int, int, int, int, int, int, int]] = set()
    for page_index, page in enumerate(pages):
        if not isinstance(page, dict):
            raise CacheError(f"{name}: font page {page_index} is malformed")
        texture = page.get("texture")
        metrics = page.get("characters")
        if (page.get("page") != page_index or not isinstance(texture, dict)
                or not isinstance(metrics, list)
                or len(metrics) > characters_per_page):
            raise CacheError(f"{name}: font page {page_index} is malformed")
        width, height = texture.get("w"), texture.get("h")
        if (not _is_int(width) or not _is_int(height)
                or not 1 <= width <= MAX_CACHE_V3_FONT_PAGE_DIMENSION
                or not 1 <= height <= MAX_CACHE_V3_FONT_PAGE_DIMENSION):
            raise CacheError(f"{name}: font page dimensions are invalid")
        page_pixels = _font_checked_multiply(width, height, "atlas")
        atlas_pixels = _font_checked_add(atlas_pixels, page_pixels, "atlas")
        expansion = _font_checked_multiply(atlas_pixels, 4, "atlas expansion")
        if (atlas_pixels > MAX_CACHE_V3_FONT_ATLAS_PIXELS
                or expansion > MAX_CACHE_V3_FONT_ATLAS_EXPANSION_BYTES):
            raise CacheError(f"{name}: font atlas exceeds its materialization budget")
        page_pixels_by_page.append(page_pixels)
        declared_glyphs = _font_checked_add(
            declared_glyphs, len(metrics), "declared glyph count")
        if declared_glyphs > MAX_CACHE_V3_FONT_GLYPHS:
            raise CacheError(f"{name}: declared glyph count exceeds its safe bound")

    if declared_glyphs == 0 or declared_glyphs != len(characters):
        raise CacheError(f"{name}: font glyph registry is inconsistent")

    # No occupancy storage or per-glyph work begins until every page/count and
    # aggregate allocation budget has closed against the flattened registry.
    occupancy = [bytearray(page_pixels) for page_pixels in page_pixels_by_page]
    for page_index, page in enumerate(pages):
        texture = page["texture"]
        metrics = page["characters"]
        width, height = texture["w"], texture["h"]

        for local_index, metric in enumerate(metrics):
            values = (metric.get("x"), metric.get("y"),
                      metric.get("w"), metric.get("h"))
            if (metric.get("local_index") != local_index
                    or any(not _is_int(value) or value < 0
                           or value > 0xffffffff for value in values)
                    or values[0] + values[2] > width
                    or values[1] + values[3] > height):
                raise CacheError(
                    f"{name}: glyph {page_index}:{local_index} "
                    "extends beyond its atlas")
            _font_codepoint(page_index, characters_per_page, local_index)
            x, y, glyph_width, glyph_height = values
            pixels = _font_checked_multiply(
                glyph_width, glyph_height, "glyph work")
            glyph_pixels = _font_checked_add(
                glyph_pixels, pixels, "glyph work")
            glyph_bdf = _font_checked_add(
                _font_checked_multiply(pixels, 2, "BDF"),
                glyph_height, "BDF")
            glyph_bdf = _font_checked_add(glyph_bdf, 512, "BDF")
            bdf_bytes = _font_checked_add(bdf_bytes, glyph_bdf, "BDF")
            if glyph_pixels > MAX_CACHE_V3_FONT_GLYPH_PIXELS:
                raise CacheError(f"{name}: font glyph work exceeds its safe bound")
            if bdf_bytes > MAX_CACHE_V3_FONT_BDF_BYTES:
                raise CacheError(f"{name}: font BDF output exceeds its safe bound")

            alias = (page_index, x, y, glyph_width, glyph_height,
                     glyph_width, 0, 0)
            exact_alias = alias in aliases
            if not exact_alias:
                page_occupancy = occupancy[page_index]
                for row in range(y, y + glyph_height):
                    start = row * width + x
                    end = start + glyph_width
                    if any(page_occupancy[start:end]):
                        raise CacheError(f"{name}: font glyph rectangles overlap")
                    page_occupancy[start:end] = b"\1" * glyph_width
            aliases.add(alias)
            line_height = max(line_height, glyph_height)
    if line_height == 0 or line_height > MAX_CACHE_V3_FONT_PAGE_DIMENSION:
        raise CacheError(f"{name}: font line metrics are invalid")
    return {
        "atlas_pixels": atlas_pixels,
        "atlas_expansion_bytes": atlas_pixels * 4,
        "glyph_pixels": glyph_pixels,
        "bdf_bytes": bdf_bytes,
        "line_height": line_height,
    }


def encode_cache_v3_font_object(font: dict, package: Package, *, fact_id: str,
                                source_path: str, source_export: str,
                                source_bytes: bytes,
                                atlas_logical_ids: dict[str, str]) -> bytes:
    identity_table = _canonical_identity_table(package)
    _source_reference, source_export, source = _resolve_source_export(
        package, source_export, "font fact identity",
        expected_classes=("Font",), identity_table=identity_table)
    if source["name"] != font.get("name"):
        raise CacheError(
            f"{font.get('name', '<font>')}: decoded font does not match "
            f"its source export {source_export}")
    plan = _font_v2_materialization_plan(font)
    semantic_references = []
    pages = font.get("pages")
    characters_per_page = font.get("characters_per_page")
    characters = font.get("characters")
    if (not isinstance(pages, list)
            or not 1 <= len(pages) <= MAX_CACHE_V3_FONT_PAGES
            or not _is_int(characters_per_page) or characters_per_page <= 0
            or characters_per_page > MAX_CACHE_V3_FONT_GLYPHS
            or characters_per_page & (characters_per_page - 1)
            or not isinstance(characters, list)
            or len(characters) > MAX_CACHE_V3_FONT_GLYPHS):
        raise CacheError(f"{font.get('name', '<font>')}: invalid font page layout")
    payload = bytearray(b"WOTFNT3\0")
    line_height = plan["line_height"]
    payload.extend(struct.pack(
        "<2H7I", 2, 0, characters_per_page, len(pages), len(characters),
        len(characters), 0, line_height, line_height))
    expected_glyph = 0
    resolved_pages = []
    for page_index, page in enumerate(pages):
        if page.get("page") != page_index:
            raise CacheError(f"{font['name']}: font pages are not contiguous")
        identity = _package_identity(
            package, page.get("texture_ref"),
            f"{font['name']} page {page_index} texture",
            identity_table=identity_table)
        expected_class = identity_table["classes"][page.get("texture_ref")]
        semantic_references.append((
            identity, expected_class,
            f"{font['name']} page {page_index} texture"))
        texture = page.get("texture")
        metrics = page.get("characters")
        if (not isinstance(texture, dict) or not isinstance(metrics, list)
                or len(metrics) > characters_per_page):
            raise CacheError(f"{font['name']}: font page {page_index} is malformed")
        width, height = texture.get("w"), texture.get("h")
        if (not _is_int(width) or not _is_int(height)
                or width <= 0 or height <= 0
                or width > 0xffffffff or height > 0xffffffff):
            raise CacheError(f"{font['name']}: font page dimensions are invalid")
        logical_id = atlas_logical_ids.get(identity)
        if not isinstance(logical_id, str) or not logical_id:
            raise CacheError(
                f"{font['name']}: font atlas {identity} lacks one "
                "authenticated texture logical id")
        if any(item["logical_id"] == logical_id for item in resolved_pages):
            raise CacheError(
                f"{font['name']}: two atlas pages bind {logical_id}")
        resolved_pages.append({
            "character_count": len(metrics),
            "format": "p8",
            "height": height,
            "logical_id": logical_id,
            "ordinal": page_index,
            "source_identity": identity,
            "source_sha256": hashlib.sha256(source_bytes).hexdigest(),
            "width": width,
        })
        payload.extend(struct.pack("<I", page_index))
        _append_v3_string(payload, logical_id, "font atlas logical id")
        _append_v3_string(payload, identity, "font atlas identity")
        payload.extend(struct.pack("<2I", width, height))
        _append_v3_string(payload, "p8", "font atlas format")
        payload.extend(hashlib.sha256(source_bytes).digest())
        payload.extend(struct.pack("<I", len(metrics)))
    for page_index, page in enumerate(pages):
        metrics = page["characters"]
        for local_index, _metric in enumerate(metrics):
            codepoint = _font_codepoint(
                page_index, characters_per_page, local_index)
            payload.extend(struct.pack("<2I", codepoint, expected_glyph))
            expected_glyph += 1
    expected_glyph = 0
    for page_index, page in enumerate(pages):
        metrics = page["characters"]
        width, height = page["texture"]["w"], page["texture"]["h"]
        for local_index, metric in enumerate(metrics):
            values = (metric.get("x"), metric.get("y"),
                      metric.get("w"), metric.get("h"))
            if (metric.get("local_index") != local_index
                    or any(not _is_int(value) or value < 0
                           or value > 0xffffffff for value in values)
                    or values[0] + values[2] > width
                    or values[1] + values[3] > height):
                raise CacheError(
                    f"{font['name']}: glyph {page_index}:{local_index} "
                    "extends beyond its atlas")
            codepoint = _font_codepoint(
                page_index, characters_per_page, local_index)
            if expected_glyph >= len(characters):
                raise CacheError(f"{font['name']}: font glyph indices are not contiguous")
            expected_glyph += 1
            payload.extend(struct.pack(
                "<9I", codepoint, page_index, *values, values[2], 0, 0))
    if expected_glyph != len(characters):
        raise CacheError(f"{font['name']}: font glyph registry is inconsistent")
    header = {
        "baseline": line_height,
        "character_map_count": len(characters),
        "characters_per_page": characters_per_page,
        "encoding": "ue1-font-pages-v2",
        "fact_id": fact_id,
        "glyph_count": len(characters),
        "importer": "font-v2",
        "importer_version": 2,
        "kerning_count": 0,
        "kind": "font",
        "line_height": line_height,
        "page_count": len(pages),
        "pages": resolved_pages,
        "payload_byte_length": len(payload),
        "source": _cache_v3_source(source_path, source_export, source_bytes),
    }
    _require_semantic_reference_closure(identity_table, semantic_references)
    return encode_cache_v3_object(header, payload)


PROCEDURAL_PAYLOAD_VERSION = 2
PROCEDURAL_ENCODING = "ue1-fractal-inputs-v2"
PROCEDURAL_IMPORTER_VERSION = 2
PROCEDURAL_MAX_DROPS = 256
_PROCEDURAL_INPUT_CLASSES = ("Texture",) + tuple(sorted(FRACTAL_CLASSES))


def _procedural_property(properties: Sequence[dict], name: str,
                         expected_type: int, size: int, context: str):
    """Absent -> None (engine zero-init, bake_flipbooks props.get(X, 0));
    present with the wrong UE1 type or width fails closed (native parity)."""
    for item in properties:
        if item["array_index"] == 0 and item["name"].casefold() == name.casefold():
            if item["type"] != expected_type or len(item["value"]) != size:
                raise CacheError(
                    f"{context}: procedural parameter {name} has the wrong type")
            return item["value"]
    return None


def _procedural_byte(properties, name, context) -> int:
    value = _procedural_property(properties, name, 1, 1, context)
    return 0 if value is None else value[0]


def _procedural_bool(properties, name, context) -> bool:
    value = _procedural_property(properties, name, 3, 1, context)
    return False if value is None else value[0] != 0


def _procedural_int(properties, name, context) -> int:
    value = _procedural_property(properties, name, 2, 4, context)
    return 0 if value is None else struct.unpack("<i", value)[0]


def _procedural_float(properties, name, context) -> float:
    value = _procedural_property(properties, name, 4, 4, context)
    if value is None:
        return 0.0
    number = struct.unpack("<f", value)[0]
    if number != number or number in (float("inf"), float("-inf")):
        raise CacheError(f"{context}: procedural parameter {name} is not finite")
    return number


def _procedural_drops(properties: Sequence[dict], context: str) -> list[bytes]:
    """The static Drops[256] array serializes its non-zero elements as indexed
    ADrop struct tags; the first NumDrops in index order are live."""
    live = max(0, min(_procedural_int(properties, "NumDrops", context),
                      PROCEDURAL_MAX_DROPS))
    drops = []
    previous = None
    for item in properties:
        if item["name"].casefold() != "drops":
            continue
        if (item["type"] != 10 or len(item["value"]) != 8
                or item["struct"].casefold() != "adrop"
                or (previous is not None and item["array_index"] <= previous)
                or item["array_index"] >= PROCEDURAL_MAX_DROPS):
            raise CacheError(f"{context}: procedural drop table is malformed")
        previous = item["array_index"]
        if len(drops) < live:
            drops.append(bytes(item["value"]))
    return drops


def _resolve_procedural_object(package: Package, reference: int,
                               allowed_classes: Sequence[str], context: str,
                               resolve_package):
    """Resolve a local export or a foreign import to exactly one export of an
    allowed class (native WoTRemainingResolveObjectReference)."""
    if not _is_int(reference) or reference == 0:
        raise CacheError(f"{context}: required procedural object reference is null")
    if reference > 0:
        if reference > len(package.exports):
            raise CacheError(f"{context}: object reference is outside the registry")
        export = package.exports[reference - 1]
        class_name = package.objname(export["cls"]) if export["cls"] else "Class"
        if class_name not in allowed_classes:
            raise CacheError(
                f"{context}: procedural reference resolves to {class_name}")
        return package, export, class_name
    if reference < -len(package.imports):
        raise CacheError(f"{context}: object reference is outside the registry")
    raw_path = package.objpath(reference) or ""
    if "." not in raw_path:
        raise CacheError(f"{context}: foreign procedural reference has no package")
    package_name = raw_path.split(".", 1)[0]
    if package_name.casefold() == package.name.casefold():
        target = package
    else:
        target = resolve_package(package_name) if resolve_package else None
        if target is None:
            raise CacheError(
                f"{context}: procedural reference names a package outside the "
                f"contract ({package_name})")
    matches = []
    for index, export in enumerate(target.exports, 1):
        class_name = target.objname(export["cls"]) if export["cls"] else "Class"
        if class_name not in allowed_classes:
            continue
        # objpath() is package-relative for exports; the native path builder
        # prefixes the package name (WoTRemainingObjectPath).
        if (f"{target.name}.{target.objpath(index) or ''}".casefold()
                != raw_path.casefold()):
            continue
        matches.append((export, class_name))
    if len(matches) != 1:
        raise CacheError(
            f"{context}: foreign procedural reference {raw_path} resolves to "
            f"{len(matches)} exports")
    return target, matches[0][0], matches[0][1]


def _procedural_input(package: Package, props: dict, properties: Sequence[dict],
                      name: str, context: str, resolve_package) -> dict:
    """SourceTexture/GlassTexture: kind (0 none/1 texture/2 procedural),
    canonical identity, class, and mip-0 dimensions."""
    row = next((item for item in properties
                if item["array_index"] == 0
                and item["name"].casefold() == name.casefold()), None)
    if row is None:
        return {"kind": 0, "identity": "", "class": "", "width": 0, "height": 0}
    if row["type"] != 5:
        raise CacheError(f"{context}: {name} is not an object reference")
    reference = props.get(name, 0)
    if not reference:
        return {"kind": 0, "identity": "", "class": "", "width": 0, "height": 0}
    target, export, class_name = _resolve_procedural_object(
        package, reference, _PROCEDURAL_INPUT_CLASSES, f"{context}.{name}",
        resolve_package)
    if class_name == "Texture":
        texture = decode_texture(target, export)
        kind, width, height = 1, texture["w"], texture["h"]
    else:
        fractal = decode_fractal(target, export)
        if not fractal["mips"]:
            raise CacheError(f"{context}.{name}: source fractal has no mip")
        kind, width, height = 2, fractal["mips"][0][0], fractal["mips"][0][1]
    if not row["object_identity"]:
        raise CacheError(f"{context}.{name}: input lacks its canonical identity")
    return {"kind": kind, "identity": row["object_identity"],
            "class": class_name, "width": width, "height": height}


def _append_procedural_input(payload: bytearray, item: dict) -> None:
    payload.extend(bytes((item["kind"],)))
    _append_v3_string(payload, item["identity"], "procedural input identity",
                      optional=True)
    payload.extend(struct.pack("<II", item["width"], item["height"]))


def encode_cache_v3_procedural_object(
        fractal: dict, package: Package, export: dict, *, fact_id: str,
        source_path: str, source_export: str, source_bytes: bytes,
        resolve_package=None) -> bytes:
    """Procedural v2: the v1 body followed by the object's own palette bytes,
    the simulation parameters (bake_flipbooks _runtime_base), the live ADrop
    table, and the SourceTexture/GlassTexture identities with mip-0 sizes.
    ``resolve_package(name)`` serves foreign (import) references; ``None``
    means only the fact's own package is available.  Mirrors
    Source/WoTRetailReader/Private/FontProcedural.cpp byte for byte."""
    class_name = fractal.get("cls")
    if class_name not in FRACTAL_CLASSES:
        raise CacheError(f"{fractal.get('name', '<fractal>')}: unsupported fractal class")
    identity_table = _canonical_identity_table(package)
    export_reference, source_export, source = _resolve_source_export(
        package, source_export, "procedural fact identity",
        expected_classes=tuple(sorted(FRACTAL_CLASSES)),
        identity_table=identity_table)
    if source is not export:
        raise CacheError(
            f"{fractal.get('name', '<fractal>')}: decoded procedural export "
            f"does not match its source registry row {export_reference}")
    context = source_export
    mips = fractal.get("mips")
    if not isinstance(mips, list) or not mips:
        raise CacheError(f"{fractal['name']}: procedural texture has no mip dimensions")
    properties = _decoded_property_tags(
        package, export, identity_table=identity_table)
    states = fractal.get("sparks", fractal.get("drops", []))
    if (not isinstance(states, list)
            or len(states) > MAX_CACHE_V3_PROCEDURAL_STATES
            or any(len(state) != 8 for state in states)):
        raise CacheError(f"{fractal['name']}: procedural state vector is malformed")
    props = fractal.get("props", {})

    # Own palette (every shipped Wet/Ice palette is its source's palette object).
    palette_row = next((item for item in properties
                        if item["array_index"] == 0
                        and item["name"].casefold() == "palette"), None)
    palette_reference = props.get("Palette", 0)
    if (palette_row is None or palette_row["type"] != 5 or not palette_reference
            or not palette_row["object_identity"]):
        raise CacheError(f"{context}: procedural texture has no palette reference")
    palette_package, palette_export, _palette_class = _resolve_procedural_object(
        package, palette_reference, ("Palette",), f"{context}.Palette",
        resolve_package)
    palette = decode_palette(palette_package, palette_export)
    if len(palette) != 256 or any(len(color) != 4 for color in palette):
        raise CacheError(f"{context}: procedural palette is malformed")
    palette_bytes = bytes(channel for color in palette for channel in color)

    masked = _procedural_bool(properties, "bMasked", context)         and class_name == "IceTexture"
    parameters = struct.pack(
        "<BBBBffIBBBBBBBBBBBBIfffff",
        1 if masked else 0,
        1 if _procedural_bool(properties, "bRising", context) else 0,
        1 if _procedural_bool(properties, "MoveIce", context) else 0,
        0,
        _procedural_float(properties, "MinFrameRate", context),
        _procedural_float(properties, "MaxFrameRate", context),
        _procedural_byte(properties, "PrimeCount", context),
        _procedural_byte(properties, "WaveAmp", context),
        _procedural_byte(properties, "BumpMapLight", context),
        _procedural_byte(properties, "BumpMapAngle", context),
        _procedural_byte(properties, "PhongRange", context),
        _procedural_byte(properties, "PhongSize", context),
        _procedural_byte(properties, "RenderHeat", context),
        _procedural_byte(properties, "PanningStyle", context),
        _procedural_byte(properties, "TimeMethod", context),
        _procedural_byte(properties, "HorizPanSpeed", context),
        _procedural_byte(properties, "VertPanSpeed", context),
        _procedural_byte(properties, "Frequency", context),
        _procedural_byte(properties, "Amplitude", context),
        _procedural_int(properties, "SparksLimit", context) & 0xffffffff,
        _procedural_float(properties, "MasterCount", context),
        _procedural_float(properties, "UDisplace", context),
        _procedural_float(properties, "VDisplace", context),
        _procedural_float(properties, "UPosition", context),
        _procedural_float(properties, "VPosition", context))
    drops = _procedural_drops(properties, context)
    source_input = _procedural_input(
        package, props, properties, "SourceTexture", context, resolve_package)
    glass_input = _procedural_input(
        package, props, properties, "GlassTexture", context, resolve_package)

    payload = bytearray(b"WOTPRC3\0")
    payload.extend(struct.pack("<2H3I", PROCEDURAL_PAYLOAD_VERSION, 0,
                               len(mips), len(states), PROCEDURAL_RANDOM_SEED))
    _append_v3_properties(payload, properties)
    dimensions = []
    for mip_index, mip in enumerate(mips):
        if not isinstance(mip, tuple) or len(mip) != 3:
            raise CacheError(f"{fractal['name']}: procedural mip {mip_index} is malformed")
        width, height, data = mip
        data = bytes(data)
        if (not _is_int(width) or not _is_int(height)
                or width < 1 or height < 1 or width & (width - 1)
                or height & (height - 1) or len(data) != width * height):
            raise CacheError(f"{fractal['name']}: procedural mip dimensions are invalid")
        payload.extend(struct.pack("<3I", width, height, len(data)))
        payload.extend(data)
        dimensions.append({"height": height, "width": width})
    for state in states:
        payload.extend(bytes(state))
    payload.extend(palette_bytes)
    payload.extend(parameters)
    payload.extend(struct.pack("<I", len(drops)))
    for drop in drops:
        payload.extend(drop)
    _append_procedural_input(payload, source_input)
    _append_procedural_input(payload, glass_input)
    references = sorted({
        item["object_identity"] for item in properties
        if item["object_identity"]
    }, key=lambda value: (value.casefold(), value))
    header = {
        "class_name": class_name,
        "dimensions": dimensions,
        "drop_count": len(drops),
        "encoding": PROCEDURAL_ENCODING,
        "fact_id": fact_id,
        "glass_class": glass_input["class"],
        "glass_texture": glass_input["identity"],
        "importer": "procedural-v1",
        "importer_version": PROCEDURAL_IMPORTER_VERSION,
        "kind": "procedural",
        "masked": 1 if masked else 0,
        "mip_count": len(mips),
        "palette_identity": palette_row["object_identity"],
        "payload_byte_length": len(payload),
        "property_count": len(properties),
        "random_scope": "process-global-canonical-order",
        "random_seed": PROCEDURAL_RANDOM_SEED,
        "references": references,
        "source": _cache_v3_source(source_path, source_export, source_bytes),
        "source_class": source_input["class"],
        "source_texture": source_input["identity"],
        "state_count": len(states),
        "state_kind": "sparks" if class_name == "FireTexture" else (
            "drops" if class_name in ("WaterTexture", "WetTexture", "WaveTexture")
            else "none"),
    }
    _require_semantic_reference_closure(identity_table, [
        (item["object_identity"], item["object_class"],
         f"{package.name}.{export['name']}.{item['name']}")
        for item in properties if item["object_identity"]
    ])
    return encode_cache_v3_object(header, payload)


def retail_package_resolver(retail_root: Path):
    """Foreign-reference resolver over a retail tree: Textures/*.utx and
    System/*.u by case-insensitive package name, parsed once."""
    cache: dict[str, Package | None] = {}

    def resolve(name: str):
        key = name.casefold()
        if key in cache:
            return cache[key]
        found = None
        for directory, suffix in (("Textures", ".utx"), ("System", ".u")):
            root = Path(retail_root) / directory
            if not root.is_dir():
                continue
            for path in root.iterdir():
                if path.is_file() and path.name.casefold() == f"{name}{suffix}".casefold():
                    found = Package(path)
                    break
            if found is not None:
                break
        cache[key] = found
        return found
    return resolve


def _class_exports(package: Package, identity_table: dict | None = None
                   ) -> list[tuple[int, dict, str, bool]]:
    if identity_table is None:
        identity_table = _canonical_identity_table(package)
    classes = []
    folded = {}
    for export_index, export in enumerate(package.exports, 1):
        if export["cls"] != 0:
            continue
        placeholder = identity_table["placeholders"][export_index]
        identity = _package_identity(
            package, export_index, "class export",
            identity_table=identity_table)
        key = identity.casefold()
        if key in folded:
            raise CacheError(
                f"duplicate class identifier after case folding: {identity}")
        folded[key] = identity
        classes.append((export_index, export, identity, placeholder))
    classes.sort(key=lambda item: (item[2].casefold(), item[2]))
    return classes


def _package_registry(package: Package,
                      identity_table: dict | None = None) -> dict:
    if identity_table is None:
        identity_table = _canonical_identity_table(package)
    folded_names = {}
    for name in package.names:
        key = name.casefold()
        if key in folded_names and folded_names[key] != name:
            raise CacheError(
                f"duplicate package name after case folding: "
                f"{folded_names[key]!r}/{name!r}")
        folded_names[key] = name
    names = sorted(folded_names.values(), key=lambda value: (value.casefold(), value))
    imports = []
    for reference in range(-1, -len(package.imports) - 1, -1):
        imported = package.imports[-reference - 1]
        imports.append({
            "class": identity_table["classes"][reference],
            "identity": _package_identity(
                package, reference, "registry import",
                identity_table=identity_table),
        })
    imports.sort(key=lambda item: (item["identity"].casefold(), item["identity"],
                                   item["class"].casefold(), item["class"]))
    objects = []
    classes = _class_exports(package, identity_table)
    class_by_index = {index: (identity, placeholder)
                      for index, _export, identity, placeholder in classes}
    raw_identities = []
    for export_index, export in enumerate(package.exports, 1):
        if export_index in class_by_index:
            identity, placeholder = class_by_index[export_index]
        else:
            identity = _package_identity(
                package, export_index, "registry export",
                identity_table=identity_table)
            placeholder = False
        raw_identities.append((identity, placeholder))
    for export_index, export in enumerate(package.exports, 1):
        identity, placeholder = raw_identities[export_index - 1]
        class_identity = identity_table["classes"][export_index]
        objects.append({
            "class": class_identity,
            "export_index": export_index,
            "identity": identity,
            "placeholder": placeholder,
        })
    objects.sort(key=lambda item: (item["identity"].casefold(), item["identity"]))
    directives = []
    for export_index, export in enumerate(package.exports, 1):
        if package.objname(export["cls"]) != "TextBuffer" or export["size"] == 0:
            continue
        decoded = decode_script_text(package, export)
        owner = _package_identity(
            package, export_index, "script registry owner",
            identity_table=identity_table)
        for line_index, line in enumerate(decoded["text"].splitlines()):
            if line.lstrip().lower().startswith(b"#exec"):
                directives.append({
                    "line": bytes(line), "line_index": line_index,
                    "owner": owner,
                    "owner_class": identity_table["classes"][export_index],
                })
    directives.sort(key=lambda item: (
        item["owner"].casefold(), item["owner"], item["line_index"]))
    references = [
        (identity, "Core.Class", f"registry class {identity}")
        for _index, _export, identity, _placeholder in classes
    ] + [
        (directive["owner"], directive["owner_class"],
         f"registry directive owner {directive['owner']}")
        for directive in directives
    ]
    _require_semantic_reference_closure(identity_table, references)
    return {"classes": classes, "directives": directives,
            "imports": imports, "names": names, "objects": objects,
            "identity_table": identity_table}


def encode_cache_v3_registry_object(package: Package, *, fact_id: str,
                                    source_path: str, source_export: str,
                                    source_bytes: bytes) -> bytes:
    registry = _package_registry(package, _canonical_identity_table(package))
    if any(len(registry[key]) > MAX_CACHE_V3_REGISTRY_ITEMS
           for key in ("classes", "directives", "imports", "names", "objects")):
        raise CacheError(f"{package.name}: registry exceeds its item bound")
    payload = bytearray(b"WOTREG3\0")
    payload.extend(struct.pack(
        "<2H5I", 1, 0, len(registry["names"]), len(registry["imports"]),
        len(registry["objects"]), len(registry["classes"]),
        len(registry["directives"])))
    for name in registry["names"]:
        _append_v3_string(payload, name, "registry name")
    for imported in registry["imports"]:
        _append_v3_string(payload, imported["identity"], "import identity")
        _append_v3_string(payload, imported["class"], "import class")
    for obj in registry["objects"]:
        _append_v3_string(payload, obj["identity"], "object identity")
        _append_v3_string(payload, obj["class"], "object class")
        payload.extend(struct.pack("<I", obj["export_index"]))
        payload.extend(bytes((1 if obj["placeholder"] else 0,)))
    for export_index, _export, identity, placeholder in registry["classes"]:
        _append_v3_string(payload, identity, "class identity")
        payload.extend(struct.pack("<I", export_index))
        payload.extend(bytes((1 if placeholder else 0,)))
    for directive in registry["directives"]:
        _append_v3_string(payload, directive["owner"], "directive owner")
        payload.extend(struct.pack("<II", directive["line_index"],
                                   len(directive["line"])))
        payload.extend(directive["line"])
    header = {
        "class_count": len(registry["classes"]),
        "directive_count": len(registry["directives"]),
        "encoding": "ue1-package-registry-v1",
        "export_count": len(registry["objects"]),
        "fact_id": fact_id,
        "import_count": len(registry["imports"]),
        "importer": "registry-v1",
        "importer_version": CACHE_V3_IMPORTER_VERSION,
        "kind": "registry",
        "name_count": len(registry["names"]),
        "payload_byte_length": len(payload),
        "source": _cache_v3_source(source_path, source_export, source_bytes),
    }
    return encode_cache_v3_object(header, payload)


def encode_cache_v3_class_defaults_object(
        package: Package, *, fact_id: str, source_path: str,
        source_export: str, source_bytes: bytes) -> bytes:
    identity_table = _canonical_identity_table(package)
    classes = _class_exports(package, identity_table)
    script_resolver = _ScriptResolver(package.path)
    declaration_package = script_resolver.package(package.name)
    if declaration_package is None:
        raise CacheError(f"{package.name}: declaration package is unavailable")
    payload = bytearray(b"WOTDEF3\0")
    payload.extend(struct.pack("<2HI", 3, 0, len(classes)))
    default_count = 0
    reference_count = 0
    for export_index, export, identity, placeholder in classes:
        properties = [] if placeholder else _decoded_class_defaults(
            package, export, identity_table=identity_table,
            script_resolver=script_resolver,
            class_ref=(declaration_package, export_index))
        default_count += len(properties)
        pending = list(properties)
        while pending:
            prop = pending.pop()
            reference_count += bool(prop["object_identity"])
            pending.extend(prop.get("members", []))
        _append_v3_string(payload, identity, "class-default identity")
        payload.extend(struct.pack("<I", export_index))
        superclass = _package_identity(
            package, export["sup"], f"{identity} superclass", optional=True,
            identity_table=identity_table)
        _append_v3_string(payload, superclass, "class superclass", optional=True)
        payload.extend(bytes((1 if placeholder else 0,)))
        _append_v3_properties(payload, properties, include_struct_members=True)
    header = {
        "class_count": len(classes),
        "default_count": default_count,
        "encoding": "ue1-class-defaults-v2",
        "fact_id": fact_id,
        "importer": "class-default-v2",
        "importer_version": 2,
        "kind": "class_default",
        "payload_byte_length": len(payload),
        "reference_count": reference_count,
        "source": _cache_v3_source(source_path, source_export, source_bytes),
    }
    references = []
    for export_index, export, identity, placeholder in classes:
        references.append((identity, "Core.Class", f"class row {identity}"))
        superclass = _package_identity(
            package, export["sup"], f"{identity} superclass", optional=True,
            identity_table=identity_table)
        if superclass:
            references.append((
                superclass, identity_table["classes"][export["sup"]],
                f"{identity} superclass"))
        if not placeholder:
            pending = list(_decoded_class_defaults(
                package, export, identity_table=identity_table,
                script_resolver=script_resolver,
                class_ref=(declaration_package, export_index)))
            while pending:
                prop = pending.pop()
                if prop["object_identity"]:
                    references.append((
                        prop["object_identity"], prop["object_class"],
                        f"{identity}.{prop['name']}"))
                pending.extend(prop.get("members", []))
    _require_semantic_reference_closure(identity_table, references)
    return encode_cache_v3_object(header, payload)


def _decoded_class_defaults(package: Package, export: dict, *,
                            identity_table: dict | None = None,
                            script_resolver: _ScriptResolver | None = None,
                            class_ref: tuple[Package, int] | None = None) -> list[dict]:
    decoded = _read_class_defaults(package, export)
    if identity_table is None:
        identity_table = _canonical_identity_table(package)
    properties = _canonical_properties_from_tags(
        package, export, decoded["defaults"],
        identity_table=identity_table,
        context=f"{package.name}.{export['name']}",
        script_resolver=script_resolver, class_ref=class_ref)
    # UWindowLookAndFeel.uc declares HeadingActiveTitleColor without a
    # defaultproperties assignment, so UE1 omits its all-zero Color from the
    # tagged CDO stream. Match the native importer by preserving that declared
    # implicit default in the typed graph instead of inventing a UI fallback.
    if (package.name == "UWindow" and export["name"] == "UWindowLookAndFeel"
            and script_resolver is not None and class_ref is not None
            and not any(prop["array_index"] == 0
                        and prop["name"].casefold()
                        == "headingactivetitlecolor"
                        for prop in properties)):
        value = bytes(4)
        properties.append({
            "array_index": 0,
            "name": "HeadingActiveTitleColor",
            "name_value": "",
            "object_class": "",
            "object_identity": "",
            "struct": "Color",
            "type": 10,
            "value": value,
            "members": _resolved_reflected_struct_members(
                package, class_ref, "HeadingActiveTitleColor", 0, "Color",
                value, identity_table=identity_table,
                resolver=script_resolver,
                context="UWindow.UWindowLookAndFeel.HeadingActiveTitleColor"),
        })
        properties.sort(key=lambda item: (
            item["name"].casefold(), item["array_index"], item["name"]))
    return properties


def audit_remaining_reference_closure(package: Package) -> dict[str, dict[str, int]]:
    """Join every semantic reference to the package registry."""
    identity_table = _canonical_identity_table(package)
    registry = _package_registry(package, identity_table)
    references = {
        "class_ids": [],
        "superclasses": [],
        "typed_object_properties": [],
        "procedural_ids": [],
        "procedural_references": [],
        "font_ids": [],
        "font_pages": [],
        "directive_owners": [],
    }
    for export_index, export, identity, placeholder in _class_exports(
            package, identity_table):
        references["class_ids"].append(
            (identity, "Core.Class", f"class row {identity}"))
        if export["sup"]:
            super_identity, super_class = _bind_semantic_reference(
                package, export["sup"], f"{identity} superclass",
                identity_table=identity_table)
            references["superclasses"].append(
                (super_identity, super_class, f"{identity} superclass"))
        if not placeholder:
            for prop in _decoded_class_defaults(
                    package, export, identity_table=identity_table):
                if prop["object_identity"]:
                    references["typed_object_properties"].append((
                        prop["object_identity"], prop["object_class"],
                        f"{identity}.{prop['name']}"))

    for export_index, export in enumerate(package.exports, 1):
        class_name = package.objname(export["cls"])
        if class_name == "Font":
            identity = identity_table["identities"][export_index]
            references["font_ids"].append((
                identity, identity_table["classes"][export_index],
                f"font fact {identity}"))
            font = decode_font(package, export)
            for page in font["pages"]:
                identity, expected_class = _bind_semantic_reference(
                    package, page["texture_ref"],
                    f"{font['name']} page {page['page']} texture",
                    identity_table=identity_table)
                references["font_pages"].append((
                    identity, expected_class,
                    f"{font['name']} page {page['page']} texture"))
        elif class_name in FRACTAL_CLASSES:
            identity = identity_table["identities"][export_index]
            references["procedural_ids"].append((
                identity, identity_table["classes"][export_index],
                f"procedural fact {identity}"))
            properties = _decoded_property_tags(
                package, export, identity_table=identity_table)
            bound = []
            for prop in properties:
                if not prop["object_identity"]:
                    continue
                item = (prop["object_identity"], prop["object_class"],
                        f"{package.name}.{export['name']}.{prop['name']}")
                references["typed_object_properties"].append(item)
                bound.append(item)
            seen = set()
            for item in sorted(bound, key=lambda value: (
                    value[0].casefold(), value[0], value[1].casefold(), value[1])):
                key = (item[0].casefold(), item[1].casefold())
                if key not in seen:
                    seen.add(key)
                    references["procedural_references"].append(item)

    row_class = {row["identity"].casefold(): row["class"]
                 for row in identity_table["rows"]}
    for directive in registry["directives"]:
        references["directive_owners"].append((
            directive["owner"], row_class[directive["owner"].casefold()],
            f"directive owner {directive['owner']}"))

    result = {name: _semantic_reference_closure_counts(identity_table, items)
              for name, items in references.items()}
    all_references = [item for items in references.values() for item in items]
    result["all"] = _semantic_reference_closure_counts(
        identity_table, all_references)
    return result


_CP1252_UNDEFINED = frozenset((0x81, 0x8d, 0x8f, 0x90, 0x9d))


def parse_localization_bytes(raw: bytes) -> tuple[list[str], list[tuple[str, str]]]:
    raw = bytes(raw)
    if not raw or b"\0" in raw or any(byte in _CP1252_UNDEFINED for byte in raw):
        raise CacheError("localization source is not valid Windows-1252 text")
    try:
        text = raw.decode("cp1252")
    except UnicodeDecodeError as exc:
        raise CacheError("localization source is not valid Windows-1252 text") from exc
    sections = []
    rows = []
    section = None
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        stripped = raw_line.strip()
        if not stripped or stripped.startswith((";", "#")):
            continue
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].strip()
            if not section:
                raise CacheError(f"localization line {line_number} has an empty section")
            sections.append(section)
            continue
        if section is None or "=" not in raw_line:
            raise CacheError(f"localization line {line_number} is malformed")
        key, _value = raw_line.split("=", 1)
        key = key.strip()
        if not key:
            raise CacheError(f"localization line {line_number} has an empty key")
        identity = f"{section}.{key}"
        rows.append((section, key))
    if not sections:
        raise CacheError("localization source has no sections")
    return sections, rows


def encode_cache_v3_localization_object(
        raw: bytes, *, fact_id: str, source_path: str,
        source_export: str) -> bytes:
    sections, rows = parse_localization_bytes(raw)
    if len(rows) > MAX_CACHE_V3_LOCALIZATION_ROWS:
        raise CacheError("localization source exceeds its row bound")
    payload = bytes(raw)
    header = {
        "encoding": "windows-1252-ini-bytes-v1",
        "entry_count": len(rows),
        "fact_id": fact_id,
        "fallback_language": "en",
        "importer": "localization-v1",
        "importer_version": CACHE_V3_IMPORTER_VERSION,
        "kind": "localization",
        "payload_byte_length": len(payload),
        "section_count": len(sections),
        "source": _cache_v3_source(source_path, source_export, payload),
    }
    return encode_cache_v3_object(header, payload)


def cache_v3_manifest_bytes(
        blobs: Sequence[dict], *,
        build_identity: dict[str, str] | None = None,
        world_descriptors: Sequence[dict] | None = None,
        front_end_descriptor_root: str | None = None) -> bytes:
    """Canonical native/Python manifest projection for completed objects."""
    records = []
    seen = set()
    for blob in blobs:
        fields = {"logical_id", "schema", "bytes", "satisfies"}
        if not isinstance(blob, dict) or set(blob) != fields:
            raise CacheError("cache-v3 manifest blob has an invalid field set")
        logical_id = blob["logical_id"]
        if not isinstance(logical_id, str) or not logical_id or logical_id in seen:
            raise CacheError("cache-v3 manifest has a duplicate logical id")
        seen.add(logical_id)
        raw = bytes(blob["bytes"])
        satisfies = sorted(blob["satisfies"])
        if (not raw or not isinstance(blob["schema"], str) or not blob["schema"]
                or not satisfies or any(not isinstance(item, str) or not item
                                        for item in satisfies)):
            raise CacheError("cache-v3 manifest blob is incomplete")
        records.append({
            "logical_id": logical_id,
            "satisfies": satisfies,
            "schema": blob["schema"],
            "sha256": _sha256_bytes(raw),
            "size": len(raw),
        })
    records.sort(key=lambda item: item["logical_id"])
    manifest = {
        "blobs": records,
        "cache_version": CACHE_V3_VERSION,
        "format": CACHE_FORMAT,
        "hash_algorithm": HASH_ALGORITHM,
    }
    if build_identity is not None:
        required = {
            "contract_sha256", "installer_sha256",
            "production_descriptor_sha256",
        }
        if (not isinstance(build_identity, dict)
                or set(build_identity) != required
                or any(not _is_sha256(build_identity[field])
                       for field in required)):
            raise CacheError("cache-v3 build identity is incomplete")
        manifest.update(build_identity)
        if (world_descriptors is None or not isinstance(world_descriptors, (list, tuple))
                or any(not isinstance(row, dict) for row in world_descriptors)
                or not _is_sha256(front_end_descriptor_root)):
            raise CacheError("cache-v3 completed manifest metadata is incomplete")
        manifest["world_descriptors"] = list(world_descriptors)
        manifest["front_end_descriptor_root"] = front_end_descriptor_root
    elif world_descriptors is not None or front_end_descriptor_root is not None:
        raise CacheError("cache-v3 fixture metadata requires a build identity")
    return _canonical_json_bytes(manifest)


def required_world_names() -> tuple[str, ...]:
    """Return the sole multiplayer world census owned by import_all.MP_MAPS."""
    names = tuple(MP_MAPS)
    if len(names) != 19 or len(set(names)) != len(names):
        raise CacheError("import_all.MP_MAPS is not the required nineteen-world set")
    return names


def _world_hash(domain: bytes, value: object) -> str:
    digest = hashlib.sha256()
    digest.update(domain)
    encoded = _canonical_json_bytes(value)
    digest.update(struct.pack("<Q", len(encoded)))
    digest.update(encoded)
    return digest.hexdigest()


def _world_map_without_root(world: dict) -> dict:
    return {key: value for key, value in world.items() if key != "map_root"}


def world_map_root(world: dict) -> str:
    """Hash every canonical fact in one world, excluding only its own root."""
    if not isinstance(world, dict):
        raise CacheError("world descriptor must be an object")
    return _world_hash(WORLD_MAP_ROOT_DOMAIN, _world_map_without_root(world))


def world_set_root(worlds: Sequence[dict]) -> str:
    """Framed digest over the ordered identities and independently rooted maps."""
    digest = hashlib.sha256()
    digest.update(WORLD_SET_ROOT_DOMAIN)
    digest.update(struct.pack("<I", len(worlds)))
    for world in worlds:
        identity = world.get("identity") if isinstance(world, dict) else None
        root = world.get("map_root") if isinstance(world, dict) else None
        if not isinstance(identity, str) or not identity or not _is_sha256(root):
            raise CacheError("world-set root input is incomplete")
        for encoded in (identity.encode("utf-8"), bytes.fromhex(root)):
            digest.update(struct.pack("<Q", len(encoded)))
            digest.update(encoded)
    return digest.hexdigest()


def _synthetic_world(index: int, identity: str) -> dict:
    """Small but semantically complete map used by both world-set runners."""
    actor_prefix = f"{identity}."
    actors = [
        {
            "class": "LevelInfo", "identity": actor_prefix + "LevelInfo0",
            "properties": {"AmbientBrightness": 8 + index, "bSkyZone": False},
            "references": [],
        },
        {
            "class": "Mover", "identity": actor_prefix + "Mover0",
            "properties": {"bBlockActors": True, "key_num": index % 8},
            "references": [actor_prefix + "LevelInfo0"],
        },
        {
            "class": "Light", "identity": actor_prefix + "Light0",
            "properties": {"LightBrightness": 32 + index, "LightRadius": 16},
            "references": [actor_prefix + "ZoneInfo0"],
        },
        {
            "class": "AmbientSound", "identity": actor_prefix + "AmbientSound0",
            "properties": {"SoundRadius": 24, "SoundVolume": 128},
            "references": ["sound:AmbientA.Water.DIVE"],
        },
        {
            "class": "WotPathNode", "identity": actor_prefix + "WotPathNode0",
            "properties": {"CollisionHeight": 40, "CollisionRadius": 40},
            "references": [actor_prefix + "WotPathNode1"],
        },
        {
            "class": "WotPathNode", "identity": actor_prefix + "WotPathNode1",
            "properties": {"CollisionHeight": 40, "CollisionRadius": 40},
            "references": [],
        },
        {
            "class": "SkyZoneInfo", "identity": actor_prefix + "ZoneInfo0",
            "properties": {"AmbientBrightness": 4, "bSkyZone": True},
            "references": [],
        },
    ]
    world = {
        "actors": actors,
        "attachments": [{"child": actor_prefix + "Mover0",
                         "parent": actor_prefix + "LevelInfo0"}],
        "bsp": {
            "bounds_bits": [0, 0, 0, 1120403456, 1120403456, 1120403456],
            "collision_surfaces": [0],
            "nodes": [{"back": -1, "front": -1, "surface": 0,
                       "vertex_indices": [0, 1, 2], "zones": [0, 1]}],
            "portals": [{"back_zone": 0, "front_zone": 1, "surface": 1}],
            "surfaces": [
                {"flags": 0, "material": "Environment__Stone"},
                {"flags": 67108864, "material": "Environment__Portal"},
            ],
            "zones": [
                {"actor": actor_prefix + "LevelInfo0", "connectivity": 3,
                 "visibility": 3},
                {"actor": actor_prefix + "ZoneInfo0", "connectivity": 3,
                 "visibility": 3},
            ],
        },
        "identity": identity,
        "lights": [actor_prefix + "Light0"],
        "navigation_seeds": [actor_prefix + "WotPathNode0",
                             actor_prefix + "WotPathNode1"],
        "sky_zone": actor_prefix + "ZoneInfo0",
        "sounds": [{"actor": actor_prefix + "AmbientSound0",
                    "asset": "AmbientA.Water.DIVE"}],
    }
    world["map_root"] = world_map_root(world)
    return world


def synthetic_map_set_v3() -> dict:
    worlds = [_synthetic_world(index, identity)
              for index, identity in enumerate(required_world_names())]
    return {
        "format": WORLD_SET_FORMAT,
        "maps": worlds,
        "set_root": world_set_root(worlds),
    }


def _require_exact_fields(value: object, fields: set[str], label: str) -> dict:
    if not isinstance(value, dict) or set(value) != fields:
        raise CacheError(f"{label} has an invalid canonical field set")
    return value


def validate_map_set_v3(document: object, *, interrupt_after: int | None = None) -> str:
    """Validate all maps and cross-map facts before returning the set root.

    This function deliberately has no writer argument.  The cache builder calls
    it before exposing any completed manifest, mirroring the native importer's
    discovery pass.  ``interrupt_after`` is a test fault point counted in maps.
    """
    root = _require_exact_fields(
        document, {"format", "maps", "set_root"}, "world set")
    if root["format"] != WORLD_SET_FORMAT or not isinstance(root["maps"], list):
        raise CacheError("world set has an invalid format or map vector")
    required = required_world_names()
    identities = [item.get("identity") if isinstance(item, dict) else None
                  for item in root["maps"]]
    if any(not isinstance(identity, str) or not identity
           for identity in identities):
        raise CacheError("world set has an invalid map identity")
    if len(identities) != len(set(identities)):
        raise CacheError("world set has a duplicate map identity")
    if tuple(identities) != required:
        raise CacheError("world set is not exactly import_all.MP_MAPS in canonical order")

    actor_ids: set[str] = set()
    actor_classes: dict[str, str] = {}
    for world_index, world_value in enumerate(root["maps"]):
        if interrupt_after is not None and world_index >= interrupt_after:
            raise CacheError(f"world-set import interrupted before map {world_index + 1}")
        world = _require_exact_fields(world_value, {
            "actors", "attachments", "bsp", "identity", "lights", "map_root",
            "navigation_seeds", "sky_zone", "sounds",
        }, f"world[{world_index}]")
        if (not isinstance(world["actors"], list)
                or not 1 <= len(world["actors"]) <= MAX_WORLD_ACTORS
                or not isinstance(world["attachments"], list)
                or not isinstance(world["lights"], list)
                or not isinstance(world["navigation_seeds"], list)
                or not isinstance(world["sounds"], list)
                or not isinstance(world["sky_zone"], str)):
            raise CacheError(f"world[{world_index}] has invalid actor registries")
        for actor_index, actor_value in enumerate(world["actors"]):
            actor = _require_exact_fields(
                actor_value, {"class", "identity", "properties", "references"},
                f"world[{world_index}].actors[{actor_index}]")
            identity = actor["identity"]
            if (not isinstance(identity, str) or not identity
                    or identity in actor_ids or not isinstance(actor["class"], str)
                    or not actor["class"] or not isinstance(actor["properties"], dict)
                    or not isinstance(actor["references"], list)):
                raise CacheError(f"world[{world_index}] has an invalid actor identity")
            actor_ids.add(identity)
            actor_classes[identity] = actor["class"]

    reference_count = 0
    for world_index, world in enumerate(root["maps"]):
        bsp = _require_exact_fields(world["bsp"], {
            "bounds_bits", "collision_surfaces", "nodes", "portals", "surfaces",
            "zones",
        }, f"world[{world_index}].bsp")
        if (not isinstance(bsp["nodes"], list)
                or len(bsp["nodes"]) > MAX_WORLD_BSP_NODES
                or not isinstance(bsp["surfaces"], list)
                or len(bsp["surfaces"]) > MAX_WORLD_BSP_NODES
                or not isinstance(bsp["zones"], list)
                or not 1 <= len(bsp["zones"]) <= 64
                or not isinstance(bsp["portals"], list)
                or not isinstance(bsp["collision_surfaces"], list)
                or not isinstance(bsp["bounds_bits"], list)
                or len(bsp["bounds_bits"]) != 6
                or any(not _is_int(bits) or not 0 <= bits <= 0xffffffff
                       for bits in bsp["bounds_bits"])):
            raise CacheError(f"world[{world_index}].bsp vectors are invalid")
        surface_count = len(bsp["surfaces"])
        zone_count = len(bsp["zones"])
        node_count = len(bsp["nodes"])
        for surface_index, surface_value in enumerate(bsp["surfaces"]):
            surface = _require_exact_fields(
                surface_value, {"flags", "material"},
                f"world[{world_index}].bsp.surfaces[{surface_index}]")
            if (not _is_int(surface["flags"])
                    or not 0 <= surface["flags"] <= 0xffffffff
                    or not isinstance(surface["material"], str)
                    or not surface["material"]):
                raise CacheError(f"world[{world_index}] has a corrupt BSP surface")
        for zone_index, zone_value in enumerate(bsp["zones"]):
            zone = _require_exact_fields(
                zone_value, {"actor", "connectivity", "visibility"},
                f"world[{world_index}].bsp.zones[{zone_index}]")
            if (not isinstance(zone["actor"], str) or not zone["actor"]
                    or any(not _is_int(zone[key]) or not 0 <= zone[key] < (1 << 64)
                           for key in ("connectivity", "visibility"))):
                raise CacheError(f"world[{world_index}] has a corrupt BSP zone")
        for node_index, node_value in enumerate(bsp["nodes"]):
            node = _require_exact_fields(node_value, {
                "back", "front", "surface", "vertex_indices", "zones",
            }, f"world[{world_index}].bsp.nodes[{node_index}]")
            if (not _is_int(node["surface"]) or not 0 <= node["surface"] < surface_count
                    or any(not _is_int(node[branch])
                           or not -1 <= node[branch] < node_count
                           for branch in ("back", "front"))
                    or not isinstance(node["vertex_indices"], list)
                    or len(node["vertex_indices"]) < 3
                    or any(not _is_int(vertex) or vertex < 0
                           for vertex in node["vertex_indices"])
                    or not isinstance(node["zones"], list) or len(node["zones"]) != 2
                    or any(not _is_int(zone) or not 0 <= zone < zone_count
                           for zone in node["zones"])):
                raise CacheError(f"world[{world_index}] has a corrupt BSP node")
        for portal in bsp["portals"]:
            _require_exact_fields(
                portal, {"back_zone", "front_zone", "surface"}, "BSP portal")
            if (not _is_int(portal["surface"])
                    or not 0 <= portal["surface"] < surface_count
                    or not bsp["surfaces"][portal["surface"]]["flags"] & 0x04000000
                    or any(not _is_int(portal[key]) or not 0 <= portal[key] < zone_count
                           for key in ("back_zone", "front_zone"))):
                raise CacheError(f"world[{world_index}] has a corrupt BSP portal")
        collision_surfaces = bsp["collision_surfaces"]
        for surface in collision_surfaces:
            if (not _is_int(surface) or not 0 <= surface < surface_count
                    or bsp["surfaces"][surface]["flags"] & 0x04000001):
                raise CacheError(f"world[{world_index}] has invalid collision")
        if len(collision_surfaces) != len(set(collision_surfaces)):
            raise CacheError(f"world[{world_index}] has duplicate collision")

        local_ids = {actor["identity"] for actor in world["actors"]}
        if any(zone["actor"] not in local_ids for zone in bsp["zones"]):
            raise CacheError(f"world[{world_index}] has a dangling BSP zone actor")
        edges: dict[str, list[str]] = {identity: [] for identity in local_ids}
        for attachment in world["attachments"]:
            _require_exact_fields(attachment, {"child", "parent"}, "attachment")
            child, parent = attachment["child"], attachment["parent"]
            if child not in local_ids or parent not in local_ids:
                raise CacheError(f"world[{world_index}] has a dangling attachment")
            edges[child].append(parent)
        visiting: set[str] = set()
        visited: set[str] = set()

        def visit(identity: str) -> None:
            if identity in visiting:
                raise CacheError(f"world[{world_index}] has an attachment cycle")
            if identity in visited:
                return
            visiting.add(identity)
            for parent in edges[identity]:
                visit(parent)
            visiting.remove(identity)
            visited.add(identity)

        for identity in sorted(local_ids):
            visit(identity)
        for actor in world["actors"]:
            for reference in actor["references"]:
                reference_count += 1
                if (not isinstance(reference, str)
                        or (reference not in actor_ids
                            and not reference.startswith("sound:"))):
                    raise CacheError(f"world[{world_index}] has a dangling actor reference")
        if reference_count > MAX_WORLD_REFERENCES:
            raise CacheError("world set exceeds its reference bound")
        for light in world["lights"]:
            if light not in local_ids or actor_classes.get(light) != "Light":
                raise CacheError(f"world[{world_index}] has an invalid light registry")
        for seed in world["navigation_seeds"]:
            if seed not in local_ids or actor_classes.get(seed) != "WotPathNode":
                raise CacheError(f"world[{world_index}] has an invalid navigation seed")
        if (world["sky_zone"] not in local_ids
                or actor_classes.get(world["sky_zone"]) != "SkyZoneInfo"):
            raise CacheError(f"world[{world_index}] has an invalid sky/zone binding")
        for sound in world["sounds"]:
            _require_exact_fields(sound, {"actor", "asset"}, "sound binding")
            if (sound["actor"] not in local_ids
                    or actor_classes.get(sound["actor"]) != "AmbientSound"
                    or not isinstance(sound["asset"], str) or not sound["asset"]):
                raise CacheError(f"world[{world_index}] has an invalid sound binding")
        actual_map_root = world_map_root(world)
        if world["map_root"] != actual_map_root:
            raise CacheError(f"world[{world_index}] map root mismatch")

    actual_set_root = world_set_root(root["maps"])
    if root["set_root"] != actual_set_root:
        raise CacheError("world set root mismatch")
    return actual_set_root


def retail_world_census(retail_root: Path) -> list[dict]:
    """Reproducible package-byte census for the exact MP_MAPS source set."""
    records = []
    for identity in required_world_names():
        path = Path(retail_root) / "Maps" / f"{identity}.wot"
        if not path.is_file():
            raise CacheError(f"missing multiplayer world source: {path}")
        source = path.read_bytes()
        package = Package(path)
        levels = [item for item in package.exports
                  if package.objname(item["cls"]) == "Level"]
        models = [item for item in package.exports
                  if package.objname(item["cls"]) == "Model"]
        if len(levels) != 1 or not models:
            raise CacheError(f"{path} does not contain one Level and BSP Model data")
        model = decode_model(package, max(models, key=lambda item: item["size"]))
        records.append({
            "exports": len(package.exports),
            "identity": identity,
            "imports": len(package.imports),
            "nodes": len(model["nodes"]),
            "package_sha256": _sha256_bytes(source),
            "points": len(model["points"]),
            "portal_surfaces": sum(
                bool(surface["flags"] & 0x04000000)
                for surface in model["surfs"]),
            "size": len(source),
            "surfaces": len(model["surfs"]),
            "version": package.version,
            "vertices": len(model["verts"]),
        })
    return records


def retail_world_set_root(retail_root: Path) -> tuple[str, list[dict]]:
    """Source-byte root mirrored by ImportRequiredWorldSet descriptors."""
    census = retail_world_census(retail_root)
    rooted = []
    for record in census:
        digest = hashlib.sha256()
        digest.update(WORLD_MAP_SOURCE_ROOT_DOMAIN)
        for encoded in (record["identity"].encode("utf-8"),
                        bytes.fromhex(record["package_sha256"])):
            digest.update(struct.pack("<Q", len(encoded)))
            digest.update(encoded)
        rooted.append({**record, "map_root": digest.hexdigest()})
    digest = hashlib.sha256()
    digest.update(WORLD_SET_ROOT_DOMAIN)
    digest.update(struct.pack("<I", len(rooted)))
    for record in rooted:
        for encoded in (record["identity"].encode("utf-8"),
                        bytes.fromhex(record["map_root"])):
            digest.update(struct.pack("<Q", len(encoded)))
            digest.update(encoded)
    return digest.hexdigest(), rooted


def _ue1_cindex(value: int) -> bytes:
    if isinstance(value, bool) or not isinstance(value, int):
        raise CacheError("UE1 compact index must be an integer")
    negative = value < 0
    remaining = abs(value)
    first = remaining & 0x3f
    remaining >>= 6
    result = bytearray((first | (0x80 if negative else 0)
                        | (0x40 if remaining else 0),))
    while remaining:
        byte = remaining & 0x7f
        remaining >>= 7
        result.append(byte | (0x80 if remaining else 0))
        if len(result) > 5:
            raise CacheError("UE1 compact index exceeds five bytes")
    return bytes(result)


def _ue1_property(name_index: int, property_type: int,
                  value: bytes = b"", *, bool_value: bool = False) -> bytes:
    if property_type == 3:
        return (_ue1_cindex(name_index)
                + bytes((property_type | (0x80 if bool_value else 0),)))
    size = len(value)
    fixed_sizes = {1: 0x00, 2: 0x10, 4: 0x20, 12: 0x30, 16: 0x40}
    if size in fixed_sizes:
        size_bytes = b""
        size_bits = fixed_sizes[size]
    elif size <= 0xff:
        size_bytes = bytes((size,))
        size_bits = 0x50
    elif size <= 0xffff:
        size_bytes = struct.pack("<H", size)
        size_bits = 0x60
    else:
        size_bytes = struct.pack("<I", size)
        size_bits = 0x70
    return (_ue1_cindex(name_index) + bytes((property_type | size_bits,))
            + size_bytes + value)


def _ue1_package(exports: Sequence[dict], *, version: int = 68,
                 extra_names: Sequence[str] = (),
                 object_imports: Sequence[dict] = ()) -> bytes:
    """Emit the minimal real UE1 package grammar consumed by both readers."""
    base_names = ["None", "Core", "Class", "Package", "Palette", "Texture",
                  "Sound", "Music", "Format", "bMasked", "WAV"]
    names = list(dict.fromkeys(
        base_names + [entry["class"] for entry in exports]
        + [entry["name"] for entry in exports]
        + [value for entry in object_imports for value in (
            entry["class_package"], entry["class_name"], entry["name"])]
        + list(extra_names)))
    name_index = {name: index for index, name in enumerate(names)}
    class_names = list(dict.fromkeys(
        entry["class"] for entry in exports
        if entry.get("class_ref") is None))
    imports = bytearray()
    for class_name in class_names:
        imports += _ue1_cindex(name_index["Core"])
        imports += _ue1_cindex(name_index["Class"])
        imports += struct.pack("<i", 0)
        imports += _ue1_cindex(name_index[class_name])
    for entry in object_imports:
        imports += _ue1_cindex(name_index[entry["class_package"]])
        imports += _ue1_cindex(name_index[entry["class_name"]])
        imports += struct.pack("<i", entry.get("outer", 0))
        imports += _ue1_cindex(name_index[entry["name"]])
    class_ref = {name: -(index + 1) for index, name in enumerate(class_names)}

    name_table = bytearray()
    for name in names:
        encoded = name.encode("latin-1") + b"\0"
        name_table += _ue1_cindex(len(encoded)) + encoded + struct.pack("<I", 0)

    payload_offset = 4096
    payloads = [bytes(entry["payload"](name_index, payload_offset))
                if callable(entry["payload"]) else bytes(entry["payload"])
                for entry in exports]
    # Texture/Sound lazy-array end offsets depend on their final export offsets.
    current = payload_offset
    resolved_payloads = []
    for entry, payload in zip(exports, payloads):
        factory = entry["payload"]
        if callable(factory):
            payload = bytes(factory(name_index, current))
        resolved_payloads.append(payload)
        current += len(payload)

    export_table = bytearray()
    current = payload_offset
    for entry, payload in zip(exports, resolved_payloads):
        export_table += _ue1_cindex(
            entry.get("class_ref", class_ref.get(entry["class"], 0)))
        export_table += _ue1_cindex(entry.get("super_ref", 0))
        export_table += struct.pack("<i", entry.get("group", 0))
        export_table += _ue1_cindex(name_index[entry["name"]])
        export_table += struct.pack("<I", 0)
        export_table += _ue1_cindex(len(payload))
        if payload:
            export_table += _ue1_cindex(current)
        current += len(payload)

    name_offset = 64
    import_offset = name_offset + len(name_table)
    export_offset = import_offset + len(imports)
    header = struct.pack(
        "<IHHIiiiiii", 0x9E2A83C1, version, 0, 0,
        len(names), name_offset, len(exports), export_offset,
        len(class_names) + len(object_imports), import_offset)
    if len(header) > name_offset or export_offset + len(export_table) > payload_offset:
        raise CacheError("synthetic UE1 package tables exceed their fixed bound")
    package = bytearray(header)
    package.extend(b"\0" * (name_offset - len(package)))
    package.extend(name_table)
    package.extend(imports)
    package.extend(export_table)
    package.extend(b"\0" * (payload_offset - len(package)))
    for payload in resolved_payloads:
        package.extend(payload)
    return bytes(package)


def _ue1_palette_payload(colors: Sequence[Sequence[int]]):
    def build(names: dict[str, int], _offset: int) -> bytes:
        result = bytearray(_ue1_cindex(names["None"]))
        result += _ue1_cindex(len(colors))
        for color in colors:
            result += bytes(color)
        return bytes(result)
    return build


def _ue1_texture_payload(texture: dict, palette_export: int, *,
                         mip_bits: Sequence[tuple[int, int]] | None = None,
                         lazy_end_adjust: int = 0,
                         hostile_bool_animation: str | None = None):
    def build(names: dict[str, int], offset: int) -> bytes:
        result = bytearray()
        result += _ue1_property(
            names["Palette"], 5, _ue1_cindex(palette_export))
        result += _ue1_property(
            names["bMasked"], 3, bool_value=bool(texture.get("masked")))
        result += _ue1_property(
            names["Format"], 1, bytes((texture.get("format", 0),)))
        if hostile_bool_animation is not None:
            result += _ue1_property(
                names[hostile_bool_animation], 3, bool_value=True)
        if texture.get("anim_next", 0):
            result += _ue1_property(
                names["AnimNext"], 5, _ue1_cindex(texture["anim_next"]))
        if texture.get("min_frame_rate", 0.0):
            result += _ue1_property(
                names["MinFrameRate"], 4,
                struct.pack("<f", texture["min_frame_rate"]))
        if texture.get("max_frame_rate", 0.0):
            result += _ue1_property(
                names["MaxFrameRate"], 4,
                struct.pack("<f", texture["max_frame_rate"]))
        if texture.get("prime_count", 0):
            result += _ue1_property(
                names["PrimeCount"], 1, bytes((texture["prime_count"],)))
        result += _ue1_cindex(names["None"])
        result += _ue1_cindex(len(texture["mips"]))
        for mip_index, (width, height, indices) in enumerate(texture["mips"]):
            indices = bytes(indices)
            lazy_end_at = len(result)
            result += b"\0\0\0\0"
            result += _ue1_cindex(len(indices)) + indices
            struct.pack_into(
                "<i", result, lazy_end_at,
                offset + len(result) + lazy_end_adjust)
            if mip_bits is None:
                if (width <= 0 or height <= 0
                        or width & (width - 1) or height & (height - 1)):
                    raise CacheError(
                        f"{texture.get('name', '<texture>')}: synthetic FMipmap "
                        f"dimensions must be powers of two, got {width}x{height}")
                ubits = width.bit_length() - 1
                vbits = height.bit_length() - 1
            else:
                ubits, vbits = mip_bits[mip_index]
            result += struct.pack("<iiBB", width, height, ubits, vbits)
        return bytes(result)
    return build


def _ue1_stream_payload(declared_format: str, payload: bytes):
    def build(names: dict[str, int], offset: int) -> bytes:
        prefix = _ue1_cindex(names["None"]) + _ue1_cindex(names[declared_format])
        size = _ue1_cindex(len(payload))
        end = offset + len(prefix) + 4 + len(size) + len(payload)
        return prefix + struct.pack("<i", end) + size + bytes(payload)
    return build


def _ue1_font_payload(texture_export: int,
                      metrics: Sequence[tuple[int, int, int, int]],
                      characters_per_page: int):
    def build(names: dict[str, int], _offset: int) -> bytes:
        result = bytearray(_ue1_cindex(names["None"]))
        result.extend(_ue1_cindex(1))
        result.extend(_ue1_cindex(texture_export))
        result.extend(_ue1_cindex(len(metrics)))
        for metric in metrics:
            result.extend(struct.pack("<4i", *metric))
        result.extend(struct.pack("<i", characters_per_page))
        return bytes(result)
    return build


def _ue1_fractal_payload(properties: Sequence[dict], mips: Sequence[tuple],
                         states: Sequence[Sequence[int]] = ()):
    def build(names: dict[str, int], _offset: int) -> bytes:
        result = bytearray()
        for prop in properties:
            property_type = prop["type"]
            if property_type == 10:
                value = bytes(prop.get("value", b""))
                base = _ue1_property(names[prop["name"]], property_type, value)
                # StructProperty's struct-name reference follows the info byte.
                name_length = len(_ue1_cindex(names[prop["name"]]))
                result.extend(base[:name_length + 1])
                result.extend(_ue1_cindex(names[prop["struct"]]))
                result.extend(base[name_length + 1:])
            else:
                result.extend(_ue1_property(
                    names[prop["name"]], property_type,
                    bytes(prop.get("value", b"")),
                    bool_value=bool(prop.get("bool", False))))
        result.extend(_ue1_cindex(names["None"]))
        result.extend(_ue1_cindex(len(mips)))
        for width, height, indices in mips:
            indices = bytes(indices)
            result.extend(_ue1_cindex(len(indices)))
            result.extend(indices)
            result.extend(struct.pack(
                "<iiBB", width, height,
                width.bit_length() - 1, height.bit_length() - 1))
        if states:
            result.extend(_ue1_cindex(len(states)))
            for state in states:
                result.extend(bytes(state))
        return bytes(result)
    return build


def _ue1_text_buffer_payload(text: bytes):
    def build(names: dict[str, int], _offset: int) -> bytes:
        encoded = bytes(text) + b"\0"
        return (_ue1_cindex(names["None"]) + struct.pack("<ii", 0, 0)
                + _ue1_cindex(len(encoded)) + encoded)
    return build


def _ue1_struct_payload(friendly_name: str, children: int, *,
                        super_ref: int = 0, next_ref: int = 0):
    def build(names: dict[str, int], _offset: int) -> bytes:
        result = bytearray(_ue1_cindex(names["None"]))
        for reference in (super_ref, next_ref, 0, children):
            result.extend(_ue1_cindex(reference))
        result.extend(_ue1_cindex(names[friendly_name]))
        result.extend(struct.pack("<iii", 0, 0, 0))
        return bytes(result)
    return build


def _ue1_field_payload(next_ref: int):
    def build(names: dict[str, int], _offset: int) -> bytes:
        return (_ue1_cindex(names["None"]) + _ue1_cindex(0)
                + _ue1_cindex(next_ref))
    return build


def _ue1_property_export_payload(next_ref: int, *, array_dim: int = 1,
                                  flags: int = 0, type_refs: Sequence[int] = (),
                                  fixed_count: int | None = None,
                                  rep_offset: int = 0):
    def build(names: dict[str, int], _offset: int) -> bytes:
        result = bytearray(_ue1_cindex(names["None"]))
        result.extend(_ue1_cindex(0))
        result.extend(_ue1_cindex(next_ref))
        result.extend(struct.pack("<iI", array_dim, flags))
        result.extend(_ue1_cindex(names["None"]))
        if flags & 0x20:
            result.extend(struct.pack("<H", rep_offset))
        for reference in type_refs:
            result.extend(_ue1_cindex(reference))
        if fixed_count is not None:
            result.extend(struct.pack("<i", fixed_count))
        return bytes(result)
    return build


def _ue1_default_property(names: dict[str, int], prop: dict) -> bytes:
    property_type = prop["type"]
    name = _ue1_cindex(names[prop["name"]])
    if property_type == 3:
        return name + bytes((0x50 | property_type, 0))
    value = bytes(prop.get("value", b""))
    encoded = _ue1_property(names[prop["name"]], property_type, value)
    info_at = len(name)
    if property_type == 10:
        encoded = (encoded[:info_at + 1]
                   + _ue1_cindex(names[prop["struct"]])
                   + encoded[info_at + 1:])
    array_index = prop.get("array_index", 0)
    if array_index:
        info = encoded[info_at] | 0x80
        if array_index < 0x80:
            array_bytes = bytes((array_index,))
        elif array_index < 0x4000:
            array_bytes = bytes((0x80 | (array_index >> 8), array_index & 0xff))
        else:
            if array_index > 0x3fffffff:
                raise CacheError("synthetic property array index exceeds UE1 encoding")
            array_bytes = bytes((0xc0 | (array_index >> 24),
                                 (array_index >> 16) & 0xff,
                                 (array_index >> 8) & 0xff,
                                 array_index & 0xff))
        # Insert the array index after struct/size metadata and before value.
        value_offset = len(encoded) - len(value)
        encoded = bytes((*(encoded[:info_at]), info, *encoded[info_at + 1:value_offset],
                         *array_bytes, *encoded[value_offset:]))
    return encoded


def _ue1_class_payload(script_text_ref: int, defaults: Sequence[dict],
                       *, mutation: str | None = None):
    def build(names: dict[str, int], _offset: int) -> bytes:
        result = bytearray()
        for reference in (0, 0, script_text_ref, 0):
            result.extend(_ue1_cindex(reference))
        result.extend(_ue1_cindex(names["SyntheticClass"]))
        result.extend(struct.pack("<iii", 0, 0, 0))
        result.extend(bytes(42))
        result.extend(_ue1_cindex(0))  # dependencies
        result.extend(_ue1_cindex(0))  # package imports
        result.extend(_ue1_cindex(0))  # ClassWithin
        result.extend(_ue1_cindex(names["None"]))
        for prop in defaults:
            result.extend(_ue1_default_property(names, prop))
        if mutation == "unknown-property-type":
            result.extend(_ue1_property(names["Mystery"], 7, b"\x01"))
        result.extend(_ue1_cindex(names["None"]))
        return bytes(result)
    return build


def _synthetic_core_source() -> bytes:
    exports = [
        {"class": "ScriptStruct", "name": "Vector", "payload": b""},
        {"class": "FloatProperty", "name": "X", "group": 1, "payload": b""},
        {"class": "FloatProperty", "name": "Y", "group": 1, "payload": b""},
        {"class": "FloatProperty", "name": "Z", "group": 1, "payload": b""},
    ]
    exports[0]["payload"] = _ue1_struct_payload("Vector", 2)
    exports[1]["payload"] = _ue1_property_export_payload(3)
    exports[2]["payload"] = _ue1_property_export_payload(4)
    exports[3]["payload"] = _ue1_property_export_payload(0)
    return _ue1_package(exports, extra_names=("ScriptStruct", "FloatProperty"))


def _synthetic_remaining_source(*, mutation: str | None = None) -> bytes:
    palette = [(index, 255 - index, index // 2, 255) for index in range(256)]
    atlas = {"name": "Atlas", "format": 0, "masked": True,
             "mips": [(8, 8, bytes(range(64)))]}
    metrics = [(index % 4 * 2, index // 4 * 2, 2, 2) for index in range(16)]
    if mutation == "invalid-font-metric":
        metrics[-1] = (7, 7, 2, 2)
    fire_mip = bytes((index * 7) & 0xff for index in range(64))
    fire_properties = [
        {"name": "Palette", "type": 5, "value": _ue1_cindex(1)},
        {"name": "bParametric", "type": 3, "bool": True},
        {"name": "RenderHeat", "type": 1, "value": b"\xc0"},
        {"name": "SparksLimit", "type": 2,
         "value": struct.pack("<i", 8)},
        {"name": "FX_Frequency", "type": 1, "value": b"\x05"},
    ]
    directives = (
        b"class SyntheticClass extends Object;\r\n"
        b"#exec TEXTURE IMPORT NAME=Atlas FILE=Atlas.pcx\r\n"
        b"#exec OBJ LOAD FILE=Sounds.uax PACKAGE=Sounds\r\n")
    defaults = [
        {"name": "Scale", "type": 4, "value": b"\0\0\0\x80"},
        {"name": "AtlasRef", "type": 5, "value": _ue1_cindex(2)},
        {"name": "StateName", "type": 6, "value": None},
        {"name": "Origin", "type": 10, "struct": "Vector",
         "value": struct.pack("<3f", 1.0, 2.0, 3.0)},
        {"name": "Composite", "type": 10, "struct": "CompositeStruct",
         "value": (struct.pack("<i6f", 42, 1.0, 2.0, 3.0,
                               4.0, 5.0, 6.0)
                   + struct.pack("<5i", 7, 8, 9, 99, 314))},
        {"name": "DepthBoundary", "type": 10,
         "struct": "DepthStruct0" if mutation == "struct-depth-9"
         else "DepthStruct1", "value": b""},
        {"name": "IndexedValue", "type": 2, "array_index": 130,
         "value": struct.pack("<i", 17)},
    ]
    if mutation == "reference-collision":
        defaults.append({
            "name": "MeshRef", "type": 5, "value": _ue1_cindex(8)})
    if mutation == "unsafe-object-reference":
        defaults[1]["value"] = _ue1_cindex(999999)
    if mutation == "struct-array-index-out-of-range":
        next(item for item in defaults
             if item["name"] == "Composite")["array_index"] = 1

    def class_payload(names: dict[str, int], offset: int) -> bytes:
        resolved = []
        for prop in defaults:
            item = dict(prop)
            if item["name"] == "StateName":
                item["value"] = _ue1_cindex(names["Active"])
            resolved.append(item)
        return _ue1_class_payload(
            6, resolved,
            mutation=mutation if mutation in (
                "unknown-property-type", "unsafe-object-reference") else None
        )(names, offset)

    exports = [
        {"class": "Palette", "name": "Palette0",
         "payload": _ue1_palette_payload(palette)},
        {"class": "Texture", "name": "Atlas",
         "payload": _ue1_texture_payload(atlas, 1)},
        {"class": "Font", "name": "FontA",
         "payload": _ue1_font_payload(2, metrics, 16)},
        {"class": "FireTexture", "name": "FireA",
         "payload": _ue1_fractal_payload(
             fire_properties, [(8, 8, fire_mip)],
             ((4, 220, 2, 3, 0, 0, 0, 0),
              (24, 255, 5, 5, 3, 2, 1, 64)))},
        {"class": "Texture", "name": "SourceTexture",
         "payload": _ue1_texture_payload(atlas, 1)},
        {"class": "TextBuffer", "name": "ScriptText", "group": 7,
         "payload": _ue1_text_buffer_payload(directives)},
        {"class": "Class", "class_ref": 0, "name": "SyntheticClass",
         "payload": class_payload},
    ]
    if mutation == "reference-collision":
        # Export 7 (Class) and export 8 (Mesh) deliberately share one raw
        # dotted path.  The class default targets export 8 and proves that the
        # canonical suffix is bound by reference, not guessed from the path.
        exports.append({
            "class": "Mesh", "name": "SyntheticClass", "payload": b""})

    nested_struct_ref = len(exports) + 1
    exports.append({"class": "ScriptStruct", "name": "NestedStruct",
                    "payload": b""})
    nested_value_ref = len(exports) + 1
    exports.append({"class": "IntProperty", "name": "NestedValue",
                    "group": nested_struct_ref, "payload": b""})
    base_composite_struct_ref = len(exports) + 1
    exports.append({"class": "ScriptStruct", "name": "BaseCompositeStruct",
                    "payload": b""})
    base_value_ref = len(exports) + 1
    exports.append({"class": "IntProperty", "name": "BaseValue",
                    "group": base_composite_struct_ref, "payload": b""})
    composite_struct_ref = len(exports) + 1
    exports.append({"class": "ScriptStruct", "name": "CompositeStruct",
                    "payload": b""})
    function_ref = len(exports) + 1
    exports.append({"class": "Function", "name": "IgnoredFunction",
                    "group": composite_struct_ref, "payload": b""})
    transient_ref = len(exports) + 1
    exports.append({"class": "IntProperty", "name": "TransientValue",
                    "group": composite_struct_ref, "payload": b""})
    nested_member_ref = len(exports) + 1
    exports.append({"class": "StructProperty", "name": "Nested",
                    "group": composite_struct_ref, "payload": b""})
    vectors_ref = len(exports) + 1
    exports.append({"class": "StructProperty", "name": "Vectors",
                    "group": composite_struct_ref, "payload": b""})
    fixed_values_ref = len(exports) + 1
    exports.append({"class": "FixedArrayProperty", "name": "FixedValues",
                    "group": composite_struct_ref, "payload": b""})
    fixed_inner_ref = len(exports) + 1
    exports.append({"class": "IntProperty", "name": "FixedInner",
                    "group": composite_struct_ref, "payload": b""})
    fixed_wrapper_refs = []
    for suffix in "BCDE":
        fixed_wrapper_refs.append(len(exports) + 1)
        exports.append({"class": "FixedArrayProperty",
                        "name": f"FixedWrapper{suffix}",
                        "group": composite_struct_ref, "payload": b""})
    replicated_value_ref = len(exports) + 1
    exports.append({"class": "IntProperty", "name": "ReplicatedValue",
                    "group": composite_struct_ref, "payload": b""})
    origin_property_ref = len(exports) + 1
    exports.append({"class": "StructProperty", "name": "Origin",
                    "group": 7, "payload": b""})
    composite_property_ref = len(exports) + 1
    exports.append({"class": "StructProperty", "name": "Composite",
                    "group": 7, "payload": b""})

    depth_struct_refs = []
    depth_member_refs = []
    for depth_index in range(10):
        depth_struct_refs.append(len(exports) + 1)
        exports.append({"class": "ScriptStruct",
                        "name": f"DepthStruct{depth_index}", "payload": b""})
        if depth_index < 9:
            depth_member_refs.append(len(exports) + 1)
            exports.append({"class": "StructProperty", "name": "Next",
                            "group": depth_struct_refs[-1], "payload": b""})
    depth_boundary_property_ref = len(exports) + 1
    exports.append({"class": "StructProperty", "name": "DepthBoundary",
                    "group": 7, "payload": b""})

    class_names = list(dict.fromkeys(
        entry["class"] for entry in exports
        if entry.get("class_ref") is None))
    core_package_ref = -(len(class_names) + 1)
    core_vector_ref = -(len(class_names) + 2)
    missing_package_ref = -(len(class_names) + 3)
    missing_struct_ref = -(len(class_names) + 4)
    object_imports = [
        {"class_package": "Core", "class_name": "Package", "outer": 0,
         "name": "Core"},
        {"class_package": "Core", "class_name": "ScriptStruct",
         "outer": core_package_ref, "name": "Vector"},
        {"class_package": "Core", "class_name": "Package", "outer": 0,
         "name": "Missing"},
        {"class_package": "Core", "class_name": "ScriptStruct",
         "outer": missing_package_ref, "name": "MissingStruct"},
    ]

    exports[nested_struct_ref - 1]["payload"] = _ue1_struct_payload(
        "NestedStruct", nested_value_ref)
    exports[nested_value_ref - 1]["payload"] = _ue1_property_export_payload(0)
    exports[base_composite_struct_ref - 1]["payload"] = _ue1_struct_payload(
        "BaseCompositeStruct", base_value_ref)
    exports[base_value_ref - 1]["payload"] = _ue1_property_export_payload(0)
    exports[composite_struct_ref - 1]["payload"] = _ue1_struct_payload(
        "CompositeStruct", function_ref, super_ref=base_composite_struct_ref)
    exports[function_ref - 1]["payload"] = _ue1_field_payload(transient_ref)
    exports[transient_ref - 1]["payload"] = _ue1_property_export_payload(
        nested_member_ref, flags=0x2000)
    exports[nested_member_ref - 1]["payload"] = _ue1_property_export_payload(
        vectors_ref, type_refs=(nested_struct_ref,))
    exports[vectors_ref - 1]["payload"] = _ue1_property_export_payload(
        function_ref if mutation == "struct-next-cycle" else fixed_values_ref,
        array_dim=2, type_refs=(core_vector_ref,))
    fixed_inner = fixed_inner_ref
    if mutation == "fixed-array-self-cycle":
        fixed_inner = fixed_values_ref
    elif mutation in ("fixed-array-two-node-cycle",
                      "fixed-array-wrapper-overflow"):
        fixed_inner = fixed_wrapper_refs[0]
    exports[fixed_values_ref - 1]["payload"] = _ue1_property_export_payload(
        replicated_value_ref, type_refs=(fixed_inner,), fixed_count=3)
    for wrapper_index, wrapper_ref in enumerate(fixed_wrapper_refs):
        if mutation == "fixed-array-two-node-cycle" and wrapper_index == 0:
            inner_ref = fixed_values_ref
        elif mutation == "fixed-array-wrapper-overflow":
            inner_ref = (fixed_wrapper_refs[wrapper_index + 1]
                         if wrapper_index + 1 < len(fixed_wrapper_refs)
                         else fixed_inner_ref)
        else:
            inner_ref = fixed_inner_ref
        exports[wrapper_ref - 1]["payload"] = _ue1_property_export_payload(
            0, type_refs=(inner_ref,), fixed_count=1)
    exports[fixed_inner_ref - 1]["payload"] = _ue1_property_export_payload(0)
    exports[replicated_value_ref - 1]["payload"] = _ue1_property_export_payload(
        0, flags=0x20, rep_offset=0x3456)
    exports[origin_property_ref - 1]["payload"] = _ue1_property_export_payload(
        0, type_refs=(core_vector_ref,))
    exports[composite_property_ref - 1]["payload"] = _ue1_property_export_payload(
        0, type_refs=(missing_struct_ref if mutation == "unresolvable-struct"
                      else composite_struct_ref,))
    for depth_index, struct_ref in enumerate(depth_struct_refs):
        child_ref = depth_member_refs[depth_index] if depth_index < 9 else 0
        exports[struct_ref - 1]["payload"] = _ue1_struct_payload(
            f"DepthStruct{depth_index}", child_ref)
        if depth_index < 9:
            exports[depth_member_refs[depth_index] - 1]["payload"] = \
                _ue1_property_export_payload(
                    0, type_refs=(depth_struct_refs[depth_index + 1],))
    depth_root = depth_struct_refs[0] if mutation == "struct-depth-9" \
        else depth_struct_refs[1]
    exports[depth_boundary_property_ref - 1]["payload"] = \
        _ue1_property_export_payload(0, type_refs=(depth_root,))
    extra_names = [
        "Active", "AtlasRef", "BaseCompositeStruct", "BaseValue", "Composite",
        "CompositeStruct", "Core", "DepthBoundary", "FixedArrayProperty",
        "FixedInner", "FixedValues", "FixedWrapperB", "FixedWrapperC",
        "FixedWrapperD", "FixedWrapperE", "FloatProperty", "Function",
        "FX_Frequency", "Font", "FireTexture", "IgnoredFunction", "IntProperty",
        "IndexedValue", "Mystery", "Next", "Origin", "RenderHeat",
        "ReplicatedValue", "Scale",
        "Missing", "MissingStruct", "Nested", "NestedStruct", "NestedValue",
        "ScriptStruct", "SparksLimit", "StateName", "StructProperty",
        "SyntheticClass", "TextBuffer", "TransientValue", "Vector", "Vectors",
        "bParametric",
    ]
    extra_names.extend(f"DepthStruct{index}" for index in range(10))
    if mutation == "reference-collision":
        extra_names.extend(("Mesh", "MeshRef"))
    if mutation == "duplicate-case-name":
        extra_names.extend(("Duplicate", "duplicate"))
    return _ue1_package(
        exports, extra_names=extra_names, object_imports=object_imports)


def _ue1_mesh_payload(mesh: dict):
    """Emit the v68 Mesh/LodMesh serialization decoded by ue1pkg.decode_mesh."""
    def build(names: dict[str, int], offset: int) -> bytes:
        result = bytearray(_ue1_cindex(names["None"]))

        def append_array(items, encode, *, lazy=False,
                         count_override: int | None = None,
                         lazy_end_adjust: int = 0) -> None:
            lazy_offset = None
            if lazy:
                lazy_offset = len(result)
                result.extend(b"\0\0\0\0")
            count = len(items) if count_override is None else count_override
            result.extend(_ue1_cindex(count))
            for item in items:
                result.extend(encode(item))
            if lazy_offset is not None:
                struct.pack_into(
                    "<i", result, lazy_offset,
                    offset + len(result) + lazy_end_adjust)

        result.extend(bytes(41))  # UPrimitive FBox + FSphere
        append_array(
            mesh["saved_vertices"], lambda value: struct.pack("<I", value),
            lazy=True, count_override=mesh.get("saved_count_override"),
            lazy_end_adjust=mesh.get("saved_lazy_end_adjust", 0))
        append_array(mesh.get("serialized_triangles", mesh.get("triangles", [])),
                     lambda triangle: struct.pack(
                         "<3H6BIi", *triangle["indices"],
                         *(component for uv in triangle["uvs"] for component in uv),
                         triangle["poly_flags"], triangle["texture_index"]),
                     lazy=True)

        def encode_sequence(sequence: dict) -> bytes:
            encoded = bytearray()
            encoded.extend(_ue1_cindex(names[sequence["name"]]))
            encoded.extend(_ue1_cindex(names[sequence["group"]]))
            encoded.extend(struct.pack(
                "<ii", sequence["start"], sequence["num"]))
            notifications = sequence.get("notifications", [])
            encoded.extend(_ue1_cindex(len(notifications)))
            for notify in notifications:
                encoded.extend(struct.pack("<f", notify["time"]))
                encoded.extend(_ue1_cindex(names[notify["function"]]))
            encoded.extend(struct.pack("<f", sequence["rate"]))
            return bytes(encoded)

        append_array(mesh.get("sequences", []), encode_sequence)
        append_array(mesh.get("connects", []),
                     lambda pair: struct.pack("<ii", *pair), lazy=True)
        result.extend(bytes(41))  # UMesh's redundant FBox + FSphere
        append_array(mesh.get("vert_links", []),
                     lambda value: struct.pack("<i", value), lazy=True)
        append_array(mesh.get("textures", []), _ue1_cindex)
        append_array(mesh.get("frame_boxes", []), lambda _item: bytes(25))
        append_array(mesh.get("frame_spheres", []), lambda _item: bytes(16))
        result.extend(struct.pack(
            "<iiII3f3f3iii", mesh["frame_verts"], mesh["anim_frames"],
            mesh.get("and_flags", 0), mesh.get("or_flags", 0),
            *mesh["scale"], *mesh["origin"],
            *mesh.get("rotation_origin", (0, 0, 0)),
            mesh.get("current_poly", 0), mesh.get("current_vertex", 0)))
        append_array(mesh.get("texture_lod", []),
                     lambda value: struct.pack("<f", value))

        lod = mesh.get("lod")
        if lod is not None:
            append_array(lod.get("collapse_points", []),
                         lambda value: struct.pack("<H", value))
            append_array(lod.get("face_levels", []),
                         lambda value: struct.pack("<H", value))
            append_array(lod.get("faces", []),
                         lambda face: struct.pack("<4H", *face))
            append_array(lod.get("collapse_wedges", []),
                         lambda value: struct.pack("<H", value))
            append_array(lod.get("wedges", []),
                         lambda wedge: struct.pack("<H2B", *wedge))
            append_array(lod.get("materials", []),
                         lambda material: struct.pack("<Ii", *material))
            append_array(lod.get("special_faces", []),
                         lambda face: struct.pack("<4H", *face))
            result.extend(struct.pack(
                "<iifffiff", lod["model_verts"], lod["special_verts"],
                lod.get("mesh_scale_max", 1.0), lod.get("hysteresis", 0.0),
                lod.get("strength", 1.0), lod.get("min_verts", 0),
                lod.get("morph", 0.0), lod.get("z_displace", 0.0)))
            append_array(lod.get("remap_anim_verts", []),
                         lambda value: struct.pack("<H", value))
            result.extend(struct.pack("<i", lod["old_frame_verts"]))
        return bytes(result)
    return build


def _synthetic_mesh_source(*, mutation: str | None = None) -> bytes:
    def packed(x: int, y: int, z: int) -> int:
        return ((x & 0x7ff) | ((y & 0x7ff) << 11)
                | ((z & 0x3ff) << 22))

    plain_triangles = [
        {"indices": (0, 1, 2), "uvs": ((0, 0), (255, 0), (255, 255)),
         "poly_flags": 0, "texture_index": 0},
        {"indices": (0, 2, 3), "uvs": ((0, 0), (255, 255), (0, 255)),
         "poly_flags": 0x00000102, "texture_index": 1},
        {"indices": (1, 3, 2), "uvs": ((32, 64), (96, 128), (160, 192)),
         "poly_flags": 0x00400010, "texture_index": 0},
    ]
    if mutation == "triangle-index":
        plain_triangles[0] = dict(plain_triangles[0], indices=(0, 1, 99))
    plain_sequences = [
        # UE1 accepts a terminal one-past count; shipped retail uses it too.
        {"name": "All", "group": "None", "start": 0, "num": 4,
         "rate": 30.0, "notifications": []},
        {"name": "Move", "group": "Moving", "start": 1, "num": 2,
         "rate": 12.5,
         "notifications": [{"time": 0.25, "function": "Footstep"}]},
    ]
    if mutation == "sequence-range":
        # End=5 exceeds the three-frame stream by two, so this remains invalid
        # after preserving UE1's valid one-past terminal convention.
        plain_sequences[1] = dict(plain_sequences[1], start=2, num=3)
    elif mutation == "sequence-overlap":
        plain_sequences[1] = dict(plain_sequences[1], name="All")
    elif mutation == "notify-time":
        plain_sequences[1] = dict(
            plain_sequences[1],
            notifications=[{"time": 1.5, "function": "Footstep"}])
    plain = {
        "saved_vertices": [
            packed(0, 0, 0), packed(10, 0, 0), packed(10, 10, 0), packed(0, 10, 0),
            packed(0, 0, 2), packed(10, 0, 2), packed(10, 10, 2), packed(0, 10, 2),
            packed(0, 0, 4), packed(10, 0, 4), packed(10, 10, 4), packed(0, 10, 4),
        ],
        "triangles": plain_triangles,
        "sequences": plain_sequences,
        "textures": [1, 2],
        "frame_verts": 4,
        "anim_frames": 3,
        "scale": (0.5, 0.5, 1.0),
        "origin": (1.0, 2.0, 0.0),
        "rotation_origin": (16384, 16384, 16384),
    }
    if mutation == "lazy-end":
        plain["saved_lazy_end_adjust"] = 1
    elif mutation == "numeric-overflow":
        plain["frame_verts"] = 1_000_000
        plain["anim_frames"] = 1_000_000
    elif mutation == "decompression-limit":
        plain["saved_vertices"] = []
        plain["saved_count_override"] = MAX_CACHE_V3_PACKED_VERTICES + 1
    elif mutation == "triangle-count-bomb":
        plain["triangles"] = [
            dict(plain_triangles[0])
            for _index in range(MAX_CACHE_V3_MESH_TRIANGLES + 1)
        ]

    lod = {
        "saved_vertices": [
            packed(0, 0, 0), packed(1, 0, 0), packed(2, 0, 0),
            packed(3, 0, 0), packed(4, 0, 0),
            packed(0, 1, 0), packed(1, 1, 0), packed(2, 1, 0),
            packed(3, 1, 0), packed(4, 1, 0),
        ],
        "serialized_triangles": [],
        "sequences": [{"name": "All", "group": "None", "start": 0,
                       "num": 2, "rate": 24.0, "notifications": []}],
        "textures": [1, 2],
        "frame_verts": 4,
        "anim_frames": 2,
        "scale": (1.0, 2.0, 3.0),
        "origin": (0.0, 0.0, 0.0),
        "lod": {
            "faces": [(0, 1, 2, 0), (0, 2, 3, 1)],
            "wedges": [(0, 0, 0), (1, 255, 0),
                       (2, 255, 255), (0, 0, 255)],
            "materials": [(0, 0), (0x00000002, 1)],
            "special_faces": [(0, 1, 2, 0)],
            "model_verts": 3,
            "special_verts": 1,
            "remap_anim_verts": [4, 2, 0, 1],
            "old_frame_verts": 5,
        },
    }
    if mutation == "lod-material":
        lod["lod"]["faces"][0] = (0, 1, 2, 9)
    elif mutation == "lod-remap":
        lod["lod"]["remap_anim_verts"] = [4, 2, 0, 99]
    elif mutation == "lod-special-overflow":
        lod["scale"] = (struct.unpack("<f", b"\xff\xff\x7f\x7f")[0], 1.0, 1.0)
        lod["lod"]["faces"] = []
        lod["lod"]["special_faces"] = [(0, 1, 2, 0)]

    extra_names = ("All", "Move", "Moving", "Footstep")
    exports = [
        {"class": "Texture", "name": "SkinA", "payload": b""},
        {"class": "Texture", "name": "SkinB", "payload": b""},
        {"class": "Mesh", "name": "PlainMesh",
         "payload": _ue1_mesh_payload(plain)},
        {"class": "LodMesh", "name": "LodHero",
         "payload": _ue1_mesh_payload(lod)},
    ]
    if mutation == "skeletal-class":
        exports[2] = {"class": "SkeletalMesh", "name": "PlainMesh",
                      "payload": _ue1_mesh_payload(plain)}
    return _ue1_package(exports, extra_names=extra_names)


def _decode_synthetic_meshes(source: bytes) -> tuple[Package, dict, dict, object]:
    temporary = tempfile.TemporaryDirectory()
    path = Path(temporary.name) / "mesh.u"
    path.write_bytes(source)
    package = Package(path)
    decoded = {
        export["name"]: decode_mesh(package, export)
        for export in package.exports
        if package.objname(export["cls"]) in ("Mesh", "LodMesh")
    }
    return package, decoded["PlainMesh"], decoded["LodHero"], temporary


def _synthetic_wav(*, bits: int, frames: int, sample_rate: int = 22050,
                   channels: int = 1,
                   loops: Sequence[tuple[int, int, int]] = (),
                   block_align: int | None = None,
                   byte_rate: int | None = None, format_tag: int = 1) -> bytes:
    expected_align = channels * (bits // 8)
    stored_align = expected_align if block_align is None else block_align
    stored_rate = sample_rate * stored_align if byte_rate is None else byte_rate
    samples = bytes(index & 0xff for index in range(frames * max(expected_align, 1)))
    fmt = struct.pack(
        "<4sIHHIIHH", b"fmt ", 16, format_tag, channels, sample_rate,
        stored_rate, stored_align, bits)
    chunks = [fmt]
    if loops:
        smpl_body = bytearray(struct.pack(
            "<9I", 0, 0, 0, 60, 0, 0, 0, len(loops), 0))
        for loop_index, (start, end, play_count) in enumerate(loops):
            smpl_body += struct.pack(
                "<6I", loop_index, 0, start, end, 0, play_count)
        chunks.append(struct.pack("<4sI", b"smpl", len(smpl_body)) + smpl_body)
    data = struct.pack("<4sI", b"data", len(samples)) + samples
    if len(samples) & 1:
        data += b"\0"
    chunks.append(data)
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def _load_inno_fixture_helpers():
    import importlib.util
    path = REPO / "Tools/build/stage_innoextract_native.py"
    spec = importlib.util.spec_from_file_location("wot_cache_v3_inno_fixture", path)
    if spec is None or spec.loader is None:
        raise CacheError(f"cannot load checked-in Inno fixture generator: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _synthetic_inno(entries: Sequence[tuple[str, bytes]], *,
                    declared_sizes: Sequence[int] | None = None,
                    hash_mismatch_index: int | None = None) -> bytes:
    stage = _load_inno_fixture_helpers()
    if not entries:
        raise CacheError("synthetic cache-v3 installer needs an entry")
    destinations = [("{app}\\" + path.replace("/", "\\")).encode("ascii")
                    for path, _data in entries]
    primary = stage._main_header(destinations, compression=1)
    chunks = [bytes(data) for _path, data in entries]
    expected_chunks = list(chunks)
    if hash_mismatch_index is not None:
        damaged = bytearray(chunks[hash_mismatch_index])
        damaged[-1] ^= 0x5a
        chunks[hash_mismatch_index] = bytes(damaged)
    declared = (list(declared_sizes) if declared_sizes is not None
                else [len(chunk) for chunk in expected_chunks])
    if len(declared) != len(entries):
        raise CacheError("synthetic cache-v3 declared-size count mismatch")
    secondary_parts = []
    chunk_offset = 0
    for index, (actual, expected) in enumerate(zip(chunks, expected_chunks)):
        secondary_parts.append(stage._data_entry(
            chunk_offset=chunk_offset,
            uncompressed=expected,
            stored_chunk=actual,
            compressed=False,
            declared_size=declared[index]))
        chunk_offset += len(stage.CHUNK_MAGIC) + len(actual)
    setup_headers = (stage.VERSION_BANNER_STORED + stage._stored_block(primary)
                     + stage._stored_block(b"".join(secondary_parts)))
    header_offset = 0x100
    data_offset = header_offset + len(setup_headers)
    data = b"".join(stage.CHUNK_MAGIC + chunk for chunk in chunks)
    loader_offset = 0x80
    image = bytearray(max(header_offset, loader_offset + 40))
    image[0:2] = b"MZ"
    struct.pack_into("<III", image, 0x30, 0x6F6E6E49, loader_offset,
                     (~loader_offset) & 0xffffffff)
    loader_values = struct.pack(
        "<IIIIII", 0, 0, 0, 0, header_offset, data_offset)
    loader_body = stage.LOADER_MAGIC + loader_values
    import binascii
    loader_table = (loader_body
                    + struct.pack("<I", binascii.crc32(loader_body) & 0xffffffff))
    image[loader_offset:loader_offset + len(loader_table)] = loader_table
    image[header_offset:header_offset] = setup_headers + data
    return bytes(image)


def synthetic_cache_v3_fixtures() -> dict[str, bytes]:
    """One authored synthetic corpus consumed independently by Python/C++."""
    palette_a = [(index, 255 - index, index // 2, 255) for index in range(256)]
    palette_b = [(255 - index, index, (index * 3) & 0xff,
                  0 if index == 0 else 255) for index in range(256)]
    stone = {"name": "Stone", "format": 0, "masked": True,
             "anim_next": 6,
             "anim_next_identity": "texture.SurfaceFloor.Stone",
             "min_frame_rate": 2.0, "max_frame_rate": 7.0, "prime_count": 3,
             "mips": [(2, 2, b"\x00\x01\x02\x03"), (1, 1, b"\x02")]}
    other_stone = {"name": "Stone", "format": 0, "masked": False,
                   "mips": [(4, 1, b"\x03\x02\x01\x00")]}
    texture_source = _ue1_package([
        {"class": "Palette", "name": "PaletteStone",
         "group": 3, "payload": _ue1_palette_payload(palette_a)},
        {"class": "Palette", "name": "PaletteGlass",
         "group": 4, "payload": _ue1_palette_payload(palette_b)},
        {"class": "Package", "name": "SurfaceWall", "payload": b""},
        {"class": "Package", "name": "SurfaceFloor", "payload": b""},
        {"class": "Texture", "name": "Stone",
         "group": 3, "payload": _ue1_texture_payload(stone, 1)},
        {"class": "Texture", "name": "Stone",
         "group": 4, "payload": _ue1_texture_payload(other_stone, 2)},
    ], extra_names=("AnimNext", "MinFrameRate", "MaxFrameRate", "PrimeCount"))

    identical_loops = _synthetic_wav(
        bits=16, frames=4096,
        loops=((0, 4095, 0), (0, 4095, 0)))
    layered = _synthetic_wav(
        bits=16, frames=8,
        loops=((0, 3, 0), (4, 7, 2)))
    click = _synthetic_wav(bits=8, frames=3, channels=2)
    sound_source = _ue1_package([
        # UE1 class objects legitimately use class reference zero. They are
        # unrelated to the exact Sound candidates and must not poison the
        # entire package parse.
        {"class": "Class", "class_ref": 0, "name": "SyntheticSoundClass",
         "payload": b""},
        {"class": "Package", "name": "Ambient", "payload": b""},
        {"class": "Sound", "name": "Hum", "group": 2,
         "payload": _ue1_stream_payload("WAV", identical_loops)},
        {"class": "Sound", "name": "Layered",
         "payload": _ue1_stream_payload("WAV", layered)},
        {"class": "Sound", "name": "Click",
         "payload": _ue1_stream_payload("WAV", click)},
    ])

    objects = {
        "cache-v3-texture-masked.bin": encode_cache_v3_texture_object(
            stone, palette_a, fact_id="synthetic.texture.masked",
            source_path="Synthetic/texture.u",
            source_export="texture.SurfaceWall.Stone",
            source_bytes=texture_source),
        "cache-v3-texture-opaque.bin": encode_cache_v3_texture_object(
            other_stone, palette_b, fact_id="synthetic.texture.opaque",
            source_path="Synthetic/texture.u",
            source_export="texture.SurfaceFloor.Stone",
            source_bytes=texture_source),
        "cache-v3-sound-two-identical-loops.bin": encode_cache_v3_sound_object(
            identical_loops, fact_id="synthetic.sound.two-identical-loops",
            source_path="Synthetic/sound.u",
            source_export="sound.Ambient.Hum",
            source_bytes=sound_source),
        "cache-v3-sound-two-distinct-loops.bin": encode_cache_v3_sound_object(
            layered, fact_id="synthetic.sound.two-distinct-loops",
            source_path="Synthetic/sound.u", source_export="sound.Layered",
            source_bytes=sound_source),
        "cache-v3-sound-pcm8-stereo.bin": encode_cache_v3_sound_object(
            click, fact_id="synthetic.sound.pcm8-stereo",
            source_path="Synthetic/sound.u", source_export="sound.Click",
            source_bytes=sound_source),
    }
    mesh_source = _synthetic_mesh_source()
    mesh_package, plain_mesh, lod_mesh, mesh_temporary = \
        _decode_synthetic_meshes(mesh_source)
    try:
        objects.update({
            "cache-v3-mesh-plain.bin": encode_cache_v3_mesh_object(
                plain_mesh, mesh_package, fact_id="synthetic.mesh.plain",
                source_path="Synthetic/mesh.u", source_export="mesh.PlainMesh",
                source_bytes=mesh_source),
            "cache-v3-animation-plain.bin": encode_cache_v3_animation_object(
                plain_mesh, mesh_package, fact_id="synthetic.animation.plain",
                source_path="Synthetic/mesh.u", source_export="mesh.PlainMesh",
                source_bytes=mesh_source),
            "cache-v3-mesh-lod.bin": encode_cache_v3_mesh_object(
                lod_mesh, mesh_package, fact_id="synthetic.mesh.lod",
                source_path="Synthetic/mesh.u", source_export="mesh.LodHero",
                source_bytes=mesh_source),
            "cache-v3-animation-lod.bin": encode_cache_v3_animation_object(
                lod_mesh, mesh_package, fact_id="synthetic.animation.lod",
                source_path="Synthetic/mesh.u", source_export="mesh.LodHero",
                source_bytes=mesh_source),
        })
    finally:
        mesh_temporary.cleanup()
    objects["cache-v3-mesh-importer-input.bin"] = _synthetic_inno([
        ("Synthetic/mesh.u", mesh_source),
    ])
    mesh_records = (
        ("cache-v3-mesh-plain.bin", "cache/mesh/synthetic-plain",
         "wot-mesh-v3", "synthetic.mesh.plain"),
        ("cache-v3-animation-plain.bin", "cache/animation/synthetic-plain",
         "wot-animation-v3", "synthetic.animation.plain"),
        ("cache-v3-mesh-lod.bin", "cache/mesh/synthetic-lod",
         "wot-mesh-v3", "synthetic.mesh.lod"),
        ("cache-v3-animation-lod.bin", "cache/animation/synthetic-lod",
         "wot-animation-v3", "synthetic.animation.lod"),
    )
    objects["cache-v3-mesh-manifest.bin"] = cache_v3_manifest_bytes([
        {"logical_id": logical_id, "schema": schema,
         "bytes": objects[filename], "satisfies": [fact_id]}
        for filename, logical_id, schema, fact_id in mesh_records
    ])

    remaining_source = _synthetic_remaining_source()
    core_source = _synthetic_core_source()
    remaining_localization = (
        b"[General]\r\nGreeting=Hello \\n world\r\n"
        b"Quoted=\"A \"\"quoted\"\" value\"\r\n"
        b"[Menu]\r\nEntry=Entr\xe9e  \r\n")
    remaining_temporary = tempfile.TemporaryDirectory()
    try:
        remaining_path = Path(remaining_temporary.name) / "remaining.u"
        remaining_path.write_bytes(remaining_source)
        core_path = Path(remaining_temporary.name) / "Core.u"
        core_path.write_bytes(core_source)
        remaining_package = Package(remaining_path)
        core_package = Package(core_path)
        by_name = {export["name"]: export
                   for export in remaining_package.exports}
        font = decode_font(remaining_package, by_name["FontA"])
        fractal = decode_fractal(remaining_package, by_name["FireA"])
        objects.update({
            "cache-v3-font-pages.bin": encode_cache_v3_font_object(
                font, remaining_package, fact_id="synthetic.font.pages",
                source_path="Synthetic/remaining.u",
                source_export="remaining.FontA", source_bytes=remaining_source,
                atlas_logical_ids={
                    "remaining.Atlas": "cache/texture/synthetic-atlas"}),
            "cache-v3-procedural-fire.bin": encode_cache_v3_procedural_object(
                fractal, remaining_package, by_name["FireA"],
                fact_id="synthetic.procedural.fire",
                source_path="Synthetic/remaining.u",
                source_export="remaining.FireA", source_bytes=remaining_source),
            "cache-v3-registry-package.bin": encode_cache_v3_registry_object(
                remaining_package, fact_id="synthetic.registry.package",
                source_path="Synthetic/remaining.u", source_export="remaining",
                source_bytes=remaining_source),
            "cache-v3-registry-core.bin": encode_cache_v3_registry_object(
                core_package, fact_id="synthetic.registry.core",
                source_path="Synthetic/Core.u", source_export="Core",
                source_bytes=core_source),
            "cache-v3-class-defaults.bin": encode_cache_v3_class_defaults_object(
                remaining_package, fact_id="synthetic.class-default.package",
                source_path="Synthetic/remaining.u", source_export="remaining",
                source_bytes=remaining_source),
            "cache-v3-localization-table.bin": encode_cache_v3_localization_object(
                remaining_localization,
                fact_id="synthetic.localization.english",
                source_path="Synthetic/remaining.int",
                source_export="remaining.int"),
        })
    finally:
        remaining_temporary.cleanup()
    objects["cache-v3-remaining-importer-input.bin"] = _synthetic_inno([
        ("Synthetic/remaining.u", remaining_source),
        ("Synthetic/Core.u", core_source),
        ("Synthetic/remaining.int", remaining_localization),
    ])
    remaining_records = (
        ("cache-v3-font-pages.bin", "cache/font/synthetic-pages",
         "wot-font-v3", "synthetic.font.pages"),
        ("cache-v3-procedural-fire.bin", "cache/procedural/synthetic-fire",
         "wot-procedural-v3", "synthetic.procedural.fire"),
        ("cache-v3-registry-package.bin", "cache/registry/synthetic-package",
         "wot-registry-v3", "synthetic.registry.package"),
        ("cache-v3-registry-core.bin", "cache/registry/synthetic-core",
         "wot-registry-v3", "synthetic.registry.core"),
        ("cache-v3-localization-table.bin", "cache/localization/synthetic-en",
         "wot-localization-v3", "synthetic.localization.english"),
        ("cache-v3-class-defaults.bin", "cache/class-default/synthetic-package",
         "wot-class-default-v3", "synthetic.class-default.package"),
    )
    objects["cache-v3-remaining-manifest.bin"] = cache_v3_manifest_bytes([
        {"logical_id": logical_id, "schema": schema,
         "bytes": objects[filename], "satisfies": [fact_id]}
        for filename, logical_id, schema, fact_id in remaining_records
    ])
    for filename, mutation in {
        "cache-v3-font-invalid-metric.bin": "invalid-font-metric",
        "cache-v3-registry-case-collision.bin": "duplicate-case-name",
        "cache-v3-class-default-unknown-type.bin": "unknown-property-type",
        "cache-v3-class-default-unsafe-reference.bin": "unsafe-object-reference",
        "cache-v3-class-default-next-cycle.bin": "struct-next-cycle",
        "cache-v3-class-default-unresolvable-struct.bin": "unresolvable-struct",
        "cache-v3-class-default-fixed-array-self-cycle.bin":
            "fixed-array-self-cycle",
        "cache-v3-class-default-fixed-array-two-node-cycle.bin":
            "fixed-array-two-node-cycle",
        "cache-v3-class-default-fixed-array-wrapper-overflow.bin":
            "fixed-array-wrapper-overflow",
        "cache-v3-class-default-array-index-out-of-range.bin":
            "struct-array-index-out-of-range",
        "cache-v3-class-default-depth-9.bin": "struct-depth-9",
        "cache-v3-reference-collision.bin": "reference-collision",
    }.items():
        objects[filename] = _synthetic_inno([
            ("Synthetic/remaining.u", _synthetic_remaining_source(mutation=mutation)),
            ("Synthetic/Core.u", core_source),
            ("Synthetic/remaining.int", remaining_localization),
        ])
    objects["cache-v3-localization-invalid-encoding.bin"] = _synthetic_inno([
        ("Synthetic/remaining.u", remaining_source),
        ("Synthetic/remaining.int", b"[General]\r\nBad=\x81\r\n"),
    ])
    objects["cache-v3-localization-missing-fallback.bin"] = _synthetic_inno([
        ("Synthetic/remaining.u", remaining_source),
    ])
    mesh_rejections = {
        "cache-v3-mesh-triangle-out-of-range.bin": "triangle-index",
        "cache-v3-mesh-corrupt-frame-offset.bin": "lazy-end",
        "cache-v3-mesh-numeric-overflow.bin": "numeric-overflow",
        "cache-v3-mesh-decompression-limit.bin": "decompression-limit",
        "cache-v3-mesh-triangle-count-bomb.bin": "triangle-count-bomb",
        "cache-v3-mesh-lod-special-overflow.bin": "lod-special-overflow",
        "cache-v3-animation-sequence-out-of-range.bin": "sequence-range",
        "cache-v3-animation-sequence-overlap.bin": "sequence-overlap",
        "cache-v3-animation-notify-out-of-range.bin": "notify-time",
        "cache-v3-mesh-lod-material-out-of-range.bin": "lod-material",
        "cache-v3-mesh-lod-remap-out-of-range.bin": "lod-remap",
        "cache-v3-mesh-skeletal-class-rejected.bin": "skeletal-class",
    }
    for filename, mutation in mesh_rejections.items():
        objects[filename] = _synthetic_inno([
            ("Synthetic/mesh.u", _synthetic_mesh_source(mutation=mutation)),
        ])
    records = []
    fixture_records = (
        ("cache-v3-texture-masked.bin", "cache/texture/synthetic-masked",
         "wot-texture-v3", "synthetic.texture.masked"),
        ("cache-v3-texture-opaque.bin", "cache/texture/synthetic-opaque",
         "wot-texture-v3", "synthetic.texture.opaque"),
        ("cache-v3-sound-two-identical-loops.bin",
         "cache/sound/synthetic-two-identical-loops",
         "wot-sound-v3", "synthetic.sound.two-identical-loops"),
        ("cache-v3-sound-two-distinct-loops.bin",
         "cache/sound/synthetic-two-distinct-loops",
         "wot-sound-v3", "synthetic.sound.two-distinct-loops"),
        ("cache-v3-sound-pcm8-stereo.bin", "cache/sound/synthetic-pcm8-stereo",
         "wot-sound-v3", "synthetic.sound.pcm8-stereo"),
    )
    for filename, logical_id, schema, fact_id in fixture_records:
        records.append({"logical_id": logical_id, "schema": schema,
                        "bytes": objects[filename], "satisfies": [fact_id]})
    objects["cache-v3-manifest.bin"] = cache_v3_manifest_bytes(records)
    objects["cache-v3-importer-input.bin"] = _synthetic_inno([
        ("Synthetic/texture.u", texture_source),
        ("Synthetic/sound.u", sound_source),
    ])

    case_texture = {"name": "CaseStone", "format": 0, "masked": False,
                    "mips": [(1, 1, b"\x01")]}
    case_unique_source = _ue1_package([
        {"class": "Palette", "name": "CasePalette",
         "payload": _ue1_palette_payload(palette_a)},
        {"class": "Texture", "name": "CaseStone",
         "payload": _ue1_texture_payload(case_texture, 1)},
    ])
    objects["cache-v3-export-case-unique.bin"] = _synthetic_inno([
        ("Synthetic/CaseUnique.u", case_unique_source),
    ])

    case_collision_source = _ue1_package([
        {"class": "Texture", "name": "Stone", "payload": b""},
        {"class": "Texture", "name": "stone", "payload": b""},
    ])
    objects["cache-v3-export-case-collision.bin"] = _synthetic_inno([
        ("Synthetic/CaseCollision.u", case_collision_source),
    ])

    # If serialized name components could contain dots, these two exports
    # would both flatten to DottedAlias.Surface.Stone.
    dotted_alias_source = _ue1_package([
        {"class": "Package", "name": "Surface", "payload": b""},
        {"class": "Texture", "name": "Surface.Stone", "payload": b""},
        {"class": "Texture", "name": "Stone", "group": 1, "payload": b""},
    ])
    objects["cache-v3-export-dotted-alias.bin"] = _synthetic_inno([
        ("Synthetic/DottedAlias.u", dotted_alias_source),
    ])

    empty_name_source = _ue1_package([
        {"class": "Texture", "name": "", "payload": b""},
    ])
    objects["cache-v3-export-empty-name.bin"] = _synthetic_inno([
        ("Synthetic/EmptyName.u", empty_name_source),
    ])

    def texture_rejection(name: str, texture: dict,
                          colors: Sequence[Sequence[int]] = palette_a,
                          **payload_options: object) -> bytes:
        del name
        source = _ue1_package([
            {"class": "Palette", "name": "PaletteBad",
             "payload": _ue1_palette_payload(colors)},
            {"class": "Texture", "name": "Bad",
             "payload": _ue1_texture_payload(
                 texture, 1, **payload_options)},
        ], extra_names=("AnimNext", "MinFrameRate", "MaxFrameRate", "PrimeCount"))
        return _synthetic_inno([("Synthetic/reject-texture.u", source)])

    invalid_dimension = {"name": "Bad", "format": 0, "masked": False,
                         "mips": [(MAX_TEXTURE_DIMENSION + 1, 1, b"\0")]}
    zero_dimension = {"name": "Bad", "format": 0, "masked": False,
                      "mips": [(0, 1, b"")]}
    invalid_format = {"name": "Bad", "format": 1, "masked": False,
                      "mips": [(1, 1, b"\0")]}
    objects["cache-v3-texture-invalid-dimension.bin"] = texture_rejection(
        "dimension", invalid_dimension, mip_bits=((14, 0),))
    objects["cache-v3-texture-zero-dimension.bin"] = texture_rejection(
        "zero-dimension", zero_dimension, mip_bits=((0, 0),))
    objects["cache-v3-texture-invalid-format.bin"] = texture_rejection(
        "format", invalid_format)
    objects["cache-v3-texture-invalid-palette.bin"] = texture_rejection(
        "palette", {"name": "Bad", "format": 0, "masked": False,
                    "mips": [(1, 1, b"\0")]}, palette_a[:-1])
    objects["cache-v3-texture-invalid-bits.bin"] = texture_rejection(
        "bits", {"name": "Bad", "format": 0, "masked": False,
                 "mips": [(2, 2, b"\0\1\2\3")]}, mip_bits=((0, 0),))
    objects["cache-v3-texture-invalid-successor.bin"] = texture_rejection(
        "successor", {"name": "Bad", "format": 0, "masked": False,
                      "mips": [(4, 4, bytes(16)), (1, 1, b"\0")]})
    objects["cache-v3-texture-invalid-lazy-end.bin"] = texture_rejection(
        "lazy-end", {"name": "Bad", "format": 0, "masked": False,
                     "mips": [(1, 1, b"\0")]}, lazy_end_adjust=1)
    for property_name in ("AnimNext", "MinFrameRate", "MaxFrameRate", "PrimeCount"):
        fixture_name = property_name.replace("FrameRate", "-frame-rate").replace(
            "AnimNext", "anim-next").replace("PrimeCount", "prime-count").lower()
        objects[f"cache-v3-texture-bool-{fixture_name}.bin"] = texture_rejection(
            property_name, {"name": "Bad", "format": 0, "masked": False,
                            "mips": [(1, 1, b"\0")]},
            hostile_bool_animation=property_name)

    def stream_rejection(filename: str, class_name: str, export_name: str,
                         declared_format: str, stream: bytes) -> None:
        source = _ue1_package([{
            "class": class_name, "name": export_name,
            "payload": _ue1_stream_payload(declared_format, stream),
        }])
        objects[filename] = _synthetic_inno(
            [("Synthetic/reject-stream.u", source)])

    stream_rejection("cache-v3-sound-invalid-rate.bin", "Sound", "Bad",
                     "WAV", _synthetic_wav(bits=16, frames=1, sample_rate=0))
    stream_rejection("cache-v3-sound-invalid-channels.bin", "Sound", "Bad",
                     "WAV", _synthetic_wav(bits=16, frames=1, channels=9))
    stream_rejection("cache-v3-sound-invalid-block-align.bin", "Sound", "Bad",
                     "WAV", _synthetic_wav(bits=16, frames=1, block_align=1))
    stream_rejection("cache-v3-sound-invalid-byte-rate.bin", "Sound", "Bad",
                     "WAV", _synthetic_wav(bits=16, frames=1, byte_rate=1))
    stream_rejection("cache-v3-sound-invalid-bit-depth.bin", "Sound", "Bad",
                     "WAV", _synthetic_wav(bits=24, frames=1))
    stream_rejection("cache-v3-sound-invalid-format-tag.bin", "Sound", "Bad",
                     "WAV", _synthetic_wav(bits=16, frames=1, format_tag=3))
    stream_rejection("cache-v3-sound-invalid-loop.bin", "Sound", "Bad",
                     "WAV", _synthetic_wav(
                         bits=16, frames=4, loops=((1, 9, 0),)))
    stream_rejection("cache-v3-sound-over-loop-cap.bin", "Sound", "Bad",
                     "WAV", _synthetic_wav(
                         bits=16, frames=4,
                         loops=((0, 3, 0),) * (MAX_WAV_SAMPLER_LOOPS + 1)))

    music_class_source = _ue1_package([{
        "class": "Music", "name": "Bad",
        "payload": _ue1_stream_payload(
            "WAV", _synthetic_wav(bits=16, frames=1)),
    }])
    objects["cache-v3-sound-music-class-rejected.bin"] = _synthetic_inno(
        [("Synthetic/reject-stream.u", music_class_source)])

    bomb_source = _ue1_package([{
        "class": "Sound", "name": "Bad",
        "payload": _ue1_stream_payload("WAV", _synthetic_wav(bits=16, frames=1)),
    }])
    objects["cache-v3-sound-bomb-size.bin"] = _synthetic_inno(
        [("Synthetic/reject-stream.u", bomb_source)],
        declared_sizes=[MAX_BLOB_SIZE + 1])
    objects["cache-v3-sound-hash-mismatch.bin"] = _synthetic_inno(
        [("Synthetic/reject-stream.u", bomb_source)], hash_mismatch_index=0)
    return objects


def write_synthetic_cache_v3_fixtures(output_dir: Path) -> list[Path]:
    fixtures = synthetic_cache_v3_fixtures()
    output_dir.mkdir(parents=True, exist_ok=True)
    for stale in output_dir.glob("cache-v3-*.bin"):
        if stale.name not in fixtures:
            stale.unlink()
    written = []
    for name, raw in sorted(fixtures.items()):
        path = output_dir / name
        _atomic_write(path, raw)
        written.append(path)
    map_set = synthetic_map_set_v3()
    validate_map_set_v3(map_set)
    map_set_path = output_dir / "map-set-v3-synthetic.json"
    _atomic_write(map_set_path, _canonical_json_bytes(map_set))
    written.append(map_set_path)
    return written


def _texture_mip_metadata(texture: dict) -> list[dict]:
    return [{
        "width": width,
        "height": height,
        "stored_byte_length": len(indices),
        "expanded_byte_length": width * height * 4,
    } for width, height, indices in texture["mips"]]


def _texture_entries(pkg: Package, source: str) -> tuple[dict, list[dict]]:
    palettes = {}
    for export_index, export in enumerate(pkg.exports, 1):
        if pkg.objname(export["cls"]) == "Palette":
            try:
                palettes[export_index] = decode_palette(pkg, export)
            except (IndexError, KeyError, struct.error, ValueError):
                # A referencing texture receives the deterministic missing-
                # palette skip below. Unreferenced broken palettes are not
                # payload entries of their own.
                pass

    entries: dict[str, tuple[dict, bytes]] = {}
    occurrences: dict[str, int] = {}
    skipped = []
    for export in pkg.exports:
        if pkg.objname(export["cls"]) != "Texture":
            continue
        name = export["name"]
        key = _entry_key("textures", pkg.name, name)
        occurrences[key] = occurrences.get(key, 0) + 1
        try:
            texture = decode_texture(pkg, export)
            if texture["format"] != 0:
                raise CacheError(f"unsupported texture format {texture['format']}")
            palette = palettes.get(texture["palette"])
            if palette is None:
                raise CacheError(f"missing palette ref {texture['palette']}")
            payload = encode_texture_payload(texture, palette)
        except (CacheError, IndexError, KeyError, struct.error, ValueError) as exc:
            skipped.append({
                "type": "textures", "source": source, "package": pkg.name,
                "name": name, "reason": str(exc),
            })
            continue
        entry = {
            "type": "textures",
            "package": pkg.name,
            "name": name,
            "encoding": TEXTURE_ENCODING,
            "width": texture["mips"][0][0],
            "height": texture["mips"][0][1],
            "mip_count": len(texture["mips"]),
            "mips": _texture_mip_metadata(texture),
            "palette_colors": len(palette),
            "masked": bool(texture["masked"]),
        }
        entries[key] = (entry, payload)  # later duplicate wins, like UCC export

    for key, count in occurrences.items():
        if count > 1 and key in entries:
            entries[key][0]["export_occurrences"] = count
    return entries, skipped


def _sound_entries(pkg: Package, source: str) -> tuple[dict, list[dict]]:
    del source  # sounds have no deliberately skippable encoding in version 1
    entries: dict[str, tuple[dict, bytes]] = {}
    occurrences: dict[str, int] = {}
    for export in pkg.exports:
        if pkg.objname(export["cls"]) != "Sound":
            continue
        name = export["name"]
        key = _entry_key("sounds", pkg.name, name)
        occurrences[key] = occurrences.get(key, 0) + 1
        format_name, data = decode_sound(pkg, export)
        if format_name.casefold() != "wav":
            raise CacheError(
                f"{pkg.name}.{name}: unsupported sound format {format_name!r}")
        payload = bytes(data)
        wav_metadata = parse_wav_metadata(payload, f"{pkg.name}.{name}")
        entries[key] = ({
            "type": "sounds",
            "package": pkg.name,
            "name": name,
            "encoding": SOUND_ENCODING,
            **wav_metadata,
        }, payload)  # later duplicate wins, matching import_sounds/export_sounds

    for key, count in occurrences.items():
        if count > 1:
            entries[key][0]["export_occurrences"] = count
    return entries, []


def _mesh_entries(pkg: Package, source: str) -> tuple[dict, list[dict]]:
    del source
    entries: dict[str, tuple[dict, bytes]] = {}
    occurrences: dict[str, int] = {}
    for export in pkg.exports:
        class_name = pkg.objname(export["cls"])
        if class_name not in ("Mesh", "LodMesh"):
            continue
        name = export["name"]
        key = _entry_key("meshes", pkg.name, name)
        occurrences[key] = occurrences.get(key, 0) + 1
        mesh = decode_mesh(pkg, export)
        payload = encode_mesh_payload(mesh)
        entries[key] = ({
            "type": "meshes",
            "package": pkg.name,
            "name": name,
            "encoding": MESH_ENCODING,
            "class_name": mesh["class_name"],
            "frame_verts": mesh["frame_verts"],
            "anim_frames": mesh["anim_frames"],
            "packed_vertex_count": len(mesh["packed_vertices"]),
            "frame_byte_length": mesh["frame_verts"] * 4,
        }, payload)

    for key, count in occurrences.items():
        if count > 1:
            entries[key][0]["export_occurrences"] = count
    return entries, []


def _provenance_file(conversion_dir: Path, path: Path, label: str) -> dict:
    try:
        relative = path.resolve().relative_to(conversion_dir.resolve()).as_posix()
    except ValueError as exc:
        raise CacheError(f"{label} escaped BSP conversion directory: {path}") from exc
    _validate_source_path(relative, label)
    if not path.is_file():
        raise CacheError(f"missing {label}: {path}")
    return {"path": relative, "size": path.stat().st_size, "sha256": sha256_file(path)}


def _bsp_entries(retail_root: Path, map_path: Path, source: str,
                 conversion_dir: Path | None) -> tuple[dict, list[dict]]:
    """Build split-aware BSP payload parts from canonical bsp2obj outputs."""
    map_name = map_path.stem
    if map_name not in MP_MAPS:
        raise CacheError(f"BSP payload map is outside the multiplayer scope: {map_name}")
    if conversion_dir is None:
        raise CacheError(
            "--bsp-conv-dir is required for the bsps type (run bsp2obj/lightmap stages first)")
    conversion_dir = Path(conversion_dir).resolve()
    if not conversion_dir.is_dir():
        raise CacheError(f"missing BSP conversion directory: {conversion_dir}")
    atlas = conversion_dir / f"{map_name}_lmatlas.json"
    sky_split = conversion_dir / f"{map_name}.t3d.json"
    converter = CONVERT_DIR / "bsp2obj.py"
    payload_encoder = CONVERT_DIR / "bsp_payload.py"
    shared_provenance = {
        "schema": "bsp-payload-provenance-v1",
        "map": {
            "path": source,
            "size": map_path.stat().st_size,
            "sha256": _sha256_file(map_path),
        },
        "texture_dimensions_sha256": texture_dimensions_sha256(conversion_dir),
        "lightmap_atlas": _provenance_file(conversion_dir, atlas, "lightmap atlas"),
        "sky_split": _provenance_file(conversion_dir, sky_split, "sky-split T3D JSON"),
        "converter": {
            "path": converter.relative_to(REPO).as_posix(),
            "size": converter.stat().st_size,
            "sha256": _sha256_file(converter),
        },
        "payload_encoder": {
            "path": payload_encoder.relative_to(REPO).as_posix(),
            "size": payload_encoder.stat().st_size,
            "sha256": _sha256_file(payload_encoder),
        },
    }
    if len(shared_provenance) > MAX_BSP_PROVENANCE_INPUTS:
        raise CacheError("BSP provenance exceeds compiled input cap")

    entries = {}
    with tempfile.TemporaryDirectory(prefix=f"wot-bsp-{map_name}-") as temporary:
        try:
            parts = split_bsp_glb_parts(conversion_dir, map_name, Path(temporary))
        except (OSError, ValueError) as exc:
            raise CacheError(f"{map_name}: cannot split BSP payloads: {exc}") from exc
        for part in parts:
            role = part["role"]
            glb = Path(part["path"])
            try:
                geometry = decode_bsp_glb(glb, role)
                payload = encode_bsp_payload(geometry)
                parsed = parse_bsp_payload(payload)
            except (OSError, ValueError) as exc:
                raise CacheError(f"{glb}: cannot build BSP payload: {exc}") from exc
            # The binary contract is float32. Its parsed values are authoritative
            # for manifest bounds/metadata after the expected float32 narrowing.
            geometry = parsed
            name = part["asset_name"]
            key = _entry_key("bsps", map_name, name)
            provenance = {
                **shared_provenance,
                # Provenance stays schema-v1: the canonical full GLB is the
                # authenticated source from which this deterministic part came.
                "source_glb": _provenance_file(
                    conversion_dir, Path(part["source_path"]), f"{role} BSP GLB"),
            }
            entries[key] = ({
                "type": "bsps",
                "package": map_name,
                "name": name,
                "encoding": BSP_ENCODING,
                **bsp_manifest_metadata(geometry, provenance),
            }, payload)
    return entries, []


def _package_input_sha256(kind: str, source_record: dict,
                          bsp_conversion_dir: Path | None) -> str:
    """Fingerprint every decoder input used to bless a resumable marker."""
    if kind != "bsps":
        return source_record["sha256"]
    if bsp_conversion_dir is None:
        raise CacheError("--bsp-conv-dir is required for the bsps type")
    conversion_dir = Path(bsp_conversion_dir).resolve()
    map_name = Path(source_record["path"]).stem
    paths = [
        CONVERT_DIR / "bsp2obj.py",
        CONVERT_DIR / "bsp_payload.py",
        IMPORT_DIR / "world_fractal_split.py",
        conversion_dir / f"{map_name}_lmatlas.json",
        conversion_dir / f"{map_name}.t3d.json",
        conversion_dir / f"{map_name}.bsp.json",
        conversion_dir / f"{map_name}.glb",
    ]
    portal_glb = conversion_dir / f"{map_name}_portals.glb"
    if portal_glb.is_file():
        paths.append(portal_glb)
    flipbook_manifests = sorted(
        (conversion_dir / "flipbooks").glob("*/_flipbooks.json"),
        key=lambda path: (str(path).casefold(), str(path)))
    if not flipbook_manifests:
        raise CacheError(
            f"missing BSP build input: no runtime flipbook manifests under "
            f"{conversion_dir / 'flipbooks'}")
    paths.extend(flipbook_manifests)
    digest = hashlib.sha256()
    digest.update(b"WOT-BSP-BUILD-INPUTS-V1\0")
    for value in (source_record["sha256"], texture_dimensions_sha256(conversion_dir)):
        digest.update(value.encode("ascii"))
    for path in paths:
        if not path.is_file():
            raise CacheError(f"missing BSP build input: {path}")
        try:
            identity = "conv/" + path.resolve().relative_to(
                conversion_dir).as_posix()
        except ValueError:
            identity = "repo/" + path.resolve().relative_to(REPO).as_posix()
        digest.update(identity.encode("utf-8"))
        digest.update(path.stat().st_size.to_bytes(8, "little"))
        digest.update(_sha256_file(path).encode("ascii"))
    return digest.hexdigest()


def decode_package(kind: str, path: Path, source: str, *,
                   retail_root: Path | None = None,
                   bsp_conversion_dir: Path | None = None) -> tuple[dict, list[dict]]:
    if kind == "bsps":
        if retail_root is None:
            raise CacheError("BSP decode requires the retail root")
        return _bsp_entries(retail_root, path, source, bsp_conversion_dir)
    pkg = Package(path)
    if kind == "textures":
        return _texture_entries(pkg, source)
    if kind == "sounds":
        return _sound_entries(pkg, source)
    if kind == "meshes":
        return _mesh_entries(pkg, source)
    raise CacheError(f"unknown payload type {kind!r}")


_BASE_ENTRY_FIELDS = {
    "type", "package", "name", "encoding", "blob", "size", "sha256",
}
_TYPE_ENTRY_FIELDS = {
    "textures": {
        "width", "height", "mip_count", "mips", "palette_colors", "masked",
    },
    "sounds": {
        "pcm_format_tag", "channels", "sample_rate", "bits_per_sample",
        "frame_count", "data_byte_length", "duration_seconds",
    },
    "meshes": {
        "class_name", "frame_verts", "anim_frames", "packed_vertex_count",
        "frame_byte_length",
    },
    "bsps": {
        "role", "vertex_count", "index_count", "triangle_count",
        "section_count", "uv_channels", "bounds_min", "bounds_max",
        "tangent_algorithm", "sections", "provenance",
    },
}


def _validate_finite_vector(value: object, count: int, label: str) -> None:
    if (not isinstance(value, list) or len(value) != count
            or any(isinstance(item, bool) or not isinstance(item, (int, float))
                   or not math.isfinite(item) for item in value)):
        raise CacheError(f"invalid {label}")


def _validate_provenance_file(value: object, label: str) -> None:
    if not isinstance(value, dict) or set(value) != {"path", "size", "sha256"}:
        raise CacheError(f"invalid BSP {label} provenance")
    _validate_source_path(value.get("path"), f"BSP {label} path")
    if (not _is_int(value.get("size")) or value["size"] < 0
            or not _is_sha256(value.get("sha256"))):
        raise CacheError(f"invalid BSP {label} provenance values")


def _validate_bsp_provenance(value: object, package: str) -> None:
    fields = {
        "schema", "map", "texture_dimensions_sha256", "lightmap_atlas",
        "sky_split", "converter", "payload_encoder", "source_glb",
    }
    if not isinstance(value, dict) or set(value) != fields \
            or len(value) > MAX_BSP_PROVENANCE_INPUTS \
            or value.get("schema") != "bsp-payload-provenance-v1" \
            or not _is_sha256(value.get("texture_dimensions_sha256")):
        raise CacheError("invalid BSP provenance contract")
    for field in ("map", "lightmap_atlas", "sky_split", "converter",
                  "payload_encoder", "source_glb"):
        _validate_provenance_file(value[field], field)
    if PurePosixPath(value["map"]["path"]).stem != package:
        raise CacheError("BSP provenance map disagrees with entry package")


def _parse_texture_payload(payload: bytes, label: str) -> dict:
    header_size = struct.calcsize("<8sHHII")
    if len(payload) < header_size:
        raise CacheError(f"{label}: truncated texture header")
    magic, version, flags, palette_colors, mip_count = struct.unpack_from(
        "<8sHHII", payload)
    if magic != TEXTURE_MAGIC + b"\0" or version != 1 or flags & ~1:
        raise CacheError(f"{label}: invalid texture blob header")
    if palette_colors != 256 or not 1 <= mip_count <= MAX_TEXTURE_MIPS:
        raise CacheError(f"{label}: invalid texture palette/mip counts")
    offset = header_size + palette_colors * 4
    if offset > len(payload):
        raise CacheError(f"{label}: truncated texture palette")
    mips = []
    for mip_index in range(mip_count):
        if offset + 12 > len(payload):
            raise CacheError(f"{label}: truncated texture mip {mip_index} header")
        width, height, stored = struct.unpack_from("<III", payload, offset)
        offset += 12
        if (not 1 <= width <= MAX_TEXTURE_DIMENSION
                or not 1 <= height <= MAX_TEXTURE_DIMENSION
                or stored != width * height):
            raise CacheError(f"{label}: invalid texture mip {mip_index}")
        if offset + stored > len(payload):
            raise CacheError(f"{label}: truncated texture mip {mip_index} data")
        mips.append({
            "width": width,
            "height": height,
            "stored_byte_length": stored,
            "expanded_byte_length": stored * 4,
        })
        offset += stored
    if offset != len(payload):
        raise CacheError(f"{label}: trailing texture blob bytes")
    return {
        "width": mips[0]["width"],
        "height": mips[0]["height"],
        "mip_count": mip_count,
        "mips": mips,
        "palette_colors": palette_colors,
        "masked": bool(flags & 1),
    }


def _parse_mesh_payload(payload: bytes, label: str) -> dict:
    header_size = struct.calcsize("<8sHHIII")
    if len(payload) < header_size:
        raise CacheError(f"{label}: truncated mesh header")
    magic, version, flags, frame_verts, anim_frames, packed_count = (
        struct.unpack_from("<8sHHIII", payload))
    if magic != MESH_MAGIC or version != 1 or flags != 0:
        raise CacheError(f"{label}: invalid mesh blob header")
    if frame_verts < 1 or anim_frames < 1:
        raise CacheError(f"{label}: invalid mesh frame counts")
    if packed_count != frame_verts * anim_frames:
        raise CacheError(f"{label}: inconsistent mesh vertex counts")
    expected_size = header_size + packed_count * 4
    if expected_size != len(payload):
        raise CacheError(
            f"{label}: mesh blob length {len(payload)} != {expected_size}")
    return {
        "frame_verts": frame_verts,
        "anim_frames": anim_frames,
        "packed_vertex_count": packed_count,
        "frame_byte_length": frame_verts * 4,
    }


def _entry_expanded_byte_length(entry: dict) -> int:
    if entry["type"] == "textures":
        return sum(mip["expanded_byte_length"] for mip in entry["mips"])
    if entry["type"] == "sounds":
        return entry["data_byte_length"]
    if entry["type"] == "meshes":
        return entry["frame_byte_length"] * entry["anim_frames"]
    return entry["vertex_count"] * 40 + entry["index_count"] * 4


def _validate_entry_record(key: object, entry: object, *,
                           expected_kind: str | None = None,
                           expected_package: str | None = None,
                           payload: bytes | None = None) -> None:
    """Single version-2 entry grammar for fresh, resumed, and loaded data."""
    if not isinstance(key, str) or not isinstance(entry, dict):
        raise CacheError("cache entries must map strings to objects")
    kind = entry.get("type")
    if kind not in TYPE_ORDER or (expected_kind is not None and kind != expected_kind):
        raise CacheError(f"entry {key!r} has an invalid type")
    package = entry.get("package")
    name = entry.get("name")
    if key != _entry_key(kind, package, name):
        raise CacheError(f"entry {key!r} disagrees with its package/name")
    if expected_package is not None and package != expected_package:
        raise CacheError(
            f"entry {key!r} package {package!r} != source {expected_package!r}")
    if entry.get("encoding") != TYPE_ENCODINGS[kind]:
        raise CacheError(f"entry {key!r} has an unsupported encoding")

    allowed_fields = _BASE_ENTRY_FIELDS | _TYPE_ENTRY_FIELDS[kind]
    if "export_occurrences" in entry:
        allowed_fields = allowed_fields | {"export_occurrences"}
        if (not _is_int(entry["export_occurrences"])
                or entry["export_occurrences"] < 2):
            raise CacheError(f"entry {key!r} has invalid export occurrences")
    if set(entry) != allowed_fields:
        raise CacheError(f"entry {key!r} has missing or unknown metadata fields")

    size = entry.get("size")
    sha256 = entry.get("sha256")
    if not _is_int(size) or not 0 < size <= MAX_BLOB_SIZE:
        raise CacheError(f"entry {key!r} has invalid blob size")
    expected_blob = _blob_relative_path(sha256)
    if entry.get("blob") != expected_blob:
        raise CacheError(f"entry {key!r} has an invalid blob path")

    if kind == "textures":
        mips = entry.get("mips")
        mip_count = entry.get("mip_count")
        if (not isinstance(mips, list) or not _is_int(mip_count)
                or not 1 <= mip_count <= MAX_TEXTURE_MIPS
                or len(mips) != mip_count
                or entry.get("palette_colors") != 256
                or type(entry.get("masked")) is not bool):
            raise CacheError(f"entry {key!r} has invalid texture metadata")
        for mip_index, mip in enumerate(mips):
            if not isinstance(mip, dict) or set(mip) != {
                    "width", "height", "stored_byte_length",
                    "expanded_byte_length"}:
                raise CacheError(f"entry {key!r} has invalid mip {mip_index}")
            width = mip.get("width")
            height = mip.get("height")
            stored = mip.get("stored_byte_length")
            expanded = mip.get("expanded_byte_length")
            if (not _is_int(width) or not _is_int(height)
                    or not 1 <= width <= MAX_TEXTURE_DIMENSION
                    or not 1 <= height <= MAX_TEXTURE_DIMENSION
                    or not _is_int(stored) or stored != width * height
                    or not _is_int(expanded) or expanded != stored * 4):
                raise CacheError(f"entry {key!r} has invalid mip {mip_index}")
        if (entry.get("width") != mips[0]["width"]
                or entry.get("height") != mips[0]["height"]):
            raise CacheError(f"entry {key!r} base dimensions disagree with mip 0")
    elif kind == "sounds":
        int_fields = (
            "pcm_format_tag", "channels", "sample_rate", "bits_per_sample",
            "frame_count", "data_byte_length",
        )
        if any(not _is_int(entry.get(field)) for field in int_fields):
            raise CacheError(f"entry {key!r} has invalid PCM metadata types")
        channels = entry["channels"]
        sample_rate = entry["sample_rate"]
        bits = entry["bits_per_sample"]
        frame_count = entry["frame_count"]
        data_length = entry["data_byte_length"]
        duration = entry.get("duration_seconds")
        if (entry["pcm_format_tag"] != 1 or not 1 <= channels <= 8
                or not 1 <= sample_rate <= 384_000 or bits not in (8, 16)
                or frame_count < 0 or data_length < 0
                or data_length != frame_count * channels * (bits // 8)
                or isinstance(duration, bool)
                or not isinstance(duration, (int, float))
                or duration != frame_count / sample_rate):
            raise CacheError(f"entry {key!r} has inconsistent PCM metadata")
    elif kind == "meshes":
        int_fields = (
            "frame_verts", "anim_frames", "packed_vertex_count",
            "frame_byte_length",
        )
        if any(not _is_int(entry.get(field)) for field in int_fields):
            raise CacheError(f"entry {key!r} has invalid mesh metadata types")
        if entry.get("class_name") not in ("Mesh", "LodMesh"):
            raise CacheError(f"entry {key!r} has invalid mesh class")
        frame_verts = entry["frame_verts"]
        anim_frames = entry["anim_frames"]
        if (frame_verts < 1 or anim_frames < 1
                or entry["packed_vertex_count"] != frame_verts * anim_frames
                or entry["frame_byte_length"] != frame_verts * 4):
            raise CacheError(f"entry {key!r} has inconsistent mesh metadata")
    else:
        int_fields = (
            "vertex_count", "index_count", "triangle_count", "section_count",
            "uv_channels",
        )
        if any(not _is_int(entry.get(field)) for field in int_fields):
            raise CacheError(f"entry {key!r} has invalid BSP count types")
        role = entry.get("role")
        vertices = entry["vertex_count"]
        indices = entry["index_count"]
        sections = entry.get("sections")
        if (role not in ("main", "portal")
                or not 1 <= vertices <= MAX_BSP_VERTICES
                or not 3 <= indices <= MAX_BSP_INDICES or indices % 3
                or entry["triangle_count"] != indices // 3
                or not 1 <= entry["section_count"] <= MAX_BSP_SECTIONS
                or entry["uv_channels"] != 2
                or entry.get("tangent_algorithm") != BSP_TANGENT_ALGORITHM
                or not isinstance(sections, list)
                or len(sections) != entry["section_count"]):
            raise CacheError(f"entry {key!r} has inconsistent BSP metadata")
        _validate_finite_vector(entry.get("bounds_min"), 3, f"{key} bounds_min")
        _validate_finite_vector(entry.get("bounds_max"), 3, f"{key} bounds_max")
        if any(abs(value) > MAX_BSP_COORDINATE_ABS
               for value in entry["bounds_min"] + entry["bounds_max"]):
            raise CacheError(f"entry {key!r} exceeds the BSP coordinate cap")
        if any(minimum > maximum for minimum, maximum in zip(
                entry["bounds_min"], entry["bounds_max"])):
            raise CacheError(f"entry {key!r} has reversed BSP bounds")
        expected_first = 0
        for section_index, section in enumerate(sections):
            fields = {"material_slot", "first_index", "index_count", "triangle_count",
                      "min_vertex", "max_vertex", "collision_enabled"}
            if not isinstance(section, dict) or set(section) != fields:
                raise CacheError(f"entry {key!r} has invalid BSP section fields")
            slot = section.get("material_slot")
            first = section.get("first_index")
            count = section.get("index_count")
            minimum = section.get("min_vertex")
            maximum = section.get("max_vertex")
            if (not isinstance(slot, str) or not slot
                    or len(slot.encode("utf-8")) > MAX_BSP_SLOT_NAME_BYTES
                    or any(char in slot for char in "\0\r\n")
                    or any(ord(char) < 32 or ord(char) == 127 for char in slot)
                    or not _is_int(first) or first != expected_first
                    or not _is_int(count) or count <= 0 or count % 3
                    or first + count > indices
                    or section.get("triangle_count") != count // 3
                    or not _is_int(minimum) or not _is_int(maximum)
                    or not 0 <= minimum <= maximum < vertices
                    or type(section.get("collision_enabled")) is not bool
                    or section["collision_enabled"] != (role == "main")):
                raise CacheError(f"entry {key!r} has invalid BSP section {section_index}")
            expected_first += count
        if expected_first != indices:
            raise CacheError(f"entry {key!r} BSP sections do not cover indices")
        _validate_bsp_provenance(entry.get("provenance"), package)

    if payload is not None:
        payload = bytes(payload)
        if len(payload) != size or _sha256_bytes(payload) != sha256:
            raise CacheError(f"entry {key!r} payload size/hash mismatch")
        if kind == "textures":
            parsed = _parse_texture_payload(payload, key)
            expected = {field: entry[field] for field in _TYPE_ENTRY_FIELDS[kind]}
            if parsed != expected:
                raise CacheError(f"entry {key!r} metadata disagrees with texture blob")
        elif kind == "sounds":
            parsed = parse_wav_metadata(payload, key)
            expected = {field: entry[field] for field in _TYPE_ENTRY_FIELDS[kind]}
            if parsed != expected:
                raise CacheError(f"entry {key!r} metadata disagrees with WAV blob")
        elif kind == "meshes":
            parsed = _parse_mesh_payload(payload, key)
            expected = {
                field: entry[field] for field in (
                    "frame_verts", "anim_frames", "packed_vertex_count",
                    "frame_byte_length")
            }
            if parsed != expected:
                raise CacheError(f"entry {key!r} metadata disagrees with mesh blob")
        else:
            try:
                parsed = parse_bsp_payload(payload)
            except ValueError as exc:
                raise CacheError(f"entry {key!r} has invalid BSP blob: {exc}") from exc
            fields = _TYPE_ENTRY_FIELDS[kind] - {"provenance"}
            expected = {field: entry[field] for field in fields}
            actual = {field: parsed[field] for field in fields}
            if actual != expected:
                raise CacheError(f"entry {key!r} metadata disagrees with BSP blob")


def _validate_global_limits(entries: dict) -> int:
    if len(entries) > MAX_ENTRIES:
        raise CacheError(
            f"cache has {len(entries)} entries; version-2 cap is {MAX_ENTRIES}")
    total_expanded = 0
    for key, entry in entries.items():
        _validate_entry_record(key, entry)
        expanded = _entry_expanded_byte_length(entry)
        if expanded > MAX_TOTAL_EXPANDED_BYTES - total_expanded:
            raise CacheError(
                "cache exceeds version-2 total expanded-byte cap "
                f"{MAX_TOTAL_EXPANDED_BYTES}")
        total_expanded += expanded
    return total_expanded


def _validate_source_record(source: object, *, expected_kind: str | None = None) -> None:
    if not isinstance(source, dict) or set(source) != {"type", "path", "size", "sha256"}:
        raise CacheError("invalid source record")
    kind = source.get("type")
    if kind not in TYPE_ORDER or (expected_kind is not None and kind != expected_kind):
        raise CacheError("invalid source type")
    _validate_source_path(source.get("path"), "source path")
    if not _is_int(source.get("size")) or source["size"] < 0:
        raise CacheError("invalid source size")
    if not _is_sha256(source.get("sha256")):
        raise CacheError("invalid source SHA-256")


def _validate_skipped_record(item: object, *, expected_kind: str | None = None,
                             expected_package: str | None = None) -> None:
    fields = {"type", "source", "package", "name", "reason"}
    if not isinstance(item, dict) or set(item) != fields:
        raise CacheError("invalid skipped-entry record")
    kind = item.get("type")
    if kind not in TYPE_ORDER or (expected_kind is not None and kind != expected_kind):
        raise CacheError("invalid skipped-entry type")
    _validate_source_path(item.get("source"), "skipped source path")
    _validate_component(item.get("package"), "skipped package name")
    _validate_component(item.get("name"), "skipped export name", allow_separator=True)
    if expected_package is not None and item["package"] != expected_package:
        raise CacheError("skipped-entry package disagrees with its source")
    if not isinstance(item.get("reason"), str) or not item["reason"]:
        raise CacheError("invalid skipped-entry reason")


def _write_blob(cache_dir: Path, payload: bytes) -> dict:
    if not 0 < len(payload) <= MAX_BLOB_SIZE:
        raise CacheError(
            f"payload has {len(payload)} bytes; version-2 blob cap is {MAX_BLOB_SIZE}")
    sha256 = _sha256_bytes(payload)
    relative = _blob_relative_path(sha256)
    target = _relative_cache_path(cache_dir, relative)
    if (not target.is_file() or target.stat().st_size != len(payload)
            or _sha256_file(target) != sha256):
        _atomic_write(target, payload)
    return {"blob": relative, "size": len(payload), "sha256": sha256}


def _marker_path(cache_dir: Path, kind: str, source: str) -> Path:
    marker_id = hashlib.sha256(f"{kind}\0{source}".encode("utf-8")).hexdigest()[:24]
    return cache_dir / ".state" / "packages" / kind / f"{marker_id}.json"


def _load_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CacheError(f"cannot read {path}: {exc}") from exc


def _marker_entries_if_valid(cache_dir: Path, marker_path: Path,
                             kind: str, source_record: dict,
                             input_sha256: str | None = None) -> tuple[dict, list] | None:
    if not marker_path.is_file():
        return None
    try:
        marker = _load_json(marker_path)
        input_sha256 = input_sha256 or source_record["sha256"]
        if (not isinstance(marker, dict)
                or marker.get("marker_version") != MARKER_VERSION
                or marker.get("cache_version") != CACHE_VERSION
                or marker.get("type") != kind
                or marker.get("source") != source_record
                or marker.get("input_sha256") != input_sha256
                or not isinstance(marker.get("entries"), dict)
                or not isinstance(marker.get("skipped"), list)):
            return None
        if set(marker) != {
                "marker_version", "cache_version", "type", "source",
                "input_sha256", "entries", "skipped"}:
            return None
        _validate_source_record(marker["source"], expected_kind=kind)
        if len(marker["entries"]) > MAX_ENTRIES:
            return None
        expected_package = PurePosixPath(source_record["path"]).stem
        for key, entry in marker["entries"].items():
            _validate_entry_record(
                key, entry, expected_kind=kind, expected_package=expected_package)
            blob = _relative_cache_path(cache_dir, entry["blob"])
            if not blob.is_file() or blob.stat().st_size != entry["size"]:
                return None
            _validate_entry_record(
                key, entry, expected_kind=kind, expected_package=expected_package,
                payload=blob.read_bytes())
        for item in marker["skipped"]:
            _validate_skipped_record(
                item, expected_kind=kind, expected_package=expected_package)
            if item["source"] != source_record["path"]:
                return None
        return marker["entries"], marker["skipped"]
    except (CacheError, KeyError, TypeError, ValueError):
        return None


def _materialize_package(cache_dir: Path, kind: str, source_record: dict,
                         decoded: dict, skipped: list[dict], marker_path: Path,
                         input_sha256: str | None = None) -> dict:
    entries = {}
    expected_package = PurePosixPath(source_record["path"]).stem
    if len(decoded) > MAX_ENTRIES:
        raise CacheError(f"source package exceeds version-2 entry cap {MAX_ENTRIES}")
    for key in sorted(decoded):
        metadata, payload = decoded[key]
        entry = {**metadata, **_write_blob(cache_dir, payload)}
        _validate_entry_record(
            key, entry, expected_kind=kind, expected_package=expected_package,
            payload=payload)
        entries[key] = entry
    for item in skipped:
        _validate_skipped_record(
            item, expected_kind=kind, expected_package=expected_package)
        if item["source"] != source_record["path"]:
            raise CacheError("skipped-entry source disagrees with package marker")
    marker = {
        "marker_version": MARKER_VERSION,
        "cache_version": CACHE_VERSION,
        "type": kind,
        "source": source_record,
        "input_sha256": input_sha256 or source_record["sha256"],
        "entries": entries,
        "skipped": sorted(
            skipped,
            key=lambda item: (item["source"].casefold(), item["name"].casefold(),
                              item["reason"]),
        ),
    }
    _atomic_write(marker_path, _pretty_json_bytes(marker))
    return entries


def _load_existing_manifest(cache_dir: Path) -> dict | None:
    path = cache_dir / "manifest.json"
    if not path.exists():
        return None
    manifest = _load_json(path)
    _validate_manifest_record(manifest, str(path), require_hash=True)
    return manifest


def _manifest_summary(entries: dict, sources: list[dict]) -> dict:
    counts = {kind: 0 for kind in TYPE_ORDER}
    logical_bytes = 0
    unique_blobs = {}
    for entry in entries.values():
        kind = entry["type"]
        counts[kind] += 1
        logical_bytes += entry["size"]
        prior = unique_blobs.setdefault(entry["blob"], entry["size"])
        if prior != entry["size"]:
            raise CacheError(f"blob {entry['blob']} has conflicting sizes")
    source_counts = {kind: 0 for kind in TYPE_ORDER}
    for source in sources:
        source_counts[source["type"]] += 1
    return {
        "entries": counts,
        "source_packages": source_counts,
        "logical_payload_bytes": logical_bytes,
        "total_expanded_bytes": _validate_global_limits(entries),
        "unique_blobs": len(unique_blobs),
        "unique_blob_bytes": sum(unique_blobs.values()),
    }


def _validate_manifest_record(manifest: object, label: str, *,
                              require_hash: bool) -> None:
    fields = {
        "format", "cache_version", "hash_algorithm", "limits", "types",
        "entries", "sources", "skipped", "summary",
    }
    if require_hash:
        fields.add("cache_sha256")
    if not isinstance(manifest, dict) or set(manifest) != fields:
        raise CacheError(
            f"{label} is not a compatible version-{CACHE_VERSION} cache")
    if (manifest.get("format") != CACHE_FORMAT
            or manifest.get("cache_version") != CACHE_VERSION
            or manifest.get("hash_algorithm") != HASH_ALGORITHM
            or manifest.get("limits") != CACHE_LIMITS
            or not isinstance(manifest.get("entries"), dict)
            or not isinstance(manifest.get("sources"), list)
            or not isinstance(manifest.get("skipped"), list)
            or not isinstance(manifest.get("types"), list)
            or not isinstance(manifest.get("summary"), dict)
            or (require_hash and not _is_sha256(manifest.get("cache_sha256")))):
        raise CacheError(f"{label} has an invalid version-2 manifest contract")

    types = manifest["types"]
    if (len(types) != len(set(types)) or any(kind not in TYPE_ORDER for kind in types)
            or types != [kind for kind in TYPE_ORDER if kind in types]):
        raise CacheError(f"{label} has an invalid type list")

    source_keys = set()
    source_packages = {kind: set() for kind in TYPE_ORDER}
    for source in manifest["sources"]:
        _validate_source_record(source)
        if source["type"] not in types:
            raise CacheError(f"{label} source has an undeclared type")
        source_key = (source["type"], source["path"])
        if source_key in source_keys:
            raise CacheError(f"{label} repeats source {source_key}")
        source_keys.add(source_key)
        source_packages[source["type"]].add(PurePosixPath(source["path"]).stem)
    ordered_sources = sorted(
        manifest["sources"],
        key=lambda item: (_type_sort_key(item["type"]), item["path"].casefold(),
                          item["path"]),
    )
    if manifest["sources"] != ordered_sources:
        raise CacheError(f"{label} sources are not in canonical order")

    if list(manifest["entries"]) != sorted(manifest["entries"]):
        raise CacheError(f"{label} entries are not in canonical order")
    for key, entry in manifest["entries"].items():
        _validate_entry_record(key, entry)
        if entry["type"] not in types:
            raise CacheError(f"{label} entry has an undeclared type")
        if entry["package"] not in source_packages[entry["type"]]:
            raise CacheError(f"{label} entry package has no matching source")
    _validate_global_limits(manifest["entries"])

    for item in manifest["skipped"]:
        _validate_skipped_record(item)
        if item["type"] not in types or (item["type"], item["source"]) not in source_keys:
            raise CacheError(f"{label} skipped entry has no matching source")
        if item["package"] != PurePosixPath(item["source"]).stem:
            raise CacheError(f"{label} skipped entry package disagrees with source")
    ordered_skipped = sorted(
        manifest["skipped"],
        key=lambda item: (_type_sort_key(item["type"]), item["source"].casefold(),
                          item["name"].casefold(), item["reason"]),
    )
    if manifest["skipped"] != ordered_skipped:
        raise CacheError(f"{label} skipped entries are not in canonical order")

    expected_summary = _manifest_summary(manifest["entries"], manifest["sources"])
    if manifest["summary"] != expected_summary:
        raise CacheError(f"{label} has an invalid summary")


def compute_cache_sha256(manifest: dict) -> str:
    core = dict(manifest)
    core.pop("cache_sha256", None)
    canonical = _canonical_json_bytes(core)
    digest = hashlib.sha256()
    digest.update(b"WOT-PAYLOAD-CACHE-HASH-V2\0")
    digest.update(len(canonical).to_bytes(8, "little"))
    digest.update(canonical)

    blobs = {}
    for entry in manifest["entries"].values():
        record = (entry["size"], entry["sha256"])
        prior = blobs.setdefault(entry["blob"], record)
        if prior != record:
            raise CacheError(f"blob {entry['blob']} has conflicting records")
    for relative, (size, sha256) in sorted(blobs.items()):
        record = f"{relative}\0{size}\0{sha256}\n".encode("ascii")
        digest.update(len(record).to_bytes(8, "little"))
        digest.update(record)
    return digest.hexdigest()


def _garbage_collect_blobs(cache_dir: Path, entries: dict) -> None:
    referenced = {entry["blob"] for entry in entries.values()}
    blob_root = cache_dir / "blobs"
    if not blob_root.is_dir():
        return
    for path in sorted(blob_root.rglob("*.blob")):
        relative = path.relative_to(cache_dir).as_posix()
        if relative not in referenced:
            path.unlink()
    for directory in sorted((path for path in blob_root.rglob("*") if path.is_dir()),
                            reverse=True):
        try:
            directory.rmdir()
        except OSError:
            pass


def build_cache(retail_root: Path, cache_dir: Path,
                types: Sequence[str], *, bsp_conversion_dir: Path | None = None,
                progress=print) -> tuple[dict, dict]:
    retail_root = retail_root.resolve()
    cache_dir = cache_dir.resolve()
    cache_dir.mkdir(parents=True, exist_ok=True)
    selected = set(types)
    existing = _load_existing_manifest(cache_dir)
    if existing is not None:
        expected_root = compute_cache_sha256(existing)
        if existing.get("cache_sha256") != expected_root:
            raise CacheError("existing manifest whole-cache hash does not match")
        # A killed update may have written valid new blobs/markers before the
        # old manifest was atomically replaced. They are candidates for resume,
        # not proof that the completed old manifest is corrupt; final GC removes
        # anything the new manifest does not reference.
        _validate_manifest_blobs(cache_dir, existing, reject_unreferenced=False)

    entries = {
        key: entry for key, entry in (existing or {}).get("entries", {}).items()
        if entry.get("type") not in selected
    }
    sources = [
        source for source in (existing or {}).get("sources", [])
        if source.get("type") not in selected
    ]
    skipped = [
        item for item in (existing or {}).get("skipped", [])
        if item.get("type") not in selected
    ]
    cache_types = set((existing or {}).get("types", [])) - selected
    stats = {"packages_built": 0, "packages_resumed": 0}

    for kind in types:
        packages = source_packages(retail_root, kind)
        cache_types.add(kind)
        for index, package_path in enumerate(packages, 1):
            source = _source_relative(retail_root, package_path)
            progress(f"[{kind}] {index}/{len(packages)} {source}")
            source_record = _source_record(retail_root, kind, package_path)
            input_sha256 = _package_input_sha256(
                kind, source_record, bsp_conversion_dir)
            marker_path = _marker_path(cache_dir, kind, source)
            resumed = _marker_entries_if_valid(
                cache_dir, marker_path, kind, source_record, input_sha256)
            if resumed is not None:
                package_entries, package_skipped = resumed
                stats["packages_resumed"] += 1
            else:
                decoded, package_skipped = decode_package(
                    kind, package_path, source, retail_root=retail_root,
                    bsp_conversion_dir=bsp_conversion_dir)
                package_entries = _materialize_package(
                    cache_dir, kind, source_record, decoded, package_skipped,
                    marker_path, input_sha256)
                stats["packages_built"] += 1
            for key, entry in package_entries.items():
                if key in entries:
                    raise CacheError(f"duplicate semantic cache key {key}")
                entries[key] = entry
            sources.append(source_record)
            skipped.extend(package_skipped)

    ordered_sources = sorted(
        sources,
        key=lambda item: (_type_sort_key(item["type"]), item["path"].casefold(),
                          item["path"]),
    )
    ordered_skipped = sorted(
        skipped,
        key=lambda item: (_type_sort_key(item["type"]), item["source"].casefold(),
                          item["name"].casefold(), item["reason"]),
    )
    manifest = {
        "format": CACHE_FORMAT,
        "cache_version": CACHE_VERSION,
        "hash_algorithm": HASH_ALGORITHM,
        "limits": dict(CACHE_LIMITS),
        "types": [kind for kind in TYPE_ORDER if kind in cache_types],
        "entries": {key: entries[key] for key in sorted(entries)},
        "sources": ordered_sources,
        "skipped": ordered_skipped,
        "summary": _manifest_summary(entries, ordered_sources),
    }
    _validate_manifest_record(manifest, "fresh manifest", require_hash=False)
    manifest["cache_sha256"] = compute_cache_sha256(manifest)
    _validate_manifest_record(manifest, "fresh manifest", require_hash=True)
    _atomic_write(cache_dir / "manifest.json", _pretty_json_bytes(manifest))
    _garbage_collect_blobs(cache_dir, entries)
    return manifest, stats


def _validate_manifest_blobs(cache_dir: Path, manifest: dict, *,
                             reject_unreferenced: bool = True) -> None:
    referenced = set()
    checked: dict[str, bytes] = {}
    for key, entry in manifest["entries"].items():
        _validate_entry_record(key, entry)
        expected_relative = entry["blob"]
        referenced.add(expected_relative)
        payload = checked.get(expected_relative)
        if payload is None:
            path = _relative_cache_path(cache_dir, expected_relative)
            if not path.is_file():
                raise CacheError(f"missing blob for {key}: {expected_relative}")
            if path.stat().st_size != entry["size"]:
                raise CacheError(f"blob size mismatch for {key}: {expected_relative}")
            payload = path.read_bytes()
            checked[expected_relative] = payload
        _validate_entry_record(key, entry, payload=payload)

    actual = ({path.relative_to(cache_dir).as_posix()
               for path in (cache_dir / "blobs").rglob("*.blob")}
              if (cache_dir / "blobs").is_dir() else set())
    extra = sorted(actual - referenced)
    if reject_unreferenced and extra:
        raise CacheError(f"unreferenced blob in completed cache: {extra[0]}")


def verify_cache(retail_root: Path, cache_dir: Path,
                 types: Sequence[str], *, bsp_conversion_dir: Path | None = None,
                 progress=print) -> tuple[dict, dict]:
    retail_root = retail_root.resolve()
    cache_dir = cache_dir.resolve()
    manifest = _load_existing_manifest(cache_dir)
    if manifest is None:
        raise CacheError(f"missing cache manifest: {cache_dir / 'manifest.json'}")
    missing_types = set(types) - set(manifest["types"])
    if missing_types:
        raise CacheError(
            "cache does not contain selected type(s): " + ", ".join(sorted(missing_types)))
    expected_root = compute_cache_sha256(manifest)
    if manifest.get("cache_sha256") != expected_root:
        raise CacheError(
            f"whole-cache hash mismatch: manifest has {manifest.get('cache_sha256')!r}, "
            f"computed {expected_root}")
    _validate_manifest_blobs(cache_dir, manifest)

    selected = set(types)
    actual_entries = {
        key: entry for key, entry in manifest["entries"].items()
        if entry["type"] in selected
    }
    actual_sources = {
        (source["type"], source["path"]): source
        for source in manifest["sources"] if source["type"] in selected
    }
    actual_skipped = sorted(
        (item for item in manifest["skipped"] if item["type"] in selected),
        key=lambda item: (_type_sort_key(item["type"]), item["source"].casefold(),
                          item["name"].casefold(), item["reason"]),
    )
    expected_keys = set()
    expected_source_keys = set()
    expected_skipped = []
    verified_entries = 0

    for kind in types:
        packages = source_packages(retail_root, kind)
        for index, package_path in enumerate(packages, 1):
            source = _source_relative(retail_root, package_path)
            progress(f"[verify:{kind}] {index}/{len(packages)} {source}")
            source_record = _source_record(retail_root, kind, package_path)
            source_key = (kind, source)
            expected_source_keys.add(source_key)
            if actual_sources.get(source_key) != source_record:
                raise CacheError(f"source record mismatch for {kind}:{source}")

            decoded, package_skipped = decode_package(
                kind, package_path, source, retail_root=retail_root,
                bsp_conversion_dir=bsp_conversion_dir)
            expected_skipped.extend(package_skipped)
            for key, (metadata, payload) in decoded.items():
                if key in expected_keys:
                    raise CacheError(f"duplicate expected semantic cache key {key}")
                expected_keys.add(key)
                actual = actual_entries.get(key)
                if actual is None:
                    raise CacheError(f"manifest is missing decoded entry {key}")
                actual_metadata = {
                    field: value for field, value in actual.items()
                    if field not in ("blob", "size", "sha256")
                }
                if actual_metadata != metadata:
                    raise CacheError(f"metadata mismatch for {key}")
                sha256 = _sha256_bytes(payload)
                if actual["sha256"] != sha256 or actual["size"] != len(payload):
                    raise CacheError(f"decoded payload hash/size mismatch for {key}")
                blob = _relative_cache_path(cache_dir, actual["blob"])
                if blob.read_bytes() != payload:
                    raise CacheError(f"decoded payload byte mismatch for {key}")
                verified_entries += 1

    extra_entries = sorted(set(actual_entries) - expected_keys)
    if extra_entries:
        raise CacheError(f"manifest has entry absent from retail decode: {extra_entries[0]}")
    extra_sources = sorted(set(actual_sources) - expected_source_keys)
    if extra_sources:
        raise CacheError(
            f"manifest has source absent from retail scope: {extra_sources[0]}")
    expected_skipped = sorted(
        expected_skipped,
        key=lambda item: (_type_sort_key(item["type"]), item["source"].casefold(),
                          item["name"].casefold(), item["reason"]),
    )
    if actual_skipped != expected_skipped:
        raise CacheError("deterministic skipped-entry records differ from retail decode")
    return manifest, {"entries_verified": verified_entries,
                      "packages_verified": len(expected_source_keys)}


def _directory_size(path: Path) -> int:
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def _human_bytes(size: int) -> str:
    value = float(size)
    for suffix in ("B", "KiB", "MiB", "GiB"):
        if value < 1024.0 or suffix == "GiB":
            return f"{value:.2f} {suffix}" if suffix != "B" else f"{int(value)} B"
        value /= 1024.0
    raise AssertionError("unreachable")


def _print_report(mode: str, manifest: dict, stats: dict,
                  cache_dir: Path, elapsed: float) -> None:
    summary = manifest["summary"]
    print(f"{mode} payload cache {manifest['cache_sha256']}")
    for kind in TYPE_ORDER:
        if kind in manifest["types"]:
            print(f"  {kind}: {summary['entries'][kind]} entries, "
                  f"{summary['source_packages'][kind]} source packages")
    print(f"  logical payload: {summary['logical_payload_bytes']} bytes "
          f"({_human_bytes(summary['logical_payload_bytes'])})")
    print(f"  expanded payload: {summary['total_expanded_bytes']} bytes "
          f"({_human_bytes(summary['total_expanded_bytes'])})")
    print(f"  unique blobs: {summary['unique_blobs']}, "
          f"{summary['unique_blob_bytes']} bytes "
          f"({_human_bytes(summary['unique_blob_bytes'])})")
    directory_size = _directory_size(cache_dir)
    print(f"  cache directory: {directory_size} bytes "
          f"({_human_bytes(directory_size)})")
    for key in sorted(stats):
        print(f"  {key.replace('_', ' ')}: {stats[key]}")
    print(f"  elapsed seconds: {elapsed:.3f}")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("retail_root", type=Path, nargs="?",
                        help="retail tree containing System/Textures/Sounds")
    parser.add_argument("cache_dir", type=Path, nargs="?",
                        help="player-local payload cache directory")
    parser.add_argument(
        "--contract-version", type=int, choices=(2, 3), default=3,
        help=("cache contract to emit; version 3 is the default fixture path, "
              "while the frozen retail-cache builder requires explicit version 2"))
    parser.add_argument(
        "--emit-synthetic-fixtures", type=Path,
        help="write the cache-v3 native/Python parity corpus to this directory")
    parser.add_argument(
        "--types", default=",".join(DEFAULT_TYPES),
        help=("comma-separated subset of textures,sounds,meshes,bsps "
              "(default: textures,sounds,meshes)"))
    parser.add_argument(
        "--bsp-conv-dir", type=Path,
        help=("bsp2obj/lightmap output directory; required when --types "
              "includes bsps"))
    parser.add_argument(
        "--verify", action="store_true",
        help="verify blobs and re-decode/compare against retail without writing")
    parser.add_argument("--quiet", action="store_true",
                        help="suppress per-package progress lines")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.emit_synthetic_fixtures is not None:
            if args.contract_version != CACHE_V3_VERSION:
                raise CacheError("synthetic typed fixtures require --contract-version 3")
            if args.retail_root is not None or args.cache_dir is not None:
                raise CacheError(
                    "synthetic fixture emission does not accept retail/cache roots")
            written = write_synthetic_cache_v3_fixtures(
                args.emit_synthetic_fixtures)
            print(f"Wrote {len(written)} cache-v3 fixtures to "
                  f"{args.emit_synthetic_fixtures.resolve()}")
            return 0
        if args.retail_root is None:
            args.retail_root = original_dir()
        if args.contract_version != CACHE_VERSION:
            raise CacheError(
                "the retail cache path is the frozen v2 oracle; pass "
                "--contract-version 2 explicitly")
        if args.retail_root is None or args.cache_dir is None:
            raise CacheError("retail_root and cache_dir are required for cache-v2")
        types = parse_types(args.types)
        progress = (lambda _message: None) if args.quiet else print
        started = time.perf_counter()
        if args.verify:
            manifest, stats = verify_cache(
                args.retail_root, args.cache_dir, types,
                bsp_conversion_dir=args.bsp_conv_dir, progress=progress)
            mode = "Verified"
        else:
            manifest, stats = build_cache(
                args.retail_root, args.cache_dir, types,
                bsp_conversion_dir=args.bsp_conv_dir, progress=progress)
            mode = "Built"
        _print_report(mode, manifest, stats, args.cache_dir.resolve(),
                      time.perf_counter() - started)
        return 0
    except (CacheError, OSError, ValueError) as exc:
        print(f"build_payload_cache: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
