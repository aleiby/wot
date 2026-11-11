#!/usr/bin/env python3
"""Run the guarded release's closed first-run acceptance matrix.

The harness is intentionally file judged.  Packaged guarded clients are always
launched with ``-unattended`` and no debugger is attached.  Download, cache,
boot-failure, process, loopback-request, and net-test snapshot evidence is
folded into one atomic canonical receipt.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import errno
import hashlib
import http.client
import json
import os
import plistlib
import random
import re
import select
import shlex
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import zipfile
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path, PureWindowsPath
from typing import Callable, Iterable, Optional, Sequence

_IS_WINDOWS = os.name == "nt"


class _ProcessBasicInformation(ctypes.Structure):
    _fields_ = [
        ("Reserved1", ctypes.c_void_p),
        ("PebBaseAddress", ctypes.c_void_p),
        ("Reserved2", ctypes.c_void_p * 2),
        ("UniqueProcessId", ctypes.c_size_t),
        ("InheritedFromUniqueProcessId", ctypes.c_size_t),
    ]


class _JobObjectBasicLimitInformation(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64),
        ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", ctypes.c_uint32),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", ctypes.c_uint32),
        ("Affinity", ctypes.c_size_t),
        ("PriorityClass", ctypes.c_uint32),
        ("SchedulingClass", ctypes.c_uint32),
    ]


class _IoCounters(ctypes.Structure):
    _fields_ = [
        ("ReadOperationCount", ctypes.c_uint64),
        ("WriteOperationCount", ctypes.c_uint64),
        ("OtherOperationCount", ctypes.c_uint64),
        ("ReadTransferCount", ctypes.c_uint64),
        ("WriteTransferCount", ctypes.c_uint64),
        ("OtherTransferCount", ctypes.c_uint64),
    ]


class _JobObjectExtendedLimitInformation(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", _JobObjectBasicLimitInformation),
        ("IoInfo", _IoCounters),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


class _JobObjectAssociateCompletionPort(ctypes.Structure):
    _fields_ = [
        ("CompletionKey", ctypes.c_void_p),
        ("CompletionPort", ctypes.c_void_p),
    ]


class _FileId128(ctypes.Structure):
    _fields_ = [("Identifier", ctypes.c_ubyte * 16)]


class _FileIdInfo(ctypes.Structure):
    _fields_ = [
        ("VolumeSerialNumber", ctypes.c_uint64),
        ("FileId", _FileId128),
    ]


class _FileTime(ctypes.Structure):
    _fields_ = [
        ("low", ctypes.c_uint32),
        ("high", ctypes.c_uint32),
    ]


if _IS_WINDOWS:
    import msvcrt
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
    _create_file = _kernel32.CreateFileW
    _create_file.argtypes = [
        ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
        ctypes.c_void_p,
    ]
    _create_file.restype = ctypes.c_void_p
    _get_file_information_by_handle_ex = (
        _kernel32.GetFileInformationByHandleEx)
    _get_file_information_by_handle_ex.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
    ]
    _get_file_information_by_handle_ex.restype = ctypes.c_int
    _close_handle = _kernel32.CloseHandle
    _close_handle.argtypes = [ctypes.c_void_p]
    _close_handle.restype = ctypes.c_int
    _open_process = _kernel32.OpenProcess
    _open_process.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    _open_process.restype = ctypes.c_void_p
    _query_full_process_image_name = _kernel32.QueryFullProcessImageNameW
    _query_full_process_image_name.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_wchar_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    _query_full_process_image_name.restype = ctypes.c_int
    _terminate_process = _kernel32.TerminateProcess
    _terminate_process.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    _terminate_process.restype = ctypes.c_int
    _wait_for_single_object = _kernel32.WaitForSingleObject
    _wait_for_single_object.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    _wait_for_single_object.restype = ctypes.c_uint32
    _create_job_object = _kernel32.CreateJobObjectW
    _create_job_object.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    _create_job_object.restype = ctypes.c_void_p
    _set_information_job_object = _kernel32.SetInformationJobObject
    _set_information_job_object.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
    ]
    _set_information_job_object.restype = ctypes.c_int
    _create_io_completion_port = _kernel32.CreateIoCompletionPort
    _create_io_completion_port.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32,
    ]
    _create_io_completion_port.restype = ctypes.c_void_p
    _get_queued_completion_status = _kernel32.GetQueuedCompletionStatus
    _get_queued_completion_status.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_void_p),
        ctypes.c_uint32,
    ]
    _get_queued_completion_status.restype = ctypes.c_int
    _post_queued_completion_status = _kernel32.PostQueuedCompletionStatus
    _post_queued_completion_status.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_size_t, ctypes.c_void_p,
    ]
    _post_queued_completion_status.restype = ctypes.c_int
    _assign_process_to_job_object = _kernel32.AssignProcessToJobObject
    _assign_process_to_job_object.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _assign_process_to_job_object.restype = ctypes.c_int
    _get_exit_code_process = _kernel32.GetExitCodeProcess
    _get_exit_code_process.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32),
    ]
    _get_exit_code_process.restype = ctypes.c_int
    _get_process_times = _kernel32.GetProcessTimes
    _get_process_times.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_FileTime), ctypes.POINTER(_FileTime),
        ctypes.POINTER(_FileTime), ctypes.POINTER(_FileTime),
    ]
    _get_process_times.restype = ctypes.c_int
    _nt_query_information_process = _ntdll.NtQueryInformationProcess
    _nt_query_information_process.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    _nt_query_information_process.restype = ctypes.c_long


def _read_shared_bytes(path: Path) -> bytes:
    """Read a file the game may open at the same moment, without blocking it.

    The game opens its heartbeat and update markers with DELETE access
    (OpenOrCreateNativeFileAtReadWriteNoFollow). On Windows, Python's open()
    shares only read and write, so a funnel poll that overlapped one of those
    opens failed it with a sharing violation; the heartbeat writer then drops
    records for its retry interval, and one discovery run lost a
    second launch's Ready and frontend-handoff records that way. Sharing
    delete as well leaves every game open free to succeed.
    """
    if not _IS_WINDOWS:
        return path.read_bytes()
    with os.fdopen(_open_shared_read_descriptor(path), "rb") as stream:
        return stream.read()


def _open_shared_read_descriptor(path: Path) -> int:
    """Windows only: a read-only descriptor that shares read, write and delete."""
    handle = _create_file(
        str(path),
        0x80000000,  # GENERIC_READ
        0x1 | 0x2 | 0x4,  # FILE_SHARE_READ | WRITE | DELETE
        None, 3,  # OPEN_EXISTING
        0x80,  # FILE_ATTRIBUTE_NORMAL
        None)
    if handle == ctypes.c_void_p(-1).value:
        error = ctypes.get_last_error()
        if error in {2, 3}:
            raise FileNotFoundError(errno.ENOENT, os.strerror(errno.ENOENT),
                                    str(path))
        failure = ctypes.WinError(error)
        failure.filename = str(path)
        raise failure
    try:
        return msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except OSError:
        _close_handle(handle)
        raise


def _read_shared_text(path: Path, encoding: str) -> str:
    # Same newline handling as Path.read_text (universal newlines).
    text = _read_shared_bytes(path).decode(encoding)
    return text.replace("\r\n", "\n").replace("\r", "\n")


REPO = Path(__file__).resolve().parents[2]
DIST = Path(__file__).resolve().parent
if str(DIST) not in sys.path:
    sys.path.insert(0, str(DIST))

import bootstrap  # noqa: E402
import build_stripped_pak  # noqa: E402
import release_formats as formats  # noqa: E402
import update_funnel_fixtures as update_fixtures  # noqa: E402

# The closed neutral-support set is owned by the packaging pipeline; a copy
# here drifted (6 roots vs the pipeline's 27+) and rejected every current
# release. Read it from the single source.
NEUTRAL_SUPPORT_ROOTS = build_stripped_pak.NEUTRAL_SUPPORT_ROOT_PACKAGES
NEUTRAL_SUPPORT_HARD_DEPENDENCIES = (
    build_stripped_pak.NEUTRAL_SUPPORT_HARD_DEPENDENCIES)


RECEIPT_FORMAT = formats.FUNNEL_RECEIPT_FORMAT
CELL_RECEIPT_FORMAT = formats.FUNNEL_CELL_RECEIPT_FORMAT
REQUEST_LOG_FORMAT = "wot-funnel-loopback-request-v1"
SESSION_EVIDENCE_FORMAT = "wot-funnel-session-evidence-v2"
GUARDED_RELEASE_FORMAT = "wot-guarded-release-v1"
PRODUCT_RECEIPT_FORMAT = "wot-guarded-product-receipt-v1"
SOURCE_SNAPSHOT_FORMAT = "wot-source-snapshot-v1"
COOKED_CONTAINER_RECEIPT_FORMAT = (
    "wot-guarded-cooked-container-inventory-v2")
NEUTRAL_SUPPORT_RECEIPT_FORMAT = "wot-neutral-support-asset-receipt-v1"
ZEN_RECEIPT_FORMAT = "wot-guarded-zen-lifetime-v1"
ZEN_SPONSOR_ENDPOINT = "http://127.0.0.1:18558"
ZEN_NO_AUTO_LAUNCH = "-NoZenAutoLaunch=127.0.0.1:18558"
WIN64_SERIAL_LOCAL_UBT_ARGUMENTS = (
    "-MaxParallelActions=1", "-NoXGE", "-NoFASTBuild",
    "-NoSNDBS", "-NoUBA", "-UBADisableRemote",
)
BOOT_FAILURE_LEAF = "boot-failure-v1.json"
HEARTBEAT_GLOB = "bootstrap-heartbeat-v1.*.*.jsonl"
HEARTBEAT_NAME = re.compile(
    r"^bootstrap-heartbeat-v1\.([1-9][0-9]*)\.([^.]+)\.jsonl$")
CACHED_HANDOFF_LIMIT_SECONDS = 3.0
INSTALLER_NAME = bootstrap.INSTALLER_NAME
INSTALLER_SIZE = bootstrap.INSTALLER_SIZE
INSTALLER_SHA256 = bootstrap.INSTALLER_SHA256
DEFAULT_CACHE = REPO / "Saved" / "FunnelCache" / INSTALLER_NAME
DEFAULT_EVIDENCE_ROOT = (
    REPO / "Saved" / "ReleaseEvidence" / "funnel" / "acceptance"
)
TEST_ANNOUNCEMENT_URL_ENV = "WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL"
TEST_RELEASE_BASE_URL_ENV = "WOT_BOOTSTRAP_TEST_RELEASE_BASE_URL"
TEST_DOWNLOADS_DIR_ENV = "WOT_BOOTSTRAP_TEST_DOWNLOADS_DIR"
TEST_PICKED_INSTALLER_ENV = "WOT_BOOTSTRAP_TEST_PICKED_INSTALLER"
REAL_PREFERENCES_HASH_LIMIT = 16 * 1024 * 1024
MAC_CLEANUP_QUIET_POLLS = 3
MAC_CLEANUP_MAX_PASSES = 32

CELL_NAMES = formats.FUNNEL_CELL_NAMES
CELL_INDEX = {name: index for index, name in enumerate(CELL_NAMES)}
CELL_VERDICTS = formats.FUNNEL_CELL_VERDICTS
RECEIPT_KEYS = formats.FUNNEL_RECEIPT_KEYS
RELEASE_RECEIPT_KEYS = formats.FUNNEL_RELEASE_KEYS
INSTALLER_RECEIPT_KEYS = formats.FUNNEL_INSTALLER_KEYS
CELL_KEYS = formats.FUNNEL_CELL_KEYS


class FunnelFailure(RuntimeError):
    """The acceptance contract could not be proved."""


class _CachedHandoffIncomplete(FunnelFailure):
    """The launched process has not durably published the complete gate yet."""


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _canonical_json_bytes(value: object) -> bytes:
    return (json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ) + "\n").encode("utf-8")


def _canonical_release_bytes(value: object) -> bytes:
    """Match build_stripped_pak.py's release/source identity serializer."""
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")


def _mac_real_support_roots() -> tuple[Path, Path, Path]:
    """Return the login account's real support roots, ignoring test HOME."""
    if sys.platform != "darwin":
        raise FunnelFailure("real Mac preferences can only be guarded on macOS")
    try:
        import pwd
        home = Path(pwd.getpwuid(os.getuid()).pw_dir)
    except (ImportError, KeyError, OSError) as exc:
        raise FunnelFailure(
            f"cannot resolve the real Mac login home directory: {exc}") from exc
    if not home.is_absolute():
        raise FunnelFailure("the real Mac login home directory is not absolute")
    support = home / "Library" / "Application Support"
    return (support / "WoT Reborn", support / "WoT",
            support / "Epic" / "WoT")


