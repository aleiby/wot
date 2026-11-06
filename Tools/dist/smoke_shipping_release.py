#!/usr/bin/env python3
"""Smoke-test a guarded SHIPPING release package end to end.

The first-run funnel (run_first_run_funnel.py) proves the Test twin through
seams Shipping compiles out: the env-var loopback installer/announcement URLs
(WOT_BOOTSTRAP_TEST_ENDPOINTS), -wottestinstallroot and the -wotnettest
channel (WOT_WITH_BOOTSTRAP_TEST_HARNESSES=0 in guarded Shipping,
WoT.Build.cs:18-23). This smoke is the Shipping-side proof, built only from
evidence a Shipping image still produces:

* the release.json chain: the same validators the funnel applies to a Test
  release (run_first_run_funnel._resolve_release, configuration="Shipping"),
  plus the production target receipts, the -skipcook stage proof and the
  test_twin binding to the funnel-proved Test release;
* the extracted image itself: the decorated WoT-<P>-Shipping game, no Test
  image, no staged debug files, and none of the compiled test-seam literals
  (release_formats.TEST_SEAM_MARKERS) in the game;
* the plain per-process bootstrap heartbeat. UE_LOG is compiled out of guarded Test and
  Shipping alike (Build.h:323 and :351, NO_LOGGING = !USE_LOGGING_IN_SHIPPING;
  WoT.Target.cs never sets bUseLoggingInShipping), but
  WoTRecordBootstrapHeartbeat writes
  <NSSearchPath Application Support>/WoT Reborn/UpdaterState/
  bootstrap-heartbeat-v1.<pid>.<guid>.jsonl on Mac in every
  WOT_GUARDED_BOOTSTRAP build, Shipping
  included: phase-ready/frontend-handoff for the front end
  (WoTFrontEndPlayerController.cpp:37-73), host-travel/world-exposed/
  player-possess (net=2) for a hosted neutral-shell map (WoTSessionSubsystem.cpp:737,
  WoTWorldMaterializer.cpp:3127, WoTPlayerController.cpp:2779);
* the per-user published ContentCache/current manifest that the
  bootstrap publishes after building the cache (same state root).

The installer must be supplied: it is copied ADJACENT to the extracted tree,
which is the only no-network path a Shipping image has (the loopback URL seam
is compiled out), so an adjacent-installer boot never contacts archive.org or
the announcement URL (WoTGameBootstrapWorker.cpp:1229-1256).

Hosting has no automation seam in Shipping by design: -ExecCmds is ignored
(UnrealEngine.cpp:2555-2564, !UE_BUILD_SHIPPING) and there is no console
(ALLOW_CONSOLE_IN_SHIPPING 0). With --host-trigger manual (the default) the
operator clicks Play > Host Match in the visible window when prompted and the
smoke judges the heartbeat; --host-trigger none stops after the front end.
"""
from __future__ import annotations

import argparse
import ctypes
import ntpath
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Iterable, Optional

DIST = Path(__file__).resolve().parent
if str(DIST) not in sys.path:
    sys.path.insert(0, str(DIST))

import release_formats as formats  # noqa: E402
import run_first_run_funnel as funnel  # noqa: E402
import sign_release  # noqa: E402

SMOKE_FORMAT = "wot-shipping-smoke-v1"
SHIPPING = formats.SHIPPING_CONFIGURATION
DEFAULT_EVIDENCE_ROOT = (
    funnel.REPO / "Saved" / "ReleaseEvidence" / "shipping-smoke")
# UE ENetMode: NM_Standalone=0, NM_DedicatedServer=1, NM_ListenServer=2.
LISTEN_SERVER_NET_MODE = 2
SHELL_PACKAGE = re.compile(r"/WoTNeutralShells/(WoTMapShell(?:0[1-9]|1[0-9]))\b")
# Heartbeat events that end a smoke: a durable refusal/failure the guarded
# client records instead of logging (WoTBootstrapSubsystem.cpp:300, 1096;
# WoTFrontEndSlate.cpp:1357, 1742; WoTWorldMaterializer.cpp:552;
# WoTFrontEndPlayerController.cpp:50).
FAILURE_EVENTS = frozenset({
    "fail", "initialization-failed", "frontend-handoff-refused",
    "host-refused", "host-failed", "world-rejected", "world-return-rejected",
})
DEBUG_FILE_SUFFIXES = (".pdb", ".dsym")


