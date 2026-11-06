#!/usr/bin/env python3
"""Resumable seven-stage tester bootstrap for WoT Reborn.

The bootstrap never executes the retail installer.  It downloads and verifies
the Inno Setup archive, extracts it with innoextract, regenerates the original
payload locally, imports through one UnrealEditor-Cmd Python session, and cooks
the client.  Stage stamps and per-item outputs make interrupted runs resumable.
"""
from __future__ import annotations

import argparse
import ast
import functools
import hashlib
import inspect
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Iterable, Optional, Sequence, Union


REPO = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO / "Tools"
IMPORT_DIR = REPO / "Tools" / "import"
CONVERT_DIR = REPO / "Tools" / "convert"
sys.path.insert(0, str(TOOLS_DIR))
sys.path.insert(0, str(IMPORT_DIR))
from wot_config import (ConfigurationError, devtree_dir, exports_dir,
                        installed_engine_root)  # noqa: E402
from import_all import (  # noqa: E402 - intentionally shared pipeline metadata
    IMPORT_STEP_SPECS,
    MP_MAPS,
    SYSTEM_SOUND_PACKAGES,
    _validate_mesh_outputs,
    mesh_mapping_carriers_requested,
)

STAGES = ("download", "unpack", "extract", "convert", "import", "cook", "assemble")
STAGE_NUM = {name: index for index, name in enumerate(STAGES, 1)}

ARCHIVE_ITEM = "twot_gog_classic"
ARCHIVE_FILE = "The Wheel Of Time [GOG]/setup_the_wheel_of_time_333b_(54846).exe"
INSTALLER_SIZE = 1_029_509_080
INSTALLER_MD5 = "0630c998ad0abc9539b161a532e05f67"
INSTALLER_SHA1 = "3a06f43e63bf2c73c3831864bccde59ca37f77da"
INSTALLER_SHA256 = "0ad6474ebc4b5d367b27197fd856733beb45179dc6d6c094641521b16b63f8bb"
INSTALLER_NAME = "setup_the_wheel_of_time_333b_(54846).exe"
EXPECTED_WAV_FILES = 1_196
KEEP_DIRS = ("System", "Maps", "Textures", "Sounds", "Music")
RETAIL_PACKAGE_COUNTS = {
    "System": (".u", 20),
    "Maps": (".wot", 45),
    "Textures": (".utx", 36),
    "Sounds": (".uax", 27),
    "Music": (".mp3", 10),
}

# ---------------------------------------------------------------------------
# INNOEXTRACT PIN BLOCK -- RELEASE OWNER MUST FILL BEFORE SHIPPING BOOTSTRAP.
# Set one audited version, immutable release-asset URL, and lowercase SHA-256
# per supported host.  Placeholder pins fail closed before downloaded bytes can
# be executed.  A PATH-installed innoextract remains usable for development.
# ---------------------------------------------------------------------------
# Pinned 2026-07-14 (assets downloaded + hashed by the release owner; upstream
# publishes no macOS binary — Darwin relies on PATH, e.g. `brew install
# innoextract`).
INNOEXTRACT_PIN_VERSION = "1.9"
INNOEXTRACT_PINS = {
    "Windows": {
        "url": "https://github.com/dscharrer/innoextract/releases/download/"
               "1.9/innoextract-1.9-windows.zip",
        "sha256": "6989342c9b026a00a72a38f23b62a8e6a22cc5de69805cf47d68ac2fec993065",
    },
    "Darwin": {
        # No upstream macOS release asset; fail closed to the PATH install.
        "url": "",
        "sha256": "",
    },
    "Linux": {
        "url": "https://github.com/dscharrer/innoextract/releases/download/"
               "1.9/innoextract-1.9-linux.tar.xz",
        "sha256": "008efe5011476ccc4aae17c3e22038b5a1bc5c7aad2b9d4d869537bf3874d21f",
    },
}
# ---------------------------------------------------------------------------


CONVERT_PHASES = (
    ("dev-inputs", "Copy checkout exports, or decode script and T3D from retail."),
    ("metadata", "Copy optional checkout inventory metadata into the work tree."),
    ("meshes", "Convert 243 meshes (retail packages by default; devtree = oracle) BEFORE textures so masked slots can request keyed variants."),
    ("textures", "Decode all retail Texture/System packages and make the neutral texture."),
    ("fonts", "Decode the WOT.u offline HUD font atlases/metrics."),
    ("flipbooks", "Bake all Fire/Wet/Wave/Ice procedural textures to sheets."),
    ("t3d", "Convert the 19 selected work-tree MP T3Ds to actor-semantics JSON."),
    ("class2mesh", "Build the placed-class to default-mesh lookup."),
    # zones MUST precede lightmaps: lightmap_atlas reads the zones
    # export's light_anim rows to bake the waveform/shimmer response atlases
    # (a clean-tree convert died in lightmaps when this ran first).
    ("zones", "Extract gameplay zone volumes from compiled BSP."),
    ("lightmaps", "Compose and atlas the retail lightmaps for every MP map."),
    ("sky", "Bake panorama and portal-rest sky records for every MP map."),
    ("skygeo", "Extract camera-locked child-sky geometry."),
    ("bsp", "Convert compiled BSP and portal geometry to GLB."),
    ("movers", "Convert mover brushes and event metadata."),
    ("fog", "Extract the leaf-baked volumetric fog-light set."),
    ("leaves", "Bake corona permeated-leaf visibility."),
)

# A completion marker is reusable only while both its converter implementation
# and this bootstrap's invocation/output contract are unchanged.  The source
# closure below captures constants declared by converter modules (including
# their local dependencies); bump the phase token when bootstrap-side arguments
# or output validation change without a corresponding converter-source edit.
CONVERTER_STAGE_SCHEMAS = {
    "dev-inputs-script-retail-python": "script-export-v1",
    "dev-inputs-t3d-retail-python": "level-export-v1",
    "dev-inputs-script-retail-ucc": "ucc-script-export-v1",
    "dev-inputs-t3d-retail-ucc": "ucc-level-export-v1",
    "textures": "texture-manifest-v1",
    "fonts": "font-manifest-v1",
    "flipbooks": "flipbook-manifest-v1",
    "t3d": "actor-semantics-v1",
    "class2mesh": "class-mesh-map-v1",
    "mesh-source": "retail-mesh-source-v1",
    "meshes-*": "mesh-glb-meta-v1",
    "lightmaps": "lightmap-atlas-v1",
    "sky-base": "sky-base-v1",
    "sky-rest": "sky-rest-v1",
    "skygeo": "sky-geometry-v1",
    "bsp": "bsp-geometry-v1",
    "movers": "mover-record-v1",
    "zones": "zone-record-v1",
    "fog": "fog-light-record-v1",
    "leaves": "leaf-visibility-v1",
}

CONVERTER_STAGE_ENTRYPOINTS = {
    "dev-inputs-script-retail-python": (CONVERT_DIR / "ue1pkg.py",),
    "dev-inputs-t3d-retail-python": (CONVERT_DIR / "ue1pkg.py",),
    # UCC performs the decode; bootstrap.py contains the local command and
    # publication logic whose identity we can bind.
    "dev-inputs-script-retail-ucc": (Path(__file__).resolve(),),
    "dev-inputs-t3d-retail-ucc": (Path(__file__).resolve(),),
    "textures": (CONVERT_DIR / "utx2png.py",),
    "fonts": (CONVERT_DIR / "ue1pkg.py",),
    "flipbooks": (CONVERT_DIR / "bake_flipbooks.py",),
    "t3d": (CONVERT_DIR / "t3d.py",),
    "class2mesh": (CONVERT_DIR / "class2mesh.py",),
    "mesh-source": (CONVERT_DIR / "ue1pkg.py",),
    "meshes-*": (CONVERT_DIR / "mesh2glb.py",),
    "lightmaps": (CONVERT_DIR / "lightmap_atlas.py",),
    "sky-base": (CONVERT_DIR / "skybake.py",),
    "sky-rest": (CONVERT_DIR / "skybake.py",),
    "skygeo": (CONVERT_DIR / "skygeo.py",),
    "bsp": (CONVERT_DIR / "bsp2obj.py",),
    "movers": (CONVERT_DIR / "movers2glb.py",),
    "zones": (CONVERT_DIR / "zones.py",),
    "fog": (CONVERT_DIR / "foglights.py",),
    "leaves": (CONVERT_DIR / "leafbake.py",),
}

HUD_FONT_NAMES = (
    "F_Charge", "F_Key", "F_KeySelected", "F_WOTReg14",
    "F_Charge_S", "F_Key_S", "F_KeySelected_S", "F_WOTReg14_S",
)

# UCC can export ScriptText from the 15 Legend/game packages.  The remaining
# five retail System packages contain stripped Epic engine ScriptText and must
# not be treated as export failures (the same rule as extract_all.sh).
SCRIPT_EXPORT_PACKAGES = (
    "Angreal", "Environment", "Fire", "IpServer", "Legend", "MachinShin",
    "ParticleSystems", "UBrowser", "UWindow", "WOT", "WoTBase",
    "WoTBrowser", "WoTDecorations", "WoTPawns", "WoTTraps",
)
STRIPPED_SCRIPT_PACKAGES = frozenset(("Core", "Editor", "Engine", "IpDrv", "WinDrv"))


class StageFailure(RuntimeError):
    def __init__(self, stage: str, message: str):
        super().__init__(f"stage '{stage}' failed: {message}")
        self.stage = stage


@dataclass
class Context:
    args: argparse.Namespace
    repo: Path
    work: Path
    original: Path
    conv: Path
    exports: Path
    stamps: Path
    markers: Path
    statuses: dict[str, str] = field(default_factory=dict)
    engine_root: Optional[Path] = None
    staged_path: Optional[Path] = None
    dev_input_source: Optional[str] = None


def log(stage: str, message: str) -> None:
    now = datetime.now().strftime("%H:%M:%S")
    print(f"[{now}] [{stage}] {message}", flush=True)


def archive_url() -> str:
    item = urllib.parse.quote(ARCHIVE_ITEM, safe="")
    filename = urllib.parse.quote(ARCHIVE_FILE, safe="/")
    return f"https://archive.org/download/{item}/{filename}"


def digest(path: Path, algorithm: str) -> str:
    h = hashlib.new(algorithm)
    with path.open("rb") as handle:
        while True:
            block = handle.read(8 * 1024 * 1024)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def installer_digests(path: Path) -> tuple[str, str, str]:
    """Compute every pinned installer digest in one 1 GB read."""
    md5 = hashlib.md5()
    sha1 = hashlib.sha1()
    sha256 = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(8 * 1024 * 1024)
            if not block:
                break
            md5.update(block)
            sha1.update(block)
            sha256.update(block)
    return md5.hexdigest(), sha1.hexdigest(), sha256.hexdigest()


def verify_installer(path: Path) -> None:
    if not path.is_file():
        raise ValueError(f"installer is missing: {path}")
    size = path.stat().st_size
    if size != INSTALLER_SIZE:
        raise ValueError(f"installer size {size:,} != expected {INSTALLER_SIZE:,}")
    md5, sha1, sha256 = installer_digests(path)
    if md5 != INSTALLER_MD5:
        raise ValueError(f"installer md5 {md5} != expected {INSTALLER_MD5}")
    if sha1 != INSTALLER_SHA1:
        raise ValueError(f"installer sha1 {sha1} != expected {INSTALLER_SHA1}")
    if sha256 != INSTALLER_SHA256:
        raise ValueError(
            f"installer sha256 {sha256} != expected {INSTALLER_SHA256}")


def _partial_path(destination: Path) -> Path:
    return destination.with_name(destination.name + ".part")


def _download_metadata_path(destination: Path) -> Path:
    return destination.with_name(destination.name + ".part.json")


def _remove_download_artifacts(destination: Path) -> None:
    """Remove a completed download and every resumable sidecar."""
    for path in (destination, _partial_path(destination),
                 _download_metadata_path(destination)):
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def _read_download_metadata(destination: Path, url: str) -> dict[str, str]:
    path = _download_metadata_path(destination)
    try:
        value = json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError, OSError, UnicodeError):
        return {}
    if not isinstance(value, dict) or value.get("url") != url:
        return {}
    result = {"url": url}
    result.update({key: value[key] for key in ("etag", "last_modified")
                   if isinstance(value.get(key), str) and value[key]})
    return result


