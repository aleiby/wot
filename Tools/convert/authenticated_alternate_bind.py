"""Exact authority for the one transient non-collapsed stripped mesh bind."""
import hashlib
import json
import math
import struct
from pathlib import Path


FORMAT = "wot-authenticated-alternate-bind-v1"
IMPORT_RECEIPT_FORMAT = "wot-authenticated-alternate-bind-import-v1"
IDENTITY = "mesh:Angreal__TaintExpGeoB"
SOURCE_FRAME_COUNT = 13
BIND_FRAME = 6
TARGET_FRAMES = (0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12)
HASHES = {
    "raw_base_positions_sha256":
        "f4a3958685c579d73a7928d3f04fc61d473e7d634341fe795f40bd9d09310aaf",
    "raw_base_normals_sha256":
        "a3e19ecacea8735ce8e283ca6910fc3caa8667b357c4a3a585a374337cc4d81a",
    "bind_positions_sha256":
        "d740b8e7f68faad60a336f5e8a75d0b9081160d3eabc05b6d5e7a3554c15d79c",
    "bind_normals_sha256":
        "85dd91ebb82da4b8a58db98b710c2dcff702d45d85bf2e954b12e3aa5f603f60",
}
_SCALAR_KEYS = frozenset((
    "format", "source_identity", "source_frame_count", "bind_frame",
    "target_frames", *HASHES.keys()))
METADATA_KEYS = _SCALAR_KEYS | frozenset(("bind_positions", "bind_normals"))


def vector_sha256(kind, vectors):
    if kind not in ("positions", "normals"):
        raise ValueError(f"invalid alternate-bind vector kind: {kind}")
    if not isinstance(vectors, (list, tuple)):
        raise ValueError("alternate-bind vectors must be a sequence")
    digest = hashlib.sha256()
    digest.update(b"WOT-AUTHENTICATED-ALTERNATE-BIND-V1\0")
    digest.update(kind.encode("ascii") + b"\0")
    digest.update(len(vectors).to_bytes(8, "little"))
    for vector in vectors:
        if (not isinstance(vector, (list, tuple)) or len(vector) != 3
                or not all(type(value) in (int, float)
                           and math.isfinite(value) for value in vector)):
            raise ValueError("alternate-bind vectors must be finite xyz triples")
        digest.update(struct.pack("<3f", *vector))
    return digest.hexdigest()


def authority_receipt():
    return {
        "format": FORMAT,
        "source_identity": IDENTITY,
        "source_frame_count": SOURCE_FRAME_COUNT,
        "bind_frame": BIND_FRAME,
        "target_frames": list(TARGET_FRAMES),
        **HASHES,
    }


def validate_metadata_shape(value):
    if not isinstance(value, dict) or set(value) != METADATA_KEYS:
        raise ValueError("authenticated alternate-bind metadata schema is not exact")
    if (type(value["source_frame_count"]) is not int
            or type(value["bind_frame"]) is not int
            or not isinstance(value["target_frames"], list)
            or any(type(frame) is not int for frame in value["target_frames"])):
        raise ValueError(
            "authenticated alternate-bind frame authority types are not exact")
    expected = authority_receipt()
    for key, wanted in expected.items():
        if value.get(key) != wanted:
            raise ValueError(f"authenticated alternate-bind {key} changed")
    return value


def validate_authority(value):
    expected = authority_receipt()
    if not isinstance(value, dict) or set(value) != set(expected):
        raise ValueError("authenticated alternate-bind authority schema is not exact")
    if (type(value["source_frame_count"]) is not int
            or type(value["bind_frame"]) is not int
            or not isinstance(value["target_frames"], list)
            or any(type(frame) is not int for frame in value["target_frames"])):
        raise ValueError(
            "authenticated alternate-bind frame authority types are not exact")
    if value != expected:
        raise ValueError("authenticated alternate-bind authority changed")
    return value


def make_import_receipt(authority, *, asset, native_morph_target_count,
                        has_vertex_colors):
    receipt = {
        "format": IMPORT_RECEIPT_FORMAT,
        "authority": validate_authority(authority),
        "asset": asset,
        "native_morph_target_count": native_morph_target_count,
        "has_vertex_colors": has_vertex_colors,
    }
    return validate_import_receipt(receipt)


def validate_import_receipt(value):
    keys = {"format", "authority", "asset", "native_morph_target_count",
            "has_vertex_colors"}
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError("authenticated alternate-bind import receipt schema is not exact")
    if value["format"] != IMPORT_RECEIPT_FORMAT:
        raise ValueError("authenticated alternate-bind import receipt format changed")
    validate_authority(value["authority"])
    if value["asset"] != ("/Game/WoT/Meshes/TaintExpGeoB/TaintExpGeoB/"
                          "SkeletalMeshes/TaintExpGeoB.TaintExpGeoB"):
        raise ValueError("authenticated alternate-bind imported asset changed")
    if (type(value["native_morph_target_count"]) is not int
            or value["native_morph_target_count"] != 0):
        raise ValueError("authenticated alternate-bind retained native morph targets")
    if value["has_vertex_colors"] is not False:
        raise ValueError("authenticated alternate-bind retained mapping vertex colors")
    return value


def write_import_receipt(path, receipt):
    validate_import_receipt(receipt)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def require_for_import(value, *, payload_key, layout, stripped,
                       mapping_carriers):
    """Validate authority and return transient vectors plus vector-free proof."""
    if value is None:
        if stripped and mapping_carriers and payload_key == IDENTITY:
            raise ValueError("authenticated alternate-bind metadata is required")
        return None
    if not stripped or not mapping_carriers:
        raise ValueError("authenticated alternate-bind is stripped-carrier only")
    validate_metadata_shape(value)
    if payload_key != IDENTITY:
        raise ValueError("authenticated alternate-bind payload identity changed")
    if (not isinstance(layout, dict) or layout.get("version") != 1
            or layout.get("frame_verts") != 38
            or layout.get("anim_frames") != SOURCE_FRAME_COUNT):
        raise ValueError("authenticated alternate-bind payload layout changed")
    positions = value["bind_positions"]
    normals = value["bind_normals"]
    raw_positions = layout.get("base_positions")
    raw_normals = layout.get("base_normals")
    if any(len(vectors) != 38 for vectors in (
            positions, normals, raw_positions or (), raw_normals or ())):
        raise ValueError("authenticated alternate-bind vector count changed")
    actual = {
        "raw_base_positions_sha256": vector_sha256("positions", raw_positions),
        "raw_base_normals_sha256": vector_sha256("normals", raw_normals),
        "bind_positions_sha256": vector_sha256("positions", positions),
        "bind_normals_sha256": vector_sha256("normals", normals),
    }
    if actual != HASHES:
        raise ValueError("authenticated alternate-bind vectors are not pinned")
    extents = [max(point[axis] for point in positions)
               - min(point[axis] for point in positions) for axis in range(3)]
    if sum(extent > 0.0 for extent in extents) < 2:
        raise ValueError("authenticated alternate-bind frame is collapsed")
    return {
        "positions": positions,
        "normals": normals,
        "authority": authority_receipt(),
    }