class SmokeFailure(RuntimeError):
    """The Shipping package failed a smoke check."""


class _ProcessEntry32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", ctypes.c_uint32),
        ("cntUsage", ctypes.c_uint32),
        ("th32ProcessID", ctypes.c_uint32),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", ctypes.c_uint32),
        ("cntThreads", ctypes.c_uint32),
        ("th32ParentProcessID", ctypes.c_uint32),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", ctypes.c_uint32),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class _FileTime(ctypes.Structure):
    _fields_ = [
        ("dwLowDateTime", ctypes.c_uint32),
        ("dwHighDateTime", ctypes.c_uint32),
    ]


def _windows_process_creation_time(kernel32: object, handle: object,
                                   description: str) -> int:
    get_times = kernel32.GetProcessTimes
    get_times.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(_FileTime)] * 4
    get_times.restype = ctypes.c_int
    creation = _FileTime()
    exit_time = _FileTime()
    kernel_time = _FileTime()
    user_time = _FileTime()
    if not get_times(handle, ctypes.byref(creation), ctypes.byref(exit_time),
                     ctypes.byref(kernel_time), ctypes.byref(user_time)):
        raise SmokeFailure(
            f"could not query {description} creation time: "
            f"Win32 error {ctypes.get_last_error()}")
    return (int(creation.dwHighDateTime) << 32) | int(creation.dwLowDateTime)


def _windows_launcher_creation_time(launcher: subprocess.Popen) -> int:
    """Creation time from Popen's retained Windows process handle."""
    handle = getattr(launcher, "_handle", None)
    if handle is None:
        raise SmokeFailure("the Shipping launcher has no retained process handle")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    return _windows_process_creation_time(
        kernel32, handle, f"Shipping launcher process {launcher.pid}")


def _normalized_windows_image_path(path: object) -> str:
    """Normalize equivalent DOS/UNC image spellings for identity comparison."""
    value = os.fspath(path).replace("/", "\\")
    if value.casefold().startswith("\\\\?\\unc\\"):
        value = "\\\\" + value[8:]
    elif value.casefold().startswith("\\\\?\\"):
        value = value[4:]
    return ntpath.normpath(ntpath.abspath(value)).casefold()


class _WindowsChildProcess:
    """Retained native handle for the exact game child found in Toolhelp."""

    def __init__(self, pid: int):
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        open_process = kernel32.OpenProcess
        open_process.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        open_process.restype = ctypes.c_void_p
        self._kernel32 = kernel32
        self._kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        self._kernel32.CloseHandle.restype = ctypes.c_int
        self.pid = pid
        # SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE.
        self._handle = open_process(0x00100000 | 0x1000 | 0x0001, 0, pid)
        if not self._handle:
            raise SmokeFailure(
                f"could not retain the Shipping game process {pid}: "
                f"Win32 error {ctypes.get_last_error()}")
        self.returncode: Optional[int] = None

    def creation_time(self) -> int:
        return _windows_process_creation_time(
            self._kernel32, self._handle,
            f"Shipping game process {self.pid}")

    def image_path(self) -> str:
        query = self._kernel32.QueryFullProcessImageNameW
        query.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                          ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
        query.restype = ctypes.c_int
        buffer = ctypes.create_unicode_buffer(32768)
        size = ctypes.c_uint32(len(buffer))
        if not query(self._handle, 0, buffer, ctypes.byref(size)):
            raise SmokeFailure(
                f"could not query Shipping game process {self.pid} image: "
                f"Win32 error {ctypes.get_last_error()}")
        return buffer.value

    def poll(self) -> Optional[int]:
        if self.returncode is not None:
            return self.returncode
        get_exit_code = self._kernel32.GetExitCodeProcess
        get_exit_code.argtypes = [ctypes.c_void_p,
                                  ctypes.POINTER(ctypes.c_uint32)]
        get_exit_code.restype = ctypes.c_int
        code = ctypes.c_uint32()
        if not get_exit_code(self._handle, ctypes.byref(code)):
            raise SmokeFailure(
                f"could not query Shipping game process {self.pid}: "
                f"Win32 error {ctypes.get_last_error()}")
        if code.value != 259:  # STILL_ACTIVE
            self.returncode = int(code.value)
        return self.returncode

    def wait(self, timeout: Optional[float] = None) -> int:
        wait = self._kernel32.WaitForSingleObject
        wait.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        wait.restype = ctypes.c_uint32
        milliseconds = 0xffffffff if timeout is None else max(
            0, min(0xfffffffe, int(timeout * 1000)))
        result = wait(self._handle, milliseconds)
        if result == 0x00000102:  # WAIT_TIMEOUT
            raise subprocess.TimeoutExpired(
                ["WoT-Win64-Shipping.exe"], timeout)
        if result != 0:
            raise SmokeFailure(
                f"could not wait for Shipping game process {self.pid}: "
                f"Win32 error {ctypes.get_last_error()}")
        code = self.poll()
        if code is None:
            raise SmokeFailure(
                f"Shipping game process {self.pid} signaled without an exit code")
        return code

    def kill(self) -> None:
        terminate = self._kernel32.TerminateProcess
        terminate.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        terminate.restype = ctypes.c_int
        if not terminate(self._handle, 9):
            error = ctypes.get_last_error()
            if self.poll() is None:
                raise SmokeFailure(
                    f"could not terminate Shipping game process {self.pid}: "
                    f"Win32 error {error}")

    def close(self) -> None:
        if self._handle:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None