def _write_download_metadata(destination: Path, url: str,
                             headers: object) -> None:
    get = getattr(headers, "get", lambda _key: None)
    value = {
        "url": url,
        "etag": get("ETag"),
        "last_modified": get("Last-Modified"),
    }
    path = _download_metadata_path(destination)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


# --- Resume decision ---------------------------------------------------------
#
# These decisions apply to original-installer acquisition only. Release updates
# always download from byte zero.

APPEND = "Append"
RESTART_FROM_ZERO = "RestartFromZero"
COMPLETE = "Complete"
REJECT = "Reject"
_MAX_UINT64 = (1 << 64) - 1


def _parse_content_range(value):
    """Parse exactly `bytes START-END/TOTAL`, with uint64 decimal fields."""
    match = re.fullmatch(r"bytes ([0-9]+)-([0-9]+)/([0-9]+)", value)
    if match is None:
        return None
    fields = tuple(int(field) for field in match.groups())
    if any(field > _MAX_UINT64 for field in fields):
        return None
    return fields


def decide_http_body(*, expected_size, partial_bytes, record_etag,
                     record_last_modified, status, content_range, etag,
                     last_modified, body_length):
    """Decide what to do with a response, from headers alone.

    Resuming is where a downloader quietly corrupts things: the server may ignore
    Range and send the whole object, or the origin content may have changed since
    the partial was written. Appending in either case yields a file of the right
    SIZE and the wrong CONTENT, and the hash check then fails long after the
    bandwidth was spent.
    """
    has_partial = partial_bytes > 0
    has_validator = bool(record_etag) or bool(record_last_modified)

    if expected_size == 0:
        return REJECT
    if partial_bytes > expected_size:
        return REJECT

    if status == 416:
        # The correct answer when the partial is already the entire object.
        if has_partial and partial_bytes == expected_size:
            return COMPLETE
        return REJECT

    if status == 200:
        # Range was ignored, or the validator changed. The body starts at zero, so
        # the old partial is DISCARDED rather than spliced onto.
        if body_length != 0 and body_length != expected_size:
            return REJECT
        return RESTART_FROM_ZERO

    if status == 206:
        if not has_partial:
            return REJECT
        if not has_validator:
            # Nothing ties these bytes to the ones already on disk. Restarting
            # costs bandwidth; appending would cost correctness.
            return REJECT
        parsed_range = _parse_content_range(content_range)
        if parsed_range is None:
            return REJECT
        range_start, range_end, range_total = parsed_range
        if (range_start != partial_bytes
                or range_total != expected_size
                or range_end != range_total - 1
                or range_end < range_start
                or (body_length != 0
                    and range_end - range_start + 1 != body_length)):
            return REJECT
        if etag and record_etag and etag != record_etag:
            return REJECT
        if last_modified and record_last_modified and last_modified != record_last_modified:
            return REJECT
        if body_length != 0 and partial_bytes + body_length != expected_size:
            return REJECT
        return APPEND

    return REJECT


def download_resumable(url: str, destination: Path,
                       expected_size: Optional[int] = None) -> Path:
    """Download via a .part file, resuming against the same origin object."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = _partial_path(destination)
    metadata_path = _download_metadata_path(destination)
    metadata = _read_download_metadata(destination, url)
    if partial.exists() and metadata_path.exists() and not metadata:
        # A sidecar for another URL (or unreadable state) cannot validate this
        # partial.  Restart rather than combining two origin objects.
        partial.unlink()
        metadata_path.unlink(missing_ok=True)
    offset = partial.stat().st_size if partial.exists() else 0
    headers = {"User-Agent": "wot-bootstrap/phase1"}
    if offset:
        headers["Range"] = f"bytes={offset}-"
        validator = metadata.get("etag") or metadata.get("last_modified")
        if validator:
            headers["If-Range"] = validator
    request = urllib.request.Request(url, headers=headers)
    try:
        response = urllib.request.urlopen(request, timeout=60)
    except urllib.error.HTTPError as exc:
        # A 416 arrives as an exception rather than a response, but it is the same
        # decision: the range was refused, which is correct exactly when the
        # partial already holds the whole object.
        if expected_size is not None and decide_http_body(
                expected_size=expected_size, partial_bytes=offset,
                record_etag=metadata.get("etag", ""),
                record_last_modified=metadata.get("last_modified", ""),
                status=exc.code, content_range="", etag="", last_modified="",
                body_length=0) == COMPLETE:
            partial.replace(destination)
            metadata_path.unlink(missing_ok=True)
            return destination
        raise

    status = getattr(response, "status", response.getcode())

    def _header(name):
        value = response.headers.get(name)
        return value if isinstance(value, str) else ""

    if expected_size is None:
        # The strict policy cannot run without a known size: there is no way to
        # tell a complete object from a truncated one, so `Complete` and the
        # would-not-reach-the-end check have no meaning. This path keeps the
        # older, weaker behavior and is used only for pins that carry no size.
        append = bool(offset and status == 206)
        parsed_range = _parse_content_range(_header("Content-Range")) if append else None
        try:
            body_length = int(_header("Content-Length") or 0)
        except ValueError:
            body_length = 0
        if append and (parsed_range is None
                       or parsed_range[0] != offset
                       or parsed_range[1] != parsed_range[2] - 1
                       or parsed_range[1] < parsed_range[0]
                       or (body_length != 0
                           and parsed_range[1] - parsed_range[0] + 1
                           != body_length)):
            response.close()
            partial.unlink(missing_ok=True)
            metadata_path.unlink(missing_ok=True)
            raise ValueError(
                f"resume response starts at the wrong offset: "
                f"{_header('Content-Range')!r}; discarded the partial download")
    else:
        try:
            body_length = int(_header("Content-Length") or 0)
        except ValueError:
            body_length = 0

        # Apply the original-installer acquisition policy.
        action = decide_http_body(
            expected_size=expected_size, partial_bytes=offset,
            record_etag=metadata.get("etag", ""),
            record_last_modified=metadata.get("last_modified", ""),
            status=status, content_range=_header("Content-Range"),
            etag=_header("ETag"), last_modified=_header("Last-Modified"),
            body_length=body_length)

        if action == REJECT:
            response.close()
            partial.unlink(missing_ok=True)
            metadata_path.unlink(missing_ok=True)
            raise ValueError(
                f"refusing the response: status {status}, content-range "
                f"{_header('Content-Range')!r}; discarded the partial download")
        if action == COMPLETE:
            response.close()
            partial.replace(destination)
            metadata_path.unlink(missing_ok=True)
            return destination
        append = action == APPEND

    if offset and not append:
        # A full body starts at zero, so the partial is truncated rather than
        # having a second read spliced onto it.
        offset = 0
    mode = "ab" if append else "wb"
    downloaded = offset
    last_report = time.monotonic()
    _write_download_metadata(destination, url, response.headers)
    with response, partial.open(mode) as handle:
        while True:
            block = response.read(4 * 1024 * 1024)
            if not block:
                break
            handle.write(block)
            downloaded += len(block)
            if time.monotonic() - last_report >= 10:
                suffix = f"/{expected_size:,}" if expected_size else ""
                print(f"  downloaded {downloaded:,}{suffix} bytes", flush=True)
                last_report = time.monotonic()
    if expected_size is not None and downloaded != expected_size:
        raise ValueError(
            f"download ended at {downloaded:,} bytes; expected {expected_size:,}; "
            f"rerun to resume {partial}")
    partial.replace(destination)
    metadata_path.unlink(missing_ok=True)
    return destination


def stamp_path(ctx: Context, stage: str) -> Path:
    return ctx.stamps / f"{STAGE_NUM[stage]:02d}-{stage}.json"


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def write_stamp(ctx: Context, stage: str, details: dict[str, object]) -> None:
    write_json(stamp_path(ctx, stage), {
        "stage": stage,
        "completed_utc": datetime.now(timezone.utc).isoformat(),
        **details,
    })


def _successful_command_stamp(ctx: Context, stage: str) -> bool:
    try:
        value = json.loads(stamp_path(ctx, stage).read_text())
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return False
    return (isinstance(value, dict) and value.get("stage") == stage
            and value.get("command_exit_code") == 0)


# --only-stage is a repair/debugging escape hatch, so its inputs cannot rely on
# earlier runners having executed in this process.  Declare the durable evidence
# each isolated stage consumes; a supplied --from-original satisfies unpack.
STAGE_PREREQUISITES: dict[str, tuple[str, ...]] = {
    "download": (),
    "unpack": ("download-stamp",),
    "extract": ("unpack-stamp",),
    "convert": ("unpack-stamp",),
    "import": ("extract-stamp", "extract-manifest",
               "convert-stamp", "convert-manifest"),
    "cook": ("import-stamp",),
    "assemble": ("cook-stamp",),
}


def _prerequisite_path(ctx: Context, name: str) -> Path:
    if name.endswith("-stamp"):
        return stamp_path(ctx, name.removesuffix("-stamp"))
    if name == "extract-manifest":
        return ctx.work / "extract-manifest.json"
    if name == "convert-manifest":
        return ctx.work / "convert-manifest.json"
    raise AssertionError(f"unknown prerequisite declaration: {name}")


def validate_stage_prerequisites(ctx: Context, stage: str) -> None:
    missing: list[str] = []
    for name in STAGE_PREREQUISITES[stage]:
        if name == "unpack-stamp" and ctx.args.from_original:
            continue
        path = _prerequisite_path(ctx, name)
        command_stage = name.removesuffix("-stamp")
        if (name in {"import-stamp", "cook-stamp"}
                and not _successful_command_stamp(ctx, command_stage)):
            missing.append(f"{name} with command_exit_code=0 ({path})")
        elif not path.is_file():
            missing.append(f"{name} ({path})")
    if missing:
        raise StageFailure(
            stage,
            "--only-stage is missing prerequisite(s): " + ", ".join(missing)
            + "; run the prerequisite stage(s) first or pass "
              "--allow-missing-prereqs")


def parse_named(values: Iterable[str], known: Sequence[str], kind: str) -> set[str]:
    aliases = {name: name for name in known}
    aliases.update({str(index): name for index, name in enumerate(known, 1)})
    result: set[str] = set()
    unknown: list[str] = []
    for raw in values:
        for value in raw.split(","):
            key = value.strip().lower().replace("_", "-")
            if not key:
                continue
            if key not in aliases:
                unknown.append(value)
            else:
                result.add(aliases[key])
    if unknown:
        raise ValueError(
            f"unknown {kind}(s): {', '.join(unknown)}; choices: {', '.join(known)}")
    return result


def ensure_original_tree(root: Path) -> None:
    problems: list[str] = []
    for name in KEEP_DIRS:
        directory = root / name
        if not directory.is_dir():
            problems.append(f"missing {name}/")
            continue
        files = [path for path in directory.iterdir() if path.is_file()]
        if not files:
            problems.append(f"{name}/ is empty")
            continue
        suffix, expected = RETAIL_PACKAGE_COUNTS[name]
        actual = sum(path.suffix.lower() == suffix for path in files)
        if actual != expected:
            problems.append(
                f"{name}/ has {actual} {suffix} file(s), expected {expected}")
    if problems:
        raise ValueError(f"retail tree {root} is incomplete: {', '.join(problems)}")


PathArgument = Union[os.PathLike[str], str]


def run_command(stage: str, command: Sequence[PathArgument],
                *, cwd: Path = REPO, env: Optional[dict[str, str]] = None,
                quiet: bool = False, timeout: Optional[float] = None) -> None:
    rendered = subprocess.list2cmdline([str(item) for item in command])
    log(stage, f"RUN {rendered}")
    try:
        subprocess.run(
            [str(item) for item in command], cwd=str(cwd), check=True, env=env,
            stdout=subprocess.DEVNULL if quiet else None,
            stderr=subprocess.DEVNULL if quiet else None,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise StageFailure(
            stage, f"command exceeded {timeout:.0f}s and was killed: {rendered}") from exc
    except subprocess.CalledProcessError as exc:
        raise StageFailure(stage, f"command exited {exc.returncode}: {rendered}") from exc


def selected_convert_phases(ctx: Context) -> set[str]:
    names = tuple(name for name, _ in CONVERT_PHASES)
    only = parse_named(ctx.args.convert_only, names, "conversion phase")
    skip = parse_named(ctx.args.convert_skip, names, "conversion phase")
    return (set(names) if not only else only) - skip


def print_plan(ctx: Context, selected_stages: set[str]) -> None:
    print("WoT tester bootstrap plan (dry run; no network, engine probe, or writes)")
    print(f"  repo:       {ctx.repo}")
    print(f"  work dir:   {ctx.work}")
    print(f"  retail:     {ctx.original}")
    print(f"  converted:  {ctx.conv}")
    print(f"  exports:    {ctx.exports}")
    print(f"  mesh source: {ctx.args.mesh_source}")
    print("  mesh GLBs:  " + ("stripped carriers enabled"
          if mesh_mapping_carriers_requested() else "canonical carrier-free"))
    print(f"  archive:    {archive_url()}")
    print(f"  installer:  {INSTALLER_SIZE:,} bytes; md5 {INSTALLER_MD5}; sha1 {INSTALLER_SHA1}")
    convert_selected = selected_convert_phases(ctx)
    for index, name in enumerate(STAGES, 1):
        state = "RUN"
        if name not in selected_stages:
            state = "FILTERED"
        elif ctx.args.from_original and name in {"download", "unpack"}:
            state = "PROVIDED"
        print(f"\n{index}. [{state}] {name}")
        if name == "download":
            if getattr(ctx.args, "installer", None) is not None:
                print(f"   verify provided --installer {installer_path(ctx)} (size/md5/sha1; never modified)")
            else:
                print(f"   {archive_url()} -> {ctx.work / INSTALLER_NAME} (Range resume + size/md5/sha1)")
        elif name == "unpack":
            print(f"   innoextract -> {ctx.work / 'original'}; retain {', '.join(KEEP_DIRS)}")
        elif name == "extract":
            print(f"   {len(SYSTEM_SOUND_PACKAGES)} System .u + retail Sounds/*.uax -> {ctx.exports / 'sounds'}")
        elif name == "convert":
            for phase, description in CONVERT_PHASES:
                phase_state = "RUN" if phase in convert_selected else "SKIP"
                print(f"   [{phase_state}] {phase}: {description}")
        elif name == "import":
            print("   one UnrealEditor-Cmd -run=pythonscript session: Tools/import/import_all.py")
            print("   ordered steps: " + " -> ".join(step.name for step in IMPORT_STEP_SPECS))
        elif name == "cook":
            print("   RunUAT BuildCookRun -platform=<Win64|Mac> -clientconfig=Development -build -cook -stage -pak -nop4 -utf8output -unattended (-NoBootstrapExe on Win64)")
        elif name == "assemble":
            print(f"   report staged client under {ctx.work / 'staged'}")
        prereqs = STAGE_PREREQUISITES[name]
        if prereqs:
            print(f"   isolated prerequisite evidence: {', '.join(prereqs)}")
    print(f"\nInnoextract PIN block: {Path(__file__).resolve()}:{INNO_PIN_LINE}")


def installer_path(ctx: Context) -> Path:
    """The verified installer's location: a user-provided file, else the work dir."""
    provided = getattr(ctx.args, "installer", None)
    if provided:
        return provided.expanduser().resolve()
    return ctx.work / INSTALLER_NAME


