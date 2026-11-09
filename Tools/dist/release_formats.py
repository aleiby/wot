#!/usr/bin/env python3
"""The release artifact formats, mirrored from the C++ that consumes them.

Every reader in this module is a line-for-line port of the native check it names,
so a publish set this module accepts is one the shipped game and updater accept,
and every writer emits the only byte spelling those readers allow. Citations are
to the native source of truth:

  canonical JSON   Source/WoTBootstrapCore/Public/WoTBootstrapCore/CanonicalJson.h:3-18
                   (== json.dumps(sort_keys=True, separators=(",", ":"),
                   ensure_ascii=True); int64 integers only; lower-case \\uXXXX)
  announcement     Source/WoTBootstrapCore/Private/Announcement.cpp:66-195
  key ring         Source/WoTBootstrapCore/Private/KeyRing.cpp:4-189
  release manifest Source/WoTBootstrapCore/Private/ReleaseManifest.cpp:180-387
  installed tree   Source/WoTBootstrapCore/Private/InstalledTree.cpp:50-413
  archive rules    Source/WoTBootstrapCore/Private/ArchiveRules.cpp (ValidateArchiveIndex)
  ZIP container    Source/WoTUpdaterCore/Private/ReleaseStaging.cpp:450-719
  staging order    Source/WoTUpdaterCore/Private/ReleaseStaging.cpp:1254-1707
  URL policy       Source/WoTUpdateTransports/Private/WoTUpdateTransportPolicy.cpp:61-248
  compiled values  Source/WoTBootstrapCore/Public/WoTBootstrapCore/ArtifactTypes.h:19-28

Stdlib only; runs on the Mac host's Python 3.9.
"""
from __future__ import annotations

import hashlib
import json
import mmap
import posixpath
import re
import stat
import struct
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Dict, Iterable, List, Optional, Sequence, Tuple

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
RELEASE_DIR = REPOSITORY_ROOT / "Build" / "WoTRelease"
DEFAULT_TRUST = RELEASE_DIR / "bootstrap-trust-v1.json"
DEFAULT_ROOT_RING = RELEASE_DIR / "public-root-ring.json"
DEFAULT_ANNOUNCEMENT_EXPORT = RELEASE_DIR / "announcement-public-key-v1.json"
DEFAULT_CACHE_CONTRACT = RELEASE_DIR / "cache-contract-v3-production.json"
CORE_PUBLIC = REPOSITORY_ROOT / "Source" / "WoTBootstrapCore" / "Public" / "WoTBootstrapCore"
ARTIFACT_TYPES_HEADER = CORE_PUBLIC / "ArtifactTypes.h"
INSTALLED_TREE_HEADER = CORE_PUBLIC / "InstalledTree.h"
TRUST_HEADER = CORE_PUBLIC / "Generated" / "BootstrapTrust.generated.h"

ANNOUNCEMENT_FORMAT = "wot-announcement-v1"
RELEASE_KEY_RING_FORMAT = "wot-release-key-ring-v1"
OFFLINE_ROOT_RING_FORMAT = "wot-offline-root-ring-v1"
RELEASE_MANIFEST_FORMAT = "wot-release-manifest-v2"
INSTALLED_TREE_FORMAT = "wot-installed-tree-v1"
GUARDED_RELEASE_FORMAT = "wot-guarded-release-v1"
TEST_TWIN_FORMAT = "wot-guarded-test-twin-v1"
RELEASE_IDENTITY_FORMAT = "wot-release-identity-v1"
RELEASE_IDENTITY_VERSION = 1
RELEASE_IDENTITY_MAX_BYTES = 4 * 1024
RELEASE_VERSION_FORMAT = "wot-release-version-v1"
RELEASE_VERSION_VERSION = 1
RELEASE_VERSION_MAX_BYTES = 4 * 1024
# The first release sequence whose runnable trees carry release-version.json.
# Sequences 0 and 1 (v0.1.0, v0.1.1) were packaged before the stamp existed, so
# their signed sets stay verifiable without it; a stamp they do carry must
# still match.
RELEASE_VERSION_FIRST_SEQUENCE = 2
_HUMAN_VERSION_PATTERN = re.compile(r"^[0-9][0-9A-Za-z.+-]{0,31}$")
# The signed per-platform record of the cache a release builds (CacheLineageV1):
# which content revision and label it stamps, and the manifest it produces from
# the pinned installer and contract. The next release's cache gate compares its
# own Shipping cache against it.
CACHE_LINEAGE_FORMAT = "wot-cache-lineage-v1"
CACHE_LINEAGE_FIELDS = (
    "contract_sha256", "format", "installer_sha256", "label", "manifest_sha256",
    "platform", "release_sequence", "revision",
)

# Guarded build configurations (WoT.Target.cs:15-60). Test compiles the
# WOT_BOOTSTRAP_TEST_IDENTITIES/ENDPOINTS seams the first-run funnel drives;
# Shipping compiles them out and is the only configuration a public release
# attests (release-manifest-v2.schema.json target_attestation.configuration
# const "Shipping"; Build/WoTRelease/test-keys/README.md "Test bytes are never
# release bytes").
TEST_CONFIGURATION = "Test"
SHIPPING_CONFIGURATION = "Shipping"
GUARDED_CONFIGURATIONS = (TEST_CONFIGURATION, SHIPPING_CONFIGURATION)
RELEASE_CONFIGURATION = SHIPPING_CONFIGURATION
# Literal tokens that exist in a binary only when a test seam is compiled in:
# the env-var URL overrides (WoTPlatformHttpHopWindows.cpp:153-181 and the Mac
# hop, both under WOT_BOOTSTRAP_TEST_ENDPOINTS), the per-process install-root
# override (WoTGameBootstrapWorker.cpp:446-460) and the net-test channel
# switches (WoTNetTestSubsystem.cpp:18-36), both under
# WOT_WITH_BOOTSTRAP_TEST_HARNESSES, which WoT.Build.cs:18-23 sets to 0 only in
# guarded Shipping. run_first_run_funnel requires the first two in a Test
# binary; a Shipping binary must contain none of them.
TEST_SEAM_MARKERS = (
    "WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL",
    "WOT_BOOTSTRAP_TEST_RELEASE_BASE_URL",
    "ed54a59fb1ac3a51239351362941b868e85a60e3d7b2485d828821dc7a69c279",
    "ba3c735cdbe4235e6cadf81c412b883a3472d1045faea189f584f8beca46e343",
    "wottestinstallroot=",
    "wotnettestport=",
    # WoTPreferencesSubsystem.cpp FixedPreferencesPath, same harness gate.
    "wottestpreferencesdir=",
    "wottestuninstall=",
    "wot-test-fail-after-started",
    # LAN installer sharing (WoTInstallerShareProtocol.cpp, same harness
    # gate): the loopback pairing switch and the receiver-off variable.
    "wottestlanloopback",
    "WOT_TEST_LAN_RECEIVER_OFF",
)

PLATFORMS = ("Win64", "Mac")
REPOSITORY = "aleiby/wot"
INT64_MAX = 2 ** 63 - 1
INT64_MIN = -(2 ** 63)
UINT32_MAX = 2 ** 32 - 1
INT32_MAX = 2 ** 31 - 1

# Asset names. The announcement and ring names are fixed by the compiled URLs in
# BootstrapTrust.generated.h:82-97 (generated from bootstrap-trust-v1.json).
KEY_RING_ASSET = "WoT-root-key-ring-v1.json"
RELEASE_NOTES_ASSET = "WoT-release-notes-v1.json"
PLAYER_DOWNLOAD_ASSETS = {
    "Win64": "WoT-Reborn-Windows.zip",
    "Mac": "WoT-Reborn-Mac.dmg",
}
RELEASE_NOTES_FORMAT = "wot-release-notes-v1"
RELEASE_NOTES_MAX_BYTES = 256 * 1024
RELEASE_NOTES_MAX_ENTRY_BYTES = 16 * 1024
RELEASE_NOTES_MAX_ENTRIES = 64
RELEASE_NOTES_LATEST_PATH = "/releases/latest/download/" + RELEASE_NOTES_ASSET
SIGNATURE_SUFFIX = ".sig"

# Full runner receipts use the strict, complete, ordered platform matrix. The
# base-only shape remains schema-valid advisory evidence; downstream receipt
# policy decides whether either shape gates a release.
FUNNEL_RECEIPT_FORMAT = "wot-first-run-funnel-v1"
FUNNEL_CELL_RECEIPT_FORMAT = "wot-first-run-funnel-cell-v1"
FUNNEL_CELL_NAMES = (
    "fresh-first-run",
    "second-launch",
    "damaged-cache-rebuild",
    "downloads-pickup",
    "host-join",
)
FUNNEL_REQUIRED_CELLS = FUNNEL_CELL_NAMES
# The funnel serves release assets under this loopback path prefix. The
# game's strict loopback rule (WoTParseStrictTestLoopbackUrl) refuses a base
# URL without a path, and the Test transport appends
# /releases/download/<tag>/<asset> to the base (WoTPlatformHttpHop*).
FUNNEL_RELEASE_BASE_PATH = "/release-base"
UPDATE_FUNNEL_CELLS = (
    "update-apply",
    "update-bad-build-rolls-back",
    "interrupted-swap",
)
UPDATE_FUNNEL_CELLS_BY_PLATFORM = {
    "Win64": UPDATE_FUNNEL_CELLS,
    "Mac": UPDATE_FUNNEL_CELLS + ("mac-uninstall",),
}
MAC_NOTARIZATION_ENV = {"LANG": "C", "LC_ALL": "C"}
FUNNEL_RECEIPT_KEYS = {
    "format", "platform", "verdict", "started_utc", "completed_utc",
    "duration_seconds", "release", "installer", "cells", "decisions",
}
FUNNEL_RELEASE_KEYS = {
    "manifest_path", "manifest_sha256", "asset_path", "asset_size",
    "asset_sha256", "git_commit", "release_sequence",
    "source_snapshot_sha256", "development_allow_source_drift",
    "stapled",
}
FUNNEL_INSTALLER_KEYS = {"path", "size", "sha256", "acquisition"}
PINNED_INSTALLER_NAME = "setup_the_wheel_of_time_333b_(54846).exe"
PINNED_INSTALLER_SIZE = 1_029_509_080
PINNED_INSTALLER_SHA256 = (
    "0ad6474ebc4b5d367b27197fd856733beb45179dc6d6c094641521b16b63f8bb")
CACHE_MANIFEST_FORMAT = "wot-payload-cache"
CACHE_MANIFEST_VERSION = 3
FUNNEL_CELL_KEYS = {
    "format", "index", "name", "verdict", "started_utc", "completed_utc",
    "duration_seconds", "phase_timings_seconds", "phase_order", "evidence",
    "decisions", "skip_reason", "error",
}
FUNNEL_CELL_VERDICTS = {"pass", "fail", "skip"}

UPDATE_EVIDENCE_COMMON_KEYS = {
    "base", "cleanup", "derivation_receipt", "processes", "request_transcript",
    "trees", "variant_tree_root", "trial_absent", "previous_absent",
}
UPDATE_EVIDENCE_KEYS = {
    "update-apply": UPDATE_EVIDENCE_COMMON_KEYS | {"ready_marker", "explicit_launch", "frontend_handoff"},
    "update-bad-build-rolls-back": UPDATE_EVIDENCE_COMMON_KEYS | {
        "ready_marker", "started_marker", "bad_exit_status", "failure_notice",
        "archive_request_count_after_staging", "archive_request_count_after_normal_recheck",
        "recheck", "frontend_handoff",
    },
    "interrupted-swap": UPDATE_EVIDENCE_COMMON_KEYS | {
        "missing_live_before", "started_marker", "failure_notice", "frontend_handoff",
        "player_data_unchanged", "recovery_without_trial",
    },
}
MAC_UPDATE_EVIDENCE_KEYS = {
    "update-apply": UPDATE_EVIDENCE_COMMON_KEYS | {
        "ready_marker", "explicit_launch", "frontend_handoff",
    },
    "update-bad-build-rolls-back": UPDATE_EVIDENCE_COMMON_KEYS | {
        "ready_marker", "started_marker", "bad_exit_status",
        "rollback_exit_status", "rolled_back_marker", "failure_notice",
        "archive_request_count_after_staging",
        "archive_request_count_after_normal_recheck", "recheck",
        "frontend_handoff",
    },
    "interrupted-swap": UPDATE_EVIDENCE_COMMON_KEYS | {
        "ready_marker", "interrupted_layout", "staged_absent",
        "frontend_handoff",
    },
}
UPDATE_EVIDENCE_KEYS_BY_PLATFORM = {
    "Win64": UPDATE_EVIDENCE_KEYS,
    "Mac": MAC_UPDATE_EVIDENCE_KEYS | {"mac-uninstall": {
        "network", "paths", "postconditions", "process", "protected_artifacts",
        "request_transcript", "result", "session_gate",
    }},
}


def announcement_asset(platform: str) -> str:
    return f"WoT-{platform}-announcement-v1.json"


def manifest_asset(platform: str) -> str:
    return f"WoT-{platform}-release-manifest-v2.json"


def cache_lineage_asset(platform: str) -> str:
    return f"WoT-{platform}-cache-lineage-v1.json"


def update_archive_asset(platform: str, sequence: int, commit: str) -> str:
    return f"WoT-{platform}-update-r{sequence}-{commit}.zip"


def guarded_install_zip_asset(platform: str, commit: str, configuration: str) -> str:
    """The guarded zip's name (build_stripped_pak._release_asset_name).

    The Test funnel artifact keeps the historical ``guarded`` name; the Shipping
    release artifact says so in its name, so a Test zip can never be mistaken
    for (or published as) the release download.
    """
    if configuration not in GUARDED_CONFIGURATIONS:
        raise FormatError(f"unknown guarded configuration {configuration!r}")
    flavor = "guarded-shipping" if configuration == SHIPPING_CONFIGURATION else "guarded"
    return f"WoT-{platform}-{flavor}-{commit}.zip"


def guarded_install_dmg_asset(commit: str, configuration: str) -> str:
    """The GitHub-safe Mac first-install image name.

    GitHub rewrites spaces in uploaded asset names, so the on-disk asset uses
    the release set's existing flat hyphenated grammar.  The mounted volume is
    still named ``WoT Reborn``.
    """
    if configuration not in GUARDED_CONFIGURATIONS:
        raise FormatError(f"unknown guarded configuration {configuration!r}")
    flavor = "guarded-shipping" if configuration == SHIPPING_CONFIGURATION else "guarded"
    return f"WoT-Mac-{flavor}-{commit}.dmg"


def player_download_asset_sources(verified_or_platforms) -> Dict[str, str]:
    """Map fixed player downloads to their commit-named Shipping sources."""
    platforms = getattr(verified_or_platforms, "platforms", verified_or_platforms)
    if not platforms or any(
            result.configuration != SHIPPING_CONFIGURATION
            for result in platforms.values()):
        return {}
    sources = {}
    for platform, fixed_name in PLAYER_DOWNLOAD_ASSETS.items():
        result = platforms.get(platform)
        if result is None:
            continue
        source_name = result.install_zip if platform == "Win64" else result.dmg
        if source_name is not None:
            sources[fixed_name] = source_name
    return sources


def main_executable_matches_configuration(relative: str, platform: str,
                                          configuration: str) -> bool:
    """UBT decorates non-Development game images <Target>-<Platform>-<Config>."""
    leaf = relative.rsplit("/", 1)[-1]
    expected = f"WoT-{platform}-{configuration}"
    return leaf == (expected + ".exe" if platform == "Win64" else expected)


def find_test_seam_markers(data) -> List[str]:
    """TEST_SEAM_MARKERS present in ``data`` as ASCII, UTF-16LE or UTF-32LE."""
    found = []
    for marker in TEST_SEAM_MARKERS:
        if any(marker.encode(encoding) in data
               for encoding in ("ascii", "utf-16-le", "utf-32-le")):
            found.append(marker)
    return found


# The first-install archive's single top-level folder. Windows additionally
# carries a stable launcher beside the replaceable Game tree.
GUARDED_ZIP_ROOTS = {"Win64": "WoT Reborn", "Mac": "WoT.app"}
GUARDED_TREE_ROOTS = {"Win64": "WoT Reborn/Game", "Mac": "WoT.app"}
UPDATE_TREE_ROOTS = {"Win64": "Game", "Mac": "WoT.app"}
INVENTORY_NAME = "installed-tree-v1.json"
INVENTORY_SIDECARS = (INVENTORY_NAME, INVENTORY_NAME + ".sig")   # InstalledTree.cpp:21-24
ARCHIVE_ROOT_ID = "wot-archive-root-v1"   # the value every native fixture signs

# FWoTArchiveLimits (ArchiveRules.h:43-49).
ARCHIVE_MAX_ENTRIES = 65536
ARCHIVE_MAX_TOTAL_BYTES = 8 * 1024 ** 3
ARCHIVE_MAX_ENTRY_BYTES = 4 * 1024 ** 3
ARCHIVE_MAX_DEPTH = 32

ZIP_LOCAL = 0x04034B50
ZIP_CENTRAL = 0x02014B50
ZIP_END = 0x06054B50
ZIP_UTF8_FLAG = 0x0800
ZIP_VERSION_MADE_BY = 0x033F     # Unix, spec 6.3 -- as the native fixtures write it
ZIP_VERSION_NEEDED = 20
ZIP_FIXED_DOS_DATE = 0x0021      # 1980-01-01: reproducible, carries no meaning
ZIP_FIXED_DOS_TIME = 0x0000


class FormatError(Exception):
    """An artifact is not the exact shape the native reader accepts."""


# ---------------------------------------------------------------------------
# Canonical JSON
# ---------------------------------------------------------------------------

def _check_canonical_value(value, what: str, depth: int = 0) -> None:
    if depth > 32:
        raise FormatError(f"{what}: nesting exceeds the canonical depth limit of 32")
    if isinstance(value, bool) or value is None:
        return
    if isinstance(value, int):
        if value < INT64_MIN or value > INT64_MAX:
            raise FormatError(f"{what}: integer {value} is outside int64")
        return
    if isinstance(value, float):
        raise FormatError(f"{what}: canonical JSON carries integers only")
    if isinstance(value, str):
        if any(0xD800 <= ord(character) <= 0xDFFF for character in value):
            raise FormatError(f"{what}: string carries an unpaired surrogate")
        if len(value.encode("utf-8")) > 1024 * 1024:
            raise FormatError(f"{what}: string exceeds 1 MiB")
        return
    if isinstance(value, list):
        if len(value) > 65536:
            raise FormatError(f"{what}: array exceeds 65536 elements")
        for element in value:
            _check_canonical_value(element, what, depth + 1)
        return
    if isinstance(value, dict):
        if len(value) > 65536:
            raise FormatError(f"{what}: object exceeds 65536 members")
        for key, member in value.items():
            if not isinstance(key, str):
                raise FormatError(f"{what}: object keys must be strings")
            _check_canonical_value(key, what, depth + 1)
            _check_canonical_value(member, what, depth + 1)
        return
    raise FormatError(f"{what}: {type(value).__name__} has no canonical JSON form")


def canonical_bytes(value, what: str = "document") -> bytes:
    """The one byte spelling WriteCanonicalJson produces (CanonicalJson.h:7-15)."""
    _check_canonical_value(value, what)
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True).encode("ascii")
    if len(encoded) > 4 * 1024 * 1024:
        raise FormatError(f"{what}: exceeds the 4 MiB canonical document limit")
    return encoded


def _reject_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise FormatError(f"duplicate JSON member {key!r}")
        result[key] = value
    return result


def _reject_float(text):
    raise FormatError(f"non-integer number {text!r}")


def _reject_constant(text):
    raise FormatError(f"non-JSON constant {text!r}")


