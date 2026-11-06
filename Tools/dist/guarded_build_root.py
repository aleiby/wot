#!/usr/bin/env python3
"""Persistent guarded build root: incremental UBT builds, isolated evidence.

A guarded client is a monolithic, unique-build-environment target, so UBT
compiles every engine module into the PROJECT's Intermediate
(UEBuildTarget.cs; one measured Test build was 1,163 serial actions / 69 min and
the Shipping build 124 min). A fresh ISO therefore recompiled the whole engine
twice per release. The build root is a stable project directory, one per
(platform, pinned engine commit), that keeps UBT's intermediates between runs.

Why UBT's own dependency tracking keeps a persistent root correct (UE 5.8
source, Engine/Source/Programs/UnrealBuildTool):

* An action is outdated when any produced item is missing or a zero-byte
  object (Actions/ActionGraph.cs IsIndividualActionOutdated), when the
  command path/arguments/version recorded in ActionHistory for any produced
  item differ (Actions/ActionHistory.cs UpdateProducingCommandLine; a new
  entry counts as changed), when any prerequisite (source, response file,
  PCH, force-included Definitions header, link inputs, libraries) is newer
  than its last execution by more than 1 s, or when any header in the
  compiler-written dependency list is newer at all; outdatedness propagates
  through prerequisite actions (IsActionOutdatedDueToPrerequisites).
* Response files, Definitions headers and unity files are rewritten only when
  their content changes (ActionGraphBuilder.CreateIntermediateTextFile ->
  WriteFileIfChanged), and UHT output likewise (EpicGames.UHT UhtSession
  SaveIfChanged), so a changed definition or flag always moves a
  prerequisite's timestamp.
* The makefile is rebuilt when .Build.cs/.Target.cs/.uproject/.uplugin/config
  inputs are newer or source files are added/removed
  (System/TargetMakefile.cs Load, IsValidForSourceFiles).

Every one of those checks is a TIMESTAMP check, so the correctness condition
is: "content changed => timestamp strictly newer than every earlier output".
The two historical failures of an incremental guarded build both violated it,
and this module is built so they cannot recur:

* The poisoned artifact (2026-08-05): a planted binary copied with
  shutil.copy2 carries a fresh CreationTime, and UBT takes the NEWER of
  LastWriteTime and CreationTime as an output's execution time
  (ActionGraph.cs, "copied files (such as from a build cache) can have a newer
  creation time"), so it looked freshly built. Here nothing is ever copied
  INTO the build root's Intermediate or Binaries -- UBT is their only writer;
  products only ever flow OUT to the ISO.
* copy2 preserves source mtimes, so an ISO seeded after a build could hold
  changed content with an OLDER mtime than the object compiled from the old
  content. Here the root is mirrored BY CONTENT: an identical file is left
  untouched (UBT correctly sees nothing changed) and a changed or new file is
  written fresh (mtime = now), at least TIMESTAMP_SLOP_SECONDS after the last
  UBT exit, which exceeds UBT's 1 s prerequisite slop.

The earlier "incremental build trusted its action history and skipped the
relink" diagnosis cannot happen by ActionGraph.cs -- a missing
product always makes its action outdated; that failure was the decorated
WoTUpdater-<P>-Test naming bug.

A UBT run killed mid-flight is the one hole those timestamp checks leave: a
product the dead linker or compiler left half-written is non-empty and newer
than every prerequisite, so the next run calls it up to date (once,
a memory reaper killed the Test link, the rerun skipped the
relink and staged a truncated WoT-Win64-Test.exe that only the funnel's seam
check caught). begin_ubt/end_ubt therefore keep an in-flight marker in the
root's state that only a zero exit clears; the next begin_ubt that finds it
deletes every UBT-owned file written since the dead run started, so each
action that run touched is outdated again (ActionHistory is kept: a missing
entry already makes its action outdated, and losing the file would make
EVERY action outdated). transfer_products also refuses a PE or Mach-O image
whose headers describe more bytes than the file holds.

Stdlib only; runs on the Mac host's Python 3.9.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import stat
import struct
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Callable, Sequence


STATE_FORMAT = "wot-guarded-build-root-state-v1"
TRANSFER_FORMAT = "wot-guarded-build-root-transfer-v1"
STATE_NAME = ".wot-build-root.json"
# Everything UBT (and the .Build.cs/.Target.cs rules it compiles) reads from a
# project directory. Tools/EnginePatches are not UBT inputs: the wrapper runs
# from the ISO, so its verifier, patch manifest and receipt recorder are the
# ISO's own snapshot-bound copies.
SYNC_ENTRIES = ("WoT.uproject", "Config", "Source", "Plugins", "Build",
                "ThirdParty")
# Owned by UBT alone. Wiped when the engine identity changes or the root's
# provenance is unknown.
UBT_OWNED = ("Intermediate", "Binaries")
# UBT's per-target command-line history (Actions/ActionHistory.cs
# GetProjectLocation/GetEngineLocation), kept through interrupted-run
# recovery.
UBT_HISTORY_NAMES = frozenset(("ActionHistory.dat", "ActionHistory.bin"))
IN_FLIGHT_KEY = "ubt_in_flight"
# Receipt product types that are linked images (TargetReceipt
# BuildProductType); each must be a complete PE (Win64) or Mach-O (Mac).
IMAGE_PRODUCT_TYPES = frozenset(("Executable", "DynamicLibrary"))
# ActionGraph.cs treats a prerequisite as newer only when it is > 1 s newer
# than the output; dependency-list headers use a strict comparison.
TIMESTAMP_SLOP_SECONDS = 2.0
# How far before an interrupted run's recorded start its writes may be
# stamped (NTFS stamps lag time.time() by a clock tick). Separate from
# TIMESTAMP_SLOP_SECONDS, which only paces mirror writes after a UBT exit.
RECOVERY_SLOP_SECONDS = 2.0
HEADER_READ_BYTES = 1 << 16
ENGINE_IDENTITY_KEYS = ("format", "platform", "commit", "tag", "toolchain",
                        "checks")
_TEMP_SUFFIX = ".wot-sync-tmp"


class BuildRootError(RuntimeError):
    """The build root cannot be used; the caller must fail closed."""


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def default_parent(repo: Path) -> Path:
    """<main checkout>/.guarded-build, shared by every worktree.

    Two directory levels below a checkout root on purpose: UBT looks for a
    .git only in the project directory and its parent
    (System/SourceFileWorkingSet.cs TryCreateGitWorkingSet), so no project
    file in the root can enter the adaptive non-unity working set, exactly as
    an ignored .stripped-pak-* ISO never does.
    """
    result = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "--git-common-dir"],
        capture_output=True, text=True)
    if result.returncode != 0 or not result.stdout.strip():
        return repo.resolve() / ".guarded-build"
    common = Path(result.stdout.strip())
    if not common.is_absolute():
        common = repo / common
    return common.resolve().parent / ".guarded-build"


def root_for(parent: Path, platform_name: str, engine_commit: str) -> Path:
    if len(engine_commit) < 12 or any(
            char not in "0123456789abcdef" for char in engine_commit):
        raise BuildRootError(
            f"engine pin is not a hex commit: {engine_commit!r}")
    return parent / f"{platform_name}-{engine_commit[:12]}"


def engine_identity(receipt: dict) -> dict:
    """The path-independent identity of a verify_ue58_source receipt."""
    if not isinstance(receipt, dict) or receipt.get(
            "format") != "wot-ue58-source-receipt-v1":
        raise BuildRootError("engine verification receipt is malformed")
    return {key: receipt.get(key) for key in ENGINE_IDENTITY_KEYS}


class BuildRootLock:
    """Exclusive, process-lifetime lock beside (not inside) the build root.

    The OS releases it when the holder dies, so a killed pipeline never leaves
    a stale lock. The lock file lives outside the root so a wipe cannot drop it.
    """

    def __init__(self, root: Path):
        self.root = root
        self.path = root.parent / (root.name + ".lock")
        self._handle = None

    def __enter__(self) -> "BuildRootLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        handle = open(self.path, "a+b")
        try:
            if os.name == "nt":
                import msvcrt
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            handle.close()
            raise BuildRootError(
                f"guarded build root {self.root} is in use by another "
                f"pipeline (lock {self.path})") from exc
        self._handle = handle
        return self

    def __exit__(self, *_exc) -> None:
        handle, self._handle = self._handle, None
        if handle is None:
            return
        try:
            if os.name == "nt":
                import msvcrt
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        finally:
            handle.close()


def read_state(root: Path) -> dict | None:
    try:
        value = json.loads((root / STATE_NAME).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict) or value.get("format") != STATE_FORMAT:
        return None
    return value


def _write_state(root: Path, state: dict) -> None:
    write_json(root / STATE_NAME, {**state, "format": STATE_FORMAT,
                                   "updated_utc": _utc_now()})


def _wipe_ubt_outputs(root: Path) -> None:
    for name in UBT_OWNED:
        _remove(root / name)


def prepare(root: Path, platform_name: str, identity: dict, *,
            clock: Callable[[], float] = time.time) -> dict:
    """Create the root, or reset UBT's outputs when their provenance changed.

    Reset reasons: no state file beside existing outputs (unknown provenance),
    a different platform, a different engine/toolchain identity, or a clock
    that ran backwards past the last UBT exit (timestamps would lie).
    """
    root.mkdir(parents=True, exist_ok=True)
    state = read_state(root)
    has_outputs = any((root / name).exists() for name in UBT_OWNED)
    reason = None
    if state is None:
        if has_outputs:
            reason = "no build-root state beside existing UBT outputs"
    elif state.get("platform") != platform_name:
        reason = "build-root platform changed"
    elif state.get("engine_identity") != identity:
        reason = "engine or toolchain identity changed"
    else:
        last_exit = state.get("last_ubt_exit_epoch")
        if isinstance(last_exit, (int, float)) and clock() < last_exit - 1.0:
            reason = "system clock is behind the last UBT exit"
    if reason is not None:
        _wipe_ubt_outputs(root)
    previous_exit = None if reason is not None or state is None else \
        state.get("last_ubt_exit_epoch")
    # A reset already removed whatever an interrupted run left behind.
    in_flight = None if reason is not None or state is None else \
        state.get(IN_FLIGHT_KEY)
    new_state = {
        "platform": platform_name,
        "engine_identity": identity,
        "last_ubt_exit_epoch": previous_exit,
    }
    if in_flight is not None:
        new_state[IN_FLIGHT_KEY] = in_flight
    _write_state(root, new_state)
    return {"root": str(root), "reset": reason}


def mark_ubt_exit(root: Path, *, clock: Callable[[], float] = time.time
                  ) -> None:
    """Record when UBT last wrote outputs (success or failure)."""
    state = read_state(root)
    if state is None:
        raise BuildRootError(f"build-root state vanished: {root}")
    state["last_ubt_exit_epoch"] = clock()
    _write_state(root, state)


def _recover_interrupted_ubt(root: Path, marker: dict) -> dict:
    """Delete every UBT-owned file a dead UBT run may have half-written.

    UBT takes the newer of LastWriteTime and CreationTime as an output's
    execution time (ActionGraph.cs), so the same newer-of rule decides which
    files that run wrote; the slop covers coarse filesystem clocks.
    """
    started = marker.get("started_epoch")
    if not isinstance(started, (int, float)):
        # A marker without a usable start time cannot bound the damage.
        _wipe_ubt_outputs(root)
        return {"label": marker.get("label"), "wiped": True, "deleted": 0}
    threshold = started - RECOVERY_SLOP_SECONDS
    deleted = 0
    for name in UBT_OWNED:
        base = root / name
        if not base.is_dir():
            continue
        for directory, _dirs, files in os.walk(base):
            for file_name in files:
                if file_name in UBT_HISTORY_NAMES:
                    continue
                path = Path(directory) / file_name
                try:
                    info = path.lstat()
                except OSError:
                    continue
                if max(info.st_mtime, info.st_ctime) >= threshold:
                    _remove(path)
                    deleted += 1
    return {"label": marker.get("label"), "wiped": False,
            "started_epoch": started, "deleted": deleted}


def begin_ubt(root: Path, label: str, *,
              clock: Callable[[], float] = time.time) -> dict | None:
    """Mark a UBT run in flight, first undoing a previous run that died.

    Returns the recovery summary when an earlier run's marker was still set
    (that run was killed, or exited nonzero), else None.
    """
    state = read_state(root)
    if state is None:
        raise BuildRootError(f"build-root state vanished: {root}")
    recovered = None
    marker = state.get(IN_FLIGHT_KEY)
    if marker is not None:
        recovered = _recover_interrupted_ubt(
            root, marker if isinstance(marker, dict) else {})
    state[IN_FLIGHT_KEY] = {"label": label, "started_epoch": clock()}
    _write_state(root, state)
    return recovered


def end_ubt(root: Path, *, succeeded: bool,
            clock: Callable[[], float] = time.time) -> None:
    """Record the UBT exit; only a zero exit clears the in-flight marker."""
    state = read_state(root)
    if state is None:
        raise BuildRootError(f"build-root state vanished: {root}")
    state["last_ubt_exit_epoch"] = clock()
    if succeeded:
        state.pop(IN_FLIGHT_KEY, None)
    _write_state(root, state)


def _image_extent_pe(data: bytes, size: int, label: str) -> int:
    """Bytes a PE file's headers say it holds (winnt.h layouts)."""
    if size < 64 or len(data) < 64 or data[:2] != b"MZ":
        raise BuildRootError(f"{label}: not a PE image (no MZ header)")
    e_lfanew, = struct.unpack_from("<I", data, 0x3C)
    if (e_lfanew + 24 > len(data)
            or data[e_lfanew:e_lfanew + 4] != b"PE\0\0"):
        raise BuildRootError(f"{label}: PE signature missing")
    sections, = struct.unpack_from("<H", data, e_lfanew + 6)
    optional_size, = struct.unpack_from("<H", data, e_lfanew + 20)
    optional = e_lfanew + 24
    table = optional + optional_size
    if (sections == 0 or optional_size < 64
            or table + 40 * sections > len(data)):
        raise BuildRootError(f"{label}: PE section table is incomplete")
    magic, = struct.unpack_from("<H", data, optional)
    if magic == 0x20B:
        rva_count_at, directories_at = optional + 108, optional + 112
    elif magic == 0x10B:
        rva_count_at, directories_at = optional + 92, optional + 96
    else:
        raise BuildRootError(
            f"{label}: unknown PE optional header magic {magic:#x}")
    extent, = struct.unpack_from("<I", data, optional + 60)  # SizeOfHeaders
    for index in range(sections):
        raw_size, raw_pointer = struct.unpack_from(
            "<II", data, table + 40 * index + 16)
        if raw_size:
            extent = max(extent, raw_pointer + raw_size)
    rva_count, = struct.unpack_from("<I", data, rva_count_at)
    if rva_count > 4 and directories_at + 8 * 5 <= table:
        # IMAGE_DIRECTORY_ENTRY_SECURITY holds a FILE offset, not an RVA.
        offset, length = struct.unpack_from("<II", data, directories_at + 32)
        if length:
            extent = max(extent, offset + length)
    return extent