def stage_download(ctx: Context) -> str:
    installer = installer_path(ctx)
    user_provided = getattr(ctx.args, "installer", None) is not None
    if user_provided:
        # Never delete or overwrite a user-provided file; verify it in place.
        log("download", f"verifying provided {installer}")
        try:
            verify_installer(installer)
        except Exception as exc:
            raise StageFailure(
                "download",
                f"--installer file failed verification (left untouched): {exc}") from exc
        write_stamp(ctx, "download", {"path": str(installer), "provided": True})
        return "provided"
    if installer.exists():
        log("download", f"verifying existing {installer}")
        try:
            verify_installer(installer)
        except Exception as exc:
            _remove_download_artifacts(installer)
            raise StageFailure(
                "download", f"installer verification failed; removed bad download: {exc}") from exc
        write_stamp(ctx, "download", {"path": str(installer), "cached": True})
        return "cached"
    log("download", f"downloading {archive_url()}")
    try:
        download_resumable(archive_url(), installer, INSTALLER_SIZE)
    except Exception as exc:
        raise StageFailure("download", str(exc)) from exc
    try:
        verify_installer(installer)
    except Exception as exc:
        _remove_download_artifacts(installer)
        raise StageFailure(
            "download", f"installer verification failed; removed bad download: {exc}") from exc
    write_stamp(ctx, "download", {"path": str(installer), "cached": False})
    return "ran"


def _pin_is_filled(pin: dict[str, str]) -> bool:
    sha = pin.get("sha256", "")
    return (not INNOEXTRACT_PIN_VERSION.startswith("TODO")
            and not pin.get("url", "").startswith("TODO")
            and len(sha) == 64 and all(ch in "0123456789abcdef" for ch in sha))


def _safe_extract_archive(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    dest = destination.resolve()

    def safe(member: str) -> bool:
        candidate = (destination / member).resolve()
        try:
            candidate.relative_to(dest)
            return True
        except ValueError:
            return False

    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as handle:
            bad = [item.filename for item in handle.infolist() if not safe(item.filename)]
            if bad:
                raise ValueError(f"unsafe archive member: {bad[0]}")
            handle.extractall(destination)
    elif tarfile.is_tarfile(archive):
        with tarfile.open(archive) as handle:
            members = handle.getmembers()
            bad = [item.name for item in members if not safe(item.name)]
            if bad:
                raise ValueError(f"unsafe archive member: {bad[0]}")
            links = [item.name for item in members if item.issym() or item.islnk()]
            if links:
                raise ValueError(f"archive links are not allowed: {links[0]}")
            for item in members:
                if not item.linkname:
                    continue
                # Symlink targets are relative to the link's parent; hard-link
                # targets are archive-root relative.  Links are rejected above,
                # but retain this independent destination check as defense in depth.
                base = Path(item.name).parent if item.issym() else Path()
                if not safe(str(base / item.linkname)):
                    raise ValueError(
                        f"unsafe archive link target: {item.name} -> {item.linkname}")
            kwargs: dict[str, object] = {}
            if "filter" in inspect.signature(handle.extractall).parameters:
                kwargs["filter"] = "data"
            handle.extractall(destination, **kwargs)
    else:
        shutil.copy2(archive, destination / archive.name)


def _inno_candidates(tools: Path) -> list[Path]:
    return sorted(
        list(tools.rglob("innoextract")) + list(tools.rglob("innoextract.exe")),
        key=lambda path: str(path).lower(),
    )


def _verified_work_innoextract(tools: Path, pin: dict[str, str]) -> Optional[Path]:
    marker = tools / ".bootstrap-complete.json"
    try:
        value = json.loads(marker.read_text())
        relative = Path(value["executable"])
    except (FileNotFoundError, json.JSONDecodeError, KeyError, TypeError, OSError):
        return None
    if (value.get("version") != INNOEXTRACT_PIN_VERSION
            or value.get("url") != pin["url"]
            or value.get("archive_sha256") != pin["sha256"]
            or relative.is_absolute()):
        return None
    executable = (tools / relative).resolve()
    try:
        executable.relative_to(tools.resolve())
    except ValueError:
        return None
    return executable if executable.is_file() else None


def locate_innoextract(ctx: Context) -> Path:
    found = shutil.which("innoextract") or shutil.which("innoextract.exe")
    if found:
        return Path(found)
    host = platform.system()
    pin = INNOEXTRACT_PINS.get(host)
    if not pin or not _pin_is_filled(pin):
        raise StageFailure(
            "unpack",
            f"innoextract is not on PATH and the {host!r} PIN block at "
            f"{Path(__file__).resolve()}:{INNO_PIN_LINE} still contains TODO values")
    tools = ctx.work / "tools" / f"innoextract-{INNOEXTRACT_PIN_VERSION}-{host}"
    # This completion marker protects crash recovery, not a hostile concurrent
    # writer (TOCTOU): the bootstrap work directory is a single-user trust domain.
    cached = _verified_work_innoextract(tools, pin)
    if cached:
        return cached
    suffix = Path(urllib.parse.urlparse(pin["url"]).path).name or "innoextract-release"
    archive = ctx.work / "downloads" / suffix
    # An unmarked work-tree executable has no verification provenance.  Clear
    # it and fetch the pinned archive again rather than trusting or merging it.
    if tools.exists():
        shutil.rmtree(tools)
    # A final archive without the extraction marker is deliberately fetched
    # again.  Preserve an interrupted .part + validator sidecar so that fetch
    # can still resume safely.
    archive.unlink(missing_ok=True)
    try:
        download_resumable(pin["url"], archive)
        actual = digest(archive, "sha256")
        if actual != pin["sha256"]:
            _remove_download_artifacts(archive)
            raise StageFailure(
                "unpack", "innoextract archive verification failed; removed bad "
                f"download: sha256 {actual} != pinned {pin['sha256']}")
        _safe_extract_archive(archive, tools)
        candidates = _inno_candidates(tools)
        if not candidates:
            raise ValueError(f"pinned asset contains no innoextract executable: {archive}")
        executable = candidates[0]
        if host != "Windows":
            executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
        write_json(tools / ".bootstrap-complete.json", {
            "version": INNOEXTRACT_PIN_VERSION,
            "url": pin["url"],
            "archive_sha256": pin["sha256"],
            "executable": str(executable.relative_to(tools)),
        })
    except StageFailure:
        if tools.exists():
            shutil.rmtree(tools)
        raise
    except Exception as exc:
        if tools.exists():
            shutil.rmtree(tools)
        raise StageFailure("unpack", f"could not fetch pinned innoextract: {exc}") from exc
    return executable


def _case_child(root: Path, wanted: str) -> Optional[Path]:
    for child in root.iterdir():
        if child.is_dir() and child.name.lower() == wanted.lower():
            return child
    return None


def locate_game_tree(extracted: Path) -> Path:
    candidates: list[Path] = []
    for root, dirs, _files in os.walk(extracted):
        current = Path(root)
        names = {name.lower() for name in dirs}
        if {"system", "maps", "textures", "sounds", "music"}.issubset(names):
            candidates.append(current)
        # Installer archives are shallow; avoid walking redistributable forests.
        try:
            depth = len(current.relative_to(extracted).parts)
        except ValueError:
            depth = 99
        if depth >= 5:
            dirs[:] = []
    if not candidates:
        raise ValueError(f"could not locate retail System/Maps/Textures/Sounds/Music under {extracted}")
    return sorted(candidates, key=lambda path: (len(path.parts), str(path).lower()))[0]


def stage_unpack(ctx: Context) -> str:
    target = ctx.work / "original"
    try:
        ensure_original_tree(target)
        ctx.original = target
        write_stamp(ctx, "unpack", {"path": str(target), "cached": True})
        return "cached"
    except ValueError:
        pass
    installer = installer_path(ctx)
    try:
        verify_installer(installer)
    except Exception as exc:
        raise StageFailure("unpack", f"verified installer required first: {exc}") from exc
    tool = locate_innoextract(ctx)
    scratch = ctx.work / "inno-unpacked"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)
    run_command("unpack", [tool, "--extract", "--output-dir", scratch, installer])
    try:
        source = locate_game_tree(scratch)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        for name in KEEP_DIRS:
            actual = _case_child(source, name)
            if actual is None:
                raise ValueError(f"extracted tree is missing {name}")
            shutil.copytree(actual, target / name)
        ensure_original_tree(target)
    except Exception as exc:
        raise StageFailure("unpack", str(exc)) from exc
    finally:
        if scratch.exists():
            shutil.rmtree(scratch)
    ctx.original = target
    write_stamp(ctx, "unpack", {"path": str(target), "tool": str(tool), "cached": False})
    return "ran"


def sound_sources(original: Path) -> list[Path]:
    sources = sorted((original / "Sounds").glob("*.uax"), key=lambda p: p.name.lower())
    missing: list[Path] = []
    for name in SYSTEM_SOUND_PACKAGES:
        path = original / "System" / f"{name}.u"
        if path.is_file():
            sources.append(path)
        else:
            missing.append(path)
    if missing:
        raise ValueError(f"missing embedded-sound package: {missing[0]}")
    return sources