def _toolhelp_process_entries() -> list[tuple[int, int, str]]:
    """One no-dependency Windows process snapshot: pid, parent pid, image."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int
    create_snapshot = kernel32.CreateToolhelp32Snapshot
    create_snapshot.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    create_snapshot.restype = ctypes.c_void_p
    first = kernel32.Process32FirstW
    next_entry = kernel32.Process32NextW
    for function in (first, next_entry):
        function.argtypes = [ctypes.c_void_p,
                             ctypes.POINTER(_ProcessEntry32W)]
        function.restype = ctypes.c_int
    snapshot = create_snapshot(0x00000002, 0)  # TH32CS_SNAPPROCESS
    invalid = ctypes.c_void_p(-1).value
    if snapshot in (None, invalid):
        raise SmokeFailure(
            f"could not snapshot Windows processes: "
            f"Win32 error {ctypes.get_last_error()}")
    entries: list[tuple[int, int, str]] = []
    try:
        entry = _ProcessEntry32W()
        entry.dwSize = ctypes.sizeof(entry)
        if not first(snapshot, ctypes.byref(entry)):
            error = ctypes.get_last_error()
            if error != 18:  # ERROR_NO_MORE_FILES
                raise SmokeFailure(
                    f"could not read the Windows process snapshot: "
                    f"Win32 error {error}")
            return entries
        while True:
            entries.append((int(entry.th32ProcessID),
                            int(entry.th32ParentProcessID), entry.szExeFile))
            entry.dwSize = ctypes.sizeof(entry)
            if not next_entry(snapshot, ctypes.byref(entry)):
                error = ctypes.get_last_error()
                if error != 18:
                    raise SmokeFailure(
                        f"could not continue the Windows process snapshot: "
                        f"Win32 error {error}")
                break
    finally:
        kernel32.CloseHandle(snapshot)
    return entries


def _discover_windows_game_child(
        launcher: subprocess.Popen, expected_image: Path,
        timeout: float) -> _WindowsChildProcess:
    """Find the launcher's exact, configuration-specific direct game child."""
    if os.name != "nt":
        raise SmokeFailure("Windows game-child discovery requires Windows")
    deadline = time.monotonic() + timeout
    expected_name = expected_image.name.casefold()
    expected_path = _normalized_windows_image_path(expected_image)
    launcher_creation = _windows_launcher_creation_time(launcher)
    while time.monotonic() < deadline:
        for pid, parent_pid, image in _toolhelp_process_entries():
            if (parent_pid != launcher.pid
                    or Path(image).name.casefold() != expected_name):
                continue
            try:
                candidate = _WindowsChildProcess(pid)
            except SmokeFailure:
                # The snapshot is advisory. A candidate can disappear before
                # OpenProcess; only a successfully retained and verified handle
                # can ever become the process later terminated by the smoke.
                continue
            verified = False
            try:
                if candidate.creation_time() < launcher_creation:
                    continue
                if (_normalized_windows_image_path(candidate.image_path())
                        != expected_path):
                    continue
                if not any(
                        fresh_pid == pid and fresh_parent == launcher.pid
                        for fresh_pid, fresh_parent, _fresh_image
                        in _toolhelp_process_entries()):
                    continue
                verified = True
                return candidate
            except SmokeFailure:
                continue
            finally:
                if not verified:
                    candidate.close()
        if launcher.poll() is not None:
            raise SmokeFailure(
                f"Shipping launcher exited {launcher.returncode} before starting "
                f"{expected_image.name}")
        time.sleep(0.05)
    raise SmokeFailure(
        f"timed out after {timeout:g}s waiting for the Shipping launcher to "
        f"start {expected_image.name}")