def _mac_real_support_inventory(
    roots: Sequence[Path] | None = None,
) -> dict:
    """Inventory without following links; any unreadable entry fails closed."""
    selected_roots = tuple(roots) if roots is not None else _mac_real_support_roots()
    inventory = []

    def entry_row(path: Path, relative: str) -> tuple[dict, bool]:
        try:
            details = path.lstat()
        except OSError as exc:
            raise FunnelFailure(
                f"cannot inspect real Mac preference entry {path}: {exc}") from exc
        is_directory = stat.S_ISDIR(details.st_mode)
        is_file = stat.S_ISREG(details.st_mode)
        kind = ("directory" if is_directory else "file" if is_file
                else "symlink" if stat.S_ISLNK(details.st_mode) else "other")
        if not is_directory and not is_file:
            raise FunnelFailure(
                "real Mac preference inventory refuses non-regular entry "
                f"{path} ({kind})")
        row = {
            "mtime_ns": details.st_mtime_ns,
            "name": relative,
            "sha256": None,
            "size": details.st_size,
            "type": kind,
        }
        if is_file and details.st_size < REAL_PREFERENCES_HASH_LIMIT:
            descriptor = None
            try:
                descriptor = os.open(
                    path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
                digest = hashlib.sha256()
                with os.fdopen(descriptor, "rb") as handle:
                    descriptor = None
                    while chunk := handle.read(1024 * 1024):
                        digest.update(chunk)
                    rechecked = os.fstat(handle.fileno())
                row["sha256"] = digest.hexdigest()
            except OSError as exc:
                raise FunnelFailure(
                    f"cannot hash real Mac preference file {path}: {exc}") from exc
            finally:
                if descriptor is not None:
                    os.close(descriptor)
            if (rechecked.st_dev != details.st_dev
                    or rechecked.st_ino != details.st_ino
                    or rechecked.st_size != details.st_size
                    or rechecked.st_mtime_ns != details.st_mtime_ns
                    or not stat.S_ISREG(rechecked.st_mode)):
                raise FunnelFailure(
                    f"real Mac preference file changed during inventory: {path}")
        return row, is_directory

    def visit(directory: Path, prefix: str, rows: list[dict]) -> None:
        try:
            children = sorted(os.scandir(directory), key=lambda item: item.name)
        except OSError as exc:
            raise FunnelFailure(
                f"cannot list real Mac preference directory {directory}: {exc}") from exc
        for child in children:
            relative = f"{prefix}/{child.name}" if prefix else child.name
            row, is_directory = entry_row(Path(child.path), relative)
            rows.append(row)
            if is_directory:
                visit(Path(child.path), relative, rows)

    for root in selected_roots:
        absolute = root.expanduser().absolute()
        try:
            absolute.lstat()
        except FileNotFoundError:
            inventory.append({
                "entries": [], "exists": False, "root": str(absolute)})
            continue
        except OSError as exc:
            raise FunnelFailure(
                f"cannot inspect real Mac preference root {absolute}: {exc}") from exc
        rows = []
        root_row, is_directory = entry_row(absolute, ".")
        rows.append(root_row)
        if is_directory:
            visit(absolute, "", rows)
        inventory.append({
            "entries": rows, "exists": True, "root": str(absolute)})
    return {
        "format": "wot-real-mac-preferences-inventory-v1",
        "hash_limit_bytes": REAL_PREFERENCES_HASH_LIMIT,
        "roots": inventory,
    }


def _pretty_json_bytes(value: object) -> bytes:
    """Match the pipeline's on-disk receipt writer."""
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _validate_release_archive_alternate_bind_absence(asset: Path) -> None:
    try:
        with zipfile.ZipFile(asset, "r") as archive:
            for row in archive.infolist():
                if row.is_dir():
                    continue
                normalized = row.filename.replace("\\", "/").casefold()
                leaf = normalized.rsplit("/", 1)[-1]
                forbidden_name = (
                    normalized.endswith(".glb")
                    or normalized.endswith(".meta.json")
                    or leaf == "authenticated-alternate-bind.json")
                with archive.open(row, "r") as handle:
                    prefix = handle.read(min(row.file_size, 16 * 1024 * 1024))
                forbidden_content = (
                    prefix.startswith(b"glTF")
                    or b"wot-authenticated-alternate-bind-v1" in prefix
                    or b'"authenticated_alternate_bind"' in prefix)
                if forbidden_name or forbidden_content:
                    raise FunnelFailure(
                        "release archive contains forbidden alternate-bind "
                        f"source/receipt artifact: {row.filename}")
    except (OSError, zipfile.BadZipFile, RuntimeError) as exc:
        raise FunnelFailure(
            f"cannot inspect guarded release archive {asset}: {exc}") from exc


def _atomic_write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(
        f".{path.name}.{os.getpid()}.{time.monotonic_ns()}.tmp"
    )
    data = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    descriptor = os.open(
        temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
    )
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        if os.name != "nt":
            # Directory fsync is the POSIX half of durable publication;
            # Windows cannot open a directory through os.open at all
            # (PermissionError), and NTFS journals the rename metadata.
            directory = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def _read_json(path: Path, what: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FunnelFailure(f"cannot read {what} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise FunnelFailure(f"{what} is not a JSON object: {path}")
    return value


def _parse_heartbeat_timestamp(value: object, what: str) -> float:
    if not isinstance(value, str) or not value:
        raise FunnelFailure(f"{what} has no UTC timestamp")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if parsed.tzinfo is None or parsed.utcoffset() is None:
            raise ValueError("timestamp is not timezone-aware")
        return parsed.timestamp()
    except (OSError, OverflowError, ValueError) as exc:
        raise FunnelFailure(f"{what} has an invalid UTC timestamp") from exc


def _read_heartbeat_records(
    root: Path, prelaunch_files: frozenset[Path], *,
    expected_pid: int | None = None, expected_epoch: int | None = None,
) -> list[dict]:
    """Read complete valid records from process files created after launch."""
    if (expected_pid is not None
            and (type(expected_pid) is not int or expected_pid <= 0)):
        raise FunnelFailure("heartbeat process id is invalid")
    if (expected_epoch is not None
            and (type(expected_epoch) is not int or expected_epoch < 0)):
        raise FunnelFailure("heartbeat epoch is invalid")
    try:
        current = {path for path in root.glob(HEARTBEAT_GLOB) if path.is_file()}
    except OSError as exc:
        raise FunnelFailure(
            f"cannot discover bootstrap heartbeats in {root}: {exc}") from exc
    records = []
    for path in sorted(current - set(prelaunch_files), key=lambda item: item.name):
        match = HEARTBEAT_NAME.fullmatch(path.name)
        if match is None:
            continue
        file_pid = int(match.group(1))
        if expected_pid is not None and file_pid != expected_pid:
            continue
        try:
            data = _read_shared_bytes(path)
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise FunnelFailure(
                f"cannot read bootstrap heartbeat {path}: {exc}") from exc
        complete = data[:data.rfind(b"\n") + 1] if b"\n" in data else b""
        for raw in complete.splitlines():
            if not raw.strip():
                continue
            try:
                value = json.loads(raw)
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if (not isinstance(value, dict)
                    or type(value.get("pid")) is not int
                    or value["pid"] != file_pid
                    or type(value.get("epoch")) is not int
                    or value["epoch"] < 0
                    or not isinstance(value.get("event"), str)
                    or not value["event"]
                    or not isinstance(value.get("phase"), str)
                    or not value["phase"]
                    or ("detail" in value
                        and not isinstance(value["detail"], str))
                    or ("completed" in value
                        and (type(value["completed"]) is not int
                             or value["completed"] < 0))
                    or ("total" in value
                        and (type(value["total"]) is not int
                             or value["total"] < 0))):
                continue
            try:
                _parse_heartbeat_timestamp(
                    value.get("ts_utc"), "heartbeat record")
            except FunnelFailure:
                continue
            if expected_epoch is not None and value["epoch"] != expected_epoch:
                continue
            records.append(value)
    return records


def _validate_cold_cache_provenance(value: object) -> dict:
    keys = {"classification", "evidence", "normal_reboot_utc"}
    if not isinstance(value, dict) or set(value) != keys:
        raise FunnelFailure("cold_cache_provenance schema is not exact")
    classification = value.get("classification")
    evidence = value.get("evidence")
    reboot = value.get("normal_reboot_utc")
    if not isinstance(evidence, str) or not evidence:
        raise FunnelFailure("cold_cache_provenance evidence is absent")
    if classification == "warm-repeat":
        if evidence != "same-invocation-profile" or reboot is not None:
            raise FunnelFailure("warm cold_cache_provenance is not auditable")
    elif classification == "cold-post-reboot":
        raise FunnelFailure(
            "cold_cache_provenance requires Task 7 OS-derived reboot evidence"
        )
    else:
        raise FunnelFailure("cold_cache_provenance classification is invalid")
    return dict(value)


def front_end_evidence(records: list[dict], pid: int) -> Optional[dict]:
    """Ready then frontend-handoff in one bootstrap epoch (the funnel's rule,
    run_first_run_funnel._cached_handoff_receipt, minus its 3 s cached budget)."""
    launched = [record for record in records if record.get("pid") == pid]
    ready = [(index, record) for index, record in enumerate(launched)
             if record.get("event") == "phase-ready"
             and record.get("phase") == "Ready"]
    for ready_index, ready_record in ready:
        epoch = ready_record.get("epoch")
        if not isinstance(epoch, int) or epoch <= 0:
            continue
        for handoff in launched[ready_index + 1:]:
            if (handoff.get("event") == "frontend-handoff"
                    and handoff.get("phase") == "Ready"
                    and handoff.get("epoch") == epoch):
                return {"ready": ready_record, "frontend_handoff": handoff,
                        "epoch": epoch}
    return None


def _cached_handoff_receipt(
    records: list[dict], *, expected_pid: int, process_started_epoch: float,
    network_bytes: int, cache_writes: int,
    cold_cache_provenance: object,
) -> dict:
    provenance = _validate_cold_cache_provenance(cold_cache_provenance)
    counters = {
        "network_bytes": network_bytes,
        "cache_writes": cache_writes,
    }
    for name, count in counters.items():
        if not isinstance(count, int) or isinstance(count, bool) or count < 0:
            raise FunnelFailure(f"{name} is not a nonnegative integer")
        if count != 0:
            raise FunnelFailure(f"cached launch {name} must be zero, found {count}")
    if not isinstance(expected_pid, int) or expected_pid <= 0:
        raise FunnelFailure("cached launch process id is invalid")
    if not isinstance(process_started_epoch, (int, float)):
        raise FunnelFailure("cached launch process start is invalid")

    launched = [record for record in records if record.get("pid") == expected_pid]
    checking = [
        (index, record) for index, record in enumerate(launched)
        if record.get("event") == "phase-begin"
        and record.get("phase") == "CheckingOriginalGameData"
    ]
    ready = [
        (index, record) for index, record in enumerate(launched)
        if record.get("event") == "phase-ready"
        and record.get("phase") == "Ready"
    ]
    handoff = [
        (index, record) for index, record in enumerate(launched)
        if record.get("event") == "frontend-handoff"
        and record.get("phase") == "Ready"
    ]
    if not checking:
        raise _CachedHandoffIncomplete(
            "missing complete CheckingOriginalGameData entry for launched process"
        )
    if not ready:
        raise _CachedHandoffIncomplete(
            "missing complete Ready record for launched process"
        )
    if not handoff:
        raise _CachedHandoffIncomplete(
            "missing complete frontend-handoff record for launched process"
        )
    checking_index, checking_record = checking[-1]
    ready_index, ready_record = ready[-1]
    handoff_index, handoff_record = handoff[-1]
    if not checking_index < ready_index < handoff_index:
        raise FunnelFailure(
            "cached heartbeat order must be CheckingOriginalGameData, Ready, "
            "frontend-handoff"
        )
    ready_epoch = ready_record.get("epoch")
    if (
        not isinstance(ready_epoch, int) or ready_epoch <= 0
        or handoff_record.get("epoch") != ready_epoch
    ):
        raise FunnelFailure(
            "Ready and frontend-handoff do not share one bootstrap epoch"
        )
    checking_epoch = checking_record.get("epoch")
    if (
        not isinstance(checking_epoch, int) or checking_epoch <= 0
        or checking_epoch >= ready_epoch
    ):
        raise FunnelFailure(
            "CheckingOriginalGameData bootstrap epoch does not precede Ready"
        )

    checking_timestamp = _parse_heartbeat_timestamp(
        checking_record.get("ts_utc"), "CheckingOriginalGameData record"
    )
    ready_timestamp = _parse_heartbeat_timestamp(
        ready_record.get("ts_utc"), "Ready record"
    )
    handoff_timestamp = _parse_heartbeat_timestamp(
        handoff_record.get("ts_utc"), "frontend-handoff record"
    )
    ready_seconds = round(ready_timestamp - checking_timestamp, 6)
    handoff_seconds = round(handoff_timestamp - checking_timestamp, 6)
    process_start_seconds = round(handoff_timestamp - process_started_epoch, 6)
    if checking_timestamp < process_started_epoch:
        raise FunnelFailure(
            "CheckingOriginalGameData predates the launched process"
        )
    if ready_seconds < 0 or handoff_seconds < ready_seconds:
        raise FunnelFailure("cached heartbeat timestamps are out of order")
    if process_start_seconds < 0:
        raise FunnelFailure("frontend-handoff predates the launched process")
    return {
        "ready_seconds": ready_seconds,
        "frontend_handoff_seconds": handoff_seconds,
        "process_start_seconds": process_start_seconds,
        **counters,
        "cold_cache_provenance": provenance,
        "heartbeat_pid": expected_pid,
        "heartbeat_epoch": ready_epoch,
        "latency_warning": (
            "frontend-handoff exceeded 3.0 seconds"
            if process_start_seconds > CACHED_HANDOFF_LIMIT_SECONDS else None),
    }


def _wait_for_cached_handoff(
    client: "ClientProcess", timeout: float, *, network_bytes: int,
    cache_writes: int,
    cold_cache_provenance: object,
) -> dict:
    deadline = time.monotonic() + timeout
    last_incomplete = "no heartbeat records"
    while time.monotonic() < deadline:
        client.poll_failure()
        records = _read_heartbeat_records(
            client.heartbeat_path, client.heartbeat_offset,
            expected_pid=client.process.pid)
        try:
            receipt = _cached_handoff_receipt(
                records, expected_pid=client.process.pid,
                process_started_epoch=client.started_epoch,
                network_bytes=network_bytes,
                cache_writes=cache_writes,
                cold_cache_provenance=cold_cache_provenance,
            )
        except _CachedHandoffIncomplete as exc:
            last_incomplete = str(exc)
            time.sleep(0.01)
            continue
        receipt["heartbeat_records"] = records
        receipt["heartbeat_path"] = str(client.heartbeat_path)
        return receipt
    raise FunnelFailure(
        f"timed out waiting for durable cached bootstrap handoff: {last_incomplete}"
    )


def _run_capture(command: list[str], cwd: Path | None = None) -> dict:
    result = subprocess.run(
        command, cwd=str(cwd) if cwd else None, capture_output=True, text=True
    )
    return {
        "command": command,
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def _git_bytes(arguments: list[str], what: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(REPO), *arguments], capture_output=True
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise FunnelFailure(f"cannot capture {what}: {detail}")
    return result.stdout


def _current_source_snapshot() -> dict:
    """Reproduce the pipeline's Source/Tools snapshot identity."""
    paths = build_stripped_pak.SOURCE_SNAPSHOT_PATHS
    commit = _git_bytes(["rev-parse", "--short=9", "HEAD"], "HEAD").decode(
        "ascii"
    ).strip()
    tracked = _git_bytes(
        ["diff", "--binary", "--no-ext-diff", "HEAD", "--", *paths],
        "the Source/Tools tracked diff",
    )
    raw = _git_bytes(
        ["ls-files", "--others", "--exclude-standard", "-z", "--", *paths],
        "the Source/Tools untracked inventory",
    )
    untracked = []
    for encoded in sorted(item for item in raw.split(b"\0") if item):
        try:
            relative = encoded.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise FunnelFailure(
                "an untracked Source/Tools path is not UTF-8"
            ) from exc
        path = REPO / relative
        if path.is_symlink():
            untracked.append({
                "path": relative, "type": "symlink",
                "target": os.readlink(path),
            })
        elif path.is_file():
            untracked.append({
                "path": relative, "type": "file",
                "size": path.stat().st_size,
                "sha256": _sha256_file(path),
            })
        else:
            raise FunnelFailure(
                f"untracked source object is not a file/link: {path}"
            )
    identity = {
        "format": SOURCE_SNAPSHOT_FORMAT,
        "git_commit": commit,
        "paths": list(paths),
        "tracked_diff_size": len(tracked),
        "tracked_diff_sha256": hashlib.sha256(tracked).hexdigest(),
        "untracked": untracked,
    }
    identity["sha256"] = hashlib.sha256(
        _canonical_release_bytes(identity)
    ).hexdigest()
    return identity


def _safe_leaf(value: object, what: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or Path(value).name != value
        or "/" in value
        or "\\" in value
    ):
        raise FunnelFailure(f"{what} is not a safe leaf name")
    return value


def _validate_digest(value: object, what: str) -> str:
    if (
        not isinstance(value, str)
        or re.fullmatch(r"[0-9a-f]{64}", value) is None
    ):
        raise FunnelFailure(f"{what} is not a lowercase SHA-256")
    return value


def _validate_neutral_support_proof(value: object) -> dict:
    required = {
        "format", "authority_format", "authority_relative_path",
        "authority_sha256", "asset_count", "roots", "hard_dependencies",
        "assets", "assets_sha256",
    }
    if not isinstance(value, dict) or set(value) != required:
        raise FunnelFailure("release neutral_support_authority is malformed")
    packages = (*NEUTRAL_SUPPORT_ROOTS, *NEUTRAL_SUPPORT_HARD_DEPENDENCIES)
    assets = value.get("assets")
    if (value.get("format") != NEUTRAL_SUPPORT_RECEIPT_FORMAT
            or value.get("authority_format")
                != "wot-neutral-support-asset-authority-v1"
            or value.get("authority_relative_path")
                != "Tools/dist/neutral-support-assets-v1.json"
            or value.get("asset_count") != len(packages)
            or value.get("roots") != list(NEUTRAL_SUPPORT_ROOTS)
            or value.get("hard_dependencies")
                != list(NEUTRAL_SUPPORT_HARD_DEPENDENCIES)
            or not isinstance(assets, list)
            or [row.get("package") if isinstance(row, dict) else None
                for row in assets] != list(packages)):
        raise FunnelFailure(
            "release neutral_support_authority does not prove exact assets")
    for package, row in zip(packages, assets):
        expected_relative = (
            "Content" + package.removeprefix("/Game") + ".uasset")
        if (set(row) != {"package", "relative_path", "sha256", "size"}
                or row.get("relative_path") != expected_relative
                or not isinstance(row.get("size"), int)
                or row["size"] <= 0):
            raise FunnelFailure(
                "release neutral_support_authority asset row is malformed")
        _validate_digest(row.get("sha256"), "neutral support asset digest")
    _validate_digest(
        value.get("authority_sha256"), "neutral support authority digest")
    assets_sha256 = _validate_digest(
        value.get("assets_sha256"), "neutral support asset-tree digest")
    if assets_sha256 != hashlib.sha256(
            _canonical_release_bytes(assets)).hexdigest():
        raise FunnelFailure(
            "release neutral_support_authority asset-tree digest is invalid")
    authority = {
        "format": value["authority_format"],
        "roots": value["roots"],
        "hard_dependencies": value["hard_dependencies"],
        "assets": assets,
    }
    if value["authority_sha256"] != hashlib.sha256(
            _pretty_json_bytes(authority)).hexdigest():
        raise FunnelFailure(
            "release neutral_support_authority authority digest is invalid")
    return value


def _validate_release_unreal_command(
        command: object, kind: str, platform_name: str) -> None:
    if not isinstance(command, list) or not command or not all(
            isinstance(token, str) for token in command):
        raise FunnelFailure("release Zen v3 Unreal command is malformed")
    if kind == "editor":
        ddc = [token for token in command
               if token.casefold().startswith("-ddc=")]
        no_auto = [token for token in command
                   if token.casefold().startswith("-nozenautolaunch=")]
        if (ddc != ["-ddc=(ZenLocal)"]
                or no_auto != [ZEN_NO_AUTO_LAUNCH]
                or any("127.0.0.1:8558" in token.casefold()
                       for token in command)):
            raise FunnelFailure(
                "release Zen v3 editor command is not isolated")
        return
    if kind != "cook":
        raise FunnelFailure("release Zen v3 Unreal command kind is invalid")
    option_rows = [
        token for token in command
        if token.startswith("-AdditionalCookerOptions=")]
    if len(option_rows) != 1:
        raise FunnelFailure(
            "release Zen v3 cook has no exact PACKAGE proof")
    tokens = option_rows[0].split("=", 1)[1].split()
    outer = [token for token in command if token not in option_rows]
    ddc = [token for token in tokens
           if token.casefold().startswith("-ddc=")]
    no_auto = [token for token in tokens
               if token.casefold().startswith("-nozenautolaunch=")]
    packages = [token for token in tokens
                if token.casefold().startswith("-package=")]
    expected_package = "-PACKAGE=" + "+".join(NEUTRAL_SUPPORT_ROOTS)
    if (tokens.count("-skipzenstore") != 1
            or ddc != ["-ddc=(ZenLocal)"]
            or no_auto != [ZEN_NO_AUTO_LAUNCH]
            or packages != [expected_package]
            or any(token.casefold().startswith(
                ("-ddc=", "-nozenautolaunch=", "-package="))
                for token in outer)
            or any("127.0.0.1:8558" in token.casefold()
                   for token in outer)):
        raise FunnelFailure(
            "release Zen v3 cook does not preserve the exact PACKAGE vector")
    if platform_name == "Win64":
        expected_ubt = (
            "-UbtArgs=" + " ".join(WIN64_SERIAL_LOCAL_UBT_ARGUMENTS))
        ubt = [token for token in command
               if token.casefold().startswith("-ubtargs")]
        control_prefixes = (
            "-maxparallelactions", "-xge", "-noxge",
            "-fastbuild", "-nofastbuild", "-sndbs", "-nosndbs",
            "-uba", "-nouba", "-allowremoteexecutor",
        )
        outer_controls = [
            token for token in command
            if token.casefold().startswith(control_prefixes)]
        if ubt != [expected_ubt] or outer_controls != ["-NoXGE"]:
            raise FunnelFailure(
                "release Win64 cook does not preserve the exact "
                "serial/local UBT contract")


def _validate_zen_lifetime_proof(
        value: object, platform_name: str) -> dict:
    if not isinstance(value, dict) or set(value) != {
            "receipt", "receipt_sha256"}:
        raise FunnelFailure("release Zen v3 lifetime proof is malformed")
    receipt = value.get("receipt")
    if not isinstance(receipt, dict):
        raise FunnelFailure("release Zen v3 lifetime receipt is missing")
    expected_sha = hashlib.sha256(_pretty_json_bytes(receipt)).hexdigest()
    if value.get("receipt_sha256") != expected_sha:
        raise FunnelFailure("release Zen v3 lifetime receipt digest is invalid")
    required_receipt_keys = {
        "format", "mechanism", "unreal_isolation_switches", "endpoint",
        "owner", "sponsor", "version", "before", "endpoint_preflight",
        "health", "processes_after_lifecycle", "monitor",
        "unreal_commands", "environment_policy",
        "stripped_ddc_environment_overrides", "after", "cleanup",
        "lifecycle_status",
    }
    if set(receipt) != required_receipt_keys:
        raise FunnelFailure(
            "release Zen lifetime receipt is not exact sponsored v3")
    overrides = receipt.get("stripped_ddc_environment_overrides")
    commands = receipt.get("unreal_commands")
    expected_rows = (
        ("import", "editor"),
        ("finalize-stripped-fractal-catalog", "editor"),
        ("generate-neutral-map-shells", "editor"),
        ("cook", "cook"),
    )
    if (receipt.get("format") != ZEN_RECEIPT_FORMAT
            or receipt.get("mechanism") != "pipeline-sponsored-zen-v3"
            or receipt.get("unreal_isolation_switches")
                != ["-ddc=(ZenLocal)", ZEN_NO_AUTO_LAUNCH]
            or receipt.get("endpoint") != ZEN_SPONSOR_ENDPOINT
            or receipt.get("environment_policy")
                != "strip-all-ue-datacache-overrides-v1"
            or not isinstance(overrides, list)
            or overrides != sorted(set(overrides))
            or not isinstance(commands, list)
            or len(commands) != len(expected_rows)
            or receipt.get("lifecycle_status") != "success"):
        raise FunnelFailure(
            "release Zen lifetime receipt is not pipeline-sponsored v3")
    cleanup = receipt.get("cleanup")
    sponsor = receipt.get("sponsor")
    owner = receipt.get("owner")
    version = receipt.get("version")
    preflight = receipt.get("endpoint_preflight")
    health = receipt.get("health")
    monitor = receipt.get("monitor")
    if (not isinstance(owner, dict)
            or set(owner) != {"kind", "pid"}
            or owner.get("kind") != "pipeline-process"
            or type(owner.get("pid")) is not int
            or owner["pid"] <= 0
            or not isinstance(sponsor, dict)
            or set(sponsor) != {"pid", "command", "log"}
            or type(sponsor.get("pid")) is not int
            or sponsor["pid"] <= 0
            or sponsor["pid"] == owner["pid"]
            or not isinstance(sponsor.get("command"), list)
            or not all(isinstance(token, str) and token
                       for token in sponsor["command"])
            or not isinstance(sponsor.get("log"), str)
            or not sponsor["log"]
            or not isinstance(version, dict)
            or set(version) != {"path", "size", "sha256"}
            or not isinstance(version.get("path"), str)
            or not version["path"]
            or type(version.get("size")) is not int
            or version["size"] <= 0
            or not isinstance(cleanup, dict)
            or set(cleanup) != {
                "action", "target_pid", "signal", "forced",
                "wait_return_code", "verified_stopped",
                "data_directory_removed"}
            or cleanup.get("action") != "stop-pipeline-sponsor"
            or cleanup.get("target_pid") != sponsor["pid"]
            or not isinstance(cleanup.get("signal"), str)
            or type(cleanup.get("forced")) is not bool
            or type(cleanup.get("wait_return_code")) is not int
            or cleanup.get("verified_stopped") is not True
            or cleanup.get("data_directory_removed") is not True
            or not isinstance(preflight, dict)
            or set(preflight) != {
                "host", "port", "exclusive_bind_succeeded", "checked_utc"}
            or preflight.get("host") != "127.0.0.1"
            or preflight.get("port") != 18558
            or preflight.get("exclusive_bind_succeeded") is not True
            or not isinstance(preflight.get("checked_utc"), str)
            or not isinstance(health, dict)
            or set(health) != {
                "url", "status_code", "body_sha256", "checked_utc"}
            or health.get("url")
                != ZEN_SPONSOR_ENDPOINT + "/health/ready"
            or health.get("status_code") != 200
            or not isinstance(health.get("checked_utc"), str)
            or not isinstance(monitor, dict)
            or set(monitor) != {
                "interval_seconds", "sample_count", "started_utc",
                "finished_utc", "violation"}
            or monitor.get("interval_seconds") != 0.1
            or type(monitor.get("sample_count")) is not int
            or monitor["sample_count"] <= 0
            or not isinstance(monitor.get("started_utc"), str)
            or not isinstance(monitor.get("finished_utc"), str)
            or monitor.get("violation") is not None):
        raise FunnelFailure(
            "release Zen lifetime receipt is not pipeline-sponsored v3")
    _validate_digest(version.get("sha256"), "Zen executable digest")
    _validate_digest(health.get("body_sha256"), "Zen health-body digest")
    sponsor_command = sponsor["command"]
    expected_zen_leaf = (
        "zenserver" if platform_name == "Mac" else "zenserver.exe")
    version_leaf = version["path"].replace("\\", "/").rsplit("/", 1)[-1]
    normalized_data_dir = (
        "/" + sponsor_command[6].replace("\\", "/").lstrip("/")) \
        if len(sponsor_command) > 6 else ""
    expected_data_suffix = (
        f"/Saved/GuardedZenSponsor-{owner['pid']}")
    normalized_log = "/" + sponsor["log"].replace("\\", "/").lstrip("/")
    if (len(sponsor_command) != 12
            or version_leaf.casefold() != expected_zen_leaf
            or sponsor_command[0] != version["path"]
            or sponsor_command[1:6] != [
                "--port", "18558", "--allow-port-probing=false",
                "--http-forceloopback", "--data-dir"]
            or not normalized_data_dir.endswith(expected_data_suffix)
            or sponsor_command[7:] != [
                "--owner-pid", str(owner["pid"]), "--detach=false",
                "--no-sentry", "--quiet"]
            or not normalized_log.endswith("/logs/zen-sponsor.log")):
        raise FunnelFailure(
            "release Zen lifetime receipt is not pipeline-sponsored v3")

    def process_set(rows: object, label: str) -> set[tuple[int, str]]:
        if not isinstance(rows, list):
            raise FunnelFailure(f"release Zen v3 {label} is malformed")
        result = set()
        pids = set()
        for row in rows:
            if (not isinstance(row, dict)
                    or set(row) != {"pid", "command"}
                    or type(row.get("pid")) is not int
                    or row["pid"] <= 0
                    or not isinstance(row.get("command"), str)
                    or not row["command"]
                    or row["pid"] in pids):
                raise FunnelFailure(f"release Zen v3 {label} is malformed")
            pids.add(row["pid"])
            result.add((row["pid"], row["command"]))
        return result

    before = process_set(receipt.get("before"), "preexisting process set")
    during = process_set(
        receipt.get("processes_after_lifecycle"), "during process set")
    after = process_set(receipt.get("after"), "final process set")
    sponsor_identity = (
        sponsor["pid"], " ".join(sponsor_command).replace('"', ""))
    if (sponsor["pid"] in {pid for pid, _command in before}
            or any(re.search(
                r"(?:^|\s)--port(?:\s+|=)18558(?:\s|$)",
                command, flags=re.IGNORECASE) is not None
                for _pid, command in before)
            or during != before | {sponsor_identity}
            or after != before):
        raise FunnelFailure(
            "release Zen lifetime receipt is not pipeline-sponsored v3")
    try:
        chronology = [datetime.fromisoformat(value) for value in (
            preflight["checked_utc"], health["checked_utc"],
            monitor["started_utc"], monitor["finished_utc"])]
        if (not all(value.tzinfo is not None for value in chronology)
                or chronology != sorted(chronology)):
            raise ValueError("unordered")
    except (TypeError, ValueError) as exc:
        raise FunnelFailure(
            "release Zen lifetime receipt is not pipeline-sponsored v3") from exc
    for row, (stage, kind) in zip(commands, expected_rows):
        if (not isinstance(row, dict)
                or set(row) != {
                    "stage", "kind", "command", "log", "log_sha256",
                    "exit_code", "stripped_ddc_environment_overrides"}
                or row.get("stage") != stage
                or row.get("kind") != kind
                or type(row.get("exit_code")) is not int
                or row.get("exit_code") != 0
                or not isinstance(row.get("log"), str)
                or not row["log"]
                or row.get("stripped_ddc_environment_overrides") != overrides):
            raise FunnelFailure(
                "release Zen v3 Unreal command sequence is malformed")
        _validate_digest(row.get("log_sha256"), "Zen command-log digest")
        _validate_release_unreal_command(
            row.get("command"), kind, platform_name)
    return value


def _validate_release_cooked_paths(value: object) -> dict:
    if (not isinstance(value, list)
            or not all(isinstance(path, str) and path for path in value)):
        raise FunnelFailure("release cooked path census is malformed")
    shell_prefix = "../../../WoT/Plugins/WoTNeutralShells/Content/"
    map_identities = {
        "../../../WoT/Content/WoT/Maps/WoTBootstrap",
        "../../../WoT/Content/WoT/Maps/WoTFrontEnd",
        *{
            shell_prefix + f"WoTMapShell{slot:02d}"
            for slot in range(1, 20)
        },
    }
    support_identities = {
        "../../../WoT/Content" + package.removeprefix("/Game")
        for package in (
            *NEUTRAL_SUPPORT_ROOTS, *NEUTRAL_SUPPORT_HARD_DEPENDENCIES)
    }
    allowed = map_identities | support_identities
    project_prefixes = ("../../../wot/content/", "../../../wot/plugins/")
    package_extensions = {".uasset", ".umap", ".uexp", ".ubulk", ".uptnl"}
    normalized = [path.replace("\\", "/") for path in value]
    # The same descriptor rule as build_stripped_pak._validate_guarded_cooked_paths:
    # exactly one canonical neutral-shell descriptor; mount-rooted engine plugin
    # descriptors (Mac UAT stages them, Win64 none) are engine metadata, and an
    # enabled optional code plugin may add its own descriptor (exact path).
    plugin_descriptor = (
        "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin")
    optional_descriptors = build_stripped_pak.OPTIONAL_CODE_PLUGIN_DESCRIPTORS
    descriptor_paths = [
        path for path in normalized if path.casefold().endswith(".uplugin")]
    if len(descriptor_paths) != len(set(descriptor_paths)):
        raise FunnelFailure(
            "release cooked path census contains a duplicate plugin descriptor")
    engine_descriptors = [
        path for path in descriptor_paths
        if _release_engine_plugin_descriptor(path)]
    if [path for path in descriptor_paths
            if path not in engine_descriptors
            and path not in optional_descriptors] != [plugin_descriptor]:
        raise FunnelFailure(
            "release cooked path census must stage exactly the neutral-shell "
            "plugin descriptor")
    project_paths = [
        path for path in normalized
        if path.casefold().startswith(project_prefixes)
        and Path(path).suffix.casefold() in package_extensions
    ]
    if len(project_paths) != len(set(project_paths)):
        raise FunnelFailure(
            "release cooked path census contains duplicate project paths")
    identities = set()
    primary_maps = set()
    primary_support = set()
    denied = []
    for path in sorted(project_paths):
        suffix = Path(path).suffix.casefold()
        identity = path[:-len(suffix)]
        identities.add(identity)
        if identity not in allowed:
            denied.append(path)
        elif suffix == ".umap" and identity in map_identities:
            primary_maps.add(identity)
        elif suffix == ".uasset" and identity in support_identities:
            primary_support.add(identity)
    if denied:
        raise FunnelFailure(
            "release cooked path census contains extra or retail packages")
    if (map_identities - primary_maps
            or support_identities - primary_support):
        raise FunnelFailure(
            "release cooked path census is missing required packages")
    return {
        "project_content_count": len(project_paths),
        "retail_derived_count": 0,
        "neutral_map_count": len(primary_maps),
        "neutral_support_package_count": len(primary_support),
        "neutral_shell_plugin_descriptor_count": 1,
        "engine_plugin_descriptor_count": len(engine_descriptors),
    }


def _release_engine_plugin_descriptor(path: str) -> bool:
    """build_stripped_pak._is_engine_plugin_descriptor, mirrored."""
    parts = path.removeprefix("../../../").split("/")
    return (len(parts) >= 4 and parts[0] == "Engine" and parts[1] == "Plugins"
            and ".." not in parts and "" not in parts
            and parts[-1].casefold().endswith(".uplugin"))


def _validate_full_cooked_receipt(
        receipt: object, platform_name: str,
        configuration: str = "Test") -> dict:
    required = {
        "format", "platform", "configuration", "tool", "containers",
        "paths", "path_count", "validation", "log", "log_sha256",
    }
    if not isinstance(receipt, dict) or set(receipt) != required:
        raise FunnelFailure(
            "release cooked-container receipt is not exact full v2")
    tool = receipt.get("tool")
    tool_path = tool.get("path") if isinstance(tool, dict) else None
    tool_leaf = tool_path.replace("\\", "/").rsplit("/", 1)[-1].casefold() \
        if isinstance(tool_path, str) else ""
    log_path = receipt.get("log")
    normalized_log = log_path.replace("\\", "/") \
        if isinstance(log_path, str) else ""
    # build_stripped_pak._cooked_inventory_paths: Test keeps the historical
    # log name; the Shipping stage's inventory log is suffixed.
    log_leaf = ("cooked-container-inventory.log" if configuration == "Test"
                else f"cooked-container-inventory-{configuration.casefold()}.log")
    paths = receipt.get("paths")
    if (receipt.get("format") != COOKED_CONTAINER_RECEIPT_FORMAT
            or receipt.get("platform") != platform_name
            or receipt.get("configuration") != configuration
            or not isinstance(tool, dict)
            or set(tool) != {"path", "size", "sha256"}
            or not isinstance(tool.get("path"), str)
            or not tool["path"]
            or tool_leaf not in {"unrealpak", "unrealpak.exe"}
            or type(tool.get("size")) is not int
            or tool["size"] <= 0
            or not isinstance(receipt.get("log"), str)
            or not receipt["log"]
            or not (normalized_log == "logs/" + log_leaf
                    or normalized_log.endswith("/logs/" + log_leaf))
            or not isinstance(paths, list)
            or not all(isinstance(path, str) and path and "\\" not in path
                       for path in paths)
            or paths != sorted(set(paths))):
        raise FunnelFailure(
            "release cooked-container receipt is not exact full v2")
    _validate_digest(tool.get("sha256"), "cooked inventory tool digest")
    _validate_digest(
        receipt.get("log_sha256"), "cooked inventory log digest")
    containers = receipt.get("containers")
    if not isinstance(containers, list) or not containers:
        raise FunnelFailure(
            "release cooked-container receipt is not exact full v2")
    seen = set()
    listed_entry_count = 0
    for row in containers:
        if not isinstance(row, dict):
            raise FunnelFailure(
                "release cooked-container receipt is not exact full v2")
        is_global = set(row) == {
            "path", "size", "sha256", "entry_count", "listing"}
        is_listed = set(row) == {
            "path", "size", "sha256", "entry_count"}
        relative = row.get("path")
        normalized = relative.replace("\\", "/") \
            if isinstance(relative, str) else ""
        if (not (is_global or is_listed)
                or not normalized
                or "\\" in relative
                or normalized.startswith("/")
                or ":" in normalized.split("/", 1)[0]
                or ".." in normalized.split("/")
                or normalized in seen
                or Path(normalized).suffix.casefold() not in {".pak", ".utoc"}
                or type(row.get("size")) is not int
                or row["size"] <= 0):
            raise FunnelFailure(
                "release cooked-container receipt is not exact full v2")
        seen.add(normalized)
        _validate_digest(row.get("sha256"), "cooked container digest")
        if is_global:
            if (row.get("entry_count") is not None
                    or row.get("listing")
                    != "global-container-has-no-directory-index"):
                raise FunnelFailure(
                    "release cooked-container receipt is not exact full v2")
        elif (type(row.get("entry_count")) is not int
              or row["entry_count"] <= 0):
            raise FunnelFailure(
                "release cooked-container receipt is not exact full v2")
        else:
            listed_entry_count += row["entry_count"]
    if (type(receipt.get("path_count")) is not int
            or receipt["path_count"] <= 0
            or listed_entry_count < receipt["path_count"]):
        raise FunnelFailure(
            "release cooked-container receipt is not exact full v2")
    return receipt


def _resolve_release(
    release_json: Path, *, require_current_source: bool = True,
    configuration: str = "Test",
) -> tuple[dict, Path]:
    """Validate a guarded release.json of ``configuration`` and its asset.

    The funnel drives Test seams and accepts only Test; the Shipping smoke
    (smoke_shipping_release.py) reuses this with configuration="Shipping".
    """
    release_json = release_json.expanduser().resolve()
    manifest = _read_json(release_json, "guarded release manifest")
    required = {
        "format", "platform", "configuration", "asset", "size", "sha256",
        "git_commit", "release_sequence", "source_snapshot", "products", "audit",
        "target_receipts", "engine_identity", "generated_utc", "zen_lifetime",
        "cooked_container_inventory", "neutral_support_authority",
    }
    missing = sorted(required - set(manifest))
    if missing:
        raise FunnelFailure(
            "guarded release manifest is missing required receipt field(s): "
            + ", ".join(missing)
        )
    if (
        manifest.get("format") != GUARDED_RELEASE_FORMAT
        or manifest.get("configuration") != configuration
        or manifest.get("platform") not in {"Mac", "Win64"}
    ):
        raise FunnelFailure(
            f"acceptance requires a guarded Mac/Win64 {configuration} release "
            "manifest"
        )
    release_sequence = manifest.get("release_sequence")
    if (type(release_sequence) is not int
            or not 0 <= release_sequence <= formats.INT64_MAX):
        raise FunnelFailure("guarded release sequence is not a non-negative int64")
    asset = release_json.parent / _safe_leaf(
        manifest.get("asset"), "release asset"
    )
    if not asset.is_file():
        raise FunnelFailure(f"guarded release asset is missing: {asset}")
    if (
        not isinstance(manifest.get("size"), int)
        or manifest["size"] != asset.stat().st_size
        or _validate_digest(manifest.get("sha256"), "release asset digest")
        != _sha256_file(asset)
    ):
        raise FunnelFailure("guarded release asset differs from release.json")
    _validate_release_archive_alternate_bind_absence(asset)

    source = manifest.get("source_snapshot")
    if (
        not isinstance(source, dict)
        or source.get("format") != SOURCE_SNAPSHOT_FORMAT
        or source.get("git_commit") != manifest.get("git_commit")
    ):
        raise FunnelFailure("release source-snapshot receipt is malformed")
    _validate_digest(source.get("sha256"), "source-snapshot digest")
    # Checked before the receipts below, whose rules may have changed since
    # the release was built: a stale release needs a rebuild, not a diagnosis.
    if require_current_source:
        current = _current_source_snapshot()
        if source != current:
            raise FunnelFailure(
                "release Source/Tools snapshot differs from the current tree; "
                "rebuild with Tools/dist/build_stripped_pak.py --guarded "
                f"(release {source.get('sha256')}, current {current['sha256']})"
            )
    neutral_support = _validate_neutral_support_proof(
        manifest.get("neutral_support_authority"))
    products = manifest.get("products")
    if not isinstance(products, dict) or set(products) != {
        "receipt", "receipt_sha256"
    }:
        raise FunnelFailure("release product receipt binding is missing")
    product_receipt = products.get("receipt")
    if (
        not isinstance(product_receipt, dict)
        or product_receipt.get("format") != PRODUCT_RECEIPT_FORMAT
        or product_receipt.get("platform") != manifest["platform"]
        or product_receipt.get("configuration") != configuration
        or set(product_receipt.get("products", {})) != {"WoT"}
    ):
        raise FunnelFailure("guarded product receipt is malformed")
    expected_product_sha = hashlib.sha256(
        _pretty_json_bytes(product_receipt)
    ).hexdigest()
    if products.get("receipt_sha256") != expected_product_sha:
        raise FunnelFailure("guarded product receipt digest does not verify")
    try:
        signing_facts = formats.validate_mac_signing_receipt(
            product_receipt, "guarded product receipt")
    except formats.FormatError as exc:
        raise FunnelFailure(str(exc)) from exc
    if manifest["platform"] != "Mac" and signing_facts is not None:
        raise FunnelFailure("non-Mac product receipt has Mac signing facts")
    for name, product in product_receipt["products"].items():
        if not isinstance(product, dict):
            raise FunnelFailure(f"{name} product receipt is malformed")
        for key in (
            "path", "size", "sha256", "compiled_receipt_sha256",
            "engine_receipt_sha256",
        ):
            if key not in product:
                raise FunnelFailure(
                    f"{name} product receipt is missing {key}"
                )
        _validate_digest(product["sha256"], f"{name} product digest")

    target_receipts = manifest.get("target_receipts")
    if not isinstance(target_receipts, dict) or set(target_receipts) != {
        "WoT"
    }:
        raise FunnelFailure("release target build receipts are missing")
    for name, pair in target_receipts.items():
        if not isinstance(pair, dict):
            raise FunnelFailure(f"{name} target receipt is malformed")
        if not isinstance(pair.get("compiled_receipt"), dict):
            raise FunnelFailure(f"{name} compiled build receipt is missing")
        if not isinstance(pair.get("engine_receipt"), dict):
            raise FunnelFailure(f"{name} engine build receipt is missing")
        for key in ("compiled_receipt_sha256", "engine_receipt_sha256"):
            _validate_digest(pair.get(key), f"{name} {key}")
        if pair["compiled_receipt_sha256"] != hashlib.sha256(
            _pretty_json_bytes(pair["compiled_receipt"])
        ).hexdigest():
            raise FunnelFailure(f"{name} compiled build receipt digest is invalid")
        if pair["engine_receipt_sha256"] != hashlib.sha256(
            _pretty_json_bytes(pair["engine_receipt"])
        ).hexdigest():
            raise FunnelFailure(f"{name} engine build receipt digest is invalid")

    audit = manifest.get("audit")
    if (
        not isinstance(audit, dict)
        or audit.get("verdict") != "pass"
        or "staged_tree_receipt_sha256" not in audit
        or "staged_tree_sha256" not in audit
    ):
        raise FunnelFailure("release staged-tree audit receipt is not passing")
    cooked_inventory = manifest.get("cooked_container_inventory")
    if not isinstance(cooked_inventory, dict) or set(cooked_inventory) != {
        "receipt_sha256", "receipt", "path_count", "validation"
    }:
        raise FunnelFailure(
            "release cooked_container_inventory path census is missing or malformed")
    receipt_sha256 = _validate_digest(
        cooked_inventory.get("receipt_sha256"),
        "cooked-container inventory receipt digest",
    )
    cooked_receipt = cooked_inventory.get("receipt")
    if (not isinstance(cooked_receipt, dict)
            or receipt_sha256 != hashlib.sha256(
                _pretty_json_bytes(cooked_receipt)).hexdigest()
            or not isinstance(cooked_receipt.get("paths"), list)
            or cooked_receipt.get("path_count")
                != len(cooked_receipt.get("paths", []))
            or cooked_inventory.get("path_count")
                != cooked_receipt.get("path_count")
            or cooked_inventory.get("validation")
                != cooked_receipt.get("validation")):
        raise FunnelFailure(
            "release cooked_container_inventory path census is invalid")
    _validate_full_cooked_receipt(
        cooked_receipt, manifest["platform"], configuration)
    validation = cooked_inventory.get("validation")
    derived_validation = _validate_release_cooked_paths(
        cooked_receipt.get("paths"))
    if (cooked_receipt.get("validation") != derived_validation
            or validation != derived_validation):
        raise FunnelFailure(
            "release cooked path census validation does not verify")
    if (
        not isinstance(cooked_inventory.get("path_count"), int)
        or cooked_inventory["path_count"] < 30
        or not isinstance(validation, dict)
        or set(validation) != {
            "project_content_count", "retail_derived_count", "neutral_map_count",
            "neutral_support_package_count",
            "neutral_shell_plugin_descriptor_count",
            "engine_plugin_descriptor_count",
        }
        or validation.get("neutral_shell_plugin_descriptor_count") != 1
        or not isinstance(validation.get("engine_plugin_descriptor_count"), int)
        or validation["engine_plugin_descriptor_count"] < 0
        or not isinstance(validation.get("project_content_count"), int)
        or validation["project_content_count"] < 30
        or validation.get("retail_derived_count") != 0
        or validation.get("neutral_map_count") != 21
        or validation.get("neutral_support_package_count") != len(
            (*NEUTRAL_SUPPORT_ROOTS, *NEUTRAL_SUPPORT_HARD_DEPENDENCIES))
    ):
        raise FunnelFailure(
            "release cooked_container_inventory did not prove the exact "
            "neutral map and support closure")
    _validate_zen_lifetime_proof(
        manifest.get("zen_lifetime"), manifest["platform"])

    authority_path = REPO / neutral_support["authority_relative_path"]
    if (not authority_path.is_file()
            or _sha256_file(authority_path)
                != neutral_support["authority_sha256"]):
        raise FunnelFailure(
            "release neutral-support authority differs from current source")
    return manifest, asset


def _cell_names(platform_name: str) -> tuple[str, ...]:
    if platform_name == "Win64":
        return tuple(CELL_NAMES) + tuple(formats.UPDATE_FUNNEL_CELLS)
    if platform_name == "Mac":
        return (tuple(CELL_NAMES)
                + tuple(formats.UPDATE_FUNNEL_CELLS_BY_PLATFORM["Mac"]))
    raise FunnelFailure(f"unsupported funnel platform {platform_name!r}")


def _parse_cells(values: Iterable[str], platform_name: str = "Mac") -> set[str]:
    names = _cell_names(platform_name)
    aliases = {name: name for name in names}
    aliases.update({str(index): name for index, name in enumerate(names)})
    selected: set[str] = set()
    unknown: list[str] = []
    for raw in values:
        for item in raw.split(","):
            key = item.strip().casefold().replace("_", "-")
            if not key:
                continue
            if key not in aliases:
                unknown.append(item)
            else:
                selected.add(aliases[key])
    if unknown:
        raise FunnelFailure(
            "unknown cell(s): "
            + ", ".join(unknown)
            + "; choices: "
            + ", ".join(f"{i}={name}" for i, name in enumerate(names))
        )
    return selected or set(names)


def _empty_cell(index: int, name: str) -> dict:
    return {
        "format": CELL_RECEIPT_FORMAT,
        "index": index,
        "name": name,
        "verdict": "skip",
        "started_utc": None,
        "completed_utc": None,
        "duration_seconds": 0.0,
        "phase_timings_seconds": {},
        "phase_order": [],
        "evidence": {},
        "decisions": [],
        "skip_reason": "not selected",
        "error": None,
    }


def _new_receipt(platform_name: str) -> dict:
    names = _cell_names(platform_name)
    return {
        "format": RECEIPT_FORMAT,
        "platform": platform_name,
        "verdict": "fail",
        "started_utc": _utc_now(),
        "completed_utc": None,
        "duration_seconds": 0.0,
        "release": {},
        "installer": {},
        "cells": [
            _empty_cell(index, name)
            for index, name in enumerate(names)
        ],
        "decisions": [],
    }


def _mark_unreached_selected_cells(
    receipt: dict, selected: set[str]
) -> None:
    index_by_name = {
        cell["name"]: index for index, cell in enumerate(receipt["cells"])
    }
    for name in selected:
        cell = receipt["cells"][index_by_name[name]]
        if cell["started_utc"] is None and cell["skip_reason"] == "not selected":
            cell["skip_reason"] = "not reached after prior failure"


def _validate_receipt_schema(receipt: dict) -> None:
    if set(receipt) != RECEIPT_KEYS:
        raise FunnelFailure("funnel receipt top-level schema is not exact")
    if receipt.get("format") != RECEIPT_FORMAT:
        raise FunnelFailure("funnel receipt format is unsupported")
    if receipt.get("platform") not in {"Mac", "Win64"}:
        raise FunnelFailure("funnel receipt platform is unsupported")
    if receipt.get("verdict") not in {"pass", "fail"}:
        raise FunnelFailure("funnel receipt verdict is invalid")
    if not isinstance(receipt.get("started_utc"), str):
        raise FunnelFailure("funnel receipt start timestamp is invalid")
    if receipt.get("completed_utc") is not None and not isinstance(
        receipt["completed_utc"], str
    ):
        raise FunnelFailure("funnel receipt completion timestamp is invalid")
    if (
        not isinstance(receipt.get("duration_seconds"), (int, float))
        or receipt["duration_seconds"] < 0
    ):
        raise FunnelFailure("funnel receipt duration is invalid")
    release = receipt.get("release")
    if not isinstance(release, dict) or (
        release and set(release) != RELEASE_RECEIPT_KEYS
    ):
        raise FunnelFailure("funnel release binding schema is not exact")
    installer = receipt.get("installer")
    if not isinstance(installer, dict) or (
        installer and set(installer) != INSTALLER_RECEIPT_KEYS
    ):
        raise FunnelFailure("funnel installer binding schema is not exact")
    if not isinstance(receipt.get("decisions"), list) or not all(
        isinstance(item, str) for item in receipt["decisions"]
    ):
        raise FunnelFailure("funnel receipt decisions are invalid")
    cells = receipt.get("cells")
    names = _cell_names(receipt.get("platform"))
    if not isinstance(cells, list) or len(cells) != len(names):
        raise FunnelFailure("funnel receipt does not contain the closed matrix")
    for index, (cell, name) in enumerate(zip(cells, names)):
        if not isinstance(cell, dict) or set(cell) != CELL_KEYS:
            raise FunnelFailure(f"cell {index} schema is not exact")
        if (
            cell.get("format") != CELL_RECEIPT_FORMAT
            or cell.get("index") != index
            or cell.get("name") != name
            or cell.get("verdict") not in CELL_VERDICTS
        ):
            raise FunnelFailure(f"cell {index} identity/verdict is invalid")
        if not isinstance(cell.get("phase_timings_seconds"), dict):
            raise FunnelFailure(f"cell {index} phase timings are invalid")
        if not isinstance(cell.get("phase_order"), list):
            raise FunnelFailure(f"cell {index} phase order is invalid")
        if not isinstance(cell.get("evidence"), dict):
            raise FunnelFailure(f"cell {index} evidence is invalid")
        if not isinstance(cell.get("decisions"), list):
            raise FunnelFailure(f"cell {index} decisions are invalid")
        if not all(isinstance(item, str) for item in cell["decisions"]):
            raise FunnelFailure(f"cell {index} decision is not text")
        if (
            not isinstance(cell.get("duration_seconds"), (int, float))
            or cell["duration_seconds"] < 0
        ):
            raise FunnelFailure(f"cell {index} duration is invalid")
        if not all(
            isinstance(phase, str) for phase in cell["phase_order"]
        ):
            raise FunnelFailure(f"cell {index} phase trace is invalid")
        if not all(
            isinstance(phase, str)
            and isinstance(seconds, (int, float))
            and seconds >= 0
            for phase, seconds in cell["phase_timings_seconds"].items()
        ):
            raise FunnelFailure(f"cell {index} phase timing is invalid")
    try:
        formats.validate_funnel_receipt_schema(receipt, receipt.get("platform"))
    except formats.FormatError as exc:
        raise FunnelFailure(f"funnel receipt schema is invalid: {exc}") from exc


def _run_cell(receipt: dict, name: str, operation: Callable[[dict], None]) -> None:
    cell = next((item for item in receipt["cells"] if item["name"] == name), None)
    if cell is None:
        raise FunnelFailure(f"cell {name!r} is outside this platform matrix")
    started = time.monotonic()
    cell.update({
        "verdict": "fail",
        "started_utc": _utc_now(),
        "completed_utc": None,
        "duration_seconds": 0.0,
        "phase_timings_seconds": {},
        "phase_order": [],
        "evidence": {},
        "decisions": [],
        "skip_reason": None,
        "error": None,
    })
    try:
        operation(cell)
        if cell["verdict"] == "fail":
            cell["verdict"] = "pass"
    except BaseException as exc:
        cell["verdict"] = "fail"
        cell["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        cell["completed_utc"] = _utc_now()
        cell["duration_seconds"] = round(time.monotonic() - started, 6)


def _run_update_cell(
    receipt: dict, name: str, operation: Callable[[dict], None],
    evidence_root: Path,
) -> None:
    """Run a Mac-capable cell with the mandatory real-preferences tripwire."""
    if receipt.get("platform") != "Mac":
        _run_cell(receipt, name, operation)
        return

    safety_path = evidence_root / "safety" / f"{name}-real-preferences.json"

    def guarded(cell: dict) -> None:
        before = _mac_real_support_inventory()
        record = {
            "after": None,
            "before": before,
            "cell": name,
            "format": "wot-real-mac-preferences-guard-v1",
            "unchanged": None,
        }
        _atomic_write_json(safety_path, record)
        try:
            operation(cell)
        finally:
            after = _mac_real_support_inventory()
            record["after"] = after
            record["unchanged"] = after == before
            _atomic_write_json(safety_path, record)
            if after != before:
                raise FunnelFailure(
                    "SAFETY: real Mac preferences changed during update cell "
                    f"{name}; aborting the run (inventory: {safety_path})")

    _run_cell(receipt, name, guarded)


def _verify_installer(path: Path) -> dict:
    path = path.expanduser().resolve()
    try:
        bootstrap.verify_installer(path)
    except Exception as exc:
        raise FunnelFailure(f"installer failed compiled pin verification: {exc}") from exc
    return {
        "path": str(path),
        "size": path.stat().st_size,
        "sha256": _sha256_file(path),
    }


def _instance_tree_kill(process: subprocess.Popen) -> str:
    """Hard-kill the exact process represented by a retained native handle.

    POSIX launches an isolated process group. On Windows ``Popen.kill`` calls
    TerminateProcess on the retained process handle, avoiding a PID-reuse race;
    the funnel's process observer separately owns and cleans descendants.
    """
    if os.name == "nt":
        process.kill()
        return "TerminateProcess(handle)"
    os.killpg(process.pid, signal.SIGKILL)
    return "SIGKILL"


class _RangeServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(
        self, installer: Path, request_log: Path,
        bytes_per_second: int = 0,
        release_assets: dict[str, Path] | None = None,
    ):
        super().__init__(("127.0.0.1", 0), _RangeHandler)
        self.installer = installer
        self.assets = {"/installer": Path(installer)}
        for route, path in (release_assets or {}).items():
            parsed = urllib.parse.urlsplit(route)
            if (not route.startswith("/") or parsed.path != route
                    or (route not in {"/announcement.json",
                                      "/announcement.json.sig",
                                      RELEASE_NOTES_ROUTE}
                        and re.fullmatch(
                            re.escape(formats.FUNNEL_RELEASE_BASE_PATH)
                            + r"/releases/download/[A-Za-z0-9][A-Za-z0-9._-]*/"
                            r"[A-Za-z0-9][A-Za-z0-9._-]*", route) is None)):
                raise FunnelFailure(
                    f"loopback release asset route is not allowed: {route!r}")
            if route in self.assets:
                raise FunnelFailure(
                    f"loopback release asset route appears twice: {route!r}")
            if not Path(path).is_file():
                raise FunnelFailure(
                    f"loopback release asset is missing: {path}")
            self.assets[route] = Path(path)
        self.request_log = request_log
        self.bytes_per_second = bytes_per_second
        self.log_lock = threading.Lock()
        self.active_lock = threading.Condition()
        self.active_requests = 0
        self.etags = {
            route: f'"wot-{_sha256_file(path)}"'
            for route, path in self.assets.items()
        }
        # Kept for the installer-resume specs and If-Range callers.
        self.etag = self.etags["/installer"]
        self.last_modified = "Wed, 29 Jul 2026 00:00:00 GMT"

    def process_request(self, request, client_address) -> None:
        with self.active_lock:
            self.active_requests += 1
        try:
            super().process_request(request, client_address)
        except BaseException:
            with self.active_lock:
                self.active_requests -= 1
                self.active_lock.notify_all()
            raise

    def process_request_thread(self, request, client_address) -> None:
        try:
            super().process_request_thread(request, client_address)
        finally:
            with self.active_lock:
                self.active_requests -= 1
                self.active_lock.notify_all()

    def wait_for_idle(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        with self.active_lock:
            while self.active_requests:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self.active_lock.wait(remaining)
            return True

    def record(self, value: dict) -> None:
        line = json.dumps(value, separators=(",", ":"), sort_keys=True) + "\n"
        self.request_log.parent.mkdir(parents=True, exist_ok=True)
        with self.log_lock:
            with self.request_log.open("a", encoding="utf-8") as handle:
                handle.write(line)
                handle.flush()
                os.fsync(handle.fileno())


class _RangeHandler(BaseHTTPRequestHandler):
    server: _RangeServer
    protocol_version = "HTTP/1.1"

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def _record(
        self, status: int, *, range_start: int | None = None,
        range_end: int | None = None, response_bytes: int = 0,
    ) -> None:
        self.server.record({
            "format": REQUEST_LOG_FORMAT,
            "timestamp_utc": _utc_now(),
            "method": self.command,
            "path": self.path,
            "range": self.headers.get("Range", ""),
            "if_range": self.headers.get("If-Range", ""),
            "status": status,
            "range_start": range_start,
            "range_end": range_end,
            "response_bytes": response_bytes,
        })

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
        parsed = urllib.parse.urlsplit(self.path)
        target = (self.server.assets.get(parsed.path)
                  if self.path == parsed.path else None)
        if target is None:
            self._record(404)
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        size = target.stat().st_size
        etag = self.server.etags[parsed.path]
        start, end, status = 0, size - 1, 200
        raw_range = self.headers.get("Range", "")
        if raw_range:
            match = re.fullmatch(r"bytes=([0-9]+)-", raw_range)
            if match is None:
                self._record(400)
                self.send_response(400)
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return
            start = int(match.group(1))
            if start >= size:
                self._record(416, range_start=start)
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return
            if_range = self.headers.get("If-Range", "")
            if if_range and if_range not in {
                etag, self.server.last_modified
            }:
                start = 0
            else:
                status = 206
        length = end - start + 1
        self.send_response(status)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("ETag", etag)
        self.send_header("Last-Modified", self.server.last_modified)
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Connection", "close")
        self.end_headers()
        sent = 0
        began = time.monotonic()
        try:
            with target.open("rb") as handle:
                handle.seek(start)
                remaining = length
                while remaining:
                    block = handle.read(min(1 << 20, remaining))
                    if not block:
                        break
                    self.wfile.write(block)
                    sent += len(block)
                    remaining -= len(block)
                    if self.server.bytes_per_second > 0:
                        target = sent / self.server.bytes_per_second
                        delay = target - (time.monotonic() - began)
                        if delay > 0:
                            time.sleep(delay)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self._record(
                status, range_start=start, range_end=end,
                response_bytes=sent,
            )


class LoopbackInstallerServer:
    def __init__(
        self, installer: Path, request_log: Path,
        bytes_per_second: int = 0,
        release_assets: dict[str, Path] | None = None,
        release_note_sequences: Sequence[int] = (),
    ):
        self.request_log = request_log
        self.request_log.parent.mkdir(parents=True, exist_ok=True)
        self.request_log.write_text("", encoding="utf-8")
        notes_path = self.request_log.parent / formats.RELEASE_NOTES_ASSET
        notes_path.write_bytes(formats.release_notes_bytes({
            "format": formats.RELEASE_NOTES_FORMAT,
            "notes": [{
                "release_sequence": sequence,
                "text": f"Funnel release {sequence}",
            } for sequence in sorted(set(release_note_sequences))
                      if sequence >= 1],
        }))
        assets = dict(release_assets or {})
        if RELEASE_NOTES_ROUTE in assets:
            raise FunnelFailure("release notes route is owned by the funnel fixture")
        assets[RELEASE_NOTES_ROUTE] = notes_path
        self.server = _RangeServer(
            installer, request_log, bytes_per_second=bytes_per_second,
            release_assets=assets,
        )
        self.thread = threading.Thread(
            target=self.server.serve_forever,
            name="wot-funnel-loopback", daemon=True,
        )

    @property
    def installer_url(self) -> str:
        return f"http://127.0.0.1:{self.server.server_port}/installer"

    @property
    def announcement_url(self) -> str:
        return f"http://127.0.0.1:{self.server.server_port}/announcement.json"

    @property
    def release_base_url(self) -> str:
        return (f"http://127.0.0.1:{self.server.server_port}"
                f"{formats.FUNNEL_RELEASE_BASE_PATH}")

    def __enter__(self) -> "LoopbackInstallerServer":
        self.thread.start()
        return self

    def __exit__(self, *_args: object) -> None:
        self.server.shutdown()
        settled = self.server.wait_for_idle(5.0)
        self.server.server_close()
        self.thread.join(timeout=5)
        if not settled:
            raise FunnelFailure("loopback request handlers did not settle")

    def requests(self, *, require_settled: bool = True) -> list[dict]:
        if require_settled and self.server.active_requests:
            raise FunnelFailure(
                "loopback network evidence has an in-flight request"
            )
        if not self.request_log.is_file():
            return []
        result = []
        for number, line in enumerate(
            self.request_log.read_text(encoding="utf-8").splitlines(), 1
        ):
            try:
                value = json.loads(line)
            except json.JSONDecodeError as exc:
                raise FunnelFailure(
                    f"malformed loopback request log line {number}"
                ) from exc
            if not isinstance(value, dict) or value.get(
                "format"
            ) != REQUEST_LOG_FORMAT:
                raise FunnelFailure("malformed loopback request receipt")
            result.append(value)
        return result


def _fixture_file(directory: Path, reference: dict, what: str) -> Path:
    keys = {"name", "sha256", "size"}
    if not isinstance(reference, dict) or not keys.issubset(reference):
        raise FunnelFailure(f"{what} fixture reference is malformed")
    path = directory / reference["name"]
    if not path.is_file():
        raise FunnelFailure(f"{what} fixture file is missing: {path}")
    digest, size = formats.sha256_file(path)
    if (digest, size) != (reference["sha256"], reference["size"]):
        raise FunnelFailure(f"{what} fixture file differs from its derivation receipt")
    return path


def _fixture_release_assets(receipt_path: Path, label: str,
                            platform_name: str = "Win64") -> tuple[dict, dict]:
    receipt = _read_json(receipt_path, "update fixture derivation receipt")
    if (receipt.get("format") != update_fixtures.DERIVATION_FORMAT
            or receipt.get("platform") != platform_name):
        raise FunnelFailure("update fixture derivation receipt is unsupported")
    variant = receipt.get("variants", {}).get(label)
    if not isinstance(variant, dict):
        raise FunnelFailure(f"update fixture has no {label!r} variant")
    directory = receipt_path.parent / variant["directory"]
    tag = variant.get("tag")
    if not isinstance(tag, str) or not tag:
        raise FunnelFailure(f"update fixture {label} tag is invalid")
    assets = {}
    for key in ("announcement", "key_ring", "manifest"):
        reference = variant[key]
        path = _fixture_file(directory, reference, f"{label} {key}")
        signature = reference.get("signature")
        signature_path = _fixture_file(
            directory, signature, f"{label} {key} signature")
        if key == "announcement":
            assets["/announcement.json"] = path
            assets["/announcement.json.sig"] = signature_path
        else:
            prefix = (f"{formats.FUNNEL_RELEASE_BASE_PATH}"
                      f"/releases/download/{tag}")
            assets[f"{prefix}/{path.name}"] = path
            assets[f"{prefix}/{signature_path.name}"] = signature_path
    archive = _fixture_file(directory, variant["archive"], f"{label} archive")
    assets[f"{formats.FUNNEL_RELEASE_BASE_PATH}/releases/download/{tag}/"
           f"{archive.name}"] = archive
    return receipt, assets


def _receipt_file_ref(path: Path, *, relative_to: Path | None = None) -> dict:
    digest, size = formats.sha256_file(path)
    recorded = (path.resolve().relative_to(relative_to.resolve()).as_posix()
                if relative_to is not None else str(path))
    return {"path": recorded, "sha256": digest, "size": size}


def _base_update_evidence(receipt: dict) -> dict:
    base = receipt["base"]
    return {
        "archive_sha256": base["archive"]["sha256"],
        "git_commit": base["commit"],
        "release_json_sha256": base["release_json_sha256"],
        "release_sequence": base["release_sequence"],
        "tree_root": base["tree_root"],
    }


_ACTIVATION_PHASES = (
    "Idle", "Journaled", "OldRetired", "Swapped", "Committed",
    "HealthPending", "HealthStalled", "RollbackJournaled", "BadRetired",
    "RolledBack", "RollbackFinalized", "RollbackExhausted", "Healthy",
)


STAGING_ATTEMPTS = 3

RELEASE_NOTES_ROUTE = (formats.FUNNEL_RELEASE_BASE_PATH
                       + formats.RELEASE_NOTES_LATEST_PATH)


def _windows_directory_identity(path: Path) -> tuple[int, int, int]:
    handle = _create_file(
        str(path),
        0x80,  # FILE_READ_ATTRIBUTES
        0x1 | 0x2 | 0x4,  # FILE_SHARE_READ | WRITE | DELETE
        None, 3,  # OPEN_EXISTING
        0x02000000 | 0x00200000,  # BACKUP_SEMANTICS | OPEN_REPARSE_POINT
        None)
    if handle == ctypes.c_void_p(-1).value:
        error = ctypes.get_last_error()
        if error in {2, 3}:
            raise FileNotFoundError(path)
        raise ctypes.WinError(error)
    try:
        info = _FileIdInfo()
        if not _get_file_information_by_handle_ex(
                handle, 18, ctypes.byref(info), ctypes.sizeof(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        raw = bytes(info.FileId.Identifier)
        return (int(info.VolumeSerialNumber),
                int.from_bytes(raw[:8], "little"),
                int.from_bytes(raw[8:], "little"))
    finally:
        _close_handle(handle)


def _posix_directory_identity(path: Path) -> tuple[int, int, int]:
    information = os.stat(path, follow_symlinks=False)
    return int(information.st_dev), int(information.st_ino), 0


# The game writes its Saved directories inside the install tree at runtime
# (config, logs, crash-reporter settings); they are not
# part of any signed tree, so tree identity leaves them out.
INSTALL_RUNTIME_ROOTS = ("Engine/Saved", "WoT/Saved")
MAC_INSTALL_RUNTIME_ROOTS = (
    "Contents/UE/Engine/Saved", "Contents/UE/WoT/Saved")


def _is_install_runtime_path(relative: str, platform_name: str) -> bool:
    roots = (MAC_INSTALL_RUNTIME_ROOTS if platform_name == "Mac"
             else INSTALL_RUNTIME_ROOTS)
    return any(relative == runtime or relative.startswith(runtime + "/")
               for runtime in roots)


def _installed_tree_root(root: Path, platform_name: str = "Win64") -> str:
    if not root.is_dir():
        raise FunnelFailure(f"installed tree is missing: {root}")
    if platform_name == "Mac" and root.name != formats.GUARDED_ZIP_ROOTS["Mac"]:
        raise FunnelFailure(f"Mac installed tree is not WoT.app: {root}")
    entries = []
    for path in root.rglob("*"):
        relative = path.relative_to(root).as_posix()
        if (relative in formats.INVENTORY_SIDECARS
                or _is_install_runtime_path(relative, platform_name)):
            continue
        if path.is_symlink():
            if platform_name != "Mac":
                raise FunnelFailure(
                    f"installed tree contains a symbolic link: {relative}")
            entries.append(formats.TreeEntry(
                relative, "symlink", stat.S_IMODE(path.lstat().st_mode),
                0, "", os.readlink(str(path))))
        elif path.is_dir():
            entries.append(formats.TreeEntry(
                relative, "directory",
                stat.S_IMODE(path.stat().st_mode)
                if platform_name == "Mac" else 0o755, 0, ""))
        elif path.is_file():
            digest, size = formats.sha256_file(path)
            entries.append(formats.TreeEntry(
                relative, "file",
                stat.S_IMODE(path.stat().st_mode)
                if platform_name == "Mac" else 0o644, size, digest))
        else:
            raise FunnelFailure(f"installed tree contains an unsafe entry: {relative}")
    return formats.compute_installed_tree_root(
        platform_name, formats.sort_installed_tree_entries(entries))


def _heartbeat_sequence(record: dict) -> int | None:
    if record.get("event") != "release-identity":
        return None
    match = re.search(r"(?:^|; )sequence=([0-9]+)(?: |$)",
                      str(record.get("detail", "")))
    return int(match.group(1)) if match else None


def _release_notes_observation(records: list[dict], pid: int) -> dict:
    matches = [record for record in records
               if record.get("event") == "release-notes-result"
               and record.get("pid") == pid]
    if len(matches) != 1:
        raise FunnelFailure(
            f"process {pid} did not publish exactly one release-notes result")
    detail = str(matches[0].get("detail", ""))
    parsed = re.fullmatch(
        r"outcome=(selected|no-entry|unavailable|invalid|not-attempted) "
        r"sequence=([0-9]+) commit=(.*)", detail)
    if parsed is None:
        raise FunnelFailure("release-notes-result detail is malformed")
    return {
        "commit": parsed.group(3),
        "outcome": parsed.group(1),
        "pid": pid,
        "sequence": int(parsed.group(2)),
    }


def _wait_release_notes_result(path: Path, start_offset: frozenset[Path], pid: int,
                               timeout: float) -> tuple[dict, list[dict]]:
    records = _wait_heartbeat(
        path, start_offset,
        lambda values: sum(
            record.get("event") == "release-notes-result"
            and record.get("pid") == pid for record in values) >= 1,
        timeout, f"process {pid} release-notes result")
    return _release_notes_observation(records, pid), records


def _wait_client_notes_if_ready(client: "ClientProcess",
                                timeout: float) -> Optional[dict]:
    if not hasattr(client, "heartbeat_path"):
        return None
    deadline = time.monotonic() + max(0.0, timeout)
    remaining = lambda: max(0.001, deadline - time.monotonic())
    records = _wait_heartbeat(
        client.heartbeat_path, client.heartbeat_offset,
        lambda _values: True, remaining(),
        f"process {client.process.pid} heartbeat snapshot")
    if not any(record.get("event") == "phase-ready"
               and record.get("pid") == client.process.pid
               for record in records):
        return None
    observation, _records = _wait_release_notes_result(
        client.heartbeat_path, client.heartbeat_offset,
        client.process.pid, remaining())
    return observation


def _notes_request_count(requests: list[dict]) -> int:
    return sum(urllib.parse.urlsplit(str(request.get("path", ""))).path
               == RELEASE_NOTES_ROUTE
               and request.get("path") == RELEASE_NOTES_ROUTE
               for request in requests)


def _update_result_observation(record: dict) -> dict:
    if record.get("event") != "update-check-result":
        raise FunnelFailure("heartbeat is not update-check-result")
    segment = str(record.get("detail", "")).split("; ")[-1]
    tokens = segment.split()
    if not tokens:
        raise FunnelFailure("update-check-result detail is empty")
    values = {}
    for token in tokens[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    sequence_text = values.get("sequence")
    try:
        sequence = int(sequence_text) if sequence_text is not None else None
    except ValueError as exc:
        raise FunnelFailure("update-check-result has no valid sequence") from exc
    return {
        "event": "update-check-result",
        "pid": record.get("pid"),
        "reason": values.get("reason", ""),
        "result": tokens[0],
        "sequence": sequence,
        "status": values.get("status", ""),
    }


def _matches_update_result(record: dict, *, pid: int, status: str,
                           reason: str, sequence: int) -> bool:
    if record.get("pid") != pid or record.get("event") != "update-check-result":
        return False
    try:
        observed = _update_result_observation(record)
    except FunnelFailure:
        return False
    return (observed["status"] == status and observed["reason"] == reason
            and observed["sequence"] == sequence)


def _process_evidence(records: list[dict], role: str, pid: int,
                      *, ready: bool | None = None) -> dict:
    sequences = {_heartbeat_sequence(record) for record in records
                 if record.get("pid") == pid}
    sequences.discard(None)
    if len(sequences) != 1:
        raise FunnelFailure(f"process {role} did not publish one release identity")
    observed_ready = any(record.get("pid") == pid
                         and record.get("event") == "phase-ready"
                         for record in records)
    if ready is not None and observed_ready != ready:
        raise FunnelFailure(
            f"process {role} Ready={observed_ready}, expected {ready}")
    return {
        "pid": pid,
        "ready": observed_ready,
        "release_identity_sequence": sequences.pop(),
        "role": role,
    }


# Several 2 s installer probe intervals: a half-written file must survive them
# without being accepted or failing the operation.
PARTIAL_HOLD_SECONDS = 8.0

def _wait_heartbeat(path: Path, start_offset: frozenset[Path], predicate: Callable[[list[dict]], bool],
                    timeout: float, what: str) -> list[dict]:
    deadline = time.monotonic() + timeout
    last = []
    while time.monotonic() < deadline:
        last = _read_heartbeat_records(path, start_offset)
        if predicate(last):
            return last
        time.sleep(0.1)
    raise FunnelFailure(f"timed out waiting for {what}")


def _enumerate_win_process_ids() -> list[int]:
    """Use CIM only as a live PID list; ownership is checked on a handle."""
    powershell = (Path(os.environ.get("SystemRoot", r"C:\Windows"))
                  / "System32" / "WindowsPowerShell" / "v1.0"
                  / "powershell.exe")
    script = (
        "@(Get-CimInstance Win32_Process -ErrorAction Stop | "
        "ForEach-Object {[int]$_.ProcessId}) | ConvertTo-Json -Compress")
    result = subprocess.run(
        [str(powershell), "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise FunnelFailure(
            "cannot enumerate Win32_Process ids: "
            + (result.stderr or result.stdout).strip())
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise FunnelFailure("cannot decode Win32_Process id list") from exc
    if type(value) is int:
        value = [value]
    if (not isinstance(value, list)
            or any(type(pid) is not int or pid < 0 for pid in value)):
        raise FunnelFailure("Win32_Process id list is invalid")
    # Win32_Process lists the System Idle Process as pid 0.
    return [pid for pid in value if pid != 0]


FIREWALL_RULES_FORMAT = "wot-funnel-firewall-rules-v2"
_FIREWALL_QUERY_ERRORS = (FunnelFailure, OSError, subprocess.SubprocessError,
                          ValueError)


def _list_firewall_rules() -> list[dict]:
    """Every Windows Firewall rule's InstanceID and program path, by a
    read-only query, as ``{"instance_id": ..., "program": ...}``.

    Answering (or dismissing) a firewall prompt leaves rules that name the
    prompting executable, so a new rule naming a cell's executable means that
    cell raised a prompt. Nothing here creates, changes or removes a rule.
    """
    powershell = (Path(os.environ.get("SystemRoot", r"C:\Windows"))
                  / "System32" / "WindowsPowerShell" / "v1.0"
                  / "powershell.exe")
    script = (
        "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
        "ConvertTo-Json -Compress -InputObject "
        "@(Get-NetFirewallApplicationFilter -ErrorAction Stop | "
        "ForEach-Object {@{InstanceID = [string]$_.InstanceID; "
        "Program = [string]$_.Program}})")
    result = subprocess.run(
        [str(powershell), "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
        timeout=120)
    if result.returncode:
        raise FunnelFailure(
            "cannot list Windows Firewall rules: "
            + (result.stderr or result.stdout).strip())
    try:
        value = json.loads(result.stdout.strip() or "[]")
    except json.JSONDecodeError as exc:
        raise FunnelFailure("cannot decode the Windows Firewall rule list") from exc
    if isinstance(value, dict):
        value = [value]
    if (not isinstance(value, list)
            or any(not isinstance(rule, dict)
                   or not isinstance(rule.get("InstanceID"), str)
                   or not isinstance(rule.get("Program"), str)
                   for rule in value)):
        raise FunnelFailure("the Windows Firewall rule list is invalid")
    return [{"instance_id": rule["InstanceID"], "program": rule["Program"]}
            for rule in value]


def _windows_program_is_under(program: str, root: str) -> bool:
    """Case-insensitive Windows containment on whole path components.

    Pure text (PureWindowsPath), so it behaves the same on every host. Rule
    entries that are not absolute paths ("Any", "System", %SystemRoot%...) are
    never under a cell.
    """
    candidate = PureWindowsPath(program)
    parent = PureWindowsPath(root)
    if not candidate.is_absolute() or not parent.is_absolute():
        return False
    candidate_parts = [part.casefold() for part in candidate.parts]
    parent_parts = [part.casefold() for part in parent.parts]
    return (len(candidate_parts) >= len(parent_parts)
            and candidate_parts[:len(parent_parts)] == parent_parts)


def _firewall_rule_key(rule: dict) -> tuple[str, str]:
    return rule["instance_id"], rule["program"]


def _firewall_rules_under(rules: Sequence[dict],
                          cell_roots: Sequence[str]) -> list[dict]:
    """The rules whose program sits under any of ``cell_roots``, sorted."""
    under = {
        _firewall_rule_key(rule): rule for rule in rules
        if any(_windows_program_is_under(rule["program"], root)
               for root in cell_roots)
    }
    return [under[key] for key in sorted(under)]


def _snapshot_firewall_rules() -> dict:
    """Every rule before the first cell launches, or the query's error.

    The whole list is kept: the clients' install roots are known only after
    they launch, so the matching happens when the run ends."""
    try:
        return {"error": None, "rules": _list_firewall_rules()}
    except _FIREWALL_QUERY_ERRORS as exc:
        return {"error": str(exc), "rules": None}


def _firewall_rule_check(roots: Sequence[Path | str],
                         before: dict) -> dict:
    """The read-only firewall evidence for the cells under ``roots``: both
    snapshots (only the rules under the cells) and the rules that are new
    since ``before`` (None when either snapshot failed)."""
    cell_roots = sorted({str(root) for root in roots})

    def snapshot(rules: list[dict] | None, error: str | None) -> dict:
        if error is not None or rules is None:
            return {"error": error or "the rules were not read",
                    "rules_examined": None, "rules_under_cells": None}
        return {"error": None, "rules_examined": len(rules),
                "rules_under_cells": _firewall_rules_under(rules, cell_roots)}

    before_record = snapshot(before.get("rules"), before.get("error"))
    try:
        after_record = snapshot(_list_firewall_rules(), None)
    except _FIREWALL_QUERY_ERRORS as exc:
        after_record = snapshot(None, str(exc))
    new = None
    if before_record["error"] is None and after_record["error"] is None:
        known = {_firewall_rule_key(rule)
                 for rule in before_record["rules_under_cells"]}
        new = [rule for rule in after_record["rules_under_cells"]
               if _firewall_rule_key(rule) not in known]
    return {
        "format": FIREWALL_RULES_FORMAT,
        "cell_roots": cell_roots,
        "before": before_record,
        "after": after_record,
        "new_rules_under_cells": new,
    }


def _record_cell_firewall_rules(evidence: Path,
                                roots: Sequence[Path | str],
                                before: dict) -> dict:
    """Write ``safety/firewall-rules.json``; ``passed`` is false when a rule
    naming a program under a cell root appeared during the run, or when either
    query failed (fail closed: the run cannot then prove it raised no prompt).
    Rules from earlier runs under a reused work root are not this run's."""
    record = _firewall_rule_check(roots, before)
    record["passed"] = record["new_rules_under_cells"] == []
    _atomic_write_json(evidence / "safety" / "firewall-rules.json", record)
    return record


def _path_is_under(path: str, root: Path) -> bool:
    try:
        candidate_path = Path(path).resolve()
        parent_path = root.resolve()
        if _IS_WINDOWS:
            candidate = str(candidate_path).rstrip("\\/").casefold()
            parent = str(parent_path).rstrip("\\/").casefold()
            return candidate == parent or candidate.startswith(parent + os.sep)
        try:
            candidate_path.relative_to(parent_path)
            return True
        except ValueError:
            return False
    except OSError:
        return False


def _terminate_external_pid(pid: int, *, scratch_root: Path) -> dict:
    """Terminate a Windows process only when its pinned handle proves ownership."""
    if not _IS_WINDOWS:
        return {"pid": pid, "action": "skip", "reason": "not-ours"}
    process_access = 0x0001 | 0x1000 | 0x00100000
    handle = _open_process(process_access, False, pid)
    if not handle:
        return {"pid": pid, "action": "skip", "reason": "not-ours"}
    try:
        capacity = 32768
        image = ctypes.create_unicode_buffer(capacity)
        length = ctypes.c_uint32(capacity)
        if not _query_full_process_image_name(
                handle, 0, image, ctypes.byref(length)):
            return {"pid": pid, "action": "skip", "reason": "not-ours"}
        executable = image.value
        if not _path_is_under(executable, scratch_root):
            return {"pid": pid, "action": "skip", "reason": "not-ours",
                    "executable_path": executable}
        if not _terminate_process(handle, 1):
            error = ctypes.get_last_error()
            raise FunnelFailure(
                f"could not terminate owned pid {pid} (Win32 error {error})")
        wait = _wait_for_single_object(handle, 30000)
        if wait != 0:
            raise FunnelFailure(
                f"timed out waiting for terminated owned pid {pid} (wait {wait})")
        return {"pid": pid, "action": "terminated",
                "executable_path": executable}
    finally:
        _close_handle(handle)


def _terminate_owned_pid(platform_name: str, pid: int, *, scratch_root: Path,
                         process_observer=None, token: Optional[str] = None,
                         profile: Optional[Path] = None,
                         direct_process=None) -> dict:
    if platform_name == "Mac":
        if not isinstance(process_observer, MacProcessObserver):
            raise FunnelFailure("Mac termination requires its process observer")
        return _terminate_mac_pid(
            pid, observer=process_observer, allowed_roots=[scratch_root],
            token=token, profile=profile, direct_process=direct_process)
    return _terminate_external_pid(pid, scratch_root=scratch_root)


def _cleanup_owned_mac_processes(observer: MacProcessObserver,
                                 scratch_root: Path) -> list[dict]:
    """Terminate live Mac identities proven to belong to this funnel run."""
    observer.poll()
    events = observer.events()
    live_starts = {}
    for row in events:
        kind = row.get("kind")
        if kind not in {"start", "stop"}:
            continue
        identity = observer.identity(row)
        if kind == "start":
            live_starts[identity] = row
        else:
            live_starts.pop(identity, None)
    starts = list(live_starts.values())
    candidates = []
    seen = set()
    for row in starts:
        if row.get("pid") in seen:
            continue
        argv = row.get("argv", [])
        profiles = [argument.split("=", 1)[1] for argument in argv
                    if argument.startswith("-wottestpreferencesdir=")]
        tokens = [argument.split("=", 1)[1] for argument in argv
                  if argument.startswith("-wotnettesttoken=")]
        if (_path_is_under(row.get("executable_path", ""), scratch_root)
                or (any(_path_is_under(value, scratch_root) for value in profiles)
                    and any(value.startswith("wot-funnel-") for value in tokens))):
            candidates.append(row)
            seen.add(row["pid"])
    candidates.sort(key=lambda row: (row.get("start_time", 0), row["pid"]))
    cleanup = []
    for row in candidates:
        pid = row["pid"]
        executable_root = Path(row["executable_path"]).parent
        try:
            current = observer.authenticate(
                pid, allowed_roots=[executable_root])
        except FunnelFailure as exc:
            if _mac_pid_exists(pid):
                cleanup.append({
                    "action": "aborted-identity-change", "pid": pid,
                    "reason": str(exc), "signals": [],
                })
            continue
        if observer.identity(current) != observer.identity(row):
            cleanup.append({
                "action": "aborted-identity-change", "pid": pid,
                "reason": "identity changed before process cleanup",
                "signals": [],
            })
            continue
        cleanup.append(_terminate_mac_pid(
            pid, observer=observer, allowed_roots=[executable_root]))
    return cleanup


def _quiesce_owned_mac_processes(observer: MacProcessObserver,
                                 scratch_root: Path) -> list[dict]:
    """Prove quiescence with complete polls, independent of poll latency."""
    cleanup = []
    poll_evidence = []
    quiet_polls = 0
    for pass_index in range(1, MAC_CLEANUP_MAX_PASSES + 1):
        started = time.monotonic()
        actions = _cleanup_owned_mac_processes(observer, scratch_root)
        elapsed = round(time.monotonic() - started, 6)
        cleanup.extend(actions)
        poll_evidence.append({
            "actions": len(actions),
            "duration_seconds": elapsed,
            "pass": pass_index,
        })
        quiet_polls = quiet_polls + 1 if not actions else 0
        if quiet_polls == MAC_CLEANUP_QUIET_POLLS:
            cleanup.append({
                "action": "mac-quiescence-proven",
                "consecutive_empty_polls": quiet_polls,
                "polls": poll_evidence,
            })
            return cleanup
        time.sleep(0.1)
    raise FunnelFailure(
        "run-owned Mac processes did not quiesce after "
        f"{MAC_CLEANUP_MAX_PASSES} complete cleanup polls")


def _cleanup_processes_under(root: Path) -> list[dict]:
    if not _IS_WINDOWS:
        return []
    results = [_terminate_external_pid(pid, scratch_root=root)
               for pid in _enumerate_win_process_ids()]
    return [result for result in results
            if result.get("action") == "terminated"]


def _find_update_failed_notice(profile: Path, platform_name: str,
                               failed: int, restored: int) -> Path:
    expected = f"WUF1\nfailed={failed}\nrestored={restored}\nstate=pending\n"
    path = (_preferences_root(profile, platform_name)
            / "WoTUpdateFailedNotice")
    try:
        matches = int(path.is_file()
                      and path.read_text(encoding="utf-8") == expected)
    except (OSError, UnicodeError):
        matches = 0
    if matches != 1:
        raise FunnelFailure(f"expected update-failed notice at {path}")
    return path


def _mac_proc_pidpath(pid: int, library=None) -> str:
    if type(pid) is not int or pid <= 0:
        raise FunnelFailure("proc_pidpath requires a positive pid")
    if library is None:
        if sys.platform != "darwin":
            raise FunnelFailure("proc_pidpath is available only on Darwin")
        library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    function = library.proc_pidpath
    function.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    function.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    length = function(pid, buffer, len(buffer))
    if length <= 0:
        error = ctypes.get_errno()
        raise FunnelFailure(
            f"proc_pidpath could not authenticate pid {pid} (errno {error})")
    if length >= len(buffer):
        raise FunnelFailure(
            f"proc_pidpath returned a truncated path for pid {pid}")
    path = os.fsdecode(buffer.raw[:length].split(b"\0", 1)[0])
    if not path or not os.path.isabs(path):
        raise FunnelFailure(
            f"proc_pidpath returned an invalid path for pid {pid}")
    return path


def _parse_kern_procargs2(raw: bytes) -> list[str]:
    """Decode argv from Darwin's KERN_PROCARGS2 buffer."""
    if len(raw) < struct.calcsize("=i"):
        raise FunnelFailure("KERN_PROCARGS2 returned a truncated buffer")
    argc = struct.unpack_from("=i", raw)[0]
    if argc <= 0:
        raise FunnelFailure("KERN_PROCARGS2 returned an invalid argc")
    offset = struct.calcsize("=i")
    executable_end = raw.find(b"\0", offset)
    if executable_end < 0:
        raise FunnelFailure("KERN_PROCARGS2 omitted the executable terminator")
    offset = executable_end + 1
    while offset < len(raw) and raw[offset] == 0:
        offset += 1
    argv = []
    for _index in range(argc):
        end = raw.find(b"\0", offset)
        if end < 0:
            raise FunnelFailure("KERN_PROCARGS2 returned truncated argv")
        argv.append(os.fsdecode(raw[offset:end]))
        offset = end + 1
    if not argv or not argv[0]:
        raise FunnelFailure("KERN_PROCARGS2 returned empty argv")
    return argv


def _mac_proc_argv(pid: int, library=None) -> list[str]:
    """Read a process's reversible argv through Darwin KERN_PROCARGS2."""
    if type(pid) is not int or pid <= 0:
        raise FunnelFailure("KERN_PROCARGS2 requires a positive pid")
    if library is None:
        if sys.platform != "darwin":
            raise FunnelFailure("KERN_PROCARGS2 is available only on Darwin")
        library = ctypes.CDLL(None, use_errno=True)
    function = library.sysctl
    function.argtypes = [
        ctypes.POINTER(ctypes.c_int), ctypes.c_uint, ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_size_t,
    ]
    function.restype = ctypes.c_int
    mib = (ctypes.c_int * 3)(1, 49, pid)  # CTL_KERN, KERN_PROCARGS2, pid
    size = ctypes.c_size_t()
    if function(mib, 3, None, ctypes.byref(size), None, 0) != 0 or size.value == 0:
        error = ctypes.get_errno()
        raise FunnelFailure(
            f"KERN_PROCARGS2 could not size pid {pid} argv (errno {error})")
    buffer = ctypes.create_string_buffer(size.value)
    if function(mib, 3, buffer, ctypes.byref(size), None, 0) != 0:
        error = ctypes.get_errno()
        raise FunnelFailure(
            f"KERN_PROCARGS2 could not read pid {pid} argv (errno {error})")
    return _parse_kern_procargs2(buffer.raw[:size.value])


def _mac_ps_processes() -> list[dict]:
    completed = subprocess.run(
        ["/bin/ps", "-axo", "pid=,ppid=,uid=,lstart=,command="],
        capture_output=True, text=True, timeout=15)
    if completed.returncode != 0:
        raise FunnelFailure(
            "cannot enumerate Mac processes: "
            + (completed.stderr or completed.stdout).strip())
    rows = []
    for line in completed.stdout.splitlines():
        fields = line.strip().split(None, 8)
        if len(fields) != 9:
            continue
        try:
            pid, parent_pid, uid = map(int, fields[:3])
            started = time.mktime(time.strptime(
                " ".join(fields[3:8]), "%a %b %d %H:%M:%S %Y"))
            argv = shlex.split(fields[8])
        except (ValueError, OverflowError):
            continue
        if pid > 0 and uid >= 0 and argv:
            rows.append({
                "argv": argv,
                "command": fields[8],
                "parent_pid": parent_pid,
                "pid": pid,
                "start_time": started,
                "uid": uid,
            })
    return rows


class MacProcessObserver:
    """Observer with libproc-authenticated paths and KERN_PROCARGS2 argv."""

    def __init__(self, path: Optional[Path] = None, *, ps_reader=None,
                 proc_path=None, proc_argv=None, uid: Optional[int] = None):
        if sys.platform != "darwin" and (ps_reader is None or proc_path is None):
            raise FunnelFailure("the Mac process observer requires Darwin")
        self.path = Path(path) if path is not None else None
        self._ps_reader = ps_reader or _mac_ps_processes
        self._proc_path = proc_path or _mac_proc_pidpath
        self._proc_argv = (proc_argv if proc_argv is not None else
                           _mac_proc_argv
                           if sys.platform == "darwin" and ps_reader is None
                           else None)
        self.uid = os.getuid() if uid is None else uid
        self._lock = threading.RLock()
        self._poll_gate = threading.Lock()
        self._stop_event = threading.Event()
        self._monitor_thread = None
        self._known: dict[tuple, dict] = {}
        self._events: list[dict] = []
        self._event_file = None
        if self.path is not None:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._event_file = self.path.open(
                "w", encoding="utf-8", newline="\n")
        self.poll()
        if ps_reader is None:
            self._monitor_thread = threading.Thread(
                target=self._monitor, name="wot-funnel-mac-observer",
                daemon=True)
            self._monitor_thread.start()

    def _monitor(self) -> None:
        while not self._stop_event.wait(0.05):
            try:
                self.poll()
            except BaseException as exc:
                with self._lock:
                    self._append({"kind": "observer-error", "error": str(exc)})
                return

    def _append(self, row: dict) -> None:
        with self._lock:
            self._events.append(row)
            if self._event_file is not None:
                self._event_file.write(json.dumps(
                    row, ensure_ascii=False, separators=(",", ":")) + "\n")
                self._event_file.flush()

    def poll(self) -> list[dict]:
        with self._poll_gate:
            return self._poll_once()

    def _poll_once(self) -> list[dict]:
        before_rows = {row["pid"]: row for row in self._ps_reader()
                       if row.get("uid") == self.uid}
        after_rows = {row["pid"]: row for row in self._ps_reader()
                      if row.get("uid") == self.uid}
        with self._lock:
            known_by_basic = {
                (row["pid"], row["uid"], row["start_time"]): row
                for row in self._known.values()
            }
        current = {}
        for pid, before in before_rows.items():
            after = after_rows.get(pid)
            if after is None:
                continue
            if any(before.get(key) != after.get(key)
                   for key in ("pid", "uid", "start_time")):
                continue
            basic = (pid, after["uid"], after["start_time"])
            known = known_by_basic.get(basic)
            if known is not None:
                row = dict(known)
            else:
                try:
                    executable_path = self._proc_path(pid)
                    argv = (self._proc_argv(pid) if self._proc_argv is not None
                            else list(after.get("argv", [])))
                except FunnelFailure:
                    continue
                row = dict(after)
                row["argv"] = argv
                row["executable_path"] = executable_path
            row["authenticated"] = True
            current[self.identity(row)] = row
        with self._lock:
            for identity in sorted(set(self._known) - set(current)):
                row = self._known[identity]
                self._append({"kind": "stop", "pid": row["pid"],
                              "uid": row["uid"],
                              "start_time": row["start_time"],
                              "executable_path": row["executable_path"]})
            for identity in sorted(set(current) - set(self._known)):
                row = current[identity]
                self._append({"kind": "start", **row})
            self._known = current
        return self.events()

    def events(self) -> list[dict]:
        with self._lock:
            errors = [row for row in self._events
                      if row.get("kind") == "observer-error"]
            if errors:
                raise FunnelFailure(
                    "Mac process observer failed: " + errors[-1]["error"])
            return [dict(row) for row in self._events]

    def recorded(self, pid: int) -> dict:
        """Return the latest authenticated start record, including exited PIDs."""
        self.poll()
        row = next((value for value in reversed(self._events)
                    if value.get("kind") == "start"
                    and value.get("pid") == pid), None)
        if row is None:
            raise FunnelFailure(f"pid {pid} has no authenticated Mac start")
        result = dict(row)
        result.pop("kind", None)
        return result

    def mark(self) -> int:
        self.poll()
        return len(self._events)

    @staticmethod
    def _normalized_start(row: dict) -> dict:
        result = dict(row)
        result["name"] = Path(result["executable_path"]).name
        return result

    @staticmethod
    def _start_binds_pid(row: dict, parent_pid: int) -> bool:
        return (row.get("parent_pid") == parent_pid
                or f"--wait-pid={parent_pid}" in row.get("argv", []))

    def find_start(self, *, mark: int, parent_pid: int,
                   timeout: float) -> Optional[dict]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll()
            started = next((row for row in self._events[mark:]
                            if row.get("kind") == "start"
                            and self._start_binds_pid(row, parent_pid)), None)
            if started is not None:
                return self._normalized_start(started)
            time.sleep(0.05)
        return None

    def wait_for_start(self, *, mark: int, parent_pid: int,
                       timeout: float) -> dict:
        started = self.find_start(
            mark=mark, parent_pid=parent_pid, timeout=timeout)
        if started is None:
            raise FunnelFailure(
                f"timed out waiting for Mac child of pid {parent_pid}")
        return started

    def wait_for_pid_stop(self, *, mark: int, pid: int,
                          timeout: float) -> tuple[dict, list[dict]]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll()
            events = self.events()[mark:]
            stopped = next((row for row in events
                            if row.get("kind") == "stop"
                            and row.get("pid") == pid), None)
            if stopped is not None:
                started = next((row for row in reversed(self._events[:mark]
                                                        + events)
                                if row.get("kind") == "start"
                                and row.get("pid") == pid), {})
                result = dict(stopped)
                result.update({
                    "exit_status": None,
                    "name": Path(stopped["executable_path"]).name,
                    "parent_pid": started.get("parent_pid", 0),
                })
                normalized_events = []
                for event in events:
                    value = dict(event)
                    if event.get("kind") in {"start", "stop"}:
                        value["name"] = Path(
                            event.get("executable_path", "")).name
                    normalized_events.append(value)
                return result, normalized_events
            time.sleep(0.05)
        raise FunnelFailure(f"timed out waiting for Mac pid {pid} to exit")


    @staticmethod
    def identity(row: dict) -> tuple:
        return (row["pid"], row["uid"], row["start_time"],
                row["executable_path"])

    def describe(self, pid: int, *, before: Optional[dict] = None) -> dict:
        if before is None:
            before = next((row for row in self._ps_reader()
                           if row["pid"] == pid), None)
        if before is None:
            raise FunnelFailure(f"pid {pid} is not running")
        executable_path = self._proc_path(pid)
        after = next((row for row in self._ps_reader()
                      if row["pid"] == pid), None)
        stable_fields = ("pid", "uid", "start_time")
        if (after is None or any(before.get(key) != after.get(key)
                                 for key in stable_fields)):
            raise FunnelFailure(
                f"pid {pid} changed identity during authentication")
        result = dict(after)
        if self._proc_argv is not None:
            result["argv"] = self._proc_argv(pid)
        result["executable_path"] = executable_path
        result["authenticated"] = True
        return result

    def authenticate(self, pid: int, *, allowed_roots: Sequence[Path],
                     token: Optional[str] = None,
                     profile: Optional[Path] = None,
                     started_after: Optional[float] = None) -> dict:
        row = self.describe(pid)
        if row["uid"] != self.uid:
            raise FunnelFailure(f"pid {pid} belongs to another uid")
        if started_after is not None and row["start_time"] + 1 < started_after:
            raise FunnelFailure(f"pid {pid} predates the funnel launch")
        if not any(_path_is_under(row["executable_path"], root)
                   for root in allowed_roots):
            raise FunnelFailure(f"pid {pid} executable is outside funnel roots")
        if token is not None and not any(
                arg in {f"-wotnettesttoken={token}", f"--token={token}"}
                for arg in row["argv"]):
            raise FunnelFailure(f"pid {pid} does not carry the funnel token")
        if (profile is not None
                and f"-wottestpreferencesdir={profile}" not in row["argv"]):
            raise FunnelFailure(f"pid {pid} does not carry the funnel profile")
        return row

    def wait_for_authenticated(
            self, *, allowed_roots: Sequence[Path], token: str,
            profile: Path, started_after: float, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for row in self._ps_reader():
                argv = row.get("argv", [])
                if f"-wotnettesttoken={token}" not in argv:
                    continue
                try:
                    return self.authenticate(
                        row["pid"], allowed_roots=allowed_roots, token=token,
                        profile=profile, started_after=started_after)
                except FunnelFailure:
                    continue
            time.sleep(0.05)
        raise FunnelFailure("timed out waiting for an authenticated Mac process")

    def observe_identity(self, pid: int, timeout: float = 5.0) -> dict:
        """Return the live pid plus the ps start time used as its identity."""
        deadline = time.monotonic() + timeout
        last_error = None
        while time.monotonic() < deadline:
            try:
                row = self.describe(pid)
                return {
                    "pid": pid,
                    "process_start_id": f"{row['start_time']:.6f}",
                    "observed_monotonic_seconds": time.monotonic(),
                }
            except FunnelFailure as exc:
                last_error = exc
                time.sleep(0.01)
        raise FunnelFailure(
            f"timed out waiting for Mac process identity for pid {pid}: "
            f"{last_error}")

    def close(self) -> None:
        self._stop_event.set()
        if self._monitor_thread is not None:
            self._monitor_thread.join(timeout=5)
            if self._monitor_thread.is_alive():
                raise FunnelFailure("Mac process observer did not stop")
            self._monitor_thread = None
        if self._event_file is not None:
            self._event_file.close()
            self._event_file = None

    def adopt(self, popen: subprocess.Popen, *, may_exit: bool = False) -> None:
        """Confirm a direct launch is visible; Mac descendants are observed by ps.

        A launch designed to exit at once (the bad build, the rollback relaunch)
        can finish before ps sees it; KERN_PROCARGS2 then fails on the zombie.
        With may_exit the caller owns the verdict through the exit status."""
        deadline = time.monotonic() + 5.0
        last_error = None
        while time.monotonic() < deadline:
            try:
                self.authenticate(
                    popen.pid,
                    allowed_roots=[Path(popen.args[0]).resolve().parent])
                return
            except FunnelFailure as exc:
                last_error = exc
                if popen.poll() is not None:
                    if may_exit:
                        return
                    break
                time.sleep(0.05)
        raise FunnelFailure(
            f"could not adopt Mac pid {popen.pid}: {last_error}")

    def __enter__(self):
        return self

    def __exit__(self, _type, _value, _traceback) -> None:
        self.close()


def _mac_pid_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        if sys.platform == "darwin":
            completed = subprocess.run(
                ["/bin/ps", "-p", str(pid), "-o", "stat="],
                capture_output=True, text=True, timeout=5)
            return (completed.returncode == 0
                    and bool(completed.stdout.strip())
                    and not completed.stdout.lstrip().startswith("Z"))
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def _mac_open_exit_watch(pid: int):
    """Best-effort kernel tracking of the admitted process, not a reused PID."""
    if sys.platform != "darwin" or not hasattr(select, "kqueue"):
        return None
    queue = None
    try:
        queue = select.kqueue()
        event = select.kevent(
            pid, filter=select.KQ_FILTER_PROC,
            flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
            fflags=select.KQ_NOTE_EXIT)
        queue.control([event], 0, 0)
        return queue
    except OSError:
        if queue is not None:
            queue.close()
        return None


def _mac_exit_observed(watch) -> bool:
    return bool(watch is not None and watch.control(None, 1, 0))


# POSIX has no identity-bound kill primitive for a non-child process. macOS
# allocates PIDs sequentially, so a non-child is re-read immediately before
# each kill(2), and a mismatch in any (pid, uid, start_time, executable_path)
# field aborts. A residual check-to-kill race remains. Direct unreaped children
# instead use their Popen handle; their PID cannot be reused before they are
# reaped.
def _terminate_mac_pid(pid: int, *, observer: MacProcessObserver,
                       allowed_roots: Sequence[Path],
                       token: Optional[str] = None,
                       profile: Optional[Path] = None,
                       direct_process: Optional[subprocess.Popen] = None,
                       timeout: float = 5.0) -> dict:
    authenticated = observer.authenticate(
        pid, allowed_roots=allowed_roots, token=token, profile=profile)
    if direct_process is not None:
        if direct_process.pid != pid or direct_process.poll() is not None:
            return {"action": "aborted-identity-change", "pid": pid,
                    "reason": "direct child is not the live admitted process",
                    "signals": []}
        direct_process.terminate()
        try:
            direct_process.wait(timeout=timeout)
            signals = ["SIGTERM"]
        except subprocess.TimeoutExpired:
            direct_process.kill()
            direct_process.wait(timeout=timeout)
            signals = ["SIGTERM", "SIGKILL"]
        return {"action": "terminated", "executable_path":
                authenticated["executable_path"], "pid": pid,
                "signals": signals}
    pinned = observer.identity(authenticated)
    exit_watch = _mac_open_exit_watch(pid)
    try:
        before_term = observer.authenticate(
            pid, allowed_roots=allowed_roots, token=token, profile=profile)
    except FunnelFailure as exc:
        if exit_watch is not None:
            exit_watch.close()
        return {"action": "aborted-identity-change", "pid": pid,
                "reason": str(exc), "signals": []}
    if observer.identity(before_term) != pinned:
        if exit_watch is not None:
            exit_watch.close()
        return {"action": "aborted-identity-change", "pid": pid,
                "reason": "identity changed before SIGTERM", "signals": []}
    try:
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if _mac_exit_observed(exit_watch) or not _mac_pid_exists(pid):
                return {"action": "terminated", "executable_path":
                        authenticated["executable_path"], "pid": pid,
                        "signals": ["SIGTERM"]}
            time.sleep(0.05)
        # Re-authenticate immediately before escalation so PID reuse cannot
        # target a process that was not the one admitted above.
        try:
            before_kill = observer.authenticate(
                pid, allowed_roots=allowed_roots, token=token, profile=profile)
        except FunnelFailure as exc:
            return {"action": "aborted-identity-change",
                    "executable_path": authenticated["executable_path"],
                    "pid": pid, "reason": str(exc), "signals": ["SIGTERM"]}
        if observer.identity(before_kill) != pinned:
            return {"action": "aborted-identity-change",
                    "executable_path": authenticated["executable_path"],
                    "pid": pid, "reason": "identity changed before SIGKILL",
                    "signals": ["SIGTERM"]}
        os.kill(pid, signal.SIGKILL)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if _mac_exit_observed(exit_watch) or not _mac_pid_exists(pid):
                return {"action": "terminated", "executable_path":
                        authenticated["executable_path"], "pid": pid,
                        "signals": ["SIGTERM", "SIGKILL"]}
            time.sleep(0.05)
        raise FunnelFailure(f"owned Mac pid {pid} did not exit after SIGKILL")
    finally:
        if exit_watch is not None:
            exit_watch.close()


class _WinJobProcessObserver:
    """Capture the process tree owned by one runner through a Windows job."""

    _JOB_OBJECT_ASSOCIATE_COMPLETION_PORT_INFORMATION = 7
    _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
    _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
    _JOB_OBJECT_MSG_NEW_PROCESS = 6
    _JOB_OBJECT_MSG_EXIT_PROCESS = 7
    _JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS = 8
    _PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    _SYNCHRONIZE = 0x00100000
    _INFINITE = 0xFFFFFFFF

    def __init__(self, path: Path):
        if os.name != "nt":
            raise FunnelFailure("the Win64 process observer requires Windows")
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._events: list[dict] = []
        self._processes: dict[int, dict] = {}
        self._reader_error: BaseException | None = None
        self._closed = False
        self._job = None
        self._completion_port = None
        self._reader_thread: threading.Thread | None = None
        self._event_file = self.path.open(
            "w", encoding="utf-8", newline="\n")
        self._event_file.write('{"ready":true}\n')
        self._event_file.flush()
        try:
            self._job = _create_job_object(None, None)
            if not self._job:
                self._raise_last_win_error("could not create process-observer job")
            invalid_handle = ctypes.c_void_p(-1).value
            self._completion_port = _create_io_completion_port(
                invalid_handle, None, 0, 1)
            if not self._completion_port:
                self._raise_last_win_error(
                    "could not create process-observer completion port")

            limits = _JobObjectExtendedLimitInformation()
            limits.BasicLimitInformation.LimitFlags = (
                self._JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
            if not _set_information_job_object(
                    self._job,
                    self._JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                    ctypes.byref(limits), ctypes.sizeof(limits)):
                self._raise_last_win_error(
                    "could not set process-observer job limits")

            association = _JobObjectAssociateCompletionPort()
            association.CompletionKey = 1
            association.CompletionPort = self._completion_port
            if not _set_information_job_object(
                    self._job,
                    self._JOB_OBJECT_ASSOCIATE_COMPLETION_PORT_INFORMATION,
                    ctypes.byref(association), ctypes.sizeof(association)):
                self._raise_last_win_error(
                    "could not associate the process-observer completion port")

            self._reader_thread = threading.Thread(
                target=self._read_messages,
                name="wot-funnel-job-observer",
                daemon=True)
            self._reader_thread.start()
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _raise_last_win_error(action: str) -> None:
        raise FunnelFailure(
            f"{action} (Win32 error {ctypes.get_last_error()})")

    def _read_messages(self) -> None:
        try:
            while True:
                message = ctypes.c_uint32()
                key = ctypes.c_size_t()
                process_id = ctypes.c_void_p()
                if not _get_queued_completion_status(
                        self._completion_port, ctypes.byref(message),
                        ctypes.byref(key), ctypes.byref(process_id),
                        self._INFINITE):
                    self._raise_last_win_error(
                        "process-observer completion-port read failed")
                if message.value == 0 and key.value == 0 and not process_id.value:
                    return
                if key.value == 1 and process_id.value:
                    self._handle_message(message.value, int(process_id.value))
        except BaseException as exc:
            self._reader_error = exc

    def _handle_message(self, message: int, pid: int) -> None:
        if message == self._JOB_OBJECT_MSG_NEW_PROCESS:
            handle = _open_process(
                self._PROCESS_QUERY_LIMITED_INFORMATION | self._SYNCHRONIZE,
                False, pid)
            name = ""
            parent_pid = None
            process_start_id = None
            if handle:
                capacity = 32768
                image = ctypes.create_unicode_buffer(capacity)
                length = ctypes.c_uint32(capacity)
                if _query_full_process_image_name(
                        handle, 0, image, ctypes.byref(length)):
                    name = PureWindowsPath(image.value).name
                basic = _ProcessBasicInformation()
                returned = ctypes.c_uint32()
                status = _nt_query_information_process(
                    handle, 0, ctypes.byref(basic), ctypes.sizeof(basic),
                    ctypes.byref(returned))
                if status >= 0:
                    parent_pid = int(basic.InheritedFromUniqueProcessId)
                process_start_id = self._process_start_id(handle)
            with self._lock:
                self._processes[pid] = {
                    "handle": handle,
                    "name": name,
                    "parent_pid": parent_pid,
                    "process_start_id": process_start_id,
                }
            self._append_event({
                "kind": "start",
                "pid": pid,
                "name": name,
                "parent_pid": parent_pid,
            })
        elif message in {
                self._JOB_OBJECT_MSG_EXIT_PROCESS,
                self._JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS}:
            with self._lock:
                process = self._processes.pop(pid, {
                    "handle": None, "name": "", "parent_pid": None,
                    "process_start_id": None})
            handle = process["handle"]
            exit_status = None
            if handle:
                try:
                    status = ctypes.c_uint32()
                    if _get_exit_code_process(handle, ctypes.byref(status)):
                        exit_status = int(status.value)
                finally:
                    _close_handle(handle)
            self._append_event({
                "kind": "stop",
                "pid": pid,
                "name": process["name"],
                "parent_pid": process["parent_pid"],
                "exit_status": exit_status,
            })

    @staticmethod
    def _process_start_id(handle) -> str:
        creation = _FileTime()
        exit_time = _FileTime()
        kernel = _FileTime()
        user = _FileTime()
        if not _get_process_times(
                handle, ctypes.byref(creation), ctypes.byref(exit_time),
                ctypes.byref(kernel), ctypes.byref(user)):
            _WinJobProcessObserver._raise_last_win_error(
                "could not query process-observer start identity")
        return f"{creation.high:08x}{creation.low:08x}"

    def _append_event(self, row: dict) -> None:
        with self._lock:
            self._events.append(row)
            self._event_file.write(json.dumps(
                row, ensure_ascii=False, separators=(",", ":")) + "\n")
            self._event_file.flush()

    def _raise_reader_error(self) -> None:
        if self._reader_error is not None:
            raise FunnelFailure(
                f"Win64 process observer failed: {self._reader_error}") \
                from self._reader_error

    def adopt(self, popen: subprocess.Popen) -> None:
        self._raise_reader_error()
        if self._closed or not self._job:
            raise FunnelFailure("cannot adopt a process into a closed observer")
        process_handle = getattr(popen, "_handle", None)
        if process_handle is None:
            raise FunnelFailure("launched process has no Windows process handle")
        # Launcher children may appear after startup, so
        # assigning its still-live Popen handle immediately closes the only
        # inheritance race before the game child is created.
        if not _assign_process_to_job_object(self._job, int(process_handle)):
            self._raise_last_win_error(
                f"could not adopt launched pid {popen.pid} into observer job")

    def observe_identity(self, pid: int, timeout: float = 5.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._raise_reader_error()
            with self._lock:
                process = self._processes.get(pid)
                if process is not None:
                    handle = process["handle"]
                    admitted_start = process["process_start_id"]
                    if not handle or not admitted_start:
                        raise FunnelFailure(
                            f"process observer could not authenticate pid {pid}")
                    status = ctypes.c_uint32()
                    if (not _get_exit_code_process(handle, ctypes.byref(status))
                            or status.value != 259):
                        raise FunnelFailure(
                            f"process observer pid {pid} is no longer active")
                    current_start = self._process_start_id(handle)
                    if current_start != admitted_start:
                        raise FunnelFailure(
                            f"process observer pid {pid} changed start identity")
                    return {
                        "pid": pid,
                        "process_start_id": current_start,
                        "observed_monotonic_seconds": time.monotonic(),
                    }
            time.sleep(0.01)
        raise FunnelFailure(
            f"timed out waiting for process-observer identity for pid {pid}")

    def events(self) -> list[dict]:
        self._raise_reader_error()
        with self._lock:
            return [dict(row) for row in self._events]

    def mark(self) -> int:
        return len(self.events())


    def wait_for_start(self, *, mark: int, parent_pid: int,
                       timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            started = next((row for row in self.events()[mark:]
                            if row.get("kind") == "start"
                            and row.get("parent_pid") == parent_pid), None)
            if started is not None:
                return started
            time.sleep(0.05)
        raise FunnelFailure(
            f"timed out waiting for child of {parent_pid} to start")

    def find_start(self, *, mark: int, parent_pid: int,
                   timeout: float) -> dict | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            started = next((row for row in self.events()[mark:]
                            if row.get("kind") == "start"
                            and row.get("parent_pid") == parent_pid), None)
            if started is not None:
                return started
            time.sleep(0.05)
        return None

    def wait_for_pid_stop(self, *, mark: int, pid: int,
                          timeout: float) -> tuple[dict, list[dict]]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            events = self.events()[mark:]
            stopped = next((row for row in events
                            if row.get("kind") == "stop"
                            and row.get("pid") == pid), None)
            if stopped is not None:
                return stopped, events
            time.sleep(0.05)
        raise FunnelFailure(f"timed out waiting for pid {pid} to exit")


    def close(self) -> None:
        if self._closed:
            return
        close_error = None
        if self._job:
            if not _close_handle(self._job):
                close_error = FunnelFailure(
                    "could not close process-observer job "
                    f"(Win32 error {ctypes.get_last_error()})")
            self._job = None
        if self._completion_port and self._reader_thread is not None:
            if not _post_queued_completion_status(
                    self._completion_port, 0, 0, None):
                close_error = close_error or FunnelFailure(
                    "could not stop process-observer reader "
                    f"(Win32 error {ctypes.get_last_error()})")
            self._reader_thread.join(timeout=5)
            if self._reader_thread.is_alive():
                raise FunnelFailure("process-observer reader did not stop")
        for process in self._processes.values():
            if process["handle"]:
                _close_handle(process["handle"])
        self._processes.clear()
        if self._completion_port:
            if not _close_handle(self._completion_port):
                close_error = close_error or FunnelFailure(
                    "could not close process-observer completion port "
                    f"(Win32 error {ctypes.get_last_error()})")
            self._completion_port = None
        self._event_file.close()
        self._closed = True
        if close_error is not None:
            raise close_error
        self._raise_reader_error()

    def __enter__(self) -> "_WinJobProcessObserver":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


def _make_update_process_observer(platform_name: str, path: Path):
    if platform_name == "Mac":
        return MacProcessObserver(path)
    if platform_name == "Win64":
        return _WinJobProcessObserver(path)
    raise FunnelFailure(f"unsupported funnel platform {platform_name!r}")


def _require_empty_directory(path: Path, what: str) -> None:
    if path.exists() and (not path.is_dir() or any(path.iterdir())):
        raise FunnelFailure(f"{what} is dirty; expected an empty root: {path}")
    path.mkdir(parents=True, exist_ok=True)


def _make_scratch_root(supplied: Path | None) -> tuple[Path, bool]:
    if supplied is not None:
        root = supplied.expanduser().resolve()
        _require_empty_directory(root, "funnel work root")
        return root, False
    # Canonicalize /var -> /private/var before a guarded launch.
    return Path(tempfile.mkdtemp(prefix="wot-first-run-funnel-")).resolve(), True


def _extract_release(asset: Path, destination: Path, platform_name: str) -> Path:
    _require_empty_directory(destination, "release extraction root")
    if platform_name == "Mac":
        result = subprocess.run(
            ["ditto", "-x", "-k", str(asset), str(destination)],
            capture_output=True, text=True,
        )
        if result.returncode:
            raise FunnelFailure(
                f"cannot extract guarded Mac app: {result.stderr.strip()}"
            )
        root = destination / "WoT.app"
    else:
        with zipfile.ZipFile(asset) as archive:
            base = destination.resolve()
            for item in archive.infolist():
                target = (destination / item.filename).resolve()
                try:
                    target.relative_to(base)
                except ValueError as exc:
                    raise FunnelFailure(
                        f"unsafe release archive member: {item.filename}"
                    ) from exc
            archive.extractall(destination)
        install_root = destination / "WoT Reborn"
        launcher = install_root / "WoT Reborn.exe"
        root = install_root / "Game"
        if not launcher.is_file():
            raise FunnelFailure("Windows release is missing WoT Reborn.exe")
    if not root.exists():
        raise FunnelFailure("extracted guarded release tree is missing")
    return root


def _clone_release_tree(source: Path, destination: Path) -> Path:
    if destination.exists():
        raise FunnelFailure(f"release clone already exists: {destination}")
    # Installed-session attestation requires every file in the guarded tree to
    # have exactly one name. Hard-link cloning makes both the base extraction
    # and the cell copy fail closed before bootstrap can publish its session.
    if source.name == "Game" and (source.parent / "WoT Reborn.exe").is_file():
        if destination.name != "Game":
            raise FunnelFailure("Windows release clone destination is not Game")
        # The single Windows folder holds WoT Reborn.exe, Game and, once a cell
        # has run, its Data (host-join reuses fresh-first-run's built cache).
        # The installer a previous cell placed beside them is not part of the
        # release: each cell places its own, and _copy_installer_adjacent
        # refuses an existing one.
        top = source.parent

        def skip_adjacent_installer(directory: str, names: list) -> list:
            if Path(directory) == top and INSTALLER_NAME in names:
                return [INSTALLER_NAME]
            return []

        shutil.copytree(top, destination.parent, symlinks=True,
                        copy_function=shutil.copy2,
                        ignore=skip_adjacent_installer)
    else:
        shutil.copytree(source, destination, symlinks=True,
                        copy_function=shutil.copy2)
    return destination


def _release_clone_source(source: Path) -> Path:
    """The exact tree copied for one cell, including Win64's stable siblings."""
    if source.name == "Game" and (source.parent / "WoT Reborn.exe").is_file():
        return source.parent
    return source


def _cell_tree_destination(base_tree: Path, scratch: Path, name: str) -> Path:
    """Keep each app/exe tree in its own parent for adjacent-file isolation."""
    if base_tree.name == "Game":
        return scratch / "cell-trees" / name / "WoT Reborn" / "Game"
    return scratch / "cell-trees" / name / base_tree.name


def _resolve_executable(tree: Path, platform_name: str) -> Path:
    if platform_name == "Mac":
        plist = tree / "Contents" / "Info.plist"
        try:
            with plist.open("rb") as handle:
                leaf = plistlib.load(handle).get("CFBundleExecutable")
        except (OSError, plistlib.InvalidFileException) as exc:
            raise FunnelFailure(f"cannot read packaged Info.plist: {exc}") from exc
        _safe_leaf(leaf, "CFBundleExecutable")
        executable = tree / "Contents" / "MacOS" / leaf
    else:
        candidates = sorted(tree.rglob("WoT*-Test.exe"))
        if len(candidates) != 1:
            candidates = sorted(tree.rglob("WoT*.exe"))
        if len(candidates) != 1:
            raise FunnelFailure("cannot identify one packaged Win64 game executable")
        executable = candidates[0]
    if not executable.is_file():
        raise FunnelFailure(f"packaged executable is missing: {executable}")
    return executable


def _validate_release_base_url(url: str) -> None:
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != "http" or parsed.hostname != "127.0.0.1"
            or parsed.port is None or not parsed.path
            or parsed.path == "/" or parsed.query or parsed.fragment):
        raise FunnelFailure(
            "test release base URL must be loopback HTTP with a path marker")


def _require_runtime_endpoint_seam(executable: Path) -> None:
    # Test receipts prove that only the announcement and release endpoints can
    # be redirected to loopback. Original-game data has no network endpoint.
    with executable.open("rb") as handle:
        data = handle.read()
    missing = [
        name for name in (TEST_ANNOUNCEMENT_URL_ENV,
                          TEST_RELEASE_BASE_URL_ENV)
        if not any(name.encode(encoding) in data
                   for encoding in ("ascii", "utf-16-le", "utf-32-le"))
    ]
    if missing:
        raise FunnelFailure(
            "guarded Test executable is missing runtime endpoint seams "
            f"({', '.join(missing)}); refusing a repeatable-cell launch "
            "that could contact the public network"
        )


_WIN_INSTALL_ROOTS: dict[Path, Path] = {}


def _win_install_root_from_executable(executable: Path) -> Path:
    current = executable.resolve().parent
    while current != current.parent:
        if current.name.casefold() == "game":
            return current.parent
        current = current.parent
    raise FunnelFailure("Win64 executable is not below the live Game tree")


def _profile_environment(profile: Path, platform_name: str,
                         install_root: Path | None = None) -> dict[str, str]:
    environment = os.environ.copy()
    if platform_name == "Mac":
        environment["HOME"] = str(profile)
        environment["CFFIXED_USER_HOME"] = str(profile)
        # NSSearchPathForDirectoriesInDomains honors CFFIXED_USER_HOME but does
        # not create the redirected Application Support base. A real profile
        # already has it; prepare the equivalent base for each fresh cell.
        (profile / "Library" / "Application Support").mkdir(
            parents=True, exist_ok=True)
        state = _state_root(profile, platform_name).resolve()
        try:
            state.relative_to(profile.resolve())
        except ValueError as exc:
            raise FunnelFailure(
                "Mac state root escapes the redirected funnel profile") from exc
    else:
        # The redirected roots must EXIST before launch: a Windows client
        # whose %LOCALAPPDATA% names a nonexistent directory exits 1 before
        # any logging or heartbeat, and the graphics driver then creates its
        # cache dirs inside the dying process - so every manual relaunch of
        # the same profile "works", hiding the defect from everything except
        # the harness's always-fresh profiles (bisected 2026-08-04). A real
        # user's AppData always exists; the harness must match that contract.
        local = profile / "AppData" / "Local"
        roaming = profile / "AppData" / "Roaming"
        temporary = local / "Temp"
        local.mkdir(parents=True, exist_ok=True)
        roaming.mkdir(parents=True, exist_ok=True)
        temporary.mkdir(parents=True, exist_ok=True)
        environment["USERPROFILE"] = str(profile)
        environment["LOCALAPPDATA"] = str(local)
        environment["APPDATA"] = str(roaming)
        environment["TMP"] = str(temporary)
        environment["TEMP"] = str(temporary)
        _state_root(profile, platform_name, install_root).mkdir(
            parents=True, exist_ok=True)
    # Never let a guarded funnel launch inspect the login account's Downloads.
    # Each cell owns the profile named by its cell, so this is both per-cell and
    # inside the caller's work root. The directory starts empty and remains
    # available for a cell that models a browser completing a download.
    downloads = profile / "FunnelDownloads"
    downloads.mkdir(parents=True, exist_ok=True)
    environment[TEST_DOWNLOADS_DIR_ENV] = str(downloads.resolve())
    return environment


def _state_root(profile: Path, platform_name: str,
                install_root: Path | None = None) -> Path:
    if platform_name == "Mac":
        return profile / "Library" / "Application Support" / "WoT Reborn"
    install = install_root or _WIN_INSTALL_ROOTS.get(profile.resolve())
    if install is None:
        raise FunnelFailure("Win64 state-root lookup has no bound install root")
    return install / "Data"


def _preferences_root(profile: Path, platform_name: str,
                      install_root: Path | None = None) -> Path:
    return _state_root(profile, platform_name, install_root) / "Preferences"


# Unreal Engine's per-machine identifier store. FGenericPlatformMisc::GetMachineId
# (GenericPlatformMisc.cpp:2137) persists [Unreal Engine/Identifiers] MachineId
# through FPlatformMisc::SetStoredValue on every first launch of any UE app; on the
# Mac that is this file, shared by all Unreal software (Windows keeps it in the
# registry). It is allowed as the single engine exception to the
# one-folder rule, and only while it holds nothing but that identifier.
ENGINE_IDENTIFIER_STORE = (
    Path("Application Support") / "Epic" / "Epic Games" / "KeyValueStore.ini")
_ENGINE_MACHINE_ID = re.compile(r"^MachineId=[0-9A-Fa-f]{32}$")


def _require_engine_identifier_store_only(path: Path) -> None:
    """The exception covers UE's MachineId and nothing else."""
    lines = [line.strip() for line in
             path.read_text(encoding="utf-8", errors="replace").splitlines()
             if line.strip()]
    if (len(lines) != 2 or lines[0] != "[Unreal Engine/Identifiers]"
            or not _ENGINE_MACHINE_ID.match(lines[1])):
        raise FunnelFailure(
            "fresh Mac launch wrote more than the engine MachineId to "
            f"{ENGINE_IDENTIFIER_STORE.as_posix()}")


def _require_mac_profile_library_contained(profile: Path) -> dict:
    """Fail if a fresh Mac launch wrote outside R or known OS residue."""
    library = profile / "Library"
    allowed_directories = (
        Path("Application Support") / "WoT Reborn",
        Path("Caches") / "com.aleiby.wot",
        Path("Saved Application State"),
        Path("HTTPStorages"),
        Path("Logs") / "DiagnosticReports",
    )
    allowed_files = (Path("Preferences") / "com.aleiby.wot.plist",
                     ENGINE_IDENTIFIER_STORE)

    def is_allowed(relative: Path) -> bool:
        parts = relative.parts
        return any(relative == leaf or leaf.parts[:len(parts)] == parts
                   for leaf in allowed_files) or any(
            parts[:len(root.parts)] == root.parts
            or root.parts[:len(parts)] == parts
            for root in allowed_directories
        )

    entries = []
    outside = []
    if library.exists():
        for path in sorted(library.rglob("*")):
            relative = path.relative_to(library)
            row = relative.as_posix()
            entries.append(row)
            if not is_allowed(relative):
                outside.append(row)
    if outside:
        raise FunnelFailure(
            "fresh Mac launch wrote outside Library/Application Support/"
            f"WoT Reborn: {outside}")
    identifier_store = library / ENGINE_IDENTIFIER_STORE
    if identifier_store.is_file():
        _require_engine_identifier_store_only(identifier_store)
    return {
        "engine_identifier_store": (
            "machine-id-only" if identifier_store.is_file() else "absent"),
        "allowed_roots": [path.as_posix() for path in allowed_directories],
        "allowed_files": [path.as_posix() for path in allowed_files],
        "entries": entries,
        "outside_root": [],
    }


def _require_win_profile_has_no_game_data(profile: Path) -> dict:
    """Prove no WoT or Epic-product state reached redirected AppData."""
    appdata = profile / "AppData"
    entries = []
    forbidden = []
    if appdata.exists():
        for path in sorted(appdata.rglob("*")):
            relative = path.relative_to(appdata)
            row = relative.as_posix()
            entries.append(row)
            folded = tuple(part.casefold() for part in relative.parts)
            compact_parts = tuple(
                re.sub(r"[\s._-]+", "", part) for part in folded)
            if any(part.startswith((
                    "wot", "epic", "unrealengine", "crashreportclient"))
                    for part in compact_parts):
                forbidden.append(row)
    if forbidden:
        raise FunnelFailure(
            "Win64 launch wrote game data into redirected AppData: "
            + ", ".join(forbidden[:8]))
    return {"entries": entries, "forbidden": forbidden}


def _boot_failures(state_root: Path) -> list[Path]:
    """Boot-failure receipts anywhere under the state root except the cache.

    This runs every ~0.1 s while the game builds its cache. Listing
    ContentCache would hold a directory handle inside `building`, and Windows
    refuses the game's `building` -> `current` publish rename while any other
    process has a handle open below that directory.
    """
    if not state_root.exists():
        return []
    found = []
    for directory, subdirectories, files in os.walk(state_root):
        subdirectories[:] = [name for name in subdirectories
                             if name != "ContentCache"]
        if BOOT_FAILURE_LEAF in files:
            found.append(Path(directory) / BOOT_FAILURE_LEAF)
    return sorted(found)


def _manifests(state_root: Path) -> list[Path]:
    manifest = state_root / "ContentCache" / "current" / "manifest.json"
    return [manifest] if manifest.is_file() else []


def _heartbeat_launch_boundary(
    state_root: Path,
) -> tuple[Path, frozenset[Path]]:
    root = state_root / "UpdaterState"
    try:
        existing = frozenset(
            path for path in root.glob(HEARTBEAT_GLOB) if path.is_file())
    except OSError as exc:
        raise FunnelFailure(
            f"cannot snapshot bootstrap heartbeat filenames: {exc}") from exc
    return root, existing


def _select_startup_critical_blob(
    manifest: dict, startup_critical_logical_ids: Iterable[str],
) -> dict:
    critical = frozenset(startup_critical_logical_ids)
    for row in manifest.get("blobs", []):
        if (isinstance(row, dict)
                and row.get("logical_id") in critical
                and re.fullmatch(r"[0-9a-f]{64}", str(row.get("sha256", "")))):
            return row
    raise FunnelFailure(
        "current cache manifest has no startup-critical manifest-listed blob")


def _snapshot_cached_launch_state(
    state_root: Path,
) -> dict[str, tuple[int, int, int]]:
    cache_root = state_root / "ContentCache"
    if not cache_root.is_dir():
        return {}
    result = {}
    for path in sorted(cache_root.rglob("*")):
        try:
            stat_result = path.stat()
        except FileNotFoundError:
            continue
        relative = path.relative_to(cache_root).as_posix()
        result[relative + ("/" if path.is_dir() else "")] = (
            -1 if path.is_dir() else stat_result.st_size,
            stat_result.st_mtime_ns, stat_result.st_ctime_ns,
        )
    return result


def _cached_launch_mutation_counts(
    before: dict[str, tuple[int, int, int]],
    after: dict[str, tuple[int, int, int]],
) -> dict[str, int]:
    changed = {
        path for path in set(before) | set(after)
        if before.get(path) != after.get(path)
    }
    return {"cache_writes": len(changed)}


def _network_bytes(requests: list[dict]) -> int:
    total = 0
    for request in requests:
        value = request.get("response_bytes")
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise FunnelFailure("loopback request response_bytes is invalid")
        total += value
    return total


def _update_roles(evidence: dict, expected: tuple[str, ...]) -> dict[str, dict]:
    roles = {process["role"]: process for process in evidence["processes"]}
    if tuple(roles) != expected:
        raise FunnelFailure(
            f"update process roles are {tuple(roles)!r}, not {expected!r}")
    return roles


def _judge_update_apply(evidence: dict, platform_name: str = "Win64") -> None:
    try:
        formats.validate_update_cell_evidence(
            "update-apply", evidence, platform_name)
    except formats.FormatError as exc:
        raise FunnelFailure(str(exc)) from exc


def _judge_update_bad_build(evidence: dict,
                            platform_name: str = "Win64") -> None:
    name = "update-bad-build-rolls-back"
    try:
        formats.validate_update_cell_evidence(name, evidence, platform_name)
    except formats.FormatError as exc:
        raise FunnelFailure(str(exc)) from exc


def _classify_post_ready_update_check_requests(
    requests: list[dict],
    heartbeat_records: list[dict],
    expected_pid: int,
) -> tuple[list[dict], list[dict]]:
    ready_timestamps = [
        _parse_heartbeat_timestamp(record.get("ts_utc"), "Ready record")
        for record in heartbeat_records
        if record.get("pid") == expected_pid
        and record.get("event") == "phase-ready"
        and record.get("phase") == "Ready"
    ]

    update_checks = []
    outside = []
    for request in requests:
        is_update_get = (
            request.get("method") == "GET"
            and request.get("path") in {
                "/announcement.json", "/announcement.json.sig",
                RELEASE_NOTES_ROUTE,
            }
        )
        if is_update_get:
            request_timestamp = _parse_heartbeat_timestamp(
                request.get("timestamp_utc"), "loopback request"
            )
            if any(
                request_timestamp >= ready_timestamp
                for ready_timestamp in ready_timestamps
            ):
                update_checks.append(request)
                continue
        outside.append(request)
    return update_checks, outside


def _snapshot_path(path: Path) -> dict:
    result = {
        "path": str(path),
        "exists": path.exists(),
    }
    if path.is_file():
        try:
            stat_result = path.stat()
            result.update({
                "size": stat_result.st_size,
                "mtime": stat_result.st_mtime,
                "sha256": _sha256_file(path),
            })
            if path.suffix == ".json":
                result["json"] = _read_json(path, "evidence")
        except (PermissionError, FileNotFoundError) as exc:
            # Live-state snapshots race the client's own writers. On Windows
            # an exclusively-held in-flight file (the .part download mid
            # write) denies every reader, and a path can vanish between
            # listing and hashing when a phase completes (part renamed to its
            # final name). Both are evidence of concurrent progress, not
            # failures; settled files never trip this. POSIX allows the
            # concurrent read, which is why only Windows observes it.
            result["unreadable_live_writer"] = str(exc)
    return result


class TestConn:
    def __init__(self, port: int, token: str, timeout: float = 5.0):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout)
        if self.socket.getsockname() == self.socket.getpeername():
            # TCP simultaneous open with itself: nothing listened on the port yet
            # and the OS chose that same port as this socket's source, so every
            # line we send would come straight back as a "reply". Retry it like
            # a refused connection.
            self.socket.close()
            raise ConnectionRefusedError(
                f"net-test socket connected to itself on port {port}")
        self.socket.settimeout(timeout)
        self.buffer = b""
        self.next_id = 1
        self._send({"token": token})

    def _send(self, value: dict) -> None:
        self.socket.sendall((json.dumps(value) + "\n").encode("utf-8"))

    def command(self, name: str, **values: object) -> dict:
        request_id = self.next_id
        self.next_id += 1
        self._send({"id": request_id, "cmd": name, **values})
        while True:
            while b"\n" not in self.buffer:
                chunk = self.socket.recv(65536)
                if not chunk:
                    raise FunnelFailure(
                        f"net-test channel closed awaiting {name}"
                    )
                self.buffer += chunk
            line, self.buffer = self.buffer.split(b"\n", 1)
            if not line.strip():
                continue
            value = json.loads(line)
            if value.get("id") == request_id:
                if not value.get("ok"):
                    raise FunnelFailure(f"net-test command {name} failed: {value}")
                return value

    def close(self) -> None:
        self.socket.close()


# Below the ephemeral range on Windows and macOS (49152-65535) and Linux
# (32768-60999): a port the OS may hand out as a connecting socket's own source
# port invites the self-connect TestConn refuses.
TEST_PORT_RANGE = (20000, 32767)


def _available_port() -> int:
    for _ in range(256):
        port = random.randint(*TEST_PORT_RANGE)
        with socket.socket() as probe:
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                continue
            return port
    raise FunnelFailure("no free loopback port below the ephemeral range")


class ClientProcess:
    def __init__(
        self, executable: Path, profile: Path, platform_name: str,
        log_root: Path, label: str, announcement_url: str | None,
        test_port: int | None = None,
        release_base_url: str | None = None,
        environment_overrides: dict[str, str] | None = None,
        extra_args: Iterable[str] | None = None,
    ):
        _require_empty_directory(profile, f"{label} profile")
        log_root.mkdir(parents=True, exist_ok=True)
        self.stdout_path = log_root / f"{label}.stdout.log"
        self.stderr_path = log_root / f"{label}.stderr.log"
        self.boot_failure_path = log_root / f"{label}.boot-failure.json"
        self.stdout_handle = self.stdout_path.open("wb")
        self.stderr_handle = self.stderr_path.open("wb")
        self.profile = profile
        self.platform_name = platform_name
        install_root = (_win_install_root_from_executable(executable)
                        if platform_name == "Win64" else None)
        if install_root is not None:
            _WIN_INSTALL_ROOTS[profile.resolve()] = install_root
        self.state_root = _state_root(profile, platform_name, install_root)
        self.heartbeat_path, self.heartbeat_offset = (
            _heartbeat_launch_boundary(self.state_root)
        )
        self.test_port = test_port
        self.test_token = f"wot-funnel-{os.getpid()}-{label}"
        self.connection: TestConn | None = None
        command = [
            str(executable),
            "-unattended",
            "-RenderOffscreen",
            "-ForceRes",
            "-ResX=640",
            "-ResY=480",
            "-NoSteam",
            "-nosplash",
            "-nocrashreports",
            "-windowed",
            # Keep the established packaged-Test preference seam. The shared
            # root itself follows CFFIXED_USER_HOME through NSSearchPath.
            f"-wottestpreferencesdir={_preferences_root(profile, platform_name, install_root)}",
        ]
        if test_port is not None:
            command.extend([
                "-wotnettest",
                f"-wotnettestport={test_port}",
                "-wotnettestbind=127.0.0.1",
                f"-wotnettesttoken={self.test_token}",
            ])
        command.extend(extra_args or ())
        environment = _profile_environment(profile, platform_name, install_root)
        if announcement_url:
            environment[TEST_ANNOUNCEMENT_URL_ENV] = announcement_url
        if release_base_url:
            _validate_release_base_url(release_base_url)
            environment[TEST_RELEASE_BASE_URL_ENV] = release_base_url
        environment.update(environment_overrides or {})
        self.command = command
        self.started_epoch = time.time()
        self.started_monotonic = time.monotonic()
        self.process = subprocess.Popen(
            command,
            cwd=str(executable.parent),
            env=environment,
            stdout=self.stdout_handle,
            stderr=self.stderr_handle,
            start_new_session=True,
        )

    def poll_failure(self) -> None:
        def raise_boot_failure_if_present() -> None:
            failures = _boot_failures(self.state_root)
            if not failures:
                return
            receipt = _read_json(failures[0], "boot-failure receipt")
            _atomic_write_json(self.boot_failure_path, receipt)
            raise FunnelFailure(
                "packaged client wrote a boot-failure receipt: "
                f"{receipt.get('diagnostic_code', failures[0])}"
            )

        raise_boot_failure_if_present()
        returncode = self.process.poll()
        if returncode is not None:
            # The fatal path writes and syncs its file receipt before raising,
            # but process-exit observation can still win the directory-view
            # race. Give the receipt a short bounded priority window.
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline:
                raise_boot_failure_if_present()
                time.sleep(0.05)
            raise FunnelFailure(f"packaged client exited early with {returncode}")

    def connect(self, timeout: float) -> TestConn:
        if self.test_port is None:
            raise FunnelFailure("client was not launched with a net-test port")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll_failure()
            try:
                self.connection = TestConn(self.test_port, self.test_token)
                return self.connection
            except OSError:
                time.sleep(0.25)
        raise FunnelFailure("timed out waiting for packaged net-test channel")

    def kill(self, notes_timeout: float = 15.0) -> dict:
        _wait_client_notes_if_ready(self, notes_timeout)
        signals = []
        if self.process.poll() is None:
            signals.append(_instance_tree_kill(self.process))
        returncode = self.process.wait(timeout=10)
        self.close_logs()
        return {"signals": signals, "returncode": returncode}

    def terminate(self, notes_timeout: float = 15.0) -> dict:
        _wait_client_notes_if_ready(self, notes_timeout)
        signals = []
        started = time.monotonic()
        graceful_requested = False
        if self.connection is not None:
            try:
                self.connection.command("quit")
                graceful_requested = True
            except BaseException:
                pass
            self.connection.close()
            self.connection = None
        forced = False
        returncode = None
        if self.process.poll() is None and graceful_requested:
            try:
                returncode = self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                forced = True
        if self.process.poll() is None:
            if os.name == "nt":
                # No graceful signal exists on Windows; the net-channel
                # "quit" above is the graceful path, and this is the
                # escalation for an instance that ignored it.
                signals.append(_instance_tree_kill(self.process))
            else:
                os.killpg(self.process.pid, signal.SIGTERM)
                signals.append("SIGTERM")
        if returncode is None:
            try:
                returncode = self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                forced = True
                signals.append(_instance_tree_kill(self.process))
                returncode = self.process.wait(timeout=10)
        self.close_logs()
        return {
            "signals": signals,
            "returncode": returncode,
            "forced": forced,
            "wait_seconds": round(time.monotonic() - started, 6),
        }

    def close_logs(self) -> None:
        if not self.stdout_handle.closed:
            self.stdout_handle.close()
        if not self.stderr_handle.closed:
            self.stderr_handle.close()


def _phase_observation(client: ClientProcess) -> dict:
    state = client.state_root
    manifests = _manifests(state)
    building = state / "ContentCache" / "building"
    return {
        "building": [building] if building.is_dir() else [],
        "manifests": manifests,
    }


def _wait_for_success(client: ClientProcess, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    first_seen: dict[str, float] = {}
    first_evidence: dict[str, list[dict]] = {}
    order = []
    names = (
        ("BuildingContent", "building"),
        ("VerifyingContent", "manifests"),
    )
    last = {}
    while time.monotonic() < deadline:
        client.poll_failure()
        last = _phase_observation(client)
        for phase, key in names:
            if phase not in first_seen and last[key]:
                first_seen[phase] = min(
                    path.stat().st_mtime for path in last[key]
                )
                first_evidence[key] = [
                    _snapshot_path(path) for path in last[key]
                ]
                order.append(phase)
        if last["manifests"]:
            time.sleep(2.0)
            client.poll_failure()
            break
        time.sleep(0.1)
    else:
        raise FunnelFailure(
            f"fresh funnel did not publish final cache evidence within {timeout:g}s"
        )
    order.append("StartingGame")
    first_seen["StartingGame"] = time.time()
    completed_epoch = time.time()
    ordered_times = [(phase, first_seen[phase]) for phase in order]
    phase_timings = {}
    for index, (phase, timestamp) in enumerate(ordered_times):
        end = (
            ordered_times[index + 1][1]
            if index + 1 < len(ordered_times)
            else completed_epoch
        )
        phase_timings[phase] = round(max(0.0, end - timestamp), 6)
    return {
        "phase_order": order,
        "phase_timings_seconds": phase_timings,
        "phase_first_seen_seconds": {
            phase: round(timestamp - client.started_epoch, 6)
            for phase, timestamp in first_seen.items()
        },
        "manifests": [_snapshot_path(path) for path in last["manifests"]],
        "boot_failure_receipts": [],
        "process_alive_after_activation": client.process.poll() is None,
    }


def _copy_installer_adjacent(installer: Path, tree: Path, platform_name: str) -> Path:
    # BESIDE THE TREE on both platforms. The game derives adjacency from its
    # installed-tree root's parent, never from the executable's own directory
    # - dropping a 1 GB installer inside the tree would invalidate the
    # inventory that attests it. Win64 previously used the executable's
    # directory, which matched the old (defective) resolver that treated
    # WoT/Binaries/Win64 as the tree root; with that fixed, the fixture
    # landed somewhere the client never looks and every adjacent cell
    # silently downloaded instead (2026-08-05, proven by the client writing
    # its own acquired copy beside the tree).
    parent = tree.parent
    destination = parent / INSTALLER_NAME
    if destination.exists():
        raise FunnelFailure(f"adjacent installer already exists: {destination}")
    # NEVER hard-link: the guarded probe fail-closes on LinkCount != 1 (a
    # second name could swap bytes after hashing), so a linked adjacent
    # installer is rejected as BOOTSTRAP-FAILURE at CheckingOriginalGameData.
    # APFS clonefile keeps the copy instant
    # AND single-link; plain copy is the portable fallback.
    cloned = False
    if sys.platform == "darwin":
        cloned = subprocess.run(
            ["cp", "-c", str(installer), str(destination)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode == 0
    if not cloned:
        shutil.copy2(installer, destination)
    if destination.stat().st_nlink != 1:
        raise FunnelFailure(
            f"adjacent installer must be single-link: {destination}"
        )
    _verify_installer(destination)
    return destination


def _copy_installer_file(installer: Path, destination: Path) -> Path:
    """Copy one installer without creating a second hard-link name."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise FunnelFailure(f"installer destination already exists: {destination}")
    cloned = False
    if sys.platform == "darwin":
        cloned = subprocess.run(
            ["cp", "-c", str(installer), str(destination)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode == 0
    if not cloned:
        shutil.copy2(installer, destination)
    if destination.stat().st_nlink != 1:
        raise FunnelFailure(
            f"copied installer must be single-link: {destination}")
    _verify_installer(destination)
    return destination


def _copy_installer_range(source: Path, destination: Path, start: int,
                          count: int) -> None:
    """Append exactly ``count`` bytes beginning at ``start``."""
    if start < 0 or count < 0:
        raise FunnelFailure("installer copy range is invalid")
    mode = "wb" if start == 0 else "ab"
    remaining = count
    with source.open("rb") as reader, destination.open(mode) as writer:
        reader.seek(start)
        while remaining:
            block = reader.read(min(8 * 1024 * 1024, remaining))
            if not block:
                raise FunnelFailure("installer ended before the requested range")
            writer.write(block)
            remaining -= len(block)
        writer.flush()
        os.fsync(writer.fileno())


def _heartbeat_row(records: list[dict], *, event: str, pid: int,
                   detail: str = "") -> dict:
    row = next((item for item in records
                if item.get("event") == event
                and item.get("pid") == pid
                and detail in str(item.get("detail", ""))), None)
    if row is None:
        raise FunnelFailure(
            f"missing {event} heartbeat for pid {pid} containing {detail!r}")
    return dict(row)


def _requests_for_installer(requests: list[dict]) -> list[dict]:
    return [item for item in requests if item.get("path") == "/installer"]


def _client_receipt(client: ClientProcess) -> dict:
    return {
        "pid": client.process.pid,
        "command": client.command,
        "stdout": str(client.stdout_path),
        "stderr": str(client.stderr_path),
        "state_root": str(client.state_root),
    }


def _wait_snapshot(
    client: ClientProcess, predicate: Callable[[dict], bool], timeout: float
) -> dict:
    connection = client.connection or client.connect(timeout)
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        client.poll_failure()
        last = connection.command("snapshot").get("data", {})
        if isinstance(last, dict) and predicate(last):
            _wait_client_notes_if_ready(
                client, max(0.001, deadline - time.monotonic()))
            return last
        time.sleep(0.25)
    raise FunnelFailure(f"snapshot condition timed out; last={last}")


NEUTRAL_SHELL_MAP_NAMES = frozenset(
    f"WoTMapShell{slot:02d}" for slot in range(1, 20))


def _require_hosted_neutral_shell(snapshot: dict) -> None:
    """Prove the guarded host routed to a shell rather than the raw arena.

    The snapshot publishes UWorld::GetMapName(), which UE reduces to the LEAF
    asset name (FPackageName::GetLongPackageAssetName), so a correctly routed
    host reports "WoTMapShell01" and never the "/WoTNeutralShells/..." package
    path. Membership is exact rather than a prefix: "WoTMapShell20" and
    "WoTMapShell01Backup" are not evidence of guarded shell routing.
    """
    hosted_map = snapshot.get("map")
    if (not isinstance(hosted_map, str)
            or hosted_map not in NEUTRAL_SHELL_MAP_NAMES):
        raise FunnelFailure(
            f"hosted session did not route to a neutral shell map: {hosted_map}")


# The guarded build's front end is the cooked WoTBootstrap world. Leave travels
# to FWoTLeaveStateMachine::FrontEndURLForBuild(true) == /Game/WoT/Maps/
# WoTBootstrap (Source/WoT/WoTFrontEnd.cpp), and UWoTGameEngine redirects any
# local browse of the uncooked /Game/WoT/Maps/WoTFrontEnd there as well
# (WoTGuardedBrowseReplacement), so a guarded snapshot can never report
# "WoTFrontEnd". This cell predates that routing: one earlier run
# joined, left, returned to WoTBootstrap in 0.46 s and then waited out the
# 180 s session timeout for a "WoTFrontEnd" leaf that cannot exist.
GUARDED_FRONT_END_MAP = "WoTBootstrap"


def _session_readout_token(snapshot: dict, key: str) -> str | None:
    """One `key=value` token of the snapshot's session gate readout
    (UWoTSessionSubsystem::HostDiagnostics), or None when absent."""
    readout = snapshot.get("session")
    if not isinstance(readout, str):
        return None
    for token in readout.split():
        name, sep, value = token.partition("=")
        if sep and name == key:
            return value
    return None


def _is_returned_front_end(snapshot: dict) -> bool:
    """One completed Leave: the standalone guarded front end, with the named
    OSS session destroyed and no Leave still in flight. The net-test snapshot
    always carries the session readout (UWoTSessionSubsystem::HostDiagnostics),
    so a snapshot without it is not evidence of a completed Leave."""
    if (snapshot.get("map") != GUARDED_FRONT_END_MAP
            or snapshot.get("netmode") != 0):
        return False
    return (_session_readout_token(snapshot, "named") == "0"
            and _session_readout_token(snapshot, "leave") == "0")


def _require_returned_front_end(snapshot: dict) -> None:
    """Require one completed Leave travel, not a hosted/client transition."""
    if not _is_returned_front_end(snapshot):
        raise FunnelFailure(
            "session did not return to front end as standalone: "
            f"map={snapshot.get('map')!r} netmode={snapshot.get('netmode')!r} "
            f"named={_session_readout_token(snapshot, 'named')!r} "
            f"leave={_session_readout_token(snapshot, 'leave')!r}")


def _log_text(client: ClientProcess) -> str:
    for handle in (client.stdout_handle, client.stderr_handle):
        if not handle.closed:
            handle.flush()
    result = ""
    for path in (client.stdout_path, client.stderr_path):
        try:
            result += path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            pass
    return result


def _wait_log(client: ClientProcess, pattern: str, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        client.poll_failure()
        text = _log_text(client)
        if re.search(pattern, text):
            return text
        time.sleep(0.25)
    raise FunnelFailure(f"packaged log did not match {pattern!r}")


def _directory_size(path: Path) -> int:
    total = 0
    for entry in path.rglob("*"):
        try:
            if entry.is_file() and not entry.is_symlink():
                total += entry.stat().st_size
        except OSError as exc:
            raise FunnelFailure(f"cannot size update source {entry}: {exc}") from exc
    return total


def _require_update_copy_space(base_tree: Path, successful_profile: Path,
                               scratch: Path) -> None:
    tree_size = _directory_size(_release_clone_source(base_tree))
    profile_size = _directory_size(successful_profile)
    required = 2 * (tree_size + profile_size)
    free = shutil.disk_usage(scratch).free
    if free < required:
        raise FunnelFailure(
            "insufficient scratch free space for update attempt: "
            f"free={free} bytes, required={required} bytes "
            f"(2 x (profile={profile_size} bytes + release={tree_size} bytes))")


def _remove_attempt_tree(path: Path) -> None:
    """rmtree that also clears a read-only attribute."""
    def retry_writable(function, name, _info) -> None:
        os.chmod(name, 0o700)
        function(name)

    shutil.rmtree(path, onerror=retry_writable)


def _cleanup_update_attempt(tree: Path, profile: Path, name: str) -> dict:
    """Remove one passed attempt after terminating processes from its roots."""
    _cleanup_processes_under(tree.parent)
    _cleanup_processes_under(profile)
    _remove_attempt_tree(tree.parent)
    _remove_attempt_tree(profile)
    if tree.parent.exists() or profile.exists():
        raise FunnelFailure(f"update attempt cleanup did not delete {name}")
    return {"attempt": name, "deleted": True}


# What Explorer's Extract All copies onto every file of a zip a browser
# downloaded. Real Win64 installs carry it, so the update cells run on such a
# tree too (install parity). It is stamped before any launch, so it is part of
# every baseline: churn DURING verification is proven by the activation specs'
# seam (WoT.UpdaterCore.Activation.MetadataOnlyChurnIsReverified), not here.
MOTW_ZONE_IDENTIFIER = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://github.com/\r\n"


def _stamp_mark_of_the_web(tree: Path) -> int:
    """Write Zone.Identifier on every file under tree (Win64/NTFS only).

    No SmartScreen prompt can follow: the funnel and the launcher start
    processes with CreateProcess, never ShellExecute.
    """
    count = 0
    for path in sorted(tree.rglob("*")):
        if path.is_file() and not path.is_symlink():
            with open(str(path) + ":Zone.Identifier", "w", encoding="ascii",
                      newline="") as stream:
                stream.write(MOTW_ZONE_IDENTIFIER)
            count += 1
    return count


def _prepare_update_cell(base_tree: Path, successful_profile: Path,
                         scratch: Path, installer: Path, name: str,
                         platform_name: str = "Win64"
                         ) -> tuple[Path, Path, Path]:
    _require_update_copy_space(base_tree, successful_profile, scratch)
    tree = _clone_release_tree(
        base_tree, _cell_tree_destination(base_tree, scratch, name))
    _copy_installer_adjacent(installer, tree, platform_name)
    if platform_name == "Win64":
        _stamp_mark_of_the_web(tree)
    profile = scratch / "profiles" / name
    shutil.copytree(successful_profile, profile, symlinks=True)
    return tree, profile, tree.parent / ".wot-update"


def _record_staging_retries(log: Path, retries: list[dict]) -> None:
    log.mkdir(parents=True, exist_ok=True)
    (log / "staging-retries.json").write_text(
        json.dumps(retries, indent=2, sort_keys=True), encoding="utf-8")


def _stage_update_candidate(*, tree: Path, profile: Path, update_dir: Path,
                            server: LoopbackInstallerServer, log: Path,
                            clients: list[ClientProcess], timeout: float,
                            sequence: int,
                            process_observer,
                            platform_name: str = "Win64") -> dict:
    live_before = _installed_tree_root(tree, platform_name)
    retries = []
    for attempt in range(1, STAGING_ATTEMPTS + 1):
        client = _launch_reused_profile(
            _resolve_executable(tree, platform_name), profile, platform_name, log,
            "A" if attempt == 1 else f"A-retry-{attempt - 1}",
            server.announcement_url, test_port=_available_port(),
            release_base_url=server.release_base_url)
        process_observer.adopt(client.process)
        clients.append(client)
        client.connect(timeout)
        _wait_snapshot(client, lambda value: bool(value.get("map")), timeout)
        pid = client.process.pid
        records = _wait_heartbeat(
            client.heartbeat_path, client.heartbeat_offset,
            lambda values: any(
                record.get("pid") == pid
                and record.get("event") == "update-check-result"
                for record in values),
            timeout, "background update staging")
        result = next(str(record.get("detail", "")).split(" ", 1)[0]
                      for record in records
                      if record.get("pid") == pid
                      and record.get("event") == "update-check-result")
        staged_now = any(
            record.get("pid") == pid
            and record.get("event") == "update-staged"
            and f"sequence={sequence + 1}" in str(record.get("detail", ""))
            for record in records)
        if staged_now:
            break
        failure = [str(record.get("detail", "")) for record in records
                   if record.get("pid") == pid
                   and record.get("event") == "update-check-failed"]
        retries.append({"attempt": attempt, "pid": pid, "result": result,
                        "failure": failure})
        client.terminate(timeout)
        clients.remove(client)
        # Staging is this cell's setup, not its claim. An intermittent
        # staging failure on an intact archive is retried with
        # a fresh process, exactly as the next start would; anything else
        # fails at once.
        if result not in {"download-failed", "failed"}:
            raise FunnelFailure(
                f"background update check ended {result!r} without staging")
    else:
        _record_staging_retries(log, retries)
        raise FunnelFailure(
            f"background staging failed {STAGING_ATTEMPTS} times: {retries!r}")
    if retries:
        _record_staging_retries(log, retries)
    ready = _read_shared_text(update_dir / "ready", "ascii")
    if ready != f"{sequence + 1}\n":
        raise FunnelFailure("update-staged heartbeat has no matching ready marker")
    process = _process_evidence(records, "A", client.process.pid, ready=True)
    termination = client.terminate(timeout)
    clients.remove(client)
    if termination["forced"] or termination["returncode"] != 0:
        raise FunnelFailure("staging game did not quit normally")
    if _installed_tree_root(tree, platform_name) != live_before or (update_dir / "trial").exists():
        raise FunnelFailure("LIVE changed while the staging process quit")
    return {"live_before": live_before, "process": process, "ready_marker": ready}


def _stage_mac_update_candidate(*, tree: Path, profile: Path,
                                update_dir: Path,
                                server: LoopbackInstallerServer, log: Path,
                                clients: list[ClientProcess], timeout: float,
                                sequence: int, process_observer) -> dict:
    """Stage on Mac and leave the live client connected for the quit phase."""
    live_before = _installed_tree_root(tree, "Mac")
    retries = []
    for attempt in range(1, STAGING_ATTEMPTS + 1):
        client = _launch_reused_profile(
            _resolve_executable(tree, "Mac"), profile, "Mac", log,
            "A" if attempt == 1 else f"A-retry-{attempt - 1}",
            server.announcement_url, test_port=_available_port(),
            release_base_url=server.release_base_url)
        process_observer.adopt(client.process)
        clients.append(client)
        client.connect(timeout)
        _wait_snapshot(client, lambda value: bool(value.get("map")), timeout)
        pid = client.process.pid
        records = _wait_heartbeat(
            client.heartbeat_path, client.heartbeat_offset,
            lambda values: any(
                record.get("pid") == pid
                and record.get("event") == "update-check-result"
                for record in values),
            timeout, "background update staging")
        result = next(str(record.get("detail", "")).split(" ", 1)[0]
                      for record in records
                      if record.get("pid") == pid
                      and record.get("event") == "update-check-result")
        staged_now = any(
            record.get("pid") == pid
            and record.get("event") == "update-staged"
            and f"sequence={sequence + 1}" in str(record.get("detail", ""))
            for record in records)
        if staged_now:
            break
        failure = [str(record.get("detail", "")) for record in records
                   if record.get("pid") == pid
                   and record.get("event") == "update-check-failed"]
        retries.append({"attempt": attempt, "pid": pid, "result": result,
                        "failure": failure})
        termination = client.terminate(timeout)
        clients.remove(client)
        if termination["forced"] or termination["returncode"] != 0:
            raise FunnelFailure("failed Mac staging attempt did not quit normally")
        if result not in {"download-failed", "failed"}:
            raise FunnelFailure(
                f"background update check ended {result!r} without staging")
    else:
        _record_staging_retries(log, retries)
        raise FunnelFailure(
            f"background staging failed {STAGING_ATTEMPTS} times: {retries!r}")
    if retries:
        _record_staging_retries(log, retries)
    ready = _read_shared_text(update_dir / "ready", "ascii")
    if ready != f"{sequence + 1}\n":
        raise FunnelFailure("update-staged heartbeat has no matching ready marker")
    if (_installed_tree_root(tree, "Mac") != live_before
            or (update_dir / "trial").exists()):
        raise FunnelFailure("Mac LIVE changed before the graceful quit")
    return {
        "client": client,
        "live_before": live_before,
        "process": _process_evidence(records, "A", pid, ready=True),
        "ready_marker": ready,
    }


def _wait_frontend_launch(client: ClientProcess, sequence: int, timeout: float) -> tuple[int, list[dict]]:
    def completed_pids(rows):
        identities = {row["pid"] for row in rows if _heartbeat_sequence(row) == sequence}
        return {row["pid"] for row in rows
                if row.get("event") == "frontend-handoff" and row.get("pid") in identities}

    records = _wait_heartbeat(client.heartbeat_path, client.heartbeat_offset,
        lambda rows: bool(completed_pids(rows)),
        timeout, "explicit launcher start frontend-handoff")
    pids = completed_pids(records)
    if len(pids) != 1:
        raise FunnelFailure("explicit launcher start did not identify one game process")
    pid = pids.pop()
    client.connect(timeout)
    return pid, records


def _run_launcher_update_cell(name: str, *, fixture_receipt: Path, evidence_receipt: Path,
        receipt_path: Path, base_tree: Path, successful_profile: Path, scratch: Path,
        installer: Path, log: Path, clients: list, observer, timeout: float) -> dict:
    label = "bad" if name == "update-bad-build-rolls-back" else "good"
    fixture, assets = _fixture_release_assets(fixture_receipt, label, "Win64")
    sequence = fixture["base"]["release_sequence"]
    variant = fixture["variants"][label]
    tree, profile, update_dir = _prepare_update_cell(base_tree, successful_profile,
        scratch, installer, name, "Win64")
    processes = []
    with LoopbackInstallerServer(installer, log / "loopback-requests.jsonl",
            release_assets=assets, release_note_sequences=(sequence, sequence + 1)) as server:
        def launch(role):
            client = _launch_reused_profile(_resolve_executable(tree, "Win64"), profile,
                "Win64", log, role, server.announcement_url, test_port=_available_port(),
                release_base_url=server.release_base_url, through_launcher=True)
            observer.adopt(client.process)
            clients.append(client)
            return client

        def quit_client(client):
            result = client.terminate(timeout)
            clients.remove(client)
            if result["forced"] or result["returncode"] != 0:
                raise FunnelFailure("updated game did not quit normally")

        result = {}
        if name == "interrupted-swap":
            live_before = _installed_tree_root(tree, "Win64")
            data = tree.parent / "Data"
            data.mkdir(exist_ok=True)
            sentinel = data / "funnel-player-data.txt"
            sentinel.write_bytes(b"player data survives missing-Game recovery\n")
            sentinel_digest = _sha256_file(sentinel)
            previous = update_dir / "prev" / "Game"
            previous.parent.mkdir(parents=True, exist_ok=True)
            tree.rename(previous)
            (update_dir / "trial").write_bytes(f"{sequence + 1} started\n".encode("ascii"))
            # Resolve the game path from the previous tree before recovering missing LIVE.
            game_relative = _resolve_executable(previous, "Win64").relative_to(previous)
            expected_game = tree / game_relative
            client = _launch_reused_profile(expected_game, profile, "Win64", log, "B",
                server.announcement_url, test_port=_available_port(),
                release_base_url=server.release_base_url, through_launcher=True)
            observer.adopt(client.process); clients.append(client)
            pid, records = _wait_frontend_launch(client, sequence, timeout)
            processes.append(_process_evidence(records, "B", pid, ready=True))
            notice = _find_update_failed_notice(profile, "Win64", sequence + 1, sequence)
            result.update(started_marker=f"{sequence + 1} started\n", missing_live_before=True,
                player_data_unchanged=_sha256_file(sentinel) == sentinel_digest)
            quit_client(client)
            # The same interrupted rename without any trial is also recoverable.
            previous.parent.mkdir(parents=True, exist_ok=True)
            tree.rename(previous)
            second = _launch_reused_profile(expected_game, profile, "Win64", log, "C",
                server.announcement_url, test_port=_available_port(),
                release_base_url=server.release_base_url, through_launcher=True)
            observer.adopt(second.process); clients.append(second)
            pid, records = _wait_frontend_launch(second, sequence, timeout)
            processes.append(_process_evidence(records, "C", pid, ready=True))
            result["recovery_without_trial"] = tree.is_dir() and not previous.exists()
            result["player_data_unchanged"] &= _sha256_file(sentinel) == sentinel_digest
            quit_client(second)
        else:
            stage = _stage_update_candidate(tree=tree, profile=profile, update_dir=update_dir,
                server=server, log=log, clients=clients, timeout=timeout, sequence=sequence,
                process_observer=observer)
            live_before = stage["live_before"]
            processes.append(stage["process"])
            result["ready_marker"] = stage["ready_marker"]
            archive_route = next(route for route, path in assets.items() if path.name == variant["archive"]["name"])
            archive_count = lambda: sum(row["path"] == archive_route for row in server.requests())
            after_stage = archive_count()
            launch_mark = observer.mark()
            client = launch("B")
            if name == "update-bad-build-rolls-back":
                bad_game = observer.wait_for_start(mark=launch_mark,
                    parent_pid=client.process.pid, timeout=timeout)
                launcher_status = client.process.wait(timeout=timeout)
                stopped, _events = observer.wait_for_pid_stop(mark=launch_mark,
                    pid=bad_game["pid"], timeout=timeout)
                result["bad_exit_status"] = stopped["exit_status"]
                if result["bad_exit_status"] != launcher_status:
                    raise FunnelFailure("launcher exit does not match the observed bad game")
                client.close_logs(); clients.remove(client)
                result["started_marker"] = _read_shared_text(update_dir / "trial", "ascii")
                if result["started_marker"] != f"{sequence + 1} started\n" or result["bad_exit_status"] == 0:
                    raise FunnelFailure("bad fixture did not fail after writing started")
                client = launch("C")
                pid, records = _wait_frontend_launch(client, sequence, timeout)
                records = _wait_heartbeat(client.heartbeat_path, client.heartbeat_offset,
                    lambda rows: any(_matches_update_result(row, pid=pid, status="KnownBad",
                        reason="failed-release", sequence=sequence + 1) for row in rows),
                    timeout, "failed-sequence suppression")
                processes.append(_process_evidence(records, "C", pid, ready=True))
                result["recheck"] = _update_result_observation(next(row for row in reversed(records)
                    if _matches_update_result(row, pid=pid, status="KnownBad", reason="failed-release", sequence=sequence + 1)))
                notice = _find_update_failed_notice(profile, "Win64", sequence + 1, sequence)
                server.server.wait_for_idle(5.0)
                result.update(archive_request_count_after_staging=after_stage,
                    archive_request_count_after_normal_recheck=archive_count())
            else:
                pid, records = _wait_frontend_launch(client, sequence + 1, timeout)
                processes.append(_process_evidence(records, "B", pid, ready=True))
                result["explicit_launch"] = True
            quit_client(client)
        if name != "update-apply":
            result["failure_notice"] = {"path": str(notice), "sha256": _sha256_file(notice),
                "state": "pending", "failed_sequence": sequence + 1, "restored_sequence": sequence}
        result.update(base=_base_update_evidence(fixture),
            derivation_receipt=_receipt_file_ref(evidence_receipt, relative_to=receipt_path.parent),
            processes=processes, request_transcript=server.requests(),
            trees={"live_before": live_before, "live_after": _installed_tree_root(tree, "Win64"),
                   "previous_after": None if not (update_dir / "prev" / "Game").exists() else _installed_tree_root(update_dir / "prev" / "Game", "Win64")},
            variant_tree_root=variant["tree_root"], trial_absent=not (update_dir / "trial").exists(),
            previous_absent=not (update_dir / "prev" / "Game").exists(), frontend_handoff=True,
            cleanup=[{"attempt": name, "deleted": True}])
        formats.validate_update_cell_evidence(name, result)
    result["cleanup"] = [_cleanup_update_attempt(tree, profile, name)]
    return result


def _run_mac_update_cell(name: str, *, fixture_receipt: Path,
        evidence_receipt: Path, receipt_path: Path, base_tree: Path,
        successful_profile: Path, scratch: Path, installer: Path, log: Path,
        clients: list, observer, timeout: float) -> dict:
    label = "bad" if name == "update-bad-build-rolls-back" else "good"
    fixture, assets = _fixture_release_assets(fixture_receipt, label, "Mac")
    sequence = fixture["base"]["release_sequence"]
    variant = fixture["variants"][label]
    tree, profile, update_dir = _prepare_update_cell(
        base_tree, successful_profile, scratch, installer, name, "Mac")
    previous = update_dir / "prev" / tree.name
    staged_tree = update_dir / "staged" / tree.name
    processes = []

    with LoopbackInstallerServer(
            installer, log / "loopback-requests.jsonl",
            release_assets=assets,
            release_note_sequences=(sequence, sequence + 1)) as server:
        def launch(role, *, may_exit=False):
            client = _launch_reused_profile(
                _resolve_executable(tree, "Mac"), profile, "Mac", log, role,
                server.announcement_url, test_port=_available_port(),
                release_base_url=server.release_base_url)
            observer.adopt(client.process, may_exit=may_exit)
            clients.append(client)
            return client

        def quit_client(client):
            result = client.terminate(timeout)
            clients.remove(client)
            if result["forced"] or result["returncode"] != 0:
                raise FunnelFailure("updated Mac game did not quit normally")

        def wait_for_exit(client, description):
            try:
                status = client.process.wait(timeout=timeout)
            except subprocess.TimeoutExpired as exc:
                raise FunnelFailure(
                    f"{description} did not exit within the cell timeout") from exc
            client.close_logs()
            clients.remove(client)
            return status

        stage = _stage_mac_update_candidate(
            tree=tree, profile=profile, update_dir=update_dir, server=server,
            log=log, clients=clients, timeout=timeout, sequence=sequence,
            process_observer=observer)
        live_before = stage["live_before"]
        processes.append(stage["process"])
        result = {"ready_marker": stage["ready_marker"]}
        archive_route = next(
            route for route, path in assets.items()
            if path.name == variant["archive"]["name"])
        archive_count = lambda: sum(
            row["path"] == archive_route for row in server.requests())
        after_stage = archive_count()

        if name == "interrupted-swap":
            killed = stage["client"].kill(timeout)
            clients.remove(stage["client"])
            if killed["returncode"] == 0:
                raise FunnelFailure("interrupted Mac staging launch exited normally")
            temporary = update_dir / "interrupted-live"
            if temporary.exists():
                raise FunnelFailure("interrupted-swap temporary path already exists")
            tree.rename(temporary)
            staged_tree.rename(tree)
            temporary.rename(staged_tree)
            interrupted_layout = (
                _installed_tree_root(tree, "Mac") == variant["tree_root"]
                and _installed_tree_root(staged_tree, "Mac") == live_before
                and _read_shared_text(update_dir / "ready", "ascii")
                == f"{sequence + 1}\n"
                and not (update_dir / "trial").exists())
            client = launch("B")
            pid, records = _wait_frontend_launch(
                client, sequence + 1, timeout)
            processes.append(_process_evidence(records, "B", pid, ready=True))
            quit_client(client)
            result.update(
                interrupted_layout=interrupted_layout,
                staged_absent=not staged_tree.exists())
        else:
            quit_client(stage["client"])
            pending = _read_shared_text(update_dir / "trial", "ascii")
            if (pending != f"{sequence + 1} pending\n"
                    or (update_dir / "ready").exists()
                    or _installed_tree_root(tree, "Mac")
                    != variant["tree_root"]
                    or _installed_tree_root(previous, "Mac") != live_before):
                raise FunnelFailure(
                    "Mac graceful quit did not publish live/prev/trial")

            if name == "update-apply":
                client = launch("B")
                pid, records = _wait_frontend_launch(
                    client, sequence + 1, timeout)
                processes.append(
                    _process_evidence(records, "B", pid, ready=True))
                result["explicit_launch"] = True
                quit_client(client)
            else:
                bad = launch("B", may_exit=True)
                result["bad_exit_status"] = wait_for_exit(
                    bad, "bad Mac update launch")
                result["started_marker"] = _read_shared_text(
                    update_dir / "trial", "ascii")
                if (result["started_marker"]
                        != f"{sequence + 1} started\n"
                        or result["bad_exit_status"] == 0):
                    raise FunnelFailure(
                        "bad Mac fixture did not fail after writing started")

                rollback = launch("C", may_exit=True)
                result["rollback_exit_status"] = wait_for_exit(
                    rollback, "Mac rollback launch")
                rolled_back = update_dir / "rolled-back"
                result["rolled_back_marker"] = _read_shared_text(
                    rolled_back, "ascii")
                if (result["rollback_exit_status"] != 1
                        or result["rolled_back_marker"]
                        != f"{sequence + 1}\n"
                        or (update_dir / "trial").exists()
                        or _installed_tree_root(tree, "Mac") != live_before):
                    raise FunnelFailure(
                        "Mac rollback launch did not restore and record the base")

                client = launch("D")
                pid, records = _wait_frontend_launch(client, sequence, timeout)
                records = _wait_heartbeat(
                    client.heartbeat_path, client.heartbeat_offset,
                    lambda rows: any(_matches_update_result(
                        row, pid=pid, status="KnownBad",
                        reason="failed-release", sequence=sequence + 1)
                        for row in rows),
                    timeout, "failed-sequence suppression")
                processes.append(
                    _process_evidence(records, "D", pid, ready=True))
                result["recheck"] = _update_result_observation(next(
                    row for row in reversed(records)
                    if _matches_update_result(
                        row, pid=pid, status="KnownBad",
                        reason="failed-release", sequence=sequence + 1)))
                notice = _find_update_failed_notice(
                    profile, "Mac", sequence + 1, sequence)
                server.server.wait_for_idle(5.0)
                result.update(
                    archive_request_count_after_staging=after_stage,
                    archive_request_count_after_normal_recheck=archive_count(),
                    failure_notice={
                        "path": str(notice), "sha256": _sha256_file(notice),
                        "state": "pending", "failed_sequence": sequence + 1,
                        "restored_sequence": sequence,
                    })
                quit_client(client)

        result.update(
            base=_base_update_evidence(fixture),
            derivation_receipt=_receipt_file_ref(
                evidence_receipt, relative_to=receipt_path.parent),
            processes=processes,
            request_transcript=server.requests(),
            trees={
                "live_before": live_before,
                "live_after": _installed_tree_root(tree, "Mac"),
                "previous_after": (None if not previous.exists()
                                   else _installed_tree_root(previous, "Mac")),
            },
            variant_tree_root=variant["tree_root"],
            trial_absent=not (update_dir / "trial").exists(),
            previous_absent=not previous.exists(),
            frontend_handoff=True,
            cleanup=[{"attempt": name, "deleted": True}],
        )
        formats.validate_update_cell_evidence(name, result, "Mac")
    result["cleanup"] = [_cleanup_update_attempt(tree, profile, name)]
    return result


def _run_acceptance(args: argparse.Namespace) -> dict:
    manifest, asset = _resolve_funnel_release(args)
    platform_name = manifest["platform"]
    selected = _parse_cells(args.cells, platform_name)
    if args.platform and args.platform != platform_name:
        raise FunnelFailure(
            f"--platform {args.platform} disagrees with release {platform_name}"
        )
    evidence = (
        args.evidence.expanduser().resolve()
        if args.evidence
        else DEFAULT_EVIDENCE_ROOT / platform_name
    )
    evidence.mkdir(parents=True, exist_ok=True)
    receipt_path = evidence / "funnel-receipt.json"
    receipt = _new_receipt(platform_name)
    receipt_started = time.monotonic()
    release_stapled = (manifest.get("products", {}).get("receipt", {})
                       .get("stapled"))
    receipt["release"] = {
        "manifest_path": str(args.release.expanduser().resolve()),
        "manifest_sha256": _sha256_file(args.release.expanduser().resolve()),
        "asset_path": str(asset),
        "asset_size": asset.stat().st_size,
        "asset_sha256": _sha256_file(asset),
        "git_commit": manifest["git_commit"],
        "release_sequence": manifest["release_sequence"],
        "source_snapshot_sha256": manifest["source_snapshot"]["sha256"],
        "development_allow_source_drift": bool(getattr(
            args, "development_allow_source_drift", False)),
        "stapled": (release_stapled if platform_name == "Mac" else None),
    }
    scratch, remove_scratch = _make_scratch_root(args.work_root)
    installer: Path | None = None
    clients: list[ClientProcess] = []
    successful_profile: Path | None = None
    successful_tree: Path | None = None
    base_tree: Path | None = None
    update_fixture_receipt: Path | None = None
    evidence_derivation_receipt: Path | None = None
    process_observer: _WinJobProcessObserver | None = None

    def save() -> None:
        receipt["completed_utc"] = _utc_now()
        receipt["duration_seconds"] = round(
            time.monotonic() - receipt_started, 6
        )
        _validate_receipt_schema(receipt)
        _atomic_write_json(receipt_path, receipt)

    # Whole-run tripwire on the developer's REAL data roots: taken before the
    # first launch and compared only after every run-owned process has been
    # cleaned up, so no cell (guarded or not) can write there unnoticed.
    real_profile_guard_path = (
        evidence / "safety" / "whole-run-real-preferences.json")
    real_profile_before = (
        _mac_real_support_inventory()
        if platform_name == "Mac" and selected else None)
    if real_profile_before is not None:
        _atomic_write_json(real_profile_guard_path, {
            "after": None, "before": real_profile_before,
            "format": "wot-real-mac-preferences-guard-v1",
            "scope": "whole-run", "unchanged": None,
        })
    # Windows Firewall rules before the first launch: a reused work root
    # keeps the rules earlier runs left, so only rules that are new by the end
    # of this run count against it.
    check_firewall = platform_name == "Win64" and bool(selected) and _IS_WINDOWS
    firewall_before = _snapshot_firewall_rules() if check_firewall else None

    try:
        if firewall_before is not None and firewall_before["error"] is not None:
            raise FunnelFailure(
                "SAFETY: cannot read the Windows Firewall rules before the "
                f"first cell: {firewall_before['error']}")
        if args.installer:
            installer = args.installer.expanduser().resolve()
            receipt["installer"] = _verify_installer(installer)
            receipt["installer"]["acquisition"] = "provided"
        elif args.cache.is_file():
            installer = args.cache.expanduser().resolve()
            receipt["installer"] = _verify_installer(installer)
            receipt["installer"]["acquisition"] = "existing-cache"
        if installer is None and selected:
            raise FunnelFailure(
                "selected cells require a pin-valid player-supplied installer; "
                "pass --installer or populate --cache"
            )

        if selected:
            base_tree = _extract_release(
                asset, scratch / "base-release", platform_name
            )
            base_executable = _resolve_executable(base_tree, platform_name)
            _require_runtime_endpoint_seam(base_executable)

        if "fresh-first-run" in selected:
            def fresh(cell: dict) -> None:
                nonlocal successful_profile, successful_tree
                tree = _clone_release_tree(
                    base_tree,
                    _cell_tree_destination(base_tree, scratch, "fresh"),
                )
                adjacent = _copy_installer_adjacent(
                    installer, tree, platform_name
                )
                executable = _resolve_executable(tree, platform_name)
                profile = scratch / "profiles" / "fresh"
                log = evidence / "cells" / "fresh-first-run"
                with LoopbackInstallerServer(
                    installer, log / "loopback-requests.jsonl",
                    args.loopback_bytes_per_second,
                    release_note_sequences=(manifest["release_sequence"],),
                ) as server:
                    client = ClientProcess(
                        executable, profile, platform_name, log, "client",
                        server.announcement_url,
                        release_base_url=server.release_base_url,
                    )
                    clients.append(client)
                    result = _wait_for_success(client, args.cell_timeout)
                    heartbeat_records = _wait_heartbeat(
                        client.heartbeat_path, client.heartbeat_offset,
                        lambda values: front_end_evidence(values, client.process.pid) is not None,
                        args.cell_timeout, "fresh first-run frontend handoff")
                    detected = _heartbeat_row(
                        heartbeat_records, event="original-game-detected",
                        pid=client.process.pid, detail="source=adjacent")
                    notes, _records = _wait_release_notes_result(
                        client.heartbeat_path, client.heartbeat_offset,
                        client.process.pid, args.cell_timeout)
                    requests = server.requests()
                    notes_count = _notes_request_count(requests)
                    expected_outcome = ("not-attempted"
                                        if manifest["release_sequence"] == 0
                                        else "selected")
                    expected_count = 0 if manifest["release_sequence"] == 0 else 1
                    if (notes["outcome"] != expected_outcome
                            or notes["sequence"] != manifest["release_sequence"]
                            or notes["commit"] != manifest["git_commit"]
                            or notes_count != expected_count):
                        raise FunnelFailure(
                            "fresh first run release-notes evidence is invalid")
                    installer_requests = _requests_for_installer(requests)
                    if installer_requests:
                        raise FunnelFailure(
                            "fresh first run requested the original-game installer"
                        )
                    result["adjacent_installer"] = _receipt_file_ref(adjacent)
                    result["installer_requests"] = installer_requests
                    result["loopback_requests"] = requests
                    result["notes_outcome"] = notes["outcome"]
                    result["notes_request_count"] = notes_count
                    result["notes_result"] = notes
                    result["process"] = _client_receipt(client)
                    result["detected_heartbeat"] = detected
                    result["frontend_handoff"] = front_end_evidence(
                        heartbeat_records, client.process.pid)["frontend_handoff"]
                    result["termination"] = client.terminate(args.cell_timeout)
                    clients.remove(client)
                    if platform_name == "Mac":
                        result["mac_profile_library"] = (
                            _require_mac_profile_library_contained(profile))
                    else:
                        result["win_profile_appdata"] = (
                            _require_win_profile_has_no_game_data(profile))
                cell["phase_order"] = result.pop("phase_order")
                cell["phase_timings_seconds"] = result.pop(
                    "phase_timings_seconds"
                )
                cell["evidence"] = result
                cell["decisions"] = [
                    "the player-supplied adjacent installer was present before launch and no installer network request occurred",
                    "activation requires the published current manifest, no boot-failure receipt, and a still-live process",
                ]
                successful_profile = profile
                successful_tree = tree
            _run_cell(receipt, "fresh-first-run", fresh)
            save()


        if "downloads-pickup" in selected:
            def downloads_pickup(cell: dict) -> None:
                attempts = []
                models = (["partial-final"] if platform_name == "Mac" else
                          ["chrome-edge", "firefox"])
                for model in models:
                    tree = _clone_release_tree(
                        base_tree, _cell_tree_destination(
                            base_tree, scratch, f"downloads-pickup-{model}"))
                    executable = _resolve_executable(tree, platform_name)
                    profile = scratch / "profiles" / f"downloads-pickup-{model}"
                    log = evidence / "cells" / "downloads-pickup" / model
                    with LoopbackInstallerServer(
                        installer, log / "loopback-requests.jsonl",
                        release_note_sequences=(manifest["release_sequence"],),
                    ) as server, _make_update_process_observer(
                        platform_name, log / "process-observer.jsonl",
                    ) as process_observer:
                        client = ClientProcess(
                            executable, profile, platform_name, log, "client",
                            server.announcement_url,
                            test_port=_available_port(),
                            release_base_url=server.release_base_url)
                        clients.append(client)
                        process_observer.adopt(client.process)
                        termination = None
                        try:
                            before_identity = process_observer.observe_identity(
                                client.process.pid)
                            records = _wait_heartbeat(
                                client.heartbeat_path, client.heartbeat_offset,
                                lambda values: any(
                                    row.get("event") == "original-game-needed"
                                    and row.get("phase") == "OriginalGameNeeded"
                                    and row.get("pid") == client.process.pid
                                    and "reason=missing" in
                                        str(row.get("detail", ""))
                                    for row in values),
                                args.cell_timeout,
                                f"downloads-pickup {model} missing heartbeat")
                            entry = _heartbeat_row(
                                records, event="original-game-needed",
                                pid=client.process.pid, detail="reason=missing")
                            entry["monotonic_seconds"] = time.monotonic()
                            downloads = profile / "FunnelDownloads"
                            destination = downloads / INSTALLER_NAME
                            waiting = None
                            if model == "partial-final":
                                first_half = INSTALLER_SIZE // 2
                                _copy_installer_range(
                                    installer, destination, 0, first_half)
                                # The game logs original-game-needed only on
                                # entering the phase; the Downloading state is
                                # a UI progress event. Prove instead that a
                                # half-written file is never accepted or fatal
                                # across several probe intervals.
                                hold_started = time.monotonic()
                                try:
                                    early = _wait_heartbeat(
                                        client.heartbeat_path,
                                        client.heartbeat_offset,
                                        lambda values: any(
                                            row.get("pid") ==
                                                client.process.pid
                                            and row.get("event") in (
                                                "original-game-detected",
                                                "fail")
                                            for row in values),
                                        PARTIAL_HOLD_SECONDS,
                                        "downloads-pickup partial hold")
                                except FunnelFailure:
                                    early = None
                                if early is not None:
                                    raise FunnelFailure(
                                        "downloads-pickup accepted or failed "
                                        "on a half-written installer")
                                if client.process.poll() is not None:
                                    raise FunnelFailure(
                                        "downloads-pickup game exited during "
                                        "the partial hold")
                                waiting = {
                                    "event": "partial-hold",
                                    "pid": client.process.pid,
                                    "hold_seconds": round(
                                        time.monotonic() - hold_started, 3),
                                    "monotonic_seconds": time.monotonic(),
                                }
                                _copy_installer_range(
                                    installer, destination, first_half,
                                    INSTALLER_SIZE - first_half)
                                _verify_installer(destination)
                            elif model == "chrome-edge":
                                temporary = downloads / (INSTALLER_NAME
                                                          + ".crdownload")
                                _copy_installer_file(installer, temporary)
                                os.replace(temporary, destination)
                            else:
                                destination.touch()
                                temporary = downloads / (INSTALLER_NAME
                                                          + ".part")
                                _copy_installer_file(installer, temporary)
                                os.replace(temporary, destination)
                            completed_monotonic = time.monotonic()
                            detected_records = _wait_heartbeat(
                                client.heartbeat_path, client.heartbeat_offset,
                                lambda values: any(
                                    row.get("event") ==
                                        "original-game-detected"
                                    and row.get("phase") ==
                                        "OriginalGameNeeded"
                                    and row.get("pid") == client.process.pid
                                    and "source=downloads" in
                                        str(row.get("detail", ""))
                                    for row in values),
                                args.cell_timeout,
                                f"downloads-pickup {model} detection")
                            detected = _heartbeat_row(
                                detected_records,
                                event="original-game-detected",
                                pid=client.process.pid,
                                detail="source=downloads")
                            detected["monotonic_seconds"] = time.monotonic()
                            result = _wait_for_success(
                                client, args.cell_timeout)
                            result["front_end_snapshot"] = _wait_snapshot(
                                client, _is_returned_front_end,
                                args.cell_timeout)
                            result["process"] = _client_receipt(client)
                            after_identity = process_observer.observe_identity(
                                client.process.pid)
                            if any(before_identity[key] != after_identity[key]
                                   for key in ("pid", "process_start_id")):
                                raise FunnelFailure(
                                    f"downloads-pickup {model} changed process identity")
                            if platform_name == "Mac":
                                termination = _terminate_mac_pid(
                                    client.process.pid,
                                    observer=process_observer,
                                    allowed_roots=[tree],
                                    token=client.test_token,
                                    profile=_preferences_root(profile, platform_name),
                                    direct_process=client.process)
                                client.close_logs()
                            else:
                                termination = client.terminate(
                                    args.cell_timeout)
                            clients.remove(client)
                        finally:
                            if platform_name == "Mac" and client in clients:
                                _terminate_mac_pid(
                                    client.process.pid,
                                    observer=process_observer,
                                    allowed_roots=[tree],
                                    token=client.test_token,
                                    profile=_preferences_root(profile, platform_name),
                                    direct_process=client.process)
                                client.close_logs()
                                clients.remove(client)
                        installer_requests = _requests_for_installer(
                            server.requests())
                        if installer_requests:
                            raise FunnelFailure(
                                "downloads-pickup requested installer bytes")
                        result["termination"] = termination
                        downloaded = _receipt_file_ref(destination)
                        downloaded["monotonic_seconds"] = completed_monotonic
                        attempts.append({
                            "model": model,
                            "entry_heartbeat": entry,
                            "partial_hold": waiting,
                            "detected_heartbeat": detected,
                            "downloaded_installer": downloaded,
                            "installer_requests": installer_requests,
                            "process_identity": {
                                "observer": (
                                    "mac-ps-start-time"
                                    if platform_name == "Mac" else
                                    "win32-job-get-process-times"),
                                "before": before_identity,
                                "after": after_identity,
                            },
                            "final": result,
                        })
                cell["phase_order"] = [
                    "OriginalGameNeeded", "browser-download",
                    "original-game-detected", "Ready"]
                cell["phase_timings_seconds"] = {}
                cell["evidence"] = {"attempts": attempts}
                cell["decisions"] = [
                    "every browser model began missing and completed only in the isolated per-cell Downloads directory",
                    "the same process detected source=downloads and reached the front end without an installer network request",
                ]
            _run_update_cell(
                receipt, "downloads-pickup", downloads_pickup, evidence)
            save()


        if "second-launch" in selected:
            if successful_profile is None or successful_tree is None:
                raise FunnelFailure(
                    "second-launch requires fresh-first-run in the same invocation"
                )

            def second_launch(cell: dict) -> None:
                executable = _resolve_executable(
                    successful_tree, platform_name
                )
                state_root = _state_root(successful_profile, platform_name)
                cached_state_before = _snapshot_cached_launch_state(state_root)
                before_manifest_mtimes = {
                    str(path): path.stat().st_mtime_ns
                    for path in _manifests(
                        state_root
                    )
                }
                cache_provenance = {
                    "classification": "warm-repeat",
                    "evidence": "same-invocation-profile",
                    "normal_reboot_utc": None,
                }
                log = evidence / "cells" / "second-launch"
                with LoopbackInstallerServer(
                    installer, log / "loopback-requests.jsonl",
                    release_note_sequences=(manifest["release_sequence"],),
                ) as server:
                    client = _launch_reused_profile(
                        executable, successful_profile, platform_name,
                        log, "client", server.announcement_url,
                        test_port=_available_port(),
                        release_base_url=server.release_base_url,
                    )
                    clients.append(client)
                    client.connect(args.fast_boot_timeout)
                    connected = time.monotonic()
                    cached_receipt = _wait_for_cached_handoff(
                        client, args.fast_boot_timeout, network_bytes=0,
                        cache_writes=0,
                        cold_cache_provenance=cache_provenance,
                    )
                    snapshot = _wait_snapshot(
                        client, lambda value: bool(value.get("map")),
                        args.fast_boot_timeout,
                    )
                    termination = client.terminate(args.cell_timeout)
                    clients.remove(client)
                all_requests = server.requests()
                after_manifest_mtimes = {
                    str(path): path.stat().st_mtime_ns
                    for path in _manifests(client.state_root)
                }
                cached_state_after = _snapshot_cached_launch_state(
                    client.state_root
                )
                mutation_counts = _cached_launch_mutation_counts(
                    cached_state_before, cached_state_after
                )
                heartbeat_records = cached_receipt.pop("heartbeat_records")
                update_check_requests, outside_update_check = (
                    _classify_post_ready_update_check_requests(
                        all_requests, heartbeat_records, client.process.pid
                    )
                )
                network_bytes = _network_bytes(outside_update_check)
                # Re-evaluate the gate with settled, externally observed
                # mutation evidence before publishing the cell receipt.
                cached_receipt = _cached_handoff_receipt(
                    heartbeat_records,
                    expected_pid=client.process.pid,
                    process_started_epoch=client.started_epoch,
                    network_bytes=network_bytes,
                    cold_cache_provenance=cache_provenance,
                    **mutation_counts,
                )
                if outside_update_check:
                    raise FunnelFailure(
                        "second launch made a network request outside the "
                        "post-Ready update check"
                    )
                if before_manifest_mtimes != after_manifest_mtimes:
                    raise FunnelFailure("second launch rewrote importer manifest evidence")
                cell["phase_order"] = [
                    "CheckingOriginalGameData", "Ready", "frontend-handoff"
                ]
                cell["phase_timings_seconds"] = {
                    "ready": cached_receipt["ready_seconds"],
                    "frontend_handoff": cached_receipt[
                        "frontend_handoff_seconds"
                    ],
                    "process_start": cached_receipt["process_start_seconds"],
                    "net_channel_connect": round(connected - client.started_monotonic, 6),
                }
                cell["evidence"] = {
                    **cached_receipt,
                    "server_requests": all_requests,
                    "update_check_requests": update_check_requests,
                    "manifest_mtimes_unchanged":
                        before_manifest_mtimes == after_manifest_mtimes,
                    "snapshot": snapshot,
                    "heartbeat_path": str(client.heartbeat_path),
                    "termination": termination,
                }
                cell["decisions"] = [
                    "cached readiness requires same-process Ready then frontend-handoff records with one bootstrap epoch; net-test connection and map visibility are not readiness evidence",
                    "latency begins at process creation; process_start_seconds above 3.0 records a warning without masking cache mutation or rebuild failures",
                    "cold_cache_provenance says warm-repeat because this cell reuses the same-invocation profile and makes no post-reboot claim",
                ]
            _run_cell(receipt, "second-launch", second_launch)
            save()

        if "damaged-cache-rebuild" in selected:
            if successful_profile is None or successful_tree is None:
                raise FunnelFailure(
                    "damaged-cache-rebuild requires fresh-first-run in the same invocation")

            def damaged_cache(cell: dict) -> None:
                state_root = _state_root(successful_profile, platform_name)
                current = state_root / "ContentCache" / "current"
                manifest_path = current / "manifest.json"
                old_manifest = _read_json(manifest_path, "current cache manifest")
                contract = _read_json(
                    formats.DEFAULT_CACHE_CONTRACT,
                    "production cache contract")
                startup_critical = contract.get("payload", {}).get(
                    "startup_critical_logical_ids")
                if not isinstance(startup_critical, list):
                    raise FunnelFailure(
                        "production cache contract has no startup-critical closure")
                damaged = _select_startup_critical_blob(
                    old_manifest, startup_critical)
                blob = current / "blobs" / (
                    hashlib.sha256(damaged["logical_id"].encode("utf-8")).hexdigest()
                    + ".blob")
                original = blob.read_bytes()
                if not original:
                    raise FunnelFailure("startup-critical cache blob is empty")
                blob.write_bytes(bytes([original[0] ^ 1]) + original[1:])
                log = evidence / "cells" / "damaged-cache-rebuild"
                executable = _resolve_executable(successful_tree, platform_name)
                with LoopbackInstallerServer(
                    installer, log / "loopback-requests.jsonl",
                    release_note_sequences=(manifest["release_sequence"],),
                ) as server:
                    client = _launch_reused_profile(
                        executable, successful_profile, platform_name, log,
                        "client", server.announcement_url,
                        release_base_url=server.release_base_url)
                    clients.append(client)
                    deadline = time.monotonic() + args.cell_timeout
                    while time.monotonic() < deadline:
                        client.poll_failure()
                        # A shared read never blocks the game deleting or
                        # renaming the blob while it rebuilds.
                        if (manifest_path.is_file() and blob.is_file()
                                and hashlib.sha256(_read_shared_bytes(blob)).hexdigest()
                                    == damaged["sha256"]):
                            break
                        time.sleep(0.1)
                    else:
                        raise FunnelFailure("damaged cache did not rebuild")
                    result = _wait_for_success(client, args.cell_timeout)
                    handoff = _wait_heartbeat(
                        client.heartbeat_path, client.heartbeat_offset,
                        lambda values: front_end_evidence(values, client.process.pid) is not None,
                        args.cell_timeout,
                        "damaged-cache rebuild frontend handoff")
                    result["termination"] = client.terminate(args.cell_timeout)
                    clients.remove(client)
                    requests = _requests_for_installer(server.requests())
                if requests:
                    raise FunnelFailure("damaged-cache rebuild downloaded installer")
                cell["phase_order"] = ["DamagedCache", *result["phase_order"]]
                cell["phase_timings_seconds"] = result["phase_timings_seconds"]
                cell["evidence"] = {
                    "damaged_logical_id": damaged["logical_id"],
                    "expected_blob_sha256": damaged["sha256"],
                    "frontend_handoff": front_end_evidence(
                        handoff, client.process.pid)["frontend_handoff"],
                    "repaired_blob_sha256": hashlib.sha256(blob.read_bytes()).hexdigest(),
                    "final": result,
                    "installer_requests": requests,
                }
                cell["decisions"] = [
                    "a real manifest-listed startup-critical blob was corrupted and rebuilt from the adjacent installer without a synthetic damage marker",
                ]
            _run_cell(receipt, "damaged-cache-rebuild", damaged_cache)
            save()

        if "host-join" in selected:
            if successful_profile is None or successful_tree is None:
                raise FunnelFailure(
                    "host-join requires fresh-first-run in the same invocation"
                )

            def host_join(cell: dict) -> None:
                tree_a = _clone_release_tree(
                    successful_tree,
                    _cell_tree_destination(
                        successful_tree, scratch, "host-a"
                    ),
                )
                tree_b = _clone_release_tree(
                    successful_tree,
                    _cell_tree_destination(
                        successful_tree, scratch, "host-b"
                    ),
                )
                _copy_installer_adjacent(installer, tree_a, platform_name)
                _copy_installer_adjacent(installer, tree_b, platform_name)
                profile_a = scratch / "profiles" / "host-a"
                profile_b = scratch / "profiles" / "host-b"
                shutil.copytree(successful_profile, profile_a, symlinks=True)
                shutil.copytree(successful_profile, profile_b, symlinks=True)
                log = evidence / "cells" / "host-join"
                with LoopbackInstallerServer(
                    installer, log / "loopback-requests.jsonl",
                    release_note_sequences=(manifest["release_sequence"],),
                ) as host_server, LoopbackInstallerServer(
                    installer, log / "loopback-requests-joiner.jsonl",
                    release_note_sequences=(manifest["release_sequence"],),
                ) as joiner_server:
                    a = _launch_reused_profile(
                        _resolve_executable(tree_a, platform_name),
                        profile_a, platform_name, log, "host",
                        host_server.announcement_url, test_port=_available_port(),
                        release_base_url=host_server.release_base_url,
                    )
                    b = _launch_reused_profile(
                        _resolve_executable(tree_b, platform_name),
                        profile_b, platform_name, log, "joiner",
                        joiner_server.announcement_url,
                        test_port=_available_port(),
                        release_base_url=joiner_server.release_base_url,
                    )
                    clients.extend((a, b))
                    ca = a.connect(args.fast_boot_timeout)
                    cb = b.connect(args.fast_boot_timeout)
                    before_a = _wait_snapshot(
                        a, lambda value: bool(value.get("map")),
                        args.fast_boot_timeout,
                    )
                    _wait_snapshot(
                        b, lambda value: bool(value.get("map")),
                        args.fast_boot_timeout,
                    )
                    host_deadline = time.monotonic() + args.session_timeout
                    hosted = None
                    while time.monotonic() < host_deadline:
                        a.poll_failure()
                        ca.command("exec", c="WoTHost true")
                        candidate = ca.command("snapshot").get("data", {})
                        if candidate.get("netmode") == 2:
                            hosted = candidate
                            break
                        time.sleep(0.5)
                    if hosted is None:
                        raise FunnelFailure("instance A did not reach hosted state")
                    _require_hosted_neutral_shell(hosted)
                    # WoTFind is retried inside the loop, never one-shot: the
                    # only readiness gate B passed is "a map exists", which is
                    # true all through bootstrap, so a single Find can land
                    # before B is authorized and be refused -- measured on Mac
                    # (WoTFind at Ready-minus-0.6s, searching=0, then 250
                    # WoTJoin refusals against a search that never ran).
                    # Join goes FIRST: it only accepts the last COMPLETED
                    # search, and a fresh Find bumps the generation and would
                    # invalidate it -- Find-first would starve Join forever.
                    # Re-issuing Find is otherwise safe: an in-flight search
                    # refuses the duplicate; a completed empty one re-arms.
                    join_deadline = time.monotonic() + args.session_timeout
                    joined = None

                    def _exec_tolerant(command: str) -> None:
                        # Once Join dispatches, B tears down the front end to
                        # travel; a verb landing in that window is unrouted and
                        # exec reports ok=false. That is expected churn here --
                        # the snapshot condition below is the arbiter -- so
                        # only this loop tolerates it.
                        try:
                            cb.command("exec", c=command)
                        except FunnelFailure:
                            pass

                    while time.monotonic() < join_deadline:
                        b.poll_failure()
                        _exec_tolerant("WoTJoin 0")
                        _exec_tolerant("WoTFind true")
                        candidate = cb.command("snapshot").get("data", {})
                        if (
                            candidate.get("netmode") == 3
                            and candidate.get("map") == hosted.get("map")
                        ):
                            joined = candidate
                            break
                        time.sleep(0.5)
                    if joined is None:
                        raise FunnelFailure(
                            "instance B did not discover and join session index 0"
                        )
                    cb.command("exec", c="WoTLeave")
                    joiner_returned = _wait_snapshot(
                        b, _is_returned_front_end, args.session_timeout,
                    )
                    _require_returned_front_end(joiner_returned)
                    ca.command("exec", c="WoTLeave")
                    host_returned = _wait_snapshot(
                        a, _is_returned_front_end, args.session_timeout,
                    )
                    _require_returned_front_end(host_returned)
                    host_text = _log_text(a)
                    join_text = _log_text(b)
                    term_b = b.terminate(args.cell_timeout)
                    term_a = a.terminate(args.cell_timeout)
                    clients.remove(b)
                    clients.remove(a)
                host_requests = host_server.requests()
                joiner_requests = joiner_server.requests()
                host_heartbeats = _read_heartbeat_records(
                    a.heartbeat_path, a.heartbeat_offset
                )
                joiner_heartbeats = _read_heartbeat_records(
                    b.heartbeat_path, b.heartbeat_offset
                )
                host_update_checks, host_outside = (
                    _classify_post_ready_update_check_requests(
                        host_requests, host_heartbeats, a.process.pid
                    )
                )
                joiner_update_checks, joiner_outside = (
                    _classify_post_ready_update_check_requests(
                        joiner_requests, joiner_heartbeats, b.process.pid
                    )
                )
                all_requests = sorted(
                    host_requests + joiner_requests,
                    key=lambda item: item.get("timestamp_utc", ""),
                )
                update_check_requests = sorted(
                    host_update_checks + joiner_update_checks,
                    key=lambda item: item.get("timestamp_utc", ""),
                )
                outside_update_check = host_outside + joiner_outside
                if outside_update_check:
                    raise FunnelFailure(
                        "host/join fast path made a network request outside "
                        "the post-Ready update check"
                    )
                session = "WoT Server"
                session_evidence = {
                    "format": SESSION_EVIDENCE_FORMAT,
                    "session_name": session,
                    "map": hosted["map"],
                    "host": {
                        "state": "hosted",
                        "session_name": session,
                        "netmode": hosted["netmode"],
                        "map": hosted["map"],
                        "snapshot": hosted,
                        "returned_front_end": host_returned,
                        "log_sha256": hashlib.sha256(
                            host_text.encode("utf-8")
                        ).hexdigest(),
                    },
                    "joiner": {
                        "state": "joined",
                        "session_name": session,
                        "netmode": joined["netmode"],
                        "map": joined["map"],
                        "snapshot": joined,
                        "returned_front_end": joiner_returned,
                        "log_sha256": hashlib.sha256(
                            join_text.encode("utf-8")
                        ).hexdigest(),
                    },
                }
                session_path = log / "session-evidence.json"
                _atomic_write_json(session_path, session_evidence)
                cell["phase_order"] = ["host", "find", "join", "leave"]
                cell["phase_timings_seconds"] = {
                    "host_join_total": round(
                        time.monotonic() - a.started_monotonic, 6
                    )
                }
                cell["evidence"] = {
                    "session_receipt": _snapshot_path(session_path),
                    "same_session": True,
                    "same_map": hosted["map"] == joined["map"],
                    "joiner_reached_joined_state": joined["netmode"] == 3,
                    "host_returned_to_front_end": host_returned,
                    "joiner_returned_to_front_end": joiner_returned,
                    "pre_host_snapshot": before_a,
                    "server_requests": all_requests,
                    "update_check_requests": update_check_requests,
                    "host_termination": term_a,
                    "joiner_termination": term_b,
                }
                cell["decisions"] = [
                    "session evidence is external and Test-only: token-gated packaged snapshots plus discovery logs are atomically folded into one file",
                    "R3 is fixed only if this same-box LAN discovery proof fails",
                ]
            _run_cell(receipt, "host-join", host_join)
            save()


        update_selected = selected & set(formats.UPDATE_FUNNEL_CELLS)
        if update_selected:
            if successful_profile is None or successful_tree is None:
                raise FunnelFailure("update cells require fresh-first-run in the same invocation")
            update_fixture_receipt = update_fixtures.build_update_fixtures(
                args.release, base_tree, scratch / "update-fixtures", platform=platform_name)
            evidence_derivation_receipt = evidence / "update-fixture-derivation-v1.json"
            shutil.copy2(update_fixture_receipt, evidence_derivation_receipt)
            process_observer = _make_update_process_observer(platform_name, evidence / "process-events.jsonl")
        for name in formats.UPDATE_FUNNEL_CELLS:
            if name not in update_selected:
                continue
            def run_update(cell: dict, name=name) -> None:
                started = time.monotonic()
                runner = (_run_mac_update_cell if platform_name == "Mac"
                          else _run_launcher_update_cell)
                cell["evidence"] = runner(name,
                    fixture_receipt=update_fixture_receipt, evidence_receipt=evidence_derivation_receipt,
                    receipt_path=receipt_path, base_tree=successful_tree, successful_profile=successful_profile,
                    scratch=scratch, installer=installer, log=evidence / "cells" / name,
                    clients=clients, observer=process_observer, timeout=args.cell_timeout)
                cell["phase_order"] = ["stage-or-arrange", "quit", "explicit-launch", "frontend-handoff", "verify-markers"]
                cell["phase_timings_seconds"] = {"total": round(time.monotonic() - started, 6)}
            _run_update_cell(receipt, name, run_update, evidence)
            save()

        if "mac-uninstall" in selected:
            if platform_name != "Mac":
                raise FunnelFailure("mac-uninstall is Mac-only")
            if successful_profile is None or successful_tree is None:
                raise FunnelFailure(
                    "mac-uninstall requires the successful first-run tree and profile")

            def mac_uninstall(cell: dict) -> None:
                started = time.monotonic()
                log = evidence / "cells" / "mac-uninstall"
                cell["evidence"] = _run_mac_uninstall_cell(
                    successful_tree, successful_profile, installer, log,
                    args.cell_timeout)
                cell["phase_order"] = [
                    "launch-existing-profile", "confirm-uninstall",
                    "remove-data", "remove-updater-temp", "recycle-app",
                    "normal-exit", "verify-absence",
                ]
                cell["phase_timings_seconds"] = {
                    "total": round(time.monotonic() - started, 6)}
                cell["decisions"] = [
                    "the final Mac-only cell consumed the successful isolated profile and app tree in place",
                    "the Test-only confirm seam published the closed uninstall result before normal exit",
                ]

            _run_update_cell(
                receipt, "mac-uninstall", mac_uninstall, evidence)
            save()

        selected_cells = [
            next(cell for cell in receipt["cells"] if cell["name"] == name)
            for name in selected
        ]
        receipt["verdict"] = (
            "pass"
            if all(cell["verdict"] in {"pass", "skip"} for cell in selected_cells)
            else "fail"
        )
        receipt["decisions"].append(
            f"{platform_name} was exercised by the shared platform-neutral "
            "harness and must publish its own receipt"
        )
        save()
        if receipt["verdict"] != "pass":
            raise FunnelFailure("one or more selected cells failed")
        return receipt
    except BaseException:
        receipt["verdict"] = "fail"
        _mark_unreached_selected_cells(receipt, selected)
        save()
        raise
    finally:
        cleanup = []
        for client in list(clients):
            try:
                client.terminate(args.cell_timeout)
            except BaseException:
                pass
        if process_observer is not None:
            if platform_name == "Mac":
                try:
                    cleanup.extend(_quiesce_owned_mac_processes(
                        process_observer, scratch))
                except BaseException as exc:
                    cleanup.append({"action": "mac-cleanup-error",
                                    "error": str(exc)})
            try:
                process_observer.close()
            except BaseException as exc:
                cleanup.append({"action": "observer-close-error",
                                "error": str(exc)})
        try:
            cleanup.extend(_cleanup_processes_under(scratch))
        except BaseException as exc:
            cleanup.append({"action": "cleanup-error", "error": str(exc)})
        _atomic_write_json(evidence / "process-cleanup.json", cleanup)
        if real_profile_before is not None:
            real_profile_after = _mac_real_support_inventory()
            real_profile_unchanged = real_profile_after == real_profile_before
            _atomic_write_json(real_profile_guard_path, {
                "after": real_profile_after, "before": real_profile_before,
                "format": "wot-real-mac-preferences-guard-v1",
                "scope": "whole-run", "unchanged": real_profile_unchanged,
            })
            if not real_profile_unchanged:
                receipt["verdict"] = "fail"
                receipt["decisions"].append(
                    "SAFETY: the developer's real Mac data roots changed "
                    "during the run")
                save()
                if sys.exc_info()[0] is None:
                    raise FunnelFailure(
                        "SAFETY: real Mac data roots changed during the run "
                        f"(inventory: {real_profile_guard_path})")
        if firewall_before is not None:
            # Every cell tree lives under the scratch root; the launched
            # clients' install roots are listed too. A rule naming any of
            # them that was not there before the first launch means a cell
            # raised a Windows Firewall prompt.
            firewall = _record_cell_firewall_rules(
                evidence,
                [scratch, *sorted(set(_WIN_INSTALL_ROOTS.values()), key=str)],
                firewall_before)
            if not firewall["passed"]:
                receipt["verdict"] = "fail"
                receipt["decisions"].append(
                    "SAFETY: a new Windows Firewall rule names a cell's "
                    "program (a cell raised a firewall prompt), or a "
                    "read-only rule query failed")
                save()
                if sys.exc_info()[0] is None:
                    raise FunnelFailure(
                        "SAFETY: new Windows Firewall rules name cell programs "
                        "or the rules could not be read (evidence: "
                        f"{evidence / 'safety' / 'firewall-rules.json'})")
        cleanup_failed = any(row.get("action") in {
            "terminated", "aborted-identity-change", "mac-cleanup-error",
            "observer-close-error",
            "cleanup-error",
        } for row in cleanup)
        if cleanup_failed:
            receipt["verdict"] = "fail"
            receipt["decisions"].append(
                "run-owned process cleanup was required or could not be proven")
            save()
            if sys.exc_info()[0] is None:
                raise FunnelFailure(
                    "run-owned process cleanup was required or failed")
        if remove_scratch and receipt["verdict"] == "pass":
            shutil.rmtree(scratch, ignore_errors=True)


def _launch_reused_profile(
    executable: Path, profile: Path, platform_name: str,
    log_root: Path, label: str, announcement_url: str | None,
    test_port: int | None = None,
    release_base_url: str | None = None,
    environment_overrides: dict[str, str] | None = None,
    extra_args: Iterable[str] | None = None,
    through_launcher: bool = False,
) -> ClientProcess:
    """Launch without the clean-profile assertion for an intentional relaunch."""
    if not profile.is_dir():
        raise FunnelFailure(f"relaunch profile is missing: {profile}")
    client = ClientProcess.__new__(ClientProcess)
    log_root.mkdir(parents=True, exist_ok=True)
    client.stdout_path = log_root / f"{label}.stdout.log"
    client.stderr_path = log_root / f"{label}.stderr.log"
    client.boot_failure_path = log_root / f"{label}.boot-failure.json"
    client.stdout_handle = client.stdout_path.open("wb")
    client.stderr_handle = client.stderr_path.open("wb")
    client.profile = profile
    client.platform_name = platform_name
    install_root = (_win_install_root_from_executable(executable)
                    if platform_name == "Win64" else None)
    if install_root is not None:
        _WIN_INSTALL_ROOTS[profile.resolve()] = install_root
    client.state_root = _state_root(profile, platform_name, install_root)
    client.heartbeat_path, client.heartbeat_offset = (
        _heartbeat_launch_boundary(client.state_root)
    )
    client.test_port = test_port
    client.test_token = f"wot-funnel-{os.getpid()}-{label}"
    client.connection = None
    command = [
        str(executable), "-unattended", "-RenderOffscreen", "-ForceRes",
        "-ResX=640", "-ResY=480", "-NoSteam", "-nosplash",
        "-nocrashreports", "-windowed",
        f"-wottestpreferencesdir={_preferences_root(profile, platform_name, install_root)}",
    ]
    if test_port is not None:
        command.extend([
            "-wotnettest", f"-wotnettestport={test_port}",
            "-wotnettestbind=127.0.0.1",
            f"-wotnettesttoken={client.test_token}",
        ])
    command.extend(extra_args or ())
    environment = _profile_environment(profile, platform_name, install_root)
    # A caller's shell must never redirect a Test launch to an unrecorded
    # endpoint. Each reused-profile launch receives only its explicit pair.
    environment.pop(TEST_ANNOUNCEMENT_URL_ENV, None)
    environment.pop(TEST_RELEASE_BASE_URL_ENV, None)
    if announcement_url:
        environment[TEST_ANNOUNCEMENT_URL_ENV] = announcement_url
    if release_base_url:
        _validate_release_base_url(release_base_url)
        environment[TEST_RELEASE_BASE_URL_ENV] = release_base_url
    environment.update(environment_overrides or {})
    use_windows_launcher = through_launcher and platform_name == "Win64"
    if use_windows_launcher:
        command[0] = str(install_root / "WoT Reborn.exe")
    client.command = command
    client.started_epoch = time.time()
    client.started_monotonic = time.monotonic()
    client.process = subprocess.Popen(
        command,
        cwd=str(install_root if use_windows_launcher else executable.parent),
        env=environment,
        stdout=client.stdout_handle, stderr=client.stderr_handle,
        start_new_session=True,
    )
    return client


def _wait_for_mac_uninstall_result(
        result_path: Path, client: ClientProcess, timeout: float) -> tuple[dict, int]:
    deadline = time.monotonic() + timeout
    result = None
    while time.monotonic() < deadline:
        if result_path.is_file():
            result = _read_json(result_path, "Mac uninstall result")
            break
        if client.process.poll() is not None:
            # The result is written immediately before normal exit. Allow the
            # filesystem publication to become visible without treating that
            # expected exit as a bootstrap failure.
            time.sleep(0.05)
        else:
            time.sleep(0.05)
    if result is None:
        raise FunnelFailure(
            f"Mac uninstall did not publish {result_path} within {timeout}s")
    remaining = max(0.0, deadline - time.monotonic())
    try:
        exit_status = client.process.wait(timeout=remaining)
    except subprocess.TimeoutExpired as exc:
        raise FunnelFailure("Mac uninstall client did not exit normally") from exc
    if exit_status != 0:
        raise FunnelFailure(
            f"Mac uninstall client exited with status {exit_status}")
    return result, exit_status


def _hard_reap_client_without_notes(
        client: ClientProcess, timeout: float) -> dict:
    """Hard-stop and reap a client without consulting optional evidence."""
    signals = []
    if client.process.poll() is None:
        try:
            signals.append(_instance_tree_kill(client.process))
        except ProcessLookupError:
            pass
    reap_timeout = max(1.0, min(timeout, 10.0))
    try:
        returncode = client.process.wait(timeout=reap_timeout)
    except subprocess.TimeoutExpired:
        if client.process.poll() is None:
            try:
                signals.append(_instance_tree_kill(client.process))
            except ProcessLookupError:
                pass
        returncode = client.process.wait(timeout=10.0)
    return {"signals": signals, "returncode": returncode}


def _run_mac_uninstall_cell(
        tree: Path, profile: Path, installer: Path,
        log_root: Path, timeout: float) -> dict:
    """Run the destructive final Mac cell in its already-isolated profile."""
    if not tree.is_dir() or not profile.is_dir():
        raise FunnelFailure("Mac uninstall requires the successful tree and profile")
    bundle = tree.resolve()
    profile = profile.resolve()
    installer = installer.resolve()
    executable = _resolve_executable(bundle, "Mac")
    data_root = _state_root(profile, "Mac").resolve()
    if not data_root.is_dir():
        raise FunnelFailure("Mac uninstall data root is missing before launch")
    session_gate = data_root / "session.lock"
    update_dir = (bundle.parent / ".wot-update").resolve()
    update_dir.mkdir(parents=False, exist_ok=True)
    (update_dir / "mac-uninstall-funnel-marker").write_text(
        "temporary updater data\n", encoding="utf-8")
    bundle_parent = bundle.parent
    adjacent_installer = bundle_parent / INSTALLER_NAME
    if not adjacent_installer.is_file():
        raise FunnelFailure(
            f"Mac uninstall adjacent installer is missing: {adjacent_installer}")
    sentinel = bundle_parent / ".wot-uninstall-funnel-sentinel"
    try:
        with sentinel.open("x", encoding="utf-8") as handle:
            handle.write("the bundle parent must survive uninstall\n")
    except FileExistsError as exc:
        raise FunnelFailure(
            f"Mac uninstall sibling sentinel already exists: {sentinel}") from exc

    def directory_identity(path: Path) -> dict:
        try:
            status = path.stat()
        except OSError as exc:
            raise FunnelFailure(
                f"protected directory is missing or unreadable: {path}") from exc
        if not path.is_dir() or path.is_symlink():
            raise FunnelFailure(f"protected directory is unsafe: {path}")
        return {
            "device": status.st_dev,
            "inode": status.st_ino,
            "path": str(path.resolve()),
        }

    def file_identity(path: Path) -> dict:
        try:
            status = path.stat()
        except OSError as exc:
            raise FunnelFailure(
                f"protected file is missing or unreadable: {path}") from exc
        if not path.is_file() or path.is_symlink():
            raise FunnelFailure(f"protected file is unsafe: {path}")
        return {
            "device": status.st_dev,
            "inode": status.st_ino,
            "path": str(path.resolve()),
            "sha256": _sha256_file(path),
            "size": status.st_size,
        }

    parent_before = directory_identity(bundle_parent)
    installer_before = file_identity(adjacent_installer)
    sentinel_before = file_identity(sentinel)
    session_gate_before = file_identity(session_gate)
    session_gate_mode = stat.S_IMODE(session_gate.stat().st_mode)
    if session_gate_mode & 0o077:
        raise FunnelFailure(
            "Mac uninstall session gate is not owner-only before launch")
    session_gate_before["mode"] = session_gate_mode
    result_path = data_root.parent / "wot-uninstall-result.json"
    if result_path.exists():
        result_path.unlink()

    with LoopbackInstallerServer(
            installer, log_root / "loopback-requests.jsonl") as server:
        client = _launch_reused_profile(
            executable, profile, "Mac", log_root, "uninstall",
            server.announcement_url,
            release_base_url=server.release_base_url,
            extra_args=["-wottestuninstall=confirm"])
        try:
            try:
                result, exit_status = _wait_for_mac_uninstall_result(
                    result_path, client, timeout)
            except BaseException:
                try:
                    client.terminate(timeout)
                except BaseException:
                    _hard_reap_client_without_notes(client, timeout)
                raise
        finally:
            client.close_logs()
        requests = server.requests()

    allowed_requests = {
        "/announcement.json", "/announcement.json.sig", RELEASE_NOTES_ROUTE,
    }
    if any(request.get("method") != "GET"
           or request.get("path") not in allowed_requests
           for request in requests):
        raise FunnelFailure(
            "Mac uninstall made a request outside its loopback endpoint contract")

    parent_after = directory_identity(bundle_parent)
    installer_after = file_identity(adjacent_installer)
    sentinel_after = file_identity(sentinel)
    parent_unchanged = parent_after == parent_before
    installer_unchanged = installer_after == installer_before
    sentinel_unchanged = sentinel_after == sentinel_before
    if not (parent_unchanged and installer_unchanged and sentinel_unchanged):
        raise FunnelFailure(
            "Mac uninstall changed its bundle parent, adjacent installer, or sentinel")

    bundle_absent = not bundle.exists()
    trash = profile / ".Trash"
    trash_contains = (trash / bundle.name).exists()
    evidence = {
        "paths": {
            "bundle": str(bundle),
            "bundle_parent": str(bundle_parent),
            "data_root": str(data_root),
            "installer": str(adjacent_installer),
            "profile": str(profile),
            "session_gate": str(session_gate),
            "sibling_sentinel": str(sentinel),
            "trash": str(trash),
            "update_dir": str(update_dir),
        },
        "postconditions": {
            "bundle_absent": bundle_absent,
            "bundle_parent_unchanged": parent_unchanged,
            "data_root_absent": not data_root.exists(),
            "installer_unchanged": installer_unchanged,
            "session_gate_absent": not session_gate.exists(),
            "sibling_sentinel_unchanged": sentinel_unchanged,
            "trash_contains_bundle_or_original_absent": (
                trash_contains or bundle_absent),
            "update_dir_absent": not update_dir.exists(),
        },
        "protected_artifacts": {
            "bundle_parent_after": parent_after,
            "bundle_parent_before": parent_before,
            "installer_after": installer_after,
            "installer_before": installer_before,
            "sibling_sentinel_after": sentinel_after,
            "sibling_sentinel_before": sentinel_before,
        },
        "network": {
            "announcement_url": server.announcement_url,
            "inherited_endpoint_variables_scrubbed": True,
            "release_base_url": server.release_base_url,
        },
        "process": {
            "argv": list(client.command),
            "exit_status": exit_status,
            "pid": client.process.pid,
        },
        "request_transcript": requests,
        "result": result,
        "session_gate": {
            "before": session_gate_before,
            "owner_only_before_launch": True,
        },
    }
    try:
        formats.validate_update_cell_evidence(
            "mac-uninstall", evidence, "Mac")
    except formats.FormatError as exc:
        raise FunnelFailure(str(exc)) from exc
    return evidence


def _resolve_funnel_release(args: argparse.Namespace) -> tuple[dict, Path]:
    """Resolve a funnel release, limiting the development override to drift."""
    return _resolve_release(
        args.release,
        require_current_source=not bool(getattr(
            args, "development_allow_source_drift", False)),
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the closed guarded first-run funnel matrix"
    )
    parser.add_argument("--release", type=Path, help="guarded release.json")
    parser.add_argument(
        "--installer", type=Path,
        help="pin-valid player-supplied original-game installer",
    )
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    parser.add_argument(
        "--cells", action="append", default=[],
        help=("comma-separated names or zero-based indices; defaults to the "
              "platform matrix (Mac and Win64 include their update cells)"),
    )
    parser.add_argument("--platform", choices=("Mac", "Win64"))
    parser.add_argument("--evidence", type=Path)
    parser.add_argument(
        "--work-root", type=Path,
        help="empty scratch root to retain; a dirty root is refused",
    )
    parser.add_argument(
        "--development-allow-source-drift", action="store_true",
        help=("development runs only: skip Source/Tools snapshot equality; "
              "the stamped receipt is rejected as acceptance evidence"),
    )
    parser.add_argument("--cell-timeout", type=float, default=7200.0)
    parser.add_argument("--session-timeout", type=float, default=180.0)
    parser.add_argument("--fast-boot-timeout", type=float, default=60.0)
    parser.add_argument("--acquisition-timeout", type=float, default=900.0)
    parser.add_argument(
        "--loopback-bytes-per-second", type=int, default=0,
        help="optional loopback throttle for non-kill cells",
    )
    parser.add_argument(
        "--kill-download-bytes-per-second", type=int, default=32 * 1024 * 1024
    )
    parser.add_argument(
        "--kill-download-after-bytes", type=int, default=8 * 1024 * 1024
    )
    parser.add_argument("--_download-worker", type=Path, help=argparse.SUPPRESS)
    return parser


def main() -> int:
    parser = _build_parser()
    args = parser.parse_args()
    if args._download_worker is not None:
        return _download_worker(args._download_worker)
    if args.release is None:
        parser.error("--release is required")
    for name in (
        "cell_timeout", "session_timeout", "fast_boot_timeout",
        "acquisition_timeout",
    ):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.kill_download_after_bytes <= 0:
        parser.error("--kill-download-after-bytes must be positive")
    try:
        receipt = _run_acceptance(args)
    except FunnelFailure as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    evidence = (
        args.evidence.expanduser().resolve()
        if args.evidence
        else DEFAULT_EVIDENCE_ROOT / receipt["platform"]
    )
    print(f"PASS: {evidence / 'funnel-receipt.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