def _image_extent_macho(data: bytes, label: str) -> int:
    """Bytes one thin Mach-O slice needs, from its load commands
    (mach-o/loader.h: LC_SEGMENT(_64) file ranges, LC_CODE_SIGNATURE)."""
    magic = data[:4]
    if magic == b"\xcf\xfa\xed\xfe":
        header = 32
    elif magic == b"\xce\xfa\xed\xfe":
        header = 28
    else:
        raise BuildRootError(f"{label}: not a Mach-O image")
    if header > len(data):
        raise BuildRootError(f"{label}: Mach-O header is incomplete")
    commands, commands_size = struct.unpack_from("<II", data, 16)
    extent = header + commands_size
    if extent > len(data):
        raise BuildRootError(f"{label}: Mach-O load commands are incomplete")
    at = header
    for _ in range(commands):
        if at + 8 > extent:
            raise BuildRootError(f"{label}: malformed Mach-O load commands")
        command, command_size = struct.unpack_from("<II", data, at)
        if command_size < 8 or at + command_size > extent:
            raise BuildRootError(f"{label}: malformed Mach-O load command")
        offset = length = 0
        if command == 0x19 and command_size >= 56:  # LC_SEGMENT_64
            offset, length = struct.unpack_from("<QQ", data, at + 40)
        elif command == 0x1 and command_size >= 40:  # LC_SEGMENT
            offset, length = struct.unpack_from("<II", data, at + 32)
        elif command == 0x1D and command_size >= 16:  # LC_CODE_SIGNATURE
            offset, length = struct.unpack_from("<II", data, at + 8)
        if length:
            extent = max(extent, offset + length)
        at += command_size
    return extent