# ---------------------------------------------------------------------------
# Release chain (pure; unit-tested)
# ---------------------------------------------------------------------------

def _stage_command_proof(command: object) -> list[str]:
    if not isinstance(command, list) or not all(
            isinstance(token, str) for token in command):
        raise SmokeFailure("shipping stage command is malformed")
    folded = {token.casefold() for token in command}
    required = {"buildcookrun", "-skipbuild", "-skipcook", "-stage", "-pak",
                "-nodebuginfo", "-clientconfig=shipping"}
    if (not required.issubset(folded) or folded & {"-cook", "-build"}
            or any(token.startswith(("-map=", "-additionalcookeroptions"))
                   for token in folded)):
        raise SmokeFailure(
            "shipping stage was not a -skipbuild -skipcook stage of the Test cook")
    return sorted(required)


def validate_shipping_manifest(manifest: dict) -> dict:
    """Shipping-only facts on top of the funnel's shared release validators."""
    platform_name = manifest.get("platform")
    commit = manifest.get("git_commit")
    if manifest.get("configuration") != SHIPPING:
        raise SmokeFailure("release.json is not a guarded Shipping release")
    try:
        expected_asset = formats.guarded_install_zip_asset(
            str(platform_name), str(commit), SHIPPING)
        twin = formats.parse_test_twin(manifest, "release.json")
    except formats.FormatError as exc:
        raise SmokeFailure(str(exc)) from exc
    if manifest.get("asset") != expected_asset:
        raise SmokeFailure(
            f"Shipping asset must be {expected_asset}, not {manifest.get('asset')}")
    receipts = manifest.get("target_receipts", {})
    game = receipts.get("WoT", {}).get("compiled_receipt", {})
    definitions = game.get("definitions", {})
    if (game.get("configuration") != SHIPPING
            or definitions.get("WOT_GUARDED_BOOTSTRAP") != "1"
            or definitions.get("WOT_BOOTSTRAP_COMPILED_COMPONENT_IDENTITY")
            != '"wot-game-production-v1"'
            or definitions.get("WOT_BOOTSTRAP_LIFECYCLE_ENDPOINT")
            != '"production"'
            or any(name in definitions for name in (
                "WOT_BOOTSTRAP_TEST_IDENTITIES",
                "WOT_BOOTSTRAP_TEST_ENDPOINTS"))):
        raise SmokeFailure(
            "the WoT build receipt is not the production Shipping game "
            "(WoT.Target.cs:50-58)")

    stage = manifest.get("shipping_stage", {}).get("receipt", {})
    cooked = stage.get("cooked_output", {})
    proof = _stage_command_proof(stage.get("command"))
    if (stage.get("configuration") != SHIPPING
            or stage.get("exit_code") != 0
            or not isinstance(cooked, dict)
            or not cooked.get("before")
            or cooked.get("before") != cooked.get("after")):
        raise SmokeFailure(
            "the shipping stage receipt does not prove an unchanged Test cook")
    zen = manifest.get("zen_lifetime", {})
    if zen.get("receipt_sha256") != twin["zen_lifetime_receipt_sha256"]:
        raise SmokeFailure(
            "the Shipping release names a different import+cook than its twin")
    return {
        "platform": platform_name,
        "git_commit": commit,
        "asset": expected_asset,
        "test_twin_release_json_sha256": twin["release_json_sha256"],
        "test_twin_asset": twin["asset"],
        "container_bytes": twin["container_bytes"],
        "stage_command_proof": proof,
    }


# ---------------------------------------------------------------------------
# Extracted image (pure over a directory; unit-tested)
# ---------------------------------------------------------------------------

