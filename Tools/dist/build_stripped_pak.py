#!/usr/bin/env python3
"""Build the resumable isolated stripped-payload staged client.

Steps are seed -> import -> marker -> cook -> audit -> package.  Successful steps leave
JSON evidence under ``<iso>/.stamps``; a failed command never earns a stamp.
``--guarded`` keeps that pipeline but produces the source-engine Test artifact
used by the first-run funnel.

``--guarded --configuration Shipping`` is the release mode: the same run (and
the same seeded/imported/cooked ISO) ALSO produces the guarded Shipping release
through three more steps, shipping-stage -> shipping-audit -> shipping-package.
The cooked content is configuration-independent (UAT cooks
Saved/Cooked/<CookPlatform> from the platform alone), so the Shipping client is
built by the guarded wrapper and staged from the Test cook with ``BuildCookRun
-skipbuild -skipcook -stage -pak`` into its own staging directory, audited the
same way, and packaged with a ``test_twin`` binding to the Test release the
funnel proves. Test bytes are never release bytes; Shipping bytes are never
funnel-tested directly -- the twin binding is what joins them.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import platform as host_platform
import plistlib
import posixpath
import re
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence


REPO = Path(__file__).resolve().parents[2]
DIST_DIR = Path(__file__).resolve().parent
IMPORT_DIR = REPO / "Tools" / "import"
TOOLS_DIR = REPO / "Tools"
for directory in (DIST_DIR, IMPORT_DIR, TOOLS_DIR):
    if str(directory) not in sys.path:
        sys.path.insert(0, str(directory))

import audit_stripped_pak as absence_audit  # noqa: E402
import eos_credentials  # noqa: E402
import guarded_build_root as build_root_cache  # noqa: E402
import mac_dmg  # noqa: E402
import mac_signing  # noqa: E402
import release_formats  # noqa: E402
import windows_launcher  # noqa: E402
from bootstrap import StageFailure, engine_executable, runuat_path  # noqa: E402
from import_all import MP_MAPS  # noqa: E402
from wot_config import (ConfigurationError, devtree_dir, exports_dir,
                        installed_engine_root, local_path, local_value, original_dir,
                        source_engine_root)  # noqa: E402


STEPS = ("seed", "import", "marker", "cook", "audit", "package",
         "shipping-stage", "shipping-audit", "shipping-package")
# The release-only steps: they run only for --guarded --configuration Shipping,
# after (and from) the Test cook, and never inside its Zen lifecycle.
SHIPPING_STEPS = ("shipping-stage", "shipping-audit", "shipping-package")
STEP_NUMBERS = {name: index for index, name in enumerate(STEPS, 1)}
PREREQUISITES = {
    "seed": (),
    "import": ("seed",),
    "marker": ("import",),
    "cook": ("marker",),
    "audit": ("cook",),
    "package": ("audit",),
    # Staged from the Test cook's loose Saved/Cooked output.
    "shipping-stage": ("cook",),
    "shipping-audit": ("shipping-stage",),
    # Binds the Test release (the funnel's artifact) as its twin.
    "shipping-package": ("shipping-audit", "package"),
}
# Binaries ride along: the import commandlet cannot boot the isolated
# project without the COMPILED WoT editor modules (an -unattended editor
# exits during early init when project modules are missing — first live
# run). The cook step's -build recompiles the game target regardless.
# Build rides along for Mac/Resources/*.entitlements: without it UBT
# regenerates the engine-default entitlements, which enable the macOS App
# Sandbox — a sandboxed client cannot open -WoTPayloadCache=<path> outside
# its container and every payload preflight fails "cannot open file".
SEED_COPY_SET = ("WoT.uproject", "Config", "Source", "Tools", "Content",
                 "Binaries", "Build")
GUARDED_SEED_COPY_SET = (*SEED_COPY_SET, "ThirdParty", "EnginePatches")
IMPORT_SUCCESS_MARKERS = (
    "Python script executed successfully",
    "Success - 0 error",
)
PYTHON_COMMANDLET_SUCCESS = "Python script executed successfully"
FRACTAL_FINALIZE_SUCCESS = "FINALIZE-STRIPPED-FRACTAL-CATALOG OK"
FRACTAL_FINALIZE_RECEIPT = Path(
    "Saved/ImportReceipts/finalize-stripped-fractal-catalog.json")
ALTERNATE_BIND_IMPORT_RECEIPT = Path(
    "Saved/ImportReceipts/authenticated-alternate-bind.json")
GUARDED_IMPORT_SKIP_STEPS = ("meshes", "pickups")
# What a guarded import must PRODUCE. The release cooks only the bootstrap,
# front-end and nineteen neutral-shell maps plus the authority-pinned neutral
# support assets (_cook_maps, NEUTRAL_SUPPORT_*), and _validate_guarded_cooked_
# paths refuses anything else. Of those, import_all authors exactly one: the
# no-replace bootstrap-map. frontend-map is skip-if-exists (the shipped front
# end is the seeded asset); the shells are a separate deny-read commandlet over
# an empty output root; the support assets are seeded and hash-bound before AND
# after the import. Every other importer (sounds, flipbooks, foundation, the
# 19 retail maps, ...) writes only content the guarded cook can never reach --
# one measured map step alone spent 44 of its 48 minutes in ForceDeleteObjects over
# seeded Geo/<map> assets -- and _verify_guarded_reference_closure proves, per
# run, that no shipped package names any of it (hard, soft or editor-only).
# An earlier ISO already shipped with 1,032 mesh, 43 blueprint and 3,539
# material files never rewritten by the guarded import (still hardlinked to
# the dev tree), so the allowlist + absence audit, not a stripped ISO, were
# already the retail-exclusion guarantee.
GUARDED_IMPORT_STEPS = (
    "bootstrap-map", "frontend-map", "mover-wave-runtime")
# "full" is the pre-2026-09-30 guarded import (everything but meshes/pickups),
# kept so a release can be A/B-compared against it at one commit
# (compare_guarded_builds.py) and as an escape hatch.
GUARDED_IMPORT_CHOICES = ("shipping-producers", "full")
# Persistent guarded build root (guarded_build_root.py). "auto" enables it for
# the Win64 lane, the lane this was validated on; Mac opts in with "on".
COMPILE_CACHE_CHOICES = ("auto", "on", "off")
COMPILE_CACHE_FORMAT = "wot-guarded-compile-cache-v1"
# With the client and editor built (and receipted/transferred) from the build
# root, UAT's -build must not rebuild them in the ISO, whose Intermediate is
# empty by design: BuildProjectCommand.Automation.cs:90 (editor block, which
# also carries ShaderCompileWorker at :103) and :161 (cooked client) are the
# only consumers of these switches (ProjectParams.cs:742-743). UnrealPak is
# still built by -build (BuildProjectCommand.Automation.cs:112-117); the
# build root builds WoTEditor + ShaderCompileWorker exactly as that editor
# block would.
BUILD_ROOT_UAT_SKIPS = ("-skipbuildclient", "-skipbuildeditor")
REFERENCE_CLOSURE_FORMAT = "wot-guarded-reference-closure-v1"
PAYLOAD_MARKER_BLOCK = (
    "; BEGIN WOT STRIPPED-PAK REQUIRED PAYLOAD\n"
    "[SystemSettings]\n"
    "wot.PayloadInjection=1\n"
    "; END WOT STRIPPED-PAK REQUIRED PAYLOAD\n"
)
CANONICAL_BASELINE = {
    "WoT-Mac.ucas": 252_921_456,
    "WoT-Mac.pak": 10_697_198,
    "WoT-Mac.utoc": 189_931,
    "global.ucas+global.utoc": 3_272_650,
}
BOOTSTRAP_MAP = "/Game/WoT/Maps/WoTBootstrap"
FRONT_END_MAP = "/Game/WoT/Maps/WoTFrontEnd"
NEUTRAL_SHELL_NAMES = tuple(
    f"WoTMapShell{slot:02d}" for slot in range(1, 20))
NEUTRAL_SHELL_MAPS = tuple(
    f"/WoTNeutralShells/{name}" for name in NEUTRAL_SHELL_NAMES)
NEUTRAL_SUPPORT_ROOT_PACKAGES = (
    "/Game/WoT/Materials/M_WoT_World",
    "/Game/WoT/Materials/M_WoT_WorldMasked",
    "/Game/WoT/Materials/M_WoT_WorldWave",
    "/Game/WoT/Materials/M_WoT_WorldWaveMasked",
    "/Game/WoT/Materials/M_WoT_WorldWaveTranslucent",
    "/Game/WoT/Materials/M_WoT_WorldWaveModulated",
    "/Game/WoT/Materials/MI_WoT_MoverWaveRuntime",
    "/Game/WoT/Materials/MI_WoT_MoverWaveRuntimeMasked",
    "/Game/WoT/Materials/MI_WoT_MoverWaveRuntimeTranslucent",
    "/Game/WoT/Materials/MI_WoT_MoverWaveRuntimeModulated",
    "/Game/WoT/Materials/M_WoT_MeshTranslucent",
    "/Game/WoT/Materials/M_WoT_CoronaAdd",
    "/Game/WoT/Materials/M_WoT_CoronaMod",
    "/Game/WoT/Materials/M_WoT_Master",
    "/Game/WoT/Materials/M_WoT_MeshLit",
    "/Game/WoT/Materials/M_WoT_MeshLit2S",
    "/Game/WoT/Materials/M_WoT_MeshLitMasked",
    "/Game/WoT/Materials/M_WoT_MeshLitT",
    "/Game/WoT/Materials/M_WoT_MoverLit",
    "/Game/WoT/Materials/M_WoT_MoverLitMasked",
    "/Game/WoT/Materials/M_WoT_MeshModulate",
    "/Game/WoT/Materials/M_WoT_MeshFBAddGlow",
    "/Game/WoT/Materials/M_WoT_ParticleAdd",
    "/Game/WoT/Materials/M_WoT_ParticleMod",
    "/Game/WoT/Materials/M_WoT_EntryWorld",
    "/Game/WoT/Materials/M_WoT_EntryWorld_TwoSided",
    "/Game/WoT/Materials/M_WoT_EntryMasked",
    "/Game/WoT/Materials/M_WoT_EntryMasked_TwoSided",
    "/Game/WoT/Materials/M_WoT_EntryTranslucent",
    "/Game/WoT/Materials/M_WoT_EntryTranslucent_TwoSided",
    "/Game/WoT/Materials/M_WoT_EntryModulated",
    "/Game/WoT/Materials/M_WoT_EntryModulated_TwoSided",
    "/Game/WoT/Materials/M_WoT_EntryTranslucent_SceneBlend",
    "/Game/WoT/Materials/Instances/MI_GammaBoundary",
    "/Game/WoT/Materials/M_WoT_VolumeFog",
    "/Game/WoT/Materials/M_WoT_Decal",
    "/Game/WoT/Materials/M_WoT_EntryTranslucent_Mirror",
    "/Game/WoT/Materials/M_WoT_SkyWindow",
    "/Game/WoT/Materials/M_WoT_EntrySkyModulate",
    "/Game/WoT/Materials/M_WoT_EntrySkyAdd",
)
NEUTRAL_SUPPORT_HARD_DEPENDENCIES = (
    "/Game/WoT/Materials/MPC_WoTDynamicLights",
    "/Game/WoT/Materials/MPC_WoTLightWave",
    "/Game/WoT/Materials/MPC_WoTDisplay",
    "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral",
    "/Game/WoT/Materials/M_WoT_GammaBoundary",
)
NEUTRAL_SUPPORT_PACKAGES = (
    *NEUTRAL_SUPPORT_ROOT_PACKAGES,
    *NEUTRAL_SUPPORT_HARD_DEPENDENCIES,
)
NEUTRAL_SUPPORT_AUTHORITY_PATH = Path(
    "Tools/dist/neutral-support-assets-v1.json")
NEUTRAL_SUPPORT_AUTHORITY_FORMAT = (
    "wot-neutral-support-asset-authority-v1")
NEUTRAL_SUPPORT_RECEIPT_FORMAT = "wot-neutral-support-asset-receipt-v1"
NEUTRAL_SHELL_GENERATED_ROOT = Path("NeutralMapShells")
NEUTRAL_SHELL_PLUGIN_ROOT = Path("Plugins/WoTNeutralShells")
# Mirrors STAGING_PACKAGE_ROOT ("/Game/WoTNeutralShellStaging") in
# Tools/import/generate_neutral_map_shells.py: the temporary authoring root
# the generator uses and removes, which an interrupted run leaves behind.
NEUTRAL_SHELL_STAGING_ROOT = Path("Content/WoTNeutralShellStaging")
NEUTRAL_SHELL_DENIED_READ_RECEIPT = Path(
    "Saved/ImportReceipts/neutral-shells-denied-read-open.json")
NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT = Path(
    "Saved/ImportReceipts/neutral-shells-output-identity.json")
PINNED_SOURCE_ENGINES = {
    name: root for name in ("Mac", "Win64")
    for root in (source_engine_root(name, required=False),) if root is not None
}
# The guarded COOK/funnel configuration. Every guarded run cooks and packages
# Test; --configuration Shipping additionally stages a Shipping client from
# that same cook.
GUARDED_CONFIGURATION = "Test"
SHIPPING_CONFIGURATION = "Shipping"
GUARDED_CONFIGURATIONS = (GUARDED_CONFIGURATION, SHIPPING_CONFIGURATION)
STAGING_DIRECTORIES = {
    GUARDED_CONFIGURATION: "staged",
    # Its own -stagingdirectory: one staging directory holding two client
    # configs puts both executables side by side and switches UAT's per-config
    # staging rules to "any config" semantics (CopyBuildToStagingDirectory.
    # Automation.cs:1803, 2626, 3148-3165, 3595; WinPlatform.Automation.cs:181).
    SHIPPING_CONFIGURATION: "staged-shipping",
}
RELEASE_DIRECTORIES = {
    GUARDED_CONFIGURATION: "release",
    SHIPPING_CONFIGURATION: "release-shipping",
}
# Per-platform cook output UAT stages from: Saved/Cooked/<CookPlatform>
# (CopyBuildToStagingDirectory.Automation.cs:1366-1372; DeploymentContext.cs:
# 470-473; WinPlatform.Automation.cs:354-371 -> "Windows").
COOK_PLATFORMS = {"Mac": "Mac", "Win64": "Windows"}
SHIPPING_STAGE_RECEIPT_FORMAT = "wot-guarded-shipping-stage-v1"
TEST_TWIN_FORMAT = "wot-guarded-test-twin-v1"
# WoT.Target.cs:50-58: what a guarded Shipping game image must have compiled.
SHIPPING_GAME_DEFINITIONS = {
    "WOT_GUARDED_BOOTSTRAP": "1",
    "WOT_BOOTSTRAP_COMPILED_COMPONENT_IDENTITY": '"wot-game-production-v1"',
    "WOT_BOOTSTRAP_LIFECYCLE_ENDPOINT": '"production"',
    "NOINITCRASHREPORTER": "1",
    "CRASH_REPORTER_WITH_ANALYTICS": "0",
    "USE_CRASH_REPORTER_MONITOR": "0",
}
TEST_ONLY_DEFINITIONS = (
    "WOT_BOOTSTRAP_TEST_IDENTITIES", "WOT_BOOTSTRAP_TEST_ENDPOINTS")
GUARDED_GAME_PROFILE_DEFINITIONS = {
    "NOINITCRASHREPORTER": "1",
    "CRASH_REPORTER_WITH_ANALYTICS": "0",
    "USE_CRASH_REPORTER_MONITOR": "0",
}
GUARDED_RECEIPT_TARGETS = ("WoT",)
SOURCE_SNAPSHOT_PATHS = ("Source", "Tools")
SOURCE_SNAPSHOT_FORMAT = "wot-source-snapshot-v1"
TOOLS_SEED_EXACT_EXCLUSIONS = frozenset({
    "Tools/local.json",
    "Tools/test/net/nodes.json",
})
TOOLS_SEED_DIRECTORY_EXCLUSIONS = frozenset({"__pycache__", "logs"})
PRODUCT_RECEIPT_FORMAT = "wot-guarded-product-receipt-v1"
STAGED_TREE_RECEIPT_FORMAT = "wot-staged-tree-inventory-v3"
BINARY_LITERAL_AUDIT_FORMAT = "wot-staged-binary-literal-audit-v1"
CRASH_REPORT_CLIENT_AUDIT_FORMAT = "wot-crash-report-client-audit-v1"
BINARY_LITERAL_NEEDLES = ("archive.org", "twot_gog_classic")
COOKED_CONTAINER_RECEIPT_FORMAT = "wot-guarded-cooked-container-inventory-v2"
PACKAGE_SNAPSHOT_FORMAT = "wot-private-package-snapshot-v1"
PACKAGE_SNAPSHOT_PREFIX = ".wot-package-snapshot-v1-"
PACKAGE_SNAPSHOT_MARKER = ".wot-package-snapshot-owner.json"
ZEN_RECEIPT_FORMAT = "wot-guarded-zen-lifetime-v1"
ZEN_SPONSOR_PORT = 18558
ZEN_SPONSOR_ENDPOINT = f"http://127.0.0.1:{ZEN_SPONSOR_PORT}"
WIN64_SERIAL_LOCAL_UBT_ARGUMENTS = (
    "-MaxParallelActions=1", "-NoXGE", "-NoFASTBuild",
    "-NoSNDBS", "-NoUBA", "-UBADisableRemote",
)
ZEN_NO_AUTO_LAUNCH = f"-NoZenAutoLaunch=127.0.0.1:{ZEN_SPONSOR_PORT}"
ZEN_MONITOR_INTERVAL_SECONDS = 0.1
_ACTIVE_GUARDED_ZEN = False
_ACTIVE_GUARDED_ZEN_COMMANDS: list[dict] = []
_ACTIVE_GUARDED_DDC_OVERRIDES: list[str] = []
# import_all owns these no-replace outputs inside the disposable isolated
# project.  The bootstrap map is also checked in for editor workflows, so the
# seed copy must deliberately omit it and an interrupted import must clear only
# these exact generated outputs before retrying.
IMPORT_GENERATED_OUTPUTS = (
    Path("Content/WoT/Maps/WoTBootstrap.umap"),
    Path("Saved/ImportReceipts/bootstrap-map-denied-read-open.json"),
    Path("Saved/ImportReceipts/bootstrap-map-output-identity.json"),
    ALTERNATE_BIND_IMPORT_RECEIPT,
)


class BuildFailure(RuntimeError):
    """A stripped-pak step failed closed."""


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    # newline="\n" (via open; Path.write_text's newline kwarg is 3.10+ and the
    # Mac host runs 3.9): release.json binds receipts by the SHA-256 of these
    # bytes, and verifiers recompute that hash from canonical LF JSON, so
    # Windows text-mode CRLF translation made every on-disk receipt digest
    # unverifiable ("guarded product receipt digest does not verify").
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def _stamp_path(iso: Path, step: str) -> Path:
    return iso / ".stamps" / f"{STEP_NUMBERS[step]:02d}-{step}.json"


def _read_stamp(iso: Path, step: str) -> dict | None:
    try:
        value = json.loads(_stamp_path(iso, step).read_text())
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) and value.get("step") == step else None


def _write_stamp(iso: Path, step: str, details: dict) -> None:
    _write_json(_stamp_path(iso, step), {
        "step": step,
        "completed_utc": _utc_now(),
        **details,
    })


def _dependent_steps(step: str) -> tuple[str, ...]:
    """``step`` plus every step that transitively depends on it.

    For the linear Test chain this is exactly "this step and every later one";
    the Shipping branch hangs off the cook, so re-packaging the Test release
    invalidates only shipping-package, never the (expensive) Shipping stage.
    """
    affected = {step}
    changed = True
    while changed:
        changed = False
        for name, prerequisites in PREREQUISITES.items():
            if name not in affected and affected.intersection(prerequisites):
                affected.add(name)
                changed = True
    return tuple(name for name in STEPS if name in affected)


def _invalidate_from(iso: Path, step: str) -> None:
    for name in _dependent_steps(step):
        _stamp_path(iso, name).unlink(missing_ok=True)


def _split_named(values: Iterable[str], kind: str) -> set[str]:
    aliases = {name: name for name in STEPS}
    aliases.update({str(index): name for index, name in enumerate(STEPS, 1)})
    result = set()
    unknown = []
    for raw in values:
        for item in raw.split(","):
            key = item.strip().casefold().replace("_", "-")
            if not key:
                continue
            if key not in aliases:
                unknown.append(item)
            else:
                result.add(aliases[key])
    if unknown:
        raise BuildFailure(
            f"unknown {kind}(s): {', '.join(unknown)}; choices: {', '.join(STEPS)}")
    return result


def _validate_iso(iso: Path) -> Path:
    iso = iso.expanduser().resolve()
    try:
        relative = iso.relative_to(REPO.resolve())
    except ValueError as exc:
        raise BuildFailure(f"--iso must stay inside {REPO}: {iso}") from exc
    if not relative.parts:
        raise BuildFailure("--iso may not be the repository root")
    # Every seeded DIRECTORY is forbidden: _copy_tree would discover the iso
    # beneath its own source and copy recursively ("Build" was once
    # missed when it joined the copy set).
    if relative.parts[0] in {name for name in GUARDED_SEED_COPY_SET
                             if name != "WoT.uproject"}:
        raise BuildFailure(
            "--iso may not be nested under a seeded source directory")
    return iso


def _copy_tree(source: Path, target: Path, *, hardlink_files: bool) -> dict:
    counts = {"directories": 0, "hardlinks": 0, "copies": 0, "symlinks": 0}
    for root, directories, files in os.walk(source, followlinks=False):
        root_path = Path(root)
        relative = root_path.relative_to(source)
        destination_root = target / relative
        destination_root.mkdir(parents=True, exist_ok=True)
        counts["directories"] += 1
        for name in list(directories):
            item = root_path / name
            if item.is_symlink():
                destination = destination_root / name
                destination.symlink_to(os.readlink(item), target_is_directory=True)
                directories.remove(name)
                counts["symlinks"] += 1
        for name in files:
            item = root_path / name
            destination = destination_root / name
            if item.is_symlink():
                destination.symlink_to(os.readlink(item))
                counts["symlinks"] += 1
                continue
            if hardlink_files:
                try:
                    os.link(item, destination)
                    counts["hardlinks"] += 1
                    continue
                except OSError:
                    pass
            shutil.copy2(item, destination)
            counts["copies"] += 1
    return counts


def _tools_seed_inventory(root: Path) -> list[str]:
    """Return only Git-tracked or source-snapshotted Tools files.

    Ignored machine state is intentionally outside both this inventory and the
    guarded source snapshot.  Explicit exclusions stay excluded even if a local
    checkout accidentally tracks them.
    """
    raw = _git_output(
        root,
        ["ls-files", "--cached", "--others", "--exclude-standard", "-z",
         "--", "Tools"],
        "the Tools seed inventory")
    inventory = []
    for encoded in sorted(item for item in raw.split(b"\0") if item):
        try:
            relative = encoded.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise BuildFailure("a Tools seed path is not UTF-8") from exc
        path = PurePosixPath(relative)
        if relative in TOOLS_SEED_EXACT_EXCLUSIONS:
            continue
        if any(part.casefold() in TOOLS_SEED_DIRECTORY_EXCLUSIONS
               for part in path.parts):
            continue
        if path.suffix.casefold() == ".log":
            continue
        source = root / relative
        if source.is_file() or source.is_symlink():
            inventory.append(relative)
    return inventory


def _copy_tools_seed(source_root: Path, target_root: Path) -> dict:
    counts = {"directories": 0, "hardlinks": 0, "copies": 0, "symlinks": 0}
    created_directories = set()
    for relative in _tools_seed_inventory(source_root):
        source = source_root / relative
        destination = target_root / relative
        missing = []
        parent = destination.parent
        while parent != target_root and not parent.exists():
            missing.append(parent)
            parent = parent.parent
        destination.parent.mkdir(parents=True, exist_ok=True)
        created_directories.update(missing)
        if source.is_symlink():
            destination.symlink_to(os.readlink(source),
                                   target_is_directory=source.is_dir())
            counts["symlinks"] += 1
        else:
            shutil.copy2(source, destination)
            counts["copies"] += 1
    counts["directories"] = len(created_directories)
    return counts


def _merge_counts(destination: dict, source: dict) -> None:
    for key, value in source.items():
        destination[key] = destination.get(key, 0) + value


def _editor_module_mtime(root: Path) -> float:
    # The compiled editor module is the seed's one time-sensitive product: a
    # rebuild on the source tree must invalidate a copied ISO (a stale module
    # makes the -unattended editor drop the pythonscript commandlet silently).
    dylib = root / "Binaries" / "Mac" / "libUnrealEditor-WoT.dylib"
    dll = root / "Binaries" / "Win64" / "UnrealEditor-WoT.dll"
    for product in (dylib, dll):
        if product.exists():
            return product.stat().st_mtime
    return 0.0


def _entitlements_digests(root: Path) -> dict:
    # The cook signs the staged client with these files; a stale iso copy
    # would silently re-sandbox the client, so seed validity pins their
    # repo content.
    import hashlib
    digests = {}
    resources = root / "Build" / "Mac" / "Resources"
    for path in sorted(resources.glob("*.entitlements")):
        digests[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return digests


def _git_output(root: Path, arguments: Sequence[str], what: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments], capture_output=True)
    if result.returncode != 0:
        error = result.stderr.decode("utf-8", errors="replace").strip()
        raise BuildFailure(f"cannot capture {what}: {error}")
    return result.stdout


def _source_snapshot(root: Path) -> dict:
    """Bind the non-commit Source/Tools state that is copied into the ISO.

    HEAD already names every unchanged tracked byte.  The binary tracked diff
    names every tracked deviation (including staged changes and deletions), and
    the explicit untracked inventory closes the only remaining source surface.
    """
    tracked_diff = _git_output(
        root,
        ["diff", "--binary", "--no-ext-diff", "HEAD", "--",
         *SOURCE_SNAPSHOT_PATHS],
        "the tracked Source/Tools diff")
    untracked_raw = _git_output(
        root,
        ["ls-files", "--others", "--exclude-standard", "-z", "--",
         *SOURCE_SNAPSHOT_PATHS],
        "the untracked Source/Tools inventory")
    untracked = []
    for encoded in sorted(item for item in untracked_raw.split(b"\0") if item):
        try:
            relative = encoded.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise BuildFailure(
                "an untracked Source/Tools path is not UTF-8") from exc
        path = root / relative
        if path.is_symlink():
            target = os.readlink(path)
            untracked.append({
                "path": relative,
                "type": "symlink",
                "target": target,
            })
        elif path.is_file():
            untracked.append({
                "path": relative,
                "type": "file",
                "size": path.stat().st_size,
                "sha256": _sha256_file(path),
            })
        else:
            raise BuildFailure(
                f"untracked source inventory object is not a file/link: {path}")
    identity = {
        "format": SOURCE_SNAPSHOT_FORMAT,
        "git_commit": _git_commit(root),
        "paths": list(SOURCE_SNAPSHOT_PATHS),
        "tracked_diff_size": len(tracked_diff),
        "tracked_diff_sha256": hashlib.sha256(tracked_diff).hexdigest(),
        "untracked": untracked,
    }
    identity["sha256"] = hashlib.sha256(
        _canonical_release_bytes(identity)).hexdigest()
    return identity


def _neutral_support_asset_relative_path(package: str) -> str:
    return "Content" + package.removeprefix("/Game") + ".uasset"


def _read_authority_bytes(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise BuildFailure(
            f"neutral support authority is missing or malformed: {exc}") from exc


def _neutral_support_asset_receipt(root: Path) -> dict:
    authority_path = root / NEUTRAL_SUPPORT_AUTHORITY_PATH
    try:
        authority = json.loads(authority_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BuildFailure(
            f"neutral support authority is missing or malformed: {exc}") from exc
    if not isinstance(authority, dict) or set(authority) != {
            "format", "roots", "hard_dependencies", "assets"}:
        raise BuildFailure("neutral support authority has an invalid schema")
    if authority.get("format") != NEUTRAL_SUPPORT_AUTHORITY_FORMAT:
        raise BuildFailure("neutral support authority format is not approved")
    if authority.get("roots") != list(NEUTRAL_SUPPORT_ROOT_PACKAGES):
        raise BuildFailure(
            "neutral support authority does not preserve the exact ordered roots")
    if authority.get("hard_dependencies") != list(
            NEUTRAL_SUPPORT_HARD_DEPENDENCIES):
        raise BuildFailure(
            "neutral support authority does not preserve the exact hard dependencies")
    rows = authority.get("assets")
    if not isinstance(rows, list) or len(rows) != len(NEUTRAL_SUPPORT_PACKAGES):
        raise BuildFailure(
            "neutral support authority does not name the exact assets")
    expected_packages = list(NEUTRAL_SUPPORT_PACKAGES)
    actual_packages = [
        row.get("package") if isinstance(row, dict) else None for row in rows]
    if actual_packages != expected_packages or len(set(actual_packages)) != len(rows):
        raise BuildFailure(
            "neutral support authority does not name the exact assets")
    verified_rows = []
    for package, row in zip(expected_packages, rows):
        if not isinstance(row, dict) or set(row) != {
                "package", "relative_path", "sha256", "size"}:
            raise BuildFailure(
                f"neutral support authority row is malformed: {package}")
        expected_relative = _neutral_support_asset_relative_path(package)
        if row.get("relative_path") != expected_relative:
            raise BuildFailure(
                f"neutral support authority path is wrong: {package}")
        path = root / expected_relative
        try:
            # Each path is derived from the closed package vector above. The
            # Windows worktree intentionally exposes ignored Content through a
            # junction to the shared authoring tree, so a resolved-path
            # containment check would reject the approved source bytes.
            if not path.is_file():
                raise FileNotFoundError(path)
            size = path.stat().st_size
            sha256 = _sha256_file(path)
        except OSError as exc:
            raise BuildFailure(
                f"neutral support asset is missing: {expected_relative}") from exc
        if row.get("size") != size:
            raise BuildFailure(
                f"neutral support asset size changed: {expected_relative}")
        if row.get("sha256") != sha256:
            raise BuildFailure(
                f"neutral support asset sha256 changed: {expected_relative}")
        verified_rows.append(dict(row))
    assets_sha256 = hashlib.sha256(json.dumps(
        verified_rows, sort_keys=True, separators=(",", ":"),
        ensure_ascii=True).encode("ascii")).hexdigest()
    # The authority digest names CONTENT, not a checkout: a Windows autocrlf
    # checkout turns the committed LF file into CRLF, and hashing those bytes
    # recorded a digest the first-run funnel (which recomputes the canonical
    # pretty JSON) could never accept. Hash
    # the LF-normalized bytes and require them to BE the canonical form.
    authority_bytes = _read_authority_bytes(authority_path).replace(
        b"\r\n", b"\n")
    canonical = (json.dumps(authority, indent=2, sort_keys=True)
                 + "\n").encode("utf-8")
    if authority_bytes != canonical:
        raise BuildFailure(
            "neutral support authority is not canonical pretty JSON "
            "(indent 2, sorted keys, LF); regenerate it")
    return {
        "format": NEUTRAL_SUPPORT_RECEIPT_FORMAT,
        "authority_format": NEUTRAL_SUPPORT_AUTHORITY_FORMAT,
        "authority_relative_path": NEUTRAL_SUPPORT_AUTHORITY_PATH.as_posix(),
        "authority_sha256": hashlib.sha256(authority_bytes).hexdigest(),
        "asset_count": len(verified_rows),
        "roots": list(NEUTRAL_SUPPORT_ROOT_PACKAGES),
        "hard_dependencies": list(NEUTRAL_SUPPORT_HARD_DEPENDENCIES),
        "assets": verified_rows,
        "assets_sha256": assets_sha256,
    }


def _validate_neutral_support_receipt(receipt: object) -> dict:
    expected_keys = {
        "format", "authority_format", "authority_relative_path",
        "authority_sha256", "asset_count", "roots", "hard_dependencies",
        "assets", "assets_sha256",
    }
    if not isinstance(receipt, dict) or set(receipt) != expected_keys:
        raise BuildFailure("guarded neutral support proof is missing or malformed")
    if (receipt.get("format") != NEUTRAL_SUPPORT_RECEIPT_FORMAT
            or receipt.get("authority_format")
                != NEUTRAL_SUPPORT_AUTHORITY_FORMAT
            or receipt.get("authority_relative_path")
                != NEUTRAL_SUPPORT_AUTHORITY_PATH.as_posix()
            or receipt.get("asset_count") != len(NEUTRAL_SUPPORT_PACKAGES)
            or receipt.get("roots") != list(NEUTRAL_SUPPORT_ROOT_PACKAGES)
            or receipt.get("hard_dependencies")
                != list(NEUTRAL_SUPPORT_HARD_DEPENDENCIES)):
        raise BuildFailure("guarded neutral support proof identity is wrong")
    assets = receipt.get("assets")
    if (not isinstance(assets, list)
            or [row.get("package") if isinstance(row, dict) else None
                for row in assets] != list(NEUTRAL_SUPPORT_PACKAGES)):
        raise BuildFailure("guarded neutral support proof does not bind exact assets")
    for digest_name in ("authority_sha256", "assets_sha256"):
        digest = receipt.get(digest_name)
        if (not isinstance(digest, str) or len(digest) != 64
                or any(char not in "0123456789abcdefABCDEF" for char in digest)):
            raise BuildFailure(
                f"guarded neutral support proof has invalid {digest_name}")
    return receipt


def _neutral_support_lifecycle_receipt(
        iso: Path, *, require_import: bool) -> dict:
    source = _neutral_support_asset_receipt(REPO)
    isolated = _neutral_support_asset_receipt(iso)
    seed = (_read_stamp(iso, "seed") or {}).get(
        "neutral_support_authority")
    if source != isolated or seed != source:
        raise BuildFailure(
            "neutral support authority differs across source and seed")
    if require_import:
        imported = (_read_stamp(iso, "import") or {}).get(
            "neutral_support_authority")
        if imported != source:
            raise BuildFailure(
                "neutral support authority differs across seed and import")
    return _validate_neutral_support_receipt(source)


def _seed_valid(iso: Path, *, guarded: bool = False,
                release_sequence: int = 0,
                human_version: str = "") -> bool:
    stamp = _read_stamp(iso, "seed")
    copy_set = GUARDED_SEED_COPY_SET if guarded else SEED_COPY_SET
    if not _seed_inputs_valid(
            iso, stamp, copy_set, require_generated_outputs=guarded,
            release_sequence=release_sequence,
            human_version=human_version):
        return False
    # The release commit is cooked into DefaultGame.ini. A moved HEAD must
    # rebuild the isolated seed rather than reusing a differently pinned cook.
    if stamp.get("release_commit") != _git_commit(REPO):
        return False
    # Re-seed when the source editor module was rebuilt after the copy, or
    # when the signing entitlements changed under the copied Build/.
    if stamp.get("entitlements_sha256") != _entitlements_digests(REPO):
        return False
    if guarded:
        try:
            if stamp.get("neutral_support_authority") != \
                    _neutral_support_lifecycle_receipt(
                        iso, require_import=False):
                return False
        except BuildFailure:
            return False
        if stamp.get("source_snapshot") != _source_snapshot(REPO):
            return False
        if stamp.get("guarded_cook_config") != (
                "clear-directories-to-always-cook-v1"):
            return False
    return stamp.get("editor_module_mtime") == _editor_module_mtime(REPO)


def _seed_inputs_valid(iso: Path, stamp: dict | None,
                       copy_set: Sequence[str], *,
                       require_generated_outputs: bool = False,
                       release_sequence: int = 0,
                       human_version: str = "") -> bool:
    version_path = iso / "Build" / "WoTRelease" / "release-version.json"
    try:
        version_matches = (
            version_path.read_bytes()
            == release_formats.release_version_bytes(human_version))
    except OSError:
        version_matches = False
    return bool(
        stamp
        and stamp.get("copy_set") == list(copy_set)
        and (not require_generated_outputs
             or stamp.get("import_generated_outputs") == [
                 path.as_posix() for path in IMPORT_GENERATED_OUTPUTS])
        and all((iso / name).exists() for name in copy_set)
        and stamp.get("release_sequence") == release_sequence
        and stamp.get("human_version", "") == human_version
        and version_matches
        and stamp.get("release_commit") == _git_commit(REPO)
        and stamp.get("entitlements_sha256") == _entitlements_digests(REPO)
        and stamp.get("editor_module_mtime") == _editor_module_mtime(REPO))


def _upgrade_legacy_seed_build_inputs(iso: Path,
                                      release_sequence: int = 0,
                                      human_version: str = "") -> bool:
    """Add compile-only seed inputs without discarding completed imports."""
    stamp = _read_stamp(iso, "seed")
    if not _seed_inputs_valid(
            iso, stamp, SEED_COPY_SET, require_generated_outputs=True,
            release_sequence=release_sequence,
            human_version=human_version):
        return False
    # A pre-snapshot seed cannot prove which dirty/untracked scripts produced
    # its imported outputs. It must be re-seeded instead of being "upgraded"
    # into release evidence retroactively.
    if stamp.get("source_snapshot") != _source_snapshot(REPO):
        return False
    try:
        if stamp.get("neutral_support_authority") != \
                _neutral_support_lifecycle_receipt(
                    iso, require_import=False):
            return False
    except BuildFailure:
        return False
    source = REPO / "ThirdParty"
    if not source.is_dir():
        raise BuildFailure(f"seed source is missing: {source.name}")
    target = iso / "ThirdParty"
    if target.is_dir() and not target.is_symlink():
        shutil.rmtree(target)
    else:
        target.unlink(missing_ok=True)
    counts = dict(stamp.get("copy_counts", {}))
    _merge_counts(counts, _copy_tree(source, target, hardlink_files=False))
    invalidated_makefiles = _invalidate_ubt_makefiles(iso)
    _configure_guarded_cook(iso / "Config" / "DefaultGame.ini")
    # ThirdParty is a compile/link input, not an import input. Preserve the
    # expensive import/marker evidence, but invalidate the failed cook and all
    # downstream release evidence.
    _invalidate_from(iso, "cook")
    details = {
        key: value for key, value in stamp.items()
        if key not in {"step", "completed_utc"}
    }
    details.update({
        "copy_set": list(GUARDED_SEED_COPY_SET),
        "copy_counts": counts,
        "added_build_inputs": ["ThirdParty"],
        "invalidated_ubt_makefiles": invalidated_makefiles,
        "guarded_cook_config": "clear-directories-to-always-cook-v1",
    })
    _write_stamp(iso, "seed", details)
    return True


def _invalidate_ubt_makefiles(iso: Path) -> list[str]:
    """Drop only cached dependency graphs after adding compile inputs."""
    root = iso / "Intermediate" / "Build"
    invalidated = []
    for makefile in sorted(root.glob("**/Makefile.bin")):
        makefile.unlink()
        invalidated.append(makefile.relative_to(iso).as_posix())
    return invalidated


def _module_build_id(modules_path: Path) -> str | None:
    try:
        value = json.loads(modules_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    build_id = value.get("BuildId")
    return build_id if isinstance(build_id, str) and build_id else None


def _ensure_repo_editor_module(args) -> None:
    """Rebuild the repo's editor module when its BuildId drifts from the engine.

    Every guarded target build restamps the source engine's shared BuildId,
    orphaning the repo's UnrealEditor-WoT module for the NEXT pipeline run's
    import commandlet - which then fails as "the game module 'WoT' could not
    be found" behind an auto-closed dialog (Win64; on Mac the same drift
    otherwise needs a manual "resync WoTEditor with the SOURCE engine").
    The pipeline heals the drift it causes.
    """
    engine_root, _editor = _engine_root_and_editor(args.engine)
    binaries = "Mac" if args.platform == "Mac" else "Win64"
    engine_modules = (engine_root / "Engine" / "Binaries" / binaries
                      / "UnrealEditor.modules")
    repo_modules = REPO / "Binaries" / binaries / "UnrealEditor.modules"
    engine_id = _module_build_id(engine_modules)
    if engine_id is not None and engine_id == _module_build_id(repo_modules):
        return
    if args.platform == "Mac":
        command = [
            str(engine_root / "Engine" / "Build" / "BatchFiles" / "Mac"
                / "Build.sh"),
            "WoTEditor", "Mac", "Development",
            f"-project={REPO / 'WoT.uproject'}",
        ]
    else:
        command = [
            str(engine_root / "Engine" / "Build" / "BatchFiles" / "Build.bat"),
            "WoTEditor", "Win64", "Development",
            f"-project={REPO / 'WoT.uproject'}", "-NoHotReload",
        ]
        if getattr(args, "guarded", False):
            command.extend(WIN64_SERIAL_LOCAL_UBT_ARGUMENTS)
    log_path = REPO / "Saved" / "Logs" / "pipeline-editor-resync.log"
    print("[seed] RESYNC repo editor module (BuildId drift) "
          + subprocess.list2cmdline(command), flush=True)
    exit_code = _run_logged("seed", command, log_path)
    if exit_code != 0:
        raise BuildFailure(
            f"editor module resync failed ({exit_code}); see {log_path}")


def _configure_guarded_cook(config: Path) -> None:
    """Clear project-wide always-cook roots in the disposable guarded seed.

    Guarded release content is closed by the explicit bootstrap/front-end and
    nineteen neutral-shell map roots.  The developer project deliberately
    always-cooks /Game/WoT because ordinary gameplay uses dynamic soft paths;
    inheriting that setting here would silently ship the imported retail tree.
    """
    try:
        original = config.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise BuildFailure(f"cannot read guarded cook config {config}: {exc}") from exc
    section = "[/Script/UnrealEd.ProjectPackagingSettings]"
    lines = original.splitlines(keepends=True)
    section_index = next((
        index for index, line in enumerate(lines)
        if line.strip().casefold() == section.casefold()), None)
    assignment = re.compile(
        r"^\s*[+.!-]?\s*DirectoriesToAlwaysCook\s*=", re.IGNORECASE)
    filtered = [line for line in lines if not assignment.match(line)]
    if section_index is None:
        if filtered and not filtered[-1].endswith(("\n", "\r")):
            filtered[-1] += "\n"
        filtered.extend((
            "\n" + section + "\n",
            "!DirectoriesToAlwaysCook=ClearArray\n",
        ))
    else:
        filtered_section_index = next(
            index for index, line in enumerate(filtered)
            if line.strip().casefold() == section.casefold())
        filtered.insert(filtered_section_index + 1,
                        "!DirectoriesToAlwaysCook=ClearArray\n")
    updated = "".join(filtered)
    temporary = config.with_name(config.name + ".tmp")
    temporary.write_text(updated, encoding="utf-8")
    temporary.replace(config)
    assignments = [line.strip() for line in updated.splitlines()
                   if assignment.match(line)]
    if assignments != ["!DirectoriesToAlwaysCook=ClearArray"]:
        raise BuildFailure(
            "guarded cook config did not clear all always-cook roots")


def stage_seed(iso: Path, *, guarded: bool = False,
               release_sequence: int = 0,
               human_version: str = "") -> str:
    neutral_support_authority = (
        _neutral_support_asset_receipt(REPO) if guarded else None)
    copy_set = GUARDED_SEED_COPY_SET if guarded else SEED_COPY_SET
    if _seed_valid(iso, guarded=guarded,
                   release_sequence=release_sequence,
                   human_version=human_version):
        return "cached"
    if guarded and _upgrade_legacy_seed_build_inputs(
            iso, release_sequence=release_sequence,
            human_version=human_version):
        return "ran"
    source_snapshot = _source_snapshot(REPO) if guarded else None
    _invalidate_from(iso, "seed")
    iso.mkdir(parents=True, exist_ok=True)
    missing = [name for name in copy_set if not (REPO / name).exists()]
    if missing:
        raise BuildFailure("seed source is missing: " + ", ".join(missing))
    # A stamp-less seed may be a partial prior copy. Remove only the exact
    # disposable seed targets; logs/stamps outside them remain diagnostic.
    for name in copy_set:
        target = iso / name
        if target.is_dir() and not target.is_symlink():
            shutil.rmtree(target)
        else:
            target.unlink(missing_ok=True)

    shutil.copy2(REPO / "WoT.uproject", iso / "WoT.uproject")
    counts = {"directories": 0, "hardlinks": 0, "copies": 1, "symlinks": 0}
    _merge_counts(counts, _copy_tree(
        REPO / "Config", iso / "Config", hardlink_files=False))
    default_game_ini = iso / "Config" / "DefaultGame.ini"
    section_pattern = re.compile(
        r"^\s*\[\s*WoTNetVersion\s*\]\s*(?:[;#].*)?$", re.IGNORECASE)
    if any(section_pattern.match(line)
           for line in default_game_ini.read_text().splitlines()):
        raise BuildFailure(
            "repo Config/DefaultGame.ini must not contain [WoTNetVersion]")
    release_commit = _git_commit(REPO)
    if guarded:
        _configure_guarded_cook(default_game_ini)
    with default_game_ini.open("a", encoding="utf-8", newline="\n") as handle:
        handle.write(
            f"\n[WoTNetVersion]\nReleaseCommit={release_commit}\n")

    for name in copy_set:
        if name in {"WoT.uproject", "Config", "Content"}:
            continue
        if name == "Tools":
            _merge_counts(counts, _copy_tools_seed(REPO, iso))
        else:
            _merge_counts(counts, _copy_tree(
                REPO / name, iso / name, hardlink_files=False))
    # NEVER seed packaged-target products: the pipeline builds its own
    # WoT, and a stale repo game binary planted here poisoned
    # roughly every other Win64 artifact - UAT's incremental build sometimes
    # accepted the planted July-31 client as up to date, shipping a binary
    # that predated the session-mint fixes and failed every virgin boot with
    # GUARDED-SESSION-ABSENT while identical-source rebuilds worked
    # (root-caused 2026-08-05: the packaged exe lacked the current source's
    # diagnostic strings). Editor modules stay: the import commandlet loads
    # them.
    for platform_directory in ("Win64", "Mac"):
        binaries = iso / "Binaries" / platform_directory
        if not binaries.is_dir():
            continue
        for pattern in ("WoT-*", "WoTUpdater*", "WoT.exe", "WoT"):
            for stale in binaries.glob(pattern):
                if stale.is_file():
                    stale.unlink()
    # Content dominates the workspace. Prefer cp -al semantics per file and
    # fall back to copy2 on filesystems that reject hardlinks.
    _merge_counts(counts, _copy_tree(
        REPO / "Content", iso / "Content", hardlink_files=True))
    identity_path = iso / "Build" / "WoTRelease" / "release-identity.json"
    identity_path.parent.mkdir(parents=True, exist_ok=True)
    identity_path.write_bytes(release_formats.canonical_bytes({
        "format": release_formats.RELEASE_IDENTITY_FORMAT,
        "release_commit": release_commit,
        "release_sequence": release_sequence,
        "version": release_formats.RELEASE_IDENTITY_VERSION,
    }, "release identity"))
    version_path = iso / "Build" / "WoTRelease" / "release-version.json"
    version_path.write_bytes(release_formats.release_version_bytes(human_version))
    if guarded:
        isolated_neutral_support = _neutral_support_asset_receipt(iso)
        if isolated_neutral_support != neutral_support_authority:
            raise BuildFailure(
                "neutral support assets changed while the ISO seed was copied")
    # WoTBootstrap is a reviewed generated asset in the worktree, but this
    # pipeline is required to regenerate it via import_all's empty/no-replace
    # bootstrap-map leg. Unlinking the isolated hardlink leaves the worktree
    # inode intact and makes generation ownership unambiguous.
    if guarded:
        (iso / IMPORT_GENERATED_OUTPUTS[0]).unlink(missing_ok=True)
    # The stripped SOUND pass creates UWoTPayloadSoundWave assets — a
    # DIFFERENT class from the canonical USoundWave at the same paths — and
    # its batch prevalidation rightly fails closed on a class swap (first
    # composed run). The seed therefore leaves the sound tree empty for the
    # stripped importer to rebuild; every other branch overwrites same-class
    # assets in place and Audit 1 remains the completeness guarantee.
    shutil.rmtree(iso / "Content" / "WoT" / "Sounds", ignore_errors=True)
    details = {
        "source": str(REPO.resolve()),
        "copy_set": list(copy_set),
        "excluded": [
            "Saved", "Intermediate", "DerivedDataCache",
            "Tools/local.json", "Tools/test/net/nodes.json",
            "Tools/**/__pycache__", "Tools/**/logs", "Tools/**/*.log",
        ],
        "editor_module_mtime": _editor_module_mtime(REPO),
        "entitlements_sha256": _entitlements_digests(REPO),
        "release_commit": release_commit,
        "release_sequence": release_sequence,
        "human_version": human_version,
        "copy_counts": counts,
    }
    if guarded:
        details["guarded_cook_config"] = (
            "clear-directories-to-always-cook-v1")
        details["import_generated_outputs"] = [
            path.as_posix() for path in IMPORT_GENERATED_OUTPUTS]
        # File-copy seeding deliberately leaves .git untouched. Re-capturing
        # the identity after the copy rejects a source mutation during seeding.
        after_copy = _source_snapshot(REPO)
        if after_copy != source_snapshot:
            raise BuildFailure("Source/Tools changed while the ISO seed was copied")
        details["source_snapshot"] = source_snapshot
        details["source_snapshot_method"] = (
            "file-copy-with-before-and-after-snapshot-v1")
        details["neutral_support_authority"] = neutral_support_authority
    _write_stamp(iso, "seed", details)
    return "ran"


def _quote_script_value(value: Path | str) -> str:
    # NO quoting: these strings travel as ONE subprocess argv element, so any
    # embedded quotes are LITERAL by the time UE re-tokenizes its command
    # line (the first live run's pythonscript saw -script="\"/path...\"" and
    # never ran). Every input path in this pipeline is space-free; enforce
    # that instead of quoting.
    # FORWARD SLASHES only: UPythonScriptCommandlet re-parses the quoted
    # -script value through FParse::QuotedString, which PROCESSES C ESCAPE
    # SEQUENCES, so a Windows path like ...\specs\neutral-map-shells-v1.json
    # turns its \n into a newline and silently drops every argument after it.
    # Python and UE both accept drive-letter:/ style Windows paths, and Mac paths have
    # no backslashes, so this is a no-op there.
    text = str(value).replace("\\", "/")
    if '"' in text or " " in text:
        raise BuildFailure(
            f"path with spaces/quotes cannot cross the commandlet boundary: {text}")
    return text


def _engine_root_and_editor(engine: Path | None) -> tuple[Path, Path]:
    candidates = []
    if engine is not None:
        candidates.append(engine.expanduser())
    platform_name = {"Darwin": "Mac", "Windows": "Win64"}.get(
        host_platform.system())
    if platform_name:
        try:
            configured = installed_engine_root(platform_name, required=False)
        except ConfigurationError as exc:
            raise BuildFailure(str(exc)) from exc
        if configured is not None:
            candidates.append(configured)
    for candidate in candidates:
        editor = engine_executable(candidate)
        if editor is None:
            continue
        root = candidate
        if candidate.is_file():
            try:
                root = candidate.parents[3]
            except IndexError:
                root = candidate.parent
        return root.resolve(), editor.resolve()
    rendered = ", ".join(str(item) for item in candidates) or "no default"
    setting = (" or configure installed_engine_root_%s in Tools/local.json"
               % platform_name.lower()) if platform_name else ""
    raise BuildFailure(
        f"UE 5.8 UnrealEditor-Cmd not found; pass --engine{setting} "
        f"(probed {rendered})")


def _mode_is_guarded(args) -> bool:
    return bool(getattr(args, "guarded", False))


def _editor_process_affinity_arguments(value: int | None) -> list[str]:
    return [] if value is None else [f"-processaffinity={value}"]


def _resolve_build_mode(args) -> None:
    """Resolve defaults and reject configuration/engine downgrades early."""
    guarded = _mode_is_guarded(args)
    configuration = getattr(args, "configuration", None)
    if guarded:
        args.configuration = configuration or GUARDED_CONFIGURATION
        if args.configuration not in GUARDED_CONFIGURATIONS:
            raise BuildFailure(
                "--guarded requires -clientconfig=Test (funnel) or Shipping "
                "(Test funnel + Shipping release); Development is refused")
        pinned = PINNED_SOURCE_ENGINES.get(args.platform)
        if pinned is None:
            raise BuildFailure(
                "guarded builds require source_engine_root_%s in Tools/local.json "
                "or WOT_SOURCE_ENGINE_ROOT_%s"
                % (args.platform.lower(), args.platform.upper()))
        if args.engine is None:
            args.engine = pinned
        supplied = args.engine.expanduser()
        executable_names = {"UnrealEditor-Cmd", "UnrealEditor-Cmd.exe"}
        if supplied.name in executable_names:
            try:
                supplied = supplied.parents[3]
            except IndexError as exc:
                raise BuildFailure(
                    "--guarded engine executable has no source-engine root") from exc
        # strict=False is intentional: dry-run still refuses a launcher path
        # without requiring the source engine to be mounted.
        if supplied.resolve(strict=False) != pinned.resolve(strict=False):
            raise BuildFailure(
                f"--guarded requires the pinned source engine {pinned}; "
                f"refusing {args.engine}")
        installed_marker = supplied / "Engine" / "Build" / "InstalledBuild.txt"
        if installed_marker.exists():
            raise BuildFailure(
                f"--guarded refuses launcher/installed engine {supplied}")
    else:
        args.configuration = configuration or "Development"
        if args.configuration != "Development":
            raise BuildFailure(
                "the stripped flow is frozen to Development; use --guarded for "
                "Test or Shipping")
    if (getattr(args, "editor_process_affinity", None) is not None
            and (not guarded or args.platform != "Win64")):
        raise BuildFailure(
            "--editor-process-affinity requires guarded Win64")
    choice = getattr(args, "compile_cache", None) or "auto"
    if choice not in COMPILE_CACHE_CHOICES:
        raise BuildFailure(f"unknown --compile-cache {choice!r}")
    if choice == "on" and not guarded:
        raise BuildFailure("--compile-cache on requires --guarded")
    args.compile_cache = "on" if guarded and (
        choice == "on" or (choice == "auto" and args.platform == "Win64")
    ) else "off"


INNOEXTRACT_VENDOR_MANIFEST = (
    REPO / "ThirdParty" / "innoextract" / "1.9" / "vendor-manifest.json")


def _require_reviewed_innoextract(platform: str,
                                  manifest: Path = INNOEXTRACT_VENDOR_MANIFEST) -> str:
    """Refuse to package anything but the pinned innoextract library.

    A contributor build (Tools/build/build_innoextract_native.py without
    --record) links and runs, and Build.cs accepts it, but it is not the
    reviewed artifact. A guarded package seeds ThirdParty from this tree, so
    the library is held to its vendor-manifest pin before anything is built.
    """
    build_tools = str(TOOLS_DIR / "build")
    if build_tools not in sys.path:
        sys.path.insert(0, build_tools)
    import stage_innoextract_native as vendor
    try:
        return vendor.verify_library(platform, manifest)
    except (vendor.VendorError, OSError, KeyError, ValueError) as exc:
        raise BuildFailure(
            f"guarded packaging needs the reviewed innoextract library: {exc}. "
            f"Run Tools/build/fetch_innoextract_libs.py fetch --platform {platform}"
        ) from exc


def _compile_cache_enabled(args) -> bool:
    return (_mode_is_guarded(args)
            and getattr(args, "compile_cache", "off") == "on")


def _build_root(args) -> Path:
    """<parent>/<Platform>-<pinned engine commit[:12]>, resolved once."""
    cached = getattr(args, "_build_root_path", None)
    if cached is not None:
        return cached
    parent = getattr(args, "build_root_parent", None)
    parent = (Path(parent).expanduser() if parent is not None
              else build_root_cache.default_parent(REPO))
    manifest = REPO / "EnginePatches" / "UE-5.8" / "source-toolchain.json"
    try:
        commit = json.loads(manifest.read_text(encoding="utf-8"))[
            "engine"]["commit"]
    except (OSError, UnicodeError, json.JSONDecodeError, KeyError,
            TypeError) as exc:
        raise BuildFailure(
            f"cannot read the pinned engine commit from {manifest}: {exc}"
        ) from exc
    root = build_root_cache.root_for(
        parent.resolve(), args.platform, str(commit))
    args._build_root_path = root
    return root


def _validate_build_root(root: Path, iso: Path) -> None:
    """The root and the ISO are disjoint, and never inside a seeded tree."""
    root = root.resolve(strict=False)
    iso = iso.resolve(strict=False)
    for inner, outer in ((root, iso), (iso, root)):
        try:
            inner.relative_to(outer)
        except ValueError:
            continue
        raise BuildFailure(
            f"guarded build root {root} and ISO {iso} must be disjoint")
    try:
        relative = root.relative_to(REPO.resolve())
    except ValueError:
        return
    if not relative.parts or relative.parts[0] in {
            name for name in GUARDED_SEED_COPY_SET
            if name != "WoT.uproject"}:
        raise BuildFailure(
            f"guarded build root {root} may not be a seeded source directory")


def _active_build_root(args, iso: Path, engine_root: Path) -> Path | None:
    """Prepare the build root once per run: engine proof, then identity.

    Apply mode first (writes nothing when the engine is already patched, so
    no engine source is re-timestamped), then the same verifier the wrapper
    runs, whose path-independent identity keys the root: a different engine
    commit, toolchain or patch state resets UBT's outputs there.
    """
    if not _compile_cache_enabled(args):
        return None
    root = _build_root(args)
    if getattr(args, "_build_root_ready", None) is not None:
        return root
    _validate_build_root(root, iso)
    patches = iso / "EnginePatches" / "UE-5.8"
    patch_file = patches / "guarded-bootstrap.patch"
    patch_command = [
        sys.executable, str(iso / "Tools" / "dist" / "apply_engine_patch.py"),
        "--engine", str(engine_root),
        "--manifest", str(patches / "manifest.json"),
    ]
    verifier_patch_state = []
    if patch_file.is_file():
        # Private/release trees keep their existing apply-or-verify behavior.
        patch_command.extend(("--patch", str(patch_file)))
    else:
        # Public trees can prove a pre-patched engine from the exported hashes.
        patch_command.extend(("--verify-only", "--require-patched"))
        verifier_patch_state.append("--require-patched")
    _run_logged(
        "build-root-engine-patch",
        patch_command,
        iso / "logs" / "build-root-engine-patch.log",
        env=_iso_tool_environment(args, engine_root))
    receipt = iso / "receipts" / f"{args.platform}-build-root-engine.json"
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.unlink(missing_ok=True)
    _run_logged(
        "build-root-engine",
        [sys.executable,
         str(iso / "Tools" / "build" / "verify_ue58_source.py"),
         "--engine", str(engine_root),
         "--manifest", str(patches / "source-toolchain.json"),
         "--require-source",
         "--patch-manifest", str(patches / "manifest.json"),
         *verifier_patch_state,
         "--receipt", str(receipt)],
        iso / "logs" / "build-root-engine.log",
        env=_iso_tool_environment(args, engine_root))
    identity = build_root_cache.engine_identity(
        _read_json_object(receipt, "build-root engine receipt"))
    prepared = build_root_cache.prepare(root, args.platform, identity)
    if prepared["reset"]:
        print(f"[build-root] reset {root}: {prepared['reset']}", flush=True)
    args._build_root_ready = {
        **prepared,
        "engine_receipt": str(receipt),
        "engine_receipt_sha256": _sha256_file(receipt),
    }
    return root


def _transfer_receipt_path(iso: Path, platform_name: str, target: str,
                           configuration: str, phase: str = "") -> Path:
    suffix = f"-{phase}" if phase else ""
    return (iso / "receipts"
            / f"{platform_name}-{target}-{configuration}-transfer{suffix}.json")


def _compile_cache_row(args, sync: dict, transfer: dict) -> dict:
    ready = getattr(args, "_build_root_ready", None) or {}
    return {
        "format": COMPILE_CACHE_FORMAT,
        "build_root": transfer["receipt"]["build_root"],
        "build_root_reset": ready.get("reset"),
        "build_root_engine_receipt_sha256": ready.get(
            "engine_receipt_sha256"),
        "sync": {key: sync[key] for key in (
            "copied", "replaced", "deleted", "unchanged", "waited_seconds")},
        "input_tree_sha256": sync["input_tree"]["sha256"],
        "transfer_receipt": transfer["path"],
        "transfer_receipt_sha256": transfer["sha256"],
    }


def _editor_build_command(args, engine_root: Path, root: Path) -> list[str]:
    """UAT's editor block (BuildProjectCommand.Automation.cs:90-104), in the
    build root: WoTEditor + ShaderCompileWorker, host Development."""
    project = root / "WoT.uproject"
    if args.platform == "Win64":
        script = engine_root / "Engine" / "Build" / "BatchFiles" / "Build.bat"
    elif args.platform == "Mac":
        script = (engine_root / "Engine" / "Build" / "BatchFiles" / "Mac"
                  / "Build.sh")
    else:
        raise BuildFailure(f"guarded builds do not support {args.platform}")
    command = [
        str(script),
        f"-Target=WoTEditor {args.platform} Development -Project={project}",
        f"-Target=ShaderCompileWorker {args.platform} Development "
        f"-Project={project}",
        "-NoHotReload",
    ]
    if args.platform == "Win64":
        command.extend(WIN64_SERIAL_LOCAL_UBT_ARGUMENTS)
    return command


def _build_root_editor(args, iso: Path, engine_root: Path,
                       phase: str) -> dict:
    """Build the project editor in the build root and install it in the ISO.

    Every editor commandlet of the lifecycle (import, finalize, shells, cook)
    then runs editor modules compiled from the ISO's own snapshot-bound
    Source -- previously only the cook did (UAT -build in the ISO), while the
    import commandlets ran whatever the developer tree last built.
    """
    root = _active_build_root(args, iso, engine_root)
    if root is None:
        raise BuildFailure("the build-root editor requires --compile-cache on")
    sync = build_root_cache.mirror(iso, root)
    command = _editor_build_command(args, engine_root, root)
    log_path = iso / "logs" / f"build-root-editor-{phase}.log"
    _begin_build_root_ubt(root, f"WoTEditor-Development-{phase}")
    succeeded = False
    try:
        _run_logged(f"build-root-editor-{phase}", command, log_path,
                    env=_iso_tool_environment(args, engine_root))
        succeeded = True
    finally:
        build_root_cache.end_ubt(root, succeeded=succeeded)
    transfer = build_root_cache.transfer_products(
        root, iso, platform_name=args.platform, target="WoTEditor",
        configuration="Development",
        receipt_path=_transfer_receipt_path(
            iso, args.platform, "WoTEditor", "Development", phase),
        input_tree=sync["input_tree"])
    binaries = "Mac" if args.platform == "Mac" else "Win64"
    engine_id = build_root_cache.modules_build_id(
        engine_root / "Engine" / "Binaries" / binaries
        / "UnrealEditor.modules")
    iso_id = build_root_cache.modules_build_id(
        iso / "Binaries" / binaries / "UnrealEditor.modules")
    if engine_id is None or engine_id != iso_id:
        raise BuildFailure(
            "build-root editor modules do not match the engine BuildId "
            f"(engine={engine_id}, iso={iso_id}); the editor would refuse them")
    return {
        **_compile_cache_row(args, sync, transfer),
        "phase": phase,
        "command": command,
        "log": str(log_path),
        "log_sha256": _sha256_file(log_path),
        "build_id": engine_id,
    }


def _is_shipping_release(args) -> bool:
    return (_mode_is_guarded(args)
            and getattr(args, "configuration", None) == SHIPPING_CONFIGURATION)


def _guarded_receipt_paths(iso: Path, platform_name: str, target: str,
                           configuration: str = GUARDED_CONFIGURATION
                           ) -> tuple[Path, Path]:
    evidence = iso / "receipts"
    engine = evidence / f"{platform_name}-{target}-{configuration}-engine.json"
    return engine, engine.with_name(engine.stem + "-compiled.json")


def _guarded_build_command(args, iso: Path, engine_root: Path,
                           target: str, engine_receipt: Path,
                           configuration: str = GUARDED_CONFIGURATION,
                           build_root: Path | None = None) -> list[str]:
    # In the ISO, --clean/-Clean is load-bearing: the seed prunes packaged
    # target products from the ISO while its Intermediate may survive a
    # re-seed, and a copied (planted) binary carries a fresh CreationTime that
    # UBT reads as "just built" (ActionGraph.cs IsIndividualActionOutdated) --
    # the poisoned-artifact nondeterminism. The build root never receives a
    # copied output (guarded_build_root.py), so there the wrapper runs
    # -Incremental and UBT's dependency graph decides; the wrapper and its
    # receipt gate still come from the ISO's snapshot-bound Tools.
    project = (build_root or iso) / "WoT.uproject"
    if args.platform == "Mac":
        return [
            str(iso / "Tools" / "build" / "build_guarded_ue58.sh"),
            "--engine", str(engine_root),
            "--project", str(project),
            "--target", target,
            "--platform", "Mac",
            "--configuration", configuration,
            "--receipt", str(engine_receipt),
            "--clean" if build_root is None else "--incremental",
        ]
    if args.platform == "Win64":
        return [
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", str(iso / "Tools" / "build" / "build_guarded_ue58.ps1"),
            "-Engine", str(engine_root),
            "-Project", str(project),
            "-Target", target,
            "-Platform", "Win64",
            "-Configuration", configuration,
            "-Receipt", str(engine_receipt),
            "-Clean" if build_root is None else "-Incremental",
            "-SerialLocalBuild",
        ]
    raise BuildFailure(f"guarded builds do not support {args.platform}")


def _begin_build_root_ubt(root: Path, label: str) -> None:
    """Mark a build-root UBT run in flight, undoing one that died first.

    A UBT killed mid-link leaves a non-empty product newer than its inputs,
    which the next incremental run would call up to date (a truncated
    Test exe); guarded_build_root.begin_ubt deletes what that run wrote.
    """
    recovered = build_root_cache.begin_ubt(root, label)
    if recovered is not None:
        print(f"[build-root] previous UBT run {recovered.get('label')!r} did "
              "not exit cleanly; "
              + ("wiped UBT outputs" if recovered.get("wiped") else
                 f"deleted {recovered.get('deleted')} files it wrote"),
              flush=True)


def _run_guarded_target_build(args, iso: Path, engine_root: Path,
                              target: str,
                              configuration: str = GUARDED_CONFIGURATION
                              ) -> dict:
    engine_receipt, compiled_receipt = _guarded_receipt_paths(
        iso, args.platform, target, configuration)
    engine_receipt.parent.mkdir(parents=True, exist_ok=True)
    engine_receipt.unlink(missing_ok=True)
    compiled_receipt.unlink(missing_ok=True)
    build_root = _active_build_root(args, iso, engine_root)
    sync = (build_root_cache.mirror(iso, build_root)
            if build_root is not None else None)
    command = _guarded_build_command(
        args, iso, engine_root, target, engine_receipt, configuration,
        build_root)
    suffix = "" if configuration == GUARDED_CONFIGURATION else (
        "-" + configuration)
    if build_root is not None:
        _begin_build_root_ubt(build_root, f"{target}-{configuration}")
    succeeded = False
    try:
        _run_logged(
            f"guarded-{target}{suffix}", command,
            iso / "logs" / f"guarded-{target}{suffix}.log",
            env=_iso_tool_environment(args, engine_root))
        succeeded = True
    finally:
        if build_root is not None:
            build_root_cache.end_ubt(build_root, succeeded=succeeded)
    if not engine_receipt.is_file() or not compiled_receipt.is_file():
        raise BuildFailure(
            f"guarded {target} build did not publish both receipts")
    row = {
        "engine": str(engine_receipt),
        "compiled": str(compiled_receipt),
        "engine_sha256": _sha256_file(engine_receipt),
        "compiled_sha256": _sha256_file(compiled_receipt),
    }
    if build_root is not None:
        transfer = build_root_cache.transfer_products(
            build_root, iso, platform_name=args.platform, target=target,
            configuration=configuration,
            receipt_path=_transfer_receipt_path(
                iso, args.platform, target, configuration),
            input_tree=sync["input_tree"])
        row["compile_cache"] = _compile_cache_row(args, sync, transfer)
    return row


def _bind_transferred_products(product_receipt: dict, target_receipts: dict,
                               platform_name: str,
                               configuration: str) -> None:
    """Win64: every staged image IS the build-root product it was built as.

    UAT stages and _prepare_guarded_stage copies Win64 images unmodified
    (measured: both staged Shipping images hash-equal Binaries/Win64), so the
    receipted build and the staged bytes are joined by digest. Mac images are
    re-signed after staging and carry their own product receipt only.
    """
    if platform_name != "Win64":
        return
    for target in GUARDED_RECEIPT_TARGETS:
        row = (target_receipts.get(target) or {}).get("compile_cache")
        if row is None:
            continue
        path = Path(str(row.get("transfer_receipt", "")))
        transfer = _read_json_object(path, f"{target} transfer receipt")
        if row.get("transfer_receipt_sha256") != _sha256_file(path):
            raise BuildFailure(f"{target} transfer receipt changed")
        built = build_root_cache.product_digest(
            transfer, f"Binaries/Win64/{target}-Win64-{configuration}.exe")
        staged = (product_receipt.get("products", {}).get(target) or {}).get(
            "sha256")
        if built is None or built != staged:
            raise BuildFailure(
                f"staged {target} image is not the build-root product it was "
                "receipted as")


def _guarded_import_policy(args) -> str | None:
    if not _mode_is_guarded(args):
        return None
    policy = getattr(args, "guarded_import", None) or "shipping-producers"
    if policy not in GUARDED_IMPORT_CHOICES:
        raise BuildFailure(f"unknown --guarded-import {policy!r}")
    return policy


def _import_command(args, iso: Path, editor: Path) -> list[str]:
    script = iso / "Tools" / "import" / "import_all.py"
    policy = _guarded_import_policy(args)
    script_argument = " ".join((
        f"-script={_quote_script_value(script)}",
        "--payload-mode=stripped",
        f"--conv-dir={_quote_script_value(args.conv_dir.resolve())}",
        f"--exports-dir={_quote_script_value(args.exports_dir.resolve())}",
        f"--retail-root={_quote_script_value(args.retail_root.resolve())}",
        f"--mesh-source={args.mesh_source}",
        *(
            ("--only=" + ",".join(GUARDED_IMPORT_STEPS),)
            if policy == "shipping-producers" else ()
        ),
        *(
            ("--skip=" + ",".join(GUARDED_IMPORT_SKIP_STEPS),)
            if policy is not None else ()
        ),
    ))
    # The project rides as the FIRST POSITIONAL argument — the proven
    # commandlet invocation shape everywhere in this repo (-project= is UAT
    # syntax and pythonscript never saw the script when it was used here).
    return [
        str(editor), str(iso / "WoT.uproject"), "-run=pythonscript",
        script_argument, "-unattended", "-nullrhi", "-nosplash", "-stdout",
        # -nosteam: a RUNNING Steam client makes OnlineSubsystemSteam's init
        # terminate the unattended editor silently (exit 1, no log line) on
        # Windows; no commandlet here needs an online subsystem. Inert on Mac.
        "-nosteam",
        *_guarded_unreal_isolation_arguments(_mode_is_guarded(args)),
        *_editor_process_affinity_arguments(
            getattr(args, "editor_process_affinity", None)),
    ]


def _fresh_fractal_finalize_command(
        iso: Path, editor: Path,
        editor_process_affinity: int | None = None, *,
        guarded: bool = False) -> list[str]:
    script = (iso / "Tools" / "import" /
              "finalize_stripped_fractal_catalog.py")
    return [
        str(editor), str(iso / "WoT.uproject"), "-run=pythonscript",
        f"-script={_quote_script_value(script)}",
        "-unattended", "-nullrhi", "-nosplash", "-stdout", "-nosteam",
        *_guarded_unreal_isolation_arguments(guarded),
        *_editor_process_affinity_arguments(editor_process_affinity),
    ]


def _neutral_shell_generation_command(iso: Path,
                                       editor: Path,
                                       editor_process_affinity: int | None = None,
                                       *, guarded: bool = False
                                      ) -> list[str]:
    script = iso / "Tools" / "import" / "generate_neutral_map_shells.py"
    denied_roots = (
        iso / "Content",
        original_dir(required=False),
        devtree_dir(required=False),
        exports_dir(required=False),
    )
    script_arguments = [
        f"-script={_quote_script_value(script)}",
        "--spec=" + _quote_script_value(
            iso / "Tools/import/specs/neutral-map-shells-v1.json"),
        "--output-root=" + _quote_script_value(
            iso / NEUTRAL_SHELL_GENERATED_ROOT),
        "--denied-read-open-receipt="
        f"{_quote_script_value(iso / NEUTRAL_SHELL_DENIED_READ_RECEIPT)}",
        "--output-identity-receipt="
        f"{_quote_script_value(iso / NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT)}",
        *(f"--deny-read-root={_quote_script_value(root)}"
          for root in denied_roots),
        f"--require-disjoint-from={_quote_script_value(iso / 'Content')}",
        "--require-empty-output",
        "--no-replace",
    ]
    return [
        str(editor), str(iso / "WoT.uproject"), "-run=pythonscript",
        " ".join(script_arguments),
        "-unattended", "-nullrhi", "-nosplash", "-stdout", "-nosteam",
        *_guarded_unreal_isolation_arguments(guarded),
        *_editor_process_affinity_arguments(editor_process_affinity),
    ]


def _guarded_unreal_isolation_arguments(guarded: bool) -> list[str]:
    return (["-ddc=(ZenLocal)", ZEN_NO_AUTO_LAUNCH] if guarded else [])


def _validate_guarded_unreal_log(text: str, stage: str) -> None:
    normalized = text.replace("\\", "/").casefold()
    forbidden = ("/fixture/preexisting-ddc", "127.0.0.1:8558")
    found = [value for value in forbidden if value in normalized]
    if found:
        raise BuildFailure(
            f"guarded {stage} contacted preexisting DDC/Zen: "
            + ", ".join(found))


def _guarded_unreal_environment(
        source: dict[str, str] | None) -> tuple[dict[str, str], list[str]]:
    result = dict(os.environ if source is None else source)
    removed = sorted(
        key for key in result
        if key.casefold().startswith("ue-")
        and "datacache" in key.casefold())
    for key in removed:
        del result[key]
    return result, removed


def _iso_tool_environment(args, engine_root: Path | None = None,
                          source: dict[str, str] | None = None) -> dict[str, str]:
    """Build a closed machine-path environment for tools copied into the ISO."""
    result = dict(os.environ if source is None else source)
    controlled = {
        "WOT_CONVERSION_ROOT", "WOT_EXPORTS_DIR", "WOT_RETAIL_ROOT",
        "WOT_REFERENCE_ROOT", "WOT_SOURCE_ENGINE_ROOT_MAC",
        "WOT_SOURCE_ENGINE_ROOT_WIN64", "WOT_INSTALLED_ENGINE_ROOT_MAC",
        "WOT_INSTALLED_ENGINE_ROOT_WIN64", "WOT_VISUAL_STUDIO_ROOT_WIN64",
    }
    for key in controlled:
        result.pop(key, None)
    if getattr(args, "conv_dir", None) is not None:
        result["WOT_CONVERSION_ROOT"] = str(args.conv_dir.resolve())
    if getattr(args, "exports_dir", None) is not None:
        result["WOT_EXPORTS_DIR"] = str(args.exports_dir.resolve())
    if getattr(args, "retail_root", None) is not None:
        retail = args.retail_root.resolve()
        result["WOT_RETAIL_ROOT"] = str(retail)
        result["WOT_REFERENCE_ROOT"] = str(retail.parent)
    platform_name = getattr(args, "platform", None)
    if engine_root is not None and platform_name in {"Mac", "Win64"}:
        kind = "SOURCE" if _mode_is_guarded(args) else "INSTALLED"
        result[f"WOT_{kind}_ENGINE_ROOT_{platform_name.upper()}"] = \
            str(engine_root.resolve())
    for key in (
            "installed_engine_root_mac", "installed_engine_root_win64",
            "visual_studio_root_win64"):
        try:
            value = local_path(key, required=False)
        except ConfigurationError as exc:
            raise BuildFailure(str(exc)) from exc
        if value is not None:
            result[f"WOT_{key.upper()}"] = str(value)
    return result


def _run_logged(step: str, command: Sequence[str], log_path: Path, *,
                env: dict[str, str] | None = None, check: bool = True) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    command = list(command)
    # An UnrealEditor commandlet must own its log file via -abslog, with its
    # stdout DISCARDED: when the Steam client is running on Windows, the
    # moment SteamShared loads the Steam SDK the redirected stdout stream is
    # clobbered mid-run (capture freezes at the SDK-load line while the
    # process runs to completion), so a success marker grepped from captured
    # stdout can never be trusted. The -abslog file provably keeps writing
    # through SDK load (bisected 2026-08-04), and with -stdout also present
    # it contains the same content the capture used to. Non-editor commands
    # keep the plain stdout capture.
    editor_owns_log = (Path(command[0]).name.lower().startswith(
        "unrealeditor-cmd") if command else False)
    guarded_unreal_kind = None
    if _ACTIVE_GUARDED_ZEN and (editor_owns_log or step == "cook"):
        guarded_unreal_kind = _validate_guarded_unreal_command(
            command, "editor" if editor_owns_log else "cook")
        env, stripped_ddc_overrides = _guarded_unreal_environment(env)
    else:
        stripped_ddc_overrides = []
    print(f"[{step}] RUN {subprocess.list2cmdline(command)}", flush=True)
    if editor_owns_log:
        result = subprocess.run(
            command + [f"-abslog={log_path}"], cwd=str(REPO), env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        returncode = result.returncode
    else:
        with log_path.open(
                "w", encoding="utf-8", errors="backslashreplace") as log:
            result = subprocess.run(
                command, cwd=str(REPO), env=env, stdout=log,
                stderr=subprocess.STDOUT, text=True, check=False)
        returncode = result.returncode
    if guarded_unreal_kind is not None:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        _validate_guarded_unreal_log(log_text, step)
        _ACTIVE_GUARDED_ZEN_COMMANDS.append({
            "stage": step,
            "kind": guarded_unreal_kind,
            "command": command,
            "log": str(log_path),
            "log_sha256": _sha256_file(log_path),
            "exit_code": returncode,
            "stripped_ddc_environment_overrides": stripped_ddc_overrides,
        })
    if check and returncode:
        raise BuildFailure(
            f"{step} command exited {returncode}; see {log_path}")
    return returncode


def _import_cache_identity(args) -> dict:
    return {
        "payload_mode": "stripped",
        "mesh_source": args.mesh_source,
        "exports_dir": str(args.exports_dir.resolve()),
        "conv_dir": str(args.conv_dir.resolve()),
        "retail_root": str(args.retail_root.resolve()),
        "editor_process_affinity": getattr(
            args, "editor_process_affinity", None),
        "guarded_mesh_carrier_policy": (
            "skip-meshes-and-pickups-v1" if _mode_is_guarded(args)
            else "development-import-v1"),
        **({"guarded_import_steps": (
                list(GUARDED_IMPORT_STEPS)
                if _guarded_import_policy(args) == "shipping-producers"
                else ["all-except:" + ",".join(GUARDED_IMPORT_SKIP_STEPS)]),
            "import_editor_source": (
                "build-root-v1" if _compile_cache_enabled(args)
                else "iso-built-mac-v1" if args.platform == "Mac"
                else "seeded-repo-binaries-v1")}
           if _mode_is_guarded(args) else {}),
    }


def _iso_mac_editor(args, iso: Path, engine_root: Path, phase: str) -> dict:
    """Build WoTEditor against the ISO project itself (guarded Mac, no build root).

    UBT links Mac editor modules with @loader_path-relative rpaths to every
    engine plugin binary directory, so a module only resolves its plugin
    dependencies from the depth it was built at. The seed copies the repo's
    modules into <repo>/.stripped-pak, one level deeper, where those rpaths
    miss; it worked only while every plugin dependency happened to be loaded
    before the game module (dyld reuses loaded images). Once the module linked
    OnlineSubsystemEOS, which no editor commandlet preloads, the import
    commandlet refused the module ("The game module 'WoT' could not be
    loaded"). The cook already rebuilds this editor in the ISO (UAT -build), so
    building it here first costs nothing overall and fixes every commandlet
    that runs before the cook. Win64 resolves DLLs through search
    directories, not rpaths, and keeps the seeded binaries.
    """
    script = engine_root / "Engine" / "Build" / "BatchFiles" / "Mac" / "Build.sh"
    command = [str(script), "WoTEditor", "Mac", "Development",
               f"-project={iso / 'WoT.uproject'}", "-NoHotReload"]
    log_path = iso / "logs" / f"iso-editor-{phase}.log"
    _run_logged(f"iso-editor-{phase}", command, log_path,
                env=_iso_tool_environment(args, engine_root))
    engine_id = build_root_cache.modules_build_id(
        engine_root / "Engine" / "Binaries" / "Mac" / "UnrealEditor.modules")
    iso_id = build_root_cache.modules_build_id(
        iso / "Binaries" / "Mac" / "UnrealEditor.modules")
    if engine_id is None or engine_id != iso_id:
        raise BuildFailure(
            "ISO-built editor modules do not match the engine BuildId "
            f"(engine={engine_id}, iso={iso_id}); the editor would refuse them")
    return {"format": "wot-iso-editor-v1", "phase": phase,
            "log": str(log_path), "build_id": iso_id}


def _guarded_import_editor(args, iso: Path, engine_root: Path,
                           phase: str) -> dict | None:
    if _compile_cache_enabled(args):
        return _build_root_editor(args, iso, engine_root, phase)
    if _mode_is_guarded(args) and args.platform == "Mac":
        return _iso_mac_editor(args, iso, engine_root, phase)
    return None


def stage_import(args, iso: Path) -> str:
    neutral_support_authority = None
    if _mode_is_guarded(args):
        neutral_support_authority = _neutral_support_lifecycle_receipt(
            iso, require_import=False)
    stamp = _read_stamp(iso, "import")
    log_path = iso / "logs" / "import.log"
    expected = _import_cache_identity(args)
    editor_affinity = getattr(args, "editor_process_affinity", None)
    base_valid = bool(
        stamp and stamp.get("success_marker") in IMPORT_SUCCESS_MARKERS
        and type(stamp.get("command_exit_code")) is int
        and stamp.get("command_exit_code") == 0
        and all(stamp.get(key) == value for key, value in expected.items())
        and (not _mode_is_guarded(args)
             or stamp.get("neutral_support_authority")
                == neutral_support_authority)
        and log_path.is_file())
    fractal_log = iso / "logs" / "finalize-stripped-fractal-catalog.log"
    fractal_receipt = iso / FRACTAL_FINALIZE_RECEIPT
    if base_valid and not (_mode_is_guarded(args) and _ACTIVE_GUARDED_ZEN):
        if not _mode_is_guarded(args):
            return "cached"
        for cached_log, cached_stage in (
                (log_path, "import"),
                (fractal_log, "finalize"),
                (iso / "logs" / "generate-neutral-map-shells.log",
                 "neutral-shell generation")):
            if cached_log.is_file():
                _validate_guarded_unreal_log(
                    cached_log.read_text(
                        encoding="utf-8", errors="replace"),
                    cached_stage)
        deprecated_finalize_keys = any(
            key.startswith(("fresh_bsp_finalize_", "fresh_finalize_"))
            for key in stamp)
        fractal_valid = bool(
            stamp.get("fresh_fractal_finalize_success_marker")
            == PYTHON_COMMANDLET_SUCCESS
            and stamp.get("fresh_fractal_finalize_script_marker")
            == FRACTAL_FINALIZE_SUCCESS
            and fractal_log.is_file() and fractal_receipt.is_file()
            and stamp.get("fresh_fractal_finalize_receipt_sha256")
            == _sha256_file(fractal_receipt))
        neutral_shells_valid = _neutral_shell_stamp_valid(iso, stamp)
        if (fractal_valid and neutral_shells_valid
                and not deprecated_finalize_keys):
            return "cached"
        _invalidate_from(iso, "cook")
        engine_root, editor = _engine_root_and_editor(args.engine)
        details = {
            key: value for key, value in stamp.items()
            if key not in {"step", "completed_utc"}
            and not key.startswith(
                ("fresh_bsp_finalize_", "fresh_finalize_"))
        }
        import_editor = _guarded_import_editor(
            args, iso, engine_root, "import")
        if import_editor is not None:
            details["import_editor"] = import_editor
        if not fractal_valid:
            details = {
                key: value for key, value in details.items()
                if not key.startswith("fresh_fractal_finalize_")
            }
            details.update(
                _run_fresh_fractal_finalize(
                    iso, editor, fractal_log, editor_affinity,
                    env=_iso_tool_environment(args, engine_root)))
        if not neutral_shells_valid:
            details = {
                key: value for key, value in details.items()
                if not key.startswith("neutral_shells_")
            }
            details.update(_run_neutral_shell_generation(
                iso, editor,
                iso / "logs" / "generate-neutral-map-shells.log",
                editor_affinity,
                env=_iso_tool_environment(args, engine_root)))
        details["engine"] = str(engine_root)
        details["neutral_support_authority"] = \
            _neutral_support_lifecycle_receipt(
                iso, require_import=False)
        _write_stamp(iso, "import", details)
        return "ran"
    _invalidate_from(iso, "import")
    if _mode_is_guarded(args):
        _prepare_import_generated_outputs(iso)
    engine_root, editor = _engine_root_and_editor(args.engine)
    import_editor = _guarded_import_editor(args, iso, engine_root, "import")
    command = _import_command(args, iso, editor)
    env = _iso_tool_environment(args, engine_root)
    # Strip the retail-sourced fractal seed from DA_WoTFractalCatalog.
    # This orchestrator only ever builds STRIPPED paks, so the
    # flag is unconditional here; the runtime WoTFractalSubsystem rehydrates the
    # emptied wet/ice seeds from the payload cache by source_texture. Fire/wave
    # keep their baker-generated seed (not a leak). See PORT_NOTES "Built from the player's own copy of the game".
    env["WOT_STRIP_FRACTAL_SEED"] = "1"
    # Both signals are load-bearing: import_all publishes the marker only after
    # all items succeed, while guarded publication also requires the commandlet
    # itself to exit zero so an engine-level import error cannot be waived.
    exit_code = _run_logged("import", command, log_path, env=env, check=False)
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    if _mode_is_guarded(args):
        _validate_guarded_unreal_log(log_text, "import")
        if exit_code != 0:
            raise BuildFailure(
                f"import command exited {exit_code}; see {log_path}")
    success_marker = next(
        (marker for marker in IMPORT_SUCCESS_MARKERS if marker in log_text), None)
    if success_marker is None:
        raise BuildFailure(
            f"import command exited {exit_code} without a commandlet success "
            f"marker; see {log_path}")
    finalize = {}
    neutral_shells = {}
    if _mode_is_guarded(args):
        finalize = _run_fresh_fractal_finalize(
            iso, editor, fractal_log, editor_affinity, env=env)
        neutral_shells = _run_neutral_shell_generation(
            iso, editor,
            iso / "logs" / "generate-neutral-map-shells.log",
            editor_affinity, env=env)
        neutral_support_authority = _neutral_support_lifecycle_receipt(
            iso, require_import=False)
    _write_stamp(iso, "import", {
        "command_exit_code": exit_code,
        "success_marker": success_marker,
        "log": str(log_path),
        "engine": str(engine_root),
        "project": str(iso / "WoT.uproject"),
        **finalize,
        **neutral_shells,
        **({"neutral_support_authority": neutral_support_authority}
           if neutral_support_authority is not None else {}),
        **({"import_editor": import_editor}
           if import_editor is not None else {}),
        **expected,
    })
    return "ran"


def _run_fresh_fractal_finalize(iso: Path, editor: Path,
                                log_path: Path,
                                editor_process_affinity: int | None = None,
                                *, env: dict[str, str] | None = None
                                ) -> dict:
    receipt_path = iso / FRACTAL_FINALIZE_RECEIPT
    receipt_path.unlink(missing_ok=True)
    exit_code = _run_logged(
        "finalize-stripped-fractal-catalog",
        _fresh_fractal_finalize_command(
            iso, editor, editor_process_affinity, guarded=True), log_path,
        env=env, check=False)
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    _validate_guarded_unreal_log(log_text, "finalize")
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BuildFailure(
            f"fresh fractal catalog finalizer did not publish its receipt: "
            f"{exc}; see {log_path}") from exc
    receipt_valid = (
        isinstance(receipt, dict)
        and receipt.get("format") == "wot-stripped-fractal-finalize-v1"
        and receipt.get("catalog") == "/Game/WoT/Data/DA_WoTFractalCatalog"
        and receipt.get("verdict") == "pass"
        and isinstance(receipt.get("retail_source_records"), int)
        and receipt["retail_source_records"] > 0
        and isinstance(receipt.get("seed_bytes_removed"), int)
        and receipt["seed_bytes_removed"] >= 0)
    if PYTHON_COMMANDLET_SUCCESS not in log_text or not receipt_valid:
        raise BuildFailure(
            f"fresh fractal catalog finalizer exited {exit_code} without both "
            f"success proofs; see {log_path}")
    return {
        "fresh_fractal_finalize_command_exit_code": exit_code,
        "fresh_fractal_finalize_success_marker": PYTHON_COMMANDLET_SUCCESS,
        "fresh_fractal_finalize_script_marker": FRACTAL_FINALIZE_SUCCESS,
        "fresh_fractal_finalize_log": str(log_path),
        "fresh_fractal_finalize_receipt": str(receipt_path),
        "fresh_fractal_finalize_receipt_sha256": _sha256_file(receipt_path),
    }


def _neutral_shell_plugin_descriptor_bytes() -> bytes:
    descriptor = {
        "FileVersion": 3,
        "FriendlyName": "WoT Neutral Shells",
        "Version": 1,
        "VersionName": "1.0",
        "CanContainContent": True,
        "EnabledByDefault": True,
    }
    return (json.dumps(descriptor, indent=2, sort_keys=True)
            + "\n").encode("utf-8")


def _read_neutral_shell_identity_receipt(iso: Path) -> tuple[Path, list[dict]]:
    receipt_path = iso / NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT
    receipt = _read_json_object(
        receipt_path, "neutral-shell output-identity receipt")
    generated_root = iso / NEUTRAL_SHELL_GENERATED_ROOT
    rows = receipt.get("packages")
    if (receipt.get("format")
            != "neutral-map-shells-output-identity-receipt-v2"
            or receipt.get("root_identity") != str(generated_root.resolve())
            or receipt.get("root_began_absent") is not True
            or receipt.get("package_count") != len(NEUTRAL_SHELL_NAMES)
            or not isinstance(rows, list)
            or len(rows) != len(NEUTRAL_SHELL_NAMES)):
        raise BuildFailure(
            "neutral-shell output-identity receipt has the wrong census identity")
    census = hashlib.sha256()
    # zip(strict=) is 3.10+, and this script runs on the Mac host's 3.9 system
    # interpreter -- the count equality asserted above already pins the pairing.
    for slot, (name, row) in enumerate(
            zip(NEUTRAL_SHELL_NAMES, rows), 1):
        expected_path = f"{name}.umap"
        if (not isinstance(row, dict)
                or set(row) != {
                    "bytes", "package", "relative_path", "sha256", "slot"}
                or row.get("slot") != slot
                or row.get("package") != f"/WoTNeutralShells/{name}"
                or row.get("relative_path") != expected_path
                or not isinstance(row.get("bytes"), int)
                or row["bytes"] <= 0
                or not isinstance(row.get("sha256"), str)
                or re.fullmatch(r"[0-9a-f]{64}", row["sha256"]) is None):
            raise BuildFailure(
                f"neutral-shell receipt package row {slot} is malformed")
        census.update(json.dumps(
            row, sort_keys=True, separators=(",", ":"),
            ensure_ascii=True).encode("ascii"))
    if receipt.get("package_census_sha256") != census.hexdigest():
        raise BuildFailure(
            "neutral-shell receipt package census digest mismatched")
    return receipt_path, rows


def _verify_neutral_shell_plugin_census(iso: Path) -> dict:
    receipt_path, rows = _read_neutral_shell_identity_receipt(iso)
    content = iso / NEUTRAL_SHELL_PLUGIN_ROOT / "Content"
    expected_names = [f"{name}.umap" for name in NEUTRAL_SHELL_NAMES]
    if not content.is_dir():
        raise BuildFailure("neutral-shell plugin Content directory is missing")
    entries = sorted(content.iterdir(), key=lambda path: path.name)
    actual_names = [path.name for path in entries]
    if (actual_names != expected_names
            or any(not path.is_file() for path in entries)):
        missing = sorted(set(expected_names) - set(actual_names))
        extra = sorted(set(actual_names) - set(expected_names))
        raise BuildFailure(
            "neutral-shell plugin census mismatch; "
            f"missing={missing}, unexpected={extra}")
    # Names were just proven equal to the exact expected census, so entries and
    # rows are the same length; zip(strict=) is 3.10+ and the host runs 3.9.
    for path, row in zip(entries, rows):
        if (path.stat().st_size != row["bytes"]
                or _sha256_file(path) != row["sha256"]):
            raise BuildFailure(
                f"neutral-shell package changed after generation: {path.name}")
    descriptor = (iso / NEUTRAL_SHELL_PLUGIN_ROOT /
                  "WoTNeutralShells.uplugin")
    if (not descriptor.is_file()
            or descriptor.read_bytes()
            != _neutral_shell_plugin_descriptor_bytes()):
        raise BuildFailure("neutral-shell plugin descriptor changed or is missing")
    return {
        "package_count": len(rows),
        "output_identity_receipt": str(receipt_path),
        "plugin": str(iso / NEUTRAL_SHELL_PLUGIN_ROOT),
        "plugin_descriptor_sha256": _sha256_file(descriptor),
    }


def _synthesize_neutral_shell_plugin(iso: Path) -> dict:
    _receipt_path, _rows = _read_neutral_shell_identity_receipt(iso)
    generated_root = iso / NEUTRAL_SHELL_GENERATED_ROOT
    if not generated_root.is_dir():
        raise BuildFailure("neutral-shell generated output root is missing")
    plugin = iso / NEUTRAL_SHELL_PLUGIN_ROOT
    try:
        plugin.mkdir(parents=True, exist_ok=False)
        descriptor = plugin / "WoTNeutralShells.uplugin"
        descriptor.write_bytes(_neutral_shell_plugin_descriptor_bytes())
        os.rename(generated_root, plugin / "Content")
    except OSError as exc:
        raise BuildFailure(
            f"cannot synthesize neutral-shell plugin: {exc}") from exc
    _enable_neutral_shell_plugin_for_isolated_project(iso)
    return _verify_neutral_shell_plugin_census(iso)


def _enable_neutral_shell_plugin_for_isolated_project(iso: Path) -> None:
    """Make the generated content plugin an explicit project dependency."""
    project = iso / "WoT.uproject"
    document = _read_json_object(project, "isolated project descriptor")
    plugins = document.get("Plugins")
    if plugins is None:
        plugins = []
        document["Plugins"] = plugins
    if not isinstance(plugins, list):
        raise BuildFailure(
            "isolated project descriptor Plugins field is not an array")
    matching = [
        entry for entry in plugins
        if isinstance(entry, dict) and entry.get("Name") == "WoTNeutralShells"
    ]
    if len(matching) > 1:
        raise BuildFailure(
            "isolated project descriptor contains duplicate WoTNeutralShells "
            "plugin references")
    if matching:
        matching[0]["Enabled"] = True
    else:
        plugins.append({"Name": "WoTNeutralShells", "Enabled": True})
    temporary = project.with_name(project.name + ".tmp")
    try:
        with temporary.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(
                document, indent=2, sort_keys=True, ensure_ascii=True) + "\n")
        temporary.replace(project)
    except OSError as exc:
        raise BuildFailure(
            f"cannot enable neutral-shell plugin in {project}: {exc}") from exc


def _run_neutral_shell_generation(iso: Path, editor: Path,
                                  log_path: Path,
                                  editor_process_affinity: int | None = None,
                                  *, env: dict[str, str] | None = None
                                  ) -> dict:
    _prepare_neutral_shell_outputs(iso)
    exit_code = _run_logged(
        "generate-neutral-map-shells",
        _neutral_shell_generation_command(
            iso, editor, editor_process_affinity, guarded=True), log_path,
        env=env, check=False)
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    _validate_guarded_unreal_log(log_text, "neutral-shell generation")
    if PYTHON_COMMANDLET_SUCCESS not in log_text:
        raise BuildFailure(
            f"neutral-shell generator exited {exit_code} without a commandlet "
            f"success marker; see {log_path}")
    denied_path = iso / NEUTRAL_SHELL_DENIED_READ_RECEIPT
    denied = _read_json_object(denied_path, "neutral-shell denied-read receipt")
    if (denied.get("format")
            != "neutral-map-shells-denied-read-open-receipt-v2"
            or denied.get("violations") != []):
        raise BuildFailure("neutral-shell denied-read receipt is not a pass")
    plugin = _synthesize_neutral_shell_plugin(iso)
    identity_path = iso / NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT
    return {
        "neutral_shells_command_exit_code": exit_code,
        "neutral_shells_success_marker": PYTHON_COMMANDLET_SUCCESS,
        "neutral_shells_log": str(log_path),
        "neutral_shells_denied_read_open_receipt": str(denied_path),
        "neutral_shells_denied_read_open_receipt_sha256":
            _sha256_file(denied_path),
        "neutral_shells_output_identity_receipt": str(identity_path),
        "neutral_shells_output_identity_receipt_sha256":
            _sha256_file(identity_path),
        "neutral_shells_plugin": plugin["plugin"],
        "neutral_shells_plugin_descriptor_sha256":
            plugin["plugin_descriptor_sha256"],
        "neutral_shells_package_count": plugin["package_count"],
    }


def _neutral_shell_stamp_valid(iso: Path, stamp: dict) -> bool:
    denied_path = iso / NEUTRAL_SHELL_DENIED_READ_RECEIPT
    identity_path = iso / NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT
    log_path = iso / "logs" / "generate-neutral-map-shells.log"
    try:
        denied = _read_json_object(
            denied_path, "neutral-shell denied-read receipt")
        plugin = _verify_neutral_shell_plugin_census(iso)
        return bool(
            stamp.get("neutral_shells_success_marker")
            == PYTHON_COMMANDLET_SUCCESS
            and stamp.get("neutral_shells_log") == str(log_path)
            # The LOG is the authority, not the marker copied into the stamp:
            # an emptied or replaced log must re-run the leg, not report cached.
            and PYTHON_COMMANDLET_SUCCESS in log_path.read_text(
                encoding="utf-8", errors="replace")
            and stamp.get("neutral_shells_denied_read_open_receipt")
            == str(denied_path)
            and stamp.get("neutral_shells_denied_read_open_receipt_sha256")
            == _sha256_file(denied_path)
            and stamp.get("neutral_shells_output_identity_receipt")
            == str(identity_path)
            and stamp.get("neutral_shells_output_identity_receipt_sha256")
            == _sha256_file(identity_path)
            and stamp.get("neutral_shells_plugin") == plugin["plugin"]
            and stamp.get("neutral_shells_plugin_descriptor_sha256")
            == plugin["plugin_descriptor_sha256"]
            and stamp.get("neutral_shells_package_count")
            == len(NEUTRAL_SHELL_NAMES)
            and denied.get("format")
            == "neutral-map-shells-denied-read-open-receipt-v2"
            and denied.get("violations") == [])
    except (BuildFailure, OSError):
        return False


def _neutral_shell_real_path(iso: Path, relative: Path) -> Path:
    """Reject a symlink anywhere between the iso root and a cleanup target.

    Checking only the leaf is not enough: a symlinked ancestor (iso/Plugins ->
    /external) leaves the leaf looking like an ordinary directory, and the
    rmtree below would then delete the external tree instead of iso content.
    """
    path = iso / relative
    current = iso
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            raise BuildFailure(
                f"neutral-shell cleanup path crosses a symlink: {current}")
    return path


def _prepare_neutral_shell_outputs(iso: Path) -> None:
    for relative in (
            NEUTRAL_SHELL_GENERATED_ROOT, NEUTRAL_SHELL_PLUGIN_ROOT,
            # The generator authors its temporary maps under
            # Content/WoTNeutralShellStaging and hard-refuses a pre-existing
            # staging shell or a non-empty staging root, so a process killed
            # mid-generation would otherwise make EVERY retry fail forever.
            NEUTRAL_SHELL_STAGING_ROOT):
        path = _neutral_shell_real_path(iso, relative)
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink(missing_ok=True)
    for relative in (
            NEUTRAL_SHELL_DENIED_READ_RECEIPT,
            NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT):
        path = _neutral_shell_real_path(iso, relative)
        if path.is_dir():
            raise BuildFailure(
                f"neutral-shell receipt slot is unexpectedly a directory: {path}")
        path.unlink(missing_ok=True)


def _prepare_import_generated_outputs(iso: Path) -> None:
    """Restore import_all's exact no-replace output slots after a failed run."""
    for relative in IMPORT_GENERATED_OUTPUTS:
        path = iso / relative
        if path.is_dir():
            raise BuildFailure(
                f"generated import output slot is unexpectedly a directory: {path}")
        path.unlink(missing_ok=True)
    _prepare_neutral_shell_outputs(iso)


def _marker_config_valid(config: Path) -> bool:
    if not config.is_file():
        return False
    text = config.read_text(encoding="utf-8")
    if PAYLOAD_MARKER_BLOCK not in text:
        return False
    section = ""
    console_value = None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].casefold()
        elif (section == "consolevariables"
              and stripped.partition("=")[0].strip().casefold()
              == "wot.payloadinjection"):
            console_value = stripped.partition("=")[2].strip()
    return console_value == "1"


