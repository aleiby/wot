#!/usr/bin/env python3
"""Fetch the reviewed innoextract library that a public checkout does not carry.

The public repository carries no Git LFS, so ThirdParty/innoextract/1.9/lib/
is empty there and UBT cannot link WoTRetailReader. The reviewed libraries are
published as assets of one GitHub PRERELEASE of this repository (a prerelease,
so it can never become /releases/latest, which the game reads for updates).

`fetch` downloads the asset named for the platform's vendor-manifest pin and
accepts it only if its SHA-256 equals that pin. The digest is the trust
boundary, not the URL: a wrong, stale or tampered download fails closed and
nothing is placed. Asset names carry the pin's first 12 hex digits, so a
re-promoted library is a new asset and older checkouts keep fetching theirs.

`stage-assets` is the maintainer side: it verifies the promoted libraries in
this checkout against their pins and copies them out under their asset names,
ready to upload to the vendor prerelease.

To build the library from source instead, see build_innoextract_native.py.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from typing import Callable, Optional, Sequence
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_innoextract_native as stage  # noqa: E402


REPO = Path(__file__).resolve().parents[2]
VENDOR = REPO / "ThirdParty" / "innoextract" / "1.9"
MANIFEST = VENDOR / "vendor-manifest.json"
RELEASE_TAG = "vendor-innoextract-1.9"
DOWNLOAD_BASE = f"https://github.com/aleiby/wot/releases/download/{RELEASE_TAG}/"
ASSET_STEMS = {
    "Win64": ("wot_innoextract-1.9-Win64-", ".lib"),
    "Mac": ("libwot_innoextract-1.9-Mac-", ".a"),
}
LIBRARY_PATHS = {
    "Win64": "lib/Win64/wot_innoextract.lib",
    "Mac": "lib/Mac/libwot_innoextract.a",
}
MAX_BYTES = 64 * 1024 * 1024
TIMEOUT_SECONDS = 60


class FetchError(RuntimeError):
    """The library could not be fetched or did not match its pin."""


def library_pin(platform: str, manifest_path: Path = MANIFEST) -> tuple[Path, str]:
    """The platform library's destination and its manifest digest.

    The destination is fixed per platform, never taken from the manifest: a
    manifest edit that kept a valid digest must not be able to aim the atomic
    replace anywhere else.
    """
    manifest = json.loads(manifest_path.read_bytes())
    record = manifest["artifacts"]["libraries"][platform]
    if record.get("path") != LIBRARY_PATHS[platform]:
        raise FetchError(
            f"vendor-manifest.json names {record.get('path')!r} for the {platform} "
            f"library; only {LIBRARY_PATHS[platform]} is accepted")
    digest = record.get("sha256")
    if not isinstance(digest, str) or len(digest) != 64:
        raise FetchError(f"vendor-manifest.json pins no {platform} library")
    vendor = manifest_path.resolve().parent
    destination = vendor / LIBRARY_PATHS[platform]
    # The fixed path has no traversal, but a linked lib/ or platform directory
    # could still lead outside the vendor tree (non-strict resolve follows any
    # existing link even when the leaf directory is not created yet).
    if not destination.parent.resolve().is_relative_to(vendor):
        raise FetchError(f"{destination.parent} leads outside {vendor}")
    return destination, digest


def asset_name(platform: str, digest: str) -> str:
    prefix, suffix = ASSET_STEMS[platform]
    return f"{prefix}{digest[:12]}{suffix}"


def _download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "wot-fetch-innoextract"})
    with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
        data = response.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise FetchError(f"{url} is larger than {MAX_BYTES} bytes")
    return data


def fetch(platform: str, *, manifest_path: Path = MANIFEST,
          download: Callable[[str], bytes] = _download, force: bool = False) -> Path:
    destination, digest = library_pin(platform, manifest_path)
    if not force and destination.is_file() and stage._sha256_file(destination) == digest:
        print(f"{platform} library already matches its pin: {destination}")
        return destination
    url = DOWNLOAD_BASE + asset_name(platform, digest)
    print(f"fetching {url}")
    try:
        data = download(url)
    except OSError as exc:  # urllib's URLError and HTTPError are OSErrors
        raise FetchError(f"could not download {url}: {exc}") from exc
    actual = hashlib.sha256(data).hexdigest()
    if actual != digest:
        raise FetchError(
            f"{url} has SHA-256 {actual}, but vendor-manifest.json pins {digest}; "
            "nothing was placed")
    destination.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=".fetch-", dir=destination.parent)
    try:
        with os.fdopen(handle, "wb") as output:
            output.write(data)
        os.replace(temporary, destination)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise
    print(f"placed {destination} ({len(data)} bytes, matches its pin)")
    return destination


def stage_assets(output: Path, *, manifest_path: Path = MANIFEST) -> list[Path]:
    output.mkdir(parents=True, exist_ok=True)
    staged = []
    for platform in ASSET_STEMS:
        stage.verify_library(platform, manifest_path)
        source, digest = library_pin(platform, manifest_path)
        target = output / asset_name(platform, digest)
        shutil.copyfile(source, target)
        staged.append(target)
        print(f"{target.name}  sha256 {digest}")
    return staged


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    fetch_parser = commands.add_parser("fetch", help="download and place a library")
    fetch_parser.add_argument("--platform", choices=tuple(ASSET_STEMS), required=True)
    fetch_parser.add_argument("--force", action="store_true",
                              help="download even if a matching library is in place")
    assets = commands.add_parser("stage-assets",
                                 help="maintainer: copy the pinned libraries out for upload")
    assets.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.command == "fetch":
            fetch(arguments.platform, force=arguments.force)
        else:
            stage_assets(arguments.output)
    except (FetchError, stage.VendorError, OSError, KeyError) as exc:
        print(f"fetch_innoextract_libs: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