def parse_canonical(raw: bytes, what: str):
    """ParseCanonicalJson: accept exactly the canonical spelling, or refuse.

    `WriteCanonicalJson(ParseCanonicalJson(Bytes))` reproduces Bytes byte for
    byte (CanonicalJson.h:3-5), so the Python check is the same identity: parse
    strictly, re-serialize, and require equality.
    """
    if not isinstance(raw, (bytes, bytearray)):
        raise FormatError(f"{what}: expected bytes")
    if len(raw) > 4 * 1024 * 1024:
        raise FormatError(f"{what}: exceeds the 4 MiB canonical document limit")
    try:
        text = bytes(raw).decode("ascii")
    except UnicodeDecodeError as exc:
        raise FormatError(f"{what}: canonical JSON is ASCII-only") from exc
    try:
        value = json.loads(text, object_pairs_hook=_reject_duplicates,
                           parse_float=_reject_float, parse_constant=_reject_constant)
    except FormatError as exc:
        raise FormatError(f"{what}: {exc}") from exc
    except ValueError as exc:
        raise FormatError(f"{what}: not JSON: {exc}") from exc
    if canonical_bytes(value, what) != bytes(raw):
        raise FormatError(f"{what}: valid JSON but not its canonical spelling")
    return value


@dataclass(frozen=True)
class ReleaseIdentity:
    release_sequence: int
    release_commit: str


def parse_release_identity(raw: bytes, what: str = "release identity"
                           ) -> ReleaseIdentity:
    """Read the closed, canonical identity stamped into a runnable tree."""
    if len(raw) > RELEASE_IDENTITY_MAX_BYTES:
        raise FormatError(
            f"{what}: exceeds the {RELEASE_IDENTITY_MAX_BYTES}-byte limit")
    document = parse_canonical(raw, what)
    require_exact_members(
        document,
        ("format", "release_commit", "release_sequence", "version"),
        what)
    if read_string(document, "format", what) != RELEASE_IDENTITY_FORMAT:
        raise FormatError(f"{what}.format is not {RELEASE_IDENTITY_FORMAT}")
    if read_unsigned32(document, "version", what) != RELEASE_IDENTITY_VERSION:
        raise FormatError(
            f"{what}.version is not {RELEASE_IDENTITY_VERSION}")
    release_sequence = read_unsigned(document, "release_sequence", what)
    if release_sequence > INT64_MAX:
        raise FormatError(f"{what}.release_sequence exceeds int64")
    return ReleaseIdentity(
        release_sequence=release_sequence,
        release_commit=read_string(document, "release_commit", what))


def release_identity_relative(platform: str) -> str:
    if platform == "Win64":
        return "WoT/Build/WoTRelease/release-identity.json"
    if platform == "Mac":
        return "Contents/UE/WoT/Build/WoTRelease/release-identity.json"
    raise FormatError(f"unknown platform {platform!r}")


def read_release_identity_from_zip(archive_path: Path, platform: str,
                                   tree_root: str) -> ReleaseIdentity:
    """Read the one identity member from an install or update ZIP."""
    member = f"{tree_root}/{release_identity_relative(platform)}"
    what = f"{Path(archive_path).name} release identity"
    try:
        with zipfile.ZipFile(archive_path) as archive:
            matches = [info for info in archive.infolist()
                       if info.filename.rstrip("/") == member]
            if not matches:
                raise FormatError(f"{what}: missing release identity {member}")
            if len(matches) != 1 or matches[0].is_dir():
                raise FormatError(
                    f"{what}: malformed release identity member {member}")
            info = matches[0]
            if info.file_size > RELEASE_IDENTITY_MAX_BYTES:
                raise FormatError(
                    f"{what}: exceeds the {RELEASE_IDENTITY_MAX_BYTES}-byte limit")
            with archive.open(info) as handle:
                raw = handle.read(RELEASE_IDENTITY_MAX_BYTES + 1)
    except FormatError:
        raise
    except (OSError, KeyError, zipfile.BadZipFile) as exc:
        raise FormatError(f"{what}: cannot read release identity: {exc}") from exc
    try:
        return parse_release_identity(raw, what)
    except FormatError as exc:
        raise FormatError(f"malformed release identity: {exc}") from exc


def validate_human_version(text: str, what: str, allow_empty: bool) -> str:
    """Validate the human spelling shown after ``v`` in the game UI."""
    if not isinstance(text, str):
        raise FormatError(f"{what}: must be a string")
    if text == "" and allow_empty:
        return text
    if not _HUMAN_VERSION_PATTERN.fullmatch(text):
        empty = " or empty" if allow_empty else ""
        raise FormatError(
            f"{what}: must start with a digit, contain at most 32 characters "
            f"from [0-9A-Za-z.+-]{empty}")
    return text


def release_version_bytes(human_version: str) -> bytes:
    """Write the closed canonical version stamped into a runnable tree."""
    validate_human_version(
        human_version, "release version human_version", allow_empty=True)
    return canonical_bytes({
        "format": RELEASE_VERSION_FORMAT,
        "human_version": human_version,
        "version": RELEASE_VERSION_VERSION,
    }, "release version")


def parse_release_version(raw: bytes, what: str = "release version") -> str:
    """Read the closed canonical human version stamped into a runnable tree."""
    if len(raw) > RELEASE_VERSION_MAX_BYTES:
        raise FormatError(
            f"{what}: exceeds the {RELEASE_VERSION_MAX_BYTES}-byte limit")
    document = parse_canonical(raw, what)
    require_exact_members(
        document, ("format", "human_version", "version"), what)
    if read_string(document, "format", what) != RELEASE_VERSION_FORMAT:
        raise FormatError(f"{what}.format is not {RELEASE_VERSION_FORMAT}")
    if read_unsigned32(document, "version", what) != RELEASE_VERSION_VERSION:
        raise FormatError(
            f"{what}.version is not {RELEASE_VERSION_VERSION}")
    human_version = read_string(document, "human_version", what)
    return validate_human_version(
        human_version, f"{what}.human_version", allow_empty=True)


def release_version_relative(platform: str) -> str:
    if platform == "Win64":
        return "WoT/Build/WoTRelease/release-version.json"
    if platform == "Mac":
        return "Contents/UE/WoT/Build/WoTRelease/release-version.json"
    raise FormatError(f"unknown platform {platform!r}")


def read_release_version_from_zip(archive_path: Path, platform: str,
                                  tree_root: str, *,
                                  allow_missing: bool = False) -> Optional[str]:
    """Read the one release-version member from an install or update ZIP.

    With allow_missing, an archive without the member returns None (a release
    packaged before RELEASE_VERSION_FIRST_SEQUENCE); a malformed one still fails.
    """
    member = f"{tree_root}/{release_version_relative(platform)}"
    what = f"{Path(archive_path).name} release version"
    try:
        with zipfile.ZipFile(archive_path) as archive:
            matches = [info for info in archive.infolist()
                       if info.filename.rstrip("/") == member]
            if not matches:
                if allow_missing:
                    return None
                raise FormatError(f"{what}: missing release version {member}")
            mode = (matches[0].external_attr >> 16) & 0xFFFF
            file_type = stat.S_IFMT(mode)
            if (len(matches) != 1 or matches[0].is_dir()
                    or file_type not in (0, stat.S_IFREG)):
                raise FormatError(
                    f"{what}: malformed release version member {member}")
            info = matches[0]
            if info.file_size > RELEASE_VERSION_MAX_BYTES:
                raise FormatError(
                    f"{what}: exceeds the {RELEASE_VERSION_MAX_BYTES}-byte limit")
            with archive.open(info) as handle:
                raw = handle.read(RELEASE_VERSION_MAX_BYTES + 1)
    except FormatError:
        raise
    except (OSError, KeyError, zipfile.BadZipFile) as exc:
        raise FormatError(f"{what}: cannot read release version: {exc}") from exc
    try:
        return parse_release_version(raw, what)
    except FormatError as exc:
        raise FormatError(f"malformed release version: {exc}") from exc


# Closed-schema readers (CanonicalJson.h:157-199). Anything unknown, missing, or
# wrong-typed is refused; nothing is defaulted.

def require_exact_members(document, fields: Iterable[str], what: str) -> None:
    if not isinstance(document, dict):
        raise FormatError(f"{what} is not an object")
    expected = set(fields)
    actual = set(document)
    if actual != expected:
        unknown = sorted(actual - expected)
        missing = sorted(expected - actual)
        raise FormatError(f"{what} fields differ: unknown={unknown} missing={missing}")


def read_string(document: dict, key: str, what: str) -> str:
    value = document.get(key)
    if not isinstance(value, str):
        raise FormatError(f"{what}.{key} is not a string")
    return value


def read_bool(document: dict, key: str, what: str) -> bool:
    value = document.get(key)
    if not isinstance(value, bool):
        raise FormatError(f"{what}.{key} is not a boolean")
    return value


def read_unsigned(document: dict, key: str, what: str) -> int:
    value = document.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise FormatError(f"{what}.{key} is not an integer")
    if value < 0:
        raise FormatError(f"{what}.{key} is negative")
    return value


def read_unsigned32(document: dict, key: str, what: str) -> int:
    value = read_unsigned(document, key, what)
    if value > UINT32_MAX:
        raise FormatError(f"{what}.{key} exceeds uint32")
    return value


def read_object(document: dict, key: str, what: str) -> dict:
    value = document.get(key)
    if not isinstance(value, dict):
        raise FormatError(f"{what}.{key} is not an object")
    return value


_RELEASE_NOTES_WHITESPACE = (
    "\u0009\u000a\u000b\u000c\u000d\u001c\u001d\u001e\u001f\u0020"
    "\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006"
    "\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000")


def normalize_release_notes_text(text: str) -> str:
    """Return the publisher spelling required by the native plain-text UI."""
    if not isinstance(text, str):
        raise FormatError("release notes text is not a string")
    if "\0" in text:
        raise FormatError("release notes text contains U+0000")
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    normalized = "\n".join(
        line.rstrip(_RELEASE_NOTES_WHITESPACE)
        for line in normalized.split("\n"))
    normalized = normalized.rstrip(_RELEASE_NOTES_WHITESPACE)
    if not normalized:
        raise FormatError("release notes text is empty after normalization")
    if len(normalized.encode("utf-8")) > RELEASE_NOTES_MAX_ENTRY_BYTES:
        raise FormatError(
            f"release notes text exceeds {RELEASE_NOTES_MAX_ENTRY_BYTES} UTF-8 bytes")
    return normalized


def parse_release_notes(raw: bytes, what: str = "release notes") -> dict:
    """Validate the closed cumulative release-notes asset."""
    if not isinstance(raw, (bytes, bytearray)):
        raise FormatError(f"{what}: expected bytes")
    if len(raw) > RELEASE_NOTES_MAX_BYTES:
        raise FormatError(
            f"{what}: exceeds the {RELEASE_NOTES_MAX_BYTES}-byte limit")
    document = parse_canonical(raw, what)
    require_exact_members(document, ("format", "notes"), what)
    if read_string(document, "format", what) != RELEASE_NOTES_FORMAT:
        raise FormatError(f"{what}.format is not {RELEASE_NOTES_FORMAT}")
    notes = document.get("notes")
    if not isinstance(notes, list):
        raise FormatError(f"{what}.notes is not an array")
    if len(notes) > RELEASE_NOTES_MAX_ENTRIES:
        raise FormatError(
            f"{what}.notes exceeds {RELEASE_NOTES_MAX_ENTRIES} entries")
    previous = 0
    for index, entry in enumerate(notes):
        entry_what = f"{what}.notes[{index}]"
        require_exact_members(entry, ("release_sequence", "text"), entry_what)
        sequence = read_unsigned(entry, "release_sequence", entry_what)
        if sequence < 1 or sequence <= previous:
            raise FormatError(
                f"{entry_what}.release_sequence is not positive and increasing")
        text = read_string(entry, "text", entry_what)
        try:
            normalized = normalize_release_notes_text(text)
        except FormatError as exc:
            raise FormatError(f"{entry_what}.text: {exc}") from exc
        if text != normalized:
            raise FormatError(f"{entry_what}.text is not normalized")
        previous = sequence
    return document


def release_notes_bytes(document: dict) -> bytes:
    raw = canonical_bytes(document, "release notes")
    parse_release_notes(raw)
    return raw


def merge_release_notes(previous: Optional[bytes], release_sequence: int,
                        text: str) -> bytes:
    """Insert/replace one entry, dropping oldest entries to stay bounded."""
    if isinstance(release_sequence, bool) or not isinstance(release_sequence, int) \
            or release_sequence < 0 or release_sequence > INT64_MAX:
        raise FormatError("release notes sequence is outside [0, INT64_MAX]")
    document = (parse_release_notes(previous) if previous is not None else {
        "format": RELEASE_NOTES_FORMAT,
        "notes": [],
    })
    if release_sequence == 0:
        return release_notes_bytes(document)
    normalized = normalize_release_notes_text(text)
    entries = [dict(entry) for entry in document["notes"]
               if entry["release_sequence"] != release_sequence]
    entries.append({"release_sequence": release_sequence, "text": normalized})
    entries.sort(key=lambda entry: entry["release_sequence"])
    merged = {"format": RELEASE_NOTES_FORMAT, "notes": entries}
    while len(entries) > RELEASE_NOTES_MAX_ENTRIES \
            or len(canonical_bytes(merged, "release notes")) > RELEASE_NOTES_MAX_BYTES:
        drop = next((index for index, entry in enumerate(entries)
                     if entry["release_sequence"] != release_sequence), None)
        if drop is None:
            raise FormatError("the added release-notes entry cannot fit the asset")
        entries.pop(drop)
    return release_notes_bytes(merged)


def replace_release_notes_entry(previous: bytes, release_sequence: int,
                                text: str) -> bytes:
    """Replace one existing entry; never adds, drops or reorders another.

    An edit that no longer fits the asset is refused rather than trimmed, so
    correcting one release can never delete another release's notes.
    """
    document = parse_release_notes(previous)
    normalized = normalize_release_notes_text(text)
    entries = [dict(entry) for entry in document["notes"]]
    targets = [entry for entry in entries
               if entry["release_sequence"] == release_sequence]
    if len(targets) != 1:
        raise FormatError(
            f"release notes asset does not contain release sequence {release_sequence}")
    targets[0]["text"] = normalized
    return release_notes_bytes({"format": RELEASE_NOTES_FORMAT, "notes": entries})


def parse_lower_hex(value, byte_count: int, what: str) -> bytes:
    """FixedBytesFromHex: exactly 2*byte_count lower-case hex (ArtifactTypes.h:64-74)."""
    if not isinstance(value, str) or len(value) != byte_count * 2 \
            or any(character not in "0123456789abcdef" for character in value):
        raise FormatError(f"{what} is not {byte_count * 2} lower-case hex characters")
    return bytes.fromhex(value)


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> Tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


# ---------------------------------------------------------------------------
# Cache lineage (signed by the release key under CacheLineageV1)
# ---------------------------------------------------------------------------

def _check_cache_lineage(document, platform: str, what: str) -> dict:
    if platform not in PLATFORMS:
        raise FormatError(f"{what}: unknown platform {platform!r}")
    require_exact_members(document, CACHE_LINEAGE_FIELDS, what)
    if read_string(document, "format", what) != CACHE_LINEAGE_FORMAT:
        raise FormatError(f"{what}.format is not {CACHE_LINEAGE_FORMAT}")
    if read_string(document, "platform", what) != platform:
        raise FormatError(f"{what}.platform is not {platform}")
    if read_unsigned(document, "release_sequence", what) > INT64_MAX:
        raise FormatError(f"{what}.release_sequence exceeds int64")
    revision = read_unsigned(document, "revision", what)
    if revision < 1:
        raise FormatError(f"{what}.revision must be at least 1")
    for key in ("label", "installer_sha256", "contract_sha256", "manifest_sha256"):
        parse_lower_hex(document.get(key), 32, f"{what}.{key}")
    content_label = _dist_module("cache_content_label")
    try:
        expected = content_label.label(revision)
    except ValueError as exc:
        raise FormatError(f"{what}.revision: {exc}") from exc
    if document["label"] != expected:
        raise FormatError(
            f"{what}.label is not the content label of revision {revision}")
    return document


def build_cache_lineage(*, platform: str, release_sequence: int, revision: int,
                        label: str, installer_sha256: str, contract_sha256: str,
                        manifest_sha256: str) -> dict:
    """The closed lineage record; refuses any value parse_cache_lineage would."""
    document = {
        "contract_sha256": contract_sha256,
        "format": CACHE_LINEAGE_FORMAT,
        "installer_sha256": installer_sha256,
        "label": label,
        "manifest_sha256": manifest_sha256,
        "platform": platform,
        "release_sequence": release_sequence,
        "revision": revision,
    }
    what = f"{platform} cache lineage"
    _check_cache_lineage(document, platform, what)
    canonical_bytes(document, what)
    return document


def parse_cache_lineage(raw: bytes, platform: str) -> dict:
    """Read one platform's lineage asset: canonical bytes and the closed schema."""
    what = (cache_lineage_asset(platform) if platform in PLATFORMS
            else "cache lineage")
    return _check_cache_lineage(parse_canonical(raw, what), platform, what)


# ---------------------------------------------------------------------------
# Compiled constants and checked-in trust
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class CompiledConstants:
    gate_protocol: int
    commit_service_protocol: int
    updater_protocol: int
    cache_contract_version: int
    release_manifest_version: int
    installed_tree_version: int
    announcement_domain: str


def load_compiled_constants(artifact_types: Path = ARTIFACT_TYPES_HEADER,
                            installed_tree: Path = INSTALLED_TREE_HEADER
                            ) -> CompiledConstants:
    """Read the protocol numbers from the header the binaries compile.

    Read rather than restated so a bumped protocol can never be silently signed
    against a stale Python copy (ArtifactTypes.h:19-28, InstalledTree.h:12-13).
    """
    text = Path(artifact_types).read_text(encoding="utf-8")
    numbers = dict(re.findall(r"inline constexpr uint32 (\w+) = (\d+);", text))
    domain = re.search(
        r'inline constexpr TCHAR AnnouncementDomain\[\] = TEXT\("([^"]+)"\);', text)
    tree = re.search(r"inline constexpr uint32 WoTInstalledTreeVersion = (\d+);",
                     Path(installed_tree).read_text(encoding="utf-8"))
    required = ("GateProtocol", "CommitServiceProtocol", "UpdaterProtocol",
                "CacheContractVersion", "ReleaseManifestVersion")
    missing = [name for name in required if name not in numbers]
    if missing or not domain or not tree:
        raise FormatError(
            f"cannot read compiled constants from {artifact_types}: missing {missing}")
    return CompiledConstants(
        gate_protocol=int(numbers["GateProtocol"]),
        commit_service_protocol=int(numbers["CommitServiceProtocol"]),
        updater_protocol=int(numbers["UpdaterProtocol"]),
        cache_contract_version=int(numbers["CacheContractVersion"]),
        release_manifest_version=int(numbers["ReleaseManifestVersion"]),
        installed_tree_version=int(tree.group(1)),
        announcement_domain=domain.group(1))


@dataclass
class Trust:
    announcement_key: bytes
    announcement_key_id: str
    root_ring: "KeyRing"
    root_ring_raw: bytes
    announcement_urls: Dict[str, str]
    recovery_url: str
    trust_raw: bytes


def _dist_module(name: str):
    import importlib
    import sys

    directory = str(Path(__file__).resolve().parent)
    if directory not in sys.path:
        sys.path.insert(0, directory)
    return importlib.import_module(name)