def stage_marker(iso: Path) -> str:
    config = iso / "Config" / "DefaultEngine.ini"
    stamp = _read_stamp(iso, "marker")
    if stamp and _marker_config_valid(config):
        return "cached"
    _invalidate_from(iso, "marker")
    if not config.is_file():
        raise BuildFailure(f"isolated engine config is missing: {config}")
    text = config.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    section = ""
    changed_console = 0
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].casefold()
            continue
        if (section == "consolevariables"
                and stripped.partition("=")[0].strip().casefold()
                == "wot.payloadinjection"):
            newline = "\r\n" if line.endswith("\r\n") else "\n"
            lines[index] = f"wot.PayloadInjection=1{newline}"
            changed_console += 1
    if changed_console != 1:
        raise BuildFailure(
            "expected exactly one existing [ConsoleVariables] "
            "wot.PayloadInjection assignment in isolated DefaultEngine.ini")
    updated = "".join(lines)
    if PAYLOAD_MARKER_BLOCK not in updated:
        if updated and not updated.endswith("\n"):
            updated += "\n"
        updated += "\n" + PAYLOAD_MARKER_BLOCK
    temporary = config.with_name(config.name + ".tmp")
    temporary.write_text(updated, encoding="utf-8")
    temporary.replace(config)
    if not _marker_config_valid(config):
        raise BuildFailure("payload-required ini marker did not converge")
    _write_stamp(iso, "marker", {
        "config": str(config),
        "system_settings": "wot.PayloadInjection=1",
        "console_variables_reconciled": "wot.PayloadInjection=1",
        "cvar_registration": "ECVF_Default",
    })
    return "ran"


