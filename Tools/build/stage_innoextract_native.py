#!/usr/bin/env python3
"""Verify, prepare, promote, and fixture-test the pinned innoextract build.

This tool is deliberately stdlib-only.  Native builds consume only a verified
archive plus a verified patch, and promotion accepts only the two fixed artifact
names used by UBT.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import tarfile
import tempfile
from typing import Iterable, Sequence
import zlib


PINNED_ARCHIVE_SHA256 = (
    "6344a69fc1ed847d4ed3e272e0da5998948c6b828cb7af39c6321aba6cf88126"
)
MANIFEST_FORMAT = "wot-innoextract-vendor-manifest-v1"
RESOLVED_DEPENDENCIES_FORMAT = "wot-innoextract-resolved-dependencies-v1"
FIXTURE_FORMAT = "wot-innoextract-5.5.6-fixture-v1"
# Recorded in place of a version for a dependency the build links nothing for on
# a platform: Win64 reaches the Win32 codepage API directly instead of linking
# iconv. The build recipe emits this same literal, and pairs it with empty
# include/library lists and a null content digest.
ABSENT_DEPENDENCY_VERSION = "Win32-codepage"
VERSION_BANNER = b"Inno Setup Setup Data (5.5.6)"
VERSION_BANNER_STORED = VERSION_BANNER.ljust(64, b"\0")
LOADER_MAGIC = b"rDlPtS07\x87eVx"  # loader/offsets.cpp known_setup_loader_versions
CHUNK_MAGIC = b"zlb\x1a"  # stream/chunk.cpp chunk_id
MAX_PATCHED_TEXT_BYTES = 16 * 1024 * 1024


class VendorError(ValueError):
    """A deterministic source or artifact validation failure."""


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def _canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode(
        "ascii"
    )


def _read_manifest(path: Path) -> dict:
    try:
        raw = path.read_bytes()
        value = json.loads(raw)
    except (OSError, json.JSONDecodeError) as exc:
        raise VendorError(f"could not read vendor manifest {path}: {exc}") from exc
    if not isinstance(value, dict) or value.get("format") != MANIFEST_FORMAT:
        raise VendorError("vendor manifest has the wrong format")
    if raw != _canonical_json(value):
        raise VendorError("vendor manifest is not canonical JSON")
    return value


def _require_sha256(value: object, field: str, *, nullable: bool = False) -> None:
    if value is None and nullable:
        return
    if not isinstance(value, str) or len(value) != 64:
        raise VendorError(f"{field} must be a SHA-256 hex digest")
    try:
        int(value, 16)
    except ValueError as exc:
        raise VendorError(f"{field} must be lowercase hexadecimal") from exc
    if value.lower() != value:
        raise VendorError(f"{field} must be lowercase hexadecimal")


def _validate_null_policy(value: object, path: tuple[str, ...] = ()) -> None:
    if value is None:
        allowed = path in {
            ("artifacts", "libraries", "Mac", "sha256"),
            ("artifacts", "libraries", "Win64", "sha256"),
        }
        allowed = allowed or (
            len(path) == 4
            and path[0:2] == ("native_inputs", "Win64")
            and path[3] in {"version", "content_sha256"}
        )
        if not allowed:
            raise VendorError(f"null is forbidden at {'.'.join(path)}")
        return
    if isinstance(value, dict):
        for key, child in value.items():
            _validate_null_policy(child, path + (str(key),))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _validate_null_policy(child, path + (str(index),))


def _safe_member_name(name: str) -> str:
    normalized = name[:-1] if name.endswith("/") else name
    path = PurePosixPath(normalized)
    if not normalized or path.is_absolute() or ".." in path.parts:
        raise VendorError(f"unsafe archive member: {name!r}")
    if not path.parts or path.parts[0] != "innoextract-1.9":
        raise VendorError(f"archive member is outside innoextract-1.9/: {name!r}")
    return name


def archive_inventory(archive: Path) -> list[str]:
    try:
        with tarfile.open(archive, "r:gz") as source:
            result = []
            for member in source.getmembers():
                result.append(_safe_member_name(member.name))
                if member.issym() or member.islnk() or member.isdev() or member.isfifo():
                    raise VendorError(f"archive has a forbidden special member: {member.name}")
            return result
    except (OSError, tarfile.TarError) as exc:
        raise VendorError(f"could not inventory {archive}: {exc}") from exc


def _extract_archive(archive: Path, destination: Path) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as source:
        for member in source.getmembers():
            _safe_member_name(member.name)
            if member.issym() or member.islnk() or member.isdev() or member.isfifo():
                raise VendorError(f"archive has a forbidden special member: {member.name}")
            target = destination.joinpath(*PurePosixPath(member.name).parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            if not member.isfile():
                raise VendorError(f"unsupported archive member: {member.name}")
            target.parent.mkdir(parents=True, exist_ok=True)
            extracted = source.extractfile(member)
            if extracted is None:
                raise VendorError(f"could not extract {member.name}")
            with target.open("wb") as output:
                shutil.copyfileobj(extracted, output)
            os.chmod(target, member.mode & 0o777)
    return destination / "innoextract-1.9"


def _parse_hunk_range(text: str) -> tuple[int, int]:
    # Unified ranges are "start,count"; an omitted count means one line.
    body = text[1:]
    if "," in body:
        start, count = body.split(",", 1)
        return int(start), int(count)
    return int(body), 1


def apply_unified_patch(source_root: Path, patch_path: Path) -> list[str]:
    """Apply the small, ordinary unified diff used by this vendor package."""

    lines = patch_path.read_text(encoding="utf-8").splitlines(keepends=True)
    index = 0
    touched: list[str] = []
    while index < len(lines):
        if not lines[index].startswith("diff --git "):
            index += 1
            continue
        index += 1
        while index < len(lines) and not lines[index].startswith("--- "):
            index += 1
        if index + 1 >= len(lines) or not lines[index + 1].startswith("+++ "):
            raise VendorError("patch is missing a ---/+++ file header")
        old_name = lines[index][4:].strip().split("\t", 1)[0]
        new_name = lines[index + 1][4:].strip().split("\t", 1)[0]
        index += 2
        relative = new_name[2:] if new_name.startswith("b/") else new_name
        if new_name == "/dev/null":
            raise VendorError("deleting upstream files is forbidden")
        target_parts = PurePosixPath(relative).parts
        if not target_parts or ".." in target_parts:
            raise VendorError(f"unsafe patch target: {relative}")
        target = source_root.joinpath(*target_parts)
        if old_name == "/dev/null":
            original: list[str] = []
        else:
            if not target.is_file() or target.stat().st_size > MAX_PATCHED_TEXT_BYTES:
                raise VendorError(f"missing or oversized patch input: {relative}")
            original = target.read_text(encoding="utf-8").splitlines(keepends=True)

        output: list[str] = []
        cursor = 0
        saw_hunk = False
        while index < len(lines) and not lines[index].startswith("diff --git "):
            if not lines[index].startswith("@@ "):
                index += 1
                continue
            saw_hunk = True
            header = lines[index].split()
            if len(header) < 3:
                raise VendorError(f"malformed hunk header for {relative}")
            old_start, _old_count = _parse_hunk_range(header[1])
            old_index = max(old_start - 1, 0)
            if old_index < cursor:
                raise VendorError(f"overlapping hunks for {relative}")
            output.extend(original[cursor:old_index])
            cursor = old_index
            index += 1
            while index < len(lines):
                patch_line = lines[index]
                if patch_line.startswith("@@ ") or patch_line.startswith("diff --git "):
                    break
                if patch_line.startswith("\\ No newline at end of file"):
                    index += 1
                    continue
                if not patch_line or patch_line[0] not in " +-":
                    index += 1
                    continue
                marker, content = patch_line[0], patch_line[1:]
                if marker in " -":
                    if cursor >= len(original) or original[cursor] != content:
                        raise VendorError(
                            f"patch context mismatch in {relative} at source line {cursor + 1}"
                        )
                    cursor += 1
                if marker in " +":
                    output.append(content)
                index += 1
        if not saw_hunk:
            raise VendorError(f"patch contains no hunks for {relative}")
        output.extend(original[cursor:])
        target.parent.mkdir(parents=True, exist_ok=True)
        # Not Path.write_text(newline=...): that keyword needs Python 3.10 and
        # the Mac host's CMake-discovered interpreter is the 3.9 from Xcode
        # CommandLineTools. open() has accepted newline forever.
        with target.open("w", encoding="utf-8", newline="") as handle:
            handle.write("".join(output))
        touched.append(relative)
    if not touched:
        raise VendorError("patch contains no files")
    if len(touched) != len(set(touched)):
        raise VendorError("patch lists one source file more than once")
    return touched


def prepare_source(archive: Path, patch: Path, destination: Path) -> list[str]:
    if destination.exists():
        shutil.rmtree(destination)
    extracted = _extract_archive(archive, destination)
    return apply_unified_patch(extracted, patch)


def _manifest_root(manifest_path: Path) -> Path:
    return manifest_path.resolve().parent


def _validate_manifest_files(manifest: dict, manifest_path: Path) -> None:
    root = _manifest_root(manifest_path)
    for field in ("license", "readme", "build_recipe", "patch"):
        record = manifest.get(field)
        if not isinstance(record, dict):
            raise VendorError(f"manifest.{field} must be an object")
        relative = record.get("path")
        _require_sha256(record.get("sha256"), f"manifest.{field}.sha256")
        if not isinstance(relative, str):
            raise VendorError(f"manifest.{field}.path must be a string")
        actual = _sha256_file(root / relative)
        if actual != record["sha256"]:
            raise VendorError(f"manifest hash mismatch for {relative}")

    toolchains = manifest.get("toolchains")
    if not isinstance(toolchains, dict) or set(toolchains) != {"Mac", "Win64"}:
        raise VendorError("manifest.toolchains must contain exactly Mac and Win64")
    for platform, record in toolchains.items():
        if not isinstance(record, dict) or not isinstance(record.get("path"), str):
            raise VendorError(f"manifest.toolchains.{platform} is invalid")
        _require_sha256(record.get("sha256"), f"manifest.toolchains.{platform}.sha256")
        if _sha256_file(root / record["path"]) != record["sha256"]:
            raise VendorError(f"toolchain hash mismatch for {platform}")

    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict):
        raise VendorError("manifest.artifacts must be an object")
    header = artifacts.get("header")
    if not isinstance(header, dict) or not isinstance(header.get("path"), str):
        raise VendorError("manifest.artifacts.header is invalid")
    _require_sha256(header.get("sha256"), "manifest.artifacts.header.sha256")
    if _sha256_file(root / header["path"]) != header["sha256"]:
        raise VendorError("adapter header hash mismatch")
    libraries = artifacts.get("libraries")
    if not isinstance(libraries, dict) or set(libraries) != {"Mac", "Win64"}:
        raise VendorError("manifest libraries must contain exactly Mac and Win64")
    for platform, record in libraries.items():
        if not isinstance(record, dict) or not isinstance(record.get("path"), str):
            raise VendorError(f"manifest library record is invalid for {platform}")
        _require_sha256(
            record.get("sha256"), f"manifest libraries {platform}", nullable=True
        )
        # The library is this recipe's OUTPUT, so its bytes do not gate source
        # verification: a checkout without the library (the public tree) must
        # still be able to build it, and a contributor's own build will not
        # match the maintainer pin. verify_library holds a placed copy to it.

    inputs = manifest.get("native_inputs")
    expected_inputs = {"boost", "liblzma", "zlib", "bzip2", "iconv"}
    if not isinstance(inputs, dict) or set(inputs) != {"Mac", "Win64"}:
        raise VendorError("native_inputs must contain exactly Mac and Win64")
    for platform, platform_inputs in inputs.items():
        if not isinstance(platform_inputs, dict) or set(platform_inputs) != expected_inputs:
            raise VendorError(
                f"native_inputs.{platform} must pin boost/liblzma/zlib/bzip2/iconv"
            )
        # Pinned inputs and a promoted library stand or fall together: a platform
        # that has not been built yet records nothing, and one whose library ships
        # can never sit on unrecorded dependencies.
        promoted = libraries[platform]["sha256"] is not None
        for name, record in platform_inputs.items():
            if not isinstance(record, dict) or set(record) != {"version", "content_sha256"}:
                raise VendorError(f"native input {platform}.{name} is invalid")
            if promoted:
                if not isinstance(record["version"], str):
                    raise VendorError(f"native input {platform}.{name} has no version")
            elif record["version"] is not None:
                raise VendorError(f"unbuilt {platform} native input {name} must remain null")
            _require_sha256(
                record["content_sha256"],
                f"native_inputs.{platform}.{name}.content_sha256",
                nullable=not promoted or record["version"] == ABSENT_DEPENDENCY_VERSION,
            )

    flags = manifest.get("compiler_flags")
    if not isinstance(flags, dict) or set(flags) != {"Mac", "Win64"}:
        raise VendorError("compiler_flags must contain exactly Mac and Win64")
    if any(not isinstance(values, list) or not values for values in flags.values()):
        raise VendorError("every platform must pin a non-empty compiler flag list")


def verify_library(platform: str, manifest_path: Path) -> str:
    """Hold a placed platform library to its manifest pin; return its digest."""
    manifest = _read_manifest(manifest_path)
    _validate_null_policy(manifest)
    _validate_manifest_files(manifest, manifest_path)
    record = manifest["artifacts"]["libraries"][platform]
    if record["sha256"] is None:
        raise VendorError(f"no {platform} library is pinned in the manifest")
    library_path = _manifest_root(manifest_path) / record["path"]
    if not library_path.is_file():
        raise VendorError(f"the {platform} library is missing: {library_path}")
    actual = _sha256_file(library_path)
    if actual != record["sha256"]:
        raise VendorError(
            f"the {platform} library differs from its manifest pin "
            "(a contributor build or a stale copy, not the reviewed artifact)"
        )
    return actual


def verify_source(
    archive: Path,
    expected_sha256: str,
    patch: Path,
    manifest_path: Path,
) -> dict:
    manifest = _read_manifest(manifest_path)
    _validate_null_policy(manifest)
    _require_sha256(expected_sha256, "--sha256")
    if expected_sha256 != PINNED_ARCHIVE_SHA256:
        raise VendorError("the command did not request the pinned innoextract archive")
    actual_archive_hash = _sha256_file(archive)
    if actual_archive_hash != expected_sha256:
        raise VendorError(
            f"archive SHA-256 mismatch: expected {expected_sha256}, got {actual_archive_hash}"
        )
    archive_record = manifest.get("archive")
    if not isinstance(archive_record, dict):
        raise VendorError("manifest.archive must be an object")
    if archive_record.get("sha256") != expected_sha256:
        raise VendorError("manifest does not bind the pinned archive digest")
    inventory = archive_inventory(archive)
    if inventory != archive_record.get("inventory"):
        raise VendorError("archive inventory differs from the manifest")
    if archive_record.get("entry_count") != len(inventory):
        raise VendorError("archive entry_count differs from its inventory")
    if len(inventory) != 150:
        raise VendorError(f"the pinned archive must contain 150 entries, found {len(inventory)}")

    manifest_patch = manifest.get("patch", {})
    if patch.resolve() != (_manifest_root(manifest_path) / manifest_patch.get("path", "")).resolve():
        raise VendorError("--patch is not the manifest-owned patch")
    _validate_manifest_files(manifest, manifest_path)

    patched_records = manifest.get("patched_sources")
    if not isinstance(patched_records, list) or not patched_records:
        raise VendorError("manifest.patched_sources must be a non-empty list")
    with tempfile.TemporaryDirectory(prefix="wot-inno-verify-") as temporary:
        destination = Path(temporary) / "source"
        touched = prepare_source(archive, patch, destination)
        expected_paths = [record.get("path") for record in patched_records]
        if touched != expected_paths:
            raise VendorError("patch file order/path list differs from patched_sources")
        source_root = destination / "innoextract-1.9"
        for index, record in enumerate(patched_records):
            if not isinstance(record, dict) or not isinstance(record.get("path"), str):
                raise VendorError(f"patched_sources[{index}] is invalid")
            _require_sha256(record.get("sha256"), f"patched_sources[{index}].sha256")
            actual = _sha256_file(source_root / record["path"])
            if actual != record["sha256"]:
                raise VendorError(f"post-patch hash mismatch for {record['path']}")
    return manifest


def _pack_string(value: bytes) -> bytes:
    # util/load.hpp util::binary_string: uint32 byte length followed by bytes.
    return struct.pack("<I", len(value)) + value


def _windows_version_range() -> bytes:
    # setup/windows.cpp windows_version_range::load: begin/end; each endpoint is
    # two {uint16 build,uint8 minor,uint8 major} values plus two service-pack bytes.
    return bytes(20)


def _file_entry(
    destination: bytes,
    location: int,
    *,
    after_install: bytes = b"",
    before_install: bytes = b"",
) -> bytes:
    value = bytearray()
    # setup/file.cpp file_entry::load: source, destination, font, strong assembly.
    value += _pack_string(b"source.bin")
    value += _pack_string(destination)
    value += _pack_string(b"")
    value += _pack_string(b"")
    # setup/item.cpp item::load_condition_data: components/tasks/languages/check,
    # then after-install and before-install for 5.5.6.
    for field in (b"", b"", b"", b"", after_install, before_install):
        value += _pack_string(field)
    # setup/item.hpp item::load_version_data -> windows_version_range::load.
    value += _windows_version_range()
    # setup/file.cpp: location, attributes, external_size, permission.
    value += struct.pack("<IIQh", location, 0, 0, -1)
    # setup/file.cpp stored_flag_reader<file_entry::flags>: 32 flags in 5.5.6.
    value += bytes(4)
    # setup/file.cpp stored_file_type_0: 0 is UserFile.
    value += b"\0"
    return bytes(value)


def _data_entry(
    *,
    chunk_offset: int,
    uncompressed: bytes,
    stored_chunk: bytes,
    compressed: bool,
    declared_size: int | None = None,
    file_offset: int = 0,
    chunk_size: int | None = None,
) -> bytes:
    value = bytearray()
    # setup/data.cpp data_entry::load: first/last slice, chunk offset, file offset,
    # file size (decoded bytes), and chunk size (bytes after stream/chunk.cpp magic).
    file_size = len(uncompressed) if declared_size is None else declared_size
    value += struct.pack(
        "<IIIQQQ",
        0,
        0,
        chunk_offset,
        file_offset,
        file_size,
        len(stored_chunk) if chunk_size is None else chunk_size,
    )
    # setup/data.cpp (5.3.9+): SHA-1 of the decoded file.
    value += hashlib.sha1(uncompressed).digest()
    # setup/data.cpp: Win32 FILETIME, file-version MS/LS.
    value += struct.pack("<qII", 0x019DB1DED53E8000, 0, 0)
    # setup/data.cpp stored_flag_reader<data_entry::flags>: ChunkCompressed is the
    # eighth stored bit for 5.5.6; the ninth bit makes this a two-byte field.
    value += bytes((0x80 if compressed else 0, 0))
    return bytes(value)


def _main_header(
    destinations: Sequence[bytes],
    *,
    compression: int = 1,
    declared_file_count: int | None = None,
    first_header_string: bytes = b"WoT fixture",
    publisher: bytes = b"WoT",
    after_installs: Sequence[bytes] | None = None,
    before_installs: Sequence[bytes] | None = None,
) -> bytes:
    value = bytearray()
    # setup/header.cpp header::load, in exact 5.5.6 field order.  Each item here is
    # util::binary_string (ansi_string has the same stored representation).
    strings = [
        first_header_string, b"WoT fixture 1.0", b"wot.fixture", b"",
        publisher, b"", b"", b"", b"", b"1.0", b"{app}", b"",
        b"setup", b"", b"", b"", b"", b"", b"", b"", b"",
        b"", b"", b"", b"", b"", b"", b"", b"", b"", b"", b"",
    ]
    if len(strings) != 32:
        raise AssertionError("5.5.6 header string field count drifted")
    for field in strings:
        value += _pack_string(field)
    # setup/header.cpp stored_char_set for ANSI installers: one bit per byte.
    value += bytes(32)
    # setup/header.cpp count fields: languages/messages/permissions/types/
    # components/tasks/directories/files/data then icon/ini/registry/delete/run sets.
    file_count = len(destinations) if declared_file_count is None else declared_file_count
    counts = [0, 0, 0, 0, 0, 0, 0, file_count, len(destinations), 0, 0, 0, 0, 0, 0, 0]
    value += struct.pack("<" + "I" * len(counts), *counts)
    # setup/header.cpp windows_version_range, colors, SHA-1 password and salt,
    # extra disk space and slices-per-disk.
    value += _windows_version_range()
    value += struct.pack("<III", 0, 0, 0)
    value += bytes(20 + 8)
    value += struct.pack("<qI", 0, 1)
    # setup/header.cpp enums: uninstall log, dir warning, privilege, language
    # dialog/detection, compression. Compression value 1 is stream::Zlib.
    value += bytes((1, 0, 0, 0, 0, compression))
    # setup/header.cpp stored_architectures_0 twice, disable-dir/program enums,
    # uninstall display size, then 47 header flags packed into six bytes.
    value += bytes((0x06, 0x06, 0, 0))
    value += struct.pack("<Q", 0)
    value += bytes(6)
    if after_installs is None:
        after_installs = (b"",) * len(destinations)
    if before_installs is None:
        before_installs = (b"",) * len(destinations)
    if len(after_installs) != len(destinations) or len(before_installs) != len(destinations):
        raise VendorError("fixture install-script counts must match destinations")
    for location, destination in enumerate(destinations):
        value += _file_entry(
            destination,
            location,
            after_install=after_installs[location],
            before_install=before_installs[location],
        )
    # setup/info.cpp load_wizard_and_decompressor: 5.5.6 stores the big and
    # small wizard images for every compression method.  Only Zlib stores the
    # additional decompressor DLL blob; emitting it for LZMA leaves four bytes
    # unconsumed at the end of the primary header stream.
    value += _pack_string(b"") * 2
    if compression == 1:
        value += _pack_string(b"")
    return bytes(value)


def _stored_block(payload: bytes) -> bytes:
    # stream/block.cpp block_reader::get (4.0.9+): CRC32 over uint32 stored_size
    # plus uint8 compressed flag, followed by CRC32+data subblocks (max 4096).
    chunks = [payload[index : index + 4096] for index in range(0, len(payload), 4096)]
    stored = b"".join(struct.pack("<I", binascii.crc32(chunk) & 0xFFFFFFFF) + chunk for chunk in chunks)
    block_header = struct.pack("<IB", len(stored), 0)
    return struct.pack("<I", binascii.crc32(block_header) & 0xFFFFFFFF) + block_header + stored


def _compressed_fixture_chunk(
    compression: str, payload: bytes, lzma_property: int | None
) -> tuple[bytes, int]:
    if compression == "zlib":
        return zlib.compress(payload, level=9), 1
    if compression == "lzma1":
        selected_size = 64 * 1024 * 1024 if lzma_property is None else lzma_property
        raw = lzma.compress(
            payload,
            format=lzma.FORMAT_RAW,
            filters=[{"id": lzma.FILTER_LZMA1, "dict_size": 1 * 1024 * 1024}],
        )
        # stream/lzma.cpp inno_lzma1_decompressor_impl: one lc/lp/pb property
        # byte (0x5d = lc3/lp0/pb2), then an arbitrary little-endian dict_size.
        return b"\x5d" + struct.pack("<I", selected_size) + raw, 3
    if compression == "lzma2":
        selected_property = 28 if lzma_property is None else lzma_property
        raw = lzma.compress(
            payload,
            format=lzma.FORMAT_RAW,
            filters=[{"id": lzma.FILTER_LZMA2, "dict_size": 1 * 1024 * 1024}],
        )
        # stream/lzma.cpp inno_lzma2_decompressor_impl: property 28 decodes to
        # exactly 64 MiB; 29 is the first representable value above the cap.
        return bytes((selected_property,)) + raw, 4
    raise VendorError(f"unsupported fixture compression: {compression}")


def generate_inno_fixture(
    *,
    duplicate_normalized_path: bool = False,
    destinations: Sequence[bytes] | None = None,
    compression: str = "zlib",
    lzma_property: int | None = None,
    declared_sizes: Sequence[int] | None = None,
    declared_file_count: int | None = None,
    first_header_string: bytes = b"WoT fixture",
    galaxy_multipart: bool = False,
) -> bytes:
    """Emit a deterministic embedded-data Inno Setup 5.5.6 installer image."""

    stored_data = b"stored fixture bytes\n"
    compressed_data = (b"zlib fixture bytes " * 16) + b"\n"
    compressed_chunk, compression_value = _compressed_fixture_chunk(
        compression, compressed_data, lzma_property
    )
    chunks = [stored_data, compressed_chunk]
    publisher = b"WoT"
    after_installs: Sequence[bytes] | None = None
    before_installs: Sequence[bytes] | None = None
    data_payloads = [stored_data, compressed_data]
    chunk_compressed = [False, True]
    if destinations is None:
        destinations = [
            b"{app}\\Data\\Stored.txt",
            (b"{app}\\data/stored.txt" if duplicate_normalized_path
             else b"{app}\\Data\\Compressed.txt"),
        ]
    if galaxy_multipart:
        galaxy_parts = [b"galaxy part one\n", b"galaxy part two\n"]
        chunks = [zlib.compress(part, level=9) for part in galaxy_parts]
        data_payloads = list(chunks)
        chunk_compressed = [False, False]
        compression_value = 0
        destinations = (
            b"{tmp}\\aa\\first.part",
            b"{tmp}\\bb\\second.part",
        )
        whole_checksum = hashlib.md5(b"".join(galaxy_parts)).hexdigest().encode("ascii")
        before_installs = (
            b"before_install('" + whole_checksum
            + b"','Data\\Galaxy.txt','2')",
            b"",
        )
        after_installs = tuple(
            b"after_install('"
            + hashlib.md5(part).hexdigest().encode("ascii")
            + b"','"
            + str(len(chunk)).encode("ascii")
            + b"','"
            + str(len(part)).encode("ascii")
            + b"')"
            for part, chunk in zip(galaxy_parts, chunks)
        )
        publisher = b"GOG.com"
    if len(destinations) != 2:
        raise VendorError("the compact fixture requires exactly two destinations")
    if declared_sizes is None:
        declared_sizes = tuple(len(payload) for payload in data_payloads)
    if len(declared_sizes) != 2 or any(size < 0 or size > 0xFFFFFFFFFFFFFFFF
                                      for size in declared_sizes):
        raise VendorError("fixture declared sizes must contain two uint64 values")
    primary = _main_header(
        destinations,
        compression=compression_value,
        declared_file_count=declared_file_count,
        first_header_string=first_header_string,
        publisher=publisher,
        after_installs=after_installs,
        before_installs=before_installs,
    )
    first_chunk_size = len(CHUNK_MAGIC) + len(chunks[0])
    if galaxy_multipart:
        solid_chunk = b"".join(chunks)
        secondary = b"".join(
            (
                _data_entry(
                    chunk_offset=0,
                    uncompressed=data_payloads[0],
                    stored_chunk=solid_chunk,
                    compressed=False,
                    declared_size=declared_sizes[0],
                    chunk_size=len(solid_chunk),
                ),
                _data_entry(
                    chunk_offset=0,
                    uncompressed=data_payloads[1],
                    stored_chunk=solid_chunk,
                    compressed=False,
                    declared_size=declared_sizes[1],
                    file_offset=len(chunks[0]),
                    chunk_size=len(solid_chunk),
                ),
            )
        )
        data = CHUNK_MAGIC + solid_chunk
    else:
        secondary = b"".join(
            (
                _data_entry(
                    chunk_offset=0,
                    uncompressed=data_payloads[0],
                    stored_chunk=chunks[0],
                    compressed=chunk_compressed[0],
                    declared_size=declared_sizes[0],
                ),
                _data_entry(
                    chunk_offset=first_chunk_size,
                    uncompressed=data_payloads[1],
                    stored_chunk=chunks[1],
                    compressed=chunk_compressed[1],
                    declared_size=declared_sizes[1],
                ),
            )
        )
        data = CHUNK_MAGIC + chunks[0] + CHUNK_MAGIC + chunks[1]
    setup_headers = VERSION_BANNER_STORED + _stored_block(primary) + _stored_block(secondary)
    header_offset = 0x100
    data_offset = header_offset + len(setup_headers)

    # loader/offsets.cpp offsets::load_from_exe_file: fixed "Inno" pointer at
    # 0x30, followed by the pointer complement. The pointed-to 4.1.6 loader table
    # remains valid for an embedded 5.5.6 setup-data stream.
    loader_offset = 0x80
    image = bytearray(max(header_offset, loader_offset + 40))
    image[0:2] = b"MZ"  # loader/exereader.cpp determine_binary_type DOS magic.
    struct.pack_into("<III", image, 0x30, 0x6F6E6E49, loader_offset, (~loader_offset) & 0xFFFFFFFF)
    loader_values = struct.pack("<IIIIII", 0, 0, 0, 0, header_offset, data_offset)
    loader_body = LOADER_MAGIC + loader_values
    loader_table = loader_body + struct.pack("<I", binascii.crc32(loader_body) & 0xFFFFFFFF)
    image[loader_offset : loader_offset + len(loader_table)] = loader_table
    image[header_offset:header_offset] = setup_headers + data
    return bytes(image)


FIXTURE_VARIANTS: dict[str, dict[str, object]] = {
    "standard": {},
    "duplicate-folded": {"duplicate_normalized_path": True},
    "duplicate-exact": {
        "destinations": (b"{app}\\Data\\Stored.txt", b"{app}\\Data\\Stored.txt")
    },
    "constant-tmp": {
        "destinations": (b"{tmp}\\Data\\Stored.txt", b"{app}\\Data\\Compressed.txt")
    },
    "constant-sys": {
        "destinations": (b"{sys}\\Data\\Stored.txt", b"{app}\\Data\\Compressed.txt")
    },
    "galaxy-multipart": {"galaxy_multipart": True},
    "bare-app": {
        "destinations": (b"{app}", b"{app}\\Data\\Compressed.txt")
    },
    "nested-app": {
        "destinations": (b"{app}\\{app}\\Stored.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-angle": {
        "destinations": (b"{app}\\Data\\Bad<Name.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-angle-close": {
        "destinations": (b"{app}\\Data\\Bad>Name.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-colon": {
        "destinations": (b"{app}\\Data\\Bad:Name.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-quote": {
        "destinations": (b"{app}\\Data\\Bad\"Name.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-pipe": {
        "destinations": (b"{app}\\Data\\Bad|Name.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-question": {
        "destinations": (b"{app}\\Data\\Bad?Name.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-star": {
        "destinations": (b"{app}\\Data\\Bad*Name.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-control": {
        "destinations": (b"{app}\\Data\\Bad\x1fName.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-trailing-dot": {
        "destinations": (b"{app}\\Data\\Bad.", b"{app}\\Data\\Compressed.txt")
    },
    "portable-trailing-space": {
        "destinations": (b"{app}\\Data\\Bad ", b"{app}\\Data\\Compressed.txt")
    },
    "portable-reserved-exact": {
        "destinations": (b"{app}\\Data\\CON.txt", b"{app}\\Data\\Compressed.txt")
    },
    "portable-reserved-folded": {
        "destinations": (b"{app}\\Data\\lPt9.bin", b"{app}\\Data\\Compressed.txt")
    },
    "path-over-limit": {
        "destinations": (b"{app}\\" + b"A" * 4091, b"{app}\\Data\\Compressed.txt")
    },
    "entry-count-over": {"declared_file_count": 65537},
    "accumulation-overflow": {
        "declared_sizes": (0xFFFFFFFFFFFFFFDF, 64)
    },
    "lzma1-boundary": {"compression": "lzma1", "lzma_property": 64 * 1024 * 1024},
    "lzma1-over": {"compression": "lzma1", "lzma_property": 64 * 1024 * 1024 + 1},
    "lzma2-boundary": {"compression": "lzma2", "lzma_property": 28},
    "lzma2-over": {"compression": "lzma2", "lzma_property": 29},
}


def fixture_metadata(data: bytes) -> dict[str, object]:
    if len(data) < 0x100 + 64 or data[0:2] != b"MZ":
        raise VendorError("fixture is not the expected installer image")
    magic, loader_offset, complement = struct.unpack_from("<III", data, 0x30)
    if magic != 0x6F6E6E49 or complement != ((~loader_offset) & 0xFFFFFFFF):
        raise VendorError("fixture loader pointer is invalid")
    if data[loader_offset : loader_offset + 12] != LOADER_MAGIC:
        raise VendorError("fixture loader magic is invalid")
    fields = struct.unpack_from("<IIIIII", data, loader_offset + 12)
    header_offset, data_offset = fields[4], fields[5]
    if data[header_offset : header_offset + 64] != VERSION_BANNER_STORED:
        raise VendorError("fixture setup-data version is invalid")
    return {
        "format": FIXTURE_FORMAT,
        "header_offset": header_offset,
        "data_offset": data_offset,
        "size": len(data),
        "sha256": _sha256_bytes(data),
    }


def _atomic_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temporary:
        temporary_path = Path(temporary.name)
        with source.open("rb") as input_file:
            shutil.copyfileobj(input_file, temporary)
        temporary.flush()
        os.fsync(temporary.fileno())
    os.replace(temporary_path, destination)


def _include_tree_digest(root: Path) -> str:
    if not root.is_dir():
        raise VendorError(f"resolved include tree is missing: {root}")
    records: list[dict[str, str]] = []
    for path in sorted((item for item in root.rglob("*") if item.is_file()),
                       key=lambda item: item.relative_to(root).as_posix()):
        records.append({
            "path": path.relative_to(root).as_posix(),
            "sha256": _sha256_file(path),
        })
    if not records:
        raise VendorError(f"resolved include tree is empty: {root}")
    return _sha256_bytes(_canonical_json(records))


def resolved_dependency_content_hash(record: dict) -> str:
    libraries = record.get("library_paths")
    includes = record.get("include_paths")
    if not isinstance(libraries, list) or not libraries or not all(
        isinstance(path, str) for path in libraries
    ):
        raise VendorError("resolved dependency has no library paths")
    if not isinstance(includes, list) or not all(isinstance(path, str) for path in includes):
        raise VendorError("resolved dependency include paths are invalid")
    library_paths = [Path(path) for path in libraries]
    if any(not path.is_file() for path in library_paths):
        missing = next(path for path in library_paths if not path.is_file())
        raise VendorError(f"resolved dependency library is missing: {missing}")

    # SDK dependencies are content-bound to the linker interface file itself.
    if all(path.suffix == ".tbd" for path in library_paths):
        if len(library_paths) == 1:
            return _sha256_file(library_paths[0])
        records = sorted(_sha256_file(path) for path in library_paths)
        return _sha256_bytes(_canonical_json(records))

    # Homebrew dependencies bind every selected archive and a path-independent,
    # sorted digest of each complete include tree.
    content = {
        "include_trees": sorted(_include_tree_digest(Path(path)) for path in includes),
        "libraries": sorted(_sha256_file(path) for path in library_paths),
    }
    return _sha256_bytes(_canonical_json(content))


def _is_platform_absent_dependency(record: dict) -> bool:
    """A dependency the build links nothing for on this platform.

    Win64 reaches the Win32 codepage API directly instead of linking iconv, so
    the recipe emits empty include and library lists for it. That is distinct
    from a dependency that failed to resolve: the recipe states the absence.
    """
    return record.get("include_paths") == [] and record.get("library_paths") == []


def verify_resolved_dependencies(
    *, platform: str, resolved_path: Path, manifest: dict, check_content: bool = True
) -> None:
    """Hold the resolved native inputs to the manifest pins.

    check_content=False is the contributor build: versions must still match,
    but the dependency bytes are not compared. MSVC and vcpkg do not produce
    byte-identical libraries across machines, so only the maintainer box that
    recorded the pins can pass the content check.
    """
    try:
        resolved = json.loads(resolved_path.read_bytes())
    except (OSError, json.JSONDecodeError) as exc:
        raise VendorError(f"could not read resolved dependencies {resolved_path}: {exc}") from exc
    expected_names = {"boost", "liblzma", "zlib", "bzip2", "iconv"}
    if (not isinstance(resolved, dict)
            or resolved.get("format") != RESOLVED_DEPENDENCIES_FORMAT
            or resolved.get("platform") != platform
            or not isinstance(resolved.get("dependencies"), dict)
            or set(resolved["dependencies"]) != expected_names):
        raise VendorError("resolved-dependencies.json has the wrong contract")
    pins = manifest["native_inputs"][platform]
    for name in sorted(expected_names):
        record = resolved["dependencies"][name]
        if not isinstance(record, dict) or not isinstance(record.get("version"), str):
            raise VendorError(f"resolved dependency {name} has no version")
        if record["version"] != pins[name]["version"]:
            raise VendorError(f"resolved dependency version mismatch for {name}")
        if _is_platform_absent_dependency(record):
            # Nothing was linked, so there is no artifact to bind. The pin must
            # say exactly that with a null digest rather than carry a stale hash
            # -- and the version still has to match, so the platform cannot go
            # from linking a real iconv to linking none without re-pinning.
            if pins[name]["content_sha256"] is not None:
                raise VendorError(
                    f"resolved dependency {name} links nothing but its pin binds content"
                )
            continue
        if not check_content:
            continue
        actual = resolved_dependency_content_hash(record)
        if actual != pins[name]["content_sha256"]:
            raise VendorError(f"resolved dependency content mismatch for {name}")


def promote(
    *,
    platform: str,
    stage: Path,
    header_destination: Path,
    library_destination: Path,
    manifest_path: Path,
    resolved_dependencies: Path,
    check_input_content: bool = True,
    record_library_digest: bool = True,
) -> None:
    expected_library = {
        "Mac": "libwot_innoextract.a",
        "Win64": "wot_innoextract.lib",
    }[platform]
    if library_destination.name != expected_library:
        raise VendorError(f"{platform} library destination must be {expected_library}")
    if header_destination.as_posix().split("/")[-3:] != [
        "include",
        "wot",
        "innoextract_adapter.h",
    ]:
        raise VendorError("header destination must be include/wot/innoextract_adapter.h")

    stage_header = stage / "include" / "wot" / "innoextract_adapter.h"
    candidates = [stage / "lib" / expected_library, stage / "lib64" / expected_library]
    stage_library = next((candidate for candidate in candidates if candidate.is_file()), None)
    if not stage_header.is_file() or stage_library is None:
        raise VendorError("stage does not contain the exact header and static library")
    staged_files = {path.relative_to(stage).as_posix() for path in stage.rglob("*") if path.is_file()}
    expected_files = {
        "include/wot/innoextract_adapter.h",
        stage_library.relative_to(stage).as_posix(),
    }
    if staged_files != expected_files:
        raise VendorError("stage contains outputs other than the fixed header and static library")
    manifest = _read_manifest(manifest_path)
    _validate_null_policy(manifest)
    verify_resolved_dependencies(
        platform=platform,
        resolved_path=resolved_dependencies,
        manifest=manifest,
        check_content=check_input_content,
    )
    header_record = manifest["artifacts"]["header"]
    if _sha256_file(stage_header) != header_record["sha256"]:
        raise VendorError("staged adapter header differs from the manifest")

    _atomic_copy(stage_header, header_destination)
    _atomic_copy(stage_library, library_destination)
    if not record_library_digest:
        return
    library_digest = _sha256_file(library_destination)
    manifest["artifacts"]["libraries"][platform]["sha256"] = library_digest
    manifest_path.write_bytes(_canonical_json(manifest))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    verify = commands.add_parser("verify-source")
    verify.add_argument("--archive", type=Path, required=True)
    verify.add_argument("--sha256", required=True)
    verify.add_argument("--patch", type=Path, required=True)
    verify.add_argument("--vendor-manifest", type=Path, required=True)

    verify_library_parser = commands.add_parser("verify-library")
    verify_library_parser.add_argument("--platform", choices=("Mac", "Win64"), required=True)
    verify_library_parser.add_argument("--vendor-manifest", type=Path, required=True)

    prepare = commands.add_parser("prepare-source")
    prepare.add_argument("--archive", type=Path, required=True)
    prepare.add_argument("--patch", type=Path, required=True)
    prepare.add_argument("--destination", type=Path, required=True)

    promote_parser = commands.add_parser("promote")
    promote_parser.add_argument("--platform", choices=("Mac", "Win64"), required=True)
    promote_parser.add_argument("--stage", type=Path, required=True)
    promote_parser.add_argument("--header-destination", type=Path, required=True)
    promote_parser.add_argument("--library-destination", type=Path, required=True)
    promote_parser.add_argument("--vendor-manifest", type=Path, required=True)
    promote_parser.add_argument("--resolved-dependencies", type=Path, required=True)
    promote_parser.add_argument(
        "--versions-only", action="store_true",
        help="contributor build: match dependency versions, not their bytes")
    promote_parser.add_argument(
        "--no-record", action="store_true",
        help="leave vendor-manifest.json untouched (the library is not re-pinned)")

    fixture = commands.add_parser("generate-fixture")
    fixture.add_argument("--output", type=Path, required=True)
    fixture.add_argument("--duplicate-normalized-path", action="store_true")
    fixture.add_argument("--variant", choices=tuple(FIXTURE_VARIANTS), default="standard")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.command == "verify-source":
            verify_source(
                arguments.archive,
                arguments.sha256,
                arguments.patch,
                arguments.vendor_manifest,
            )
        elif arguments.command == "verify-library":
            digest = verify_library(arguments.platform, arguments.vendor_manifest)
            print(f"{arguments.platform} library matches its pin: {digest}")
        elif arguments.command == "prepare-source":
            prepare_source(arguments.archive, arguments.patch, arguments.destination)
        elif arguments.command == "promote":
            promote(
                platform=arguments.platform,
                stage=arguments.stage,
                header_destination=arguments.header_destination,
                library_destination=arguments.library_destination,
                manifest_path=arguments.vendor_manifest,
                resolved_dependencies=arguments.resolved_dependencies,
                check_input_content=not arguments.versions_only,
                record_library_digest=not arguments.no_record,
            )
        elif arguments.command == "generate-fixture":
            fixture_options = dict(FIXTURE_VARIANTS[arguments.variant])
            if arguments.duplicate_normalized_path:
                if arguments.variant != "standard":
                    raise VendorError("--duplicate-normalized-path cannot be combined with --variant")
                fixture_options["duplicate_normalized_path"] = True
            data = generate_inno_fixture(**fixture_options)
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_bytes(data)
            fixture_metadata(data)
        else:  # pragma: no cover - argparse closes this set.
            raise VendorError(f"unknown command: {arguments.command}")
    except (OSError, VendorError, KeyError) as exc:
        print(f"stage_innoextract_native: {exc}", file=os.sys.stderr)
        # A dependency-hash mismatch is the failure a newcomer is most likely
        # to hit, and on its own it reads as "the build is broken" when it
        # means "your dependency versions differ from the pinned ones". Name
        # the three paths so the next step is obvious.
        print(
            "\nThis tool guards the VENDOR MAINTENANCE path (rebuilding and\n"
            "promoting the pinned third-party library). It is NOT needed to\n"
            "build or run the game: ordinary development uses the installed\n"
            "engine plus UBT and consumes the committed\n"
            "ThirdParty/<pkg>/<ver>/lib/<Platform>/ artifact as-is.\n"
            "A dependency mismatch here means the versions this box resolved\n"
            "differ from the ones pinned in vendor-manifest.json — see\n"
            "docs/BUILD-PATHS.md for which path you want and how to re-pin\n"
            "deliberately if you are changing native inputs.",
            file=os.sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