def load_trust(trust_path: Path = DEFAULT_TRUST,
               root_ring_path: Path = DEFAULT_ROOT_RING,
               announcement_path: Path = DEFAULT_ANNOUNCEMENT_EXPORT,
               header_path: Optional[Path] = TRUST_HEADER) -> Trust:
    """Load the checked-in public trust and prove its pieces agree.

    bootstrap-trust-v1.json must bind the exact announcement export and root ring
    bytes (generate_bootstrap_trust.py:275-287), and -- when the generated header
    is present -- the compiled key, root-ring bytes, and URLs must be the same
    values (BootstrapTrust.generated.h:20-97). A publish set is verified against
    what the binaries actually embed, not against a file that could have drifted.
    """
    generator = _dist_module("generate_bootstrap_trust")
    try:
        announcement_hex, announcement_raw = generator.load_announcement_export(
            Path(announcement_path))
        _, root_raw = generator.load_root_ring(Path(root_ring_path))
        trust, trust_raw = generator._load_canonical_json(
            Path(trust_path), "bootstrap trust")
    except generator.TrustError as exc:
        raise FormatError(str(exc)) from exc
    announcement_document = json.loads(announcement_raw)
    expected = generator.build_trust_document(
        announcement_key=announcement_hex, announcement_raw=announcement_raw,
        root_ring_raw=root_raw, win64_url=generator.WIN64_ANNOUNCEMENT_URL,
        mac_url=generator.MAC_ANNOUNCEMENT_URL,
        recovery_url=generator.ROOT_KEY_RING_RECOVERY_URL)
    if trust != expected:
        raise FormatError(
            f"{trust_path} does not bind the checked-in announcement export, root "
            "ring, and fixed URLs; rerun generate_bootstrap_trust.py --check")
    root_ring = parse_key_ring(parse_canonical(root_raw, "offline-root ring"),
                               OFFLINE_ROOT_RING_FORMAT)
    result = Trust(
        announcement_key=bytes.fromhex(announcement_hex),
        announcement_key_id=announcement_document["key_id"],
        root_ring=root_ring, root_ring_raw=root_raw,
        announcement_urls={"Win64": trust["win64_announcement_url"],
                           "Mac": trust["mac_announcement_url"]},
        recovery_url=trust["root_key_ring_recovery_url"], trust_raw=trust_raw)
    if header_path is not None and Path(header_path).is_file():
        _check_trust_header(Path(header_path), result)
    return result


def _check_trust_header(path: Path, trust: Trust) -> None:
    text = path.read_text(encoding="utf-8")
    key = re.search(r'AnnouncementPublicKeyHex\[\] = TEXT\("([0-9a-f]{64})"\);', text)
    ring = re.search(r"OfflineRootKeyRingCanonicalBytes\[\] = \{([^}]*)\};", text)
    recovery = re.search(r'RootKeyRingRecoveryUrl\[\] = TEXT\("([^"]+)"\);', text)
    urls = re.findall(r'AnnouncementUrl\[\] = TEXT\("([^"]+)"\);', text)
    if not key or not ring or not recovery or len(urls) != 2:
        raise FormatError(f"{path} does not have the generated trust shape")
    embedded = bytes(int(token, 16) for token in re.findall(r"0x([0-9a-f]{2})",
                                                            ring.group(1)))
    if key.group(1) != trust.announcement_key.hex():
        raise FormatError("the compiled announcement key differs from the trust JSON")
    if embedded != trust.root_ring_raw:
        raise FormatError("the compiled offline-root ring differs from the trust JSON")
    if recovery.group(1) != trust.recovery_url:
        raise FormatError("the compiled recovery URL differs from the trust JSON")
    if urls != [trust.announcement_urls["Win64"], trust.announcement_urls["Mac"]]:
        raise FormatError("the compiled announcement URLs differ from the trust JSON")


# ---------------------------------------------------------------------------
# URL policy (WoTUpdateTransportPolicy.cpp:61-248)
# ---------------------------------------------------------------------------

def _parse_absolute_url(url: str, require_ascii: bool) -> Optional[Tuple[str, str]]:
    """Port of WoTUpdateTransportParseAbsoluteUrl; returns (scheme, host)."""
    if not url:
        return None
    for character in url:
        code = ord(character)
        if code <= 0x20 or code == 0x7F or character == "\\" \
                or (require_ascii and code > 0x7F):
            return None
    scheme_end = -1
    for index, character in enumerate(url):
        if character == ":":
            scheme_end = index
            break
        if character in "/?#":
            return None
    if scheme_end <= 0 or scheme_end + 2 >= len(url) \
            or url[scheme_end + 1] != "/" or url[scheme_end + 2] != "/":
        return None
    scheme = ""
    for index in range(scheme_end):
        character = url[index]
        valid = character.isascii() and (character.isalpha() if index == 0 else (
            character.isalnum() or character in "+-."))
        if not valid:
            return None
        scheme += character.lower()
    authority_start = scheme_end + 3
    authority_end = len(url)
    for index in range(authority_start, len(url)):
        if url[index] in "/?#":
            authority_end = index
            break
    if authority_end == authority_start:
        return None
    if "#" in url[authority_end:]:
        return None
    authority = url[authority_start:authority_end]
    if any(ord(character) > 0x7F or character in "@:%[]" for character in authority):
        return None
    only_digits_and_dots = True
    for label in authority.split("."):
        if not label or label[0] == "-" or label[-1] == "-":
            return None
        if len(label) >= 4 and label[:4].lower() == "xn--":
            return None
        for character in label:
            if not (character.isascii() and (character.isalnum() or character == "-")):
                return None
            if not character.isdigit():
                only_digits_and_dots = False
    if only_digits_and_dots:
        return None
    return scheme, authority.lower()


def is_allowed_initial_release_url(url: str) -> bool:
    """WoTIsAllowedInitialReleaseUrl: https, host exactly github.com."""
    parsed = _parse_absolute_url(url, True)
    return parsed is not None and parsed == ("https", "github.com")


def release_download_url(tag: str, asset: str) -> str:
    """An immutable per-tag URL.

    Signed documents use immutable per-tag URLs. The separately fetched,
    unsigned cumulative notes asset intentionally uses release_notes_latest_url().
    """
    validate_tag(tag)
    validate_asset_name(asset)
    url = f"https://github.com/{REPOSITORY}/releases/download/{tag}/{asset}"
    if not is_allowed_initial_release_url(url):
        raise FormatError(f"{url} fails the native initial-URL policy")
    return url


def release_notes_latest_url() -> str:
    url = (f"https://github.com/{REPOSITORY}/releases/latest/download/"
           f"{RELEASE_NOTES_ASSET}")
    if not is_allowed_initial_release_url(url):
        raise FormatError(f"{url} fails the native initial-URL policy")
    return url


_TAG = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
_ASSET = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,199}$")


def validate_tag(tag: str) -> str:
    if not isinstance(tag, str) or not _TAG.match(tag) or ".." in tag:
        raise FormatError(f"release tag {tag!r} is not a plain [A-Za-z0-9._-] name")
    return tag


def validate_asset_name(name: str) -> str:
    if not isinstance(name, str) or not _ASSET.match(name) or ".." in name:
        raise FormatError(f"asset name {name!r} is not a plain [A-Za-z0-9._-] name")
    return name


# ---------------------------------------------------------------------------
# Key rings (KeyRing.cpp:4-189)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class ReleaseKey:
    key_id: str
    public_key: bytes
    first_sequence: int
    last_sequence: int


@dataclass(frozen=True)
class KeyRing:
    format: str
    epoch: int
    keys: Tuple[ReleaseKey, ...]
    revoked_key_ids: Tuple[str, ...]


def parse_key_ring(document, expected_format: str) -> KeyRing:
    what = "key ring"
    require_exact_members(document, ("epoch", "format", "keys", "revoked_key_ids"), what)
    ring_format = read_string(document, "format", what)
    if ring_format != expected_format:
        raise FormatError(f"key ring format {ring_format!r} is not {expected_format!r}")
    epoch = read_unsigned(document, "epoch", what)
    keys = document["keys"]
    if not isinstance(keys, list):
        raise FormatError("key ring field 'keys' is not an array")
    seen = set()
    parsed = []
    for index, entry in enumerate(keys):
        entry_what = f"release key {index}"
        require_exact_members(entry, ("first_sequence", "key_id", "last_sequence",
                                      "public_key"), entry_what)
        key_id = read_string(entry, "key_id", entry_what)
        if not key_id:
            raise FormatError("a release key id must not be empty")
        if key_id in seen:
            raise FormatError(f"release key id {key_id!r} appears twice")
        seen.add(key_id)
        public_key = parse_lower_hex(entry["public_key"], 32, f"{entry_what}.public_key")
        first = read_unsigned(entry, "first_sequence", entry_what)
        last = read_unsigned(entry, "last_sequence", entry_what)
        if first > last:
            raise FormatError(f"release key {key_id!r} has an inverted sequence range")
        parsed.append(ReleaseKey(key_id, public_key, first, last))
    revoked = document["revoked_key_ids"]
    if not isinstance(revoked, list) or any(not isinstance(item, str) for item in revoked):
        raise FormatError("every revoked key id must be a string")
    return KeyRing(ring_format, epoch, tuple(parsed), tuple(revoked))


def select_release_key(ring: KeyRing, key_id: str, sequence: int) -> ReleaseKey:
    """SelectReleaseKey: revocation first, exact id, inclusive window, no fallback."""
    if key_id in ring.revoked_key_ids:
        raise FormatError(f"release key {key_id!r} is revoked")
    for key in ring.keys:
        if key.key_id != key_id:
            continue
        if sequence < key.first_sequence or sequence > key.last_sequence:
            raise FormatError(
                f"release key {key_id!r} covers [{key.first_sequence}, "
                f"{key.last_sequence}], not sequence {sequence}")
        return key
    raise FormatError(f"no release key {key_id!r} in the ring")


def build_key_ring(epoch: int, keys: Sequence[ReleaseKey],
                   revoked: Sequence[str]) -> dict:
    document = {
        "epoch": epoch,
        "format": RELEASE_KEY_RING_FORMAT,
        "keys": [{"first_sequence": key.first_sequence, "key_id": key.key_id,
                  "last_sequence": key.last_sequence,
                  "public_key": key.public_key.hex()} for key in keys],
        "revoked_key_ids": list(revoked),
    }
    parse_key_ring(parse_canonical(canonical_bytes(document), "key ring"),
                   RELEASE_KEY_RING_FORMAT)
    return document


# ---------------------------------------------------------------------------
# Announcement (Announcement.cpp:6-195)
# ---------------------------------------------------------------------------

ANNOUNCEMENT_FIELDS = ("announcement_domain", "format", "key_id", "key_ring",
                       "key_ring_epoch", "manifest", "minimum_gate_protocol",
                       "platform", "release_sequence")


@dataclass(frozen=True)
class ObjectRef:
    url: str
    size: int
    sha256: str


@dataclass(frozen=True)
class Announcement:
    platform: str
    domain: str
    key_id: str
    release_sequence: int
    key_ring_epoch: int
    minimum_gate_protocol: int
    key_ring: ObjectRef
    manifest: ObjectRef


def _read_object_ref(parent: dict, key: str, what: str) -> ObjectRef:
    reference = read_object(parent, key, what)
    ref_what = f"{what}.{key}"
    require_exact_members(reference, ("sha256", "size", "url"), ref_what)
    url = read_string(reference, "url", ref_what)
    if not url.startswith("https://"):
        raise FormatError(f"{ref_what}.url is not HTTPS")
    size = read_unsigned(reference, "size", ref_what)
    if size == 0:
        raise FormatError(f"{ref_what}.size is zero")
    digest = reference.get("sha256")
    parse_lower_hex(digest, 32, f"{ref_what}.sha256")
    return ObjectRef(url, size, digest)


def parse_announcement(raw: bytes, platform: str, compiled_domain: str) -> Announcement:
    """VerifyAnnouncementV1 after its signature check (Announcement.cpp:87-194)."""
    what = "announcement"
    document = parse_canonical(raw, what)
    require_exact_members(document, ANNOUNCEMENT_FIELDS, what)
    if read_string(document, "format", what) != ANNOUNCEMENT_FORMAT:
        raise FormatError(f"announcement format is not {ANNOUNCEMENT_FORMAT!r}")
    if read_string(document, "platform", what) != platform:
        raise FormatError(f"announcement platform is not {platform!r}")
    if read_string(document, "announcement_domain", what) != compiled_domain:
        raise FormatError(f"announcement domain is not the compiled {compiled_domain!r}")
    key_id = read_string(document, "key_id", what)
    if not key_id:
        raise FormatError("the announcement names no signing key id for its manifest")
    return Announcement(
        platform=platform, domain=compiled_domain, key_id=key_id,
        release_sequence=read_unsigned(document, "release_sequence", what),
        key_ring_epoch=read_unsigned(document, "key_ring_epoch", what),
        minimum_gate_protocol=read_unsigned32(document, "minimum_gate_protocol", what),
        key_ring=_read_object_ref(document, "key_ring", what),
        manifest=_read_object_ref(document, "manifest", what))


def build_announcement(*, platform: str, domain: str, key_id: str,
                       release_sequence: int, key_ring_epoch: int,
                       minimum_gate_protocol: int, key_ring: ObjectRef,
                       manifest: ObjectRef) -> dict:
    def ref(value: ObjectRef) -> dict:
        return {"sha256": value.sha256, "size": value.size, "url": value.url}

    document = {
        "announcement_domain": domain,
        "format": ANNOUNCEMENT_FORMAT,
        "key_id": key_id,
        "key_ring": ref(key_ring),
        "key_ring_epoch": key_ring_epoch,
        "manifest": ref(manifest),
        "minimum_gate_protocol": minimum_gate_protocol,
        "platform": platform,
        "release_sequence": release_sequence,
    }
    parse_announcement(canonical_bytes(document), platform, domain)
    return document


# ---------------------------------------------------------------------------
# Release manifest v2 (ReleaseManifest.cpp:6-387)
# ---------------------------------------------------------------------------

MANIFEST_FIELDS = (
    "announcement_domain", "archive", "archive_root_id", "build_commit",
    "cache_contract_id", "cache_contract_version", "format", "generated_utc",
    "human_version", "installed_tree_root", "installed_tree_version", "key_id",
    "main_executable", "manifest_version", "minimum_commit_service_protocol",
    "minimum_gate_protocol", "minimum_updater_protocol", "platform",
    "release_sequence", "stripped_audit_verdict", "target_attestation")
ATTESTATION_FIELDS = ("configuration", "dedicated_server_startup_available",
                      "launch_module", "listen_server_present",
                      "release_gate_enabled", "target_type")


@dataclass(frozen=True)
class ComponentRef:
    name: str
    sha256: str
    size: int


@dataclass(frozen=True)
class Manifest:
    document: dict
    platform: str
    release_sequence: int
    key_id: str
    archive_name: str
    archive: ObjectRef
    installed_tree_root: ComponentRef
    main_executable: ComponentRef
    minimums: Dict[str, int]


def _read_component(parent: dict, key: str, what: str) -> ComponentRef:
    reference = read_object(parent, key, what)
    ref_what = f"{what}.{key}"
    require_exact_members(reference, ("name", "sha256", "size"), ref_what)
    name = read_string(reference, "name", ref_what)
    size = read_unsigned(reference, "size", ref_what)
    digest = reference.get("sha256")
    parse_lower_hex(digest, 32, f"{ref_what}.sha256")
    return ComponentRef(name, digest, size)


def is_bare_archive_name(name: str) -> bool:
    """ReleaseManifest.cpp:75-82: the archive name becomes a local path."""
    return bool(name) and not any(token in name for token in ("/", "\\", "..", ":"))


def parse_manifest(raw: bytes, announcement: Announcement) -> Manifest:
    """VerifyReleaseManifestV2 after its signature check (ReleaseManifest.cpp:220-386)."""
    what = "release manifest"
    document = parse_canonical(raw, what)
    require_exact_members(document, MANIFEST_FIELDS, what)
    if read_string(document, "format", what) != RELEASE_MANIFEST_FORMAT:
        raise FormatError(f"release manifest format is not {RELEASE_MANIFEST_FORMAT!r}")
    if read_unsigned32(document, "manifest_version", what) != 2:
        raise FormatError("release manifest version is not 2")
    platform = read_string(document, "platform", what)
    domain = read_string(document, "announcement_domain", what)
    sequence = read_unsigned(document, "release_sequence", what)
    key_id = read_string(document, "key_id", what)
    if domain != announcement.domain:
        raise FormatError("the release manifest domain does not match the announcement")
    if platform != announcement.platform:
        raise FormatError("the release manifest platform does not match the announcement")
    if sequence != announcement.release_sequence:
        raise FormatError("the release manifest sequence does not match the announcement")
    if key_id != announcement.key_id:
        raise FormatError("the release manifest key id does not match the announcement")
    for text_field in ("human_version", "build_commit", "generated_utc",
                       "archive_root_id", "stripped_audit_verdict", "cache_contract_id"):
        read_string(document, text_field, what)
    archive = read_object(document, "archive", what)
    require_exact_members(archive, ("name", "sha256", "size", "url"),
                          "release manifest.archive")
    archive_name = read_string(archive, "name", "release manifest.archive")
    if not is_bare_archive_name(archive_name):
        raise FormatError("the archive name must be a bare file name")
    archive_url = read_string(archive, "url", "release manifest.archive")
    if not archive_url.startswith("https://"):
        raise FormatError("the archive url is not HTTPS")
    archive_size = read_unsigned(archive, "size", "release manifest.archive")
    if archive_size == 0:
        raise FormatError("the archive size is zero")
    parse_lower_hex(archive.get("sha256"), 32, "release manifest.archive.sha256")
    tree = _read_component(document, "installed_tree_root", what)
    main = _read_component(document, "main_executable", what)
    attestation = read_object(document, "target_attestation", what)
    attestation_what = "release manifest.target_attestation"
    require_exact_members(attestation, ATTESTATION_FIELDS, attestation_what)
    for text_field in ("target_type", "configuration", "launch_module"):
        read_string(attestation, text_field, attestation_what)
    for flag in ("listen_server_present", "dedicated_server_startup_available",
                 "release_gate_enabled"):
        read_bool(attestation, flag, attestation_what)
    minimums = {name: read_unsigned32(document, name, what) for name in (
        "minimum_gate_protocol", "minimum_commit_service_protocol",
        "minimum_updater_protocol", "installed_tree_version", "cache_contract_version")}
    return Manifest(document=document, platform=platform, release_sequence=sequence,
                    key_id=key_id, archive_name=archive_name,
                    archive=ObjectRef(archive_url, archive_size, archive["sha256"]),
                    installed_tree_root=tree, main_executable=main,
                    minimums=minimums)


def supports_manifest_protocols(manifest: Manifest, compiled: CompiledConstants) -> bool:
    """SupportsManifestProtocols (ReleaseManifest.cpp:180-189)."""
    minimums = manifest.minimums
    return (minimums["minimum_gate_protocol"] <= compiled.gate_protocol
            and minimums["minimum_commit_service_protocol"]
            <= compiled.commit_service_protocol
            and minimums["minimum_updater_protocol"] <= compiled.updater_protocol
            and minimums["cache_contract_version"] <= compiled.cache_contract_version
            and minimums["installed_tree_version"] <= 1)


# ---------------------------------------------------------------------------
# Installed tree (InstalledTree.cpp)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class TreeEntry:
    path: str
    type: str            # "file" | "directory" | "symlink"
    mode: int
    size: int
    sha256: str          # "" for directories and links
    link_target: str = ""


def require_safe_relative_path(path: str) -> None:
    """RequireSafeRelativePath (InstalledTree.cpp:50-125)."""
    if not path:
        raise FormatError("the path is empty")
    if len(path) > 4096:
        raise FormatError("the path is implausibly long")
    if "\\" in path:
        raise FormatError(f"backslashes are not path separators here: {path!r}")
    if path.startswith("/"):
        raise FormatError(f"the path is absolute: {path!r}")
    if ":" in path:
        raise FormatError(f"the path carries a drive or stream qualifier: {path!r}")
    for component in path.split("/"):
        if not component:
            raise FormatError(f"the path has an empty component: {path!r}")
        if component in (".", ".."):
            raise FormatError(f"the path contains a relative component: {path!r}")
        if component.endswith(".") or component.endswith(" "):
            raise FormatError(f"a path component ends with a dot or space: {path!r}")
        if any(ord(character) < 0x20 or ord(character) == 0x7F for character in component):
            raise FormatError(f"a path component contains a control character: {path!r}")


def reject_appledouble_path(path: str) -> None:
    """Refuse metadata paths which must never enter a signed or staged tree."""
    for component in path.split("/"):
        if component == "__MACOSX" or component.startswith("._"):
            raise FormatError(f"AppleDouble metadata is not allowed: {path!r}")


def _utf16_order(path: str) -> bytes:
    # FString::Compare(CaseSensitive) compares TCHAR (UTF-16) code units.
    return path.encode("utf-16-be")