def verify_image_complete(path: Path, platform_name: str) -> None:
    """Refuse a linked image that is shorter than its own headers declare.

    A link killed mid-write leaves such a file behind; UBT's timestamp checks
    cannot tell it from a finished product. Win64 images must be PE and Mac
    images Mach-O (thin or fat); other platforms are not checked.
    """
    if platform_name not in ("Win64", "Mac"):
        return
    size = path.stat().st_size
    label = str(path)
    with path.open("rb") as handle:
        head = handle.read(HEADER_READ_BYTES)
        if platform_name == "Win64":
            extent = _image_extent_pe(head, size, label)
        elif head[:4] in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
            wide = head[3] == 0xBF
            count, = struct.unpack_from(">I", head, 4)
            entry = 32 if wide else 20
            if count == 0 or 8 + entry * count > len(head):
                raise BuildRootError(f"{label}: fat Mach-O header is incomplete")
            extent = 8 + entry * count
            for index in range(count):
                at = 8 + entry * index
                offset, length = struct.unpack_from(
                    ">QQ" if wide else ">II", head, at + 8)
                extent = max(extent, offset + length)
                if offset + length > size:
                    continue
                handle.seek(offset)
                inner = _image_extent_macho(
                    handle.read(min(length, HEADER_READ_BYTES)), label)
                if inner > length:
                    raise BuildRootError(
                        f"{label}: Mach-O slice {index} is truncated")
        else:
            extent = _image_extent_macho(head, label)
    if extent > size:
        raise BuildRootError(
            f"{label}: image is truncated ({size} bytes on disk, its headers "
            f"describe {extent}); was its link interrupted?")


