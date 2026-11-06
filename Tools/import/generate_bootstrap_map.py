#!/usr/bin/env python3
"""Author and verify the one payload-free guarded bootstrap world.

Generation is a no-replace operation.  Outside project Content the canonical
package is briefly authored at its real /Game name, copied to the sealed
output root, and removed again.  Verification is deliberately engine-free: it
parses the loose package summary, name table, and import map using only the
Python standard library.

PythonScriptCommandlet arguments belong inside the ``-script`` value::

    -script="/abs/generate_bootstrap_map.py --output-root /abs/out ..."
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable


PACKAGE_NAME = "/Game/WoT/Maps/WoTBootstrap"
GAME_MODE_PATH = "/Script/WoT.WoTBootstrapGameMode"
RELATIVE_MAP = Path("WoT/Maps/WoTBootstrap.umap")
RECEIPT_VERSION = 1
GENERATOR_IDENTITY = "generate-bootstrap-map-v1"
PACKAGE_FILE_TAG = 0x9E2A83C1
MAX_PACKAGE_BYTES = 64 * 1024 * 1024
PROJECT_ROOT = Path(__file__).resolve().parents[2]
DENIED_ROOT_NAMES = ("Content", "original", "devtree", "exports")
DENIED_PATH_CLASSES = {
    "content", "original", "devtree", "exports", "cache", "cache-v3",
    "installers", "generatedcontent", "generated-cache", "retail-cache",
}
DENIED_OUTPUT_PATH_CLASSES = DENIED_PATH_CLASSES - {"content", "generatedcontent"}
_AUDIT_INSTALLED = False
_AUDIT_POLICY: tuple[
    tuple[Path, ...], Path, Path, bool, set[Path], set[Path],
] | None = None


class GenerationFailed(RuntimeError):
    pass


class VerificationFailed(RuntimeError):
    pass


@dataclass(frozen=True)
class BootstrapMapIdentity:
    package: str
    game_mode: str
    relative_path: str
    bytes: int
    sha256: str
    import_packages: tuple[str, ...]


@dataclass(frozen=True)
class _CreatedPath:
    path: Path
    device: int
    inode: int
    is_directory: bool


class _CreatedPaths:
    """Identity-bound cleanup for paths created by this invocation only."""

    def __init__(self) -> None:
        self._paths: dict[Path, _CreatedPath] = {}

    def _record(self, path: Path, is_directory: bool) -> None:
        status = path.lstat()
        self._paths[path] = _CreatedPath(
            path, status.st_dev, status.st_ino, is_directory)

    def makedirs(self, path: Path) -> None:
        missing = []
        current = path
        while not current.exists():
            missing.append(current)
            if current == current.parent:
                break
            current = current.parent
        if not current.is_dir():
            raise GenerationFailed(f"path parent is not a directory: {current}")
        for directory in reversed(missing):
            try:
                os.mkdir(directory, 0o700)
            except FileExistsError:
                if not directory.is_dir():
                    raise GenerationFailed(
                        f"path parent appeared as a non-directory: {directory}")
            else:
                self._record(directory, True)

    def mkdir(self, path: Path) -> None:
        self.makedirs(path.parent)
        try:
            os.mkdir(path, 0o700)
        except FileExistsError as exc:
            raise GenerationFailed(
                f"output root appeared before atomic creation: {path}") from exc
        self._record(path, True)

    def record_file(self, path: Path) -> None:
        self._record(path, False)

    def owns(self, path: Path) -> bool:
        created = self._paths.get(path)
        if created is None:
            return False
        try:
            status = path.lstat()
        except FileNotFoundError:
            return False
        return (status.st_dev, status.st_ino) == (
            created.device, created.inode)

    def require_owned(self, path: Path, purpose: str) -> None:
        if not self.owns(path):
            raise GenerationFailed(
                f"{purpose} was replaced by a competing writer: {path}")

    def unlink_owned(self, path: Path) -> None:
        if not self.owns(path):
            return
        path.unlink()
        self._paths.pop(path, None)

    def cleanup(self) -> None:
        files = [created for created in self._paths.values()
                 if not created.is_directory]
        for created in sorted(files, key=lambda item: len(item.path.parts),
                              reverse=True):
            try:
                self.unlink_owned(created.path)
            except OSError:
                pass
        directories = [created for created in self._paths.values()
                       if created.is_directory]
        for created in sorted(
                directories, key=lambda item: len(item.path.parts), reverse=True):
            if not self.owns(created.path):
                continue
            try:
                created.path.rmdir()
            except OSError:
                pass
            else:
                self._paths.pop(created.path, None)


@dataclass
class _ExclusiveClaim:
    path: Path
    descriptor: int
    created: _CreatedPaths

    def require_current(self, purpose: str) -> None:
        self.created.require_owned(self.path, purpose)

    def close(self) -> None:
        if self.descriptor >= 0:
            os.close(self.descriptor)
            self.descriptor = -1


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
    ).encode("ascii")


def _resolved(path: Path) -> Path:
    return Path(os.path.realpath(os.path.abspath(os.fspath(path))))


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _same_or_contains(left: Path, right: Path) -> bool:
    try:
        common = os.path.commonpath((os.fspath(left), os.fspath(right)))
    except ValueError:
        return False
    return common in {os.fspath(left), os.fspath(right)}


def _has_denied_path_class(path: Path,
                           classes: set[str] = DENIED_PATH_CLASSES) -> bool:
    folded_parts = {part.casefold() for part in path.parts}
    folded_path = path.as_posix().casefold()
    return (bool(folded_parts & classes)
            or "wot-world-v3" in path.name.casefold()
            or "/cache/world/" in f"/{folded_path.strip('/')}/")


def _claim_new_file(path: Path, created: _CreatedPaths,
                    purpose: str) -> _ExclusiveClaim:
    created.makedirs(path.parent)
    try:
        descriptor = os.open(
            path,
            os.O_RDWR | os.O_CREAT | os.O_EXCL
            | getattr(os, "O_BINARY", 0), 0o600)
    except FileExistsError as exc:
        raise GenerationFailed(f"{purpose} no-replace claim failed: {path}") from exc
    created.record_file(path)
    return _ExclusiveClaim(path, descriptor, created)


def _write_claimed_json(claim: _ExclusiveClaim,
                        document: dict[str, Any]) -> None:
    claim.require_current("receipt publication claim")
    descriptor = claim.descriptor
    try:
        data = canonical_bytes(document) + b"\n"
        os.ftruncate(descriptor, 0)
        os.lseek(descriptor, 0, os.SEEK_SET)
        offset = 0
        while offset < len(data):
            offset += os.write(descriptor, data[offset:])
        os.fsync(descriptor)
    except OSError as exc:
        raise GenerationFailed(
            f"receipt publication failed: {claim.path}: {exc}") from exc
    claim.require_current("published receipt")


def _install_open_audit(denied_roots: tuple[Path, ...], output_root: Path,
                        destination: Path, allow_project_content: bool,
                        opened_paths: set[Path],
                        generated_paths: set[Path]) -> None:
    global _AUDIT_INSTALLED, _AUDIT_POLICY

    def audit(event: str, arguments: tuple[Any, ...]) -> None:
        policy = _AUDIT_POLICY
        if policy is None or event != "open" or not arguments:
            return
        candidate = arguments[0]
        if not isinstance(candidate, (str, bytes, os.PathLike)):
            return
        try:
            resolved = _resolved(Path(os.fsdecode(candidate)))
        except (OSError, TypeError, ValueError):
            return
        (active_denied, active_output, active_destination, content_mode,
         active_opened, active_generated) = policy
        if resolved == active_destination or resolved in active_generated:
            return
        if (not content_mode
                and (resolved == active_output or active_output in resolved.parents)):
            return
        if _has_denied_path_class(resolved):
            raise GenerationFailed(f"denied input open (class): {resolved}")
        for denied in active_denied:
            if resolved == denied or denied in resolved.parents:
                raise GenerationFailed(f"denied input open: {resolved}")
        active_opened.add(resolved)

    _AUDIT_POLICY = (
        denied_roots, output_root, destination, allow_project_content,
        opened_paths, generated_paths,
    )
    if not _AUDIT_INSTALLED:
        sys.addaudithook(audit)
        _AUDIT_INSTALLED = True


def _clear_open_audit() -> None:
    global _AUDIT_POLICY
    _AUDIT_POLICY = None


class _PackageReader:
    def __init__(self, data: bytes, limit: int | None = None):
        self.data = data
        self.limit = len(data) if limit is None else limit
        self.offset = 0

    def seek(self, offset: int) -> None:
        if offset < 0 or offset > self.limit:
            raise VerificationFailed("package table offset is out of bounds")
        self.offset = offset

    def take(self, size: int) -> bytes:
        end = self.offset + size
        if size < 0 or end > self.limit:
            raise VerificationFailed("package table is truncated")
        value = self.data[self.offset:end]
        self.offset = end
        return value

    def i32(self) -> int:
        return struct.unpack("<i", self.take(4))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def i64(self) -> int:
        return struct.unpack("<q", self.take(8))[0]

    def u8(self) -> int:
        return self.take(1)[0]

    def fstring(self) -> str:
        count = self.i32()
        if count == 0:
            return ""
        if abs(count) > 65536:
            raise VerificationFailed("package FString is unreasonably large")
        try:
            if count > 0:
                return self.take(count).rstrip(b"\0").decode("utf-8")
            return self.take(-count * 2).decode("utf-16-le").rstrip("\0")
        except UnicodeDecodeError as exc:
            raise VerificationFailed("package FString is not valid text") from exc


def _parse_package(data: bytes) -> BootstrapMapIdentity:
    """Parse the UE 5.8 package tables and WorldSettings mode binding."""
    if not data or len(data) > MAX_PACKAGE_BYTES:
        raise VerificationFailed("bootstrap package size is outside 1..64 MiB")
    reader = _PackageReader(data)
    if reader.u32() != PACKAGE_FILE_TAG:
        raise VerificationFailed("not an Unreal loose package")
    legacy = reader.i32()
    _legacy_ue3 = reader.i32()
    ue4 = reader.i32()
    ue5 = reader.i32()
    _licensee = reader.i32()
    if legacy != -9 or ue4 < 522 or ue5 < 1018:
        raise VerificationFailed("unsupported package summary version")
    reader.take(20)  # FIoHash SavedHash
    total_header_size = reader.i32()
    custom_count = reader.i32()
    if custom_count < 0 or custom_count > 4096:
        raise VerificationFailed("invalid custom-version count")
    reader.take(custom_count * 20)  # FGuid + int32 version
    package = reader.fstring()
    reader.u32()  # package flags
    name_count, name_offset = reader.i32(), reader.i32()
    reader.i32(); reader.i32()  # soft object path count/offset
    reader.fstring()  # localization id
    reader.i32(); reader.i32()  # gatherable text count/offset
    export_count, export_offset = reader.i32(), reader.i32()
    import_count, import_offset = reader.i32(), reader.i32()
    reader.i32(); reader.i32()  # cell export count/offset
    reader.i32(); reader.i32()  # cell import count/offset
    reader.i32()  # metadata offset
    reader.i32()  # depends offset
    reader.i32(); reader.i32()  # soft package reference count/offset
    reader.i32()  # searchable names offset
    reader.i32()  # thumbnail offset
    reader.i32(); reader.i32()  # import type hierarchy count/offset
    if not (0 < total_header_size <= len(data)):
        raise VerificationFailed("invalid total header size")
    if reader.offset > total_header_size:
        raise VerificationFailed("package summary extends past total header size")
    if name_count <= 0 or name_count > 1_000_000:
        raise VerificationFailed("invalid name table count")
    if import_count <= 0 or import_count > 1_000_000:
        raise VerificationFailed("invalid import table count")
    if export_count <= 0 or export_count > 1_000_000:
        raise VerificationFailed("invalid export table count")
    for label, offset in (("name", name_offset), ("import", import_offset),
                          ("export", export_offset)):
        if offset < reader.offset or offset >= total_header_size:
            raise VerificationFailed(
                f"{label} table lies outside total header size")

    reader = _PackageReader(data, total_header_size)
    reader.seek(name_offset)
    names = []
    for _index in range(name_count):
        names.append(reader.fstring())
        reader.take(4)  # legacy non-case/case-preserving hashes

    def read_fname(active_reader: _PackageReader = reader) -> str:
        index = active_reader.i32()
        number = active_reader.i32()
        if index < 0 or index >= len(names) or number < 0:
            raise VerificationFailed("invalid FName in package table")
        return names[index] if number == 0 else f"{names[index]}_{number - 1}"

    reader.seek(import_offset)
    imports: list[tuple[str, str, int, str, str]] = []
    for _index in range(import_count):
        class_package = read_fname()
        class_name = read_fname()
        outer = reader.i32()
        object_name = read_fname()
        explicit_package = read_fname()
        optional = reader.u32()
        if optional not in (0, 1):
            raise VerificationFailed("invalid optional-import flag")
        imports.append((class_package, class_name, outer,
                        object_name, explicit_package))

    def import_path(index: int, visiting: frozenset[int] = frozenset()) -> str:
        if index < 0 or index >= len(imports):
            raise VerificationFailed("invalid import index")
        if index in visiting:
            raise VerificationFailed("cyclic import outer chain")
        _class_package, _class_name, outer, object_name, explicit = imports[index]
        if explicit.startswith("/"):
            owner = explicit
        elif outer < 0:
            owner = import_path(-outer - 1, visiting | {index})
        elif outer == 0:
            owner = ""
        else:
            raise VerificationFailed("import is unexpectedly nested in an export")
        if object_name.startswith("/"):
            return object_name
        return f"{owner}.{object_name}" if owner else object_name

    reader.seek(export_offset)
    exports: list[dict[str, int | str]] = []
    for _index in range(export_count):
        class_index = reader.i32()
        reader.i32()  # super index
        reader.i32()  # template index
        outer_index = reader.i32()
        object_name = read_fname()
        reader.u32()  # object flags
        serial_size = reader.i64()
        serial_offset = reader.i64()
        for _flag in range(3):
            if reader.u32() not in (0, 1):
                raise VerificationFailed("invalid export boolean")
        if reader.u32() not in (0, 1):  # inherited instance
            raise VerificationFailed("invalid inherited-export boolean")
        reader.u32()  # package flags
        for _flag in range(3):
            if reader.u32() not in (0, 1):
                raise VerificationFailed("invalid export resource boolean")
        for _dependency in range(5):
            reader.i32()
        script_start = reader.i64()
        script_end = reader.i64()
        exports.append({
            "class_index": class_index,
            "outer_index": outer_index,
            "object_name": object_name,
            "serial_size": serial_size,
            "serial_offset": serial_offset,
            "script_start": script_start,
            "script_end": script_end,
        })

    world_settings = []
    for export in exports:
        class_index = int(export["class_index"])
        if class_index >= 0:
            continue
        owner_index = -class_index - 1
        if import_path(owner_index) == "/Script/Engine.WorldSettings":
            world_settings.append(export)
    if len(world_settings) != 1:
        raise VerificationFailed(
            "package must contain exactly one WorldSettings export")

    settings = world_settings[0]
    serial_size = int(settings["serial_size"])
    serial_offset = int(settings["serial_offset"])
    script_start = int(settings["script_start"])
    script_end = int(settings["script_end"])
    if (serial_size <= 0 or serial_offset < total_header_size
            or serial_offset + serial_size > len(data)):
        raise VerificationFailed("WorldSettings export data is out of bounds")
    if not (0 <= script_start < script_end <= serial_size):
        raise VerificationFailed("WorldSettings tagged properties are out of bounds")

    properties = _PackageReader(data, serial_offset + script_end)
    properties.seek(serial_offset + script_start)
    serialization_extensions = properties.u8()
    if serialization_extensions != 0:
        raise VerificationFailed(
            "unsupported WorldSettings serialization extensions")
    game_mode = ""
    terminated = False
    while properties.offset < properties.limit:
        property_name = read_fname(properties)
        if property_name == "None":
            terminated = True
            break
        remaining_type_nodes = 1
        type_names = []
        while remaining_type_nodes > 0:
            if len(type_names) >= 64:
                raise VerificationFailed("property type name is unreasonably deep")
            type_names.append(read_fname(properties))
            inner_count = properties.i32()
            if inner_count < 0 or inner_count > 64:
                raise VerificationFailed("invalid property type-name node")
            remaining_type_nodes += inner_count - 1
        property_size = properties.i32()
        property_flags = properties.u8()
        if property_size < 0 or property_flags & ~0x3f:
            raise VerificationFailed("invalid property tag")
        if property_flags & 0x01:
            if properties.i32() < 0:
                raise VerificationFailed("invalid property array index")
        if property_flags & 0x02:
            properties.take(16)
        if property_flags & 0x04:
            raise VerificationFailed("unsupported property-tag extensions")
        value = properties.take(property_size)
        if property_name != "DefaultGameMode":
            continue
        if game_mode:
            raise VerificationFailed("duplicate DefaultGameMode property")
        if (not type_names or type_names[0] not in {
                "ObjectProperty", "ClassProperty"} or property_size != 4):
            raise VerificationFailed("DefaultGameMode has an invalid property type")
        package_index = struct.unpack("<i", value)[0]
        if package_index >= 0:
            raise VerificationFailed("DefaultGameMode does not resolve to an import")
        mode_import_index = -package_index - 1
        if mode_import_index >= len(imports):
            raise VerificationFailed("DefaultGameMode import index is invalid")
        if imports[mode_import_index][1] != "Class":
            raise VerificationFailed("DefaultGameMode import is not a class")
        game_mode = import_path(mode_import_index)
    if not terminated or properties.offset != properties.limit:
        raise VerificationFailed(
            "WorldSettings tagged properties do not end at the export boundary")

    package_imports = set()
    for class_package, class_name, outer, object_name, explicit_package in imports:
        if class_package.startswith("/"):
            package_imports.add(class_package)
        if explicit_package.startswith("/"):
            package_imports.add(explicit_package)
        if class_name == "Package" and outer == 0 and object_name.startswith("/"):
            package_imports.add(object_name)

    return BootstrapMapIdentity(
        package=package,
        game_mode=game_mode,
        relative_path=RELATIVE_MAP.as_posix(),
        bytes=len(data),
        sha256=_sha256(data),
        import_packages=tuple(sorted(package_imports)),
    )


def identity_document_for_path(output_root: Path, map_path: Path,
                               allow_project_content: bool,
                               generator_sha256: str | None = None) -> dict[str, Any]:
    output_root = _resolved(output_root)
    map_path = _resolved(map_path)
    try:
        relative = map_path.relative_to(output_root).as_posix()
    except ValueError as exc:
        raise VerificationFailed("bootstrap map is outside its output root") from exc
    identity = _parse_package(map_path.read_bytes())
    document = {
        "allow_project_content": bool(allow_project_content),
        "bytes": identity.bytes,
        "format": "bootstrap-map-output-identity-receipt-v1",
        "game_mode": identity.game_mode,
        "generator_identity": GENERATOR_IDENTITY,
        "import_packages": list(identity.import_packages),
        "package": identity.package,
        "platform": platform.system(),
        "relative_path": relative,
        "root_identity": os.fspath(output_root),
        "sha256": identity.sha256,
        "version": RECEIPT_VERSION,
    }
    if generator_sha256 is not None:
        document.update({
            "generator_sha256": generator_sha256,
            "generator_source_identity": os.fspath(_resolved(Path(__file__))),
        })
    return document


def _validate_generation_args(
        args: argparse.Namespace) -> tuple[Path, Path, Path, Path, tuple[Path, ...]]:
    if args.denied_read_open_receipt is None or args.output_identity_receipt is None:
        raise GenerationFailed("both receipt paths are required")
    output = _resolved(args.output_root)
    denied_receipt = _resolved(args.denied_read_open_receipt)
    identity_receipt = _resolved(args.output_identity_receipt)
    project = _resolved(getattr(args, "project_root", PROJECT_ROOT))
    content = project / "Content"
    if not args.require_empty_output:
        raise GenerationFailed("generation requires --require-empty-output")
    if not args.no_replace:
        raise GenerationFailed("generation requires --no-replace")
    if denied_receipt == identity_receipt:
        raise GenerationFailed("receipt paths must be separate")
    if denied_receipt.exists() or identity_receipt.exists():
        raise GenerationFailed("receipt no-replace refused stale evidence")
    allow_content = bool(args.allow_project_content)
    if output == content:
        if not allow_content:
            raise GenerationFailed("project Content requires --allow-project-content")
    elif content == output or content in output.parents or output in content.parents:
        raise GenerationFailed("output root overlaps project Content")
    elif allow_content:
        raise GenerationFailed("--allow-project-content is valid only for project Content")
    if output == Path(output.anchor):
        raise GenerationFailed("the filesystem root cannot be an output root")
    if output != content and _has_denied_path_class(
            output, DENIED_OUTPUT_PATH_CLASSES):
        raise GenerationFailed("output root is inside a denied content/retail/cache class")
    reserved = tuple(_resolved(path) for path in
                     getattr(args, "require_disjoint_from", []))
    if output != content and output.name.casefold() == "bootstrapmap":
        sibling = output.with_name("NeutralMapShells")
        if sibling not in reserved:
            reserved += (sibling,)
    for path in reserved:
        if _same_or_contains(output, path):
            raise GenerationFailed(
                f"output root overlaps NeutralMapShells sibling root: {path}")
    destination = output / RELATIVE_MAP
    if destination.exists():
        raise GenerationFailed("no-replace refused the existing bootstrap map")
    if output != content and output.exists():
        raise GenerationFailed("output root must be absent before atomic creation")
    for receipt in (denied_receipt, identity_receipt):
        if receipt == output or output in receipt.parents:
            raise GenerationFailed("receipts must be outside the output root")
    return output, destination, denied_receipt, identity_receipt, reserved


def _copy_new(source: Path, destination: Path, created: _CreatedPaths) -> None:
    created.makedirs(destination.parent)
    source_fd = os.open(source, os.O_RDONLY | getattr(os, "O_BINARY", 0))
    destination_fd = None
    try:
        destination_fd = os.open(
            destination,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL
            | getattr(os, "O_BINARY", 0), 0o600)
        created.record_file(destination)
        while True:
            block = os.read(source_fd, 1024 * 1024)
            if not block:
                break
            view = memoryview(block)
            while view:
                written = os.write(destination_fd, view)
                view = view[written:]
        os.fsync(destination_fd)
        created.require_owned(destination, "canonical map publication")
    except FileExistsError as exc:
        raise GenerationFailed(
            f"canonical map publication lost no-replace race: {destination}") from exc
    finally:
        os.close(source_fd)
        if destination_fd is not None:
            os.close(destination_fd)


def _unreal_author(unreal: Any, destination: Path, output_root: Path,
                   generated_audit_paths: set[Path],
                   created: _CreatedPaths,
                   claims: list[_ExclusiveClaim]) -> None:
    content = _resolved(Path(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.project_content_dir())))
    canonical_file = content / RELATIVE_MAP
    direct = destination == canonical_file
    if canonical_file.exists():
        raise GenerationFailed("no-replace refused an existing canonical bootstrap package")
    # Unreal's save APIs always write the mounted /Game package to its canonical
    # Content path. From this point forward that exact absent path is an output,
    # not an input; no other path beneath Content is exempted from the audit.
    generated_audit_paths.add(canonical_file)
    claim_path = canonical_file.with_name(canonical_file.name + ".claim")
    generated_audit_paths.add(claim_path)
    canonical_claim = _claim_new_file(
        claim_path, created, "canonical-save")
    claims.append(canonical_claim)
    created.makedirs(canonical_file.parent)
    saving = unreal.EditorLoadingAndSavingUtils
    assets = unreal.EditorAssetLibrary
    world = None
    completed = False
    canonical_created = False
    try:
        # A partitioned world drags the WorldPartition streaming machinery into
        # the one map that must stay minimal, and its editor-initialized
        # subsystem cannot be safely GC-purged by inspection specs. The
        # bootstrap world is explicitly non-partitioned.
        level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not level_editor.new_level(PACKAGE_NAME, is_partitioned_world=False):
            raise GenerationFailed("failed to create a blank bootstrap world")
        if not canonical_file.is_file():
            raise GenerationFailed("new level did not publish its canonical package")
        created.record_file(canonical_file)
        canonical_created = True
        editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
        world = editor.get_editor_world()
        if world is None:
            raise GenerationFailed("failed to create a blank bootstrap world")
        settings = world.get_world_settings()
        if settings is None:
            raise GenerationFailed("blank bootstrap world has no WorldSettings")
        for actor in list(unreal.GameplayStatics.get_all_actors_of_class(
                world, unreal.Actor)):
            if actor != settings:
                actor.destroy_actor()
        remaining = list(unreal.GameplayStatics.get_all_actors_of_class(
            world, unreal.Actor))
        if remaining != [settings]:
            raise GenerationFailed("bootstrap world retained a non-WorldSettings actor")
        mode_class = unreal.load_class(None, GAME_MODE_PATH)
        if mode_class is None:
            raise GenerationFailed("bootstrap game-mode class is unavailable")
        settings.set_editor_property("default_game_mode", mode_class)
        if not saving.save_map(world, PACKAGE_NAME):
            raise GenerationFailed("failed to save the canonical bootstrap package")
        created.record_file(canonical_file)
        if not canonical_file.is_file() or not assets.does_asset_exist(PACKAGE_NAME):
            raise GenerationFailed("saved bootstrap package is not at its canonical path")
        if not direct:
            _copy_new(canonical_file, destination, created)
        completed = True
    finally:
        world = None
        if not direct or not completed:
            try:
                saving.load_map("/Engine/Maps/Entry")
            except Exception:
                pass
            if canonical_created and created.owns(canonical_file):
                try:
                    assets.delete_asset(PACKAGE_NAME)
                except Exception:
                    pass
            try:
                unreal.SystemLibrary.collect_garbage()
            except Exception:
                pass
            created.unlink_owned(canonical_file)
        # Windows refuses unlink while our own claim descriptor is still open
        # (os.open handles carry no FILE_SHARE_DELETE); POSIX permitted it and
        # generate()'s outer finally closed the descriptor later. Close first —
        # idempotent for that outer close, and unlink_owned's device/inode
        # ownership check still guards the close-to-unlink window.
        canonical_claim.close()
        created.unlink_owned(claim_path)


def generate(args: argparse.Namespace, unreal: Any | None = None,
             *, author: Callable[[Path], None] | None = None) -> tuple[dict, dict]:
    (output, destination, denied_receipt, identity_receipt,
     reserved_roots) = _validate_generation_args(args)
    content_mode = bool(args.allow_project_content)
    created = _CreatedPaths()
    claims: list[_ExclusiveClaim] = []
    content = _resolved(getattr(args, "project_root", PROJECT_ROOT)) / "Content"
    try:
        if output != content:
            created.mkdir(output)
        else:
            created.makedirs(destination.parent)
        denied_claim = _claim_new_file(
            denied_receipt, created, "denied-read/open receipt")
        claims.append(denied_claim)
        identity_claim = _claim_new_file(
            identity_receipt, created, "output-identity receipt")
        claims.append(identity_claim)
        if author is not None:
            destination_claim = _claim_new_file(
                destination, created, "canonical-save")
            claims.append(destination_claim)
    except BaseException:
        for claim in claims:
            claim.close()
        created.cleanup()
        raise
    project = _resolved(getattr(args, "project_root", PROJECT_ROOT))
    denied_roots = (tuple(project / name for name in DENIED_ROOT_NAMES)
                    + reserved_roots)
    audited: set[Path] = set()
    generated_audit_paths: set[Path] = {
        denied_receipt, identity_receipt,
    }
    _install_open_audit(
        denied_roots, output, destination, content_mode, audited,
        generated_audit_paths)
    try:
        if author is not None:
            author(destination)
            created.require_owned(destination, "canonical save")
        elif unreal is not None:
            _unreal_author(
                unreal, destination, output, generated_audit_paths,
                created, claims)
        else:
            raise GenerationFailed("Unreal authoring backend is unavailable")
        if not destination.is_file():
            raise GenerationFailed("authoring backend did not produce the bootstrap map")
        generator_path = _resolved(Path(__file__))
        source = generator_path.read_bytes()
        generator_digest = _sha256(source)
        identity = identity_document_for_path(
            output, destination, content_mode, generator_digest)
        if identity["package"] != PACKAGE_NAME:
            raise GenerationFailed("saved package has a non-canonical internal name")
        if identity["game_mode"] != GAME_MODE_PATH:
            raise GenerationFailed("saved package has the wrong game-mode override")
        if any(not package.startswith("/Script/")
               for package in identity["import_packages"]):
            raise GenerationFailed("saved package import closure is not code-only")
        opened_inputs = [{
            "path": os.fspath(generator_path), "sha256": _sha256(source),
        }]
        for path in sorted(audited):
            if path == generator_path or not path.is_file():
                continue
            raw = path.read_bytes()
            opened_inputs.append({
                "path": os.fspath(path), "sha256": _sha256(raw),
            })
        denied = {
            "allow_project_content": content_mode,
            "denied_path_classes": [
                "Content", "original", "devtree", "exports", "cache",
                "cache-v3", "installers", "GeneratedContent",
                "generated-cache", "retail-cache", "*.wot-world-v3",
                "/cache/world/",
            ],
            "denied_roots": [
                os.fspath(_resolved(getattr(args, "project_root", PROJECT_ROOT)) / name)
                for name in DENIED_ROOT_NAMES
            ],
            "format": "bootstrap-map-denied-read-open-receipt-v1",
            "generator_identity": GENERATOR_IDENTITY,
            "generator_sha256": generator_digest,
            "generator_source_identity": os.fspath(generator_path),
            "native_ue_io_audited": False,
            "native_ue_io_limitation": (
                "Python cannot observe native Unreal package or asset-registry I/O; "
                "the generated canonical staging package is the only permitted "
                "project-Content write in external-output mode."
            ),
            "opened_inputs": opened_inputs,
            "platform": platform.system(),
            "reserved_disjoint_roots": [
                os.fspath(path) for path in reserved_roots
            ],
            "root_identity": os.fspath(output),
            "version": RECEIPT_VERSION,
            "violations": [],
        }
        _write_claimed_json(denied_claim, denied)
        _write_claimed_json(identity_claim, identity)
        _clear_open_audit()
        return denied, identity
    except BaseException:
        _clear_open_audit()
        created.cleanup()
        raise
    finally:
        for claim in claims:
            claim.close()


def generate_bootstrap_map(output_root: Path, denied_read_open_receipt: Path,
                           output_identity_receipt: Path, *,
                           allow_project_content: bool = False,
                           unreal: Any | None = None) -> BootstrapMapIdentity:
    args = argparse.Namespace(
        output_root=output_root,
        denied_read_open_receipt=denied_read_open_receipt,
        output_identity_receipt=output_identity_receipt,
        require_empty_output=True,
        no_replace=True,
        allow_project_content=allow_project_content,
        project_root=PROJECT_ROOT,
        require_disjoint_from=[],
    )
    _denied, identity = generate(args, unreal)
    return BootstrapMapIdentity(
        package=identity["package"], game_mode=identity["game_mode"],
        relative_path=identity["relative_path"], bytes=identity["bytes"],
        sha256=identity["sha256"],
        import_packages=tuple(identity["import_packages"]),
    )


def verify_bootstrap_map(output_root: Path, identity_receipt: Path,
                         required_package: str = PACKAGE_NAME,
                         required_game_mode: str = GAME_MODE_PATH,
                         require_code_only_closure: bool = False) -> None:
    output = _resolved(output_root)
    receipt_path = _resolved(identity_receipt)
    try:
        raw_receipt = receipt_path.read_bytes()
        document = json.loads(raw_receipt)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise VerificationFailed(f"could not read identity receipt: {exc}") from exc
    if raw_receipt.rstrip(b"\r\n") != canonical_bytes(document):
        raise VerificationFailed("identity receipt is not canonical JSON")
    if document.get("format") != "bootstrap-map-output-identity-receipt-v1":
        raise VerificationFailed("identity receipt format mismatched")
    if document.get("root_identity") != os.fspath(output):
        raise VerificationFailed("identity receipt names a different output root")
    relative = document.get("relative_path")
    if relative != RELATIVE_MAP.as_posix():
        raise VerificationFailed("identity receipt names a non-canonical map path")
    map_path = output / RELATIVE_MAP
    try:
        actual = identity_document_for_path(
            output, map_path, bool(document.get("allow_project_content")),
            document.get("generator_sha256"))
    except (OSError, VerificationFailed) as exc:
        raise VerificationFailed(f"could not inspect bootstrap map: {exc}") from exc
    # Verification is intentionally portable (the Windows lane may inspect a
    # Mac-produced fixture and vice versa); retain the generating platform as
    # receipt provenance instead of replacing it with the verifier's host.
    actual["platform"] = document.get("platform")
    if actual != document:
        raise VerificationFailed("bootstrap bytes or identity receipt changed")
    if actual["package"] != required_package:
        raise VerificationFailed("bootstrap internal package name mismatched")
    if actual["game_mode"] != required_game_mode:
        raise VerificationFailed("bootstrap game-mode override mismatched")
    if require_code_only_closure:
        rejected = [package for package in actual["import_packages"]
                    if not package.startswith("/Script/")]
        if rejected:
            raise VerificationFailed(
                "bootstrap import closure contains content: " + ", ".join(rejected))


def _generation_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--denied-read-open-receipt", required=True, type=Path)
    parser.add_argument("--output-identity-receipt", required=True, type=Path)
    parser.add_argument("--require-empty-output", action="store_true")
    parser.add_argument("--no-replace", action="store_true")
    parser.add_argument("--allow-project-content", action="store_true")
    parser.add_argument(
        "--require-disjoint-from", action="append", default=[], type=Path)
    return parser


def _verification_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Verify a generated bootstrap map")
    parser.add_argument("verify", nargs="?")
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--identity-receipt", required=True, type=Path)
    parser.add_argument("--required-package", default=PACKAGE_NAME)
    parser.add_argument("--required-game-mode", default=GAME_MODE_PATH)
    parser.add_argument("--require-code-only-closure", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        if argv and argv[0] == "verify":
            args = _verification_parser().parse_args(argv)
            verify_bootstrap_map(
                args.output_root, args.identity_receipt,
                args.required_package, args.required_game_mode,
                args.require_code_only_closure)
            print(f"BOOTSTRAP-MAP-VERIFY-OK package={args.required_package}")
            return 0
        args, _unreal_args = _generation_parser().parse_known_args(argv)
        import unreal
        # import_all is also executed inside disposable/isolated projects.
        # Bind Content policy to the project the commandlet actually mounted,
        # not to the checkout that supplied this script.
        mounted_content = _resolved(Path(
            unreal.Paths.convert_relative_path_to_full(
                unreal.Paths.project_content_dir())))
        args.project_root = mounted_content.parent
        _denied, identity = generate(args, unreal)
        unreal.log("BOOTSTRAP-MAP-OK " + identity["sha256"])
        return 0
    except (GenerationFailed, VerificationFailed, OSError, ValueError) as exc:
        print(f"bootstrap map operation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