def parse_installed_tree(raw: bytes) -> Tuple[str, List[TreeEntry]]:
    """ParseInstalledTreeV1 (InstalledTree.cpp:127-350). Returns (platform, entries)."""
    what = "installed tree"
    document = parse_canonical(raw, what)
    require_exact_members(document, ("entries", "format", "platform", "version"), what)
    if read_string(document, "format", what) != INSTALLED_TREE_FORMAT:
        raise FormatError(f"installed tree format is not {INSTALLED_TREE_FORMAT!r}")
    if read_unsigned32(document, "version", what) != 1:
        raise FormatError("installed tree version is not 1")
    platform = read_string(document, "platform", what)
    entries = document["entries"]
    if not isinstance(entries, list):
        raise FormatError("installed tree field 'entries' is not an array")
    seen = set()
    folded = set()
    previous = None
    parsed = []
    for index, element in enumerate(entries):
        entry_what = f"installed tree entry {index}"
        require_exact_members(element, ("link_target", "mode", "path", "sha256",
                                        "size", "type"), entry_what)
        path = read_string(element, "path", entry_what)
        require_safe_relative_path(path)
        reject_appledouble_path(path)
        if path in INVENTORY_SIDECARS:
            raise FormatError("the inventory may not list itself or its signature")
        if path in seen:
            raise FormatError(f"an installed tree path appears twice: {path!r}")
        if path.lower() in folded:
            raise FormatError(f"two installed tree paths differ only by case: {path!r}")
        if previous is not None and _utf16_order(path) <= _utf16_order(previous):
            raise FormatError(f"installed tree path {path!r} does not follow {previous!r}")
        seen.add(path)
        folded.add(path.lower())
        previous = path
        entry_type = read_string(element, "type", entry_what)
        if entry_type not in ("file", "directory", "symlink"):
            raise FormatError(f"{entry_what} has unknown type {entry_type!r}")
        mode = read_unsigned(element, "mode", entry_what)
        if mode & ~0o7777:
            raise FormatError(f"{entry_what} has mode bits outside the permission set")
        if mode & 0o7000:
            raise FormatError(f"{entry_what} requests setuid/setgid/sticky")
        size = read_unsigned(element, "size", entry_what)
        digest = read_string(element, "sha256", entry_what)
        link_target = read_string(element, "link_target", entry_what)
        if entry_type == "file":
            parse_lower_hex(digest, 32, f"{entry_what}.sha256")
            if link_target:
                raise FormatError(f"{entry_what} is a file but names a link target")
        elif entry_type == "directory":
            if size != 0 or digest or link_target:
                raise FormatError(f"{entry_what} is a directory with file fields set")
        else:
            if size != 0 or digest:
                raise FormatError(f"{entry_what} is a link with file fields set")
            require_safe_relative_path(link_target)
        parsed.append(TreeEntry(path, entry_type, mode, size, digest, link_target))
    return platform, parsed


def sort_installed_tree_entries(entries: Sequence[TreeEntry]) -> List[TreeEntry]:
    """Return entries in the canonical order compute_installed_tree_root needs."""
    return sorted(entries, key=lambda entry: _utf16_order(entry.path))


def compute_installed_tree_root(platform: str, entries: Sequence[TreeEntry],
                                version: int = 1) -> str:
    """ComputeInstalledTreeRoot (InstalledTree.cpp:352-413): length-prefixed LE."""
    hasher = hashlib.sha256()

    def text(value: str) -> None:
        encoded = value.encode("utf-8")
        hasher.update(struct.pack("<Q", len(encoded)))
        hasher.update(encoded)

    def number(value: int) -> None:
        hasher.update(struct.pack("<Q", value))

    text(INSTALLED_TREE_FORMAT)
    number(version)
    text(platform)
    number(len(entries))
    previous = None
    for entry in entries:
        if previous is not None and _utf16_order(entry.path) <= _utf16_order(previous):
            raise FormatError("installed tree entries are not in canonical order")
        previous = entry.path
        text(entry.path)
        text(entry.type)
        number(entry.mode)
        number(entry.size)
        # FWoTSha256 defaults to 32 zero bytes for directories and links.
        hasher.update(bytes.fromhex(entry.sha256) if entry.sha256 else bytes(32))
        text(entry.link_target)
    return hasher.hexdigest()


def build_installed_tree(platform: str, entries: Sequence[TreeEntry]) -> bytes:
    ordered = sorted(entries, key=lambda entry: _utf16_order(entry.path))
    document = {
        "entries": [{"link_target": entry.link_target, "mode": entry.mode,
                     "path": entry.path, "sha256": entry.sha256, "size": entry.size,
                     "type": entry.type} for entry in ordered],
        "format": INSTALLED_TREE_FORMAT,
        "platform": platform,
        "version": 1,
    }
    raw = canonical_bytes(document, "installed tree")
    parse_installed_tree(raw)
    return raw


# ---------------------------------------------------------------------------
# Archive rules (ArchiveRules.cpp) and the staging ZIP reader (ReleaseStaging.cpp)
# ---------------------------------------------------------------------------

_RESERVED_DEVICES = frozenset(
    ["con", "prn", "aux", "nul"] + [f"com{n}" for n in range(1, 10)]
    + [f"lpt{n}" for n in range(1, 10)])
_APPROVED_PUNCTUATION = frozenset(" ._-+()[]@")


def is_approved_archive_component(component: str) -> bool:
    return all((character.isascii() and character.isalnum())
               or character in _APPROVED_PUNCTUATION for character in component)


def is_reserved_device_name(component: str) -> bool:
    return component.split(".", 1)[0].lower() in _RESERVED_DEVICES


@dataclass
class ZipMember:
    path: str            # trailing slash removed
    type: str            # file | directory | symlink
    mode: int
    size: int
    method: int
    crc32: int
    compressed_size: int
    data_offset: int


def _u16(data, offset: int) -> int:
    if offset + 2 > len(data):
        raise FormatError("a ZIP field extends beyond the archive")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset: int) -> int:
    if offset + 4 > len(data):
        raise FormatError("a ZIP field extends beyond the archive")
    return struct.unpack_from("<I", data, offset)[0]


def _zip_name(data, offset: int, length: int) -> str:
    if offset + length > len(data):
        raise FormatError("a ZIP name extends beyond the archive")
    raw = bytes(data[offset:offset + length])
    if b"\x00" in raw:
        raise FormatError("a ZIP name contains NUL")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise FormatError("a ZIP name is not valid UTF-8") from exc


def parse_staging_zip(data) -> List[ZipMember]:
    """Port of WoTReleaseStageParseZip (ReleaseStaging.cpp:482-676)."""
    size = len(data)
    if size < 22:
        raise FormatError("the release archive is not a ZIP file")
    end_offset = -1
    for offset in range(size - 22, max(0, size - 22 - 65535) - 1, -1):
        if _u32(data, offset) == ZIP_END and offset + 22 + _u16(data, offset + 20) == size:
            end_offset = offset
            break
    if end_offset < 0:
        raise FormatError("the ZIP end-of-central-directory record is absent or malformed")
    disk, central_disk = _u16(data, end_offset + 4), _u16(data, end_offset + 6)
    disk_entries, total = _u16(data, end_offset + 8), _u16(data, end_offset + 10)
    central_size, central_offset = _u32(data, end_offset + 12), _u32(data, end_offset + 16)
    if disk or central_disk or disk_entries != total \
            or central_offset + central_size != end_offset:
        raise FormatError("the ZIP central directory is split, ZIP64, or inconsistent")
    members: List[ZipMember] = []
    ranges: List[Tuple[int, int]] = []
    cursor = central_offset
    for _ in range(total):
        if _u32(data, cursor) != ZIP_CENTRAL:
            raise FormatError("a ZIP central header is truncated or malformed")
        flags, method = _u16(data, cursor + 8), _u16(data, cursor + 10)
        crc, compressed = _u32(data, cursor + 16), _u32(data, cursor + 20)
        uncompressed = _u32(data, cursor + 24)
        name_length, extra_length = _u16(data, cursor + 28), _u16(data, cursor + 30)
        comment_length, start_disk = _u16(data, cursor + 32), _u16(data, cursor + 34)
        external, local_offset = _u32(data, cursor + 38), _u32(data, cursor + 42)
        central_end = cursor + 46 + name_length + extra_length + comment_length
        if central_end > end_offset or start_disk != 0 or 0xFFFFFFFF in (
                compressed, uncompressed, local_offset) \
                or (flags & ~ZIP_UTF8_FLAG) or method not in (0, 8):
            raise FormatError("a ZIP entry uses an unsupported feature")
        name = _zip_name(data, cursor + 46, name_length)
        if _u32(data, local_offset) != ZIP_LOCAL:
            raise FormatError("a ZIP local header is truncated or malformed")
        local_name_length = _u16(data, local_offset + 26)
        local_extra_length = _u16(data, local_offset + 28)
        local_name = _zip_name(data, local_offset + 30, local_name_length)
        data_offset = local_offset + 30 + local_name_length + local_extra_length
        data_end = data_offset + compressed
        if (_u16(data, local_offset + 6) != flags or _u16(data, local_offset + 8) != method
                or _u32(data, local_offset + 14) != crc
                or _u32(data, local_offset + 18) != compressed
                or _u32(data, local_offset + 22) != uncompressed
                or local_name != name or data_end > central_offset
                or data_offset > 0xFFFFFFFF):
            raise FormatError(f"a ZIP central/local header disagreement: {name!r}")
        for start, stop in ranges:
            if local_offset < stop and data_end > start:
                raise FormatError(f"two ZIP local records overlap: {name!r}")
        ranges.append((local_offset, data_end))
        path = name[:-1] if name.endswith("/") else name
        unix_mode = (external >> 16) & 0xFFFF
        unix_type = unix_mode & 0o170000
        if unix_type == 0o120000 or external & 0x400:
            member_type, mode = "symlink", unix_mode & 0o777
        elif unix_type == 0o040000 or name.endswith("/") or external & 0x10:
            member_type, mode = "directory", (unix_mode & 0o777) or 0o755
            uncompressed = 0
        else:
            member_type, mode = "file", (unix_mode & 0o777) or 0o644
        members.append(ZipMember(path, member_type, mode, uncompressed, method, crc,
                                 compressed, data_offset))
        cursor = central_end
    if cursor != end_offset:
        raise FormatError("the ZIP central directory has unparsed trailing bytes")
    return members


def validate_archive_index(members: Sequence[ZipMember], layout_platform: str) -> str:
    """ValidateArchiveIndex (ArchiveRules.cpp) for the layout; returns the root name."""
    if not members:
        raise FormatError("the archive index is empty")
    if len(members) > ARCHIVE_MAX_ENTRIES:
        raise FormatError("the archive declares too many entries")
    seen = set()
    folded = set()
    total = 0
    for member in members:
        require_safe_relative_path(member.path)
        reject_appledouble_path(member.path)
        components = member.path.split("/")
        if len(components) > ARCHIVE_MAX_DEPTH:
            raise FormatError(f"the entry is nested too deeply: {member.path!r}")
        for component in components:
            if not is_approved_archive_component(component):
                raise FormatError(
                    f"an archive path component is outside the approved ASCII set: "
                    f"{member.path!r}")
            if is_reserved_device_name(component):
                raise FormatError(f"the entry names a reserved device: {member.path!r}")
        if member.path in seen:
            raise FormatError(f"the archive lists one path twice: {member.path!r}")
        if member.path.lower() in folded:
            raise FormatError(f"two archive paths differ only by case: {member.path!r}")
        seen.add(member.path)
        folded.add(member.path.lower())
        if member.size > ARCHIVE_MAX_ENTRY_BYTES or member.size > ARCHIVE_MAX_TOTAL_BYTES \
                or total > ARCHIVE_MAX_TOTAL_BYTES - member.size:
            raise FormatError(f"the archive exceeds its size limits at {member.path!r}")
        total += member.size
        if member.type == "symlink" and layout_platform == "Win64":
            raise FormatError(f"a Win64 archive may not contain links: {member.path!r}")
    root = members[0].path.split("/", 1)[0]
    for member in members:
        if member.path.split("/", 1)[0] != root:
            raise FormatError(f"the archive has more than one top-level entry: "
                              f"{member.path!r}")
    if layout_platform == "Mac" and not root.endswith(".app"):
        raise FormatError("a Mac archive root must be an .app bundle")
    if layout_platform == "Win64" and root.endswith(".app"):
        raise FormatError("a Win64 archive root must not be an .app bundle")
    return root


def _inflate_member(data, member: ZipMember) -> Tuple[str, int]:
    """Inflate + CRC (ReleaseStaging.cpp:678-719); returns (sha256, size)."""
    if member.size > INT32_MAX:
        raise FormatError(f"a ZIP member cannot fit in a bounded buffer: {member.path!r}")
    compressed = data[member.data_offset:member.data_offset + member.compressed_size]
    digest = hashlib.sha256()
    crc = 0
    produced = 0
    if member.method == 0:
        if member.compressed_size != member.size:
            raise FormatError(f"a stored ZIP member has unequal sizes: {member.path!r}")
        for start in range(0, len(compressed), 1 << 20):
            chunk = compressed[start:start + (1 << 20)]
            digest.update(chunk)
            crc = zlib.crc32(chunk, crc)
        produced = len(compressed)
    else:
        inflater = zlib.decompressobj(-15)
        for start in range(0, len(compressed), 1 << 20):
            chunk = inflater.decompress(compressed[start:start + (1 << 20)])
            produced += len(chunk)
            if produced > member.size:
                raise FormatError(f"a deflated ZIP member overflows: {member.path!r}")
            digest.update(chunk)
            crc = zlib.crc32(chunk, crc)
        tail = inflater.flush()
        produced += len(tail)
        digest.update(tail)
        crc = zlib.crc32(tail, crc)
        if not inflater.eof:
            raise FormatError(f"a deflated ZIP member is corrupt: {member.path!r}")
    if produced != member.size:
        raise FormatError(f"a ZIP member inflates to the wrong size: {member.path!r}")
    if (crc & 0xFFFFFFFF) != member.crc32:
        raise FormatError(f"a ZIP member CRC does not match: {member.path!r}")
    return digest.hexdigest(), produced


@dataclass
class StagedArchive:
    root: str
    inventory_raw: bytes
    inventory_platform: str
    entries: List[TreeEntry]            # the rehashed tree, canonical order
    tree_root: str


def _win64_mode_projection_ok(entry_type: str, mode: int) -> bool:
    # SetModeOnHeldWindowsHandle (NativeHandlesWindows.cpp:1162-1178).
    if entry_type == "directory":
        return mode in (0o755, 0o555)
    return mode in (0o644, 0o444)


def stage_archive(path: Path, platform: str) -> StagedArchive:
    """Everything StageRelease checks about the archive bytes (ReleaseStaging.cpp:1343-1664).

    The native code extracts, then rehashes the held files; the result is the same
    tree this function computes from the ZIP directly. Mode handling follows the
    host projection: exact POSIX bits on Mac (NativeHandlesMac.mm:883-919), and the
    only four modes Windows can represent on Win64 (NativeHandlesWindows.cpp:1135-1178).
    """
    with Path(path).open("rb") as handle:
        file_size = handle.seek(0, 2)
        if file_size < 22:
            raise FormatError("the release archive is not a ZIP file")
        if file_size > INT32_MAX:
            # ReleaseStaging.cpp:334-339: the download seam holds the archive in
            # one int32-sized byte array.
            raise FormatError("the release archive exceeds the int32 byte-array limit")
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
            members = parse_staging_zip(data)
            root = validate_archive_index(members, platform)
            directories = {member.path for member in members
                           if member.type == "directory"}
            for member in members:
                if member.type == "symlink":
                    raise FormatError(
                        f"release staging never materializes archive links: {member.path!r}")
                parent = member.path.rsplit("/", 1)[0] if "/" in member.path else ""
                if parent and parent not in directories:
                    raise FormatError(
                        f"a ZIP member has no explicit parent directory: {member.path!r}")
                if platform == "Win64" and not _win64_mode_projection_ok(member.type,
                                                                         member.mode):
                    raise FormatError(
                        "the signed POSIX mode has no exact Win64 filesystem projection: "
                        f"{member.path!r} mode {member.mode:o}")
            inventory_raw = None
            actual: List[TreeEntry] = []
            prefix = root + "/"
            for member in members:
                if member.type == "file":
                    digest, size = _inflate_member(data, member)
                else:
                    digest, size = "", 0
                if member.path == root:
                    if member.type != "directory":
                        raise FormatError("the archive root is not a directory")
                    continue
                relative = member.path[len(prefix):]
                if relative == INVENTORY_NAME:
                    start = member.data_offset
                    compressed = bytes(data[start:start + member.compressed_size])
                    inventory_raw = compressed if member.method == 0 else \
                        zlib.decompress(compressed, -15)
                    continue
                if relative == INVENTORY_NAME + ".sig":
                    continue   # removed by staging before the walk (ReleaseStaging.cpp:1570-1583)
                actual.append(TreeEntry(relative, member.type, member.mode, size, digest))
    if root not in directories:
        raise FormatError("the archive root has no explicit directory entry")
    if inventory_raw is None:
        raise FormatError(f"the archive does not contain {INVENTORY_NAME}")
    inventory_platform, inventory = parse_installed_tree(inventory_raw)
    if inventory_platform != platform:
        raise FormatError("the installed-tree inventory is for another platform")
    actual.sort(key=lambda entry: _utf16_order(entry.path))
    expected = {entry.path: entry for entry in inventory}
    for entry in actual:
        signed = expected.get(entry.path)
        if signed is None:
            raise FormatError(f"the staged tree contains an unsigned extra entry: "
                              f"{entry.path!r}")
        if (signed.type, signed.size, signed.sha256, signed.mode) != (
                entry.type, entry.size, entry.sha256, entry.mode):
            raise FormatError(f"the staged entry differs from the signed tree: "
                              f"{entry.path!r}")
    if len(actual) != len(inventory):
        raise FormatError("the staged tree is missing one or more signed entries")
    tree_root = compute_installed_tree_root(platform, actual)
    return StagedArchive(root=root, inventory_raw=inventory_raw,
                         inventory_platform=inventory_platform, entries=actual,
                         tree_root=tree_root)


# ---------------------------------------------------------------------------
# Deterministic update-archive construction (raw copy, no recompression)
# ---------------------------------------------------------------------------

@dataclass
class _OutputMember:
    name: str                # as stored (directories end with "/")
    external: int
    method: int
    crc32: int
    compressed_size: int
    size: int
    source_offset: Optional[int] = None   # raw copy from the source archive
    payload: Optional[bytes] = None       # bytes we author (stored)


@dataclass
class RepackResult:
    root: str
    entries: List[TreeEntry]
    inventory_raw: bytes
    tree_root: str
    dropped_metadata: List[str] = field(default_factory=list)
    source_mode_policy: str = ""


def _is_mac_metadata(path: str) -> bool:
    components = path.split("/")
    return "__MACOSX" in components or components[-1].startswith("._") \
        or components[-1] == ".DS_Store"


def _source_members(source: zipfile.ZipFile, platform: str
                    ) -> Tuple[Dict[str, zipfile.ZipInfo], Dict[str, zipfile.ZipInfo],
                               List[str]]:
    """Split a source archive into (files, directories, dropped metadata) by path."""
    files: Dict[str, zipfile.ZipInfo] = {}
    directories: Dict[str, zipfile.ZipInfo] = {}
    dropped: List[str] = []
    for info in source.infolist():
        name = info.filename
        if info.flag_bits & 0x1:
            raise FormatError(f"the source archive has an encrypted member: {name!r}")
        unix_mode = (info.external_attr >> 16) & 0xFFFF
        if info.create_system == 3 and (unix_mode & 0o170000) == 0o120000:
            raise FormatError(
                f"the source archive contains a symbolic link ({name!r}); release "
                "staging never materializes links (ReleaseStaging.cpp:1395-1400)")
        path = name.rstrip("/")
        reject_appledouble_path(path)
        if platform == "Mac" and _is_mac_metadata(path):
            dropped.append(name)
            continue
        is_directory = name.endswith("/") or (
            info.create_system == 3 and (unix_mode & 0o170000) == 0o040000)
        if path in files or path in directories:
            raise FormatError(f"the source archive lists {name!r} twice")
        if not is_directory and info.compress_type not in (zipfile.ZIP_STORED,
                                                           zipfile.ZIP_DEFLATED):
            raise FormatError(f"the source member {name!r} uses compression "
                              f"method {info.compress_type}; only 0 and 8 are staged")
        (directories if is_directory else files)[path] = info
    return files, directories, dropped


