#!/usr/bin/env python3
"""Prove that staged containers contain no canonical WoT payload probes.

The authenticated player cache is the source of truth.  This tool derives
high-entropy probes from every cache entry and scans every staged pak/IoStore
container once with a stdlib-only Aho-Corasick automaton.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import tempfile
from array import array
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable, Sequence


DIST_DIR = Path(__file__).resolve().parent
if str(DIST_DIR) not in sys.path:
    sys.path.insert(0, str(DIST_DIR))
import build_payload_cache as cache  # noqa: E402


REPORT_FORMAT = "wot-stripped-pak-audit-v1"
CONTAINER_SUFFIXES = {".pak", ".ucas", ".utoc"}
DEFAULT_CHUNK_SIZE = 1024 * 1024
PROBE_WINDOW_SIZE = 4096
PROBE_SIZE = 64
MIN_PROBE_SIZE = 64
MIN_DISTINCT_BYTES = 16

# Format/provenance witnesses for retail-derived entry products that are not
# guaranteed to retain a high-entropy 64-byte slice after cooking.  These are
# public format markers, never values copied from the retail entry map.  A
# neutral consumer may name schemas and classes; only serialized payload magic
# or canonical sidecar/T3D framing is forbidden here.
SEMANTIC_CANARY_SPECS = (
    ("entry_t3d", "t3d-map-framing", b"Begin Map"),
    ("entry_bsp", "wot-bsp-v3-payload", b"WOTPBSP\0"),
    ("entry_atlas_pixels", "wot-texture-v3-payload", b"WOTPTEX\0"),
    ("entry_atlas_metrics", "wot-font-v3-payload", b"WOTFNT3\0"),
    ("entry_menu_audio", "wot-sound-v3-object-header",
     b'"importer":"sound-v1","importer_version":1,"kind":"sound"'),
    ("entry_generated_world", "wot-world-v3-payload", b"WOTWRL3\0"),
    ("entry_descriptor", "front-end-sidecar-v1-framing",
     b'"format":"wot-front-end-descriptor-sidecar-v1"'),
    ("entry_descriptor", "front-end-sidecar-v2-framing",
     b'"format":"wot-front-end-descriptor-sidecar-v2"'),
)
SEMANTIC_KINDS = tuple(dict.fromkeys(
    kind for kind, _representation, _data in SEMANTIC_CANARY_SPECS))
AUDIT_KINDS = tuple(cache.TYPE_ORDER) + SEMANTIC_KINDS
V3_REQUIRED_SCHEMAS = {
    "wot-texture-v3": "textures",
    "wot-sound-v3": "sounds",
    "wot-mesh-v3": "meshes",
    "wot-bsp-v3": "bsps",
}
V3_SCHEMA_KINDS = {
    "wot-animation-v3": "animation",
    "wot-bsp-v3": "bsp",
    "wot-class-default-v3": "class_default",
    "wot-font-v3": "font",
    "wot-front-end-world-v3": "front_end_world",
    "wot-localization-v3": "localization",
    "wot-mesh-v3": "mesh",
    "wot-procedural-v3": "procedural",
    "wot-registry-v3": "registry",
    "wot-sound-v3": "sound",
    "wot-texture-v3": "texture",
    "wot-world-v3": "world",
}


class AuditError(RuntimeError):
    """The audit could not establish absence."""


@dataclass(frozen=True)
class ProbeSource:
    key: str
    kind: str
    representation: str
    location: str


@dataclass(frozen=True)
class Needle:
    data: bytes
    sources: tuple[ProbeSource, ...]


def build_semantic_needles() -> list[Needle]:
    """Return deterministic non-retail format/provenance absence canaries."""
    return [Needle(data, (ProbeSource(
        key=f"semantic:{kind}", kind=kind,
        representation=representation, location="anywhere"),))
        for kind, representation, data in SEMANTIC_CANARY_SPECS]


class AhoCorasick:
    """Small byte-oriented Aho-Corasick matcher."""

    def __init__(self, patterns: Sequence[bytes]):
        if not patterns or any(not pattern for pattern in patterns):
            raise ValueError("Aho-Corasick requires non-empty patterns")
        self.transitions: list[dict[int, int]] = [{}]
        self.failure = array("I", [0])
        self.outputs: dict[int, list[int]] = {}
        self.pattern_lengths = array("I", (len(pattern) for pattern in patterns))
        for pattern_id, pattern in enumerate(patterns):
            state = 0
            for byte in pattern:
                next_state = self.transitions[state].get(byte)
                if next_state is None:
                    next_state = len(self.transitions)
                    self.transitions[state][byte] = next_state
                    self.transitions.append({})
                    self.failure.append(0)
                state = next_state
            self.outputs.setdefault(state, []).append(pattern_id)

        pending = deque(self.transitions[0].values())
        while pending:
            state = pending.popleft()
            for byte, target in self.transitions[state].items():
                pending.append(target)
                fallback = self.failure[state]
                while fallback and byte not in self.transitions[fallback]:
                    fallback = self.failure[fallback]
                self.failure[target] = self.transitions[fallback].get(byte, 0)
                inherited = self.outputs.get(self.failure[target])
                if inherited:
                    self.outputs.setdefault(target, []).extend(inherited)
        for output in self.outputs.values():
            output.sort()

    def iter_matches(self, data: bytes, *, state: int = 0,
                     base_offset: int = 0):
        for index, byte in enumerate(data):
            while state and byte not in self.transitions[state]:
                state = self.failure[state]
            state = self.transitions[state].get(byte, 0)
            for pattern_id in self.outputs.get(state, ()):
                yield (base_offset + index - self.pattern_lengths[pattern_id] + 1,
                       pattern_id)
        return state

    def scan(self, data: bytes) -> list[tuple[int, int]]:
        return list(self.iter_matches(data))


def _authenticated_entries(cache_dir: Path) -> tuple[dict, dict[str, bytes]]:
    """Validate the complete manifest/root hash and every referenced blob."""
    cache_dir = Path(cache_dir).resolve()
    manifest_path = cache_dir / "manifest.json"
    manifest = cache._load_json(manifest_path)
    cache._validate_manifest_record(
        manifest, str(manifest_path), require_hash=True)
    computed = cache.compute_cache_sha256(manifest)
    if manifest.get("cache_sha256") != computed:
        raise AuditError(
            f"whole-cache hash mismatch: manifest has "
            f"{manifest.get('cache_sha256')!r}, computed {computed}")

    payloads: dict[str, bytes] = {}
    referenced = set()
    for key, entry in manifest["entries"].items():
        relative = entry["blob"]
        referenced.add(relative)
        payload = payloads.get(relative)
        if payload is None:
            path = cache._relative_cache_path(cache_dir, relative)
            if not path.is_file() or path.stat().st_size != entry["size"]:
                raise AuditError(f"missing or wrong-size blob for {key}: {relative}")
            payload = path.read_bytes()
            if hashlib.sha256(payload).hexdigest() != entry["sha256"]:
                raise AuditError(f"blob SHA-256 mismatch for {key}: {relative}")
            payloads[relative] = payload
        cache._validate_entry_record(key, entry, payload=payload)
    blob_root = cache_dir / "blobs"
    actual = ({path.relative_to(cache_dir).as_posix()
               for path in blob_root.rglob("*.blob")}
              if blob_root.is_dir() else set())
    extra = sorted(actual - referenced)
    if extra:
        raise AuditError(f"unreferenced blob in completed cache: {extra[0]}")
    return manifest, payloads


def _resolve_cache_directory(cache_path: Path) -> tuple[Path, str | None]:
    """Resolve the published v3 current directory or a direct v2 cache."""
    cache_path = Path(cache_path).resolve()
    direct_manifest = cache_path / "manifest.json"
    if direct_manifest.is_file():
        manifest = cache._load_json(direct_manifest)
        if manifest.get("cache_version") != cache.CACHE_V3_VERSION:
            return cache_path, None
    current = cache_path if cache_path.name == "current" else cache_path / "current"
    manifest_path = current / "manifest.json"
    if manifest_path.is_file():
        raw = manifest_path.read_bytes()
        return current, hashlib.sha256(raw).hexdigest()
    raise AuditError(f"cache-v3 input has no published current manifest: {cache_path}")


def _front_end_descriptor_root(header: dict):
    """The front-end world's descriptor root, in either published shape.

    Since S1 the game publishes the front-end world as a
    wot-front-end-descriptor-sidecar-v2 record, which carries the root inside
    its "descriptor" object; the earlier record kept it at the top level.
    """
    if header.get("format") == "wot-front-end-descriptor-sidecar-v2":
        descriptor = header.get("descriptor")
        return (descriptor.get("descriptor_root")
                if isinstance(descriptor, dict) else None)
    return header.get("descriptor_root")


def _decode_v3_object(payload: bytes, logical_id: str) -> tuple[dict, bytes]:
    if len(payload) < 12 or payload[:8] != cache.CACHE_V3_OBJECT_MAGIC:
        raise AuditError(f"cache-v3 object has invalid framing: {logical_id}")
    header_size = struct.unpack_from("<I", payload, 8)[0]
    if (header_size == 0 or header_size > cache.MAX_CACHE_V3_HEADER_BYTES
            or 12 + header_size > len(payload)):
        raise AuditError(f"cache-v3 object header is out of bounds: {logical_id}")
    header_bytes = payload[12:12 + header_size]
    try:
        header = json.loads(header_bytes)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AuditError(f"cache-v3 object header is invalid: {logical_id}") from exc
    if (not isinstance(header, dict)
            or cache._canonical_json_bytes(header) != header_bytes):
        raise AuditError(f"cache-v3 object header is not canonical: {logical_id}")
    body = payload[12 + header_size:]
    if header.get("payload_byte_length") != len(body):
        raise AuditError(
            f"cache-v3 object payload length disagrees with header: {logical_id}")
    return header, body


def _authenticated_v3_entries(
        cache_dir: Path, manifest_sha256: str
        ) -> tuple[dict, dict[str, tuple[dict, dict, bytes]]]:
    manifest_path = cache_dir / "manifest.json"
    raw_manifest = manifest_path.read_bytes()
    if hashlib.sha256(raw_manifest).hexdigest() != manifest_sha256:
        raise AuditError("cache-v3 current manifest digest changed after resolution")
    manifest = cache._load_json(manifest_path)
    required = {
        "blobs", "cache_version", "contract_sha256", "format",
        "front_end_descriptor_root", "hash_algorithm", "installer_sha256",
        "production_descriptor_sha256", "world_descriptors",
    }
    if (set(manifest) != required
            or manifest.get("cache_version") != cache.CACHE_V3_VERSION
            or manifest.get("format") != cache.CACHE_FORMAT
            or manifest.get("hash_algorithm") != cache.HASH_ALGORITHM
            or cache._canonical_json_bytes(manifest) != raw_manifest):
        raise AuditError("cache-v3 manifest contract is invalid")
    for field in (
            "contract_sha256", "installer_sha256",
            "production_descriptor_sha256", "front_end_descriptor_root"):
        if re.fullmatch(r"[0-9a-f]{64}", manifest.get(field, "")) is None:
            raise AuditError(f"cache-v3 manifest {field} is invalid")
    if not isinstance(manifest["world_descriptors"], list):
        raise AuditError("cache-v3 manifest world descriptors are invalid")
    rows = manifest.get("blobs")
    if not isinstance(rows, list) or not rows:
        raise AuditError("cache-v3 manifest has no blobs")
    if rows != sorted(
            rows, key=lambda item: item.get("logical_id", "").casefold()):
        raise AuditError("cache-v3 manifest blobs are not in canonical order")
    objects = {}
    referenced = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != {
                "logical_id", "satisfies", "schema", "sha256", "size"}:
            raise AuditError("cache-v3 manifest blob row is malformed")
        logical_id = row["logical_id"]
        satisfies = row["satisfies"]
        if (not isinstance(logical_id, str) or not logical_id
                or logical_id in objects
                or not isinstance(row["schema"], str) or not row["schema"]
                or re.fullmatch(r"[0-9a-f]{64}", row["sha256"]) is None
                or not isinstance(row["size"], int) or row["size"] <= 0
                or not isinstance(satisfies, list) or not satisfies
                or satisfies != sorted(satisfies)
                or len(satisfies) != len(set(satisfies))
                or any(not isinstance(item, str) or not item for item in satisfies)):
            raise AuditError(f"cache-v3 manifest blob is invalid: {logical_id!r}")
        relative = "blobs/" + hashlib.sha256(
            logical_id.encode("utf-8")).hexdigest() + ".blob"
        referenced.add(relative)
        path = cache_dir / relative
        try:
            payload = path.read_bytes()
        except OSError as exc:
            raise AuditError(f"missing cache-v3 blob for {logical_id}: {relative}") from exc
        if (len(payload) != row["size"]
                or hashlib.sha256(payload).hexdigest() != row["sha256"]):
            raise AuditError(f"cache-v3 blob identity mismatch: {logical_id}")
        schema = row["schema"]
        expected_kind = V3_SCHEMA_KINDS.get(schema)
        if expected_kind is None:
            raise AuditError(f"cache-v3 manifest has unknown schema: {schema}")
        if schema == "wot-front-end-world-v3":
            try:
                header = json.loads(payload)
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise AuditError(
                    f"cache-v3 front-end world is invalid: {logical_id}") from exc
            if (not isinstance(header, dict)
                    or cache._canonical_json_bytes(header) != payload):
                raise AuditError(
                    f"cache-v3 front-end world is not canonical: {logical_id}")
            body = payload
        else:
            header, body = _decode_v3_object(payload, logical_id)
            if header.get("kind") != expected_kind:
                raise AuditError(
                    f"cache-v3 object kind disagrees with schema: {logical_id}")
        objects[logical_id] = (row, header, body)
    front_end = objects.get("cache/front_end_world/WheelOfTime")
    if front_end is not None and (
            front_end[0]["schema"] != "wot-front-end-world-v3"
            or _front_end_descriptor_root(front_end[1])
                != manifest["front_end_descriptor_root"]):
        raise AuditError("cache-v3 front-end descriptor root does not match manifest")
    actual = ({path.relative_to(cache_dir).as_posix()
               for path in (cache_dir / "blobs").glob("*.blob")}
              if (cache_dir / "blobs").is_dir() else set())
    extra = sorted(actual - referenced)
    missing = sorted(referenced - actual)
    if missing:
        raise AuditError(f"cache-v3 manifest blob is missing: {missing[0]}")
    if extra:
        raise AuditError(f"unreferenced cache-v3 blob: {extra[0]}")
    return manifest, objects


def _probe_slices(data: bytes, key: str, kind: str,
                  representation: str) -> list[tuple[bytes, ProbeSource]]:
    if len(data) < MIN_PROBE_SIZE:
        return []
    window_size = min(PROBE_WINDOW_SIZE, len(data))
    starts = [("head", 0)]
    middle = (len(data) - window_size) // 2
    if middle:
        starts.append(("middle", middle))
    result = []
    for location, window_start in starts:
        window = data[window_start:window_start + window_size]
        selected = None
        # A 64-byte high-entropy witness is sufficient: if the canonical run
        # were present, this witness would necessarily be present. Search the
        # pinned 4096-byte head/middle windows rather than turning tens of
        # thousands of 4 KiB patterns into an impractically huge Python trie.
        for local_start in range(0, len(window) - PROBE_SIZE + 1):
            probe = bytes(window[local_start:local_start + PROBE_SIZE])
            if len(set(probe)) >= MIN_DISTINCT_BYTES:
                selected = (probe, local_start)
                break
        if selected is not None:
            probe, local_start = selected
            result.append((probe, ProbeSource(
                key=key, kind=kind, representation=representation,
                location=f"{location}+{local_start}")))
    return result


def _texture_streams(payload: bytes, entry: dict) -> tuple[bytes, bytes]:
    header_size = struct.calcsize("<8sHHII")
    _magic, _version, flags, colors, _mips = struct.unpack_from(
        "<8sHHII", payload)
    palette_offset = header_size
    mip_offset = palette_offset + colors * 4
    width, height, stored = struct.unpack_from("<III", payload, mip_offset)
    indices = payload[mip_offset + 12:mip_offset + 12 + stored]
    if (width != entry["width"] or height != entry["height"]
            or len(indices) != width * height):
        raise AuditError("authenticated texture mip-0 layout disagrees with metadata")
    palette = payload[palette_offset:mip_offset]
    bgra = bytearray(len(indices) * 4)
    masked = bool(flags & 1)
    for pixel, palette_index in enumerate(indices):
        red, green, blue, _alpha = palette[palette_index * 4:palette_index * 4 + 4]
        offset = pixel * 4
        bgra[offset:offset + 4] = bytes((
            blue, green, red, 0 if masked and palette_index == 0 else 255))
    return bytes(indices), bytes(bgra)


def _wav_data(payload: bytes) -> bytes:
    offset = 12
    while offset < len(payload):
        chunk_id = payload[offset:offset + 4]
        chunk_size = struct.unpack_from("<I", payload, offset + 4)[0]
        start = offset + 8
        if chunk_id == b"data":
            return payload[start:start + chunk_size]
        offset = start + chunk_size + (chunk_size & 1)
    raise AuditError("authenticated WAV has no data chunk")


def _mesh_frames(payload: bytes) -> bytes:
    return payload[struct.calcsize("<8sHHIII"):]


def _bsp_streams(payload: bytes, entry: dict) -> tuple[bytes, bytes]:
    header = struct.Struct("<8sHHIIIHH6f")
    section = struct.Struct("<HHIIIIB3x")
    offset = header.size
    for _ in range(entry["section_count"]):
        name_size = section.unpack_from(payload, offset)[0]
        offset += section.size + name_size
    positions_size = entry["vertex_count"] * 3 * 4
    positions = payload[offset:offset + positions_size]
    indices_offset = offset + entry["vertex_count"] * 10 * 4
    indices = payload[indices_offset:indices_offset + entry["index_count"] * 4]
    if len(positions) != positions_size or len(indices) != entry["index_count"] * 4:
        raise AuditError("authenticated BSP stream ranges are truncated")
    return positions, indices


def _finish_needles(candidates: list[tuple[bytes, ProbeSource]], stats: dict,
                    required_kinds: set[str]) -> tuple[list[Needle], dict]:
    grouped: dict[bytes, list[ProbeSource]] = {}
    for data, source in candidates:
        grouped.setdefault(data, []).append(source)
    probed_types = {source.kind for _data, source in candidates}
    missing_probes = required_kinds - probed_types
    if missing_probes:
        raise AuditError(
            "cache produced no high-entropy probes for type(s): "
            + ", ".join(sorted(missing_probes)))
    for needle in build_semantic_needles():
        grouped.setdefault(needle.data, []).extend(needle.sources)
    needles = [Needle(data=data, sources=tuple(sorted(
        sources, key=lambda item: (
            item.kind, item.key, item.representation, item.location))))
        for data, sources in sorted(grouped.items(), key=lambda item: item[0])]
    stats.update({
        "candidate_probes": len(candidates),
        "semantic_canaries": len(SEMANTIC_CANARY_SPECS),
        "unique_needles": len(needles),
    })
    return needles, stats


def _build_v3_needles(cache_dir: Path, manifest_sha256: str
                      ) -> tuple[list[Needle], dict]:
    manifest, objects = _authenticated_v3_entries(cache_dir, manifest_sha256)
    entry_counts: dict[str, int] = {}
    candidates: list[tuple[bytes, ProbeSource]] = []
    for logical_id, (row, header, body) in objects.items():
        schema = row["schema"]
        entry_counts[schema] = entry_counts.get(schema, 0) + 1
        # Probe every authenticated v3 representation, not merely the four
        # legacy cache types. This makes the v3 audit a strict superset.
        candidates.extend(_probe_slices(
            body, logical_id, schema, "authenticated-v3-payload"))
        kind = V3_REQUIRED_SCHEMAS.get(schema)
        if kind == "textures":
            layout = header.get("decoded_layout")
            mips = layout.get("mips") if isinstance(layout, dict) else None
            if not isinstance(mips, list) or not mips:
                raise AuditError(
                    f"cache-v3 texture has no decoded mip layout: {logical_id}")
            raw, bgra = _texture_streams(body, {
                "width": mips[0].get("width"),
                "height": mips[0].get("height"),
            })
            candidates.extend(_probe_slices(
                raw, logical_id, kind, "p8-mip0"))
            candidates.extend(_probe_slices(
                bgra, logical_id, kind, "bgra8-mip0"))
        elif kind == "sounds":
            candidates.extend(_probe_slices(
                _wav_data(body), logical_id, kind, "wav-pcm-data"))
        elif kind == "meshes":
            candidates.extend(_probe_slices(
                body, logical_id, kind, "wot-mesh-v3-payload"))
        elif kind == "bsps":
            positions, indices = _bsp_streams(body, header)
            candidates.extend(_probe_slices(
                positions, logical_id, kind, "position-stream"))
            candidates.extend(_probe_slices(
                indices, logical_id, kind, "index-stream"))
    missing_schemas = set(V3_REQUIRED_SCHEMAS) - set(entry_counts)
    if missing_schemas:
        raise AuditError(
            "cache is missing required audit schema(s): "
            + ", ".join(sorted(missing_schemas)))
    return _finish_needles(candidates, {
        "cache_sha256": manifest_sha256,
        "cache_version": cache.CACHE_V3_VERSION,
        "entry_counts": dict(sorted(entry_counts.items())),
    }, set(V3_REQUIRED_SCHEMAS.values()))


def build_needles(cache_dir: Path) -> tuple[list[Needle], dict]:
    cache_dir, active_manifest_sha256 = _resolve_cache_directory(cache_dir)
    if active_manifest_sha256 is not None:
        return _build_v3_needles(cache_dir, active_manifest_sha256)
    manifest, payloads = _authenticated_entries(cache_dir)
    missing_types = set(cache.TYPE_ORDER) - set(manifest["types"])
    if missing_types:
        raise AuditError(
            "cache is missing required audit type(s): "
            + ", ".join(sorted(missing_types)))
    candidates: list[tuple[bytes, ProbeSource]] = []
    for key, entry in manifest["entries"].items():
        payload = payloads[entry["blob"]]
        kind = entry["type"]
        if kind == "textures":
            raw, bgra = _texture_streams(payload, entry)
            candidates.extend(_probe_slices(raw, key, kind, "p8-mip0"))
            candidates.extend(_probe_slices(bgra, key, kind, "bgra8-mip0"))
        elif kind == "sounds":
            candidates.extend(_probe_slices(
                _wav_data(payload), key, kind, "wav-pcm-data"))
        elif kind == "meshes":
            candidates.extend(_probe_slices(
                _mesh_frames(payload), key, kind, "packed-frames"))
        elif kind == "bsps":
            positions, indices = _bsp_streams(payload, entry)
            candidates.extend(_probe_slices(
                positions, key, kind, "position-stream"))
            candidates.extend(_probe_slices(
                indices, key, kind, "index-stream"))

    return _finish_needles(candidates, {
        "cache_sha256": manifest["cache_sha256"],
        "cache_version": manifest["cache_version"],
        "entry_counts": manifest["summary"]["entries"],
    }, set(cache.TYPE_ORDER))


def scan_stream(stream: BinaryIO, automaton: AhoCorasick,
                *, chunk_size: int = DEFAULT_CHUNK_SIZE,
                digest=None) -> list[tuple[int, int]]:
    """Scan one stream, retaining automaton state across chunk boundaries."""
    if chunk_size < 1:
        raise ValueError("chunk_size must be positive")
    matches = []
    state = 0
    offset = 0
    while True:
        block = stream.read(chunk_size)
        if not block:
            break
        if digest is not None:
            digest.update(block)
        # iter_matches is a generator whose StopIteration value carries state;
        # duplicate the tiny transition loop here to keep that state explicit.
        for index, byte in enumerate(block):
            while state and byte not in automaton.transitions[state]:
                state = automaton.failure[state]
            state = automaton.transitions[state].get(byte, 0)
            for pattern_id in automaton.outputs.get(state, ()):
                matches.append((
                    offset + index - automaton.pattern_lengths[pattern_id] + 1,
                    pattern_id))
        offset += len(block)
    return matches


def scan_files(paths: Iterable[Path], needles: Sequence[Needle], *,
               root: Path | None = None,
               chunk_size: int = DEFAULT_CHUNK_SIZE) -> list[dict]:
    paths = sorted((Path(path).resolve() for path in paths), key=str)
    if not needles:
        raise AuditError(
            "authenticated cache produced zero high-entropy probes; absence is unprovable")
    automaton = AhoCorasick([needle.data for needle in needles])
    report_kinds = list(AUDIT_KINDS)
    for needle in needles:
        for source in needle.sources:
            if source.kind not in report_kinds:
                report_kinds.append(source.kind)
    reports = []
    for path in paths:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            raw_matches = scan_stream(
                stream, automaton, chunk_size=chunk_size, digest=digest)
        matches = []
        counts = {kind: 0 for kind in report_kinds}
        keys_by_type = {kind: set() for kind in report_kinds}
        for offset, pattern_id in raw_matches:
            needle = needles[pattern_id]
            for source in needle.sources:
                counts[source.kind] += 1
                keys_by_type[source.kind].add(source.key)
                matches.append({
                    "offset": offset,
                    "type": source.kind,
                    "key": source.key,
                    "representation": source.representation,
                    "location": source.location,
                    "needle_sha256": hashlib.sha256(needle.data).hexdigest(),
                    "needle_bytes": len(needle.data),
                })
        matches.sort(key=lambda item: (
            item["offset"], item["type"], item["key"],
            item["representation"], item["location"]))
        display = (path.relative_to(root.resolve()).as_posix()
                   if root is not None and path.is_relative_to(root.resolve())
                   else str(path))
        reports.append({
            "path": display,
            "size": path.stat().st_size,
            "sha256": digest.hexdigest(),
            "match_counts": counts,
            "matched_keys": {
                kind: sorted(keys_by_type[kind]) for kind in report_kinds},
            "matches": matches,
        })
    return reports


def find_containers(staged: Path) -> list[Path]:
    staged = Path(staged).resolve()
    if not staged.is_dir():
        raise AuditError(f"staged directory does not exist: {staged}")
    result = [path for path in staged.rglob("*")
              if path.is_file() and path.suffix.casefold() in CONTAINER_SUFFIXES
              and "Paks" in path.relative_to(staged).parts]
    result.sort(key=lambda path: path.relative_to(staged).as_posix())
    if not result:
        raise AuditError(f"no staged Paks containers found under {staged}")
    return result


def audit(staged: Path, cache_dir: Path, report_path: Path | None = None, *,
          chunk_size: int = DEFAULT_CHUNK_SIZE) -> dict:
    staged = Path(staged).resolve()
    needles, needle_stats = build_needles(cache_dir)
    containers = find_containers(staged)
    container_reports = scan_files(
        containers, needles, root=staged, chunk_size=chunk_size)
    total_matches = sum(len(item["matches"]) for item in container_reports)
    report = {
        "format": REPORT_FORMAT,
        "staged": str(staged),
        "cache": str(Path(cache_dir).resolve()),
        **needle_stats,
        "containers": container_reports,
        "container_count": len(container_reports),
        "total_matches": total_matches,
        "verdict": "pass" if total_matches == 0 else "fail",
    }
    if report_path is not None:
        report_path = Path(report_path)
        report_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = report_path.with_name(report_path.name + ".tmp")
        temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        temporary.replace(report_path)
    return report


def self_test() -> bool:
    planted = bytes(range(32, 112))
    source = ProbeSource("mesh:self-test", "meshes", "packed-frames", "head")
    needle = Needle(planted, (source,))
    with tempfile.TemporaryDirectory(prefix="wot-audit-self-test-") as temporary:
        root = Path(temporary)
        dirty = root / "dirty.pak"
        clean = root / "clean.pak"
        dirty.write_bytes(b"prefix" * 19 + planted + b"suffix")
        clean.write_bytes(b"unrelated" * 80)
        dirty_report = scan_files([dirty], [needle], chunk_size=31)[0]
        clean_report = scan_files([clean], [needle], chunk_size=31)[0]
        return len(dirty_report["matches"]) == 1 and not clean_report["matches"]


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--staged", type=Path,
                        help="staged build root containing **/Paks containers")
    result.add_argument("--cache", type=Path,
                        help="authenticated payload-cache directory")
    result.add_argument("--report", type=Path,
                        help="deterministic JSON report path (stdout if omitted)")
    result.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE,
                        help=argparse.SUPPRESS)
    result.add_argument("--self-test", action="store_true",
                        help="plant one needle in scratch data and prove detection")
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if args.self_test:
        passed = self_test()
        print(json.dumps({"self_test": "pass" if passed else "fail"}, sort_keys=True))
        return 0 if passed else 1
    if args.staged is None or args.cache is None:
        parser().error("--staged and --cache are required unless --self-test is used")
    try:
        report = audit(
            args.staged, args.cache, args.report, chunk_size=args.chunk_size)
    except (AuditError, cache.CacheError, OSError, ValueError) as exc:
        print(f"AUDIT-STRIPPED-PAK ERROR: {exc}", file=sys.stderr)
        return 2
    if args.report is None:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"AUDIT-STRIPPED-PAK {report['verdict'].upper()} "
              f"containers={report['container_count']} matches={report['total_matches']} "
              f"report={args.report}")
    return 0 if report["verdict"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