def _cook_maps(guarded: bool = False) -> tuple[str, ...]:
    if guarded:
        # A release cooks only payload-free consumers: the bootstrap shell,
        # code-driven front-end shell, and the exact nineteen generated
        # multiplayer shells. Retail-derived imported worlds remain developer
        # oracles and must not enter the staged container graph.
        return (BOOTSTRAP_MAP, FRONT_END_MAP, *NEUTRAL_SHELL_MAPS)
    return tuple(f"/Game/WoT/Maps/{name}" for name in MP_MAPS) + (
        FRONT_END_MAP,)


def _is_engine_plugin_descriptor(path: str) -> bool:
    """An engine plugin's .uplugin as UnrealPak lists it (mount-relative)."""
    relative = path.removeprefix("../../../")
    parts = relative.split("/")
    return (len(parts) >= 4 and parts[0] == "Engine" and parts[1] == "Plugins"
            and ".." not in parts and "" not in parts
            and parts[-1].casefold().endswith(".uplugin"))


def _validate_guarded_cooked_paths(paths: Sequence[str]) -> dict:
    """Require the exact 21 neutral maps and closed neutral support set."""
    shell_prefix = "../../../WoT/Plugins/WoTNeutralShells/Content/"
    project_prefixes = ("../../../wot/content/", "../../../wot/plugins/")
    neutral_map_identities = {
        "../../../WoT/Content/WoT/Maps/WoTBootstrap",
        "../../../WoT/Content/WoT/Maps/WoTFrontEnd",
        *{
            shell_prefix + name for name in NEUTRAL_SHELL_NAMES
        },
    }
    neutral_support_identities = {
        "../../../WoT/Content" + package.removeprefix("/Game")
        for package in NEUTRAL_SUPPORT_PACKAGES
    }
    allowed_identities = neutral_map_identities | neutral_support_identities
    package_extensions = {".uasset", ".umap", ".uexp", ".ubulk", ".uptnl"}
    normalized_paths = [path.replace("\\", "/") for path in paths]
    plugin_descriptor = (
        "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin")
    descriptor_paths = [
        path for path in normalized_paths
        if path.casefold().endswith(".uplugin")
    ]
    if len(descriptor_paths) != len(set(descriptor_paths)):
        raise BuildFailure(
            "guarded cooked inventory contains a duplicate plugin descriptor")
    # Mac UAT also stages every enabled engine plugin's descriptor (187 of
    # them, already present in the 356ec0cf Mac release before this rule);
    # Win64 stages none. Engine plugin metadata carries no project or retail
    # content, so it is allowed. Every other descriptor must be the
    # neutral-shell one, exactly once.
    engine_descriptors = [
        path for path in descriptor_paths if _is_engine_plugin_descriptor(path)]
    project_descriptors = [
        path for path in descriptor_paths if path not in engine_descriptors]
    if project_descriptors != [plugin_descriptor]:
        raise BuildFailure(
            "guarded cooked inventory must stage exactly the neutral-shell "
            f"plugin descriptor at {plugin_descriptor}; found "
            f"{project_descriptors}")
    raw_project_paths = [
        path for path in normalized_paths
        if path.casefold().startswith(project_prefixes)
        and Path(path).suffix.casefold() in package_extensions
    ]
    if len(raw_project_paths) != len(set(raw_project_paths)):
        raise BuildFailure(
            "guarded cooked inventory contains a duplicate project path")
    project_paths = sorted(set(raw_project_paths))
    identities = set()
    primary_maps = set()
    primary_support = set()
    denied = []
    for path in project_paths:
        suffix = Path(path).suffix.casefold()
        identity = path[:-len(suffix)] if suffix in package_extensions else path
        identities.add(identity)
        if identity not in allowed_identities:
            denied.append(path)
        elif suffix == ".umap" and identity in neutral_map_identities:
            primary_maps.add(identity)
        elif suffix == ".uasset" and identity in neutral_support_identities:
            primary_support.add(identity)
    if denied:
        rendered = ", ".join(denied[:8])
        if len(denied) > 8:
            rendered += f" (+{len(denied) - 8} more)"
        raise BuildFailure(
            "guarded cooked inventory contains retail-derived or unexpected "
            f"project content: {rendered}")
    missing_maps = sorted(neutral_map_identities - primary_maps)
    if missing_maps:
        raise BuildFailure(
            "guarded cooked inventory is missing neutral map packages: "
            + ", ".join(missing_maps))
    missing_support = sorted(neutral_support_identities - primary_support)
    if missing_support:
        raise BuildFailure(
            "guarded cooked inventory is missing neutral support packages: "
            + ", ".join(missing_support))
    return {
        "project_content_count": len(project_paths),
        "retail_derived_count": 0,
        "neutral_map_count": len(primary_maps),
        "neutral_support_package_count": len(primary_support),
        "neutral_shell_plugin_descriptor_count": 1,
        "engine_plugin_descriptor_count": len(engine_descriptors),
    }