def stage_extract(ctx: Context) -> str:
    try:
        ensure_original_tree(ctx.original)
        sources = sound_sources(ctx.original)
    except Exception as exc:
        raise StageFailure("extract", str(exc)) from exc
    stamp_path(ctx, "extract").unlink(missing_ok=True)
    out = ctx.exports / "sounds"
    marker_dir = ctx.markers / "sounds"
    tool = CONVERT_DIR / "ue1pkg.py"
    ran = 0
    for index, source in enumerate(sources, 1):
        package_out = out / source.stem
        marker = marker_dir / f"{source.stem}.complete"
        # Some known embedded packages (for example Fire.u) legitimately
        # contain zero Sound exports.  The package marker records successful
        # decoding; the final 1,196-file census is the aggregate integrity gate.
        if marker.exists() and package_out.is_dir():
            log("extract", f"SKIP [{index}/{len(sources)}] {source.name}")
            continue
        package_out.mkdir(parents=True, exist_ok=True)
        run_command("extract", [sys.executable, tool, "sounds", source, package_out])
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text("complete\n")
        ran += 1
    wavs = list(out.glob("*/*.wav"))
    if len(wavs) != EXPECTED_WAV_FILES:
        raise StageFailure(
            "extract", f"sound census is {len(wavs)} WAV files; expected {EXPECTED_WAV_FILES}")
    manifest = ctx.work / "extract-manifest.json"
    write_json(manifest, {"packages": len(sources), "wav_files": len(wavs), "root": str(out)})
    write_stamp(ctx, "extract", {"manifest": str(manifest), "packages_run": ran})
    return "ran" if ran else "cached"


def _dev_input_scope(ctx: Context) -> tuple[tuple[str, ...], tuple[str, ...], bool]:
    def selected(values: Iterable[str], known: Sequence[str], kind: str) -> tuple[str, ...]:
        lookup = {name.lower(): name for name in known}
        requested: list[str] = []
        unknown: list[str] = []
        for raw in values:
            for value in raw.split(","):
                key = value.strip().lower()
                if not key:
                    continue
                if key not in lookup:
                    unknown.append(value)
                elif lookup[key] not in requested:
                    requested.append(lookup[key])
        if unknown:
            raise ValueError(
                f"unknown {kind}(s): {', '.join(unknown)}; choices: {', '.join(known)}")
        return tuple(requested) if requested else tuple(known)

    maps = selected(getattr(ctx.args, "dev_input_map", ()), MP_MAPS,
                    "dev-input map")
    packages = selected(getattr(ctx.args, "dev_input_package", ()),
                        SCRIPT_EXPORT_PACKAGES, "dev-input package")
    focused = bool(getattr(ctx.args, "dev_input_map", ())
                   or getattr(ctx.args, "dev_input_package", ()))
    return maps, packages, focused


def _dev_input_census(root: Path) -> dict[str, object]:
    script_counts = {
        package: len(list((root / "script" / package).glob("*.uc")))
        for package in SCRIPT_EXPORT_PACKAGES
    }
    t3d_maps = {
        name: bool((root / "t3d" / f"{name}.t3d").is_file()
                   and (root / "t3d" / f"{name}.t3d").stat().st_size)
        for name in MP_MAPS
    }
    return {
        "t3d_files": sum(t3d_maps.values()),
        "t3d_maps": t3d_maps,
        "script_files": sum(script_counts.values()),
        "script_packages": script_counts,
    }


def _validate_dev_input_census(census: dict[str, object], maps: Sequence[str],
                               packages: Sequence[str], source: str) -> None:
    t3d_maps = census.get("t3d_maps", {})
    script_packages = census.get("script_packages", {})
    missing_maps = [name for name in maps
                    if not isinstance(t3d_maps, dict) or not t3d_maps.get(name)]
    empty_packages = [name for name in packages
                      if (not isinstance(script_packages, dict)
                          or not isinstance(script_packages.get(name), int)
                          or script_packages[name] <= 0)]
    problems: list[str] = []
    if missing_maps:
        problems.append(f"missing/nonempty MP T3D: {missing_maps[0]}")
    if empty_packages:
        problems.append(f"zero .uc files for package: {empty_packages[0]}")
    if problems:
        raise StageFailure(
            "convert", f"{source} dev-input census failed: {', '.join(problems)}")


def _dev_input_source_state(ctx: Context) -> Path:
    return ctx.markers / "dev-inputs-source.json"


def _activate_dev_input_source(ctx: Context, source: str, script_exporter: str,
                               t3d_exporter: str) -> None:
    state = _dev_input_source_state(ctx)
    try:
        previous_state = json.loads(state.read_text())
        previous = (previous_state.get("source"),
                    previous_state.get("script_exporter"),
                    previous_state.get("t3d_exporter"))
    except (FileNotFoundError, json.JSONDecodeError, OSError, AttributeError):
        previous = None
    current = (source, script_exporter, t3d_exporter)
    if previous != current:
        # script/ and t3d/ are one source unit.  Clear both before populating
        # either, and invalidate retail markers whose paths could otherwise be
        # satisfied later by checkout files at the same destinations.
        for name in ("script", "t3d"):
            _rmtree(ctx.exports / name)
        for name in ("dev-inputs-script-retail-ucc",
                     "dev-inputs-script-retail-python",
                     "dev-inputs-t3d-retail-ucc",
                     "dev-inputs-t3d-retail-python"):
            _rmtree(ctx.markers / name)
    write_json(state, {
        "source": source,
        "script_exporter": script_exporter,
        "t3d_exporter": t3d_exporter,
        "status": "preparing",
    })


def _replace_tree(source: Path, destination: Path) -> None:
    temporary = destination.with_name(destination.name + ".tmp")
    _rmtree(temporary)
    shutil.copytree(source, temporary)
    _rmtree(destination)
    os.replace(temporary, destination)


def _retail_script_packages(ctx: Context) -> tuple[str, ...]:
    available = {path.stem.lower(): path.stem
                 for path in (ctx.original / "System").glob("*.u")}
    stripped = {name.lower() for name in STRIPPED_SCRIPT_PACKAGES}
    expected = {name.lower() for name in SCRIPT_EXPORT_PACKAGES}
    exportable = set(available) - stripped
    if exportable != expected:
        missing = sorted(expected - exportable)
        unexpected = sorted(exportable - expected)
        detail = []
        if missing:
            detail.append(f"missing exportable package {missing[0]}")
        if unexpected:
            detail.append(f"unexpected non-stripped package {unexpected[0]}")
        raise StageFailure(
            "convert", "retail System package set does not match the 15-package "
            "script export list after skipping stripped Core/Editor/Engine/"
            f"IpDrv/WinDrv: {', '.join(detail)}")
    return SCRIPT_EXPORT_PACKAGES


def _prepare_ucc_workspace(ctx: Context) -> tuple[Path, Path, list[PathArgument],
                                                  Path, str, dict[str, str]]:
    retail_ucc = ctx.original / "System" / "UCC.exe"
    if not retail_ucc.is_file():
        raise StageFailure(
            "convert", f"the explicitly selected UCC development fallback "
            f"requires {retail_ucc}")
    host = platform.system()
    wine = None if host == "Windows" else shutil.which("wine")
    if host != "Windows" and not wine:
        raise StageFailure(
            "convert", "the explicitly selected UCC development fallback "
            "requires wine on non-Windows hosts; use the default pure-Python "
            "exporters or install wine for this oracle run")

    # Retail UCC.exe busy-loops FOREVER at startup when its working-directory
    # path exceeds ~128 characters (UE1 path buffers; root-caused 2026-07-15:
    # the identical tree works at a 111-char cwd and hangs at 131, under wine
    # and by the same buffers natively). A developer's --work-dir can be
    # arbitrarily deep, so the opt-in disposable UCC oracle tree ALWAYS lives
    # in a short system-temp directory, never under the work dir.
    scratch = Path(tempfile.mkdtemp(prefix="wotucc-"))
    tree = scratch / "tree"
    tree.mkdir(parents=True)
    for name in KEEP_DIRS:
        shutil.copytree(ctx.original / name, tree / name)

    environment = os.environ.copy()
    environment.pop("DISPLAY", None)
    if host == "Windows":
        export_root = scratch / "native-exports"
        _rmtree(export_root)
        return (scratch, tree / "System", [tree / "System" / "UCC.exe"],
                export_root, "", environment)

    prefix = scratch / "wineprefix"
    # Prefix initialization can itself be interrupted.  It contains no durable
    # result (completed items already live under ctx.exports), so start clean
    # whenever any item still needs UCC.
    _rmtree(prefix)
    environment.update({
        "WINEPREFIX": str(prefix.resolve()),
        "WINEDEBUG": "-all",
    })
    export_root = prefix / "drive_c" / "dev-inputs"
    return (scratch, tree / "System", [str(wine), "UCC.exe"], export_root,
            r"C:\dev-inputs", environment)


def _ucc_output_argument(export_root: Path, wine_root: str,
                         *parts: str) -> str:
    if wine_root:
        return wine_root + "\\" + "\\".join(parts)
    return str(export_root.joinpath(*parts).resolve())


def _script_marker_phase(exporter: str) -> str:
    if exporter not in ("python", "ucc"):
        raise ValueError(f"unsupported script exporter: {exporter}")
    return f"dev-inputs-script-retail-{exporter}"


def _t3d_marker_phase(exporter: str) -> str:
    if exporter not in ("python", "ucc"):
        raise ValueError(f"unsupported T3D exporter: {exporter}")
    return f"dev-inputs-t3d-retail-{exporter}"


def _run_python_script_exports(ctx: Context, packages: Sequence[str]) -> int:
    ran = 0
    marker_phase = _script_marker_phase("python")
    for index, package in enumerate(packages, 1):
        destination = ctx.exports / "script" / package
        marker = _named_marker(ctx, marker_phase, package)
        if _completion_marker_valid(marker):
            log("convert", f"SKIP dev-inputs/script [{index}/{len(packages)}] {package}")
            continue
        marker.unlink(missing_ok=True)
        _rmtree(destination)
        scratch = Path(tempfile.mkdtemp(prefix="wotscript-"))
        try:
            generated = scratch / package
            try:
                _converter("convert", "ue1pkg.py", "script",
                           ctx.original / "System" / f"{package}.u", generated)
            except StageFailure as exc:
                raise StageFailure(
                    "convert", f"pure-Python script export failed for {package}; "
                    "fix the decoder/input or rerun explicitly with "
                    f"--script-exporter ucc: {exc}") from exc
            files = sorted(generated.glob("*.uc"), key=lambda path: path.name.lower())
            if not files:
                raise StageFailure(
                    "convert", f"Python script export for {package}.u wrote zero .uc files")
            _replace_tree(generated, destination)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
        outputs = sorted(destination.glob("*.uc"), key=lambda path: path.name.lower())
        _write_completion_marker(marker, outputs)
        log("convert", f"dev-inputs/script {package}: {len(outputs)} class(es) [python]")
        ran += 1
    return ran


def _run_python_t3d_exports(ctx: Context, maps: Sequence[str]) -> int:
    ran = 0
    marker_phase = _t3d_marker_phase("python")
    for index, name in enumerate(maps, 1):
        destination = ctx.exports / "t3d" / f"{name}.t3d"
        marker = _named_marker(ctx, marker_phase, name)
        if _completion_marker_valid(marker):
            log("convert", f"SKIP dev-inputs/t3d [{index}/{len(maps)}] {name}")
            continue
        marker.unlink(missing_ok=True)
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.name + ".tmp")
        temporary.unlink(missing_ok=True)
        try:
            _converter(
                "convert", "ue1pkg.py", "level",
                ctx.original / "Maps" / f"{name}.wot", temporary,
                "--system-dir", ctx.original / "System")
        except StageFailure as exc:
            temporary.unlink(missing_ok=True)
            raise StageFailure(
                "convert", f"pure-Python T3D export failed for {name}; fix "
                "the decoder/input or rerun explicitly with "
                f"--t3d-exporter ucc: {exc}") from exc
        if not temporary.is_file() or not temporary.stat().st_size:
            raise StageFailure(
                "convert", f"Python T3D export for {name}.wot wrote no output")
        os.replace(temporary, destination)
        _write_completion_marker(marker, [destination])
        log("convert", f"dev-inputs/t3d {name} [python]")
        ran += 1
    return ran