def resolve_shipping_image(tree: Path, platform_name: str) -> Path:
    """The decorated Shipping game image."""
    tests = [path for path in tree.rglob("*")
             if path.is_file() and re.search(r"-Test(\.exe)?$", path.name)]
    if tests:
        raise SmokeFailure(
            f"Shipping tree carries a Test image: {tests[0].relative_to(tree)}")
    if platform_name == "Mac":
        game = funnel._resolve_executable(tree, "Mac")
        expected = f"WoT-Mac-{SHIPPING}"
    elif platform_name == "Win64":
        candidates = sorted(tree.rglob(f"WoT-Win64-{SHIPPING}.exe"))
        if len(candidates) != 1:
            raise SmokeFailure(
                "cannot identify one packaged WoT-Win64-Shipping.exe")
        game = candidates[0]
        expected = game.name
    else:
        raise SmokeFailure(f"unsupported platform {platform_name}")
    if game.name != expected or not game.is_file():
        raise SmokeFailure(f"packaged game image is not {expected}: {game}")
    return game


def image_seam_report(paths: Iterable[Path]) -> dict:
    """Compiled test-seam literals per image; every list must be empty."""
    report = {}
    for path in paths:
        with Path(path).open("rb") as handle:
            data = handle.read()
        report[Path(path).name] = {
            "sha256": funnel._sha256_file(Path(path)),
            "size": len(data),
            "test_seam_markers": formats.find_test_seam_markers(data),
        }
    return report


def debug_files(tree: Path) -> list[str]:
    return sorted(
        path.relative_to(tree).as_posix() for path in tree.rglob("*")
        if path.name.casefold().endswith(DEBUG_FILE_SUFFIXES))


# ---------------------------------------------------------------------------
# Runtime evidence (pure over heartbeat records / state root; unit-tested)
# ---------------------------------------------------------------------------

def _process_records(records: list[dict], pid: int) -> list[dict]:
    return [record for record in records if record.get("pid") == pid]


def failure_event(records: list[dict], pid: int) -> Optional[dict]:
    for record in _process_records(records, pid):
        if record.get("event") in FAILURE_EVENTS:
            return record
    return None


def front_end_evidence(records: list[dict], pid: int) -> Optional[dict]:
    return funnel.front_end_evidence(records, pid)


def adjacent_original_game_evidence(records: list[dict], pid: int
                                    ) -> Optional[dict]:
    for record in _process_records(records, pid):
        if (record.get("event") == "original-game-detected"
                and re.search(r"(?:^|\s)source=adjacent(?:\s|$)",
                              str(record.get("detail", "")))):
            return record
    return None


def _detail_int(detail: str, key: str) -> Optional[int]:
    match = re.search(rf"(?:^|\s){re.escape(key)}=(-?\d+)(?:\s|$)", detail)
    return int(match.group(1)) if match else None


def host_evidence(records: list[dict], pid: int) -> Optional[dict]:
    """A listen host of a neutral shell: travel, exposure, possession, in order."""
    launched = _process_records(records, pid)
    for index, travel in enumerate(launched):
        detail = str(travel.get("detail", ""))
        route = re.search(r"(?:^|\s)route=(\S+)", detail)
        shell = SHELL_PACKAGE.fullmatch(route.group(1)) if route else None
        if travel.get("event") != "host-travel" or shell is None:
            continue
        leaf = shell.group(1)
        outcome = exposed = possessed = None
        for later_index, record in enumerate(launched[index + 1:], index + 1):
            event = record.get("event")
            later_detail = str(record.get("detail", ""))
            if (outcome is None and event == "host-travel-outcome"
                    and _detail_int(later_detail, "started") == 1):
                outcome = record
            # world-exposed fires as the shell world is exposed, before the
            # listen net driver sets its mode: it logs net=0 on every real
            # guarded host (Test and Shipping, measured 2026-10-05). The
            # listen proof is player-possess net=2 below.
            elif (exposed is None and event == "world-exposed"
                  and re.search(rf"(?:^|\s)package=/WoTNeutralShells/{leaf}(?:\s|$)",
                                later_detail)):
                exposed = (later_index, record)
            elif (exposed is not None and event == "player-possess"
                  and leaf in later_detail
                  and _detail_int(later_detail, "net")
                  == LISTEN_SERVER_NET_MODE):
                possessed = record
                break
        if outcome is not None and exposed is not None and possessed is not None:
            return {"shell": leaf, "host_travel": travel,
                    "host_travel_outcome": outcome,
                    "world_exposed": exposed[1], "player_possess": possessed}
    return None