# Every package a package can load -- hard imports, soft object paths and
# editor-only references alike -- is spelled as a full package-path FName or
# string in the package (name table, soft paths, .uexp string data). A
# reference to X_7 may be stored as the FName "X" with number 8, which the
# number-suffix pass below covers.
_PACKAGE_NAME_PATTERN = re.compile(
    rb"/(?:Game|WoTNeutralShells)/[A-Za-z0-9_\-/.]+")
_PACKAGE_EXTENSIONS = (".uasset", ".umap")
_PACKAGE_SIDECARS = (".uexp", ".ubulk", ".uptnl")


def _guarded_package_mounts(iso: Path) -> tuple[tuple[str, Path], ...]:
    return (("/Game", iso / "Content"),
            ("/WoTNeutralShells", iso / NEUTRAL_SHELL_PLUGIN_ROOT / "Content"))


def _guarded_package_index(iso: Path) -> dict[str, tuple[str, Path]]:
    """casefold(package) -> (package, file) for every ISO project package."""
    index = {}
    for mount, base in _guarded_package_mounts(iso):
        if not base.is_dir():
            continue
        for current, _directories, files in os.walk(base):
            for name in files:
                path = Path(current) / name
                if path.suffix.casefold() not in _PACKAGE_EXTENSIONS:
                    continue
                package = (mount + "/"
                           + path.relative_to(base).with_suffix("").as_posix())
                index[package.casefold()] = (package, path)
    return index


def _referenced_package_names(path: Path) -> set[str]:
    data = path.read_bytes()
    for sidecar in _PACKAGE_SIDECARS:
        extra = path.with_suffix(sidecar)
        if extra.is_file():
            data += extra.read_bytes()
    names = set()
    for match in _PACKAGE_NAME_PATTERN.finditer(data):
        name = match.group(0).decode("ascii").split(".", 1)[0].rstrip("/")
        if name:
            names.add(name)
    return names


def _verify_guarded_reference_closure(iso: Path) -> dict:
    """Prove the shipped roots can reach no project package but themselves.

    The cooked-path allowlist proves what was COOKED; this proves what the
    cook could even LOAD from the project while cooking it. Starting from the
    cooked maps and support roots, every referenced project package name that
    exists in the ISO must itself be an allowlisted shipped package. Names that
    resolve to no package (the shell generator's removed staging root, a bare
    directory) cannot be loaded. Hence nothing the guarded import leaves
    un-imported (GUARDED_IMPORT_STEPS) can influence a cooked byte.
    """
    allowed = {package.casefold(): package for package in (
        BOOTSTRAP_MAP, FRONT_END_MAP, *NEUTRAL_SHELL_MAPS,
        *NEUTRAL_SUPPORT_PACKAGES)}
    index = _guarded_package_index(iso)
    closure: dict[str, dict] = {}
    unresolved: set[str] = set()
    denied: dict[str, set[str]] = {}
    stack = [BOOTSTRAP_MAP, FRONT_END_MAP, *NEUTRAL_SHELL_MAPS,
             *NEUTRAL_SUPPORT_ROOT_PACKAGES]
    while stack:
        package = stack.pop()
        if package in closure:
            continue
        row = index.get(package.casefold())
        if row is None:
            raise BuildFailure(
                f"guarded shipped package is missing from the ISO: {package}")
        path = row[1]
        closure[package] = {
            "path": path.relative_to(iso).as_posix(),
            "sha256": _sha256_file(path),
        }
        for name in sorted(_referenced_package_names(path)):
            folded = name.casefold()
            if folded in index:
                hits = [index[folded][0]]
            else:
                prefix = folded + "_"
                hits = [canonical for key, (canonical, _path) in index.items()
                        if key.startswith(prefix)
                        and key[len(prefix):].isdigit()]
                if not hits:
                    unresolved.add(name)
                    continue
            for hit in hits:
                shipped = allowed.get(hit.casefold())
                if shipped is None:
                    denied.setdefault(hit, set()).add(package)
                else:
                    stack.append(shipped)
    if denied:
        rendered = "; ".join(
            f"{hit} (from {', '.join(sorted(sources))})"
            for hit, sources in sorted(denied.items())[:8])
        raise BuildFailure(
            "guarded shipped packages reference project packages outside the "
            f"neutral allowlist: {rendered}")
    rows = sorted(closure.items())
    return {
        "format": REFERENCE_CLOSURE_FORMAT,
        "package_count": len(rows),
        "packages": [package for package, _row in rows],
        "closure_sha256": hashlib.sha256(_canonical_release_bytes(
            [[package, row] for package, row in rows])).hexdigest(),
        "unresolved_names": sorted(unresolved),
    }


def _unrealpak_executable(engine_root: Path, platform_name: str) -> Path:
    if platform_name == "Win64":
        path = engine_root / "Engine" / "Binaries" / "Win64" / "UnrealPak.exe"
    elif platform_name == "Mac":
        path = engine_root / "Engine" / "Binaries" / "Mac" / "UnrealPak"
    else:
        raise BuildFailure(
            f"guarded cooked inventory unsupported on {platform_name}")
    if not path.is_file():
        raise BuildFailure(f"source-engine UnrealPak is missing: {path}")
    return path


def _parse_unrealpak_listing(output: str) -> list[str]:
    """Entry paths from UnrealPak -List output, in one mount-rooted form.

    IoStore (.utoc) listings print each entry with its mount point already
    applied ("../../../WoT/..."). Legacy .pak listings print entries relative
    to the mount point announced on their "Listing X.pak with mount point"
    line; Mac stages its plugin descriptors and other UFS files there, so they
    appear as "WoT/..." or "Engine/...". Re-root those under their mount point
    so every container contributes the same path form.
    """
    pattern = re.compile(r'^(.*?):\s+"([^"]+)"\s+offset:\s*\d+,')
    pak_mount = re.compile(r'Listing .+? with mount point "([^"]*)"')
    mount = ""
    paths = []
    for line in output.splitlines():
        if (announced := pak_mount.search(line)):
            mount = announced.group(1).replace("\\", "/")
            continue
        if not (match := pattern.match(line)):
            continue
        path = match.group(2).replace("\\", "/")
        if ("LogPakFile" in match.group(1) and mount
                and not path.startswith(mount)):
            path = mount.rstrip("/") + "/" + path.lstrip("/")
        paths.append(path)
    return paths


def _configuration_suffix(configuration: str) -> str:
    """Evidence-file suffix: Test keeps its historical names."""
    return ("" if configuration == GUARDED_CONFIGURATION
            else "-" + configuration.casefold())


def _cooked_inventory_paths(iso: Path, configuration: str) -> tuple[Path, Path]:
    suffix = _configuration_suffix(configuration)
    return (iso / "receipts" / f"cooked-container-inventory{suffix}.json",
            iso / "logs" / f"cooked-container-inventory{suffix}.log")


def _shipped_staged_containers(staged_root: Path) -> list[Path]:
    """The staged pak/utoc containers that ship.

    Mac staging leaves the loose <Stage>/Mac/WoT/Content/Paks tree beside the
    bundle it assembles, holding byte-identical copies of the bundle's
    containers. Only WoT.app is archived (ditto, in package), so when any
    container lies inside an .app bundle the bundle copies are the shipped
    set; counting the loose twins too made every path a "duplicate". Win64
    stages no bundle and keeps every container.
    """
    containers = sorted(
        path for path in staged_root.rglob("*")
        if path.is_file() and path.suffix.casefold() in {".pak", ".utoc"})
    bundled = [
        path for path in containers
        if any(part.casefold().endswith(".app")
               for part in path.relative_to(staged_root).parts[:-1])]
    return bundled or containers


# UnrealPak lists global.utoc by failing fatally ("Missing directory index",
# the designed branch below). On Mac that fatal path then restores the cursor
# through WindowServer (FMacApplicationErrorOutputDevice::HandleErrorRestoreUI
# -> SLDisplayShowCursor), which never returns while the display is asleep:
# every unattended night run hung there until killed by hand. The fatal
# message is already written by then, so bound only this listing.
GLOBAL_CONTAINER_LIST_TIMEOUT_SECONDS = 120


def _decoded_output(value) -> str:
    """TimeoutExpired carries partial POSIX output as raw bytes (or None)."""
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def _list_staged_container(tool: Path, container: Path,
                           ) -> tuple[list[str], int | None, str]:
    """Run UnrealPak -List; returncode is None when the bounded wait expired."""
    command = [str(tool), str(container), "-List"]
    is_global = container.stem.casefold() == "global"
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, errors="replace",
            timeout=(GLOBAL_CONTAINER_LIST_TIMEOUT_SECONDS
                     if is_global else None))
    except subprocess.TimeoutExpired as exc:
        output = _decoded_output(exc.stdout) + _decoded_output(exc.stderr)
        return command, None, output + (
            f"\nTIMEOUT after {GLOBAL_CONTAINER_LIST_TIMEOUT_SECONDS}s "
            "(process killed)\n")
    return command, result.returncode, result.stdout + result.stderr


def _write_guarded_cooked_inventory(
        iso: Path, engine_root: Path, platform_name: str,
        configuration: str = GUARDED_CONFIGURATION) -> dict:
    tool = _unrealpak_executable(engine_root, platform_name)
    staged_root = iso / STAGING_DIRECTORIES[configuration]
    containers = _shipped_staged_containers(staged_root)
    if not containers:
        raise BuildFailure("guarded cook produced no listable staged containers")
    rows = []
    all_paths = []
    combined_log = []
    for container in containers:
        command, returncode, output = _list_staged_container(tool, container)
        combined_log.extend((
            f"COMMAND {subprocess.list2cmdline(command)}\n", output,
        ))
        if returncode != 0:
            if (container.stem.casefold() == "global"
                    and "Missing directory index for container" in output):
                # UE's global IoStore container intentionally has no file
                # directory: it carries global/script objects, not project
                # packages. Record and hash it, while the named project
                # container supplies the actual package-path inventory. The
                # raw absence audit still scans global.ucas byte-for-byte.
                rows.append({
                    "path": container.relative_to(staged_root).as_posix(),
                    "size": container.stat().st_size,
                    "sha256": _sha256_file(container),
                    "entry_count": None,
                    "listing": "global-container-has-no-directory-index",
                })
                continue
            raise BuildFailure(
                f"UnrealPak could not list guarded container {container} "
                + ("(timed out after "
                   f"{GLOBAL_CONTAINER_LIST_TIMEOUT_SECONDS}s)"
                   if returncode is None else f"(exit {returncode})"))
        listed = _parse_unrealpak_listing(output)
        if not listed:
            raise BuildFailure(
                f"UnrealPak listed no entries for guarded container {container}")
        all_paths.extend(listed)
        rows.append({
            "path": container.relative_to(staged_root).as_posix(),
            "size": container.stat().st_size,
            "sha256": _sha256_file(container),
            "entry_count": len(listed),
        })
    receipt_path, log_path = _cooked_inventory_paths(iso, configuration)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("".join(combined_log))
    validation = _validate_guarded_cooked_paths(all_paths)
    unique_paths = sorted(set(all_paths))
    receipt = {
        "format": COOKED_CONTAINER_RECEIPT_FORMAT,
        "platform": platform_name,
        "configuration": configuration,
        "tool": {
            "path": str(tool),
            "size": tool.stat().st_size,
            "sha256": _sha256_file(tool),
        },
        "containers": rows,
        "paths": unique_paths,
        "path_count": len(unique_paths),
        "validation": validation,
        "log": str(log_path),
        "log_sha256": _sha256_file(log_path),
    }
    _write_json(receipt_path, receipt)
    return {
        "path": str(receipt_path),
        "sha256": _sha256_file(receipt_path),
        "path_count": len(unique_paths),
        "validation": validation,
    }


def _cook_command(args, iso: Path, engine_root: Path) -> list[str]:
    guarded = _mode_is_guarded(args)
    command = [
        str(runuat_path(engine_root)), "BuildCookRun",
        f"-project={iso / 'WoT.uproject'}", f"-platform={args.platform}",
        # A guarded cook is always the Test (funnel) client; a Shipping release
        # is staged from this same cook by the shipping-stage step.
        f"-clientconfig={GUARDED_CONFIGURATION if guarded else args.configuration}",
        "-build", "-cook", "-stage", "-pak",
        "-nop4", "-utf8output", "-unattended",
        f"-stagingdirectory={iso / 'staged'}",
        "-map=" + "+".join(_cook_maps(guarded)),
    ]
    if guarded:
        # BuildCookRun does not forward otherwise-unrecognised top-level
        # switches to the cook commandlet.  Pass these explicitly through
        # AdditionalCookerOptions. The pipeline starts the endpoint itself;
        # NoZenAutoLaunch makes the cooker attach without spawning a second
        # server, while SkipZenStore keeps cooked output in ordinary files.
        cooker_options = [
            "-nullrhi", "-skipzenstore", "-ddc=(ZenLocal)",
            ZEN_NO_AUTO_LAUNCH,
            *_editor_process_affinity_arguments(
                getattr(args, "editor_process_affinity", None)),
            "-PACKAGE=" + "+".join(NEUTRAL_SUPPORT_ROOT_PACKAGES),
        ]
        command.append(
            "-AdditionalCookerOptions=" + " ".join(cooker_options))
        if args.platform == "Win64":
            command.extend((
                "-NoXGE",
                "-UbtArgs=" + " ".join(WIN64_SERIAL_LOCAL_UBT_ARGUMENTS),
            ))
        if _compile_cache_enabled(args):
            command.extend(BUILD_ROOT_UAT_SKIPS)
    if args.platform == "Win64":
        command.append("-NoBootstrapExe")
    return command


def _prepare_guarded_clean_cook(iso: Path, platform_name: str) -> None:
    """Clean only disposable project cook/stage outputs, never engine builds.

    This matches AutomationTool CookCommand.CleanupCookedData for the selected
    platform.  The explicit staging cleanup is defensive; UAT also cleans its
    stage directory before repopulating it.
    """
    cook_platform = "Mac" if platform_name == "Mac" else "Windows"
    for path in (iso / "Saved" / "Cooked" / cook_platform, iso / "staged"):
        if path.is_symlink():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            raise BuildFailure(
                f"guarded clean-cook output is not a directory: {path}")


def _zen_processes() -> list[dict]:
    if host_platform.system() == "Windows":
        # No ps on Windows; Win32_Process supplies the same pid+command rows.
        # CommandLine is null for protected/system rows -- fall back to the
        # image name so a zenserver.exe still matches. Quotes are stripped so
        # a quoted executable path still ends the match at a word boundary.
        script = (
            "Get-CimInstance Win32_Process | ForEach-Object { "
            "$line = if ($_.CommandLine) { $_.CommandLine } else { $_.Name }; "
            "'{0} {1}' -f $_.ProcessId, ($line -replace '\"', '') }")
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive",
             "-Command", script],
            capture_output=True, text=True)
    else:
        result = subprocess.run(
            ["ps", "-axo", "pid=,command="], capture_output=True, text=True)
    if result.returncode != 0:
        raise BuildFailure(
            f"cannot inspect Zen lifetime: {result.stderr.strip()}")
    rows = []
    for line in result.stdout.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        pid_text, separator, command = stripped.partition(" ")
        if not separator or not pid_text.isdigit():
            continue
        if re.search(
                r"(?:^|[/\\])zenserver(?:\.exe)?(?:\s|$)",
                command, flags=re.IGNORECASE):
            rows.append({"pid": int(pid_text), "command": command})
    return rows


def _zen_version_identity(engine_root: Path, platform_name: str) -> dict:
    executable = (engine_root / "Engine" / "Binaries" /
                  ("Mac" if platform_name == "Mac" else "Win64") /
                  ("zenserver" if platform_name == "Mac" else "zenserver.exe"))
    if not executable.is_file():
        raise BuildFailure(f"pinned Zen executable is missing: {executable}")
    return {
        "path": str(executable),
        "size": executable.stat().st_size,
        "sha256": _sha256_file(executable),
    }


def _wait_for_zen_health(
        endpoint: str, process: subprocess.Popen,
        timeout_seconds: float = 20.0) -> dict:
    deadline = time.monotonic() + timeout_seconds
    last_error = "not attempted"
    while time.monotonic() < deadline:
        return_code = process.poll()
        if return_code is not None:
            raise BuildFailure(
                f"pipeline-sponsored Zen exited before health check "
                f"(exit {return_code})")
        try:
            with urllib.request.urlopen(
                    endpoint + "/health/ready", timeout=1.0) as response:
                body = response.read()
                status = int(response.status)
            if status == 200:
                return {
                    "url": endpoint + "/health/ready",
                    "status_code": status,
                    "body_sha256": hashlib.sha256(body).hexdigest(),
                    "checked_utc": _utc_now(),
                }
            last_error = f"HTTP {status}"
        except (OSError, urllib.error.URLError) as exc:
            last_error = str(exc)
        time.sleep(0.1)
    raise BuildFailure(
        f"pipeline-sponsored Zen failed health check: {last_error}")


def _stop_zen_sponsor(process: subprocess.Popen) -> dict:
    forced = False
    signal_name = "terminate"
    if process.poll() is None:
        process.terminate()
        try:
            return_code = process.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            forced = True
            signal_name = "kill"
            process.kill()
            return_code = process.wait(timeout=10.0)
    else:
        return_code = process.returncode
        signal_name = "already-exited"
    return {
        "action": "stop-pipeline-sponsor",
        "target_pid": process.pid,
        "signal": signal_name,
        "forced": forced,
        "wait_return_code": return_code,
    }


def _zen_identity_set(rows: Sequence[dict], label: str) -> set[tuple[int, str]]:
    identities = []
    for row in rows:
        if (not isinstance(row, dict)
                or type(row.get("pid")) is not int
                or row["pid"] <= 0
                or not isinstance(row.get("command"), str)
                or not row["command"]):
            raise BuildFailure(f"{label} has a malformed Zen process identity")
        identities.append((row["pid"], row["command"]))
    if len(identities) != len(set(identities)):
        raise BuildFailure(f"{label} has duplicate Zen process identities")
    pids = [identity[0] for identity in identities]
    if len(pids) != len(set(pids)):
        raise BuildFailure(f"{label} has duplicate Zen process ids")
    return set(identities)


def _observed_zen_command(command: Sequence[str]) -> str:
    return " ".join(str(value) for value in command).replace('"', "")


def _zen_row_matches_sponsor_command(
        row: dict, sponsor_command: Sequence[str]) -> bool:
    return row["command"] == _observed_zen_command(sponsor_command)


def _canonical_zen_sponsor_command(
        executable: str, data_dir: Path, owner_pid: int) -> list[str]:
    return [
        executable,
        "--port", str(ZEN_SPONSOR_PORT),
        "--allow-port-probing=false",
        "--http-forceloopback",
        "--data-dir", str(data_dir),
        "--owner-pid", str(owner_pid),
        "--detach=false",
        "--no-sentry",
        "--quiet",
    ]


def _zen_command_advertises_fixed_port(command: str) -> bool:
    return re.search(
        rf"(?:^|\s)--port(?:\s+|=){ZEN_SPONSOR_PORT}(?:\s|$)",
        command, flags=re.IGNORECASE) is not None


def _prove_zen_endpoint_unoccupied() -> dict:
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        probe.bind(("127.0.0.1", ZEN_SPONSOR_PORT))
    except OSError as exc:
        raise BuildFailure(
            f"guarded Zen endpoint 127.0.0.1:{ZEN_SPONSOR_PORT} is occupied") \
            from exc
    finally:
        probe.close()
    return {
        "host": "127.0.0.1",
        "port": ZEN_SPONSOR_PORT,
        "exclusive_bind_succeeded": True,
        "checked_utc": _utc_now(),
    }


def _validate_guarded_zen_cook_command(command: Sequence[str]) -> None:
    values = [str(value) for value in command
              if str(value).startswith("-AdditionalCookerOptions=")]
    if len(values) != 1:
        raise BuildFailure(
            "guarded cook requires one isolated Zen cooker-options argument")
    tokens = values[0].split("=", 1)[1].split()
    outer = [str(value) for value in command if str(value) not in values]
    no_auto = [token for token in tokens
               if token.casefold().startswith("-nozenautolaunch=")]
    ddc = [token for token in tokens
           if token.casefold().startswith("-ddc=")]
    if (tokens.count("-skipzenstore") != 1
            or ddc != ["-ddc=(ZenLocal)"]
            or no_auto != [ZEN_NO_AUTO_LAUNCH]
            or any(token.casefold().startswith(
                ("-ddc=", "-nozenautolaunch=")) for token in outer)
            or any("127.0.0.1:8558" in token.casefold()
                   for token in outer)):
        raise BuildFailure(
            "guarded cook requires isolated Zen switches -skipzenstore, "
            f"-ddc=(ZenLocal), and {ZEN_NO_AUTO_LAUNCH}")
    package_roots = [
        token for token in tokens if token.casefold().startswith("-package=")]
    expected_package_roots = (
        "-PACKAGE=" + "+".join(NEUTRAL_SUPPORT_ROOT_PACKAGES))
    if (package_roots != [expected_package_roots]
            or any(str(token).casefold().startswith("-package=")
                   for token in outer)):
        raise BuildFailure(
            "guarded cook proof does not preserve the exact neutral support roots")


def _validate_guarded_unreal_command(
        command: Sequence[str], kind: str, platform_name: str = "") -> str:
    if kind == "cook":
        _validate_guarded_zen_cook_command(command)
        if not platform_name:
            platform_tokens = [
                str(value).split("=", 1)[1]
                for value in command
                if str(value).casefold().startswith("-platform=")]
            if len(platform_tokens) == 1:
                platform_name = platform_tokens[0]
        if platform_name == "Win64":
            tokens = [str(value) for value in command]
            expected_ubt = (
                "-UbtArgs=" + " ".join(WIN64_SERIAL_LOCAL_UBT_ARGUMENTS))
            ubt = [token for token in tokens
                   if token.casefold().startswith("-ubtargs")]
            control_prefixes = (
                "-maxparallelactions", "-xge", "-noxge",
                "-fastbuild", "-nofastbuild", "-sndbs", "-nosndbs",
                "-uba", "-nouba", "-allowremoteexecutor",
            )
            outer_controls = [
                token for token in tokens
                if token.casefold().startswith(control_prefixes)]
            if ubt != [expected_ubt] or outer_controls != ["-NoXGE"]:
                raise BuildFailure(
                    "guarded Win64 cook requires the exact serial/local "
                    "UBT arguments and one outer -NoXGE")
        return kind
    if kind != "editor":
        raise BuildFailure(f"unknown guarded Unreal command kind: {kind}")
    tokens = [str(value) for value in command]
    ddc = [token for token in tokens
           if token.casefold().startswith("-ddc=")]
    no_auto = [token for token in tokens
               if token.casefold().startswith("-nozenautolaunch=")]
    if ddc != ["-ddc=(ZenLocal)"] or no_auto != [ZEN_NO_AUTO_LAUNCH]:
        raise BuildFailure(
            "guarded editor command requires exactly -ddc=(ZenLocal) and "
            f"{ZEN_NO_AUTO_LAUNCH}")
    if any("127.0.0.1:8558" in token.casefold() for token in tokens):
        raise BuildFailure("guarded editor command references preexisting Zen")
    return kind