def _regenerate_dev_inputs(ctx: Context, maps: Sequence[str],
                           packages: Sequence[str], script_exporter: str,
                           t3d_exporter: str) -> int:
    all_packages = _retail_script_packages(ctx)
    if any(package not in all_packages for package in packages):
        raise AssertionError("dev-input package selection escaped the validated list")

    script_marker_phase = _script_marker_phase(script_exporter)
    pending_packages = [
        package for package in packages
        if not _completion_marker_valid(
            _named_marker(ctx, script_marker_phase, package))
    ]
    pending_maps = [
        name for name in maps
        if not _completion_marker_valid(
            _named_marker(ctx, _t3d_marker_phase(t3d_exporter), name))
    ]
    if not pending_packages and not pending_maps:
        return 0

    ran = 0
    if script_exporter == "python":
        ran += _run_python_script_exports(ctx, packages)
    if t3d_exporter == "python":
        ran += _run_python_t3d_exports(ctx, maps)

    ucc_packages = packages if script_exporter == "ucc" else ()
    ucc_maps = maps if t3d_exporter == "ucc" else ()
    if ((t3d_exporter == "ucc" and pending_maps)
            or (script_exporter == "ucc" and pending_packages)):
        scratch, system, command_prefix, export_root, wine_root, environment = \
            _prepare_ucc_workspace(ctx)
        try:
            ran += _run_ucc_exports(
                ctx, ucc_maps, ucc_packages, system, command_prefix,
                export_root, wine_root, environment)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
    return ran


def _run_ucc_exports(ctx, maps, packages, system, command_prefix,
                     export_root, wine_root, environment):
    ran = 0
    for index, package in enumerate(packages, 1):
        destination = ctx.exports / "script" / package
        marker = _named_marker(ctx, "dev-inputs-script-retail-ucc", package)
        if _completion_marker_valid(marker):
            log("convert", f"SKIP dev-inputs/script [{index}/{len(packages)}] {package}")
            continue
        marker.unlink(missing_ok=True)
        _rmtree(destination)
        ucc_output = export_root / "script" / package
        _rmtree(ucc_output)
        output_argument = _ucc_output_argument(
            export_root, wine_root, "script", package)
        run_command(
            "convert",
            [*command_prefix, "batchexport", f"{package}.u", "class", ".uc",
             output_argument],
            cwd=system, env=environment, quiet=True, timeout=900,
        )
        files = sorted(ucc_output.glob("*.uc"), key=lambda path: path.name.lower())
        if not files:
            raise StageFailure(
                "convert", f"UCC script export for {package}.u wrote zero .uc files")
        _replace_tree(ucc_output, destination)
        outputs = sorted(destination.glob("*.uc"), key=lambda path: path.name.lower())
        _write_completion_marker(marker, outputs)
        log("convert", f"dev-inputs/script {package}: {len(outputs)} class(es)")
        ran += 1

    t3d_output = export_root / "t3d"
    for index, name in enumerate(maps, 1):
        destination = ctx.exports / "t3d" / f"{name}.t3d"
        marker = _named_marker(ctx, _t3d_marker_phase("ucc"), name)
        if _completion_marker_valid(marker):
            log("convert", f"SKIP dev-inputs/t3d [{index}/{len(maps)}] {name}")
            continue
        marker.unlink(missing_ok=True)
        destination.unlink(missing_ok=True)
        my_level = t3d_output / "MyLevel.t3d"
        my_level.unlink(missing_ok=True)
        output_argument = _ucc_output_argument(export_root, wine_root, "t3d")
        run_command(
            "convert",
            [*command_prefix, "batchexport", f"{name}.wot", "level", "t3d",
             output_argument],
            cwd=system, env=environment, quiet=True, timeout=900,
        )
        if not my_level.is_file() or not my_level.stat().st_size:
            raise StageFailure(
                "convert", f"UCC T3D export for {name}.wot did not write "
                f"nonempty {my_level}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.name + ".tmp")
        os.replace(my_level, temporary)
        os.replace(temporary, destination)
        _write_completion_marker(marker, [destination])
        log("convert", f"dev-inputs/t3d {name}")
        ran += 1
    return ran


def prepare_dev_inputs(ctx: Context) -> int:
    maps, packages, focused = _dev_input_scope(ctx)
    reference_exports = exports_dir(required=False)
    checkout_script = reference_exports / "script"
    checkout_t3d = reference_exports / "t3d"
    checkout_available = checkout_script.is_dir() and checkout_t3d.is_dir()
    forced = bool(getattr(ctx.args, "regenerate_dev_inputs", False))
    source = "checkout" if checkout_available and not forced else "retail"
    script_exporter = ("checkout" if source == "checkout" else
                       getattr(ctx.args, "script_exporter", "python"))
    t3d_exporter = ("checkout" if source == "checkout" else
                    getattr(ctx.args, "t3d_exporter", "python"))
    _activate_dev_input_source(ctx, source, script_exporter, t3d_exporter)

    if source == "checkout":
        _replace_tree(checkout_script, ctx.exports / "script")
        _replace_tree(checkout_t3d, ctx.exports / "t3d")
        ran = 1
    else:
        ran = _regenerate_dev_inputs(
            ctx, maps, packages, script_exporter, t3d_exporter)

    census = _dev_input_census(ctx.exports)
    _validate_dev_input_census(census, maps, packages, source)
    manifest = ctx.work / "dev-inputs-manifest.json"
    write_json(manifest, {
        "source": source,
        "exporters": {"script": script_exporter, "t3d": t3d_exporter},
        "root": str(ctx.exports.resolve()),
        "scope": {"maps": list(maps), "script_packages": list(packages)},
        "partial": focused,
        "census": census,
    })
    write_json(_dev_input_source_state(ctx), {
        "source": source,
        "script_exporter": script_exporter,
        "t3d_exporter": t3d_exporter,
        "status": "complete",
        "manifest": str(manifest.resolve()),
    })
    ctx.dev_input_source = source
    return ran


def _copy_pipeline_metadata(ctx: Context) -> int:
    inventory = exports_dir(required=False) / "inventory"
    if inventory.is_dir():
        shutil.copytree(inventory, ctx.exports / "inventory", dirs_exist_ok=True)
        return 1
    return 0


def _converter(stage: str, script: str, *args: PathArgument) -> None:
    run_command(stage, [sys.executable, CONVERT_DIR / script, *args])


def _package_marker(ctx: Context, phase: str, source: Path) -> Path:
    token = hashlib.sha1(str(source.resolve()).encode()).hexdigest()[:12]
    return ctx.markers / phase / f"{source.stem}-{token}.complete"


def _named_marker(ctx: Context, phase: str, name: str) -> Path:
    safe_name = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in name)
    token = hashlib.sha1(name.encode()).hexdigest()[:12]
    return ctx.markers / phase / f"{safe_name}-{token}.complete"


def _local_python_module(module: str) -> Optional[Path]:
    """Resolve one imported module only when it belongs to this repository."""
    if not module:
        return None
    parts = module.split(".")
    for root in (CONVERT_DIR, IMPORT_DIR, TOOLS_DIR):
        source = root.joinpath(*parts).with_suffix(".py")
        if source.is_file():
            return source.resolve()
        package = root.joinpath(*parts, "__init__.py")
        if package.is_file():
            return package.resolve()
    return None


def _converter_source_closure(entrypoints: Sequence[Path]) -> tuple[Path, ...]:
    """Return the deterministic transitive repository-local import closure."""
    pending = [path.resolve() for path in entrypoints]
    visited: set[Path] = set()
    while pending:
        source = pending.pop()
        if source in visited:
            continue
        try:
            text = source.read_text(encoding="utf-8-sig")
            tree = ast.parse(text, filename=str(source))
        except (OSError, UnicodeError, SyntaxError) as exc:
            raise StageFailure(
                "convert", f"cannot identify converter source {source}: {exc}") from exc
        visited.add(source)
        modules: set[str] = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                modules.update(alias.name for alias in node.names)
            elif (isinstance(node, ast.ImportFrom) and node.level == 0
                  and node.module):
                modules.add(node.module)
        for module in sorted(modules, reverse=True):
            dependency = _local_python_module(module)
            if dependency is not None and dependency not in visited:
                pending.append(dependency)
    return tuple(sorted(visited, key=lambda path: str(path)))


def _converter_identity_digest(
        phase: str, sources: Sequence[Path], schema_version: str) -> str:
    """Hash a phase, its contract version, and the exact converter sources."""
    hasher = hashlib.sha256()
    for value in ("wot-converter-identity-v1", phase, schema_version):
        encoded = value.encode("utf-8")
        hasher.update(len(encoded).to_bytes(8, "big"))
        hasher.update(encoded)
    for source in sorted((path.resolve() for path in sources), key=str):
        try:
            label = source.relative_to(REPO).as_posix()
        except ValueError:
            label = str(source)
        try:
            payload = source.read_bytes()
        except OSError as exc:
            raise StageFailure(
                "convert", f"cannot hash converter source {source}: {exc}") from exc
        for value in (label.encode("utf-8"), payload):
            hasher.update(len(value).to_bytes(8, "big"))
            hasher.update(value)
    return hasher.hexdigest()


def _converter_stage_key(phase: str) -> str:
    if phase.startswith("meshes-"):
        return "meshes-*"
    if phase in CONVERTER_STAGE_SCHEMAS:
        return phase
    raise StageFailure("convert", f"no converter identity is defined for phase {phase!r}")


@functools.lru_cache(maxsize=None)
def _converter_identity_for_phase(phase: str) -> str:
    key = _converter_stage_key(phase)
    sources = _converter_source_closure(CONVERTER_STAGE_ENTRYPOINTS[key])
    return _converter_identity_digest(
        phase, sources, CONVERTER_STAGE_SCHEMAS[key])


def _completion_marker_valid(
        marker: Path, *, converter_identity: Optional[str] = None) -> bool:
    if converter_identity is None:
        converter_identity = _converter_identity_for_phase(marker.parent.name)
    try:
        value = json.loads(marker.read_text())
        outputs = value["outputs"]
    except (FileNotFoundError, json.JSONDecodeError, KeyError, TypeError, OSError):
        return False
    return (value.get("status") == "complete"
            and value.get("converter_identity_sha256") == converter_identity
            and isinstance(outputs, list)
            and all(isinstance(item, str) and Path(item).exists()
                    for item in outputs))


def _write_completion_marker(
        marker: Path, outputs: Sequence[Path], *,
        converter_identity: Optional[str] = None) -> None:
    if converter_identity is None:
        converter_identity = _converter_identity_for_phase(marker.parent.name)
    missing = [path for path in outputs if not path.exists()]
    if missing:
        raise StageFailure(
            "convert", f"converter exited successfully but did not write {missing[0]}")
    write_json(marker, {
        "status": "complete",
        "converter_identity_sha256": converter_identity,
        "outputs": [str(path.resolve()) for path in outputs],
    })


def _flipbook_source_omissions(manifest: Path) -> int:
    def unresolved_source(item: object) -> bool:
        return (isinstance(item, str)
                and ("unresolved/empty source" in item
                     or "unresolved/empty ice source or glass" in item
                     or "(ice source/glass:" in item
                     or ("(source:" in item and "parametric texture" not in item)))

    try:
        skipped = json.loads(manifest.read_text()).get("skipped", [])
    except (json.JSONDecodeError, OSError, AttributeError):
        return 1
    if not isinstance(skipped, list):
        return 1
    return sum(unresolved_source(item) for item in skipped)


def _mesh_key0_names(mesh_root: Path) -> set[str]:
    names: set[str] = set()
    for meta in mesh_root.glob("*.meta.json"):
        value = json.loads(meta.read_text())
        names.update(slot[:-2] for slot in value.get("slot_types", {})
                     if slot.endswith("_M"))
    return names


def _texture_manifest_current(manifest: Path, key0_names: set[str]) -> bool:
    """Reject pre-fix manifests/markers and missing derived PNG outputs."""
    try:
        textures = json.loads(manifest.read_text()).get("textures", {})
    except (FileNotFoundError, json.JSONDecodeError, OSError, AttributeError):
        return False
    wanted = {name.casefold() for name in key0_names}
    for name, info in textures.items():
        if (not isinstance(info, dict)
                or not isinstance(info.get("object_path"), str)
                or not isinstance(info.get("friction"), (int, float))):
            return False
        palette0 = info.get("palette0") if isinstance(info, dict) else None
        if (not isinstance(palette0, list) or len(palette0) != 3
                or any(type(channel) is not int or not 0 <= channel <= 255
                       for channel in palette0)):
            return False
        if name.casefold() in wanted:
            key0 = info.get("key0")
            if not isinstance(key0, str) or not (manifest.parent / key0).is_file():
                return False
    return True


