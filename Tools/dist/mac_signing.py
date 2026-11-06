#!/usr/bin/env python3
"""Sign, notarize, and staple macOS release artifacts.

All external commands go through a caller-supplied runner so the release
protocol can be tested without contacting Apple or invoking macOS tools.
"""
from __future__ import annotations

import json
import plistlib
import re
import subprocess
from pathlib import Path
from typing import Callable, List, Optional, Sequence


Runner = Callable[[Sequence[str]], subprocess.CompletedProcess]

MACH_O_MAGICS = {
    b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
}


class MacSigningError(RuntimeError):
    pass


def _run(runner: Runner, command: Sequence[str], what: str
         ) -> subprocess.CompletedProcess:
    result = runner(list(command))
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise MacSigningError(
            "%s failed (%s): %s" % (what, result.returncode, detail))
    return result


def mach_o_binaries(app: Path) -> list:
    """Return regular, non-symlink Mach-O files using their magic bytes."""
    app = Path(app)
    binaries = []
    for path in sorted(app.rglob("*")):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as handle:
                if handle.read(4) in MACH_O_MAGICS:
                    binaries.append(path)
    return binaries


def _main_executable(app: Path) -> Path:
    info_path = app / "Contents" / "Info.plist"
    try:
        with info_path.open("rb") as handle:
            leaf = plistlib.load(handle).get("CFBundleExecutable")
    except (OSError, plistlib.InvalidFileException) as exc:
        raise MacSigningError("cannot read app executable identity: %s" % exc) from exc
    if (not isinstance(leaf, str) or not leaf
            or Path(leaf).name != leaf or "/" in leaf or "\\" in leaf):
        raise MacSigningError("app has no safe CFBundleExecutable")
    executable = app / "Contents" / "MacOS" / leaf
    if not executable.is_file() or executable.is_symlink():
        raise MacSigningError("app main executable is missing: %s" % executable)
    return executable


def _team_id(app: Path, runner: Runner) -> Optional[str]:
    result = _run(
        runner, ["codesign", "-dv", "--verbose=4", str(app)],
        "codesign identity inspection")
    output = "%s\n%s" % (result.stdout or "", result.stderr or "")
    matches = re.findall(r"^TeamIdentifier=(.*)$", output, flags=re.MULTILINE)
    if not matches or matches[-1].strip() in {"", "not set"}:
        return None
    return matches[-1].strip()


def sign_app(app: Path, entitlements: Path, identity: Optional[str], runner: Runner
             ) -> dict:
    """Apply the final app signing pass, then deep/strict verify the bundle."""
    app = Path(app)
    entitlements = Path(entitlements)
    main = _main_executable(app)
    if identity is None:
        _run(runner, ["codesign", "--force", "--deep", "--sign", "-",
                      "--entitlements", str(entitlements), str(app)],
             "ad-hoc signing of app bundle")
        _run(runner, ["codesign", "--verify", "--deep", "--strict", str(app)],
             "codesign verification of app bundle")
        return {"signing": "adhoc", "team_id": None}

    # A nested signature must be established before its containing bundle.
    # Depth is primary; the path is a deterministic tie-breaker.
    nested = [path for path in mach_o_binaries(app) if path != main]
    nested.sort(key=lambda path: (-len(path.relative_to(app).parts),
                                  path.as_posix()))
    for binary in nested:
        _run(runner, ["codesign", "--force", "--timestamp", "--options",
                      "runtime", "--sign", identity, str(binary)],
             "Developer ID signing of %s" % binary.relative_to(app))
    _run(runner, ["codesign", "--force", "--timestamp", "--options", "runtime",
                  "--entitlements", str(entitlements), "--sign", identity,
                  str(app)], "Developer ID signing of app bundle")
    _run(runner, ["codesign", "--verify", "--deep", "--strict", "--verbose=2",
                  str(app)], "codesign verification of app bundle")
    team_id = _team_id(app, runner)
    if team_id is None:
        raise MacSigningError("Developer ID signature has no TeamIdentifier")
    return {"signing": "developer-id", "team_id": team_id}