def _validate_zen_process_partition(
        before: Sequence[dict], during: Sequence[dict], sponsor_pid: int,
        sponsor_command: Sequence[str]) -> dict:
    before_set = _zen_identity_set(before, "preexisting Zen snapshot")
    during_set = _zen_identity_set(during, "guarded-cook Zen snapshot")
    sponsor_rows = [row for row in during if row["pid"] == sponsor_pid]
    if (len(sponsor_rows) != 1
            or not _zen_row_matches_sponsor_command(
                sponsor_rows[0], sponsor_command)):
        raise BuildFailure(
            "guarded cook did not retain the exact owned Zen sponsor")
    foreign_set = {
        identity for identity in during_set if identity[0] != sponsor_pid
    }
    if foreign_set != before_set or any(
            identity[0] == sponsor_pid for identity in before_set):
        raise BuildFailure(
            "guarded cook changed the preexisting Zen process set")
    return sponsor_rows[0]


def _run_action_with_zen_monitor(
        action, before: Sequence[dict], sponsor_pid: int,
        sponsor_command: Sequence[str]):
    stop = threading.Event()
    violations = []
    sample_count = 0
    started_utc = _utc_now()

    def monitor() -> None:
        nonlocal sample_count
        while not stop.is_set():
            try:
                snapshot = _zen_processes()
                sample_count += 1
                _validate_zen_process_partition(
                    before, snapshot, sponsor_pid, sponsor_command)
            except BaseException as exc:
                violations.append(exc)
                stop.set()
                return
            stop.wait(ZEN_MONITOR_INTERVAL_SECONDS)

    thread = threading.Thread(
        target=monitor, name="guarded-zen-monitor", daemon=True)
    thread.start()
    try:
        result = action()
    finally:
        stop.set()
        thread.join()
    finished_utc = _utc_now()
    receipt = {
        "interval_seconds": ZEN_MONITOR_INTERVAL_SECONDS,
        "sample_count": sample_count,
        "started_utc": started_utc,
        "finished_utc": finished_utc,
        "violation": (str(violations[0]) if violations else None),
    }
    if violations:
        raise BuildFailure(
            "Zen process-set violation during guarded Unreal lifecycle: "
            f"{violations[0]}") from violations[0]
    if sample_count < 1:
        raise BuildFailure(
            "guarded Unreal lifecycle Zen monitor captured no samples")
    return result, receipt


def _run_logged_with_zen_monitor(
        command: Sequence[str], log_path: Path, before: Sequence[dict],
        sponsor_pid: int, sponsor_command: Sequence[str]) -> tuple[int, dict]:
    return _run_action_with_zen_monitor(
        lambda: _run_logged("cook", command, log_path), before,
        sponsor_pid, sponsor_command)


def _run_guarded_unreal_lifecycle(
        iso: Path, engine_root: Path, platform_name: str, action):
    global _ACTIVE_GUARDED_ZEN, _ACTIVE_GUARDED_ZEN_COMMANDS
    global _ACTIVE_GUARDED_DDC_OVERRIDES
    if _ACTIVE_GUARDED_ZEN:
        raise BuildFailure("guarded Unreal lifecycle cannot be nested")
    before = _zen_processes()
    _zen_identity_set(before, "preexisting Zen snapshot")
    if any(_zen_command_advertises_fixed_port(row["command"])
           for row in before):
        raise BuildFailure(
            f"preexisting Zen already advertises guarded port "
            f"{ZEN_SPONSOR_PORT}")
    endpoint_preflight = _prove_zen_endpoint_unoccupied()
    version = _zen_version_identity(engine_root, platform_name)
    data_dir = iso / "Saved" / f"GuardedZenSponsor-{os.getpid()}"
    if data_dir.exists():
        raise BuildFailure(
            f"guarded Zen data path must begin absent: {data_dir}")
    data_dir.mkdir(parents=True)
    sponsor_log = iso / "logs" / "zen-sponsor.log"
    sponsor_log.parent.mkdir(parents=True, exist_ok=True)
    sponsor_command = _canonical_zen_sponsor_command(
        version["path"], data_dir, os.getpid())
    action_result = None
    run_error = None
    health = None
    monitor_receipt = None
    processes_after_lifecycle = []
    process = None
    cleanup = {
        "action": "not-started",
        "verified_stopped": False,
        "data_directory_removed": False,
    }
    with sponsor_log.open("wb") as sponsor_output:
        try:
            process = subprocess.Popen(
                sponsor_command, stdout=sponsor_output,
                stderr=subprocess.STDOUT)
            health = _wait_for_zen_health(ZEN_SPONSOR_ENDPOINT, process)
            processes_after_start = _zen_processes()
            _validate_zen_process_partition(
                before, processes_after_start, process.pid, sponsor_command)
            _ACTIVE_GUARDED_ZEN_COMMANDS = []
            _unused_environment, _ACTIVE_GUARDED_DDC_OVERRIDES = \
                _guarded_unreal_environment(None)
            _ACTIVE_GUARDED_ZEN = True
            try:
                action_result, monitor_receipt = _run_action_with_zen_monitor(
                    action, before, process.pid, sponsor_command)
            finally:
                _ACTIVE_GUARDED_ZEN = False
            processes_after_lifecycle = _zen_processes()
            _validate_zen_process_partition(
                before, processes_after_lifecycle,
                process.pid, sponsor_command)
        except BaseException as exc:
            run_error = exc
        finally:
            if process is not None:
                try:
                    cleanup.update(_stop_zen_sponsor(process))
                except BaseException as exc:
                    if run_error is None:
                        run_error = exc
                    cleanup["stop_error"] = type(exc).__name__
    after = _zen_processes()
    try:
        cleanup["verified_stopped"] = (
            process is not None
            and all(row["pid"] != process.pid for row in after)
            and _zen_identity_set(after, "post-lifecycle Zen snapshot")
            == _zen_identity_set(before, "preexisting Zen snapshot"))
    except BuildFailure as exc:
        cleanup["verified_stopped"] = False
        if run_error is None:
            run_error = exc
    if data_dir.is_dir() and not data_dir.is_symlink():
        shutil.rmtree(data_dir)
    cleanup["data_directory_removed"] = not data_dir.exists()
    receipt = {
        "format": ZEN_RECEIPT_FORMAT,
        "mechanism": "pipeline-sponsored-zen-v3",
        "unreal_isolation_switches": [
            "-ddc=(ZenLocal)", ZEN_NO_AUTO_LAUNCH],
        "endpoint": ZEN_SPONSOR_ENDPOINT,
        "owner": {
            "kind": "pipeline-process",
            "pid": os.getpid(),
        },
        "sponsor": {
            "pid": process.pid if process is not None else None,
            "command": sponsor_command,
            "log": str(sponsor_log),
        },
        "version": version,
        "before": before,
        "endpoint_preflight": endpoint_preflight,
        "health": health,
        "processes_after_lifecycle": processes_after_lifecycle,
        "monitor": monitor_receipt,
        "unreal_commands": list(_ACTIVE_GUARDED_ZEN_COMMANDS),
        "environment_policy": "strip-all-ue-datacache-overrides-v1",
        "stripped_ddc_environment_overrides":
            list(_ACTIVE_GUARDED_DDC_OVERRIDES),
        "after": after,
        "cleanup": cleanup,
        "lifecycle_status": "error" if run_error is not None else "success",
    }
    receipt_path = iso / "receipts" / "zen-lifetime.json"
    _write_json(receipt_path, receipt)
    if run_error is not None:
        raise run_error
    if not cleanup["verified_stopped"]:
        raise BuildFailure(
            "pipeline-owned Zen cleanup did not preserve the preexisting Zen set")
    row = {
        "path": str(receipt_path),
        "sha256": _sha256_file(receipt_path),
        "receipt": receipt,
    }
    return action_result, row


def _run_guarded_cook_with_sponsored_zen(
        command: Sequence[str], log_path: Path, iso: Path,
        engine_root: Path, platform_name: str,
        env: dict[str, str] | None = None) -> dict:
    _validate_guarded_unreal_command(command, "cook", platform_name)
    _result, row = _run_guarded_unreal_lifecycle(
        iso, engine_root, platform_name,
        lambda: _run_logged("cook", command, log_path, env=env))
    return row


def _eos_credentials_preview(configuration: str) -> dict:
    """What the next staging run of `configuration` would inject: state, key
    set and canonical-file digest only (eos_credentials never exposes a
    value). Read from the local secrets file at call time, never from git."""
    return eos_credentials.receipt(eos_credentials.load(), configuration)


def _inject_eos_credentials(iso: Path, configuration: str) -> dict:
    """Rewrite the ISOLATED DefaultEngine.ini for one staging run.

    Test (and any non-Shipping) packages get one "WoTTest" artifact with
    DeploymentIdTest, Shipping packages one "WoT" artifact with
    DeploymentIdPublic. A missing or invalid secrets file leaves no artifact:
    EOS is cleanly disabled in that package and the build continues.
    """
    loaded = eos_credentials.load()
    config = iso / "Config" / "DefaultEngine.ini"
    if not config.is_file():
        if loaded.state == "present":
            raise BuildFailure(f"isolated engine config is missing: {config}")
        return eos_credentials.receipt(loaded, configuration)
    try:
        return eos_credentials.apply_to_config(config, loaded, configuration)
    except eos_credentials.CredentialError as exc:
        raise BuildFailure(f"EOS credential injection refused: {exc}") from exc


def stage_cook(args, iso: Path) -> str:
    guarded = _mode_is_guarded(args)
    eos_configuration = (GUARDED_CONFIGURATION if guarded
                         else getattr(args, "configuration", "Development"))
    maps = _cook_maps(guarded)
    stamp = _read_stamp(iso, "cook")
    try:
        containers = absence_audit.find_containers(iso / "staged")
    except (absence_audit.AuditError, OSError):
        containers = []
    guarded_cache_valid = not guarded
    if guarded:
        try:
            _guarded_receipt_bindings(stamp, args.platform)
            # The product binding re-hashes the STAGED game and updater against
            # their final-build receipts, which is the cook's evidence. The
            # Binaries/ build outputs are deliberately not required: the
            # shipping-stage wrapper build's -Clean removes the Test products
            # from Binaries/<P> (build_guarded_ue58.ps1:66-68 "$Target*";
            # build_guarded_ue58.sh:86-88) without touching the audited Test
            # staged tree.
            _guarded_product_binding(stamp, iso, args.platform)
            _guarded_zen_binding(stamp, iso, args.platform)
            guarded_cache_valid = True
        except BuildFailure:
            guarded_cache_valid = False
    if (stamp and stamp.get("command_exit_code") == 0
            and stamp.get("platform") == args.platform
            and stamp.get("maps") == list(maps)
            and (not guarded or stamp.get("clean_cook_strategy")
                 == "project-cooked-platform+staging-v1")
            and guarded_cache_valid
            and not (guarded and _ACTIVE_GUARDED_ZEN)
            and eos_credentials.stage_identity(stamp.get("eos_credentials"))
            == eos_credentials.stage_identity(
                _eos_credentials_preview(eos_configuration))
            and containers):
        return "cached"
    _invalidate_from(iso, "cook")
    engine_root, _editor = _engine_root_and_editor(args.engine)
    if guarded:
        pinned = PINNED_SOURCE_ENGINES[args.platform].resolve(strict=False)
        if engine_root.resolve(strict=False) != pinned:
            raise BuildFailure(
                f"guarded cook resolved non-pinned engine {engine_root}")
        bootstrap_package = iso / "Content" / "WoT" / "Maps" / "WoTBootstrap.umap"
        if not bootstrap_package.is_file() or BOOTSTRAP_MAP not in maps:
            raise BuildFailure(
                "guarded cook requires generated /Game/WoT/Maps/WoTBootstrap")
        neutral_support_authority = _neutral_support_lifecycle_receipt(
            iso, require_import=True)
        # Before any compile: a reference escaping the allowlist fails fast.
        reference_closure = _verify_guarded_reference_closure(iso)
        target_receipts = {
            "WoT": _run_guarded_target_build(args, iso, engine_root, "WoT")
        }
        # The editor the cook runs, built from the ISO's final inputs (the
        # neutral-shell plugin and payload marker now exist), exactly where
        # UAT's -build would otherwise have compiled it from scratch.
        cook_editor = (_build_root_editor(args, iso, engine_root, "cook")
                       if _compile_cache_enabled(args) else None)
    else:
        neutral_support_authority = None
        target_receipts = {}
    if guarded:
        _prepare_guarded_clean_cook(iso, args.platform)
    # Immediately before UAT stages the config into this package.
    eos_receipt = _inject_eos_credentials(iso, eos_configuration)
    command = _cook_command(args, iso, engine_root)
    log_path = iso / "logs" / "cook.log"
    tool_environment = _iso_tool_environment(args, engine_root)
    if guarded and _ACTIVE_GUARDED_ZEN:
        _run_logged("cook", command, log_path, env=tool_environment)
        zen_receipt = None
    elif guarded:
        zen_receipt = _run_guarded_cook_with_sponsored_zen(
            command, log_path, iso, engine_root, args.platform,
            env=tool_environment)
    else:
        zen_receipt = None
        _run_logged("cook", command, log_path, env=tool_environment)
    if guarded:
        signing_facts: dict = {}
        staged_root = _prepare_guarded_stage(
            args, iso, signing_facts=signing_facts)
        product_receipt = _write_guarded_product_receipt(
            iso, args.platform, staged_root, target_receipts,
            signing_facts=(signing_facts if args.platform == "Mac" else None))
        _bind_transferred_products(
            product_receipt["receipt"], target_receipts, args.platform,
            GUARDED_CONFIGURATION)
        cooked_inventory = _write_guarded_cooked_inventory(
            iso, engine_root, args.platform)
    containers = absence_audit.find_containers(iso / "staged")
    details = {
        "command_exit_code": 0,
        "platform": args.platform,
        "engine": str(engine_root),
        "staged": str(iso / "staged"),
        "map_count": len(maps),
        "maps": list(maps),
        "container_count": len(containers),
        "log": str(log_path),
        "eos_credentials": eos_receipt,
    }
    if guarded:
        details.update({
            "mode": "guarded",
            "configuration": GUARDED_CONFIGURATION,
            "clean_cook_strategy": "project-cooked-platform+staging-v1",
            "target_receipts": target_receipts,
            "product_receipt": product_receipt,
            "cooked_container_inventory": cooked_inventory,
            "zen_lifetime": zen_receipt,
            "neutral_support_authority": neutral_support_authority,
            "reference_closure": reference_closure,
            "compile_cache": {
                "mode": ("build-root-v1" if _compile_cache_enabled(args)
                         else "clean-iso-v1"),
                "uat_build_skips": (list(BUILD_ROOT_UAT_SKIPS)
                                    if _compile_cache_enabled(args) else []),
                "cook_editor": cook_editor,
            },
        })
    _write_stamp(iso, "cook", details)
    return "ran"