def _relative_to_root(path: str, root: str) -> str:
    if not path.startswith(root + "/") or len(path) == len(root) + 1:
        raise FormatError(f"source member {path!r} is outside {root}/")
    return path[len(root) + 1:]


def _output_mode(info: Optional[zipfile.ZipInfo], entry_type: str,
                 platform: str) -> int:
    if platform == "Win64":
        # The writable Win64 projections (NativeHandlesWindows.cpp:1171).
        return 0o755 if entry_type == "directory" else 0o644
    mode = 0
    if info is not None and info.create_system == 3:
        raw = (info.external_attr >> 16) & 0xFFFF
        if raw & 0o7000:
            raise FormatError(f"{info.filename!r} requests setuid/setgid/sticky")
        mode = raw & 0o777
    if mode == 0:
        mode = 0o755 if entry_type == "directory" else 0o644
    if not mode & 0o400 or (entry_type == "directory" and not mode & 0o100):
        name = info.filename if info is not None else "(synthesized directory)"
        raise FormatError(f"{name} mode {mode:o} would lock the updater out of its "
                          "own staged tree")
    return mode


def _raw_member_bytes(handle: BinaryIO, info: zipfile.ZipInfo) -> int:
    handle.seek(info.header_offset)
    header = handle.read(30)
    if len(header) != 30 or struct.unpack_from("<I", header, 0)[0] != ZIP_LOCAL:
        raise FormatError(f"source local header for {info.filename!r} is malformed")
    name_length, extra_length = struct.unpack_from("<HH", header, 26)
    return info.header_offset + 30 + name_length + extra_length


def _hash_source_member(handle: BinaryIO, info: zipfile.ZipInfo, data_offset: int
                        ) -> str:
    handle.seek(data_offset)
    remaining = info.compress_size
    digest = hashlib.sha256()
    crc = 0
    produced = 0
    inflater = zlib.decompressobj(-15) if info.compress_type == zipfile.ZIP_DEFLATED \
        else None
    while remaining:
        chunk = handle.read(min(remaining, 1 << 20))
        if not chunk:
            raise FormatError(f"source member {info.filename!r} is truncated")
        remaining -= len(chunk)
        plain = inflater.decompress(chunk) if inflater else chunk
        digest.update(plain)
        crc = zlib.crc32(plain, crc)
        produced += len(plain)
    if inflater:
        tail = inflater.flush()
        digest.update(tail)
        crc = zlib.crc32(tail, crc)
        produced += len(tail)
        if not inflater.eof:
            raise FormatError(f"source member {info.filename!r} is corrupt")
    if produced != info.file_size or (crc & 0xFFFFFFFF) != info.CRC:
        raise FormatError(f"source member {info.filename!r} fails its CRC/size check")
    return digest.hexdigest()


def _project_guarded_archive(source_zip: Path, platform: str
                             ) -> Tuple[RepackResult,
                                        Dict[str, Tuple[zipfile.ZipInfo, int, str]]]:
    """Project a guarded zip into the exact installed tree without writing it."""
    source_root = GUARDED_ZIP_ROOTS[platform]
    expected_root = UPDATE_TREE_ROOTS[platform]
    projection_root = GUARDED_TREE_ROOTS[platform]
    with zipfile.ZipFile(source_zip) as source, Path(source_zip).open("rb") as handle:
        files, source_directories, dropped = _source_members(source, platform)
        tree_files: Dict[str, Tuple[zipfile.ZipInfo, int, str]] = {}
        directory_infos: Dict[str, Optional[zipfile.ZipInfo]] = {}
        if platform == "Win64":
            launcher = source_root + "/WoT Reborn.exe"
            if launcher not in files:
                raise FormatError("the Windows first-install archive has no stable launcher")
            outside = [path for path in files
                       if path != launcher and not path.startswith(projection_root + "/")]
            if outside:
                raise FormatError(
                    f"Windows first-install files are outside Game/: {outside[:3]}")
        for path, info in source_directories.items():
            if path in (source_root, projection_root):
                continue
            if platform == "Win64" and not path.startswith(projection_root + "/"):
                raise FormatError(f"Windows first-install directory {path!r} is outside Game/")
            directory_infos[_relative_to_root(path, projection_root)] = info
        for path, info in files.items():
            if platform == "Win64" and path == source_root + "/WoT Reborn.exe":
                continue
            relative = _relative_to_root(path, projection_root)
            if relative in INVENTORY_SIDECARS:
                raise FormatError(f"the source already carries {relative}")
            data_offset = _raw_member_bytes(handle, info)
            tree_files[relative] = (info, data_offset,
                                    _hash_source_member(handle, info, data_offset))
        # Every ancestor gets an explicit entry: staging refuses a member whose
        # parent directory is not itself listed (ReleaseStaging.cpp:1402-1415).
        for relative in list(tree_files) + list(directory_infos):
            parts = relative.split("/")
            for depth in range(1, len(parts)):
                directory_infos.setdefault("/".join(parts[:depth]), None)
        clash = set(tree_files) & set(directory_infos)
        if clash:
            raise FormatError(f"source paths are both file and directory: {sorted(clash)}")

        entries: List[TreeEntry] = []
        for relative, directory_info in directory_infos.items():
            entries.append(TreeEntry(relative, "directory", _output_mode(
                directory_info, "directory", platform), 0, ""))
        for relative, (info, _, digest) in tree_files.items():
            entries.append(TreeEntry(relative, "file",
                                     _output_mode(info, "file", platform),
                                     info.file_size, digest))
        entries.sort(key=lambda entry: _utf16_order(entry.path))
        inventory_raw = build_installed_tree(platform, entries)
        index = [ZipMember(expected_root, "directory", 0o755, 0, 0, 0, 0, 0)]
        index += [ZipMember(f"{expected_root}/{entry.path}", entry.type, entry.mode,
                            entry.size, 0, 0, 0, 0) for entry in entries]
        index.append(ZipMember(f"{expected_root}/{INVENTORY_NAME}", "file", 0o644,
                               len(inventory_raw), 0, 0, 0, 0))
        validate_archive_index(index, platform)
        tree_root = compute_installed_tree_root(platform, entries)

    result = RepackResult(
        root=expected_root, entries=entries, inventory_raw=inventory_raw,
        tree_root=tree_root, dropped_metadata=sorted(dropped),
        source_mode_policy=("win64-projection-0644-0755"
                            if platform == "Win64" else "mac-source-posix-bits"))
    return result, tree_files


def project_guarded_archive_tree(source_zip: Path, platform: str) -> RepackResult:
    """Return the tree that repacking ``source_zip`` would sign and install."""
    result, _tree_files = _project_guarded_archive(source_zip, platform)
    return result


def repack_update_archive(source_zip: Path, destination: Path,
                          platform: str) -> RepackResult:
    """Build the updater archive from a guarded release zip, deterministically.

    The guarded zip is what the first-run funnel proves, but it is not a staging
    archive: Win64 is written by zipfile with no directory entries and host modes
    0666/0777 (build_stripped_pak.py:3326-3347), both refused by staging
    (ReleaseStaging.cpp:1402-1415; NativeHandlesWindows.cpp:1171-1178), and neither
    carries installed-tree-v1.json (ReleaseStaging.cpp:1527-1533). This rewrites
    only the CONTAINER: every file's compressed stream is copied byte for byte
    (so no zlib version can change the output), every directory gets an explicit
    entry, modes are the platform projection, the signed inventory is added
    (stored), and headers carry fixed timestamps and no extras or descriptors.
    """
    result, tree_files = _project_guarded_archive(source_zip, platform)
    expected_root = result.root

    outputs: List[_OutputMember] = [_OutputMember(
        expected_root + "/", (stat.S_IFDIR | 0o755) << 16 | 0x10, 0, 0, 0, 0,
        payload=b"")]
    for entry in result.entries:
        name = f"{expected_root}/{entry.path}"
        if entry.type == "directory":
            outputs.append(_OutputMember(
                name + "/", (stat.S_IFDIR | entry.mode) << 16 | 0x10, 0, 0, 0, 0,
                payload=b""))
        else:
            info, data_offset, _ = tree_files[entry.path]
            outputs.append(_OutputMember(
                name, (stat.S_IFREG | entry.mode) << 16, info.compress_type,
                info.CRC, info.compress_size, info.file_size,
                source_offset=data_offset))
    outputs.append(_OutputMember(
        f"{expected_root}/{INVENTORY_NAME}", (stat.S_IFREG | 0o644) << 16, 0,
        zlib.crc32(result.inventory_raw) & 0xFFFFFFFF, len(result.inventory_raw),
        len(result.inventory_raw), payload=result.inventory_raw))
    with Path(source_zip).open("rb") as handle:
        _write_zip(destination, outputs, handle)
    return result


def _write_zip(destination: Path, members: Sequence[_OutputMember],
               source: BinaryIO) -> None:
    if len(members) > 0xFFFF:
        raise FormatError("the update archive needs more than 65535 entries (ZIP64)")
    temporary = Path(str(destination) + ".partial")
    central = bytearray()
    with temporary.open("wb") as out:
        for member in members:
            name = member.name.encode("ascii")
            offset = out.tell()
            if offset > 0xFFFFFFFE or member.compressed_size > 0xFFFFFFFE \
                    or member.size > 0xFFFFFFFE:
                raise FormatError("the update archive would need ZIP64")
            out.write(struct.pack("<IHHHHHIIIHH", ZIP_LOCAL, ZIP_VERSION_NEEDED, 0,
                                  member.method, ZIP_FIXED_DOS_TIME, ZIP_FIXED_DOS_DATE,
                                  member.crc32, member.compressed_size, member.size,
                                  len(name), 0))
            out.write(name)
            if member.payload is not None:
                out.write(member.payload)
            else:
                source.seek(member.source_offset)
                remaining = member.compressed_size
                while remaining:
                    chunk = source.read(min(remaining, 1 << 20))
                    if not chunk:
                        raise FormatError(f"source data for {member.name!r} is truncated")
                    out.write(chunk)
                    remaining -= len(chunk)
            central += struct.pack("<IHHHHHHIIIHHHHHII", ZIP_CENTRAL, ZIP_VERSION_MADE_BY,
                                   ZIP_VERSION_NEEDED, 0, member.method,
                                   ZIP_FIXED_DOS_TIME, ZIP_FIXED_DOS_DATE, member.crc32,
                                   member.compressed_size, member.size, len(name), 0, 0,
                                   0, 0, member.external, offset)
            central += name
        central_offset = out.tell()
        out.write(central)
        out.write(struct.pack("<IHHHHIIH", ZIP_END, 0, 0, len(members), len(members),
                              len(central), central_offset, 0))
        final_size = out.tell()
    if final_size > INT32_MAX:
        temporary.unlink()
        raise FormatError(
            f"the update archive is {final_size} bytes; the native download seam "
            "holds at most 2147483647 (ReleaseStaging.cpp:334-339)")
    temporary.replace(destination)


# ---------------------------------------------------------------------------
# Guarded release input (build_stripped_pak.py stage_package)
# ---------------------------------------------------------------------------

@dataclass
class GuardedRelease:
    platform: str
    release_json: Path
    release_json_sha256: str
    zip_path: Path
    document: dict
    main_relative: str
    products: Dict[str, dict]
    release_sequence: int = 0
    configuration: str = TEST_CONFIGURATION
    test_twin: Optional[dict] = None
    dmg_path: Optional[Path] = None
    dmg_sidecar_path: Optional[Path] = None
    dmg: Optional[dict] = None


TEST_TWIN_FIELDS = (
    "asset", "asset_sha256", "asset_size", "configuration",
    "container_bytes", "cooked_container_inventory_receipt_sha256",
    "format", "git_commit", "human_version", "project_package_census_sha256",
    "release_json_sha256", "release_sequence", "source_snapshot_sha256",
    "staged_tree_sha256", "zen_lifetime_receipt_sha256")

GUARDED_DMG_FIELDS = (
    "background_sha256", "installed_tree_root", "name", "notarized", "sha256",
    "size", "verify_sidecar", "volume_name")
GUARDED_DMG_SIDECAR_FIELDS = ("name", "sha256", "size")

MAC_SIGNING_RECEIPT_FIELDS = (
    "notarization_submission_id", "signing", "stapled", "team_id")


def _is_lower_sha256(value) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def validate_mac_signing_receipt(receipt: dict, what: str) -> Optional[dict]:
    """Validate optional v1 Mac signing facts without invalidating old receipts."""
    present = set(receipt).intersection(MAC_SIGNING_RECEIPT_FIELDS)
    if not present:
        return None
    if present != set(MAC_SIGNING_RECEIPT_FIELDS):
        raise FormatError(f"{what} has incomplete Mac signing facts")
    signing = receipt["signing"]
    team_id = receipt["team_id"]
    submission_id = receipt["notarization_submission_id"]
    stapled = receipt["stapled"]
    if signing not in {"adhoc", "developer-id"} or type(stapled) is not bool:
        raise FormatError(f"{what} has invalid Mac signing facts")
    if signing == "adhoc":
        if team_id is not None or submission_id is not None or stapled:
            raise FormatError(f"{what} ad-hoc signing facts are contradictory")
    else:
        if not isinstance(team_id, str) or not team_id:
            raise FormatError(f"{what} Developer ID signing has no team id")
        if submission_id is not None and (
                not isinstance(submission_id, str) or not submission_id):
            raise FormatError(f"{what} has an invalid notarization submission id")
        if stapled != (submission_id is not None):
            raise FormatError(f"{what} stapling and notarization facts disagree")
    return {key: receipt[key] for key in MAC_SIGNING_RECEIPT_FIELDS}


def parse_test_twin(document: dict, what: str) -> dict:
    """The Shipping release's binding to the Test release the funnel proved.

    build_stripped_pak.py writes it in the shipping-package step from the same
    ISO's Test package: one commit, one source snapshot, one Zen-sponsored
    import+cook and one project package census behind both artifacts.
    """
    twin = document.get("test_twin")
    if not isinstance(twin, dict) or set(twin) != set(TEST_TWIN_FIELDS):
        raise FormatError(f"{what} has no exact test_twin binding to its Test release")
    snapshot = document.get("source_snapshot")
    try:
        expected_asset = guarded_install_zip_asset(
            str(document.get("platform")), str(document.get("git_commit")),
            TEST_CONFIGURATION)
    except FormatError:
        expected_asset = None
    if (isinstance(twin.get("release_sequence"), bool)
            or not isinstance(twin.get("release_sequence"), int)
            or twin.get("release_sequence") != document.get("release_sequence")):
        raise FormatError(
            f"{what} test_twin release_sequence does not bind this release")
    validate_human_version(
        twin.get("human_version"), f"{what} test_twin.human_version",
        allow_empty=True)
    if (twin.get("format") != TEST_TWIN_FORMAT
            or twin.get("configuration") != TEST_CONFIGURATION
            or twin.get("git_commit") != document.get("git_commit")
            or not isinstance(snapshot, dict)
            or twin.get("source_snapshot_sha256") != snapshot.get("sha256")
            or twin.get("asset") != expected_asset
            or isinstance(twin.get("asset_size"), bool)
            or not isinstance(twin.get("asset_size"), int)
            or twin["asset_size"] <= 0
            or not isinstance(twin.get("container_bytes"), dict)):
        raise FormatError(f"{what} test_twin does not bind the same-commit Test release")
    for key in ("asset_sha256", "cooked_container_inventory_receipt_sha256",
                "project_package_census_sha256", "release_json_sha256",
                "staged_tree_sha256", "zen_lifetime_receipt_sha256"):
        if not _is_lower_sha256(twin.get(key)):
            raise FormatError(f"{what} test_twin {key} is not a lower-case sha256")
    return twin


def load_guarded_release(release_json: Path, platform: str) -> GuardedRelease:
    """Read a guarded release.json (build_stripped_pak.py:2983-3041) and its zip.

    It is NOT canonical JSON (pretty-printed; zen_lifetime carries a float) and it
    embeds host paths, so it is an INPUT to signing, never a signed or published
    object; only derived, canonical fields reach the manifest.
    """
    release_json = Path(release_json)
    raw = release_json.read_bytes()
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise FormatError(f"{release_json} is not JSON: {exc}") from exc
    if not isinstance(document, dict) or document.get("format") != GUARDED_RELEASE_FORMAT:
        raise FormatError(f"{release_json} is not a {GUARDED_RELEASE_FORMAT} manifest")
    if document.get("platform") != platform:
        raise FormatError(f"{release_json} is for {document.get('platform')!r}, "
                          f"not {platform!r}")
    audit = document.get("audit")
    if not isinstance(audit, dict) or audit.get("verdict") != "pass":
        raise FormatError(f"{release_json} does not carry a passing absence audit")
    asset = document.get("asset")
    if not isinstance(asset, str) or "/" in asset or "\\" in asset or not asset:
        raise FormatError(f"{release_json} asset is not a bare file name")
    zip_path = release_json.parent / asset
    digest, size = sha256_file(zip_path)
    if digest != document.get("sha256") or size != document.get("size"):
        raise FormatError(f"{zip_path} does not match the sha256/size in {release_json}")
    commit = document.get("git_commit")
    if not isinstance(commit, str) or not re.match(r"^[0-9a-f]{7,40}$", commit):
        raise FormatError(f"{release_json} git_commit is not a hex commit")
    release_sequence = document.get("release_sequence")
    if (isinstance(release_sequence, bool)
            or not isinstance(release_sequence, int)
            or not 0 <= release_sequence <= INT64_MAX):
        raise FormatError(
            f"{release_json} release_sequence is not a non-negative int64")
    products_row = document.get("products")
    receipt = products_row.get("receipt") if isinstance(products_row, dict) else None
    products = receipt.get("products") if isinstance(receipt, dict) else None
    stage_root = receipt.get("stage_root") if isinstance(receipt, dict) else None
    if not isinstance(products, dict) or set(products) != {"WoT"} \
            or not isinstance(stage_root, str):
        raise FormatError(f"{release_json} has no WoT product receipt")
    configuration = document.get("configuration")
    if configuration not in GUARDED_CONFIGURATIONS:
        raise FormatError(f"{release_json} configuration {configuration!r} is not a "
                          f"guarded configuration {GUARDED_CONFIGURATIONS}")
    if receipt.get("configuration") != configuration:
        raise FormatError(f"{release_json} product receipt configuration differs from "
                          "the release configuration")
    signing_facts = validate_mac_signing_receipt(
        receipt, f"{release_json} product receipt")
    if platform != "Mac" and signing_facts is not None:
        raise FormatError(f"{release_json} non-Mac product receipt has Mac signing facts")
    if asset != guarded_install_zip_asset(platform, commit, configuration):
        raise FormatError(f"{release_json} asset {asset!r} is not the {configuration} "
                          "guarded zip name")
    relative = {}
    for target, row in products.items():
        product_path = row.get("path") if isinstance(row, dict) else None
        if not isinstance(product_path, str) or not product_path.startswith(stage_root + "/"):
            raise FormatError(f"{release_json} {target} product path is malformed")
        inside = product_path[len(stage_root) + 1:]
        if platform == "Mac":
            if not inside.startswith(GUARDED_ZIP_ROOTS["Mac"] + "/"):
                raise FormatError(f"{release_json} {target} is not inside WoT.app")
            inside = inside[len(GUARDED_ZIP_ROOTS["Mac"]) + 1:]
        relative[target] = inside
    if not main_executable_matches_configuration(relative["WoT"], platform,
                                                 configuration):
        raise FormatError(f"{release_json} main executable {relative['WoT']!r} is not "
                          f"the {configuration} game image")
    twin = (parse_test_twin(document, str(release_json))
            if configuration == SHIPPING_CONFIGURATION else None)
    dmg = None
    dmg_path = None
    dmg_sidecar_path = None
    if platform == "Mac":
        dmg = document.get("dmg")
        if not isinstance(dmg, dict) or set(dmg) != set(GUARDED_DMG_FIELDS):
            raise FormatError(f"{release_json} has no exact guarded Mac DMG binding")
        expected_dmg = guarded_install_dmg_asset(commit, configuration)
        sidecar = dmg.get("verify_sidecar")
        if (dmg.get("name") != expected_dmg
                or dmg.get("volume_name") != "WoT Reborn"
                or type(dmg.get("notarized")) is not bool
                or not _is_lower_sha256(dmg.get("background_sha256"))
                or not _is_lower_sha256(dmg.get("sha256"))
                or not _is_lower_sha256(dmg.get("installed_tree_root"))
                or isinstance(dmg.get("size"), bool)
                or not isinstance(dmg.get("size"), int) or dmg["size"] <= 0
                or not isinstance(sidecar, dict)
                or set(sidecar) != set(GUARDED_DMG_SIDECAR_FIELDS)
                or sidecar.get("name") != expected_dmg + ".verify.json"
                or not _is_lower_sha256(sidecar.get("sha256"))
                or isinstance(sidecar.get("size"), bool)
                or not isinstance(sidecar.get("size"), int) or sidecar["size"] <= 0):
            raise FormatError(f"{release_json} guarded Mac DMG binding is malformed")
        dmg_path = release_json.parent / expected_dmg
        dmg_sidecar_path = release_json.parent / sidecar["name"]
        actual_dmg = sha256_file(dmg_path)
        actual_sidecar = sha256_file(dmg_sidecar_path)
        if actual_dmg != (dmg["sha256"], dmg["size"]):
            raise FormatError(f"{dmg_path} does not match the DMG binding in {release_json}")
        if actual_sidecar != (sidecar["sha256"], sidecar["size"]):
            raise FormatError(
                f"{dmg_sidecar_path} does not match the sidecar binding in {release_json}")
        try:
            import mac_dmg
            sidecar_document = mac_dmg.verify_sidecar(dmg_path, dmg_sidecar_path)
        except (OSError, mac_dmg.DmgError) as exc:
            raise FormatError(f"{release_json} guarded Mac DMG sidecar: {exc}") from exc
        for key in ("background_sha256", "name", "notarized", "sha256", "size",
                    "volume_name", "installed_tree_root"):
            if sidecar_document[key] != dmg[key]:
                raise FormatError(
                    f"{release_json} DMG {key} differs from its canonical sidecar")
    elif "dmg" in document:
        raise FormatError(f"{release_json} carries a Mac DMG binding for {platform}")
    return GuardedRelease(platform=platform, release_json=release_json,
                          release_json_sha256=sha256_hex(raw), zip_path=zip_path,
                          document=document, main_relative=relative["WoT"],
                          products=products,
                          release_sequence=release_sequence,
                          configuration=configuration, test_twin=twin,
                          dmg_path=dmg_path, dmg_sidecar_path=dmg_sidecar_path, dmg=dmg)