def _json_output(result: subprocess.CompletedProcess, what: str) -> dict:
    candidates = [result.stdout, result.stderr]
    for candidate in candidates:
        if not candidate:
            continue
        try:
            value = json.loads(candidate)
        except (TypeError, ValueError):
            continue
        if isinstance(value, dict):
            return value
    raise MacSigningError("%s did not return a JSON object" % what)


def notary_auth_args(local_value: Callable[..., Optional[str]]
                     ) -> Optional[List[str]]:
    """notarytool credentials from machine-local config, or None.

    The App Store Connect API key file (mac_notary_key_path, mac_notary_key_id,
    mac_notary_issuer) is preferred: a notarytool keychain profile lives in the
    data-protection keychain, which is unreadable while the screen is locked,
    so an unattended package run would fail mid-way. mac_notary_profile is the
    interactive fallback.
    """
    key_path = local_value("mac_notary_key_path", required=False)
    key_id = local_value("mac_notary_key_id", required=False)
    issuer = local_value("mac_notary_issuer", required=False)
    if key_path or key_id or issuer:
        if not (key_path and key_id and issuer):
            raise MacSigningError(
                "mac_notary_key_path, mac_notary_key_id and mac_notary_issuer "
                "must all be set together")
        if not Path(key_path).expanduser().is_file():
            raise MacSigningError(
                "mac_notary_key_path does not name a readable API key file")
        return ["--key", str(Path(key_path).expanduser()), "--key-id", key_id,
                "--issuer", issuer]
    profile = local_value("mac_notary_profile", required=False)
    if profile:
        return ["--keychain-profile", profile]
    return None


def notarize(path: Path, auth: Sequence[str], runner: Runner, work: Path) -> dict:
    """Submit an app (as a ditto zip) or DMG and wait for notarization.

    ``auth`` is the notarytool credential arguments from notary_auth_args.
    """
    path = Path(path)
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    submission = path
    if path.suffix.casefold() != ".dmg":
        submission = work / (path.stem + "-app.zip")
        _run(runner, ["ditto", "-c", "-k", "--keepParent", str(path),
                      str(submission)], "notarization app archive creation")
    submit_command = [
        "xcrun", "notarytool", "submit", str(submission),
        *auth, "--wait", "--output-format", "json"]
    result = runner(submit_command)
    try:
        document = _json_output(result, "notarytool submission")
    except MacSigningError:
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "").strip()
            raise MacSigningError(
                "notarytool submission failed (%s): %s" %
                (result.returncode, detail))
        raise
    submission_id = document.get("id")
    status = document.get("status")
    if not isinstance(submission_id, str) or not submission_id:
        raise MacSigningError("notarytool submission has no id")
    if not isinstance(status, str) or not status:
        raise MacSigningError("notarytool submission has no status")
    outcome = {"submission_id": submission_id, "status": status}
    if status == "Accepted":
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "").strip()
            raise MacSigningError(
                "notarytool accepted submission but exited %s: %s" %
                (result.returncode, detail))
        return outcome

    log_path = work / "notary-log.json"
    log_result = _run(
        runner, ["xcrun", "notarytool", "log", submission_id,
                 *auth, str(log_path)],
        "notarytool rejection log")
    try:
        log = json.loads(log_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError):
        log = _json_output(log_result, "notarytool rejection log")
    issues = log.get("issues") if isinstance(log, dict) else None
    lines = []
    for issue in issues[:25] if isinstance(issues, list) else []:
        if isinstance(issue, dict):
            lines.append("%s | %s" % (issue.get("path", "<unknown>"),
                                      issue.get("message", "<no message>")))
    detail = "\n".join(lines) if lines else "notary log reported no issues"
    raise MacSigningError(
        "notarization %s (%s) was %s:\n%s" %
        (submission_id, path, status, detail))


def staple(path: Path, runner: Runner) -> None:
    path = Path(path)
    _run(runner, ["xcrun", "stapler", "staple", str(path)], "ticket stapling")
    _run(runner, ["xcrun", "stapler", "validate", str(path)],
         "stapled ticket validation")