def cache_evidence(state_root: Path) -> Optional[dict]:
    """One published ContentCache/current manifest."""
    manifests = funnel._manifests(state_root)
    if not manifests:
        return None
    return {
        "manifests": [funnel._snapshot_path(path) for path in manifests],
    }


# ---------------------------------------------------------------------------
# Process control
# ---------------------------------------------------------------------------

def launch_command(executable: Path, host_trigger: str) -> list[str]:
    """Always -unattended (the guarded boot-failure contract); visible when an
    operator must click Host, offscreen otherwise. No test switch exists."""
    command = [str(executable), "-unattended", "-ForceRes", "-windowed",
               "-NoSteam", "-nosplash", "-nocrashreports"]
    if host_trigger == "none":
        command += ["-RenderOffscreen", "-ResX=640", "-ResY=480"]
    else:
        command += ["-ResX=1280", "-ResY=720"]
    return command


def _terminate(process: subprocess.Popen) -> dict:
    signals = []
    if process.poll() is None:
        if os.name == "nt":
            signals.append(funnel._instance_tree_kill(process))
        else:
            os.killpg(process.pid, signal.SIGTERM)
            signals.append("SIGTERM")
    try:
        code = process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        signals.append(funnel._instance_tree_kill(process))
        code = process.wait(timeout=10)
    return {"signals": signals, "returncode": code}


def _wait(what: str, timeout: float, launcher: subprocess.Popen,
          game_process, game_pid: int,
          state_root: Path, heartbeat_root: Path, prelaunch_files: frozenset[Path],
          predicate: Callable[[list[dict]], Optional[dict]]) -> tuple[dict, list]:
    deadline = time.monotonic() + timeout
    records: list[dict] = []
    while time.monotonic() < deadline:
        failures = funnel._boot_failures(state_root)
        if failures:
            receipt = funnel._read_json(failures[0], "boot-failure receipt")
            raise SmokeFailure(
                f"Shipping client wrote a boot-failure receipt during {what}: "
                f"{receipt.get('diagnostic_code', failures[0])}")
        records = funnel._read_heartbeat_records(
            heartbeat_root, prelaunch_files, expected_pid=game_pid)
        failed = failure_event(records, game_pid)
        if failed is not None:
            raise SmokeFailure(
                f"Shipping client recorded {failed.get('event')!r} during "
                f"{what}: {failed.get('detail', '')}")
        evidence = predicate(records)
        if evidence is not None:
            return evidence, records
        if game_process.poll() is not None:
            raise SmokeFailure(
                f"Shipping game exited {game_process.returncode} during {what}")
        if launcher is not game_process and launcher.poll() is not None:
            raise SmokeFailure(
                f"Shipping launcher exited {launcher.returncode} while its game "
                f"was still running during {what}")
        time.sleep(0.25)
    raise SmokeFailure(f"timed out after {timeout:g}s waiting for {what}")


def _host_platform() -> Optional[str]:
    if sys.platform == "darwin":
        return "Mac"
    return "Win64" if os.name == "nt" else None


def _verify_mac_real_profile_unchanged(
        before: dict, checks: dict) -> None:
    """Record the post-run inventory and fail if any real root changed."""
    after = funnel._mac_real_support_inventory()
    unchanged = after == before
    checks["real_profile_after"] = after
    checks["real_profile_unchanged"] = unchanged
    if not unchanged:
        raise SmokeFailure(
            "SAFETY: Shipping smoke changed the developer's real WoT "
            "support roots")