def _funnel_nonnegative_number(value) -> bool:
    return type(value) in (int, float) and value >= 0


def _update_exact(value, keys, what: str) -> dict:
    if not isinstance(value, dict) or set(value) != set(keys):
        raise FormatError(f"{what} schema is not exact")
    return value


def _update_sha(value, what: str, *, optional: bool = False) -> None:
    if optional and value is None:
        return
    if not _is_lower_sha256(value):
        raise FormatError(f"{what} is not a lower-case sha256")


def _update_file_ref(value, what: str) -> None:
    value = _update_exact(value, {"path", "sha256", "size"}, what)
    if not isinstance(value["path"], str) or not value["path"]:
        raise FormatError(f"{what}.path is invalid")
    _update_sha(value["sha256"], f"{what}.sha256")
    if type(value["size"]) is not int or value["size"] <= 0:
        raise FormatError(f"{what}.size is invalid")


def _update_processes(value, what: str) -> None:
    if not isinstance(value, list) or not value:
        raise FormatError(f"{what} is not a non-empty list")
    roles = set()
    for index, process in enumerate(value):
        process_what = f"{what}[{index}]"
        process = _update_exact(
            process,
            {"pid", "ready", "release_identity_sequence", "role"},
            process_what)
        if (not isinstance(process["role"], str) or not process["role"]
                or process["role"] in roles):
            raise FormatError(f"{process_what}.role is invalid or duplicated")
        roles.add(process["role"])
        if type(process["pid"]) is not int or process["pid"] <= 0:
            raise FormatError(f"{process_what}.pid is invalid")
        if (type(process["release_identity_sequence"]) is not int
                or not 0 <= process["release_identity_sequence"] <= INT64_MAX):
            raise FormatError(
                f"{process_what}.release_identity_sequence is invalid")
        if type(process["ready"]) is not bool:
            raise FormatError(f"{process_what}.ready is invalid")


def _mac_command_evidence(value, what: str) -> dict:
    value = _update_exact(
        value, {"command", "env", "returncode", "stderr", "stdout"}, what)
    if (not isinstance(value["command"], list)
            or not value["command"]
            or not all(isinstance(argument, str) and argument
                       for argument in value["command"])
            or value["env"] != MAC_NOTARIZATION_ENV
            or type(value["returncode"]) is not int
            or value["returncode"] < 0
            or not isinstance(value["stderr"], str)
            or not isinstance(value["stdout"], str)):
        raise FormatError(f"{what} is invalid")
    return value


def _mac_command_output(value: dict) -> str:
    return value["stdout"] + "\n" + value["stderr"]


def parse_mac_notarization_evidence(codesign: dict, stapler: dict,
                                    spctl: dict) -> dict:
    """Derive notarization fields from captured command results."""
    codesign_output = _mac_command_output(codesign)
    executable_values = re.findall(
        r"^Executable=(.+)$", codesign_output, flags=re.MULTILINE)
    authorities = re.findall(
        r"^Authority=(.+)$", codesign_output, flags=re.MULTILINE)
    signatures = re.findall(
        r"^Signature=(.+)$", codesign_output, flags=re.MULTILINE)
    developer_id_authorities = [
        value for value in authorities
        if value.startswith("Developer ID Application:")
    ]
    has_adhoc = signatures == ["adhoc"] and not authorities
    has_developer_id = (
        not signatures and len(developer_id_authorities) == 1)
    if has_developer_id:
        signature_kind = "developer-id"
    elif has_adhoc:
        signature_kind = "adhoc"
    else:
        signature_kind = "other"
    team_values = re.findall(
        r"^TeamIdentifier=(.*)$", codesign_output, flags=re.MULTILINE)
    team_id = None
    if len(team_values) == 1 and team_values[0] not in {"", "not set"}:
        team_id = team_values[0]
    sources = re.findall(
        r"^source=(.+)$", _mac_command_output(spctl), flags=re.MULTILINE)
    return {
        "executable_path": (
            executable_values[0] if len(executable_values) == 1 else None),
        "notarization_ticket": stapler["returncode"] == 0,
        "signature_kind": signature_kind,
        "spctl_exit": spctl["returncode"],
        "spctl_source": sources[0] if len(sources) == 1 else None,
        "team_id": team_id,
    }


def _validate_mac_notarization_evidence(value, source_path: str,
                                        what: str) -> dict:
    value = _update_exact(value, {
        "codesign", "executable_path", "notarization_ticket",
        "signature_kind", "spctl", "spctl_exit", "spctl_source", "stapler",
        "team_id",
    }, what)
    codesign = _mac_command_evidence(value["codesign"], f"{what}.codesign")
    stapler = _mac_command_evidence(value["stapler"], f"{what}.stapler")
    spctl = _mac_command_evidence(value["spctl"], f"{what}.spctl")
    expected_commands = {
        "codesign": ["/usr/bin/codesign", "-dv", "--verbose=4", source_path],
        "stapler": ["/usr/bin/xcrun", "stapler", "validate", source_path],
        "spctl": ["/usr/sbin/spctl", "--assess", "--type", "execute",
                  "-vv", source_path],
    }
    for name, record in (("codesign", codesign), ("stapler", stapler),
                         ("spctl", spctl)):
        if record["command"] != expected_commands[name]:
            raise FormatError(f"{what}.{name}.command is invalid")
    parsed = parse_mac_notarization_evidence(codesign, stapler, spctl)
    expected_executable = (
        source_path + "/Contents/MacOS/WoT-Mac-Test")
    team_values = re.findall(
        r"^TeamIdentifier=(.*)$", _mac_command_output(codesign),
        flags=re.MULTILINE)
    if (codesign["returncode"] != 0
            or value["executable_path"] != expected_executable
            or value["signature_kind"] not in {"adhoc", "developer-id"}
            or type(value["notarization_ticket"]) is not bool
            or value["spctl_exit"] not in {0, 3}
            or (value["spctl_source"] is not None
                and (not isinstance(value["spctl_source"], str)
                     or not value["spctl_source"]))
            or (value["team_id"] is not None
                and (not isinstance(value["team_id"], str)
                     or not value["team_id"]))
            or any(value[key] != parsed[key] for key in parsed)):
        raise FormatError(f"{what} parsed fields do not match raw outputs")
    if (value["signature_kind"] == "adhoc"
            and team_values != ["not set"]):
        raise FormatError(f"{what} ad-hoc signature TeamIdentifier is invalid")
    if (value["signature_kind"] == "developer-id"
            and (len(team_values) != 1
                 or team_values[0] in {"", "not set"})):
        raise FormatError(f"{what} Developer ID TeamIdentifier is invalid")
    stapler_output = _mac_command_output(stapler)
    stapler_success = "The validate action worked" in stapler_output
    stapler_no_ticket = (
        "does not have a ticket stapled" in stapler_output.casefold())
    if (stapler_success == stapler_no_ticket
            or stapler["returncode"] != (0 if stapler_success else 65)):
        raise FormatError(f"{what}.stapler outcome contradicts its exit code")
    spctl_output = _mac_command_output(spctl)
    spctl_accepted = bool(re.search(
        r"^.+: accepted\s*$", spctl_output, flags=re.MULTILINE))
    spctl_rejected = bool(re.search(
        r"^.+: rejected\s*$", spctl_output, flags=re.MULTILINE))
    if (spctl["returncode"] == 0
            and (not spctl_accepted or spctl_rejected)):
        raise FormatError(f"{what}.spctl acceptance transcript is invalid")
    if (spctl["returncode"] == 3
            and (not spctl_rejected or spctl_accepted)):
        raise FormatError(f"{what}.spctl rejection transcript is invalid")
    if value["spctl_exit"] == 0 and value["spctl_source"] is None:
        raise FormatError(f"{what} accepted assessment has no source")
    if (value["signature_kind"] == "adhoc" and value["team_id"] is not None):
        raise FormatError(f"{what} ad-hoc signature has a team identifier")
    if (value["signature_kind"] == "developer-id"
            and value["team_id"] is None):
        raise FormatError(f"{what} Developer ID signature has no team identifier")
    return value


def _update_cleanups(value, what: str) -> None:
    if not isinstance(value, list) or not value:
        raise FormatError(f"{what} is not a non-empty list")
    attempts = set()
    for index, cleanup in enumerate(value):
        cleanup_what = f"{what}[{index}]"
        cleanup = _update_exact(
            cleanup, {"attempt", "deleted"}, cleanup_what)
        if (not isinstance(cleanup["attempt"], str)
                or not cleanup["attempt"]
                or cleanup["attempt"] in attempts
                or cleanup["deleted"] is not True):
            raise FormatError(f"{cleanup_what} is invalid")
        attempts.add(cleanup["attempt"])


def _update_observation(value, what: str) -> dict:
    value = _update_exact(
        value, {"event", "pid", "reason", "result", "sequence", "status"},
        what)
    for key in ("event", "result"):
        if not isinstance(value[key], str) or not value[key]:
            raise FormatError(f"{what}.{key} is invalid")
    for key in ("reason", "status"):
        if not isinstance(value[key], str):
            raise FormatError(f"{what}.{key} is invalid")
    if type(value["pid"]) is not int or value["pid"] <= 0:
        raise FormatError(f"{what}.pid is invalid")
    if (value["sequence"] is not None
            and (type(value["sequence"]) is not int
                 or not 0 <= value["sequence"] <= INT64_MAX)):
        raise FormatError(f"{what}.sequence is invalid")
    return value


def _release_notes_result(value, what: str) -> dict:
    value = _update_exact(
        value, {"commit", "outcome", "pid", "sequence"}, what)
    if (not isinstance(value["commit"], str)
            or value["outcome"] not in {
                "selected", "no-entry", "unavailable", "invalid",
                "not-attempted",
            }
            or type(value["pid"]) is not int or value["pid"] <= 0
            or type(value["sequence"]) is not int
            or not 0 <= value["sequence"] <= INT64_MAX):
        raise FormatError(f"{what} is invalid")
    return value


def _update_requests(value, what: str) -> None:
    if not isinstance(value, list):
        raise FormatError(f"{what} is not a list")
    keys = {
        "format", "if_range", "method", "path", "range", "range_end",
        "range_start", "response_bytes", "status", "timestamp_utc",
    }
    for index, request in enumerate(value):
        request_what = f"{what}[{index}]"
        request = _update_exact(request, keys, request_what)
        for key in ("format", "if_range", "method", "path", "range",
                    "timestamp_utc"):
            if not isinstance(request[key], str):
                raise FormatError(f"{request_what}.{key} is invalid")
        if request["format"] != "wot-funnel-loopback-request-v1":
            raise FormatError(f"{request_what}.format is invalid")
        for key in ("range_start", "range_end"):
            if (request[key] is not None
                    and (type(request[key]) is not int or request[key] < 0)):
                raise FormatError(f"{request_what}.{key} is invalid")
        for key in ("response_bytes", "status"):
            if type(request[key]) is not int or request[key] < 0:
                raise FormatError(f"{request_what}.{key} is invalid")


def _update_common_evidence(name: str, evidence: dict) -> None:
    base = _update_exact(
        evidence["base"],
        {"archive_sha256", "git_commit", "release_json_sha256",
         "release_sequence", "tree_root"},
        f"{name}.base")
    for key in ("archive_sha256", "release_json_sha256", "tree_root"):
        _update_sha(base[key], f"{name}.base.{key}")
    if not isinstance(base["git_commit"], str) or not base["git_commit"]:
        raise FormatError(f"{name}.base.git_commit is invalid")
    if (type(base["release_sequence"]) is not int
            or not 0 <= base["release_sequence"] <= INT64_MAX):
        raise FormatError(f"{name}.base.release_sequence is invalid")
    _update_file_ref(evidence["derivation_receipt"],
                     f"{name}.derivation_receipt")
    _update_cleanups(evidence["cleanup"], f"{name}.cleanup")
    _update_processes(evidence["processes"], f"{name}.processes")
    _update_requests(evidence["request_transcript"],
                     f"{name}.request_transcript")
    _update_sha(evidence["variant_tree_root"], f"{name}.variant_tree_root")
    trees = _update_exact(
        evidence["trees"],
        {"live_after", "live_before", "previous_after"},
        f"{name}.trees")
    _update_sha(trees["live_before"], f"{name}.trees.live_before")
    _update_sha(trees["live_after"], f"{name}.trees.live_after")
    _update_sha(trees["previous_after"], f"{name}.trees.previous_after",
                optional=True)


def _canonical_absolute_path(value, what: str) -> str:
    if (not isinstance(value, str) or not value.startswith("/")
            or posixpath.normpath(value) != value or "//" in value):
        raise FormatError(f"{what} is not a canonical absolute path")
    return value


def _path_is_within(value: str, root: str) -> bool:
    return value == root or value.startswith(root + "/")


