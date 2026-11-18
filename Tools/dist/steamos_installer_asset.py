#!/usr/bin/env python3
"""Build ``Install-WoT-Reborn.desktop`` exactly as a given commit defines it.

The SteamOS installer (``Tools/deck/installer``) is an unsigned release asset
that holds nothing release-specific, so every publication attaches a fresh
copy: publish_release.py builds it at the release's build commit, and
``verify_release.py --published`` rebuilds it there and compares the
downloaded bytes. It is never part of the signed set. A release whose build
commit predates the installer (``has_source`` is false) carries none.

The installer folder is read from git objects, never from the worktree, and
that commit's own ``build_desktop_entry.py`` assembles the asset (stdlib only:
the artwork is committed, and no keys are involved). Raw blobs are read, so a
checkout's line-ending conversion cannot change the bytes.

Stdlib only; runs on the Mac host's Python 3.9.
"""
from __future__ import annotations

import io
import re
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_formats as formats  # noqa: E402

ASSET_NAME = formats.STEAMOS_INSTALLER_ASSET
SOURCE_DIRECTORY = "Tools/deck/installer"
BUILDER = "build_desktop_entry.py"
BUILD_TIMEOUT_SECONDS = 300
_COMMIT = re.compile(r"[0-9a-f]{7,40}")


class SteamOSInstallerError(formats.FormatError):
    """The asset could not be built at the requested commit."""


def _git(repository: Path, arguments: Sequence[str],
         input_bytes: Optional[bytes] = None) -> bytes:
    try:
        completed = subprocess.run(
            ["git", "-C", str(repository), *arguments], input=input_bytes,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as exc:
        raise SteamOSInstallerError(f"cannot run git: {exc}") from exc
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", "replace").strip()
        raise SteamOSInstallerError(
            f"git {arguments[0]} failed in {repository}: "
            f"{detail or 'exit status %d' % completed.returncode}")
    return completed.stdout


def _cat_blobs(repository: Path, oids: Sequence[str]) -> Dict[str, bytes]:
    unique = list(dict.fromkeys(oids))
    stream = io.BytesIO(_git(repository, ["cat-file", "--batch"],
                             "".join(oid + "\n" for oid in unique).encode("ascii")))
    blobs: Dict[str, bytes] = {}
    for expected in unique:
        header = stream.readline().rstrip(b"\n").decode("ascii", "replace").split(" ")
        if len(header) != 3 or header[0] != expected or header[1] != "blob":
            raise SteamOSInstallerError(f"git cat-file returned {' '.join(header)!r} "
                                        f"for {expected}")
        size = int(header[2])
        data = stream.read(size)
        if len(data) != size or stream.read(1) != b"\n":
            raise SteamOSInstallerError("git cat-file returned truncated output")
        blobs[expected] = data
    return blobs


def _require_commit(commit: str) -> None:
    if not isinstance(commit, str) or not _COMMIT.fullmatch(commit):
        raise SteamOSInstallerError(f"build commit {commit!r} is not a lowercase hex sha")


def has_source(repository: Path, commit: str) -> bool:
    """Whether ``commit`` carries the installer's builder, so its release has the asset.

    Releases built before the installer existed carry no asset; an unknown
    commit raises, so a missing commit never reads as "older".
    """
    _require_commit(commit)
    builder = f"{SOURCE_DIRECTORY}/{BUILDER}"
    listing = _git(Path(repository), ["ls-tree", "-r", "-z", commit, "--", builder])
    for raw in listing.split(b"\0"):
        metadata, _, raw_path = raw.partition(b"\t")
        if raw_path.decode("utf-8", "replace") == builder:
            mode, kind, _ = metadata.decode("ascii").split(" ")
            return kind == "blob" and mode in ("100644", "100755")
    return False


def source_files(repository: Path, commit: str) -> Dict[str, bytes]:
    """{path inside SOURCE_DIRECTORY: raw blob bytes} as committed at ``commit``."""
    _require_commit(commit)
    listing = _git(repository, ["ls-tree", "-r", "-z", commit, "--", SOURCE_DIRECTORY])
    entries: List[Tuple[str, str]] = []
    for raw in listing.split(b"\0"):
        if not raw:
            continue
        metadata, _, raw_path = raw.partition(b"\t")
        mode, kind, oid = metadata.decode("ascii").split(" ")
        path = PurePosixPath(raw_path.decode("utf-8"))
        try:
            relative = path.relative_to(SOURCE_DIRECTORY)
        except ValueError:
            raise SteamOSInstallerError(f"git listed {path} outside {SOURCE_DIRECTORY}")
        if kind != "blob" or mode not in ("100644", "100755"):
            raise SteamOSInstallerError(
                f"{path} at {commit} is not a regular file (mode {mode})")
        if any(part in ("", ".", "..") for part in relative.parts):
            raise SteamOSInstallerError(f"{path} at {commit} is not a plain path")
        entries.append((relative.as_posix(), oid))
    if BUILDER not in {name for name, _ in entries}:
        raise SteamOSInstallerError(
            f"commit {commit} has no {SOURCE_DIRECTORY}/{BUILDER}; it predates "
            f"{ASSET_NAME}")
    blobs = _cat_blobs(repository, [oid for _, oid in entries])
    return {name: blobs[oid] for name, oid in entries}


def build_at_commit(repository: Path, commit: str) -> bytes:
    """The asset bytes that ``commit``'s own builder makes from ``commit``'s files."""
    files = source_files(Path(repository), commit)
    with tempfile.TemporaryDirectory(prefix="wot-steamos-installer-") as scratch:
        source = Path(scratch) / "installer"
        for name, data in sorted(files.items()):
            path = source.joinpath(*name.split("/"))
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        output = Path(scratch) / ASSET_NAME
        command = [sys.executable, "-I", "-B", str(source / BUILDER), "--out", str(output)]
        try:
            completed = subprocess.run(command, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE,
                                       timeout=BUILD_TIMEOUT_SECONDS)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise SteamOSInstallerError(
                f"{BUILDER} at {commit} did not run: {exc}") from exc
        if completed.returncode != 0 or not output.is_file():
            detail = (completed.stderr or completed.stdout).decode("utf-8", "replace")
            raise SteamOSInstallerError(
                f"{BUILDER} at {commit} failed (exit status {completed.returncode}): "
                f"{detail.strip()}")
        data = output.read_bytes()
    if not data.startswith(b"[Desktop Entry]\n"):
        raise SteamOSInstallerError(f"{BUILDER} at {commit} wrote no desktop entry")
    return data