def _safe_relative(value: str) -> PurePosixPath:
    relative = PurePosixPath(value)
    if (relative.is_absolute() or not relative.parts
            or any(part in ("", ".", "..") for part in relative.parts)
            or ":" in relative.parts[0]):
        raise BuildRootError(f"unsafe relative path: {value!r}")
    return relative


def _inventory(base: Path, entries: Sequence[str]) -> dict[str, tuple]:
    """relative posix path -> ("dir",) | ("file", size) | ("link", target)."""
    rows: dict[str, tuple] = {}
    for entry in entries:
        top = base / entry
        if top.is_symlink():
            rows[entry] = ("link", os.readlink(top))
            continue
        if top.is_file():
            rows[entry] = ("file", top.stat().st_size)
            continue
        if not top.is_dir():
            continue
        rows[entry] = ("dir",)
        for current, directories, files in os.walk(top, followlinks=False):
            current_path = Path(current)
            for name in list(directories):
                path = current_path / name
                relative = path.relative_to(base).as_posix()
                if path.is_symlink():
                    rows[relative] = ("link", os.readlink(path))
                    directories.remove(name)
                else:
                    rows[relative] = ("dir",)
            for name in files:
                path = current_path / name
                relative = path.relative_to(base).as_posix()
                if name.endswith(_TEMP_SUFFIX):
                    continue
                if path.is_symlink():
                    rows[relative] = ("link", os.readlink(path))
                else:
                    rows[relative] = ("file", path.stat().st_size)
    return rows