def validate_update_cell_evidence(name: str, evidence: dict,
                                  platform: str = "Win64") -> None:
    """Validate the closed, publication-grade evidence for one update cell."""
    if platform not in UPDATE_EVIDENCE_KEYS_BY_PLATFORM:
        raise FormatError(f"unsupported update evidence platform {platform!r}")
    keys = UPDATE_EVIDENCE_KEYS_BY_PLATFORM[platform].get(name)
    if keys is None:
        raise FormatError(f"{name}: no update evidence schema is registered")
    evidence = _update_exact(evidence, keys, name)
    if name == "mac-uninstall":
        if platform != "Mac":
            raise FormatError("mac-uninstall evidence is Mac-only")
        paths = _update_exact(
            evidence["paths"],
            {"bundle", "bundle_parent", "data_root", "installer", "profile",
             "session_gate", "sibling_sentinel", "trash", "update_dir"},
            "mac-uninstall.paths")
        canonical = {
            key: _canonical_absolute_path(value, f"mac-uninstall.paths.{key}")
            for key, value in paths.items()
        }
        if canonical["data_root"] != canonical["profile"] + \
                "/Library/Application Support/WoT Reborn":
            raise FormatError("mac-uninstall.data_root is outside its profile")
        if canonical["trash"] != canonical["profile"] + "/.Trash":
            raise FormatError("mac-uninstall.trash is outside its profile")
        if canonical["update_dir"] != str(
                PurePosixPath(canonical["bundle"]).parent / ".wot-update"):
            raise FormatError("mac-uninstall.update_dir is not beside the bundle")
        if canonical["session_gate"] != canonical["data_root"] + \
                "/session.lock":
            raise FormatError("mac-uninstall.session_gate is outside UpdaterState")
        if canonical["bundle_parent"] != str(PurePosixPath(canonical["bundle"]).parent):
            raise FormatError("mac-uninstall.bundle_parent is inconsistent")
        if canonical["installer"] != str(
                PurePosixPath(canonical["bundle_parent"]) / PINNED_INSTALLER_NAME):
            raise FormatError("mac-uninstall.installer is not adjacent")
        if canonical["sibling_sentinel"] != str(
                PurePosixPath(canonical["bundle_parent"])
                / ".wot-uninstall-funnel-sentinel"):
            raise FormatError("mac-uninstall sibling sentinel is not adjacent")

        postconditions = _update_exact(
            evidence["postconditions"],
            {"bundle_absent", "bundle_parent_unchanged", "data_root_absent",
             "installer_unchanged", "session_gate_absent",
             "sibling_sentinel_unchanged",
             "trash_contains_bundle_or_original_absent", "update_dir_absent"},
            "mac-uninstall.postconditions")
        if any(value is not True for value in postconditions.values()):
            raise FormatError("mac-uninstall postconditions are not all proven")

        protected = _update_exact(
            evidence["protected_artifacts"],
            {"bundle_parent_after", "bundle_parent_before", "installer_after",
             "installer_before", "sibling_sentinel_after",
             "sibling_sentinel_before"},
            "mac-uninstall.protected_artifacts")

        def protected_identity(value, what: str, *, file: bool) -> dict:
            keys = {"device", "inode", "path"}
            if file:
                keys |= {"sha256", "size"}
            value = _update_exact(value, keys, what)
            if (type(value["device"]) is not int or value["device"] < 0
                    or type(value["inode"]) is not int or value["inode"] <= 0):
                raise FormatError(f"{what} native identity is invalid")
            value["path"] = _canonical_absolute_path(
                value["path"], f"{what}.path")
            if file:
                _update_sha(value["sha256"], f"{what}.sha256")
                if type(value["size"]) is not int or value["size"] < 0:
                    raise FormatError(f"{what}.size is invalid")
            return value

        parent_before = protected_identity(
            protected["bundle_parent_before"],
            "mac-uninstall.bundle_parent_before", file=False)
        parent_after = protected_identity(
            protected["bundle_parent_after"],
            "mac-uninstall.bundle_parent_after", file=False)
        installer_before = protected_identity(
            protected["installer_before"],
            "mac-uninstall.installer_before", file=True)
        installer_after = protected_identity(
            protected["installer_after"],
            "mac-uninstall.installer_after", file=True)
        sentinel_before = protected_identity(
            protected["sibling_sentinel_before"],
            "mac-uninstall.sibling_sentinel_before", file=True)
        sentinel_after = protected_identity(
            protected["sibling_sentinel_after"],
            "mac-uninstall.sibling_sentinel_after", file=True)
        if (parent_before != parent_after
                or installer_before != installer_after
                or sentinel_before != sentinel_after):
            raise FormatError("mac-uninstall protected artifacts changed")
        if (parent_before["path"] != canonical["bundle_parent"]
                or installer_before["path"] != canonical["installer"]
                or sentinel_before["path"] != canonical["sibling_sentinel"]):
            raise FormatError("mac-uninstall protected artifact paths disagree")

        session_gate = _update_exact(
            evidence["session_gate"],
            {"before", "owner_only_before_launch"},
            "mac-uninstall.session_gate")
        gate_before = _update_exact(
            session_gate["before"],
            {"device", "inode", "mode", "path", "sha256", "size"},
            "mac-uninstall.session_gate.before")
        if (type(gate_before["device"]) is not int
                or gate_before["device"] < 0
                or type(gate_before["inode"]) is not int
                or gate_before["inode"] <= 0
                or type(gate_before["mode"]) is not int
                or gate_before["mode"] < 0
                or gate_before["mode"] > 0o777
                or gate_before["mode"] & 0o077
                or type(gate_before["size"]) is not int
                or gate_before["size"] < 0
                or session_gate["owner_only_before_launch"] is not True):
            raise FormatError("mac-uninstall session gate is not owner-only")
        gate_before["path"] = _canonical_absolute_path(
            gate_before["path"], "mac-uninstall.session_gate.before.path")
        _update_sha(
            gate_before["sha256"],
            "mac-uninstall.session_gate.before.sha256")
        if gate_before["path"] != canonical["session_gate"]:
            raise FormatError("mac-uninstall session gate path disagrees")

        network = _update_exact(
            evidence["network"],
            {"announcement_url", "inherited_endpoint_variables_scrubbed",
             "release_base_url"},
            "mac-uninstall.network")
        if (not isinstance(network["announcement_url"], str)
                or not isinstance(network["release_base_url"], str)):
            raise FormatError("mac-uninstall endpoint isolation is invalid")
        announcement = re.fullmatch(
            r"http://127\.0\.0\.1:([1-9][0-9]{0,4})/announcement\.json",
            network["announcement_url"])
        release_base = re.fullmatch(
            r"http://127\.0\.0\.1:([1-9][0-9]{0,4})"
            + re.escape(FUNNEL_RELEASE_BASE_PATH),
            network["release_base_url"])
        if (announcement is None or release_base is None
                or announcement.group(1) != release_base.group(1)
                or network["inherited_endpoint_variables_scrubbed"] is not True):
            raise FormatError("mac-uninstall endpoint isolation is invalid")
        _update_requests(
            evidence["request_transcript"],
            "mac-uninstall.request_transcript")
        allowed_requests = {
            "/announcement.json", "/announcement.json.sig",
            FUNNEL_RELEASE_BASE_PATH + RELEASE_NOTES_LATEST_PATH,
        }
        if any(request["method"] != "GET"
               or request["path"] not in allowed_requests
               for request in evidence["request_transcript"]):
            raise FormatError("mac-uninstall request transcript escaped loopback")

        process = _update_exact(
            evidence["process"], {"argv", "exit_status", "pid"},
            "mac-uninstall.process")
        argv = process["argv"]
        if (not isinstance(argv, list) or not argv
                or any(not isinstance(value, str) or not value for value in argv)):
            raise FormatError("mac-uninstall.process.argv is invalid")
        if argv.count("-wottestuninstall=confirm") != 1:
            raise FormatError("mac-uninstall process must request confirm exactly once")
        expected_profile = (
            "-wottestpreferencesdir=" + canonical["data_root"] + "/Preferences")
        if argv.count(expected_profile) != 1:
            raise FormatError("mac-uninstall process does not bind its profile")
        if (type(process["pid"]) is not int or process["pid"] <= 0
                or process["exit_status"] != 0):
            raise FormatError("mac-uninstall process did not exit normally")

        result = _update_exact(
            evidence["result"],
            {"answer", "failure_reason", "format", "remaining_paths",
             "removed_paths"},
            "mac-uninstall.result")
        if (result["format"] != "wot-uninstall-result-v1"
                or result["answer"] != "confirm"
                or result["failure_reason"] != ""):
            raise FormatError("mac-uninstall result is not a confirm result")
        if result["remaining_paths"] != []:
            raise FormatError("mac-uninstall remaining_paths is not empty")
        removed = result["removed_paths"]
        expected_removed = {
            canonical["data_root"], canonical["update_dir"],
            canonical["bundle"],
        }
        if (not isinstance(removed, list) or len(removed) != 3
                or any(not isinstance(value, str) for value in removed)
                or set(removed) != expected_removed):
            raise FormatError("mac-uninstall removed_paths is inconsistent")
        return
    _update_common_evidence(name, evidence)
    sequence = evidence["base"]["release_sequence"] + 1
    if evidence["trial_absent"] is not True or evidence["previous_absent"] is not True:
        raise FormatError(f"{name} left trial or previous content")
    if evidence["frontend_handoff"] is not True:
        raise FormatError(f"{name} never reached frontend-handoff")
    if name == "update-apply":
        if evidence["ready_marker"] != f"{sequence}\n" or evidence["explicit_launch"] is not True:
            raise FormatError("update-apply did not quit and explicitly launch the ready update")
        roles = _update_roles(evidence, ("A", "B"))
        if (roles["A"]["release_identity_sequence"] != sequence - 1
                or roles["B"]["release_identity_sequence"] != sequence
                or not all(row["ready"] for row in roles.values())):
            raise FormatError("update-apply process identities are invalid")
        if evidence["trees"]["live_after"] != evidence["variant_tree_root"]:
            raise FormatError("update-apply did not publish the candidate")
    elif platform == "Mac" and name == "interrupted-swap":
        if (evidence["ready_marker"] != f"{sequence}\n"
                or evidence["interrupted_layout"] is not True
                or evidence["staged_absent"] is not True
                or evidence["trees"]["live_after"]
                != evidence["variant_tree_root"]):
            raise FormatError(
                "Mac interrupted-swap did not retain the new live tree")
        roles = _update_roles(evidence, ("A", "B"))
        if (roles["A"]["release_identity_sequence"] != sequence - 1
                or roles["B"]["release_identity_sequence"] != sequence
                or not all(row["ready"] for row in roles.values())):
            raise FormatError("Mac interrupted-swap process identities are invalid")
    else:
        if evidence["started_marker"] != f"{sequence} started\n":
            raise FormatError(f"{name} did not prove a started trial")
        notice = _update_exact(evidence["failure_notice"],
            {"failed_sequence", "restored_sequence", "path", "sha256", "state"}, f"{name}.failure_notice")
        if (notice["failed_sequence"] != sequence or notice["restored_sequence"] != sequence - 1
                or notice["state"] not in {"pending", "seen"}):
            raise FormatError(f"{name} failure notice does not bind the rollback")
        _update_sha(notice["sha256"], f"{name}.failure_notice.sha256")
        if evidence["trees"]["live_after"] != evidence["base"]["tree_root"]:
            raise FormatError(f"{name} did not restore the base tree")
        if name == "update-bad-build-rolls-back":
            if (evidence["ready_marker"] != f"{sequence}\n"
                    or type(evidence["bad_exit_status"]) is not int or evidence["bad_exit_status"] == 0
                    or evidence["archive_request_count_after_staging"] != 1
                    or evidence["archive_request_count_after_normal_recheck"] != 1):
                raise FormatError("bad-build failure or download suppression is invalid")
            _update_observation(evidence["recheck"], f"{name}.recheck")
            result = evidence["recheck"]
            if (result["status"] != "KnownBad" or result["reason"] != "failed-release"
                    or result["sequence"] != sequence):
                raise FormatError("bad-build recheck did not suppress the failed sequence")
            if platform == "Mac":
                roles = _update_roles(evidence, ("A", "D"))
                if (roles["A"]["release_identity_sequence"] != sequence - 1
                        or roles["D"]["release_identity_sequence"] != sequence - 1
                        or not all(row["ready"] for row in roles.values())
                        or evidence["rollback_exit_status"] != 1
                        or evidence["rolled_back_marker"] != f"{sequence}\n"):
                    raise FormatError(
                        "Mac bad-build rollback launches are invalid")
        elif any(evidence[key] is not True for key in (
                "missing_live_before", "player_data_unchanged", "recovery_without_trial")):
            raise FormatError("interrupted-swap recovery or data preservation is invalid")


def _update_roles(evidence: dict, expected: tuple) -> dict:
    roles = {process["role"]: process for process in evidence["processes"]}
    if tuple(roles) != expected:
        raise FormatError(
            f"update process roles are {tuple(roles)!r}, not {expected!r}")
    return roles


def _funnel_update_archive_paths(platform: str, sequence: int,
                                 commit: str) -> set[str]:
    asset = update_archive_asset(platform, sequence + 1, commit)
    return {
        (f"{FUNNEL_RELEASE_BASE_PATH}/releases/download/"
         f"wot-funnel-{platform.casefold()}-r{sequence + 1}-{label}/{asset}")
        for label in ("good", "bad")
    }


def _announcement_chain_requests(requests: list,
                                 archive_paths: set[str]) -> list:
    return [request for request in requests
            if request["path"] in {"/announcement.json", "/announcement.json.sig"}
            or (request["path"].startswith(
                    FUNNEL_RELEASE_BASE_PATH + "/releases/download/")
                and request["path"] not in archive_paths)]