def convert_textures(ctx: Context) -> int:
    packages = (sorted((ctx.original / "Textures").glob("*.utx"), key=lambda p: p.name.lower())
                + sorted((ctx.original / "System").glob("*.u"), key=lambda p: p.name.lower()))
    key0_names = _mesh_key0_names(ctx.conv / "mesh")
    if not key0_names:
        raise StageFailure(
            "convert", "mesh metadata has no masked-slot census; run the meshes phase first")
    ran = 0
    for index, package in enumerate(packages, 1):
        marker = _package_marker(ctx, "textures", package)
        manifest = ctx.conv / "tex" / package.stem / "_manifest.json"
        if (_completion_marker_valid(marker)
                and _texture_manifest_current(manifest, key0_names)):
            log("convert", f"SKIP textures [{index}/{len(packages)}] {package.name}")
            continue
        _converter("convert", "utx2png.py", ctx.conv / "tex", package,
                   "--mesh-meta-root", ctx.conv / "mesh")
        _write_completion_marker(marker, [manifest] if manifest.exists() else [])
        ran += 1
    neutral = ctx.conv / "tex" / "WOTDetail" / "T_WoT_Neutral.png"
    if not neutral.exists():
        try:
            from PIL import Image
        except ImportError as exc:
            raise StageFailure("convert", "Pillow is required by the existing texture converters") from exc
        neutral.parent.mkdir(parents=True, exist_ok=True)
        temporary = neutral.with_name(neutral.name + ".tmp")
        Image.new("RGBA", (1, 1), (128, 128, 128, 255)).save(
            temporary, format="PNG")
        os.replace(temporary, neutral)
        ran += 1
    return ran


def convert_fonts(ctx: Context) -> int:
    manifest = ctx.conv / "fonts" / "_manifest.json"
    marker = _package_marker(ctx, "fonts", ctx.original / "System" / "WOT.u")
    if _completion_marker_valid(marker):
        log("convert", "SKIP fonts (manifest exists)")
        return 0
    _converter("convert", "ue1pkg.py", "fonts", ctx.original / "System" / "WOT.u",
               ctx.conv / "fonts", *HUD_FONT_NAMES)
    _write_completion_marker(marker, [manifest])
    return 1


def convert_flipbooks(ctx: Context) -> int:
    packages = (sorted((ctx.original / "Textures").glob("*.utx"), key=lambda p: p.name.lower())
                + sorted((ctx.original / "System").glob("*.u"), key=lambda p: p.name.lower()))
    ran = 0
    for index, package in enumerate(packages, 1):
        marker = _package_marker(ctx, "flipbooks", package)
        if _completion_marker_valid(marker):
            log("convert", f"SKIP flipbooks [{index}/{len(packages)}] {package.name}")
            continue
        _converter("convert", "bake_flipbooks.py", ctx.conv / "flipbooks",
                   "--retail-root", ctx.original, package)
        manifest = ctx.conv / "flipbooks" / package.stem / "_flipbooks.json"
        if not manifest.exists() or not _flipbook_source_omissions(manifest):
            _write_completion_marker(marker, [manifest] if manifest.exists() else [])
        ran += 1
    return ran


def convert_t3d(ctx: Context) -> int:
    ran = 0
    for name in MP_MAPS:
        output = ctx.conv / f"{name}.t3d.json"
        source = ctx.exports / "t3d" / f"{name}.t3d"
        marker = _package_marker(ctx, "t3d", source)
        if _completion_marker_valid(marker):
            continue
        _converter("convert", "t3d.py", source, output)
        _write_completion_marker(marker, [output])
        ran += 1
    return ran


def convert_class2mesh(ctx: Context) -> int:
    output = ctx.conv / "class2mesh.json"
    marker = _named_marker(ctx, "class2mesh", "class2mesh")
    if _completion_marker_valid(marker):
        return 0
    _converter("convert", "class2mesh.py", output, ctx.exports / "script")
    mapping = json.loads(output.read_text())
    known = {name.casefold() for name in mesh_names(ctx)}
    unknown = sorted({name for name in mapping.values()
                      if name.casefold() not in known}, key=str.lower)
    if unknown:
        raise StageFailure(
            "convert", "class2mesh references meshes outside the #exec census: "
            + ", ".join(unknown))
    _write_completion_marker(marker, [output])
    return 1


def mesh_names(ctx: Context) -> list[str]:
    sys.path.insert(0, str(CONVERT_DIR))
    try:
        from ue3d import parse_exec_metadata
        metadata = parse_exec_metadata(ctx.exports / "script")
    finally:
        try:
            sys.path.remove(str(CONVERT_DIR))
        except ValueError:
            pass
    return sorted((item["name"] for item in metadata.values()), key=str.lower)


def _devtree_model_dirs(ctx: Context) -> list[Path]:
    root = devtree_dir(required=False) / "U2Radiosity"
    return sorted((path for path in root.glob("*/Models") if path.is_dir()),
                  key=lambda path: str(path).lower())


def _prepare_mesh_source(ctx: Context, names: Sequence[str]) -> tuple[str, Optional[Path]]:
    """Decode canonical retail meshes; retain the reference source as an oracle."""
    if getattr(ctx.args, "mesh_source", "retail-package") == "devtree":
        if not _devtree_model_dirs(ctx):
            raise StageFailure(
                "convert", f"no devtree Models directories under "
                f"{devtree_dir(required=False) / 'U2Radiosity'}")
        return "devtree", None

    output = ctx.work / "retail-meshes"
    manifest = output / "_manifest.json"
    outputs = ([manifest]
               + [output / f"{name}{suffix}"
                  for name in names for suffix in ("_d.3d", "_a.3d")])
    marker = _named_marker(ctx, "mesh-source", "retail-package")
    if not _completion_marker_valid(marker):
        packages = sorted((ctx.original / "System").glob("*.u"),
                          key=lambda path: path.name.lower())
        if not packages:
            raise StageFailure(
                "convert", f"no retail System packages under {ctx.original}")
        script_root = (ctx.exports / "script"
                       if (ctx.exports / "script").is_dir()
                       else exports_dir(required=False) / "script")
        _converter("convert", "ue1pkg.py", "meshes", *packages, output,
                   "--script-root", script_root)
        _write_completion_marker(marker, outputs)
    return "retail-package", output


def convert_meshes(ctx: Context) -> int:
    names = mesh_names(ctx)
    source, models_dir = _prepare_mesh_source(ctx, names)
    carriers = mesh_mapping_carriers_requested()
    carrier_mode = "carriers" if carriers else "canonical"
    ran = 0
    for index, name in enumerate(names, 1):
        glb = ctx.conv / "mesh" / f"{name}.glb"
        meta = ctx.conv / "mesh" / f"{name}.meta.json"
        # Skip only when the marker is valid AND the MTT typed slots are present
        # AND the carrier mode matches; any gap forces a rebuild.
        marker = _named_marker(ctx, f"meshes-{source}-{carrier_mode}", name)
        try:
            slot_types = json.loads(meta.read_text()).get("slot_types")
        except (FileNotFoundError, json.JSONDecodeError, OSError, AttributeError):
            slot_types = None
        if _completion_marker_valid(marker) and isinstance(slot_types, dict):
            try:
                _validate_mesh_outputs(
                    ctx.conv / "mesh", [name], source,
                    expected_carriers=carriers)
            except (FileNotFoundError, ValueError, OSError):
                log("convert", f"REBUILD meshes/{source} "
                    f"[{index}/{len(names)}] {name}: wrong carrier mode")
            else:
                log("convert", f"SKIP meshes/{source} "
                    f"[{index}/{len(names)}] {name}")
                continue
        arguments: list[PathArgument] = [name, ctx.conv / "mesh"]
        if models_dir is not None:
            arguments.append(models_dir)
        arguments.extend(("--source", source,
                          "--script-root", ctx.exports / "script"))
        if carriers:
            arguments.append("--carriers")
        _converter("convert", "mesh2glb.py", *arguments)
        _validate_mesh_outputs(
            ctx.conv / "mesh", [name], source,
            expected_carriers=carriers)
        _write_completion_marker(marker, [glb, meta])
        ran += 1
    return ran


def _map_source(ctx: Context, name: str) -> Path:
    return ctx.original / "Maps" / f"{name}.wot"


def convert_lightmaps(ctx: Context) -> int:
    ran = 0
    for name in MP_MAPS:
        output = ctx.conv / f"{name}_lmatlas.json"
        png = ctx.conv / f"{name}_lmatlas.png"
        marker = _named_marker(ctx, "lightmaps", name)
        if _completion_marker_valid(marker):
            continue
        _converter("convert", "lightmap_atlas.py", _map_source(ctx, name),
                   ctx.conv / f"{name}.t3d.json", ctx.conv)
        _write_completion_marker(marker, [output, png])
        ran += 1
    return ran


def convert_sky(ctx: Context) -> int:
    ran = 0
    for name in MP_MAPS:
        base = ctx.conv / f"{name}_sky.json"
        base_marker = _named_marker(ctx, "sky-base", name)
        if not _completion_marker_valid(base_marker):
            _converter("convert", "skybake.py", _map_source(ctx, name),
                       ctx.conv / f"{name}.t3d.json", ctx.conv)
            _write_completion_marker(base_marker, [base])
            ran += 1
        rest = ctx.conv / f"{name}_skyrest.json"
        rest_marker = _named_marker(ctx, "sky-rest", name)
        if not _completion_marker_valid(rest_marker):
            _converter("convert", "skybake.py", _map_source(ctx, name),
                       ctx.conv / f"{name}.t3d.json", ctx.conv, "--rest")
            _write_completion_marker(rest_marker, [rest])
            ran += 1
    return ran


def convert_skygeo(ctx: Context) -> int:
    ran = 0
    for name in MP_MAPS:
        output = ctx.conv / f"{name}_skygeo.json"
        marker = _named_marker(ctx, "skygeo", name)
        if _completion_marker_valid(marker):
            continue
        _converter("convert", "skygeo.py", _map_source(ctx, name), ctx.conv,
                   ctx.conv / "tex", ctx.conv / f"{name}_lmatlas.json",
                   ctx.conv / f"{name}.t3d.json")
        _write_completion_marker(marker, [output])
        ran += 1
    return ran


def convert_bsp(ctx: Context) -> int:
    ran = 0
    for name in MP_MAPS:
        document = ctx.conv / f"{name}.bsp.json"
        glb = ctx.conv / f"{name}.glb"
        marker = _named_marker(ctx, "bsp", name)
        if _completion_marker_valid(marker):
            continue
        _converter("convert", "bsp2obj.py", _map_source(ctx, name), ctx.conv,
                   ctx.conv / "tex", ctx.conv / f"{name}_lmatlas.json")
        _write_completion_marker(marker, [document, glb])
        ran += 1
    return ran


def convert_movers(ctx: Context) -> int:
    ran = 0
    for name in MP_MAPS:
        out = ctx.conv / "movers" / name
        document = out / "movers.json"
        marker = _named_marker(ctx, "movers", name)
        if _completion_marker_valid(marker):
            continue
        _converter("convert", "movers2glb.py", _map_source(ctx, name),
                   ctx.conv / f"{name}.t3d.json", out, ctx.conv / "tex")
        _write_completion_marker(marker, [document])
        ran += 1
    return ran


def _zone_export_has_walk_surfaces(output: Path, manifest_root: Path) -> bool:
    """Reject schema-stale (pre-G02 / pre-G07) or texture-stale exports despite
    a completion marker."""
    try:
        data = json.loads(output.read_text(encoding="utf-8"))
        output_mtime = output.stat().st_mtime_ns
    except (FileNotFoundError, json.JSONDecodeError, OSError, AttributeError):
        return False
    ideal = data.get("ideal_player_count")
    if (not isinstance(data, dict)
            or not isinstance(data.get("check_walk_surfaces"), bool)
            or not isinstance(data.get("friction_nodes"), list)
            or not isinstance(ideal, int) or isinstance(ideal, bool)):
        return False
    # zones.py joins BSP surfaces to texture manifests. This dependency check
    # handles a zones-only run made against legacy manifests: once textures are
    # regenerated with Friction/object_path, their newer timestamp invalidates
    # the otherwise schema-current empty zone result.
    try:
        manifests = list(manifest_root.glob("*/_manifest.json"))
        return bool(manifests) and all(
            manifest.stat().st_mtime_ns <= output_mtime
            for manifest in manifests)
    except OSError:
        return False


def convert_zones(ctx: Context) -> int:
    ran = 0
    for name in MP_MAPS:
        output = ctx.exports / "zones" / f"{name}.zones.json"
        marker = _named_marker(ctx, "zones", name)
        if (_completion_marker_valid(marker)
                and _zone_export_has_walk_surfaces(output, ctx.conv / "tex")):
            continue
        output.parent.mkdir(parents=True, exist_ok=True)
        _converter("convert", "zones.py", _map_source(ctx, name), output,
                   ctx.conv / "tex")
        _write_completion_marker(marker, [output])
        ran += 1
    return ran


