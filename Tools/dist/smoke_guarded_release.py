#!/usr/bin/env python3
"""Run the bounded first-network-decision smoke for a guarded Mac release.

Success is intentionally narrow: the packaged client must stay alive until
the installer acquisition publishes its OnResponseStart-judged ``.part.json``
sidecar, or until the first non-zero partial byte is observable.  The client
is then terminated immediately so this check never downloads the full pinned
installer.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path


SMOKE_FORMAT = "wot-guarded-network-smoke-v1"
SIDECAR_FORMAT = "wot-installer-download-resume-v1"
BOOT_FAILURE_LEAF = "boot-failure-v1.json"


class SmokeFailure(RuntimeError):
    """The packaged release failed the bounded smoke contract."""


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_json(path: Path, what: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SmokeFailure(f"cannot read {what} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise SmokeFailure(f"{what} is not a JSON object: {path}")
    return value


def _atomic_write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(
        f".{path.name}.{os.getpid()}.{time.monotonic_ns()}.tmp")
    data = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    descriptor = os.open(
        temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
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


def _run_capture(command: list[str]) -> dict:
    result = subprocess.run(command, capture_output=True, text=True)
    return {
        "command": command,
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def _resolve_release(release_json: Path) -> tuple[dict, Path]:
    manifest = _read_json(release_json, "guarded release manifest")
    if (manifest.get("format") != "wot-guarded-release-v1"
            or manifest.get("platform") != "Mac"
            or manifest.get("configuration") != "Test"):
        raise SmokeFailure("smoke requires a guarded Mac Test release manifest")
    asset_leaf = manifest.get("asset")
    if (not isinstance(asset_leaf, str) or not asset_leaf
            or Path(asset_leaf).name != asset_leaf):
        raise SmokeFailure("release manifest asset path is unsafe")
    asset = release_json.parent / asset_leaf
    if not asset.is_file():
        raise SmokeFailure(f"guarded release asset is missing: {asset}")
    if (manifest.get("size") != asset.stat().st_size
            or manifest.get("sha256") != _sha256_file(asset)):
        raise SmokeFailure("guarded release asset differs from release.json")
    return manifest, asset


def _extract_executable(asset: Path, extraction: Path) -> tuple[Path, Path]:
    result = subprocess.run(
        ["ditto", "-x", "-k", str(asset), str(extraction)],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SmokeFailure(f"cannot extract guarded app: {result.stderr.strip()}")
    app = extraction / "WoT.app"
    plist_path = app / "Contents" / "Info.plist"
    try:
        with plist_path.open("rb") as handle:
            executable_leaf = plistlib.load(handle).get("CFBundleExecutable")
    except (OSError, plistlib.InvalidFileException) as exc:
        raise SmokeFailure(f"cannot read packaged Info.plist: {exc}") from exc
    if (not isinstance(executable_leaf, str) or not executable_leaf
            or "/" in executable_leaf or "\\" in executable_leaf):
        raise SmokeFailure("packaged app has no safe CFBundleExecutable")
    executable = app / "Contents" / "MacOS" / executable_leaf
    if not executable.is_file():
        raise SmokeFailure(f"packaged game executable is missing: {executable}")
    return app, executable


def _sidecar_decision(path: Path) -> tuple[dict, dict]:
    parsed = _read_json(path, "installer response-start sidecar")
    required = {
        "format", "url", "expected_size", "expected_sha256",
        "etag", "last_modified",
    }
    if (set(parsed) != required
            or parsed.get("format") != SIDECAR_FORMAT
            or not isinstance(parsed.get("url"), str)
            or not isinstance(parsed.get("expected_size"), int)
            or parsed["expected_size"] <= 0
            or not isinstance(parsed.get("expected_sha256"), str)
            or len(parsed["expected_sha256"]) != 64
            or not isinstance(parsed.get("etag"), str)
            or not isinstance(parsed.get("last_modified"), str)):
        raise SmokeFailure("installer response-start sidecar is malformed")
    # The native sink publishes this sidecar only after DecideHttpBody accepts
    # Append/RestartFromZero and applies that decision to its held .part file.
    decision = {
        "response_start": "accepted",
        "accepted_actions": ["Append", "RestartFromZero"],
        "publication_point":
            "FWoTInstallerAcquisitionSink.OnResponseStart.after-decision",
        "validators": {
            "etag": parsed["etag"],
            "last_modified": parsed["last_modified"],
        },
    }
    return parsed, decision


def _observe(profile: Path) -> dict | None:
    boot_failures = sorted(profile.rglob(BOOT_FAILURE_LEAF))
    if boot_failures:
        path = boot_failures[0]
        return {
            "kind": "boot_failure",
            "path": str(path),
            "receipt": _read_json(path, "boot-failure receipt"),
            "sha256": _sha256_file(path),
        }
    sidecars = sorted(profile.rglob("*.part.json"))
    if sidecars:
        path = sidecars[0]
        parsed, decision = _sidecar_decision(path)
        part = path.with_suffix("")
        part_size = part.stat().st_size if part.is_file() else None
        return {
            "kind": "response_start_sidecar",
            "path": str(path),
            "sha256": _sha256_file(path),
            "parsed": parsed,
            "decision": decision,
            "part_path": str(part),
            "part_size": part_size,
        }
    parts = sorted(profile.rglob("*.part"))
    for part in parts:
        try:
            size = part.stat().st_size
        except OSError:
            continue
        if size > 0:
            return {
                "kind": "first_nonzero_partial",
                "part_path": str(part),
                "part_size": size,
            }
    return None


def _stat_observation(observation: dict | None) -> dict:
    paths = []
    if observation:
        for key in ("path", "part_path"):
            value = observation.get(key)
            if isinstance(value, str) and Path(value).exists():
                paths.append(value)
    if not paths:
        return {
            "command": ["/usr/bin/stat", "-f",
                        "%N|%z|%i|%m|%p|%l", "--"],
            "returncode": None,
            "stdout": "",
            "stderr": "",
        }
    return _run_capture([
        "/usr/bin/stat", "-f", "%N|%z|%i|%m|%p|%l", "--", *paths])


def _terminate(process: subprocess.Popen, reason: str) -> dict:
    signals = []
    started = time.monotonic()
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        signals.append("SIGTERM")
    timed_out = False
    try:
        returncode = process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(process.pid, signal.SIGKILL)
        signals.append("SIGKILL")
        returncode = process.wait(timeout=10)
    return {
        "reason": reason,
        "signals": signals,
        "termination_signal": signals[-1] if signals else None,
        "returncode": returncode,
        "timed_out_after_sigterm": timed_out,
        "wait_seconds": round(time.monotonic() - started, 6),
    }


def _make_scratch_root() -> Path:
    # macOS reports the per-user temporary directory through /var, which is an
    # intermediate symlink to /private/var. The guarded client deliberately
    # refuses symlink traversal on its executable tree; launch it from the
    # canonical physical spelling while retaining an isolated temp directory.
    return Path(
        tempfile.mkdtemp(prefix="wot-guarded-smoke-")).resolve(strict=True)


def run_smoke(release_json: Path, evidence: Path, timeout: float) -> dict:
    if sys.platform != "darwin":
        raise SmokeFailure("guarded release smoke must run on macOS")
    if timeout <= 0:
        raise SmokeFailure("--timeout must be positive")
    manifest, asset = _resolve_release(release_json.resolve())
    evidence = evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    stdout_path = evidence / "client.stdout.log"
    stderr_path = evidence / "client.stderr.log"
    started_utc = _utc_now()
    receipt: dict = {
        "format": SMOKE_FORMAT,
        "verdict": "fail",
        "started_utc": started_utc,
        "release": {
            "manifest_path": str(release_json.resolve()),
            "manifest_sha256": _sha256_file(release_json.resolve()),
            "asset_path": str(asset.resolve()),
            "asset_sha256": manifest["sha256"],
            "asset_size": manifest["size"],
        },
    }
    temporary_root = _make_scratch_root()
    extraction = temporary_root / "extracted"
    profile = temporary_root / "profile"
    extraction.mkdir()
    profile.mkdir()
    process = None
    stdout_handle = None
    stderr_handle = None
    observation = None
    try:
        _app, executable = _extract_executable(asset, extraction)
        command = [
            str(executable),
            "-unattended",
            "-RenderOffscreen",
            "-NoSteam",
            "-nosplash",
            "-windowed",
            "-ResX=640",
            "-ResY=480",
        ]
        environment = os.environ.copy()
        environment["HOME"] = str(profile)
        environment["CFFIXED_USER_HOME"] = str(profile)
        stdout_handle = stdout_path.open("wb")
        stderr_handle = stderr_path.open("wb")
        process = subprocess.Popen(
            command, cwd=str(executable.parent), env=environment,
            stdout=stdout_handle, stderr=stderr_handle, start_new_session=True)
        receipt["process"] = {
            "pid": process.pid,
            "executable": str(executable),
            "executable_size": executable.stat().st_size,
            "executable_sha256": _sha256_file(executable),
            "command": command,
            "ps_at_launch": _run_capture([
                "/bin/ps", "-p", str(process.pid),
                "-o", "pid=,ppid=,lstart=,command="]),
        }
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            observation = _observe(profile)
            if observation is not None:
                break
            returncode = process.poll()
            if returncode is not None:
                raise SmokeFailure(
                    f"packaged client exited {returncode} before response-start evidence")
            time.sleep(0.1)
        if observation is None:
            raise SmokeFailure(
                f"no sidecar or non-zero partial appeared within {timeout:g}s")
        receipt["observation"] = observation
        receipt["stat"] = _stat_observation(observation)
        if observation["kind"] == "boot_failure":
            raise SmokeFailure(
                "packaged client published a guarded boot-failure receipt")
        receipt["verdict"] = "pass"
    except BaseException as exc:
        receipt["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if process is not None:
            try:
                receipt["wait"] = _terminate(
                    process,
                    (observation or {}).get("kind", "failure-or-timeout"))
            except BaseException as exc:
                receipt["verdict"] = "fail"
                receipt["wait"] = {
                    "reason": "termination-failed",
                    "error": f"{type(exc).__name__}: {exc}",
                }
        if stdout_handle is not None:
            stdout_handle.close()
        if stderr_handle is not None:
            stderr_handle.close()
        try:
            shutil.rmtree(temporary_root, ignore_errors=False)
            cleanup_error = None
        except BaseException as exc:
            cleanup_error = f"{type(exc).__name__}: {exc}"
            receipt["verdict"] = "fail"
        if "stat" not in receipt:
            receipt["stat"] = _stat_observation(observation)
        receipt["scratch_profile_cleanup"] = {
            "path": str(profile),
            "removed": not profile.exists(),
            "error": cleanup_error,
        }
        receipt["completed_utc"] = _utc_now()
        _atomic_write_json(evidence / "smoke-receipt.json", receipt)
    if receipt["verdict"] != "pass":
        raise SmokeFailure(receipt.get("error", "guarded smoke failed"))
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--release", type=Path, required=True,
        help="path to guarded release.json")
    parser.add_argument(
        "--evidence", type=Path, required=True,
        help="directory for the atomic smoke receipt and process logs")
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    try:
        run_smoke(args.release, args.evidence, args.timeout)
    except SmokeFailure as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: {args.evidence / 'smoke-receipt.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