def _parsed_update_result_detail(detail: str) -> dict:
    tokens = detail.split("; ")[-1].split()
    if not tokens:
        return {}
    result = {"result": tokens[0]}
    for token in tokens[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    try:
        result["sequence"] = int(result["sequence"])
    except (KeyError, ValueError):
        return {}
    return result


def validate_update_cell_semantics(name: str, evidence: dict, platform: str = "Win64") -> None:
    validate_update_cell_evidence(name, evidence, platform)
def _validate_derivation_file_ref(value, what: str, *, signature: bool) -> None:
    keys = {"name", "sha256", "size"} | ({"signature"} if signature else set())
    value = _update_exact(value, keys, what)
    if (not isinstance(value["name"], str) or not value["name"]
            or Path(value["name"]).name != value["name"]):
        raise FormatError(f"{what}.name is invalid")
    _update_sha(value["sha256"], f"{what}.sha256")
    if type(value["size"]) is not int or value["size"] <= 0:
        raise FormatError(f"{what}.size is invalid")
    if signature:
        _validate_derivation_file_ref(
            value["signature"], f"{what}.signature", signature=False)


def _validate_derivation_component(value, what: str) -> None:
    value = _update_exact(value, {"name", "sha256", "size"}, what)
    if not isinstance(value["name"], str) or not value["name"]:
        raise FormatError(f"{what}.name is invalid")
    _update_sha(value["sha256"], f"{what}.sha256")
    if type(value["size"]) is not int or value["size"] <= 0:
        raise FormatError(f"{what}.size is invalid")


def validate_update_derivation_receipt(document: dict) -> None:
    """Validate the complete closed shape emitted by update_funnel_fixtures."""
    document = _update_exact(
        document,
        {"base", "format", "next_release", "platform", "test_public_keys",
         "variants"},
        "update derivation receipt")
    platform = document["platform"]
    if (document["format"] != "wot-update-funnel-derivation-v1"
            or platform not in PLATFORMS):
        raise FormatError("update derivation receipt identity is invalid")
    base = _update_exact(
        document["base"],
        {"archive", "commit", "main_executable", "product_receipt_sha256",
         "release_json_sha256", "release_sequence", "tree_root"},
        "update derivation receipt.base")
    _validate_derivation_file_ref(
        base["archive"], "update derivation receipt.base.archive",
        signature=False)
    for key in ("product_receipt_sha256", "release_json_sha256", "tree_root"):
        _update_sha(base[key], f"update derivation receipt.base.{key}")
    if (not isinstance(base["commit"], str) or not base["commit"]
            or type(base["release_sequence"]) is not int
            or not 0 <= base["release_sequence"] <= INT64_MAX):
        raise FormatError("update derivation receipt base identity is invalid")
    _validate_derivation_component(
        base["main_executable"], "update derivation receipt.base.main_executable")
    if platform == "Mac":
        if (not base["main_executable"]["name"].startswith("Contents/MacOS/")
                or posixpath.normpath(base["main_executable"]["name"])
                    != base["main_executable"]["name"]):
            raise FormatError(
                "Mac derivation base components are not bundle-relative")
    next_release = _update_exact(
        document["next_release"], {"release_commit", "release_sequence"},
        "update derivation receipt.next_release")
    if (next_release["release_commit"] != base["commit"]
            or next_release["release_sequence"] != base["release_sequence"] + 1):
        raise FormatError("update derivation receipt next release is not N+1")
    keys = _update_exact(
        document["test_public_keys"],
        {"announcement", "offline_root", "release"},
        "update derivation receipt.test_public_keys")
    for key, value in keys.items():
        _update_sha(value, f"update derivation receipt.test_public_keys.{key}")
    variants = _update_exact(
        document["variants"], {"bad", "good"},
        "update derivation receipt.variants")
    variant_keys = {
        "announcement", "archive", "directory", "key_ring",
        "main_executable", "manifest", "mutations", "tag", "tree_root",
    }
    if platform == "Mac":
        variant_keys.add("signature_verification")
    for label in ("bad", "good"):
        what = f"update derivation receipt.variants.{label}"
        variant = _update_exact(variants[label], variant_keys, what)
        if (not isinstance(variant["directory"], str)
                or Path(variant["directory"]).name != variant["directory"]
                or not isinstance(variant["tag"], str) or not variant["tag"]):
            raise FormatError(f"{what} directory/tag is invalid")
        _update_sha(variant["tree_root"], f"{what}.tree_root")
        for key in ("announcement", "key_ring", "manifest"):
            _validate_derivation_file_ref(
                variant[key], f"{what}.{key}", signature=True)
        _validate_derivation_file_ref(
            variant["archive"], f"{what}.archive", signature=False)
        _validate_derivation_component(
            variant["main_executable"], f"{what}.main_executable")
        if platform == "Mac":
            if (not variant["main_executable"]["name"].startswith(
                    "Contents/MacOS/")
                    or posixpath.normpath(
                        variant["main_executable"]["name"])
                        != variant["main_executable"]["name"]):
                raise FormatError(f"{what} components are not Mac bundle-relative")
        if platform == "Mac":
            verification = _update_exact(
                variant["signature_verification"],
                {"artifact", "command", "verified"},
                f"{what}.signature_verification")
            if (verification["verified"] is not True
                    or verification["artifact"] != "extracted-post-ditto"
                    or verification["command"]
                        != "codesign --verify --deep --strict"):
                raise FormatError(f"{what}.signature_verification is invalid")
        if not isinstance(variant["mutations"], list) or not variant["mutations"]:
            raise FormatError(f"{what}.mutations is invalid")
        for index, mutation in enumerate(variant["mutations"]):
            mutation = _update_exact(
                mutation, {"operation", "path"},
                f"{what}.mutations[{index}]")
            if not all(isinstance(value, str) and value
                       for value in mutation.values()):
                raise FormatError(f"{what}.mutations[{index}] is invalid")
    variant_roots = {variants[label]["tree_root"] for label in ("bad", "good")}
    if len(variant_roots) != 2 or base["tree_root"] in variant_roots:
        raise FormatError(
            "update derivation receipt base/good/bad tree roots are not distinct")


def _load_update_derivation_receipt(receipt_path: Path, reference: dict,
                                    platform: str) -> dict:
    relative = Path(reference["path"])
    if relative.is_absolute():
        raise FormatError("derivation receipt path is not relative to the receipt")
    base = receipt_path.parent.resolve()
    resolved = (base / relative).resolve()
    try:
        resolved.relative_to(base)
    except ValueError as exc:
        raise FormatError("derivation receipt path escapes the evidence directory") from exc
    try:
        digest, size = sha256_file(resolved)
        document = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        raise FormatError(f"cannot read derivation receipt {resolved}: {exc}") from exc
    if (digest, size) != (reference["sha256"], reference["size"]):
        raise FormatError("derivation receipt size/sha256 does not match its reference")
    validate_update_derivation_receipt(document)
    if document["platform"] != platform:
        raise FormatError(
            "update derivation receipt is for another platform")
    return document


def _bind_update_derivation(name: str, evidence: dict, derivation: dict,
                            platform: str) -> None:
    if derivation["platform"] != platform:
        raise FormatError(f"{name} derivation platform does not match the funnel")
    base = evidence["base"]
    derived_base = derivation["base"]
    if (base["git_commit"] != derived_base["commit"]
            or base["release_sequence"] != derived_base["release_sequence"]
            or base["release_json_sha256"]
                != derived_base["release_json_sha256"]
            or base["archive_sha256"] != derived_base["archive"]["sha256"]
            or base["tree_root"] != derived_base["tree_root"]
            or evidence["trees"]["live_before"] != derived_base["tree_root"]):
        raise FormatError(f"{name} does not bind the derivation receipt base")
    variants = derivation["variants"]
    label = "bad" if name == "update-bad-build-rolls-back" else "good"
    if evidence["variant_tree_root"] != variants[label]["tree_root"]:
        raise FormatError(f"{name} does not bind the {label} variant tree")
    if name == "update-apply" and evidence["trees"]["live_after"] != variants["good"]["tree_root"]:
        raise FormatError("update-apply LIVE does not bind the good variant tree")


def _funnel_path_leaf(value: object) -> str:
    if not isinstance(value, str) or not value:
        return ""
    return value.replace("\\", "/").rstrip("/").rsplit("/", 1)[-1]


def _validate_original_game_cache_binding(final: object,
                                          installer: dict, *,
                                          require_front_end: bool = True) -> None:
    required_final = {
        "manifests", "boot_failure_receipts",
        "process_alive_after_activation",
    }
    if require_front_end:
        required_final.add("front_end_snapshot")
    if (not isinstance(final, dict) or not required_final.issubset(final)
            or any(field in final for field in (
                "attestations", "active_slots", "resumes",
                "complete_generation_count"))
            or final.get("boot_failure_receipts") != []
            or final.get("process_alive_after_activation") is not True):
        raise FormatError("installer acquisition final activation is invalid")

    manifests = final.get("manifests")
    if not isinstance(manifests, list) or len(manifests) != 1:
        raise FormatError("installer acquisition must capture one cache manifest")
    manifest_ref = manifests[0]
    path = manifest_ref.get("path") if isinstance(manifest_ref, dict) else None
    if (not isinstance(path, str) or not path.replace("\\", "/").endswith(
            "ContentCache/current/manifest.json")):
        raise FormatError("installer acquisition manifest is not published current")
    manifest = manifest_ref.get("json") if isinstance(manifest_ref, dict) else None
    manifest_sha256 = (manifest_ref.get("sha256")
                       if isinstance(manifest_ref, dict) else None)
    if (manifest_ref.get("exists") is not True
            or not _is_lower_sha256(manifest_sha256)
            or not isinstance(manifest, dict)
            or manifest.get("format") != CACHE_MANIFEST_FORMAT
            or manifest.get("cache_version") != CACHE_MANIFEST_VERSION
            or manifest.get("hash_algorithm") != "sha256"
            or manifest.get("installer_sha256") != PINNED_INSTALLER_SHA256
            or manifest.get("installer_sha256") != installer.get("sha256")
            or not _is_lower_sha256(manifest.get("contract_sha256"))
            or not _is_lower_sha256(manifest.get("production_descriptor_sha256"))
            or not _is_lower_sha256(manifest.get("front_end_descriptor_root"))
            or not isinstance(manifest.get("world_descriptors"), list)
            or hashlib.sha256(canonical_bytes(
                manifest, "captured cache manifest")).hexdigest()
                != manifest_sha256):
        raise FormatError(
            "installer acquisition cache manifest does not bind the pinned installer")
    if not isinstance(manifest.get("blobs"), list):
        raise FormatError(
            "installer acquisition cache manifest blobs are invalid")

def _validate_funnel_process_identity(identity: object, platform: str,
                                      pid: int, what: str) -> tuple[dict, dict]:
    if (not isinstance(identity, dict)
            or set(identity) != {"observer", "before", "after"}
            or identity.get("observer") != {
                "Win64": "win32-job-get-process-times",
                "Mac": "mac-ps-start-time",
            }[platform]):
        raise FormatError(f"{what} process identity is invalid")
    before = identity.get("before")
    after = identity.get("after")
    keys = {"pid", "process_start_id", "observed_monotonic_seconds"}
    if (not isinstance(before, dict) or set(before) != keys
            or not isinstance(after, dict) or set(after) != keys
            or before.get("pid") != pid or after.get("pid") != pid
            or not isinstance(before.get("process_start_id"), str)
            or not before["process_start_id"]
            or after.get("process_start_id") != before["process_start_id"]
            or not _funnel_nonnegative_number(
                before.get("observed_monotonic_seconds"))
            or not _funnel_nonnegative_number(
                after.get("observed_monotonic_seconds"))
            or before["observed_monotonic_seconds"]
                > after["observed_monotonic_seconds"]):
        raise FormatError(f"{what} process identity is invalid")
    return before, after


def validate_funnel_receipt_schema(document: dict, platform: str) -> None:
    """Validate the funnel's closed receipt and ordered cell matrix.

    Full runner receipts use the platform's complete matrix in order. The
    base-only advisory shape is also accepted. No other shape is valid, and
    duplicate cell names are rejected before downstream policy is applied.
    """
    if platform not in PLATFORMS:
        raise FormatError(f"unsupported funnel platform {platform!r}")
    if set(document) != FUNNEL_RECEIPT_KEYS:
        raise FormatError("top-level schema is not exact")
    if document.get("format") != FUNNEL_RECEIPT_FORMAT:
        raise FormatError(f"format is not {FUNNEL_RECEIPT_FORMAT}")
    if document.get("platform") != platform:
        raise FormatError(
            f"platform {document.get('platform')!r} is not {platform}")
    if document.get("verdict") not in {"pass", "fail"}:
        raise FormatError("top-level verdict is invalid")
    if (not isinstance(document.get("started_utc"), str)
            or not document["started_utc"]
            or (document.get("completed_utc") is not None
                and not isinstance(document["completed_utc"], str))
            or not _funnel_nonnegative_number(document.get("duration_seconds"))):
        raise FormatError("top-level timing is invalid")
    if (not isinstance(document.get("decisions"), list)
            or not all(isinstance(item, str) for item in document["decisions"])):
        raise FormatError("top-level decisions are invalid")

    release = document.get("release")
    if not isinstance(release, dict) or (
            release and set(release) != FUNNEL_RELEASE_KEYS):
        raise FormatError("release binding schema is not exact")
    installer = document.get("installer")
    if not isinstance(installer, dict) or (
            installer and set(installer) != FUNNEL_INSTALLER_KEYS):
        raise FormatError("installer binding schema is not exact")
    # A failed in-progress funnel can be saved before acquisition. A receipt
    # offered as passing publication evidence cannot omit either binding.
    if document["verdict"] == "pass" and not release:
        raise FormatError("passing receipt has no release binding")
    if document["verdict"] == "pass" and not installer:
        raise FormatError("passing receipt has no installer binding")
    if release:
        for key in ("manifest_path", "asset_path", "git_commit"):
            if not isinstance(release.get(key), str) or not release[key]:
                raise FormatError(f"release binding {key} is invalid")
        for key in ("manifest_sha256", "asset_sha256",
                    "source_snapshot_sha256"):
            if not _is_lower_sha256(release.get(key)):
                raise FormatError(f"release binding {key} is invalid")
        if (type(release.get("asset_size")) is not int
                or release["asset_size"] <= 0
                or type(release.get("release_sequence")) is not int
                or not 0 <= release["release_sequence"] <= INT64_MAX
                or type(release.get("development_allow_source_drift"))
                    is not bool
                or (release.get("stapled") is not None
                    and type(release.get("stapled")) is not bool)):
            raise FormatError("release binding size/sequence is invalid")
    if installer:
        for key in ("path", "acquisition"):
            if not isinstance(installer.get(key), str) or not installer[key]:
                raise FormatError(f"installer binding {key} is invalid")
        if (type(installer.get("size")) is not int or installer["size"] <= 0
                or not _is_lower_sha256(installer.get("sha256"))):
            raise FormatError("installer binding size/digest is invalid")
        if (_funnel_path_leaf(installer["path"]) != PINNED_INSTALLER_NAME
                or installer["size"] != PINNED_INSTALLER_SIZE
                or installer["sha256"] != PINNED_INSTALLER_SHA256):
            raise FormatError("installer binding is not the pinned installer")

    cells = document.get("cells")
    if not isinstance(cells, list):
        raise FormatError("cell matrix is not a list")
    names = [cell.get("name") if isinstance(cell, dict) else None
             for cell in cells]
    base = list(FUNNEL_CELL_NAMES)
    full = base + list(UPDATE_FUNNEL_CELLS_BY_PLATFORM[platform])
    if names not in (base, full):
        raise FormatError(
            "cell matrix is not the ordered, duplicate-free platform matrix")
    for index, (cell, name) in enumerate(zip(cells, names)):
        if not isinstance(cell, dict) or set(cell) != FUNNEL_CELL_KEYS:
            raise FormatError(f"cell {index} schema is not exact")
        if (cell.get("format") != FUNNEL_CELL_RECEIPT_FORMAT
                or cell.get("index") != index
                or cell.get("name") != name):
            raise FormatError(f"cell {index} identity is invalid")
        if cell.get("verdict") not in FUNNEL_CELL_VERDICTS:
            raise FormatError(f"cell {index} verdict is invalid")
        if ((cell.get("started_utc") is not None
             and not isinstance(cell["started_utc"], str))
                or (cell.get("completed_utc") is not None
                    and not isinstance(cell["completed_utc"], str))
                or not _funnel_nonnegative_number(cell.get("duration_seconds"))):
            raise FormatError(f"cell {index} timing is invalid")
        timings = cell.get("phase_timings_seconds")
        if (not isinstance(timings, dict)
                or not all(isinstance(phase, str)
                           and _funnel_nonnegative_number(seconds)
                           for phase, seconds in timings.items())):
            raise FormatError(f"cell {index} phase timings are invalid")
        if (not isinstance(cell.get("phase_order"), list)
                or not all(isinstance(phase, str)
                           for phase in cell["phase_order"])):
            raise FormatError(f"cell {index} phase order is invalid")
        if not isinstance(cell.get("evidence"), dict):
            raise FormatError(f"cell {index} evidence is invalid")
        if name == "fresh-first-run" and cell.get("verdict") == "pass":
            evidence_keys = {
                "adjacent_installer", "boot_failure_receipts",
                "detected_heartbeat", "frontend_handoff",
                "installer_requests", "loopback_requests", "manifests",
                "notes_outcome", "notes_request_count", "notes_result",
                "phase_first_seen_seconds", "process",
                "process_alive_after_activation", "termination",
            }
            profile_field = ("mac_profile_library" if platform == "Mac"
                             else "win_profile_appdata")
            evidence_keys.add(profile_field)
            evidence = _update_exact(
                cell["evidence"], evidence_keys, "fresh-first-run.evidence")
            if platform == "Mac":
                profile = _update_exact(evidence[profile_field], {
                    "allowed_roots", "allowed_files", "engine_identifier_store",
                    "entries", "outside_root",
                }, f"fresh-first-run.evidence.{profile_field}")
                # The one engine exception: UE's shared
                # MachineId store, holding nothing but that identifier.
                if (profile["allowed_roots"] != [
                        "Application Support/WoT Reborn",
                        "Caches/com.aleiby.wot",
                        "Saved Application State",
                        "HTTPStorages",
                        "Logs/DiagnosticReports",
                    ]
                        or profile["allowed_files"] != [
                            "Preferences/com.aleiby.wot.plist",
                            "Application Support/Epic/Epic Games/KeyValueStore.ini"]
                        or profile["engine_identifier_store"] not in (
                            "machine-id-only", "absent")
                        or profile["outside_root"] != []
                        or not isinstance(profile["entries"], list)
                        or not all(isinstance(value, str) and value
                                   for value in profile["entries"])):
                    raise FormatError(
                        "fresh-first-run.evidence.mac_profile_library is invalid")
            else:
                profile = _update_exact(evidence[profile_field], {
                    "entries", "forbidden",
                }, f"fresh-first-run.evidence.{profile_field}")
                if (profile["forbidden"] != []
                        or not isinstance(profile["entries"], list)
                        or not all(isinstance(value, str) and value
                                   for value in profile["entries"])):
                    raise FormatError(
                        "fresh-first-run.evidence.win_profile_appdata is invalid")
            sequence = release["release_sequence"]
            expected = (0, "not-attempted") if sequence == 0 else (1, "selected")
            _update_requests(
                evidence["loopback_requests"],
                "fresh-first-run.evidence.loopback_requests")
            notes_path = FUNNEL_RELEASE_BASE_PATH + RELEASE_NOTES_LATEST_PATH
            notes_count = sum(request["path"] == notes_path
                              for request in evidence["loopback_requests"])
            notes_result = _release_notes_result(
                evidence["notes_result"],
                "fresh-first-run.evidence.notes_result")
            process = _update_exact(
                evidence["process"],
                {"command", "pid", "state_root", "stderr", "stdout"},
                "fresh-first-run.evidence.process")
            handoff = evidence["frontend_handoff"]
            if (not isinstance(handoff, dict)
                    or handoff.get("event") != "frontend-handoff"
                    or handoff.get("phase") != "Ready"
                    or handoff.get("pid") != process["pid"]):
                raise FormatError("fresh-first-run never reached frontend-handoff")
            detected = evidence["detected_heartbeat"]
            if (type(evidence["notes_request_count"]) is not int
                    or (evidence["notes_request_count"],
                        evidence["notes_outcome"]) != expected
                    or evidence["notes_request_count"] != notes_count
                    or type(process["pid"]) is not int or process["pid"] <= 0
                    or not isinstance(detected, dict)
                    or detected.get("event") != "original-game-detected"
                    or detected.get("pid") != process["pid"]
                    or detected.get("detail") != "source=adjacent"
                    or notes_result != {
                        "commit": release["git_commit"],
                        "outcome": evidence["notes_outcome"],
                        "pid": process["pid"],
                        "sequence": sequence,
                    }):
                raise FormatError(
                    "fresh-first-run release-notes evidence is invalid")
        if name == "downloads-pickup" and cell.get("verdict") == "pass":
            evidence = _update_exact(
                cell["evidence"], {"attempts"},
                "downloads-pickup.evidence")
            attempts = evidence["attempts"]
            expected_models = (["partial-final"] if platform == "Mac" else
                               ["chrome-edge", "firefox"])
            if (not isinstance(attempts, list)
                    or [attempt.get("model")
                        if isinstance(attempt, dict) else None
                        for attempt in attempts] != expected_models):
                raise FormatError(
                    "downloads-pickup browser model matrix is invalid")
            for attempt_index, attempt in enumerate(attempts):
                what = f"downloads-pickup.attempts[{attempt_index}]"
                attempt = _update_exact(attempt, {
                    "model", "entry_heartbeat", "partial_hold",
                    "detected_heartbeat", "downloaded_installer",
                    "installer_requests", "process_identity", "final",
                }, what)
                entry = attempt["entry_heartbeat"]
                detected = attempt["detected_heartbeat"]
                waiting = attempt["partial_hold"]
                final = attempt["final"]
                process = final.get("process") if isinstance(final, dict) else None
                downloaded = attempt["downloaded_installer"]
                if (not isinstance(entry, dict)
                        or entry.get("event") != "original-game-needed"
                        or "reason=missing" not in
                            str(entry.get("detail", ""))
                        or not isinstance(detected, dict)
                        or detected.get("event") != "original-game-detected"
                        or detected.get("detail") != "source=downloads"
                        or not isinstance(process, dict)
                        or type(process.get("pid")) is not int
                        or process["pid"] <= 0
                        or entry.get("pid") != process["pid"]
                        or detected.get("pid") != process["pid"]
                        or attempt["installer_requests"] != []
                        or not isinstance(downloaded, dict)
                        or downloaded.get("size") != installer.get("size")
                        or downloaded.get("sha256") != installer.get("sha256")
                        or _funnel_path_leaf(downloaded.get("path", ""))
                            != PINNED_INSTALLER_NAME):
                    raise FormatError(f"{what} acquisition evidence is invalid")
                before, after = _validate_funnel_process_identity(
                    attempt["process_identity"], platform, process["pid"], what)
                if (not _funnel_nonnegative_number(
                        entry.get("monotonic_seconds"))
                        or not _funnel_nonnegative_number(
                            downloaded.get("monotonic_seconds"))
                        or not _funnel_nonnegative_number(
                            detected.get("monotonic_seconds"))
                        or not (before["observed_monotonic_seconds"]
                                <= entry["monotonic_seconds"]
                                < downloaded["monotonic_seconds"]
                                < detected["monotonic_seconds"]
                                <= after["observed_monotonic_seconds"])):
                    raise FormatError(f"{what} chronology is invalid")
                if (attempt["model"] == "partial-final"
                        and (not isinstance(waiting, dict)
                             or waiting.get("pid") != process["pid"]
                             or waiting.get("event") != "partial-hold"
                             or not _funnel_nonnegative_number(
                                waiting.get("hold_seconds"))
                             or waiting["hold_seconds"] < 6.0
                             or not _funnel_nonnegative_number(
                                waiting.get("monotonic_seconds"))
                             or not (entry["monotonic_seconds"]
                                     < waiting["monotonic_seconds"]
                                     < downloaded["monotonic_seconds"]))):
                    raise FormatError(
                        f"{what} has no partial-download hold evidence")
                if attempt["model"] != "partial-final" and waiting is not None:
                    raise FormatError(
                        f"{what} unexpectedly carries partial-download hold evidence")
                _validate_original_game_cache_binding(final, installer)
        if name == "damaged-cache-rebuild" and cell.get("verdict") == "pass":
            evidence = _update_exact(cell["evidence"], {
                "damaged_logical_id", "expected_blob_sha256",
                "frontend_handoff", "repaired_blob_sha256",
                "installer_requests", "final",
            }, "damaged-cache-rebuild.evidence")
            handoff = evidence["frontend_handoff"]
            if (not isinstance(evidence["damaged_logical_id"], str)
                    or not evidence["damaged_logical_id"]
                    or evidence["installer_requests"] != []
                    or not _is_lower_sha256(evidence["expected_blob_sha256"])
                    or evidence["expected_blob_sha256"]
                        != evidence["repaired_blob_sha256"]
                    or not isinstance(handoff, dict)
                    or handoff.get("event") != "frontend-handoff"
                    or handoff.get("phase") != "Ready"
                    or type(handoff.get("pid")) is not int
                    or handoff["pid"] <= 0
                    or not _is_lower_sha256(evidence["repaired_blob_sha256"])):
                raise FormatError("damaged cache rebuild evidence is invalid")
            _validate_original_game_cache_binding(
                evidence["final"], installer, require_front_end=False)
            blobs = evidence["final"]["manifests"][0]["json"].get("blobs")
            repaired = next((row for row in blobs
                             if isinstance(row, dict) and row.get("logical_id")
                             == evidence["damaged_logical_id"]), None)
            if (repaired is None or repaired.get("sha256")
                    != evidence["repaired_blob_sha256"]):
                raise FormatError(
                    "damaged cache did not restore the startup-critical blob")
        if name in UPDATE_EVIDENCE_KEYS and cell.get("verdict") == "pass":
            validate_update_cell_evidence(name, cell["evidence"], platform)
        if (not isinstance(cell.get("decisions"), list)
                or not all(isinstance(item, str) for item in cell["decisions"])):
            raise FormatError(f"cell {index} decisions are invalid")
        for key in ("skip_reason", "error"):
            if cell.get(key) is not None and not isinstance(cell[key], str):
                raise FormatError(f"cell {index} {key} is invalid")


def validate_funnel_receipt(path: Path, platform: str, release: GuardedRelease,
                            required_cells: Sequence[str]) -> dict:
    """Bind one funnel receipt to its guarded release and required PASS cells."""
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        raise FormatError(f"cannot read funnel receipt {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise FormatError(f"funnel receipt {path} is not a JSON object")
    try:
        validate_funnel_receipt_schema(document, platform)
    except FormatError as exc:
        raise FormatError(f"funnel receipt {path}: {exc}") from exc
    run = document.get("release")
    cells = document.get("cells")
    # The exact ordered matrix check above rejects duplicate names before this
    # projection can collapse anything.
    verdicts = {cell["name"]: cell["verdict"] for cell in cells}
    twin = release.test_twin
    expected_manifest = (twin["release_json_sha256"] if twin is not None
                         else release.release_json_sha256)
    snapshot = release.document.get("source_snapshot")
    problems = []
    if document.get("format") != FUNNEL_RECEIPT_FORMAT:
        problems.append(f"format is not {FUNNEL_RECEIPT_FORMAT}")
    if document.get("platform") != platform:
        problems.append(f"platform {document.get('platform')!r} is not {platform}")
    if document.get("verdict") != "pass":
        problems.append(f"verdict is {document.get('verdict')!r}, not 'pass'")
    if run.get("manifest_sha256") != expected_manifest:
        problems.append("it ran a different Test release.json than test_twin names"
                        if twin is not None else "it ran a different release.json")
    if run.get("git_commit") != release.document.get("git_commit"):
        problems.append("it ran a different commit")
    if run.get("release_sequence") != release.release_sequence:
        problems.append("it ran a different release sequence")
    if run.get("source_snapshot_sha256") != (
            snapshot.get("sha256") if isinstance(snapshot, dict) else None):
        problems.append("it ran a different source snapshot")
    if run.get("development_allow_source_drift") is True:
        problems.append(
            "development source drift override is not acceptance evidence")
    if twin is not None and (run.get("asset_sha256"), run.get("asset_size")) != (
            twin["asset_sha256"], twin["asset_size"]):
        problems.append("it ran a different Test zip than test_twin binds")
    update_cells = [cell for cell in cells
                    if cell["name"] in UPDATE_EVIDENCE_KEYS
                    and cell["verdict"] == "pass"]
    expected_base = {
        "archive_sha256": run.get("asset_sha256"),
        "git_commit": run.get("git_commit"),
        "release_json_sha256": run.get("manifest_sha256"),
        "release_sequence": run.get("release_sequence"),
    }
    derivations = {}
    for cell in update_cells:
        name = cell["name"]
        evidence = cell["evidence"]
        for key, expected in expected_base.items():
            if evidence["base"].get(key) != expected:
                problems.append(
                    f"cell {name} base.{key} differs from the tested release")
        reference = evidence["derivation_receipt"]
        identity = (reference["path"], reference["sha256"], reference["size"])
        try:
            derivation = derivations.get(identity)
            if derivation is None:
                derivation = _load_update_derivation_receipt(
                    Path(path), reference, platform)
                derivations[identity] = derivation
            _bind_update_derivation(name, evidence, derivation, platform)
        except FormatError as exc:
            problems.append(f"cell {name}: {exc}")
    for cell in required_cells:
        if verdicts.get(cell) != "pass":
            problems.append(f"cell {cell} is {verdicts.get(cell)!r}, not 'pass'")
    if problems:
        raise FormatError(f"{platform}: funnel receipt {path} does not prove this "
                          "release: " + "; ".join(problems))
    digest, size = sha256_file(Path(path))
    return {"cells": verdicts, "manifest_sha256": expected_manifest,
            "sha256": digest, "size": size}


def load_cache_contract_identity(path: Path = DEFAULT_CACHE_CONTRACT) -> Tuple[str, int]:
    document = json.loads(Path(path).read_text(encoding="utf-8"))
    payload = document.get("payload") if isinstance(document, dict) else None
    contract_id = payload.get("contract_id") if isinstance(payload, dict) else None
    version = payload.get("contract_version") if isinstance(payload, dict) else None
    if not isinstance(contract_id, str) or not contract_id \
            or isinstance(version, bool) or not isinstance(version, int):
        raise FormatError(f"{path} has no payload.contract_id/contract_version")
    return contract_id, version