def convert_fog(ctx: Context) -> int:
    ran = 0
    for name in MP_MAPS:
        output = ctx.exports / "fog" / f"{name}.fog.json"
        marker = _named_marker(ctx, "fog", name)
        if _completion_marker_valid(marker):
            continue
        output.parent.mkdir(parents=True, exist_ok=True)
        _converter("convert", "foglights.py", _map_source(ctx, name),
                   ctx.conv / f"{name}.t3d.json", output)
        _write_completion_marker(marker, [output])
        ran += 1
    return ran


def convert_leaves(ctx: Context) -> int:
    ran = 0
    for name in MP_MAPS:
        output = ctx.exports / "leaves" / f"{name}.leaves.json"
        marker = _named_marker(ctx, "leaves", name)
        if _completion_marker_valid(marker):
            continue
        output.parent.mkdir(parents=True, exist_ok=True)
        _converter("convert", "leafbake.py", _map_source(ctx, name),
                   ctx.conv / f"{name}.t3d.json", output)
        _write_completion_marker(marker, [output])
        ran += 1
    return ran


CONVERTERS: dict[str, Callable[[Context], int]] = {
    "textures": convert_textures,
    "fonts": convert_fonts,
    "flipbooks": convert_flipbooks,
    "t3d": convert_t3d,
    "class2mesh": convert_class2mesh,
    "meshes": convert_meshes,
    "lightmaps": convert_lightmaps,
    "sky": convert_sky,
    "skygeo": convert_skygeo,
    "bsp": convert_bsp,
    "movers": convert_movers,
    "zones": convert_zones,
    "fog": convert_fog,
    "leaves": convert_leaves,
}


def _mesh_source_census(ctx: Context) -> dict[str, int]:
    sources: dict[str, int] = {}
    for manifest in (ctx.conv / "mesh").glob("*.meta.json"):
        try:
            source = json.loads(manifest.read_text()).get("source", "unrecorded")
        except (json.JSONDecodeError, OSError, AttributeError):
            source = "invalid-manifest"
        source = str(source)
        sources[source] = sources.get(source, 0) + 1
    return dict(sorted(sources.items()))


def _recorded_dev_input_source(ctx: Context) -> Optional[str]:
    if ctx.dev_input_source:
        return ctx.dev_input_source
    try:
        value = json.loads((ctx.work / "dev-inputs-manifest.json").read_text())
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return None
    source = value.get("source") if isinstance(value, dict) else None
    return source if isinstance(source, str) else None


def _recorded_dev_input_exporters(ctx: Context) -> Optional[dict[str, str]]:
    try:
        value = json.loads((ctx.work / "dev-inputs-manifest.json").read_text())
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return None
    exporters = value.get("exporters") if isinstance(value, dict) else None
    if not isinstance(exporters, dict):
        return None
    if not all(isinstance(key, str) and isinstance(item, str)
               for key, item in exporters.items()):
        return None
    return dict(exporters)


def _conversion_census(ctx: Context) -> dict[str, object]:
    source_omissions = sum(
        _flipbook_source_omissions(manifest)
        for manifest in (ctx.conv / "flipbooks").glob("*/_flipbooks.json"))
    return {
        "texture_manifests": len(list((ctx.conv / "tex").glob("*/_manifest.json"))),
        "flipbook_manifests": len(list((ctx.conv / "flipbooks").glob("*/_flipbooks.json"))),
        "flipbook_source_omissions": source_omissions,
        "font_manifests": int((ctx.conv / "fonts" / "_manifest.json").exists()),
        "mesh_metadata": len(list((ctx.conv / "mesh").glob("*.meta.json"))),
        "mesh_sources": _mesh_source_census(ctx),
        "dev_inputs": _dev_input_census(ctx.exports),
        "t3d_json": sum((ctx.conv / f"{name}.t3d.json").exists() for name in MP_MAPS),
        "map_glbs": sum((ctx.conv / f"{name}.glb").exists() for name in MP_MAPS),
        "lightmap_atlases": sum((ctx.conv / f"{name}_lmatlas.json").exists() for name in MP_MAPS),
        "sky_records": sum((ctx.conv / f"{name}_sky.json").exists() for name in MP_MAPS),
        "skyrest_records": sum((ctx.conv / f"{name}_skyrest.json").exists() for name in MP_MAPS),
        "skygeo_records": sum((ctx.conv / f"{name}_skygeo.json").exists() for name in MP_MAPS),
        "mover_records": sum((ctx.conv / "movers" / name / "movers.json").exists()
                             for name in MP_MAPS),
        "zone_records": len(list((ctx.exports / "zones").glob("*.zones.json"))),
        "fog_records": len(list((ctx.exports / "fog").glob("*.fog.json"))),
        "leaf_records": len(list((ctx.exports / "leaves").glob("*.leaves.json"))),
    }


def stage_convert(ctx: Context) -> str:
    try:
        ensure_original_tree(ctx.original)
    except Exception as exc:
        raise StageFailure("convert", str(exc)) from exc
    # A repair run must not leave an earlier full-run stamp authoritative.  A
    # subsequent complete census recreates it after every selected item succeeds.
    stamp_path(ctx, "convert").unlink(missing_ok=True)
    ctx.conv.mkdir(parents=True, exist_ok=True)
    ctx.exports.mkdir(parents=True, exist_ok=True)
    selected = selected_convert_phases(ctx)
    ran: dict[str, int] = {}
    previous_exports = os.environ.get("WOT_EXPORTS_DIR")
    os.environ["WOT_EXPORTS_DIR"] = str(ctx.exports.resolve())
    try:
        for phase, _description in CONVERT_PHASES:
            if phase not in selected:
                log("convert", f"FILTER {phase}")
                continue
            log("convert", f"BEGIN {phase}")
            if phase == "dev-inputs":
                count = prepare_dev_inputs(ctx)
            elif phase == "metadata":
                count = _copy_pipeline_metadata(ctx)
            else:
                count = CONVERTERS[phase](ctx)
            ran[phase] = count
            log("convert", f"END {phase}: {count} item(s) executed")
    finally:
        if previous_exports is None:
            os.environ.pop("WOT_EXPORTS_DIR", None)
        else:
            os.environ["WOT_EXPORTS_DIR"] = previous_exports
    census = _conversion_census(ctx)
    partial = bool(ctx.args.convert_only or ctx.args.convert_skip)
    manifest = ctx.work / "convert-manifest.json"
    write_json(manifest, {"selected": sorted(selected), "executed": ran,
                          "census": census, "mesh_sources": census["mesh_sources"],
                          "dev_input_source": _recorded_dev_input_source(ctx),
                          "dev_input_exporters": _recorded_dev_input_exporters(ctx),
                          "partial": partial})
    if "flipbooks" in selected and census["flipbook_source_omissions"]:
        raise StageFailure(
            "convert", "incomplete conversion census: flipbook_source_omissions="
            f"{census['flipbook_source_omissions']} "
            "(cross-package sources must resolve)")
    if not partial:
        expected_meshes = len(mesh_names(ctx))
        required = {
            "font_manifests": 1,
            "mesh_metadata": expected_meshes,
            "t3d_json": len(MP_MAPS),
            "map_glbs": len(MP_MAPS),
            "lightmap_atlases": len(MP_MAPS),
            "sky_records": len(MP_MAPS),
            "skyrest_records": len(MP_MAPS),
            "skygeo_records": len(MP_MAPS),
            "mover_records": len(MP_MAPS),
            "zone_records": len(MP_MAPS),
            "fog_records": len(MP_MAPS),
            "leaf_records": len(MP_MAPS),
        }
        wrong = [f"{key}={census[key]} (expected {value})"
                 for key, value in required.items() if census[key] != value]
        if census["texture_manifests"] == 0:
            wrong.append("texture_manifests=0 (expected decoded retail textures)")
        if census["flipbook_manifests"] == 0:
            wrong.append("flipbook_manifests=0 (expected procedural texture sheets)")
        if sum(census["mesh_sources"].values()) != expected_meshes:
            wrong.append(
                f"mesh_sources={census['mesh_sources']} "
                f"(expected {expected_meshes} source-stamped meshes)")
        dev_inputs = census["dev_inputs"]
        if dev_inputs["t3d_files"] != len(MP_MAPS):
            wrong.append(
                f"dev_inputs.t3d_files={dev_inputs['t3d_files']} "
                f"(expected {len(MP_MAPS)})")
        empty_script_packages = [
            name for name, count in dev_inputs["script_packages"].items()
            if count <= 0
        ]
        if empty_script_packages:
            wrong.append(
                "dev_inputs script package has zero .uc files: "
                f"{empty_script_packages[0]} (expected all "
                f"{len(SCRIPT_EXPORT_PACKAGES)} packages nonzero)")
        for critical in (
                ctx.conv / "tex" / "Engine" / "DefaultTexture.png",
                ctx.conv / "tex" / "WOT" / "_manifest.json",
                ctx.conv / "tex" / "WoTBase" / "_manifest.json",
                ctx.conv / "tex" / "Angreal" / "_manifest.json",
                ctx.conv / "tex" / "WOTDetail" / "T_WoT_Neutral.png"):
            if not critical.exists():
                wrong.append(f"missing critical converted input {critical}")
        if wrong:
            raise StageFailure("convert", "incomplete conversion census: " + ", ".join(wrong))
        write_stamp(ctx, "convert", {"manifest": str(manifest), "census": census})
    return "partial" if partial else ("ran" if any(ran.values()) else "cached")


def engine_executable(root: Path) -> Optional[Path]:
    if root.is_file():
        return root
    host = platform.system()
    relative = {
        "Windows": Path("Engine/Binaries/Win64/UnrealEditor-Cmd.exe"),
        "Darwin": Path("Engine/Binaries/Mac/UnrealEditor-Cmd"),
        "Linux": Path("Engine/Binaries/Linux/UnrealEditor-Cmd"),
    }.get(host)
    if relative is None:
        return None
    candidate = root / relative
    return candidate if candidate.is_file() else None


def find_engine(ctx: Context, stage: str = "import") -> tuple[Path, Path]:
    candidates: list[Path] = []
    if ctx.args.engine:
        candidates.append(ctx.args.engine.expanduser())
    platform_name = {"Darwin": "Mac", "Windows": "Win64"}.get(
        platform.system())
    if platform_name:
        try:
            configured = installed_engine_root(platform_name, required=False)
        except ConfigurationError as exc:
            raise StageFailure(stage, str(exc)) from exc
        if configured is not None:
            candidates.append(configured)
    for candidate in candidates:
        executable = engine_executable(candidate)
        if executable:
            root = candidate
            if candidate.is_file():
                # <root>/Engine/Binaries/<Platform>/UnrealEditor-Cmd
                try:
                    root = candidate.parents[3]
                except IndexError:
                    root = candidate.parent
            return root.resolve(), executable.resolve()
    rendered = ", ".join(str(path) for path in candidates) or "no configured path"
    setting = (" or configure installed_engine_root_%s in Tools/local.json"
               % platform_name.lower()) if platform_name else ""
    raise StageFailure(stage, f"UE 5.8 UnrealEditor-Cmd not found; pass --engine{setting}. "
                       f"Probed: {rendered}")


def _unreal_script_argument(ctx: Context) -> str:
    def quoted(path: Path) -> str:
        return '"' + str(path.resolve()).replace('"', '\\"') + '"'
    return (f"-script={quoted(IMPORT_DIR / 'import_all.py')} "
            f"--conv-dir {quoted(ctx.conv)} "
            f"--exports-dir {quoted(ctx.exports)} "
            f"--retail-root {quoted(ctx.original)} "
            f"--mesh-source prepared")


def stage_import(ctx: Context) -> str:
    content = ctx.repo / "Content" / "WoT"
    stamp = stamp_path(ctx, "import")
    if (not ctx.args.force_import and _successful_command_stamp(ctx, "import")
            and content.is_dir()):
        return "cached"
    # Existing output without a successful command stamp is not proof that the
    # import finished; import_all may have failed after producing early assets.
    stamp.unlink(missing_ok=True)
    root, editor = find_engine(ctx)
    ctx.engine_root = root
    command = [
        editor, ctx.repo / "WoT.uproject", "-run=pythonscript",
        _unreal_script_argument(ctx), "-unattended", "-nullrhi", "-nosplash", "-stdout",
    ]
    run_command("import", command)
    if not content.is_dir():
        raise StageFailure("import", f"command completed but {content} was not created")
    write_stamp(ctx, "import", {
        "content": str(content), "engine": str(root), "command_exit_code": 0,
    })
    return "ran"