def run_smoke(args: argparse.Namespace) -> dict:
    release_json = args.release.expanduser().resolve()
    manifest, asset = funnel._resolve_release(
        release_json, require_current_source=not args.allow_source_drift,
        configuration=SHIPPING)
    platform_name = manifest["platform"]
    if platform_name != _host_platform():
        raise SmokeFailure(
            f"a {platform_name} Shipping package must be smoked on {platform_name}")
    evidence_root = (args.evidence.expanduser().resolve() if args.evidence
                     else DEFAULT_EVIDENCE_ROOT / platform_name)
    evidence_root.mkdir(parents=True, exist_ok=True)
    receipt_path = evidence_root / "shipping-smoke-receipt.json"
    receipt: dict = {
        "format": SMOKE_FORMAT,
        "verdict": "fail",
        "started_utc": funnel._utc_now(),
        "platform": platform_name,
        "host_trigger": args.host_trigger,
        "release": {
            "manifest_path": str(release_json),
            "manifest_sha256": funnel._sha256_file(release_json),
            "asset_path": str(asset),
            "asset_sha256": manifest["sha256"],
            "asset_size": manifest["size"],
        },
        "checks": {},
    }
    scratch, remove_scratch = funnel._make_scratch_root(args.work_root)
    process = None
    game_process = None
    handles = []
    real_support_before = None
    try:
        receipt["checks"]["release_chain"] = validate_shipping_manifest(manifest)
        if args.funnel_receipt is not None:
            release = formats.load_guarded_release(release_json, platform_name)
            receipt["checks"]["funnel_twin"] = sign_release._check_funnel_receipts(
                {platform_name: release},
                {platform_name: args.funnel_receipt.expanduser().resolve()},
                False)[platform_name]
        installer = funnel._verify_installer(args.installer)
        receipt["installer"] = installer
        tree = funnel._extract_release(asset, scratch / "extracted", platform_name)
        game = resolve_shipping_image(tree, platform_name)
        seams = image_seam_report((game,))
        receipt["checks"]["image"] = {
            "game": str(game.relative_to(tree)),
            "seams": seams,
            "debug_files": debug_files(tree),
        }
        if any(row["test_seam_markers"] for row in seams.values()):
            raise SmokeFailure(f"Shipping image carries compiled test seams: {seams}")
        if receipt["checks"]["image"]["debug_files"]:
            raise SmokeFailure("Shipping tree stages debug files despite -NoDebugInfo")
        receipt["checks"]["adjacent_installer"] = str(
            funnel._copy_installer_adjacent(
                Path(installer["path"]), tree, platform_name))
        profile = scratch / "profile"
        funnel._require_empty_directory(profile, "Shipping smoke profile")
        if platform_name == "Mac":
            real_support_before = funnel._mac_real_support_inventory()
            receipt["checks"]["real_profile_before"] = real_support_before
        install_root = tree.parent if platform_name == "Win64" else None
        state_root = funnel._state_root(profile, platform_name, install_root)
        heartbeat_root, prelaunch_files = funnel._heartbeat_launch_boundary(state_root)
        launched_image = (tree.parent / "WoT Reborn.exe"
                          if platform_name == "Win64" else game)
        if not launched_image.is_file():
            raise SmokeFailure(
                f"Shipping launch image is missing: {launched_image}")
        command = launch_command(launched_image, args.host_trigger)
        for name in ("stdout", "stderr"):
            handles.append((evidence_root / f"client.{name}.log").open("wb"))
        started = time.monotonic()
        process = subprocess.Popen(
            command, cwd=str(launched_image.parent),
            env=funnel._profile_environment(profile, platform_name, install_root),
            stdout=handles[0], stderr=handles[1], start_new_session=True)
        game_process = (_discover_windows_game_child(
            process, game, args.boot_timeout)
            if platform_name == "Win64" else process)
        game_pid = game_process.pid
        receipt["process"] = {
            "pid": process.pid, "launcher_pid": process.pid,
            "game_pid": game_pid, "command": command,
            "state_root": str(state_root)}

        def booted(records: list[dict]) -> Optional[dict]:
            front = front_end_evidence(records, game_pid)
            cache = cache_evidence(state_root) if front else None
            adjacent = adjacent_original_game_evidence(records, game_pid)
            return ({"front_end": front, "cache": cache,
                     "original_game": adjacent}
                    if front and cache and adjacent else None)

        boot, records = _wait("the cache build and front end", args.boot_timeout,
                              process, game_process, game_pid,
                              state_root, heartbeat_root, prelaunch_files, booted)
        boot["seconds"] = round(time.monotonic() - started, 3)
        receipt["checks"]["boot"] = boot
        if args.host_trigger == "manual":
            print("SHIPPING SMOKE: the front end is up. In the WoT window click "
                  "Play, then Host Match, within "
                  f"{args.host_timeout:g}s.", flush=True)
            host, records = _wait(
                "a hosted neutral-shell map", args.host_timeout, process,
                game_process, game_pid, state_root, heartbeat_root, prelaunch_files,
                lambda current: host_evidence(current, game_pid))
            receipt["checks"]["host"] = host
        receipt["heartbeat"] = {
            "root": str(heartbeat_root),
            "prelaunch_files": sorted(str(path) for path in prelaunch_files),
            "records": _process_records(records, game_pid)}
        # On Mac the patched guarded engine and bootstrap share one user-data
        # root, so UE's Saved directory is a direct child of that root.
        receipt["checks"]["ue_saved_under_state_root"] = (
            state_root / "Saved").is_dir()
        receipt["verdict"] = "pass"
    except (SmokeFailure, funnel.FunnelFailure, formats.FormatError,
            sign_release.SignError, OSError) as exc:
        receipt["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if game_process is not None and game_process is not process:
            try:
                receipt["game_termination"] = _terminate(game_process)
            except BaseException as exc:  # never leave a client running
                receipt["verdict"] = "fail"
                receipt["game_termination"] = {
                    "error": f"{type(exc).__name__}: {exc}"}
        if process is not None:
            try:
                if game_process is not None and game_process is not process \
                        and "returncode" in receipt.get("game_termination", {}):
                    try:
                        launcher_code = process.wait(timeout=15)
                        receipt["termination"] = {
                            "signals": [], "returncode": launcher_code}
                    except subprocess.TimeoutExpired:
                        receipt["termination"] = _terminate(process)
                else:
                    receipt["termination"] = _terminate(process)
                if game_process is not None and game_process is not process \
                        and "returncode" in receipt.get("game_termination", {}):
                    game_code = receipt["game_termination"]["returncode"]
                    launcher_code = receipt["termination"]["returncode"]
                    receipt["checks"]["launcher_matches_game_exit"] = (
                        launcher_code == game_code)
                    if launcher_code != game_code:
                        raise SmokeFailure(
                            f"Shipping launcher exit {launcher_code} did not match "
                            f"game exit {game_code}")
            except BaseException as exc:  # never leave a launcher running
                receipt["verdict"] = "fail"
                receipt["termination"] = receipt.get(
                    "termination", {"error": f"{type(exc).__name__}: {exc}"})
                receipt["error"] = f"{type(exc).__name__}: {exc}"
        if game_process is not None and game_process is not process:
            game_process.close()
        for handle in handles:
            handle.close()
        if real_support_before is not None:
            try:
                _verify_mac_real_profile_unchanged(
                    real_support_before, receipt["checks"])
            except BaseException as exc:
                receipt["verdict"] = "fail"
                if isinstance(exc, SmokeFailure):
                    receipt["error"] = f"SmokeFailure: {exc}"
                else:
                    receipt["error"] = (
                        "SmokeFailure: SAFETY: could not inventory the "
                        "developer's real WoT support roots after the smoke: "
                        f"{exc}")
        if remove_scratch:
            shutil.rmtree(scratch, ignore_errors=True)
        receipt["completed_utc"] = funnel._utc_now()
        funnel._atomic_write_json(receipt_path, receipt)
    receipt["receipt_path"] = str(receipt_path)
    if receipt["verdict"] != "pass":
        raise SmokeFailure(receipt.get("error", "Shipping smoke failed"))
    return receipt


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--release", type=Path, required=True,
                        help="guarded Shipping release.json (.stripped-pak/release-shipping)")
    parser.add_argument("--installer", type=Path, required=True,
                        help="the pin-valid 333b installer; copied adjacent (no network)")
    parser.add_argument("--funnel-receipt", type=Path,
                        help="also prove the passing funnel run of the Test twin")
    parser.add_argument("--evidence", type=Path,
                        help="receipt directory (default Saved/ReleaseEvidence/"
                        "shipping-smoke/<P>)")
    parser.add_argument("--work-root", type=Path,
                        help="empty scratch root to retain (default: temp, removed)")
    parser.add_argument("--host-trigger", choices=("manual", "none"),
                        default="manual")
    parser.add_argument("--boot-timeout", type=float, default=7200.0,
                        help="cache build + front end (a cold build takes minutes)")
    parser.add_argument("--host-timeout", type=float, default=900.0)
    parser.add_argument("--allow-source-drift", action="store_true",
                        help="do not require the working tree to match the "
                        "release source snapshot")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    for name in ("boot_timeout", "host_timeout"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    try:
        receipt = run_smoke(args)
    except (SmokeFailure, funnel.FunnelFailure, formats.FormatError,
            sign_release.SignError) as exc:
        print(f"SHIPPING SMOKE FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"SHIPPING SMOKE PASS: {receipt['receipt_path']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