def _same_bytes(left: Path, right: Path) -> bool:
    if left.stat().st_size != right.stat().st_size:
        return False
    with left.open("rb") as a, right.open("rb") as b:
        while True:
            chunk_a = a.read(1 << 20)
            chunk_b = b.read(1 << 20)
            if chunk_a != chunk_b:
                return False
            if not chunk_a:
                return True


def _make_writable(path: Path) -> None:
    """Windows refuses to replace or delete a read-only file."""
    try:
        mode = path.lstat().st_mode
    except OSError:
        return
    if not path.is_symlink() and not mode & stat.S_IWRITE:
        os.chmod(path, mode | stat.S_IWRITE)


def _rmtree(path: Path) -> None:
    def retry(function, failed, _info):
        _make_writable(Path(failed))
        function(failed)
    shutil.rmtree(path, onerror=retry)


def _remove(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        _make_writable(path)
        path.unlink()
    elif path.is_dir():
        _rmtree(path)


def tree_digest(base: Path, entries: Sequence[str] = SYNC_ENTRIES) -> dict:
    """Content identity of the synced input set (paths, types, bytes)."""
    rows = []
    for relative, kind in sorted(_inventory(base, entries).items()):
        if kind[0] == "file":
            rows.append([relative, "file", kind[1],
                         sha256_file(base / relative)])
        elif kind[0] == "link":
            rows.append([relative, "link", kind[1]])
        else:
            rows.append([relative, "dir"])
    encoded = json.dumps(rows, separators=(",", ":"),
                         ensure_ascii=True).encode("ascii")
    return {
        "entries": list(entries),
        "file_count": sum(row[1] == "file" for row in rows),
        "sha256": hashlib.sha256(encoded).hexdigest(),
    }


def mirror(source_root: Path, root: Path, *,
           entries: Sequence[str] = SYNC_ENTRIES,
           clock: Callable[[], float] = time.time,
           sleep: Callable[[float], None] = time.sleep) -> dict:
    """Make root/<entries> a byte-exact mirror of source_root/<entries>.

    Unchanged files keep their timestamps; new or changed files are written
    fresh (never copy2), strictly after the timestamp slop following the last
    UBT exit, so UBT's timestamp checks see exactly the content changes.
    """
    state = read_state(root)
    if state is None:
        raise BuildRootError(f"build root is not prepared: {root}")
    last_exit = state.get("last_ubt_exit_epoch")
    wait = 0.0
    if isinstance(last_exit, (int, float)):
        deadline = last_exit + TIMESTAMP_SLOP_SECONDS
        wait = max(0.0, deadline - clock())
        if wait:
            sleep(wait)
        if clock() < deadline - 0.05:
            raise BuildRootError(
                "clock did not advance past the last UBT exit; refusing to "
                "write build inputs whose timestamps UBT would misorder")
    source = _inventory(source_root, entries)
    target = _inventory(root, entries)
    counts = {"copied": 0, "replaced": 0, "deleted": 0, "unchanged": 0}
    # Deletions first, EXACT-case: on a case-insensitive filesystem a
    # case-only rename is then a delete followed by a fresh create, never a
    # write into the old name that the delete pass would remove again.
    for relative in sorted(set(target) - set(source), reverse=True):
        path = root / relative
        if path.exists() or path.is_symlink():
            _remove(path)
            counts["deleted"] += 1
    for relative in sorted(source):
        kind = source[relative]
        destination = root / relative
        current = target.get(relative)
        if kind[0] == "dir":
            if current is not None and current[0] != "dir":
                _remove(destination)
            destination.mkdir(parents=True, exist_ok=True)
            continue
        if kind[0] == "link":
            if current == kind:
                counts["unchanged"] += 1
                continue
            if destination.exists() or destination.is_symlink():
                _remove(destination)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.symlink_to(kind[1])
            counts["replaced" if current else "copied"] += 1
            continue
        origin = source_root / relative
        if (current is not None and current[0] == "file"
                and _same_bytes(origin, destination)):
            counts["unchanged"] += 1
            continue
        if current is not None and current[0] != "file":
            _remove(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.name + _TEMP_SUFFIX)
        # copyfile, not copy2: the new bytes must carry a NEW timestamp.
        shutil.copyfile(origin, temporary)
        shutil.copymode(origin, temporary)
        _make_writable(destination)
        os.replace(temporary, destination)
        counts["replaced" if current else "copied"] += 1
    source_digest = tree_digest(source_root, entries)
    mirrored_digest = tree_digest(root, entries)
    if source_digest != mirrored_digest:
        raise BuildRootError(
            "build root does not mirror the ISO build inputs after sync")
    return {**counts, "waited_seconds": round(wait, 3),
            "input_tree": source_digest}


def ubt_receipt_relative(platform_name: str, target: str,
                         configuration: str) -> str:
    """TargetReceipt.GetDefaultPath (System/TargetReceipt.cs:492-511)."""
    if configuration == "Development":
        name = f"{target}.target"
    else:
        name = f"{target}-{platform_name}-{configuration}.target"
    return f"Binaries/{platform_name}/{name}"


def _copy_with_digest(source: Path, destination: Path) -> tuple[int, str]:
    before = source.stat()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".wot-transfer-tmp")
    digest = hashlib.sha256()
    size = 0
    with source.open("rb") as reader, temporary.open("wb") as writer:
        for chunk in iter(lambda: reader.read(1 << 20), b""):
            digest.update(chunk)
            writer.write(chunk)
            size += len(chunk)
    shutil.copymode(source, temporary)
    _make_writable(destination)
    os.replace(temporary, destination)
    after = source.stat()
    if (before.st_size, before.st_mtime_ns) != (
            after.st_size, after.st_mtime_ns) or size != after.st_size:
        raise BuildRootError(f"build product changed while copied: {source}")
    if sha256_file(destination) != digest.hexdigest():
        raise BuildRootError(f"transferred product does not verify: "
                             f"{destination}")
    return size, digest.hexdigest()