def runuat_path(engine_root: Path) -> Path:
    name = "RunUAT.bat" if platform.system() == "Windows" else "RunUAT.sh"
    path = engine_root / "Engine" / "Build" / "BatchFiles" / name
    if not path.is_file():
        raise StageFailure("cook", f"RunUAT not found: {path}")
    return path


def cook_platform() -> str:
    if platform.system() == "Windows":
        return "Win64"
    if platform.system() == "Darwin":
        return "Mac"
    raise StageFailure("cook", "phase-1 cook is supported on Windows (Win64) and macOS (Mac)")


def find_staged_output(ctx: Context) -> Optional[Path]:
    root = ctx.work / "staged"
    if not root.is_dir():
        return None
    children = [path for path in root.iterdir() if not path.name.startswith(".")]
    if not children:
        return None
    platform_name = "Windows" if platform.system() == "Windows" else "Mac"
    for name in (cook_platform(), platform_name):
        candidate = root / name
        if candidate.exists():
            return candidate
    return root


def stage_cook(ctx: Context) -> str:
    staged = find_staged_output(ctx)
    stamp = stamp_path(ctx, "cook")
    if _successful_command_stamp(ctx, "cook") and staged is not None:
        ctx.staged_path = staged
        return "cached"
    # A non-empty staging directory can be the residue of a failed BuildCookRun.
    # Invalidate prior evidence before starting; only exit 0 earns a new stamp.
    stamp.unlink(missing_ok=True)
    if ctx.engine_root is None:
        ctx.engine_root, _editor = find_engine(ctx, "cook")
    target = cook_platform()
    command = [
        runuat_path(ctx.engine_root), "BuildCookRun",
        f"-project={ctx.repo / 'WoT.uproject'}", f"-platform={target}",
        "-clientconfig=Development", "-build", "-cook", "-stage", "-pak",
        "-nop4", "-utf8output", "-unattended",
        f"-stagingdirectory={ctx.work / 'staged'}",
    ]
    if target == "Win64":
        command.append("-NoBootstrapExe")
    run_command("cook", command)
    staged = find_staged_output(ctx)
    if staged is None:
        raise StageFailure("cook", f"BuildCookRun completed but no staged output exists under {ctx.work / 'staged'}")
    ctx.staged_path = staged
    write_stamp(ctx, "cook", {"staged": str(staged), "engine": str(ctx.engine_root),
                              "platform": target, "command_exit_code": 0})
    return "ran"


def stage_assemble(ctx: Context) -> str:
    staged = ctx.staged_path or find_staged_output(ctx)
    if staged is None:
        raise StageFailure("assemble", "no cooked/staged client found; run the cook stage first")
    report = ctx.work / "ASSEMBLE.txt"
    report.write_text(
        "WoT phase-1 tester assembly\n"
        f"Staged client: {staged.resolve()}\n"
        "Local-cook assembly.\n"
    )
    log("assemble", f"staged client: {staged.resolve()}")
    write_stamp(ctx, "assemble", {"staged": str(staged.resolve()), "report": str(report),
                                  "mode": "local-cook"})
    return "ran"


STAGE_RUNNERS: dict[str, Callable[[Context], str]] = {
    "download": stage_download,
    "unpack": stage_unpack,
    "extract": stage_extract,
    "convert": stage_convert,
    "import": stage_import,
    "cook": stage_cook,
    "assemble": stage_assemble,
}


def print_summary(ctx: Context) -> None:
    print("\nBootstrap summary")
    for index, name in enumerate(STAGES, 1):
        print(f"  {index}. {name:<8} {ctx.statuses.get(name, 'not reached')}")
    if ctx.staged_path:
        print(f"  staged: {ctx.staged_path}")


def build_context(args: argparse.Namespace) -> Context:
    work = args.work_dir.expanduser().resolve()
    original = (args.from_original.expanduser().resolve()
                if args.from_original else work / "original")
    return Context(
        args=args,
        repo=REPO,
        work=work,
        original=original,
        conv=work / "converted",
        exports=work / "exports",
        stamps=work / ".stamps",
        markers=work / ".markers",
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--work-dir", type=Path, default=REPO / ".bootstrap",
                        help="downloads, generated exports, conversions, stamps, and staged client")
    result.add_argument("--from-original", type=Path,
                        help="use an existing extracted retail tree and skip download/unpack")
    result.add_argument("--engine", type=Path,
                        help="UE_5.8 root or UnrealEditor-Cmd executable")
    result.add_argument("--dry-run", action="store_true",
                        help="print the resolved seven-stage plan without probing or writing")
    result.add_argument("--only-stage", action="append", default=[], metavar="STAGE",
                        help="run only a numbered/named stage (repeat or comma-separate)")
    result.add_argument("--skip-stage", action="append", default=[], metavar="STAGE",
                        help="skip a numbered/named stage (repeat or comma-separate)")
    result.add_argument("--convert-only", action="append", default=[], metavar="PHASE",
                        help="within stage 4, run only a conversion phase (verification/repair)")
    result.add_argument("--convert-skip", action="append", default=[], metavar="PHASE",
                        help="within stage 4, skip a conversion phase (verification/repair)")
    result.add_argument("--regenerate-dev-inputs", action="store_true",
                        help="force retail script/T3D regeneration even when "
                             "checkout exports/script and exports/t3d are available")
    result.add_argument(
        "--script-exporter", choices=("python", "ucc"), default="python",
        help="retail dev-input script exporter: pure Python by default; 'ucc' "
             "is the explicit development oracle/fallback")
    result.add_argument(
        "--t3d-exporter", choices=("python", "ucc"), default="python",
        help="retail dev-input T3D exporter: pure Python by default; 'ucc' is "
             "the explicit development oracle/fallback")
    result.add_argument(
        "--mesh-source", choices=("retail-package", "devtree"),
        default="retail-package",
        help="mesh conversion source: shipped retail packages by default; "
             "'devtree' is the later-source reference oracle")
    result.add_argument("--dev-input-map", action="append", default=[], metavar="MAP",
                        help="limit a forced dev-inputs verification/repair run "
                             "to one MP map (repeat or comma-separate)")
    result.add_argument("--dev-input-package", action="append", default=[],
                        metavar="PACKAGE",
                        help="limit a forced dev-inputs verification/repair run "
                             "to one script package (repeat or comma-separate)")
    result.add_argument("--installer", type=Path, metavar="FILE",
                        help="use an existing installer file in place (verified, "
                             "never modified or deleted) instead of downloading")
    result.add_argument("--cleanup", choices=("ask", "keep", "downloads", "intermediates"),
                        default="ask",
                        help="after a fully successful run: 'ask' prompts on a "
                             "terminal (keeps everything otherwise), 'downloads' "
                             "removes the installer + fetched tools, "
                             "'intermediates' also removes the unpacked tree and "
                             "generated exports/conversions (the staged client is "
                             "always kept)")
    result.add_argument("--force-import", action="store_true",
                        help="run import_all even when a successful import stamp exists")
    result.add_argument("--allow-missing-prereqs", action="store_true",
                        help="override prerequisite evidence checks used with --only-stage")
    return result


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parser().parse_args(argv)
    ctx = build_context(args)
    try:
        only = parse_named(args.only_stage, STAGES, "stage")
        skip = parse_named(args.skip_stage, STAGES, "stage")
        selected = (set(STAGES) if not only else only) - skip
        # Parse early for useful CLI errors even when stage 4 is filtered.
        convert_selected = selected_convert_phases(ctx)
        _maps, _packages, focused_dev_inputs = _dev_input_scope(ctx)
    except ValueError as exc:
        parser().error(str(exc))
    if (args.convert_only or args.convert_skip) and not args.dry_run:
        if selected != {"convert"}:
            parser().error("--convert-only/--convert-skip require --only-stage convert (partial conversion is not safe to import/cook)")
    if focused_dev_inputs:
        if not args.regenerate_dev_inputs:
            parser().error(
                "--dev-input-map/--dev-input-package require --regenerate-dev-inputs")
        if selected != {"convert"} or convert_selected != {"dev-inputs"}:
            parser().error(
                "focused dev-input selection requires --only-stage convert "
                "--convert-only dev-inputs")
    if args.dry_run:
        print_plan(ctx, selected)
        return 0
    ctx.work.mkdir(parents=True, exist_ok=True)
    if args.from_original:
        try:
            ensure_original_tree(ctx.original)
        except ValueError as exc:
            raise SystemExit(f"bootstrap: {exc}")
    try:
        for stage in STAGES:
            if stage not in selected:
                ctx.statuses[stage] = "filtered"
                continue
            if args.from_original and stage in {"download", "unpack"}:
                log(stage, f"SKIP --from-original {ctx.original}")
                ctx.statuses[stage] = "provided"
                continue
            if args.only_stage and not args.allow_missing_prereqs:
                validate_stage_prerequisites(ctx, stage)
            log(stage, "BEGIN")
            ctx.statuses[stage] = STAGE_RUNNERS[stage](ctx)
            log(stage, f"END ({ctx.statuses[stage]})")
    except StageFailure as exc:
        ctx.statuses[exc.stage] = "FAILED"
        print(f"\nBOOTSTRAP FAILED: {exc}", file=sys.stderr)
        print_summary(ctx)
        return 1
    print_summary(ctx)
    maybe_cleanup(ctx)
    return 0


def _rmtree(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path, ignore_errors=True)


def _drop_stamps(ctx: Context, stages: Iterable[str]) -> None:
    for name in stages:
        try:
            (ctx.stamps / f"{name}.json").unlink()
        except FileNotFoundError:
            pass


def cleanup_downloads(ctx: Context) -> None:
    """Remove the ~1 GB installer and fetched tools; rerun redownloads them."""
    if getattr(ctx.args, "installer", None) is None:
        _remove_download_artifacts(ctx.work / INSTALLER_NAME)
        _drop_stamps(ctx, ("download",))
    else:
        log("cleanup", "keeping user-provided --installer file")
    _rmtree(ctx.work / "downloads")
    _rmtree(ctx.work / "tools")


def cleanup_intermediates(ctx: Context) -> None:
    """Also remove the unpacked tree and generated exports/conversions."""
    cleanup_downloads(ctx)
    if ctx.args.from_original is None:
        _rmtree(ctx.work / "original")
        _rmtree(ctx.work / "inno-unpacked")
        _drop_stamps(ctx, ("unpack",))
    else:
        log("cleanup", "keeping external --from-original tree")
    _rmtree(ctx.exports)
    _rmtree(ctx.conv)
    _rmtree(ctx.markers)
    _rmtree(ctx.work / "dev-inputs-ucc")
    (ctx.work / "dev-inputs-manifest.json").unlink(missing_ok=True)
    _drop_stamps(ctx, ("extract", "convert"))


def maybe_cleanup(ctx: Context) -> None:
    """Offer post-success cleanup. Only after a FULL run (no filtered stages) so
    partial/debug runs never lose the inputs the next invocation needs."""
    if any(ctx.statuses.get(name) in (None, "filtered", "FAILED") for name in STAGES):
        return
    choice = ctx.args.cleanup
    if choice == "ask":
        if not sys.stdin.isatty():
            log("cleanup", "non-interactive run: keeping everything "
                           "(use --cleanup downloads|intermediates to reclaim space)")
            return
        prompt = ("\nBootstrap complete. Clean up the work directory?\n"
                  "  [k] keep everything (default — fastest reruns)\n"
                  "  [d] remove downloads (~1 GB installer + fetched tools)\n"
                  "  [i] remove downloads + unpacked tree + generated exports/"
                  "conversions (staged client kept)\n> ")
        answer = input(prompt).strip().lower()
        choice = {"d": "downloads", "i": "intermediates"}.get(answer, "keep")
    if choice == "downloads":
        cleanup_downloads(ctx)
        log("cleanup", "downloads removed")
    elif choice == "intermediates":
        cleanup_intermediates(ctx)
        log("cleanup", "downloads + intermediates removed (staged client kept)")
    else:
        log("cleanup", "keeping everything")


# Stable source location reported by --dry-run and the final implementation
# report.  Defined after the block so line-number drift above it is harmless.
INNO_PIN_LINE = next(
    index for index, line in enumerate(Path(__file__).read_text().splitlines(), 1)
    if line.startswith("INNOEXTRACT_PIN_VERSION"))


if __name__ == "__main__":
    raise SystemExit(main())