def _sha256_file(path: Path) -> str:
    import hashlib
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_json_object(path: Path, what: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BuildFailure(f"cannot read {what} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise BuildFailure(f"{what} is not a JSON object: {path}")
    return value


def _guarded_receipt_bindings(cook_stamp: dict | None,
                              platform_name: str,
                              configuration: str = GUARDED_CONFIGURATION
                              ) -> dict:
    """Bind a build stamp (the Test cook, or the Shipping stage) to receipts."""
    if (not cook_stamp or cook_stamp.get("mode") != "guarded"
            or cook_stamp.get("configuration") != configuration):
        raise BuildFailure(
            f"guarded packaging requires a {configuration} "
            + ("cook stamp" if configuration == GUARDED_CONFIGURATION
               else "stage stamp"))
    _validate_neutral_support_receipt(
        cook_stamp.get("neutral_support_authority"))
    maps = cook_stamp.get("maps")
    if not isinstance(maps, list):
        raise BuildFailure("guarded packaging cook stamp maps are malformed")
    required_maps = (BOOTSTRAP_MAP, FRONT_END_MAP, *NEUTRAL_SHELL_MAPS)
    missing_maps = [path for path in required_maps if path not in maps]
    if missing_maps:
        if FRONT_END_MAP in missing_maps:
            raise BuildFailure(
                "guarded packaging cook stamp is missing the neutral front-end shell")
        raise BuildFailure(
            "guarded packaging cook stamp is missing required bootstrap map "
            "or neutral-shell map(s): "
            + ", ".join(missing_maps))
    if maps != list(required_maps):
        raise BuildFailure(
            "guarded packaging cook stamp map census is not exactly the "
            "bootstrap/front-end shells plus nineteen multiplayer shells")
    _guarded_cooked_inventory_binding(cook_stamp, platform_name, configuration)
    rows = cook_stamp.get("target_receipts")
    if not isinstance(rows, dict) or set(rows) != set(GUARDED_RECEIPT_TARGETS):
        raise BuildFailure(
            "guarded packaging requires the exact WoT receipt")

    bindings = {}
    engine_identity = None
    for target in GUARDED_RECEIPT_TARGETS:
        row = rows.get(target)
        if not isinstance(row, dict):
            raise BuildFailure(f"guarded {target} receipt row is malformed")
        engine_path = Path(str(row.get("engine", "")))
        compiled_path = Path(str(row.get("compiled", "")))
        if not engine_path.is_file() or not compiled_path.is_file():
            raise BuildFailure(f"guarded {target} receipt file is missing")
        engine_sha256 = _sha256_file(engine_path)
        compiled_sha256 = _sha256_file(compiled_path)
        if (row.get("engine_sha256") != engine_sha256
                or row.get("compiled_sha256") != compiled_sha256):
            raise BuildFailure(f"guarded {target} receipt changed after the cook")
        engine = _read_json_object(engine_path, f"{target} engine receipt")
        compiled = _read_json_object(
            compiled_path, f"{target} compiled receipt")
        if (engine.get("format") != "wot-ue58-source-receipt-v1"
                or engine.get("platform") != platform_name
                or compiled.get("format") != "wot-guarded-build-receipt-v1"
                or compiled.get("target") != target
                or compiled.get("platform") != platform_name
                or compiled.get("configuration") != configuration):
            raise BuildFailure(f"guarded {target} receipt identity is wrong")
        definitions = compiled.get("definitions", {})
        checks = compiled.get("checks", [])
        if not isinstance(definitions, dict) or not isinstance(checks, list):
            raise BuildFailure(f"guarded {target} receipt identity is wrong")
        if target == "WoT" and configuration == GUARDED_CONFIGURATION:
            if (definitions.get("WOT_GUARDED_BOOTSTRAP") != "1"
                    or definitions.get("WOT_BOOTSTRAP_TEST_IDENTITIES") != "1"
                    or definitions.get("WOT_BOOTSTRAP_TEST_ENDPOINTS") != "1"
                    or any(definitions.get(name) != value for name, value in
                           GUARDED_GAME_PROFILE_DEFINITIONS.items())):
                raise BuildFailure("guarded WoT receipt is not the Test game")
        elif target == "WoT":
            # The production image: guarded, production identity/endpoint, and
            # no test-only definition at all (WoT.Target.cs:50-58;
            # record_build_receipt.py refuses them in Shipping already).
            if (any(definitions.get(name) != value
                    for name, value in SHIPPING_GAME_DEFINITIONS.items())
                    or any(name in definitions
                           for name in TEST_ONLY_DEFINITIONS)
                    or "test-only-definitions-absent" not in checks):
                raise BuildFailure(
                    "guarded WoT receipt is not the production Shipping game")
        if compiled.get("engine") != {
                key: engine[key]
                for key in ("commit", "tag", "engine", "toolchain", "checks")
                if key in engine}:
            raise BuildFailure(
                f"guarded {target} compiled receipt does not fold in its engine receipt")
        current_engine_identity = compiled.get("engine")
        if engine_identity is None:
            engine_identity = current_engine_identity
        elif current_engine_identity != engine_identity:
            raise BuildFailure(
                "guarded receipts name different engines")
        bindings[target] = {
            "engine_receipt_sha256": engine_sha256,
            "compiled_receipt_sha256": compiled_sha256,
            "engine_receipt": engine,
            "compiled_receipt": compiled,
        }
    return bindings


def _guarded_cooked_inventory_binding(
        cook_stamp: dict | None, platform_name: str,
        configuration: str = GUARDED_CONFIGURATION) -> dict:
    row = (cook_stamp or {}).get("cooked_container_inventory")
    if not isinstance(row, dict):
        raise BuildFailure("guarded cook has no cooked-container inventory")
    path = Path(str(row.get("path", "")))
    receipt = _read_json_object(path, "guarded cooked-container inventory")
    if (row.get("sha256") != _sha256_file(path)
            or receipt.get("format") != COOKED_CONTAINER_RECEIPT_FORMAT
            or receipt.get("platform") != platform_name
            or receipt.get("configuration") != configuration
            or receipt.get("path_count") != len(receipt.get("paths", []))):
        raise BuildFailure("guarded cooked-container inventory identity is wrong")
    tool = receipt.get("tool")
    if not isinstance(tool, dict):
        raise BuildFailure("guarded cooked-container inventory tool is malformed")
    tool_path = Path(str(tool.get("path", "")))
    if (not tool_path.is_file()
            or tool.get("size") != tool_path.stat().st_size
            or tool.get("sha256") != _sha256_file(tool_path)):
        raise BuildFailure("guarded cooked-container inventory tool changed")
    containers = receipt.get("containers")
    if not isinstance(containers, list) or not containers:
        raise BuildFailure("guarded cooked-container inventory has no containers")
    staged = path.parents[1] / STAGING_DIRECTORIES[configuration]
    current_container_paths = {
        candidate.relative_to(staged).as_posix()
        for candidate in _shipped_staged_containers(staged)
    }
    receipted_container_paths = set()
    for container in containers:
        if not isinstance(container, dict):
            raise BuildFailure("guarded cooked-container row is malformed")
        relative = str(container.get("path", ""))
        container_path = staged / relative
        try:
            container_path.resolve(strict=True).relative_to(
                staged.resolve(strict=True))
        except (OSError, ValueError) as exc:
            raise BuildFailure(
                "guarded cooked-container row escapes the staged tree") from exc
        if (not relative or relative in receipted_container_paths
                or Path(relative).suffix.casefold() not in {".pak", ".utoc"}):
            raise BuildFailure(
                "guarded cooked-container row identity is malformed")
        receipted_container_paths.add(relative)
        if (not container_path.is_file()
                or container.get("size") != container_path.stat().st_size
                or container.get("sha256") != _sha256_file(container_path)):
            raise BuildFailure(
                f"guarded cooked container changed: {container_path}")
    if receipted_container_paths != current_container_paths:
        raise BuildFailure(
            "guarded cooked-container container census differs from the "
            "current staged tree")
    validation = _validate_guarded_cooked_paths(receipt.get("paths", []))
    if (receipt.get("validation") != validation
            or row.get("validation") != validation
            or row.get("path_count") != receipt.get("path_count")):
        raise BuildFailure("guarded cooked-container validation changed")
    return {
        "receipt_sha256": row["sha256"],
        "receipt": receipt,
        "path_count": receipt["path_count"],
        "validation": validation,
    }


def _guarded_zen_receipt_has_exact_schema(receipt: object) -> bool:
    if not isinstance(receipt, dict) or set(receipt) != {
            "format", "mechanism", "unreal_isolation_switches", "endpoint",
            "owner", "sponsor", "version", "before", "endpoint_preflight",
            "health", "processes_after_lifecycle", "monitor",
            "unreal_commands", "environment_policy",
            "stripped_ddc_environment_overrides", "after", "cleanup",
            "lifecycle_status"}:
        return False
    exact_nested = (
        ("owner", {"kind", "pid"}),
        ("sponsor", {"pid", "command", "log"}),
        ("version", {"path", "size", "sha256"}),
        ("endpoint_preflight", {
            "host", "port", "exclusive_bind_succeeded", "checked_utc"}),
        ("health", {"url", "status_code", "body_sha256", "checked_utc"}),
        ("monitor", {
            "interval_seconds", "sample_count", "started_utc",
            "finished_utc", "violation"}),
        ("cleanup", {
            "action", "target_pid", "signal", "forced", "wait_return_code",
            "verified_stopped", "data_directory_removed"}),
    )
    if any(not isinstance(receipt.get(name), dict)
           or set(receipt[name]) != keys for name, keys in exact_nested):
        return False
    for name in ("before", "processes_after_lifecycle", "after"):
        rows = receipt.get(name)
        if (not isinstance(rows, list)
                or any(not isinstance(row, dict)
                       or set(row) != {"pid", "command"} for row in rows)):
            return False
    commands = receipt.get("unreal_commands")
    command_keys = {
        "stage", "kind", "command", "log", "log_sha256", "exit_code",
        "stripped_ddc_environment_overrides"}
    return bool(
        isinstance(commands, list)
        and all(isinstance(row, dict) and set(row) == command_keys
                for row in commands))


def _guarded_zen_binding(
        cook_stamp: dict | None, iso: Path, platform_name: str) -> dict:
    row = (cook_stamp or {}).get("zen_lifetime")
    if not isinstance(row, dict):
        raise BuildFailure("guarded cook has no Zen lifetime receipt")
    path = Path(str(row.get("path", "")))
    receipt = _read_json_object(path, "Zen lifetime receipt")
    exact_schema_valid = _guarded_zen_receipt_has_exact_schema(receipt)
    sponsor = receipt.get("sponsor", {})
    owner = receipt.get("owner", {})
    version = receipt.get("version", {})
    health = receipt.get("health", {})
    endpoint_preflight = receipt.get("endpoint_preflight", {})
    monitor = receipt.get("monitor", {})
    cleanup = receipt.get("cleanup", {})
    before = receipt.get("before", [])
    during = receipt.get("processes_after_lifecycle", [])
    after = receipt.get("after", [])
    stamp_platform = (cook_stamp or {}).get("platform")
    cook_log = Path(str((cook_stamp or {}).get("log", "")))
    iso = iso.resolve(strict=False)
    expected_engine = PINNED_SOURCE_ENGINES.get(str(platform_name))
    expected_receipt_path = iso / "receipts" / "zen-lifetime.json"
    engine_anchor_valid = False
    expected_editor = None
    expected_runuat = None
    expected_zen = None
    if expected_engine is not None and platform_name in ("Mac", "Win64"):
        expected_engine = expected_engine.resolve(strict=False)
        platform_dir = "Mac" if platform_name == "Mac" else "Win64"
        expected_editor = expected_engine / "Engine" / "Binaries" / \
            platform_dir / ("UnrealEditor-Cmd" if platform_name == "Mac"
                            else "UnrealEditor-Cmd.exe")
        expected_runuat = expected_engine / "Engine" / "Build" / \
            "BatchFiles" / ("RunUAT.sh" if platform_name == "Mac"
                            else "RunUAT.bat")
        expected_zen = expected_engine / "Engine" / "Binaries" / \
            platform_dir / ("zenserver" if platform_name == "Mac"
                            else "zenserver.exe")
        engine_anchor_valid = bool(
            stamp_platform == platform_name
            and Path(str((cook_stamp or {}).get("engine", ""))).resolve(
                strict=False) == expected_engine
            and path.resolve(strict=False)
            == expected_receipt_path.resolve(strict=False)
            and cook_log.resolve(strict=False)
            == (iso / "logs" / "cook.log").resolve(strict=False))
    sponsor_pid = sponsor.get("pid")
    process_proof_valid = False
    try:
        if (type(sponsor_pid) is int
                and isinstance(version.get("path"), str)):
            _validate_zen_process_partition(
                before, during, sponsor_pid, sponsor.get("command", []))
            process_proof_valid = (
                _zen_identity_set(after, "post-lifecycle Zen receipt")
                == _zen_identity_set(before, "preexisting Zen receipt")
                and all(process.get("pid") != sponsor_pid
                        for process in after)
                and not any(_zen_command_advertises_fixed_port(
                    process["command"]) for process in before))
    except BuildFailure:
        process_proof_valid = False
    sponsor_command = sponsor.get("command")
    owner_valid = (
        isinstance(owner, dict)
        and owner.get("kind") == "pipeline-process"
        and type(owner.get("pid")) is int
        and owner["pid"] > 0)
    sponsor_log = sponsor.get("log")
    sponsor_identity_valid = bool(
        type(sponsor_pid) is int
        and sponsor_pid > 0
        and owner_valid
        and sponsor_pid != owner["pid"]
        and isinstance(sponsor_log, str)
        and sponsor_log
        and ("/" + sponsor_log.replace("\\", "/").lstrip("/")).endswith(
            "/logs/zen-sponsor.log"))
    version_valid = False
    try:
        version_path = Path(str(version.get("path", "")))
        version_valid = (
            version_path.is_file()
            and expected_zen is not None
            and version_path.resolve(strict=False)
            == expected_zen.resolve(strict=False)
            and version.get("size") == version_path.stat().st_size
            and version.get("sha256") == _sha256_file(version_path))
    except OSError:
        version_valid = False
    expected_sponsor_command = None
    if owner_valid and isinstance(version.get("path"), str):
        expected_sponsor_command = _canonical_zen_sponsor_command(
            version["path"],
            path.parent.parent / "Saved" / f"GuardedZenSponsor-{owner['pid']}",
            owner["pid"])
    sponsor_command_valid = (
        path.name == "zen-lifetime.json"
        and path.parent.name == "receipts"
        and sponsor_command == expected_sponsor_command)
    preflight_valid = (
        isinstance(endpoint_preflight, dict)
        and endpoint_preflight.get("host") == "127.0.0.1"
        and endpoint_preflight.get("port") == ZEN_SPONSOR_PORT
        and endpoint_preflight.get("exclusive_bind_succeeded") is True
        and isinstance(endpoint_preflight.get("checked_utc"), str))
    monitor_valid = (
        isinstance(monitor, dict)
        and monitor.get("interval_seconds") == ZEN_MONITOR_INTERVAL_SECONDS
        and type(monitor.get("sample_count")) is int
        and monitor["sample_count"] > 0
        and isinstance(monitor.get("started_utc"), str)
        and isinstance(monitor.get("finished_utc"), str)
        and monitor.get("violation") is None)
    chronology_valid = False
    try:
        chronology = [datetime.fromisoformat(str(value)) for value in (
            endpoint_preflight.get("checked_utc"),
            health.get("checked_utc"),
            monitor.get("started_utc"),
            monitor.get("finished_utc"))]
        chronology_valid = bool(
            all(value.tzinfo is not None for value in chronology)
            and chronology == sorted(chronology))
    except (TypeError, ValueError):
        chronology_valid = False
    commands = receipt.get("unreal_commands")
    stripped_ddc_overrides = receipt.get(
        "stripped_ddc_environment_overrides")
    environment_proof_valid = bool(
        isinstance(stripped_ddc_overrides, list)
        and all(isinstance(value, str) and value
                for value in stripped_ddc_overrides)
        and stripped_ddc_overrides == sorted(set(stripped_ddc_overrides))
        and all(value.casefold().startswith("ue-")
                and "datacache" in value.casefold()
                for value in stripped_ddc_overrides))
    command_stages = (
        "import", "finalize-stripped-fractal-catalog",
        "generate-neutral-map-shells", "cook")
    command_kinds = ("editor", "editor", "editor", "cook")
    command_logs_valid = isinstance(commands, list) and len(commands) == 4
    if command_logs_valid:
        for command_row, expected_stage, expected_kind in zip(
                commands, command_stages, command_kinds):
            try:
                command_path = Path(command_row["log"])
                command = command_row["command"]
                command_logs_valid = bool(
                    isinstance(command_row, dict)
                    and command_row.get("stage") == expected_stage
                    and command_row.get("kind") == expected_kind
                    and isinstance(command, list)
                    and command
                    and command_path.is_file()
                    and command_path == path.parent.parent / "logs" / (
                        expected_stage + ".log")
                    and command_row.get("log_sha256")
                    == _sha256_file(command_path)
                    and type(command_row.get("exit_code")) is int
                    and command_row.get("exit_code") == 0
                    and command_row.get(
                        "stripped_ddc_environment_overrides")
                    == stripped_ddc_overrides)
                if not command_logs_valid:
                    break
                _validate_guarded_unreal_command(
                    command, expected_kind, platform_name)
                command_executable = Path(str(command[0])).resolve(
                    strict=False)
                if expected_kind == "editor":
                    expected_script = iso / "Tools" / "import" / {
                        "import": "import_all.py",
                        "finalize-stripped-fractal-catalog":
                            "finalize_stripped_fractal_catalog.py",
                        "generate-neutral-map-shells":
                            "generate_neutral_map_shells.py",
                    }[expected_stage]
                    run_tokens = [str(value) for value in command
                                  if str(value).casefold().startswith("-run=")]
                    script_tokens = [str(value) for value in command
                                     if str(value).casefold().startswith(
                                         "-script=")]
                    expected_script_prefix = (
                        "-script=" + _quote_script_value(expected_script))
                    script_valid = bool(
                        len(script_tokens) == 1
                        and script_tokens[0].casefold().count("-script=") == 1
                        and (script_tokens[0] == expected_script_prefix
                             if expected_stage
                             == "finalize-stripped-fractal-catalog"
                             else script_tokens[0].startswith(
                                 expected_script_prefix + " ")))
                    command_logs_valid = bool(
                        expected_editor is not None
                        and command_executable
                        == expected_editor.resolve(strict=False)
                        and len(command) > 1
                        and Path(str(command[1])).resolve(strict=False)
                        == (iso / "WoT.uproject").resolve(strict=False)
                        and run_tokens == ["-run=pythonscript"]
                        and script_valid
                        and not any(str(value).casefold().startswith(
                            "-project=") for value in command))
                else:
                    project_tokens = [str(value) for value in command
                                      if str(value).casefold().startswith(
                                          "-project=")]
                    maps = (cook_stamp or {}).get("maps", [])
                    required_cook_tokens = {
                        f"-platform={platform_name}",
                        f"-clientconfig={GUARDED_CONFIGURATION}",
                        "-build", "-cook", "-stage", "-pak",
                        f"-stagingdirectory={iso / 'staged'}",
                        "-map=" + "+".join(maps),
                    }
                    command_logs_valid = bool(
                        expected_runuat is not None
                        and command_executable
                        == expected_runuat.resolve(strict=False)
                        and len(command) > 1
                        and command[1] == "BuildCookRun"
                        and project_tokens
                        == [f"-project={iso / 'WoT.uproject'}"]
                        and required_cook_tokens.issubset(set(command)))
                if not command_logs_valid:
                    break
                _validate_guarded_unreal_log(
                    command_path.read_text(
                        encoding="utf-8", errors="replace"),
                    expected_stage)
            except (BuildFailure, KeyError, OSError, TypeError):
                command_logs_valid = False
                break
    if (row.get("sha256") != _sha256_file(path)
            or not exact_schema_valid
            or receipt.get("format") != ZEN_RECEIPT_FORMAT
            or receipt.get("mechanism") != "pipeline-sponsored-zen-v3"
            or receipt.get("unreal_isolation_switches")
            != ["-ddc=(ZenLocal)", ZEN_NO_AUTO_LAUNCH]
            or receipt.get("environment_policy")
            != "strip-all-ue-datacache-overrides-v1"
            or not environment_proof_valid
            or receipt.get("endpoint") != ZEN_SPONSOR_ENDPOINT
            or not engine_anchor_valid
            or not owner_valid
            or not sponsor_identity_valid
            or not version_valid
            or not sponsor_command_valid
            or not preflight_valid
            or health.get("url")
            != ZEN_SPONSOR_ENDPOINT + "/health/ready"
            or health.get("status_code") != 200
            or re.fullmatch(r"[0-9a-f]{64}",
                            str(health.get("body_sha256", ""))) is None
            or not process_proof_valid
            or not monitor_valid
            or not chronology_valid
            or not command_logs_valid
            or cleanup.get("action") != "stop-pipeline-sponsor"
            or cleanup.get("target_pid") != sponsor_pid
            or not isinstance(cleanup.get("signal"), str)
            or type(cleanup.get("forced")) is not bool
            or type(cleanup.get("wait_return_code")) is not int
            or cleanup.get("verified_stopped") is not True
            or cleanup.get("data_directory_removed") is not True
            or receipt.get("lifecycle_status") != "success"):
        raise BuildFailure(
            "guarded Zen lifetime receipt is not a clean sponsored run")
    return {
        "receipt_sha256": row["sha256"],
        "receipt": receipt,
    }


def _git_commit(root: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--short=9", "HEAD"],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise BuildFailure(f"cannot resolve the release commit: {result.stderr.strip()}")
    return result.stdout.strip()


def _release_asset_name(platform_name: str, commit: str,
                        guarded: bool = False,
                        configuration: str = GUARDED_CONFIGURATION) -> str:
    # Mirrors release_formats.guarded_install_zip_asset: the funnel's Test zip
    # keeps the historical name, the release zip names its configuration.
    flavor = "stripped"
    if guarded:
        flavor = ("guarded-shipping" if configuration == SHIPPING_CONFIGURATION
                  else "guarded")
    return f"WoT-{platform_name}-{flavor}-{commit}.zip"


def _canonical_release_bytes(value: object) -> bytes:
    """The one canonical serialization of a release manifest.

    Sorted keys, minimal separators, ASCII-only, no trailing newline: the same
    rules the native canonical-JSON writer must reproduce byte-for-byte.
    """
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("utf-8")


def _release_manifest(*, asset_name: str, sha256: str, size: int,
                      platform: str, commit: str, audit_stamp: dict | None) -> dict:
    # The manifest is the release's provenance contract: an asset may only be
    # published from a staged tree whose absence audit PASSED, and it pins the
    # payload-cache root hash that audit ran against.
    if not audit_stamp or audit_stamp.get("verdict") != "pass":
        raise BuildFailure(
            "release packaging requires a passing absence-audit stamp")
    return {
        "format": "wot-stripped-release-v1",
        "asset": asset_name,
        "sha256": sha256,
        "size": size,
        "platform": platform,
        "git_commit": commit,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "audit": {
            "verdict": audit_stamp["verdict"],
            "cache_sha256": audit_stamp.get("cache_sha256"),
            "container_count": audit_stamp.get("container_count"),
        },
    }


def _guarded_release_manifest(*, asset_name: str, sha256: str, size: int,
                              platform: str, commit: str,
                              audit_stamp: dict | None,
                              cook_stamp: dict | None,
                              iso: Path,
                              source_snapshot: dict,
                              configuration: str = GUARDED_CONFIGURATION,
                              build_stamp: dict | None = None,
                              test_twin: dict | None = None,
                              dmg: dict | None = None,
                              release_sequence: int = 0) -> dict:
    """The guarded release.json.

    For Test the cook stamp is also the build stamp. For Shipping the build
    stamp is the shipping-stage stamp (its receipts, products and container
    inventory) while the Test cook stamp still supplies the Zen-sponsored
    import+cook proof its content came from, and test_twin binds the Test
    release the funnel proves.
    """
    if build_stamp is None:
        build_stamp = cook_stamp
    manifest = _release_manifest(
        asset_name=asset_name, sha256=sha256, size=size,
        platform=platform, commit=commit, audit_stamp=audit_stamp)
    manifest["release_sequence"] = release_sequence
    receipts = _guarded_receipt_bindings(build_stamp, platform, configuration)
    products = _guarded_product_binding(
        build_stamp, iso, platform, configuration)
    zen = _guarded_zen_binding(cook_stamp, iso, platform)
    cooked_inventory = _guarded_cooked_inventory_binding(
        build_stamp, platform, configuration)
    neutral_support_authority = _validate_neutral_support_receipt(
        (build_stamp or {}).get("neutral_support_authority"))
    staged_tree = (audit_stamp or {}).get("staged_tree")
    if not isinstance(staged_tree, dict):
        raise BuildFailure("guarded manifest requires staged-tree audit identity")
    manifest["audit"].update({
        "staged_tree_sha256": staged_tree.get("tree_sha256"),
        "staged_tree_receipt_sha256": staged_tree.get("sha256"),
    })
    manifest.update({
        "format": "wot-guarded-release-v1",
        "configuration": configuration,
        "source_snapshot": source_snapshot,
        "engine_identity": receipts["WoT"]["compiled_receipt"]["engine"],
        "target_receipts": receipts,
        "products": products,
        "cooked_container_inventory": cooked_inventory,
        "neutral_support_authority": neutral_support_authority,
        "zen_lifetime": zen,
    })
    if platform == "Mac":
        if not isinstance(dmg, dict):
            raise BuildFailure("a guarded Mac release requires its first-install DMG")
        manifest["dmg"] = dmg
    elif dmg is not None:
        raise BuildFailure("a non-Mac release cannot carry a Mac DMG binding")
    if configuration == SHIPPING_CONFIGURATION:
        test_receipts = _guarded_receipt_bindings(cook_stamp, platform)
        if (test_receipts["WoT"]["compiled_receipt"].get("engine")
                != manifest["engine_identity"]):
            raise BuildFailure(
                "the Shipping and Test clients name different pinned engines")
        if not isinstance(test_twin, dict):
            raise BuildFailure(
                "a guarded Shipping release requires its test_twin binding")
        stage = _shipping_stage_binding(build_stamp, iso, platform)
        manifest["shipping_stage"] = stage
        manifest["test_twin"] = test_twin
    return manifest


def _guarded_mac_staged_app(
        iso: Path, configuration: str = GUARDED_CONFIGURATION) -> Path:
    staged = iso / STAGING_DIRECTORIES[configuration] / "Mac"
    normalized = staged / "WoT.app"
    if normalized.is_dir():
        return normalized
    # UAT names the staged bundle after the decorated executable
    # (ApplePlatform.Automation.cs:213-216; UEBuildTarget.cs:5486-5492).
    candidates = sorted(staged.glob(f"WoT-Mac-{configuration}.app"))
    if len(candidates) != 1 or not candidates[0].is_dir():
        raise BuildFailure(
            f"guarded Mac package requires exactly one staged "
            f"WoT-Mac-{configuration}.app")
    # The proven Test cook emits WoT-Mac-Test.app. Renaming the outer bundle is
    # the package-layout normalization; its Info.plist continues to name the
    # config-suffixed Mach-O inside Contents/MacOS.
    candidates[0].rename(normalized)
    return normalized


def _guarded_main_artifact_path(staged_root: Path,
                                platform_name: str,
                                configuration: str = GUARDED_CONFIGURATION
                                ) -> Path:
    if platform_name == "Mac":
        return staged_root / "WoT.app"
    if platform_name == "Win64":
        # The guarded Win64 cook/stage passes -NoBootstrapExe, so no root
        # launcher shim exists BY DESIGN; the artifact's entry point is the
        # decorated client (<StageDir>/WoT/Binaries/Win64/WoT-Win64-<Config>.exe,
        # DeploymentContext.cs:500, 815-818), which is also exactly what the
        # funnel (Test) and the Shipping smoke resolve.
        return (staged_root / "WoT" / "Binaries" / "Win64"
                / f"WoT-Win64-{configuration}.exe")
    raise BuildFailure(f"guarded artifact layout is undefined for {platform_name}")


def _require_guarded_staged_layout(staged_root: Path,
                                   platform_name: str,
                                   configuration: str = GUARDED_CONFIGURATION
                                   ) -> Path:
    main = _guarded_main_artifact_path(
        staged_root, platform_name, configuration)
    if not main.exists():
        raise BuildFailure(f"guarded staged main artifact is missing: {main}")
    return main


PRODUCTION_DESCRIPTOR_RELATIVE = PurePosixPath(
    "Build/WoTRelease/cache-v3-production-descriptor.json")
RELEASE_NOTES_RELATIVE = PurePosixPath(
    "Build/WoTRelease/ReleaseNotes.txt")
RELEASE_IDENTITY_RELATIVE = PurePosixPath(
    "Build/WoTRelease/release-identity.json")
RELEASE_VERSION_RELATIVE = PurePosixPath(
    "Build/WoTRelease/release-version.json")
PRODUCTION_DESCRIPTOR_PIN_SOURCE = PurePosixPath(
    "Source/WoTRetailReader/Private/CacheBuilder.cpp")


def _production_descriptor_pin(iso: Path) -> str:
    source = iso / Path(*PRODUCTION_DESCRIPTOR_PIN_SOURCE.parts)
    try:
        text = source.read_text(encoding="utf-8")
    except OSError as exc:
        raise BuildFailure(f"cannot read the descriptor pin source {source}: "
                           f"{exc}") from exc
    match = re.search(
        r"WoTCacheExpectedProductionDescriptorSha256\[\]\s*=\s*"
        r"TEXT\(\"([0-9a-f]{64})\"\)", text)
    if not match:
        raise BuildFailure(f"no production descriptor pin in {source}")
    return match.group(1)


def _require_staged_production_descriptor(staged_root: Path,
                                          iso: Path) -> Path:
    """The packaged game builds its content cache only from the descriptor it
    ships with (CacheBuilder.cpp; the compile-time source-path fallback is
    compiled out of Test/Shipping). Builds without it worked only
    on the build machine, so the package refuses to exist without it: exactly
    one staged copy, byte-identical to the ISO's, matching the compiled pin."""
    suffix = ("WoT",) + PRODUCTION_DESCRIPTOR_RELATIVE.parts
    matches = [path for path in staged_root.rglob(PRODUCTION_DESCRIPTOR_RELATIVE.name)
               if path.is_file() and path.parts[-len(suffix):] == suffix]
    # Mac staging leaves a loose <Stage>/Mac/WoT tree beside the bundle it
    # assembles; only WoT.app is archived, so its copy is the shipped one (the
    # same rule as _shipped_staged_containers). Two bundled copies still fail.
    bundled = [path for path in matches
               if any(part.casefold().endswith(".app")
                      for part in path.relative_to(staged_root).parts[:-1])]
    if bundled:
        matches = bundled
    if len(matches) != 1:
        raise BuildFailure(
            "the staged game must carry exactly one WoT/"
            f"{PRODUCTION_DESCRIPTOR_RELATIVE} (found {len(matches)}); a player's "
            "first run cannot build its content cache without it")
    staged = matches[0]
    source = iso / Path(*PRODUCTION_DESCRIPTOR_RELATIVE.parts)
    if not source.is_file() or staged.read_bytes() != source.read_bytes():
        raise BuildFailure(
            f"the staged production descriptor {staged} is not the ISO's {source}")
    pin = _production_descriptor_pin(iso)
    if _sha256_file(staged) != pin:
        raise BuildFailure(
            "the staged production descriptor does not match the compiled pin "
            f"{pin} ({PRODUCTION_DESCRIPTOR_PIN_SOURCE})")
    return staged


def _require_release_notes_absent(staged_root: Path) -> None:
    """Refuse both bundled and loose staging copies of retired local notes."""
    suffix = ("WoT",) + RELEASE_NOTES_RELATIVE.parts
    matches = [path for path in staged_root.rglob(RELEASE_NOTES_RELATIVE.name)
               if path.is_file() and path.parts[-len(suffix):] == suffix]
    if matches:
        raise BuildFailure(
            "the staged game must not carry retired local release notes: "
            + ", ".join(str(path) for path in matches))


def _require_staged_release_identity(staged_root: Path, iso: Path) -> Path:
    """Require the one shipped identity to equal the stamped ISO source."""
    suffix = ("WoT",) + RELEASE_IDENTITY_RELATIVE.parts
    matches = [path for path in staged_root.rglob(RELEASE_IDENTITY_RELATIVE.name)
               if path.is_file() and path.parts[-len(suffix):] == suffix]
    bundled = [path for path in matches
               if any(part.casefold().endswith(".app")
                      for part in path.relative_to(staged_root).parts[:-1])]
    if bundled:
        matches = bundled
    if len(matches) != 1:
        raise BuildFailure(
            "the staged game must carry exactly one WoT/"
            f"{RELEASE_IDENTITY_RELATIVE} (found {len(matches)})")
    staged = matches[0]
    source = iso / Path(*RELEASE_IDENTITY_RELATIVE.parts)
    if not source.is_file() or staged.read_bytes() != source.read_bytes():
        raise BuildFailure(
            f"the staged release identity {staged} is not the ISO's {source}")
    try:
        identity = release_formats.parse_release_identity(
            staged.read_bytes(), str(staged))
    except release_formats.FormatError as exc:
        raise BuildFailure(f"invalid staged release identity: {exc}") from exc
    if not identity.release_commit:
        raise BuildFailure("the staged release identity has empty release_commit")
    seed = _read_stamp(iso, "seed") or {}
    if identity.release_commit != seed.get("release_commit"):
        raise BuildFailure(
            "the staged release identity differs from the cooked seed commit")
    if ("release_sequence" in seed
            and identity.release_sequence != seed.get("release_sequence")):
        raise BuildFailure(
            "the staged release identity differs from the cooked seed sequence")
    return staged


def _require_staged_release_version(staged_root: Path, iso: Path) -> Path:
    """Require the one shipped human version to equal the stamped ISO source."""
    suffix = ("WoT",) + RELEASE_VERSION_RELATIVE.parts
    matches = [path for path in staged_root.rglob(RELEASE_VERSION_RELATIVE.name)
               if path.is_file() and path.parts[-len(suffix):] == suffix]
    bundled = [path for path in matches
               if any(part.casefold().endswith(".app")
                      for part in path.relative_to(staged_root).parts[:-1])]
    if bundled:
        matches = bundled
    if len(matches) != 1:
        raise BuildFailure(
            "the staged game must carry exactly one WoT/"
            f"{RELEASE_VERSION_RELATIVE} (found {len(matches)})")
    staged = matches[0]
    source = iso / Path(*RELEASE_VERSION_RELATIVE.parts)
    if not source.is_file() or staged.read_bytes() != source.read_bytes():
        raise BuildFailure(
            f"the staged release version {staged} is not the ISO's {source}")
    try:
        human_version = release_formats.parse_release_version(
            staged.read_bytes(), str(staged))
    except release_formats.FormatError as exc:
        raise BuildFailure(f"invalid staged release version: {exc}") from exc
    seed_human_version = (_read_stamp(iso, "seed") or {}).get(
        "human_version", "")
    if human_version != seed_human_version:
        raise BuildFailure(
            "the staged release version differs from the cooked seed "
            "human_version")
    return staged


def _escaping_rpaths(binary: Path, rpaths: Sequence[str], app: Path) -> list[str]:
    """Rpaths that resolve outside the install directory holding the bundle.

    UBT also links build-tree rpaths (@loader_path/../../<engine checkout>/...)
    that point back into the build machine. UE's own @executable_path/../../../
    (the folder beside WoT.app) stays: it resolves to the install directory.
    """
    install_root = os.path.normpath(str(app.parent))
    executable_dir = app / "Contents" / "MacOS"
    escaping = []
    for rpath in rpaths:
        if rpath.startswith("@loader_path"):
            resolved = str(binary.parent) + rpath[len("@loader_path"):]
        elif rpath.startswith("@executable_path"):
            resolved = str(executable_dir) + rpath[len("@executable_path"):]
        elif rpath.startswith("@"):
            continue
        elif rpath.startswith(("/System/", "/usr/lib/")):
            # Absolute system locations (Apple's prebuilt libmetalirconverter
            # carries /System/Library/Frameworks/D3DMetal.framework/Resources)
            # are the same on every Mac, not a build-machine leak.
            continue
        elif posixpath.isabs(rpath):
            # Mach-O rpaths are POSIX paths whatever host runs this check:
            # Windows' os.path.isabs (Python 3.13+) no longer treats a
            # drive-less "/..." as absolute, which would wave a leak through.
            escaping.append(rpath)
            continue
        else:
            continue
        resolved = os.path.normpath(resolved)
        if resolved != install_root and not resolved.startswith(install_root + os.sep):
            escaping.append(rpath)
    return escaping


def _mach_o_rpaths(binary: Path) -> list[str]:
    result = subprocess.run(["otool", "-l", str(binary)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise BuildFailure(f"otool -l failed for {binary}: {result.stderr.strip()}")
    rpaths = []
    lines = result.stdout.splitlines()
    for index, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for follow in lines[index + 1:index + 4]:
                match = re.match(r"\s*path (.*) \(offset \d+\)$", follow)
                if match:
                    rpaths.append(match.group(1))
                    break
    return rpaths


def _guarded_mac_binaries(app: Path) -> list[Path]:
    return mac_signing.mach_o_binaries(app)


def _strip_build_tree_rpaths(app: Path) -> int:
    """Delete rpaths that point outside the install and re-sign each edited binary.

    install_name_tool invalidates the binary's signature. The bundle re-sign
    (codesign --deep) only reaches nested code in the standard bundle folders,
    so a third-party dylib under Contents/UE would stay invalid and dyld kills
    the process (SIGKILL, Code Signature Invalid) when it maps it.
    """
    removed = 0
    for binary in _guarded_mac_binaries(app):
        # otool lists a universal binary's load commands once per slice, and one
        # -delete_rpath removes the path from every slice; delete each path once.
        escaping = _escaping_rpaths(binary, _mach_o_rpaths(binary), app)
        for rpath in dict.fromkeys(escaping):
            result = subprocess.run(
                ["install_name_tool", "-delete_rpath", rpath, str(binary)],
                capture_output=True, text=True)
            if result.returncode != 0:
                raise BuildFailure(
                    f"cannot delete rpath {rpath} from {binary}: "
                    f"{result.stderr.strip()}")
            removed += 1
        if escaping:
            for command in (["codesign", "--force", "--sign", "-", str(binary)],
                            ["codesign", "--verify", "--strict", str(binary)]):
                result = subprocess.run(command, capture_output=True, text=True)
                if result.returncode != 0:
                    raise BuildFailure(
                        f"cannot re-sign {binary} after deleting rpaths: "
                        f"{result.stderr.strip()}")
    return removed


def _require_no_build_machine_paths(app: Path, build_roots: Sequence[Path]) -> None:
    """Gate: no shipped binary may reference the build machine's trees."""
    # Mach-O strings are POSIX paths; as_posix keeps the needle host-neutral.
    needles = [Path(os.path.normpath(str(root))).as_posix().encode() + b"/"
               for root in build_roots]
    for binary in _guarded_mac_binaries(app):
        leftover = _escaping_rpaths(binary, _mach_o_rpaths(binary), app)
        if leftover:
            raise BuildFailure(
                f"{binary.relative_to(app)} keeps build-tree rpaths {leftover}")
        data = binary.read_bytes()
        for needle in needles:
            if needle in data:
                raise BuildFailure(
                    f"{binary.relative_to(app)} embeds the build path "
                    f"{needle.decode(errors='replace')}")


def _mac_signing_runner(command: Sequence[str]) -> subprocess.CompletedProcess:
    return subprocess.run(list(command), capture_output=True, text=True)


def _prepare_guarded_stage(args, iso: Path,
                           configuration: str = GUARDED_CONFIGURATION,
                           signing_facts: dict | None = None) -> Path:
    if args.platform == "Mac":
        app = _guarded_mac_staged_app(iso, configuration)
        staged_root = app.parent
        entitlements = iso / "Build" / "Mac" / "Resources" / \
            "Sandbox.Server.entitlements"
        if not entitlements.is_file():
            raise BuildFailure(
                f"guarded Mac entitlements are missing: {entitlements}")
        _strip_build_tree_rpaths(app)
        # Only the root's path is needed (no editor probe), so a fixture or a
        # host without UE still runs the gate against the ISO.
        build_roots = [iso]
        if args.engine is not None:
            build_roots.append(Path(args.engine).expanduser())
        _require_no_build_machine_paths(app, build_roots)
        if host_platform.system() != "Darwin":
            raise BuildFailure("guarded Mac packaging must run on the Mac host")
        identity = local_value("mac_signing_identity", required=False)
        try:
            notary_auth = mac_signing.notary_auth_args(local_value)
            facts = mac_signing.sign_app(
                app, entitlements, identity, _mac_signing_runner)
            submission_id = None
            stapled = False
            if identity is not None and notary_auth is not None:
                with tempfile.TemporaryDirectory(
                        prefix="wot-app-notary-") as temporary:
                    notarization = mac_signing.notarize(
                        app, notary_auth, _mac_signing_runner, Path(temporary))
                submission_id = notarization["submission_id"]
                mac_signing.staple(app, _mac_signing_runner)
                stapled = True
        except mac_signing.MacSigningError as exc:
            raise BuildFailure(f"Mac guarded signing failed: {exc}") from exc
        facts.update({
            "notarization_submission_id": submission_id,
            "stapled": stapled,
        })
        if signing_facts is not None:
            signing_facts.update(facts)
    elif args.platform == "Win64":
        staged_root = iso / STAGING_DIRECTORIES[configuration] / "Windows"
    else:
        raise BuildFailure(f"guarded packaging does not support {args.platform}")
    _require_guarded_staged_layout(staged_root, args.platform, configuration)
    return staged_root


def _guarded_staged_product_paths(
        staged_root: Path, platform_name: str,
        configuration: str = GUARDED_CONFIGURATION) -> dict[str, Path]:
    _require_guarded_staged_layout(
        staged_root, platform_name, configuration)
    if platform_name == "Mac":
        plist_path = staged_root / "WoT.app" / "Contents" / "Info.plist"
        try:
            with plist_path.open("rb") as handle:
                executable_leaf = plistlib.load(handle).get("CFBundleExecutable")
        except (OSError, plistlib.InvalidFileException) as exc:
            raise BuildFailure(
                f"cannot read guarded app executable identity: {exc}") from exc
        if (not isinstance(executable_leaf, str) or not executable_leaf
                or "/" in executable_leaf or "\\" in executable_leaf):
            raise BuildFailure(
                "guarded app has no safe CFBundleExecutable")
        game = (staged_root / "WoT.app" / "Contents" / "MacOS" /
                executable_leaf)
    elif platform_name == "Win64":
        game = _guarded_main_artifact_path(
            staged_root, platform_name, configuration)
    else:
        raise BuildFailure(
            f"guarded products are undefined for {platform_name}")
    if not game.is_file():
        raise BuildFailure("guarded final product binaries are missing")
    if (platform_name == "Mac"
            and game.name != f"WoT-Mac-{configuration}"):
        raise BuildFailure(
            f"guarded app executable {game.name!r} is not the "
            f"{configuration} image")
    return {"WoT": game}


def _write_guarded_product_receipt(
        iso: Path, platform_name: str, staged_root: Path,
        target_receipts: dict,
        configuration: str = GUARDED_CONFIGURATION,
        signing_facts: dict | None = None) -> dict:
    staged_base = iso / STAGING_DIRECTORIES[configuration]
    products = {}
    for target, path in _guarded_staged_product_paths(
            staged_root, platform_name, configuration).items():
        products[target] = {
            "path": path.relative_to(staged_base).as_posix(),
            "size": path.stat().st_size,
            "sha256": _sha256_file(path),
            "compiled_receipt_sha256":
                target_receipts[target]["compiled_sha256"],
            "engine_receipt_sha256":
                target_receipts[target]["engine_sha256"],
        }
    receipt = {
        "format": PRODUCT_RECEIPT_FORMAT,
        "platform": platform_name,
        "configuration": configuration,
        "stage_root": staged_root.relative_to(staged_base).as_posix(),
        "products": products,
    }
    if platform_name == "Mac" and signing_facts is not None:
        receipt.update(signing_facts)
    suffix = ("" if configuration == GUARDED_CONFIGURATION
              else "-" + configuration)
    path = iso / "receipts" / f"{platform_name}{suffix}-products.json"
    _write_json(path, receipt)
    return {
        "path": str(path),
        "sha256": _sha256_file(path),
        "receipt": receipt,
    }


def _guarded_product_binding(
        cook_stamp: dict | None, iso: Path, platform_name: str,
        configuration: str = GUARDED_CONFIGURATION) -> dict:
    row = (cook_stamp or {}).get("product_receipt")
    if not isinstance(row, dict):
        raise BuildFailure("guarded cook has no final product receipt")
    path = Path(str(row.get("path", "")))
    receipt = _read_json_object(path, "guarded product receipt")
    if (row.get("sha256") != _sha256_file(path)
            or receipt.get("format") != PRODUCT_RECEIPT_FORMAT
            or receipt.get("platform") != platform_name
            or receipt.get("configuration") != configuration
            or set(receipt.get("products", {}))
                != set(GUARDED_RECEIPT_TARGETS)):
        raise BuildFailure("guarded product receipt identity is wrong")
    receipt_bindings = _guarded_receipt_bindings(
        cook_stamp, platform_name, configuration)
    try:
        signing_facts = release_formats.validate_mac_signing_receipt(
            receipt, "guarded product receipt")
    except release_formats.FormatError as exc:
        raise BuildFailure(str(exc)) from exc
    if platform_name != "Mac" and signing_facts is not None:
        raise BuildFailure("non-Mac product receipt has Mac signing facts")
    staged_base = iso / STAGING_DIRECTORIES[configuration]
    stage_root = staged_base / str(receipt.get("stage_root", ""))
    expected_paths = _guarded_staged_product_paths(
        stage_root, platform_name, configuration)
    for target, product in receipt["products"].items():
        if not isinstance(product, dict):
            raise BuildFailure(f"guarded {target} product row is malformed")
        product_path = staged_base / str(product.get("path", ""))
        try:
            product_path.resolve(strict=True).relative_to(
                staged_base.resolve(strict=True))
        except (OSError, ValueError) as exc:
            raise BuildFailure(
                f"guarded {target} product escapes the staged tree") from exc
        if product_path != expected_paths[target]:
            raise BuildFailure(
                f"guarded {target} product path is not the platform path")
        if (product.get("size") != product_path.stat().st_size
                or product.get("sha256") != _sha256_file(product_path)
                or product.get("compiled_receipt_sha256")
                    != receipt_bindings[target]["compiled_receipt_sha256"]
                or product.get("engine_receipt_sha256")
                    != receipt_bindings[target]["engine_receipt_sha256"]):
            raise BuildFailure(
                f"guarded {target} product changed after its final build")
    return {
        "receipt_sha256": row["sha256"],
        "receipt": receipt,
    }


def _require_no_appledouble_entries(asset_path: Path) -> None:
    """The Mac zip carries no __MACOSX or ._* member (release_formats rule)."""
    import zipfile
    try:
        with zipfile.ZipFile(asset_path) as archive:
            names = archive.namelist()
    except (OSError, zipfile.BadZipFile) as exc:
        raise BuildFailure(f"Mac zip is unreadable: {asset_path}: {exc}") from exc
    for name in names:
        try:
            release_formats.reject_appledouble_path(name.rstrip("/"))
        except release_formats.FormatError as exc:
            raise BuildFailure(f"Mac zip {asset_path.name}: {exc}") from exc


def _require_seed_matches_head(iso: Path) -> tuple[str, dict]:
    """The ISO's seed must name HEAD and the current Source/Tools snapshot."""
    commit = _git_commit(REPO)
    seed_stamp = _read_stamp(iso, "seed") or {}
    seed_commit = seed_stamp.get("release_commit")
    if seed_commit != commit:
        raise BuildFailure(
            "release commit differs from cooked seed: "
            f"seed={seed_commit or 'missing'}, HEAD={commit}")
    source_snapshot = seed_stamp.get("source_snapshot")
    if (not isinstance(source_snapshot, dict)
            or source_snapshot != _source_snapshot(REPO)):
        raise BuildFailure(
            "release Source/Tools differ from the cooked seed snapshot")
    return commit, source_snapshot


def _project_package_census(paths: Sequence[str]) -> list[str]:
    """The project package paths a container census carries (cf. the
    allowlist in _validate_guarded_cooked_paths); engine/config entries are
    excluded because only project packages are the cooked WoT content."""
    project_prefixes = ("../../../wot/content/", "../../../wot/plugins/")
    package_extensions = {".uasset", ".umap", ".uexp", ".ubulk", ".uptnl"}
    return sorted({
        path.replace("\\", "/") for path in paths
        if path.replace("\\", "/").casefold().startswith(project_prefixes)
        and Path(path).suffix.casefold() in package_extensions
    })


def _cooked_twin_comparison(test_inventory: dict,
                            shipping_inventory: dict) -> dict:
    """Prove the Shipping containers carry the Test cook's exact packages.

    Both stages read the SAME Saved/Cooked/<CookPlatform> directory
    (CopyBuildToStagingDirectory.Automation.cs:1366-1372), so the project
    package census must be identical. Container BYTES are recorded, not
    required: UnrealPak's inputs are configuration-independent unless a
    plugin/runtime dependency is configuration-conditional
    (UEBuildTarget.cs:2512-2549; PluginReferenceDescriptor.cs:383-395), and
    byte determinism of the IoStore writer is not something this pipeline
    has proven.
    """
    test_census = _project_package_census(test_inventory.get("paths", []))
    shipping_census = _project_package_census(
        shipping_inventory.get("paths", []))
    if not test_census or test_census != shipping_census:
        raise BuildFailure(
            "Shipping containers do not carry the Test cook's exact project "
            "package census")
    test_rows = {row.get("path"): row
                 for row in test_inventory.get("containers", [])}
    shipping_rows = {row.get("path"): row
                     for row in shipping_inventory.get("containers", [])}
    if set(test_rows) != set(shipping_rows):
        raise BuildFailure(
            "Shipping staged a different container set than the Test cook: "
            f"test={sorted(map(str, test_rows))} "
            f"shipping={sorted(map(str, shipping_rows))}")
    identical = sorted(
        str(path) for path, row in test_rows.items()
        if (row.get("sha256"), row.get("size"))
        == (shipping_rows[path].get("sha256"), shipping_rows[path].get("size")))
    return {
        "project_package_census_sha256": hashlib.sha256(
            _canonical_release_bytes(test_census)).hexdigest(),
        "project_package_count": len(test_census),
        "all_listed_paths_identical": (
            test_inventory.get("paths") == shipping_inventory.get("paths")),
        "container_bytes": {
            "identical": identical,
            "differs": sorted(
                str(path) for path in test_rows if str(path) not in identical),
        },
    }


def _test_twin_binding(iso: Path, platform_name: str, commit: str,
                       source_snapshot: dict, cook_stamp: dict | None,
                       shipping_stamp: dict | None,
                       release_sequence: int) -> dict:
    """Bind the Shipping release to THIS ISO's Test release (the funnel twin).

    The funnel receipt names the Test release.json it proved by SHA-256
    (run_first_run_funnel.py _run_acceptance); release_json_sha256 below is
    that same digest, so sign_release can join a Shipping release to the
    funnel run of its twin (release_formats.parse_test_twin).
    """
    package_stamp = _read_stamp(iso, "package")
    if not package_stamp or package_stamp.get("mode") != "guarded":
        raise BuildFailure(
            "shipping-package requires the guarded Test package (the funnel twin)")
    release_json = Path(str(package_stamp.get("manifest", "")))
    test_manifest = _read_json_object(release_json, "Test release manifest")
    release_json_sha256 = _sha256_file(release_json)
    asset = release_json.parent / str(test_manifest.get("asset", ""))
    zen = test_manifest.get("zen_lifetime")
    cooked = test_manifest.get("cooked_container_inventory")
    audit = test_manifest.get("audit")
    if (package_stamp.get("release_json_sha256") != release_json_sha256
            or test_manifest.get("format") != "wot-guarded-release-v1"
            or test_manifest.get("configuration") != GUARDED_CONFIGURATION
            or test_manifest.get("platform") != platform_name
            or test_manifest.get("git_commit") != commit
            or test_manifest.get("release_sequence") != release_sequence
            or test_manifest.get("source_snapshot") != source_snapshot
            or test_manifest.get("asset") != _release_asset_name(
                platform_name, commit, True, GUARDED_CONFIGURATION)
            or not asset.is_file()
            or test_manifest.get("sha256") != _sha256_file(asset)
            or test_manifest.get("size") != asset.stat().st_size
            or package_stamp.get("sha256") != test_manifest.get("sha256")
            or not isinstance(zen, dict) or not isinstance(cooked, dict)
            or not isinstance(audit, dict)):
        raise BuildFailure(
            "the Test release is not the intact same-commit guarded Test "
            "package of this ISO")
    try:
        human_version = release_formats.read_release_version_from_zip(
            asset, platform_name,
            release_formats.GUARDED_TREE_ROOTS[platform_name])
    except release_formats.FormatError as exc:
        raise BuildFailure(
            f"the Test release has no valid release version: {exc}") from exc
    test_inventory = _guarded_cooked_inventory_binding(
        cook_stamp, platform_name)
    zen_binding = _guarded_zen_binding(cook_stamp, iso, platform_name)
    audited_test_tree = _validate_staged_tree_receipt(
        iso, _read_stamp(iso, "audit"))
    if (cooked.get("receipt_sha256") != test_inventory["receipt_sha256"]
            or zen.get("receipt_sha256") != zen_binding["receipt_sha256"]
            or audit.get("staged_tree_sha256")
            != audited_test_tree.get("tree_sha256")):
        raise BuildFailure(
            "the Test release no longer matches this ISO's cook, Zen proof, "
            "or audited Test staged tree")
    shipping_inventory = _guarded_cooked_inventory_binding(
        shipping_stamp, platform_name, SHIPPING_CONFIGURATION)
    comparison = _cooked_twin_comparison(
        test_inventory["receipt"], shipping_inventory["receipt"])
    return {
        "format": TEST_TWIN_FORMAT,
        "configuration": GUARDED_CONFIGURATION,
        "asset": test_manifest["asset"],
        "asset_sha256": test_manifest["sha256"],
        "asset_size": test_manifest["size"],
        "release_json_sha256": release_json_sha256,
        "git_commit": commit,
        "human_version": human_version,
        "release_sequence": release_sequence,
        "source_snapshot_sha256": source_snapshot["sha256"],
        "staged_tree_sha256": audit["staged_tree_sha256"],
        "cooked_container_inventory_receipt_sha256":
            test_inventory["receipt_sha256"],
        "project_package_census_sha256":
            comparison["project_package_census_sha256"],
        "zen_lifetime_receipt_sha256": zen_binding["receipt_sha256"],
        "container_bytes": comparison["container_bytes"],
    }


def stage_package(args, iso: Path) -> str:
    return _stage_package(args, iso, GUARDED_CONFIGURATION)


def stage_shipping_package(args, iso: Path) -> str:
    """Package the guarded Shipping release beside its Test (funnel) twin."""
    if not _is_shipping_release(args):
        raise BuildFailure(
            "shipping-package requires --guarded --configuration Shipping")
    return _stage_package(args, iso, SHIPPING_CONFIGURATION)


def _guarded_dmg_binding(document: dict, dmg_path: Path, sidecar_path: Path) -> dict:
    """Bind the canonical cross-host sidecar and the image it describes."""
    sidecar_digest, sidecar_size = release_formats.sha256_file(sidecar_path)
    return {
        "background_sha256": document["background_sha256"],
        "installed_tree_root": document["installed_tree_root"],
        "name": document["name"],
        "notarized": document["notarized"],
        "sha256": document["sha256"],
        "size": document["size"],
        "verify_sidecar": {
            "name": sidecar_path.name,
            "sha256": sidecar_digest,
            "size": sidecar_size,
        },
        "volume_name": document["volume_name"],
    }


def _cached_guarded_dmg(release_dir: Path, stamp: dict, expected_name: str
                        ) -> dict | None:
    binding = stamp.get("dmg")
    if not isinstance(binding, dict) or binding.get("name") != expected_name:
        return None
    sidecar = binding.get("verify_sidecar")
    if not isinstance(sidecar, dict):
        return None
    dmg_path = release_dir / expected_name
    sidecar_path = release_dir / str(sidecar.get("name", ""))
    try:
        document = mac_dmg.verify_sidecar(dmg_path, sidecar_path)
        rebound = _guarded_dmg_binding(document, dmg_path, sidecar_path)
    except (OSError, mac_dmg.DmgError, KeyError):
        return None
    return rebound if rebound == binding else None


@contextlib.contextmanager
def _private_staged_snapshot(iso: Path, staged_root: Path,
                             audited_tree: dict):
    """Clone the audited staged tree once for every Mac package reader.

    On the release Mac this first asks APFS for copy-on-write clones and falls
    back to ditto when the filesystem cannot clone.  The non-Mac branch exists
    only so the stdlib packaging specs can exercise the same contract.
    """
    receipt_path = Path(str(audited_tree.get("path", "")))
    receipt = _read_json_object(receipt_path, "staged-tree audit receipt")
    with tempfile.TemporaryDirectory(
            prefix=PACKAGE_SNAPSHOT_PREFIX, dir=str(iso)) as temporary:
        private = Path(temporary)
        os.chmod(private, 0o700)
        _write_json(private / PACKAGE_SNAPSHOT_MARKER, {
            "format": PACKAGE_SNAPSHOT_FORMAT,
            "iso": str(iso.resolve()),
        })
        snapshot = private / "staged"
        if sys.platform == "darwin":
            copied = subprocess.run(
                ["cp", "-c", "-R", str(staged_root), str(snapshot)],
                capture_output=True, text=True)
            if copied.returncode != 0:
                if snapshot.exists():
                    shutil.rmtree(snapshot)
                copied = subprocess.run(
                    ["ditto", str(staged_root), str(snapshot)],
                    capture_output=True, text=True)
            if copied.returncode != 0:
                raise BuildFailure(
                    "cannot make the private staged-tree snapshot: "
                    f"{(copied.stderr or copied.stdout or '').strip()}")
        else:
            shutil.copytree(staged_root, snapshot, symlinks=True)
        snapshotted = _staged_tree_inventory(snapshot)
        receipt_inventory = dict(receipt)
        binary_audit = receipt_inventory.pop("binary_literal_audit", None)
        crash_audit = receipt_inventory.pop("crash_report_client_audit", None)
        if (not isinstance(binary_audit, dict)
                or not isinstance(crash_audit, dict)
                or snapshotted != receipt_inventory
                or snapshotted.get("tree_sha256") !=
                audited_tree.get("tree_sha256")):
            raise BuildFailure(
                "private packaging snapshot differs from the audited staged tree")
        yield snapshot


def _cleanup_stale_package_snapshots(iso: Path) -> int:
    """Remove only abandoned private snapshots proven to belong to this ISO."""
    expected = {
        "format": PACKAGE_SNAPSHOT_FORMAT,
        "iso": str(iso.resolve()),
    }
    removed = 0
    if not iso.is_dir():
        return removed
    for candidate in iso.glob(PACKAGE_SNAPSHOT_PREFIX + "*"):
        marker = candidate / PACKAGE_SNAPSHOT_MARKER
        if candidate.is_symlink() or not candidate.is_dir() \
                or marker.is_symlink() or not marker.is_file():
            continue
        try:
            document = json.loads(marker.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, ValueError):
            continue
        if document != expected:
            continue
        shutil.rmtree(candidate)
        removed += 1
    return removed


def _stage_package(args, iso: Path, configuration: str) -> str:
    guarded = _mode_is_guarded(args)
    shipping = guarded and configuration == SHIPPING_CONFIGURATION
    audit_step = "shipping-audit" if shipping else "audit"
    build_step = "shipping-stage" if shipping else "cook"
    package_step = "shipping-package" if shipping else "package"
    if guarded and args.platform == "Mac":
        _cleanup_stale_package_snapshots(iso)
    staging = STAGING_DIRECTORIES[configuration] if guarded else "staged"
    audit_stamp = _read_stamp(iso, audit_step)
    cook_stamp = _read_stamp(iso, "cook")
    build_stamp = _read_stamp(iso, build_step)
    source_snapshot = None
    test_twin = None
    if guarded:
        # Packaging is a read-only consumer of the already-normalized, signed,
        # receipted and audited staged tree.
        _guarded_receipt_bindings(build_stamp, args.platform, configuration)
        neutral_support_authority = _neutral_support_lifecycle_receipt(
            iso, require_import=True)
        if (build_stamp or {}).get("neutral_support_authority") != \
                neutral_support_authority:
            raise BuildFailure(
                "neutral support authority differs between import and cook")
        products = _guarded_product_binding(
            build_stamp, iso, args.platform, configuration)
        _guarded_zen_binding(cook_stamp, iso, args.platform)
        audited_tree = _validate_staged_tree_receipt(
            iso, audit_stamp, configuration)
        source_snapshot = (_read_stamp(iso, "seed") or {}).get(
            "source_snapshot")
        current_source_snapshot = _source_snapshot(REPO)
        if (not isinstance(source_snapshot, dict)
                or source_snapshot != current_source_snapshot):
            raise BuildFailure(
                "release Source/Tools differ from the cooked seed snapshot")
    commit = _git_commit(REPO)
    seed_stamp = _read_stamp(iso, "seed") or {}
    seed_commit = seed_stamp.get("release_commit")
    release_sequence = getattr(args, "release_sequence", 0)
    human_version = getattr(args, "human_version", "")
    if seed_commit != commit:
        raise BuildFailure(
            "release commit differs from cooked seed: "
            f"seed={seed_commit or 'missing'}, HEAD={commit}")
    if seed_stamp.get("release_sequence") != release_sequence:
        raise BuildFailure(
            "release sequence differs from cooked seed: "
            f"seed={seed_stamp.get('release_sequence', 'missing')}, "
            f"requested={release_sequence}")
    if seed_stamp.get("human_version", "") != human_version:
        raise BuildFailure(
            "human version differs from cooked seed: "
            f"seed={seed_stamp.get('human_version', '')!r}, "
            f"requested={human_version!r}")
    if shipping:
        _shipping_stage_binding(build_stamp, iso, args.platform)
        test_twin = _test_twin_binding(
            iso, args.platform, commit, source_snapshot, cook_stamp,
            build_stamp, release_sequence)
    asset_name = _release_asset_name(
        args.platform, commit, guarded, configuration)
    release_dir = iso / (RELEASE_DIRECTORIES[configuration] if guarded
                         else "release")
    asset_path = release_dir / asset_name
    manifest_path = release_dir / "release.json"
    dmg_name = (release_formats.guarded_install_dmg_asset(commit, configuration)
                if guarded and args.platform == "Mac" else None)
    stamp = _read_stamp(iso, package_step)
    cached_dmg = (_cached_guarded_dmg(release_dir, stamp, dmg_name)
                  if stamp and dmg_name is not None else None)
    if (stamp and stamp.get("asset") == asset_name
            and stamp.get("release_sequence") == release_sequence
            and stamp.get("human_version", "") == human_version
            and stamp.get("mode") == ("guarded" if guarded else None)
            and stamp.get("audit_cache_sha256")
                == (audit_stamp or {}).get("cache_sha256")
            and asset_path.is_file()
            and stamp.get("sha256") == _sha256_file(asset_path)
            and manifest_path.is_file()
            and stamp.get("release_json_sha256")
                == _sha256_file(manifest_path)
            and (dmg_name is None or cached_dmg is not None)):
        cached_manifest = _read_json_object(
            manifest_path, "cached release manifest")
        if (cached_manifest.get("asset") == asset_name
                and (not guarded
                     or cached_manifest.get("release_sequence")
                     == release_sequence)
                and cached_manifest.get("sha256") == stamp.get("sha256")
                and cached_manifest.get("size") == stamp.get("size")
                and (not guarded
                     or (cached_manifest.get("source_snapshot")
                         == source_snapshot
                         and cached_manifest.get("configuration")
                         == configuration
                         and cached_manifest.get("audit", {}).get(
                             "staged_tree_sha256")
                         == audited_tree.get("tree_sha256")
                         and cached_manifest.get("test_twin")
                         == test_twin
                         and (dmg_name is None
                              or cached_manifest.get("dmg") == cached_dmg)))):
            return "cached"
    _invalidate_from(iso, package_step)
    release_dir.mkdir(parents=True, exist_ok=True)
    flavor = asset_name.split(f"WoT-{args.platform}-", 1)[1].rsplit("-", 1)[0]
    for stale in release_dir.glob(f"WoT-*-{flavor}-*.zip"):
        stale.unlink()
    if dmg_name is not None:
        for pattern in (f"WoT-Mac-{flavor}-*.dmg",
                        f"WoT-Mac-{flavor}-*.dmg.verify.json"):
            for stale in release_dir.glob(pattern):
                stale.unlink()
    guarded_staged_root = None
    if guarded:
        guarded_staged_root = (
            iso / staging /
            products["receipt"]["stage_root"])
        _require_guarded_staged_layout(
            guarded_staged_root, args.platform, configuration)
        _require_staged_production_descriptor(guarded_staged_root, iso)
        _require_release_notes_absent(guarded_staged_root)
        _require_staged_release_identity(guarded_staged_root, iso)
        _require_staged_release_version(guarded_staged_root, iso)
        before_zip_tree = _staged_tree_inventory(iso / staging)
        if before_zip_tree["tree_sha256"] != audited_tree["tree_sha256"]:
            raise BuildFailure("staged tree changed at zip-time preflight")
    dmg_binding = None
    if args.platform == "Mac":
        live_staging = iso / staging
        live_app = ((guarded_staged_root / "WoT.app") if guarded
                    else live_staging / args.platform / "WoT.app")
        if not live_app.is_dir():
            raise BuildFailure(f"staged app missing: {live_app}")
        snapshot_context = (_private_staged_snapshot(
            iso, live_staging, audited_tree) if guarded
            else contextlib.nullcontext(live_staging))
        with snapshot_context as snapshot:
            app = snapshot / live_app.relative_to(live_staging)
            # ditto preserves the bundle's code signature, symlinks, and exec
            # bits. Extended attributes/resource forks stay out of the zip to
            # avoid AppleDouble files, while the DMG retains them.
            result = subprocess.run(
                ["ditto", "-c", "-k", "--norsrc", "--noextattr",
                 "--keepParent", str(app), str(asset_path)],
                capture_output=True, text=True)
            if result.returncode != 0:
                raise BuildFailure(f"ditto failed: {result.stderr.strip()}")
            _require_no_appledouble_entries(asset_path)
            if dmg_name is not None:
                dmg_path = release_dir / dmg_name
                try:
                    sidecar_document = mac_dmg.build_dmg(
                        app, asset_path, dmg_path)
                    dmg_binding = _guarded_dmg_binding(
                        sidecar_document, dmg_path,
                        release_dir / mac_dmg.sidecar_name(dmg_name))
                except (OSError, mac_dmg.DmgError,
                        release_formats.FormatError) as exc:
                    asset_path.unlink(missing_ok=True)
                    dmg_path.unlink(missing_ok=True)
                    (release_dir / mac_dmg.sidecar_name(dmg_name)).unlink(
                        missing_ok=True)
                    raise BuildFailure(
                        f"Mac first-install DMG failed: {exc}") from exc
    elif args.platform == "Win64":
        # BuildCookRun stages Win64 to <stagingdirectory>/Windows. No
        # signatures/exec bits are involved, so stdlib zipfile is sufficient;
        # the asset unpacks to a single stable top-level folder (the bootstrap
        # Win64 assemble leg's contract).
        staged = guarded_staged_root if guarded else iso / "staged" / "Windows"
        if not staged.is_dir():
            raise BuildFailure(f"staged client missing: {staged}")
        main = (_guarded_main_artifact_path(staged, "Win64", configuration)
                if guarded else staged / "WoT.exe")
        if not main.is_file():
            raise BuildFailure(
                f"staged client has no game executable: {main}")
        import zipfile
        top = "WoT Reborn"
        with tempfile.TemporaryDirectory(
                prefix="wot-launcher-", dir=iso) as launcher_temporary:
            launcher = Path(launcher_temporary) / "WoT Reborn.exe"
            try:
                windows_launcher.build_launcher(
                    launcher, main, configuration)
            except windows_launcher.LauncherBuildError as exc:
                raise BuildFailure(f"Windows launcher build failed: {exc}") from exc
            with zipfile.ZipFile(asset_path, "w", zipfile.ZIP_DEFLATED) as archive:
                archive.write(launcher, f"{top}/WoT Reborn.exe")
                for member in sorted(staged.rglob("*")):
                    if member.is_file():
                        archive.write(
                            member,
                            f"{top}/Game/{member.relative_to(staged).as_posix()}")
    else:
        raise BuildFailure(
            f"release packaging does not support platform {args.platform}")
    if guarded:
        after_zip_tree = _staged_tree_inventory(iso / staging)
        if after_zip_tree != before_zip_tree:
            asset_path.unlink(missing_ok=True)
            raise BuildFailure("staged tree changed while the archive was created")
    digest = _sha256_file(asset_path)
    manifest_factory = _guarded_release_manifest if guarded else _release_manifest
    manifest_arguments = dict(
        asset_name=asset_name, sha256=digest,
        size=asset_path.stat().st_size, platform=args.platform,
        commit=commit, audit_stamp=audit_stamp)
    if guarded:
        manifest_arguments["release_sequence"] = release_sequence
        manifest_arguments["cook_stamp"] = cook_stamp
        manifest_arguments["iso"] = iso
        manifest_arguments["source_snapshot"] = source_snapshot
        manifest_arguments["configuration"] = configuration
        manifest_arguments["build_stamp"] = build_stamp
        manifest_arguments["test_twin"] = test_twin
        manifest_arguments["dmg"] = dmg_binding
    manifest = manifest_factory(**manifest_arguments)
    _write_json(manifest_path, manifest)
    package_details = {
        "asset": asset_name,
        "sha256": digest,
        "size": manifest["size"],
        "git_commit": commit,
        "release_sequence": release_sequence,
        "human_version": human_version,
        "audit_cache_sha256": (audit_stamp or {}).get("cache_sha256"),
        "manifest": str(manifest_path),
        "release_json_sha256": _sha256_file(manifest_path),
    }
    if guarded:
        package_details["mode"] = "guarded"
        package_details["configuration"] = configuration
        package_details["source_snapshot_sha256"] = source_snapshot["sha256"]
        package_details["staged_tree_sha256"] = audited_tree["tree_sha256"]
        package_details["app_name_normalization"] = (
            f"WoT-Mac-{configuration}.app renamed to WoT.app"
            if args.platform == "Mac"
            else "stable launcher beside the replaceable Game tree under WoT Reborn")
        if test_twin is not None:
            package_details["test_twin_release_json_sha256"] = (
                test_twin["release_json_sha256"])
        if dmg_binding is not None:
            package_details["dmg"] = dmg_binding
    _write_stamp(iso, package_step, package_details)
    return "ran"


def _staged_tree_inventory(staged_root: Path) -> dict:
    if not staged_root.is_dir() or staged_root.is_symlink():
        raise BuildFailure(f"staged tree root is not a real directory: {staged_root}")
    entries = []

    def visit(directory: Path) -> None:
        try:
            children = sorted(os.scandir(directory), key=lambda item: item.name)
        except OSError as exc:
            raise BuildFailure(
                f"cannot enumerate staged tree {directory}: {exc}") from exc
        for child in children:
            path = Path(child.path)
            relative = path.relative_to(staged_root).as_posix()
            _reject_forbidden_alternate_bind_name(relative)
            information = child.stat(follow_symlinks=False)
            mode = stat.S_IMODE(information.st_mode)
            if child.is_symlink():
                entries.append({
                    "path": relative,
                    "type": "symlink",
                    "mode": mode,
                    "target": os.readlink(path),
                })
            elif child.is_dir(follow_symlinks=False):
                entries.append({
                    "path": relative,
                    "type": "directory",
                    "mode": mode,
                })
                visit(path)
            elif child.is_file(follow_symlinks=False):
                _reject_forbidden_alternate_bind_artifact(
                    path, relative, information.st_size)
                entries.append({
                    "path": relative,
                    "type": "file",
                    "mode": mode,
                    "size": information.st_size,
                    "sha256": _sha256_file(path),
                })
            else:
                raise BuildFailure(
                    f"staged tree contains an unsupported object: {path}")

    visit(staged_root)
    inventory = {
        "format": STAGED_TREE_RECEIPT_FORMAT,
        "entries": entries,
        "entry_count": len(entries),
        "file_count": sum(row["type"] == "file" for row in entries),
        "total_file_bytes": sum(
            row.get("size", 0) for row in entries if row["type"] == "file"),
    }
    inventory["tree_sha256"] = hashlib.sha256(
        _canonical_release_bytes(inventory)).hexdigest()
    return inventory


def _reject_forbidden_alternate_bind_artifact(
        path: Path, relative: str, size: int) -> None:
    forbidden_content = False
    # A renamed GLB is identified by its file signature. Alternate-bind JSON
    # is tiny; scan bounded small files for its exact format/key so renaming the
    # receipt or metadata cannot bypass the path rules without rereading large
    # binaries already covered by the inventory hash pass.
    with path.open("rb") as handle:
        prefix = handle.read(min(size, 16 * 1024 * 1024))
    if prefix.startswith(b"glTF"):
        forbidden_content = True
    if (b"wot-authenticated-alternate-bind-v1" in prefix
            or b'"authenticated_alternate_bind"' in prefix):
        forbidden_content = True
    if forbidden_content:
        raise BuildFailure(
            "staged tree contains forbidden alternate-bind source/receipt "
            f"artifact: {relative}")


def _reject_forbidden_alternate_bind_name(relative: str) -> None:
    normalized = relative.replace("\\", "/").casefold()
    leaf = normalized.rsplit("/", 1)[-1]
    if (normalized.endswith(".glb")
            or normalized.endswith(".meta.json")
            or leaf == "authenticated-alternate-bind.json"):
        raise BuildFailure(
            "staged tree contains forbidden alternate-bind source/receipt "
            f"artifact: {relative}")


def _binary_image_format(path: Path) -> str | None:
    """Classify a regular PE or Mach-O by structure, never by its suffix."""
    try:
        with path.open("rb") as handle:
            prefix = handle.read(8)
            if prefix[:2] == b"MZ":
                handle.seek(0x3c)
                offset_bytes = handle.read(4)
                if len(offset_bytes) != 4:
                    return None
                pe_offset = int.from_bytes(offset_bytes, "little")
                if pe_offset < 0x40 or pe_offset > path.stat().st_size - 4:
                    return None
                handle.seek(pe_offset)
                return "PE" if handle.read(4) == b"PE\0\0" else None
            if prefix[:4] in {
                    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
                    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
                    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                    b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"}:
                return "Mach-O"
    except OSError as exc:
        raise BuildFailure(f"cannot inspect staged binary {path}: {exc}") from exc
    return None


def _contains_binary_literal(path: Path, needles: tuple[bytes, ...]
                             ) -> bytes | None:
    overlap = max(len(needle) for needle in needles) - 1
    tail = b""
    try:
        with path.open("rb") as handle:
            while True:
                chunk = handle.read(1 << 20)
                if not chunk:
                    return None
                block = tail + chunk
                for needle in needles:
                    if needle in block:
                        return needle
                tail = block[-overlap:] if overlap else b""
    except OSError as exc:
        raise BuildFailure(f"cannot scan staged binary {path}: {exc}") from exc


def _audit_forbidden_binary_literals(staged_root: Path,
                                     platform_name: str) -> dict:
    """Scan every regular staged image without following symlinks."""
    if platform_name not in {"Win64", "Mac"}:
        raise BuildFailure(
            f"binary literal audit platform is unsupported: {platform_name}")
    if not staged_root.is_dir() or staged_root.is_symlink():
        raise BuildFailure(
            f"binary literal audit root is not a real directory: {staged_root}")
    tchar_encoding = "utf-16-le" if platform_name == "Win64" else "utf-32-le"
    encoded_needles = tuple(
        [needle.encode("ascii") for needle in BINARY_LITERAL_NEEDLES]
        + [needle.encode(tchar_encoding) for needle in BINARY_LITERAL_NEEDLES])
    scanned = []
    for directory, names, files in os.walk(staged_root, followlinks=False):
        names[:] = sorted(
            name for name in names
            if not (Path(directory) / name).is_symlink())
        for name in sorted(files):
            path = Path(directory) / name
            try:
                information = path.lstat()
            except OSError as exc:
                raise BuildFailure(
                    f"cannot stat staged binary candidate {path}: {exc}") from exc
            if path.is_symlink() or not stat.S_ISREG(information.st_mode):
                continue
            image_format = _binary_image_format(path)
            if image_format is None:
                continue
            relative = path.relative_to(staged_root).as_posix()
            matched = _contains_binary_literal(path, encoded_needles)
            if matched is not None:
                raise BuildFailure(
                    "staged binary contains forbidden installer-source literal: "
                    f"{relative}")
            scanned.append({
                "path": relative,
                "format": image_format,
                "size": information.st_size,
                "sha256": _sha256_file(path),
            })
    scanned.sort(key=lambda row: row["path"])
    return {
        "format": BINARY_LITERAL_AUDIT_FORMAT,
        "platform": platform_name,
        "needles": list(BINARY_LITERAL_NEEDLES),
        "encodings": ["ascii", tchar_encoding],
        "scanned_count": len(scanned),
        "scanned": scanned,
        "verdict": "pass",
    }


def _audit_no_crash_report_client(staged_root: Path) -> dict:
    """Fail if any staged executable is a standalone CrashReportClient."""
    scanned = []
    forbidden = []
    for directory, names, files in os.walk(staged_root, followlinks=False):
        names[:] = sorted(
            name for name in names
            if not (Path(directory) / name).is_symlink())
        for name in sorted(files):
            path = Path(directory) / name
            if path.is_symlink() or not path.is_file():
                continue
            if _binary_image_format(path) is None:
                continue
            relative = path.relative_to(staged_root).as_posix()
            scanned.append(relative)
            if "crashreportclient" in relative.casefold():
                forbidden.append(relative)
    if forbidden:
        raise BuildFailure(
            "staged tree contains a CrashReportClient executable: "
            + ", ".join(forbidden))
    return {
        "format": CRASH_REPORT_CLIENT_AUDIT_FORMAT,
        "scanned_count": len(scanned),
        "scanned": scanned,
        "forbidden": [],
        "verdict": "pass",
    }


def _write_staged_tree_receipt(
        iso: Path, configuration: str = GUARDED_CONFIGURATION,
        platform_name: str | None = None) -> dict:
    staged = iso / STAGING_DIRECTORIES[configuration]
    inventory = _staged_tree_inventory(staged)
    if platform_name is None:
        platform_name = "Mac" if (staged / "Mac").is_dir() else "Win64"
    inventory["binary_literal_audit"] = \
        _audit_forbidden_binary_literals(staged, platform_name)
    inventory["crash_report_client_audit"] = \
        _audit_no_crash_report_client(staged)
    suffix = _configuration_suffix(configuration)
    path = iso / "receipts" / f"staged-tree-audit{suffix}.json"
    _write_json(path, inventory)
    return {
        "path": str(path),
        "sha256": _sha256_file(path),
        "tree_sha256": inventory["tree_sha256"],
        "entry_count": inventory["entry_count"],
        "file_count": inventory["file_count"],
        "total_file_bytes": inventory["total_file_bytes"],
    }


def _validate_staged_tree_receipt(
        iso: Path, audit_stamp: dict | None,
        configuration: str = GUARDED_CONFIGURATION) -> dict:
    row = (audit_stamp or {}).get("staged_tree")
    if not isinstance(row, dict):
        raise BuildFailure("absence audit has no staged-tree receipt")
    path = Path(str(row.get("path", "")))
    receipt = _read_json_object(path, "staged-tree audit receipt")
    current = _staged_tree_inventory(iso / STAGING_DIRECTORIES[configuration])
    binary_audit = receipt.get("binary_literal_audit")
    if not isinstance(binary_audit, dict):
        raise BuildFailure("staged-tree receipt has no binary literal audit")
    current_audit = _audit_forbidden_binary_literals(
        iso / STAGING_DIRECTORIES[configuration],
        str(binary_audit.get("platform", "")))
    crash_audit = receipt.get("crash_report_client_audit")
    if not isinstance(crash_audit, dict):
        raise BuildFailure("staged-tree receipt has no CrashReportClient audit")
    current_crash_audit = _audit_no_crash_report_client(
        iso / STAGING_DIRECTORIES[configuration])
    receipt_inventory = dict(receipt)
    receipt_inventory.pop("binary_literal_audit", None)
    receipt_inventory.pop("crash_report_client_audit", None)
    if (row.get("sha256") != _sha256_file(path)
            or receipt.get("format") != STAGED_TREE_RECEIPT_FORMAT
            or receipt_inventory != current
            or binary_audit != current_audit
            or crash_audit != current_crash_audit
            or row.get("tree_sha256") != current["tree_sha256"]):
        raise BuildFailure("staged tree changed after the absence audit")
    return row


def stage_audit(args, iso: Path) -> str:
    return _stage_audit(args, iso, GUARDED_CONFIGURATION)


def stage_shipping_audit(args, iso: Path) -> str:
    """Audit 1 on the Shipping staged tree, exactly as on the Test tree."""
    if not _is_shipping_release(args):
        raise BuildFailure(
            "shipping-audit requires --guarded --configuration Shipping")
    return _stage_audit(args, iso, SHIPPING_CONFIGURATION)


def _stage_audit(args, iso: Path, configuration: str) -> str:
    step = ("shipping-audit" if configuration == SHIPPING_CONFIGURATION
            else "audit")
    suffix = _configuration_suffix(configuration)
    staged = iso / STAGING_DIRECTORIES[configuration]
    report_path = iso / f"audit-stripped-pak{suffix}.json"
    stamp = _read_stamp(iso, step)
    cache_path = str(args.cache.resolve())
    if (stamp and stamp.get("verdict") == "pass"
            and stamp.get("cache") == cache_path and report_path.is_file()):
        try:
            report = json.loads(report_path.read_text())
        except (OSError, json.JSONDecodeError):
            report = None
        if (report and report.get("verdict") == "pass"
                and report.get("cache") == cache_path):
            try:
                _validate_staged_tree_receipt(iso, stamp, configuration)
                return "cached"
            except BuildFailure:
                pass
    _invalidate_from(iso, step)
    report = absence_audit.audit(staged, args.cache.resolve(), report_path)
    if report["verdict"] != "pass":
        raise BuildFailure(
            f"absence audit found {report['total_matches']} canonical payload "
            f"probe match(es); see {report_path}")
    staged_tree = _write_staged_tree_receipt(
        iso, configuration, args.platform)
    _write_stamp(iso, step, {
        "verdict": "pass",
        "report": str(report_path),
        "cache": cache_path,
        "cache_sha256": report["cache_sha256"],
        "container_count": report["container_count"],
        "total_matches": 0,
        "staged_tree": staged_tree,
    })
    return "ran"


# ---------------------------------------------------------------------------
# Shipping stage: the release client from the Test cook
# ---------------------------------------------------------------------------

def _loose_cooked_output_identity(iso: Path, platform_name: str) -> dict:
    """The -skipcook stage's only content input: the Test cook's loose output.

    With IoStore on (BaseGame.ini bUseIoStore=True) UAT stages a loose cook
    from <MetadataDir>/packagestore.manifest + scriptobjects.bin, where
    MetadataDir = Saved/Cooked/<CookPlatform>/<ShortProjectName>/Metadata
    (CopyBuildToStagingDirectory.Automation.cs:1370-1372, 1149-1158,
    5258-5264). A ue.projectstore marker in the platform cook dir would
    instead stage the cook out of a Zen server (same file :1120-1147), which
    this pipeline only ever runs inside its sponsored lifecycle; the guarded
    -skipzenstore full cook deletes its sandbox first
    (LooseCookedPackageWriter.cpp:179-182), so none exists. Staging itself
    writes only Crypto.json and TemporaryNonUFSFiles.txt into MetadataDir
    (CopyBuildToStagingDirectory:1438-1461, 4634-4637), so the two identity
    files are stable unless a cook ran.
    """
    cooked = (iso / "Saved" / "Cooked"
              / COOK_PLATFORMS.get(platform_name, platform_name))
    if not cooked.is_dir() or cooked.is_symlink():
        raise BuildFailure(
            f"shipping stage requires the Test cook's loose output: {cooked}")
    if (cooked / "ue.projectstore").exists():
        raise BuildFailure(
            f"{cooked / 'ue.projectstore'} would stage through a Zen project "
            "store; the guarded cook must be a loose -skipzenstore cook")
    metadata = cooked / (iso / "WoT.uproject").stem / "Metadata"
    identity = {}
    for leaf in ("packagestore.manifest", "scriptobjects.bin"):
        path = metadata / leaf
        if not path.is_file():
            raise BuildFailure(f"loose cook metadata is missing: {path}")
        identity[leaf] = {
            "size": path.stat().st_size,
            "sha256": _sha256_file(path),
        }
    return {"path": str(cooked), "metadata": identity}


def _shipping_stage_command(args, iso: Path, engine_root: Path) -> list[str]:
    """Stage (never cook, never build) the Shipping client from the Test cook.

    * -skipcook: Cook() returns immediately (CookCommand.Automation.cs:
      289-292); staging reads Saved/Cooked/<CookPlatform>, whose location
      depends only on the platform (CopyBuildToStagingDirectory:1366-1372;
      DeploymentContext.cs:470-473) and whose content the cook produced
      without any client-configuration input (CookCommand.Automation.cs:
      30-144; CommandletUtils.cs:93).
    * -skipbuild: Build() returns (BuildProjectCommand.Automation.cs:64-67;
      ProjectParams.cs:736-740), so UAT stages exactly the images the guarded
      wrapper built and receipted, from their WoT-<P>-Shipping.target receipt
      (CopyBuildToStagingDirectory:6799-6823; TargetReceipt.cs:503-509),
      instead of re-invoking UBT and also building the Development editor,
      ShaderCompileWorker and UnrealPak (BuildProjectCommand:90-118). The Mac
      .app is still assembled during staging from that receipt
      (ApplePlatform.Automation.cs:136-157, BuildWithStubXcodeProject).
    * -stagingdirectory: its own directory; CleanStagingDirectory deletes only
      <stagingdir>/<Platform> (CopyBuildToStagingDirectory:6262-6286), never
      Saved/Cooked or the Test stage.
    * -NoDebugInfo (ProjectParams.cs:964): symbol files are staged as
      DebugNonUFS for every configuration unless this is passed
      (CopyBuildToStagingDirectory:2959-2962; UEBuildWindows.cs:2192-2199),
      and the public installed tree
      carries no private symbols; the stage receipt records the PDBs instead.
    * No -map/-AdditionalCookerOptions/-ddc: nothing here cooks or loads
      packages; staging runs UnrealPak only (CopyBuildToStagingDirectory:
      473-491, 4661, 5193-5289) and touches no DDC unless
      bPackageCompressionEnableDDC (same file :4741-4748), which is unset.
    """
    command = [
        str(runuat_path(engine_root)), "BuildCookRun",
        f"-project={iso / 'WoT.uproject'}", f"-platform={args.platform}",
        f"-clientconfig={SHIPPING_CONFIGURATION}",
        "-skipbuild", "-skipcook", "-stage", "-pak", "-NoDebugInfo",
        "-nop4", "-utf8output", "-unattended",
        "-stagingdirectory="
        f"{iso / STAGING_DIRECTORIES[SHIPPING_CONFIGURATION]}",
    ]
    if args.platform == "Win64":
        # WoTLaunch.Build.cs receipt property; skips staging Epic's bootstrap
        # launcher as a second root game image (WinPlatform.Automation.cs:132).
        command.append("-NoBootstrapExe")
    return command


def _validate_shipping_stage_command(command: Sequence[str], iso: Path,
                                     platform_name: str) -> None:
    tokens = [str(value) for value in command]
    folded = [token.casefold() for token in tokens]
    required = {
        "BuildCookRun", "-skipbuild", "-skipcook", "-stage", "-pak",
        "-NoDebugInfo", f"-platform={platform_name}",
        f"-clientconfig={SHIPPING_CONFIGURATION}",
        f"-project={iso / 'WoT.uproject'}",
        "-stagingdirectory="
        f"{iso / STAGING_DIRECTORIES[SHIPPING_CONFIGURATION]}",
    }
    if platform_name == "Win64":
        required.add("-NoBootstrapExe")
    forbidden_prefixes = (
        "-map=", "-additionalcookeroptions", "-ddc=", "-nozenautolaunch",
        "-zenstore", "-cookonthefly", "-iterate", "-runchunkassigner",
        "-clientconfig=test", "-clientconfig=development",
    )
    if (not required.issubset(set(tokens))
            or any(token in {"-cook", "-build"} for token in folded)
            or any(token.startswith(forbidden_prefixes) for token in folded)
            or sum(token.startswith("-clientconfig=") for token in folded) != 1
            or any("127.0.0.1:8558" in token for token in folded)):
        raise BuildFailure(
            "guarded shipping stage must be exactly BuildCookRun -skipbuild "
            "-skipcook -stage -pak -NoDebugInfo -clientconfig=Shipping into "
            "its own staging directory")


def _shipping_symbols(iso: Path, platform_name: str) -> list[dict]:
    """Receipt the private symbols -NoDebugInfo keeps out of the release."""
    if platform_name != "Win64":
        return []
    rows = []
    for target in GUARDED_RECEIPT_TARGETS:
        path = (iso / "Binaries" / "Win64"
                / f"{target}-Win64-{SHIPPING_CONFIGURATION}.pdb")
        if path.is_file():
            rows.append({
                "path": path.relative_to(iso).as_posix(),
                "size": path.stat().st_size,
                "sha256": _sha256_file(path),
            })
    return rows


def _shipping_stage_binding(stamp: dict | None, iso: Path,
                            platform_name: str) -> dict:
    row = (stamp or {}).get("shipping_stage")
    if not isinstance(row, dict):
        raise BuildFailure("guarded shipping stage has no stage receipt")
    path = Path(str(row.get("path", "")))
    receipt = _read_json_object(path, "guarded shipping stage receipt")
    log_path = Path(str(receipt.get("log", "")))
    cooked = receipt.get("cooked_output")
    try:
        _validate_shipping_stage_command(
            receipt.get("command", []), iso, platform_name)
        command_valid = True
    except BuildFailure:
        command_valid = False
    if (row.get("sha256") != _sha256_file(path)
            or path.resolve(strict=False)
            != (iso / "receipts" / "shipping-stage.json").resolve(strict=False)
            or receipt.get("format") != SHIPPING_STAGE_RECEIPT_FORMAT
            or receipt.get("platform") != platform_name
            or receipt.get("configuration") != SHIPPING_CONFIGURATION
            or not command_valid
            or log_path.resolve(strict=False)
            != (iso / "logs" / "shipping-stage.log").resolve(strict=False)
            or not log_path.is_file()
            or receipt.get("log_sha256") != _sha256_file(log_path)
            or receipt.get("exit_code") != 0
            or not isinstance(cooked, dict)
            or cooked.get("before") != cooked.get("after")
            or receipt.get("test_cook_stamp_sha256")
            != _sha256_file(_stamp_path(iso, "cook"))):
        raise BuildFailure(
            "guarded shipping stage receipt does not prove a -skipcook stage "
            "of this ISO's Test cook")
    return {"receipt_sha256": row["sha256"], "receipt": receipt}


def stage_shipping(args, iso: Path) -> str:
    """Build the guarded Shipping client and stage it from the Test cook."""
    if not _is_shipping_release(args):
        raise BuildFailure(
            "shipping-stage requires --guarded --configuration Shipping")
    configuration = SHIPPING_CONFIGURATION
    staging = STAGING_DIRECTORIES[configuration]
    cook_stamp = _read_stamp(iso, "cook")
    # The content input is the receipted, Zen-proven Test cook; nothing here
    # may run unless that cook is intact.
    _guarded_receipt_bindings(cook_stamp, args.platform)
    _guarded_product_binding(cook_stamp, iso, args.platform)
    _guarded_zen_binding(cook_stamp, iso, args.platform)
    _commit, source_snapshot = _require_seed_matches_head(iso)
    cook_stamp_sha256 = _sha256_file(_stamp_path(iso, "cook"))
    stamp = _read_stamp(iso, "shipping-stage")
    try:
        containers = absence_audit.find_containers(iso / staging)
    except (absence_audit.AuditError, OSError):
        containers = []
    if (stamp and stamp.get("command_exit_code") == 0
            and stamp.get("platform") == args.platform
            and stamp.get("test_cook_stamp_sha256") == cook_stamp_sha256
            and stamp.get("source_snapshot_sha256")
            == source_snapshot["sha256"]
            and eos_credentials.stage_identity(stamp.get("eos_credentials"))
            == eos_credentials.stage_identity(
                _eos_credentials_preview(configuration))
            and containers):
        try:
            _guarded_receipt_bindings(stamp, args.platform, configuration)
            _guarded_product_binding(stamp, iso, args.platform, configuration)
            _shipping_stage_binding(stamp, iso, args.platform)
            return "cached"
        except BuildFailure:
            pass
    _invalidate_from(iso, "shipping-stage")
    engine_root, _editor = _engine_root_and_editor(args.engine)
    pinned = PINNED_SOURCE_ENGINES[args.platform].resolve(strict=False)
    if engine_root.resolve(strict=False) != pinned:
        raise BuildFailure(
            f"guarded shipping stage resolved non-pinned engine {engine_root}")
    neutral_support_authority = _neutral_support_lifecycle_receipt(
        iso, require_import=True)
    if cook_stamp.get("neutral_support_authority") != neutral_support_authority:
        raise BuildFailure(
            "neutral support authority differs between import and cook")
    cooked_before = _loose_cooked_output_identity(iso, args.platform)
    staged_root_directory = iso / staging
    if staged_root_directory.is_symlink():
        staged_root_directory.unlink()
    elif staged_root_directory.is_dir():
        shutil.rmtree(staged_root_directory)
    elif staged_root_directory.exists():
        raise BuildFailure(
            f"guarded shipping staging output is not a directory: "
            f"{staged_root_directory}")
    target_receipts = {}
    for target in GUARDED_RECEIPT_TARGETS:
        target_receipts[target] = _run_guarded_target_build(
            args, iso, engine_root, target, configuration)
    symbols = _shipping_symbols(iso, args.platform)
    # The Shipping stage re-reads the isolated config: swap in the PUBLIC
    # deployment for exactly this staging run.
    eos_receipt = _inject_eos_credentials(iso, configuration)
    command = _shipping_stage_command(args, iso, engine_root)
    _validate_shipping_stage_command(command, iso, args.platform)
    environment, stripped_ddc_overrides = _guarded_unreal_environment(
        _iso_tool_environment(args, engine_root))
    log_path = iso / "logs" / "shipping-stage.log"
    _run_logged("shipping-stage", command, log_path, env=environment)
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    _validate_guarded_unreal_log(log_text, "shipping stage")
    if "-run=cook" in log_text.casefold():
        raise BuildFailure(
            "the shipping stage ran a cook commandlet; it must stage the "
            "Test cook unchanged")
    cooked_after = _loose_cooked_output_identity(iso, args.platform)
    if cooked_after != cooked_before:
        raise BuildFailure(
            "the Test cook's loose output changed during the Shipping stage")
    signing_facts = {}
    staged_root = _prepare_guarded_stage(
        args, iso, configuration, signing_facts=signing_facts)
    product_receipt = _write_guarded_product_receipt(
        iso, args.platform, staged_root, target_receipts, configuration,
        signing_facts=(signing_facts if args.platform == "Mac" else None))
    _bind_transferred_products(
        product_receipt["receipt"], target_receipts, args.platform,
        configuration)
    cooked_inventory = _write_guarded_cooked_inventory(
        iso, engine_root, args.platform, configuration)
    test_inventory = _guarded_cooked_inventory_binding(
        cook_stamp, args.platform)
    comparison = _cooked_twin_comparison(
        test_inventory["receipt"],
        _read_json_object(Path(cooked_inventory["path"]),
                          "Shipping cooked-container inventory"))
    stage_receipt = {
        "format": SHIPPING_STAGE_RECEIPT_FORMAT,
        "platform": args.platform,
        "configuration": configuration,
        "command": command,
        "log": str(log_path),
        "log_sha256": _sha256_file(log_path),
        "exit_code": 0,
        "environment_policy": "strip-all-ue-datacache-overrides-v1",
        "stripped_ddc_environment_overrides": stripped_ddc_overrides,
        "cooked_output": {"before": cooked_before, "after": cooked_after},
        "test_cook_stamp_sha256": cook_stamp_sha256,
        "test_twin_comparison": comparison,
        "debug_files": "excluded from the staged tree by -NoDebugInfo",
        "symbols": symbols,
    }
    stage_receipt_path = iso / "receipts" / "shipping-stage.json"
    _write_json(stage_receipt_path, stage_receipt)
    containers = absence_audit.find_containers(iso / staging)
    details = {
        "command_exit_code": 0,
        "mode": "guarded",
        "configuration": configuration,
        "platform": args.platform,
        "engine": str(engine_root),
        "staged": str(iso / staging),
        # The census the content was cooked for: this stage inherits it.
        "maps": list(cook_stamp.get("maps", [])),
        "map_count": len(cook_stamp.get("maps", [])),
        "container_count": len(containers),
        "log": str(log_path),
        "target_receipts": target_receipts,
        "product_receipt": product_receipt,
        "cooked_container_inventory": cooked_inventory,
        "neutral_support_authority": neutral_support_authority,
        "shipping_stage": {
            "path": str(stage_receipt_path),
            "sha256": _sha256_file(stage_receipt_path),
        },
        "test_cook_stamp_sha256": cook_stamp_sha256,
        "source_snapshot_sha256": source_snapshot["sha256"],
        "eos_credentials": eos_receipt,
        "compile_cache": {
            "mode": ("build-root-v1" if _compile_cache_enabled(args)
                     else "clean-iso-v1"),
        },
    }
    # Refuse to stamp anything the package step would refuse: the production
    # receipts, the staged products, and the -skipcook stage proof.
    _guarded_receipt_bindings(details, args.platform, configuration)
    _guarded_product_binding(details, iso, args.platform, configuration)
    _write_stamp(iso, "shipping-stage", details)
    _shipping_stage_binding(_read_stamp(iso, "shipping-stage"), iso,
                            args.platform)
    return "ran"


def _require_prerequisites(iso: Path, step: str) -> None:
    missing = [name for name in PREREQUISITES[step]
               if _read_stamp(iso, name) is None]
    if missing:
        raise BuildFailure(
            f"step '{step}' is missing completion stamp(s): {', '.join(missing)}")


def _container_summary(iso: Path, staging: str = "staged") -> dict:
    try:
        containers = absence_audit.find_containers(iso / staging)
    except (absence_audit.AuditError, OSError):
        containers = []
    rows = [{
        "path": path.relative_to(iso / staging).as_posix(),
        "size": path.stat().st_size,
    } for path in containers]
    by_extension = {suffix: 0 for suffix in (".ucas", ".pak", ".utoc")}
    for path in containers:
        by_extension[path.suffix.casefold()] += path.stat().st_size
    return {
        "containers": rows,
        "container_count": len(rows),
        "total_bytes": sum(row["size"] for row in rows),
        "bytes_by_extension": by_extension,
    }


def _cache_summary(cache_dir: Path) -> dict:
    try:
        manifest = json.loads((cache_dir / "manifest.json").read_text())
    except (OSError, json.JSONDecodeError):
        return {"cache_sha256": None, "entry_counts": {}}
    return {
        "cache_sha256": manifest.get("cache_sha256"),
        "entry_counts": manifest.get("summary", {}).get("entries", {}),
    }


def write_summary(args, iso: Path, statuses: dict[str, str]) -> Path:
    guarded = _mode_is_guarded(args)
    containers = _container_summary(iso)
    audit_report_path = iso / "audit-stripped-pak.json"
    try:
        audit_report = json.loads(audit_report_path.read_text())
    except (OSError, json.JSONDecodeError):
        audit_report = None
    audit_stamp = _read_stamp(iso, "audit")
    if audit_stamp is None:
        audit_report = None
    baseline_total = sum(CANONICAL_BASELINE.values())
    summary = {
        "format": ("wot-guarded-pak-build-summary-v1" if guarded
                   else "wot-stripped-pak-build-summary-v1"),
        "generated_utc": _utc_now(),
        "iso": str(iso),
        "platform": args.platform,
        "steps": {name: statuses.get(name, "not-selected") for name in STEPS},
        "seed_copy_set": list(
            GUARDED_SEED_COPY_SET if guarded else SEED_COPY_SET),
        "seed_excluded": ["Saved", "Intermediate", "DerivedDataCache"],
        "staged": containers,
        "canonical_baseline": {
            "containers": CANONICAL_BASELINE,
            "total_bytes": baseline_total,
            "staged_delta_bytes": (containers["total_bytes"] - baseline_total
                                   if containers["container_count"] else None),
        },
        "cache": _cache_summary(args.cache.resolve()),
        "audit": ({
            "verdict": audit_report.get("verdict"),
            "total_matches": audit_report.get("total_matches"),
            "report": str(audit_report_path),
        } if audit_report else {"verdict": "not-run", "report": None}),
    }
    if guarded:
        summary["release_sequence"] = getattr(args, "release_sequence", 0)
        summary["human_version"] = getattr(args, "human_version", "")
        summary["release_commit"] = (_read_stamp(iso, "seed") or {}).get(
            "release_commit")
        summary["configuration"] = GUARDED_CONFIGURATION
        summary["release_configurations"] = (
            [GUARDED_CONFIGURATION, SHIPPING_CONFIGURATION]
            if _is_shipping_release(args) else [GUARDED_CONFIGURATION])
        if _is_shipping_release(args):
            suffix = _configuration_suffix(SHIPPING_CONFIGURATION)
            shipping_report_path = iso / f"audit-stripped-pak{suffix}.json"
            try:
                shipping_report = json.loads(shipping_report_path.read_text())
            except (OSError, json.JSONDecodeError):
                shipping_report = None
            if _read_stamp(iso, "shipping-audit") is None:
                shipping_report = None
            shipping_release = (
                iso / RELEASE_DIRECTORIES[SHIPPING_CONFIGURATION]
                / "release.json")
            summary["shipping"] = {
                "configuration": SHIPPING_CONFIGURATION,
                "staged": _container_summary(
                    iso, STAGING_DIRECTORIES[SHIPPING_CONFIGURATION]),
                "audit": ({
                    "verdict": shipping_report.get("verdict"),
                    "total_matches": shipping_report.get("total_matches"),
                    "report": str(shipping_report_path),
                } if shipping_report
                    else {"verdict": "not-run", "report": None}),
                "release": (str(shipping_release)
                            if _read_stamp(iso, "shipping-package")
                            else None),
            }
        summary["bootstrap_map"] = BOOTSTRAP_MAP
        summary["guarded_import"] = _guarded_import_policy(args)
        summary["compile_cache"] = {
            "mode": ("build-root-v1" if _compile_cache_enabled(args)
                     else "clean-iso-v1"),
            "build_root": (str(_build_root(args))
                           if _compile_cache_enabled(args) else None),
        }
        summary["neutral_shells"] = {
            "count": len(NEUTRAL_SHELL_NAMES),
            "denied_read_open_receipt": str(
                iso / NEUTRAL_SHELL_DENIED_READ_RECEIPT),
            "output_identity_receipt": str(
                iso / NEUTRAL_SHELL_OUTPUT_IDENTITY_RECEIPT),
        }
    path = iso / ("guarded-pak-summary.json" if guarded
                  else "stripped-pak-summary.json")
    _write_json(path, summary)
    return path


def _dry_run(args, iso: Path, selected: set[str]) -> None:
    platform_name = {"Darwin": "Mac", "Windows": "Win64"}.get(
        host_platform.system(), args.platform)
    try:
        engine = args.engine or installed_engine_root(platform_name)
    except ConfigurationError as exc:
        raise BuildFailure(str(exc)) from exc
    executable_names = {"UnrealEditor-Cmd", "UnrealEditor-Cmd.exe"}
    is_executable = engine.name in executable_names
    editor_relative = (Path("Engine/Binaries/Win64/UnrealEditor-Cmd.exe")
                       if host_platform.system() == "Windows"
                       else Path("Engine/Binaries/Mac/UnrealEditor-Cmd"))
    editor = engine if is_executable else engine / editor_relative
    if is_executable:
        try:
            engine_root = engine.parents[3]
        except IndexError:
            engine_root = engine.parent
    else:
        engine_root = engine
    flavor = "guarded" if _mode_is_guarded(args) else "stripped-pak"
    print(f"WoT {flavor} plan (dry run; no writes or commands)")
    print(f"  iso:       {iso}")
    print(f"  cache:     {args.cache.resolve()}")
    copy_set = GUARDED_SEED_COPY_SET if _mode_is_guarded(args) else SEED_COPY_SET
    print(f"  seed copy: {', '.join(copy_set)}")
    print("  excluded:  Saved, Intermediate, DerivedDataCache")
    build_root = _build_root(args) if _compile_cache_enabled(args) else None
    if _mode_is_guarded(args):
        print("  compile:   " + (
            f"persistent build root {build_root} (incremental, transferred)"
            if build_root is not None
            else "clean builds inside the iso"))
    for index, step in enumerate(STEPS, 1):
        print(f"  {index}. [{'RUN' if step in selected else 'SKIP'}] {step}")
    if build_root is not None and selected.intersection({"import", "cook"}):
        print("  build-root editor command (before import and before cook):")
        print("   " + subprocess.list2cmdline(
            _editor_build_command(args, engine_root, build_root)))
    if "import" in selected:
        print("  import command:")
        print("   " + subprocess.list2cmdline(_import_command(args, iso, editor)))
        if _mode_is_guarded(args):
            print("  fresh fractal finalize command:")
            print("   " + subprocess.list2cmdline(
                _fresh_fractal_finalize_command(
                    iso, editor,
                    getattr(args, "editor_process_affinity", None))))
            print("  neutral shell generation command:")
            print("   " + subprocess.list2cmdline(
                _neutral_shell_generation_command(
                    iso, editor,
                    getattr(args, "editor_process_affinity", None))))
    if "cook" in selected:
        print("  cook command:")
        if _mode_is_guarded(args):
            for target in GUARDED_RECEIPT_TARGETS:
                engine_receipt, _compiled = _guarded_receipt_paths(
                    iso, args.platform, target)
                print("   " + subprocess.list2cmdline(
                    _guarded_build_command(
                        args, iso, engine_root, target, engine_receipt,
                        build_root=build_root)))
        print("   " + subprocess.list2cmdline(
            _cook_command(args, iso, engine_root)))
    if "shipping-stage" in selected:
        print("  shipping stage commands (outside the Zen lifecycle):")
        for target in GUARDED_RECEIPT_TARGETS:
            engine_receipt, _compiled = _guarded_receipt_paths(
                iso, args.platform, target, SHIPPING_CONFIGURATION)
            print("   " + subprocess.list2cmdline(
                _guarded_build_command(
                    args, iso, engine_root, target, engine_receipt,
                    SHIPPING_CONFIGURATION, build_root)))
        print("   " + subprocess.list2cmdline(
            _shipping_stage_command(args, iso, engine_root)))


def _release_sequence_argument(value: str) -> int:
    try:
        sequence = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "must be a non-negative int64") from exc
    if not 0 <= sequence <= release_formats.INT64_MAX:
        raise argparse.ArgumentTypeError("must be a non-negative int64")
    return sequence


def _human_version_argument(value: str) -> str:
    try:
        return release_formats.validate_human_version(
            value, "--human-version", allow_empty=True)
    except release_formats.FormatError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--iso", type=Path, default=REPO / ".stripped-pak",
                        help="disposable isolated project root (inside this repo)")
    result.add_argument("--cache", type=Path, required=True,
                        help="authenticated all-type payload cache for Audit 1")
    result.add_argument(
        "--conv-dir", type=Path,
        default=local_path("conversion_root", required=False,
                           default=REPO / "Intermediate/Conv"))
    result.add_argument("--exports-dir", type=Path,
                        help="export tree (default: WOT_REFERENCE_ROOT/exports)")
    result.add_argument("--retail-root", type=Path,
                        help="retail tree (default: WOT_REFERENCE_ROOT/original)")
    result.add_argument("--mesh-source", choices=("prepared", "retail-package"),
                        default="prepared",
                        help=("mesh source for development stripped imports; "
                              "guarded imports are carrier-free and ignore it"))
    result.add_argument("--engine", type=Path,
                        help="UE_5.8 root or host UnrealEditor-Cmd executable")
    result.add_argument("--platform", choices=("Mac", "Win64"), default="Mac")
    result.add_argument(
        "--editor-process-affinity", type=int, choices=range(1, 65),
        help="Win64 logical-core count forwarded to every editor commandlet")
    result.add_argument("--guarded", action="store_true",
                        help="build the pinned-source guarded artifact(s)")
    result.add_argument(
        "--release-sequence", type=_release_sequence_argument, default=0,
        help="non-negative int64 stamped into the runnable release identity")
    result.add_argument(
        "--human-version", type=_human_version_argument, default="",
        help="human version stamped into the runnable package (empty for development)")
    result.add_argument(
        "--configuration", choices=("Development", "Test", "Shipping"),
        help=("explicit client configuration. Guarded Test (default) builds "
              "the first-run funnel artifact; guarded Shipping builds that "
              "SAME Test artifact plus the Shipping release staged from its "
              "cook (steps shipping-stage/-audit/-package, release-shipping/)"))
    result.add_argument(
        "--compile-cache", choices=COMPILE_CACHE_CHOICES, default="auto",
        help=("guarded C++ builds: 'on' compiles incrementally in a "
              "persistent per-(platform, engine pin) build root and transfers "
              "the receipted products into the iso; 'off' keeps clean builds "
              "inside the iso; 'auto' (default) is 'on' for guarded Win64"))
    result.add_argument(
        "--guarded-import", choices=GUARDED_IMPORT_CHOICES,
        default="shipping-producers",
        help=("guarded import_all steps: 'shipping-producers' (default) runs "
              "only the steps whose outputs the release cooks; 'full' is the "
              "previous every-step-but-meshes/pickups import, for A/B "
              "comparison"))
    result.add_argument(
        "--build-root-parent", type=Path,
        help=("parent of the persistent build roots (default: "
              "<main checkout>/.guarded-build)"))
    result.add_argument("--only-step", action="append", default=[], metavar="STEP",
                        help="run only one numbered/named step (repeat/comma-separate)")
    result.add_argument("--skip-step", action="append", default=[], metavar="STEP",
                        help="skip one numbered/named step (repeat/comma-separate)")
    result.add_argument("--dry-run", action="store_true",
                        help="print the resolved commands without writing or executing")
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    held = contextlib.ExitStack()
    try:
        # Resolve defaults without requiring private inputs before CLI/mode
        # validation. The selected stage reports a missing reference tree only
        # when that stage actually consumes it.
        args.exports_dir = args.exports_dir or exports_dir(required=False)
        args.retail_root = args.retail_root or original_dir(required=False)
        _resolve_build_mode(args)
        iso = _validate_iso(args.iso)
        only = _split_named(args.only_step, "--only-step")
        skip = _split_named(args.skip_step, "--skip-step")
        default_steps = set(STEPS) if _is_shipping_release(args) else (
            set(STEPS) - set(SHIPPING_STEPS))
        selected = (default_steps if not only else only) - skip
        if (selected.intersection(SHIPPING_STEPS)
                and not _is_shipping_release(args)):
            raise BuildFailure(
                "shipping-stage/-audit/-package require --guarded "
                "--configuration Shipping")
        selected_lifecycle = selected.intersection(
            {"import", "marker", "cook"})
        if (_mode_is_guarded(args) and selected_lifecycle
                and selected_lifecycle != {"import", "marker", "cook"}):
            raise BuildFailure(
                "guarded Unreal stages must select import, marker, and cook "
                "together so one isolated Zen lifecycle proves all commandlets")
        if args.dry_run:
            _dry_run(args, iso, selected)
            return 0
        if _mode_is_guarded(args):
            _require_reviewed_innoextract(args.platform)
        statuses = {}
        if _compile_cache_enabled(args):
            if selected.intersection({"import", "cook", "shipping-stage"}):
                # Held for the whole run: sync, build, transfer and the cook
                # that consumes the transfer must not interleave with another
                # run's.
                held.enter_context(
                    build_root_cache.BuildRootLock(_build_root(args)))
        elif "seed" in selected or "import" in selected:
            # Only the clean-ISO lane runs the seeded repo editor module; the
            # build root installs a snapshot-built editor instead.
            _ensure_repo_editor_module(args)
        runners = {
            "seed": lambda: stage_seed(
                iso, guarded=_mode_is_guarded(args),
                release_sequence=args.release_sequence,
                human_version=args.human_version),
            "import": lambda: stage_import(args, iso),
            "marker": lambda: stage_marker(iso),
            "cook": lambda: stage_cook(args, iso),
            "audit": lambda: stage_audit(args, iso),
            "package": lambda: stage_package(args, iso),
            "shipping-stage": lambda: stage_shipping(args, iso),
            "shipping-audit": lambda: stage_shipping_audit(args, iso),
            "shipping-package": lambda: stage_shipping_package(args, iso),
        }
        if "seed" in selected:
            _require_prerequisites(iso, "seed")
            statuses["seed"] = runners["seed"]()
            print(f"[seed] {statuses['seed']}", flush=True)
        lifecycle_steps = tuple(
            step for step in ("import", "marker", "cook")
            if step in selected)
        if _mode_is_guarded(args) and lifecycle_steps:
            engine_root, _editor = _engine_root_and_editor(args.engine)

            def run_unreal_stages():
                for step in lifecycle_steps:
                    _require_prerequisites(iso, step)
                    statuses[step] = runners[step]()
                    print(f"[{step}] {statuses[step]}", flush=True)
                return dict(statuses)

            _result, zen_lifetime = _run_guarded_unreal_lifecycle(
                iso, engine_root, args.platform, run_unreal_stages)
            for step in ("import", "cook"):
                if step not in lifecycle_steps:
                    continue
                stamp = _read_stamp(iso, step)
                if not isinstance(stamp, dict):
                    raise BuildFailure(
                        f"guarded {step} did not publish a completion stamp")
                stamp["zen_lifetime"] = zen_lifetime
                _write_stamp(iso, step, {
                    key: value for key, value in stamp.items()
                    if key not in {"step", "completed_utc"}
                })
        else:
            for step in ("import", "marker", "cook"):
                if step not in selected:
                    continue
                _require_prerequisites(iso, step)
                statuses[step] = runners[step]()
                print(f"[{step}] {statuses[step]}", flush=True)
        # The Shipping steps run after (and outside) the Test cook's Zen
        # lifecycle: a -skipcook stage launches no editor or cooker and so
        # has no DDC or Zen consumer to isolate.
        for step in ("audit", "package", *SHIPPING_STEPS):
            if step not in selected:
                continue
            _require_prerequisites(iso, step)
            statuses[step] = runners[step]()
            print(f"[{step}] {statuses[step]}", flush=True)
        summary = write_summary(args, iso, statuses)
        print(f"{'Guarded' if args.guarded else 'Stripped-pak'} summary: {summary}")
        return 0
    except (BuildFailure, ConfigurationError, StageFailure, absence_audit.AuditError,
            absence_audit.cache.CacheError, build_root_cache.BuildRootError,
            OSError, ValueError) as exc:
        print(f"BUILD-STRIPPED-PAK ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        held.close()


if __name__ == "__main__":
    raise SystemExit(main())