def _project_relative(path: str, platform_name: str, *,
                      binaries_only: bool) -> PurePosixPath | None:
    if path.startswith("$(EngineDir)/"):
        return None
    if not path.startswith("$(ProjectDir)/"):
        raise BuildRootError(f"receipt path has no known root: {path!r}")
    relative = _safe_relative(path[len("$(ProjectDir)/"):])
    if binaries_only and relative.parts[:2] != ("Binaries", platform_name):
        raise BuildRootError(
            f"build product outside Binaries/{platform_name}: {path!r}")
    return relative


def transfer_products(root: Path, iso: Path, *, platform_name: str,
                      target: str, configuration: str,
                      receipt_path: Path, input_tree: dict | None = None
                      ) -> dict:
    """Copy one target's UBT products from the root into the ISO, verified.

    The UBT target receipt the build just wrote is the authority for what the
    product set is; every $(ProjectDir) build product is copied (engine-owned
    products of shared editor targets stay where UBT built them), the receipt
    itself follows so UAT stages from it unchanged, and each project runtime
    dependency that is not a product (WoT.uproject, plugin descriptors) must
    already be byte-identical in the ISO -- it is a mirrored input.
    """
    receipt_relative = ubt_receipt_relative(
        platform_name, target, configuration)
    ubt_receipt = root / receipt_relative
    try:
        document = json.loads(ubt_receipt.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BuildRootError(
            f"UBT target receipt is missing or malformed: {ubt_receipt}"
        ) from exc
    if (not isinstance(document, dict)
            or document.get("TargetName") != target
            or document.get("Platform") != platform_name
            or document.get("Configuration") != configuration
            or not isinstance(document.get("BuildProducts"), list)):
        raise BuildRootError(
            f"UBT target receipt identity is wrong: {ubt_receipt}")
    products = []
    product_paths = set()
    engine_owned = 0
    for row in document["BuildProducts"]:
        if not isinstance(row, dict) or not isinstance(row.get("Path"), str):
            raise BuildRootError("UBT receipt build product is malformed")
        relative = _project_relative(
            row["Path"], platform_name, binaries_only=True)
        if relative is None:
            engine_owned += 1
            continue
        source = root / Path(*relative.parts)
        if not source.is_file():
            raise BuildRootError(f"UBT build product is missing: {source}")
        if row.get("Type") in IMAGE_PRODUCT_TYPES:
            verify_image_complete(source, platform_name)
        size, digest = _copy_with_digest(
            source, iso / Path(*relative.parts))
        product_paths.add(relative.as_posix())
        products.append({
            "path": relative.as_posix(),
            "type": row.get("Type"),
            "size": size,
            "sha256": digest,
        })
    if not products:
        raise BuildRootError(
            f"UBT receipt names no project build products: {ubt_receipt}")
    receipt_size, receipt_digest = _copy_with_digest(
        ubt_receipt, iso / receipt_relative)
    verified = []
    for row in document.get("RuntimeDependencies", []) or []:
        if not isinstance(row, dict) or not isinstance(row.get("Path"), str):
            raise BuildRootError("UBT receipt runtime dependency is malformed")
        relative = _project_relative(
            row["Path"], platform_name, binaries_only=False)
        if relative is None or relative.as_posix() in product_paths:
            continue
        in_root = root / Path(*relative.parts)
        in_iso = iso / Path(*relative.parts)
        if (not in_root.is_file() or not in_iso.is_file()
                or sha256_file(in_root) != sha256_file(in_iso)):
            raise BuildRootError(
                "project runtime dependency differs between the build root "
                f"and the ISO: {relative.as_posix()}")
        verified.append({"path": relative.as_posix(),
                         "sha256": sha256_file(in_iso)})
    version = document.get("Version") if isinstance(
        document.get("Version"), dict) else {}
    receipt = {
        "format": TRANSFER_FORMAT,
        "platform": platform_name,
        "target": target,
        "configuration": configuration,
        "build_root": str(root),
        "ubt_receipt": {
            "path": receipt_relative,
            "size": receipt_size,
            "sha256": receipt_digest,
            "build_id": version.get("BuildId"),
        },
        "input_tree": input_tree,
        "products": products,
        "engine_owned_products": engine_owned,
        "runtime_dependencies_verified": verified,
    }
    write_json(receipt_path, receipt)
    return {
        "path": str(receipt_path),
        "sha256": sha256_file(receipt_path),
        "receipt": receipt,
    }


def product_digest(transfer: dict, relative: str) -> str | None:
    for row in transfer.get("products", []):
        if row.get("path") == relative:
            return row.get("sha256")
    return None


def modules_build_id(path: Path) -> str | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    build_id = value.get("BuildId") if isinstance(value, dict) else None
    return build_id if isinstance(build_id, str) and build_id else None
