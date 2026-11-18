#!/usr/bin/env python3
"""Apply the reviewed WoT UE 5.8 patch to exactly the pinned source tree.

The patch manifest deliberately starts life without patched hashes.  Once the
patch exists, run ``--record`` against a pristine synthetic/copy checkout:

    python3 Tools/dist/apply_engine_patch.py \
      --engine <pristine-ue> \
      --manifest EnginePatches/UE-5.8/manifest.json \
      --patch EnginePatches/UE-5.8/guarded-bootstrap.patch --record

That mode verifies the source pin and all pristine bytes, applies the patch,
then atomically records the patch digest and every resulting hash.
Normal apply/verify modes reject an unrecorded manifest.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
import verify_ue58_source as source_verifier

MANIFEST_FORMAT = "wot-ue58-engine-patch-v1"
HEX_256 = re.compile(r"^[0-9a-f]{64}$")

PATCHED_PATHS = (
    "Engine/Source/Runtime/Core/Private/Misc/ConfigCacheIni.cpp",
    "Engine/Source/Runtime/Core/Private/Misc/Paths.cpp",
    "Engine/Source/Runtime/Core/Private/WoTUserDataRoot.cpp",
    "Engine/Source/Runtime/Core/Public/WoTUserDataRoot.h",
    "Engine/Source/Runtime/Engine/Private/UnrealEngine.cpp",
    "Engine/Source/Runtime/Engine/Private/World.cpp",
    "Engine/Source/Runtime/Engine/Private/GameUserSettings.cpp",
    "Engine/Source/Runtime/Engine/Private/GameEngine.cpp",
    "Engine/Source/Runtime/Engine/Public/WoTWorldMaterialization.h",
    "Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp",
    "Engine/Source/Runtime/Launch/Private/Mac/LaunchMac.cpp",
    "Engine/Source/Runtime/Launch/Private/Windows/LaunchWindows.cpp",
    # Mac LAN discovery: some macOS hosts refuse the limited broadcast
    # (255.255.255.255) with EHOSTUNREACH; the beacon falls back to
    # per-interface subnet-directed broadcasts, PLATFORM_MAC-only.
    "Engine/Plugins/Online/OnlineBase/Source/Private/Online/LANBeacon.cpp",
    # Internet play (EOS Connect Device ID, OSS v1): stock UserManagerEOS
    # never sends the UserLoginInfo DisplayName a desktop Device ID login
    # requires, and its auth-expiration refresh logs a Device ID user out.
    "Engine/Plugins/Online/OnlineSubsystemEOS/Source/OnlineSubsystemEOS/Private/UserManagerEOS.cpp",
)
CREATED_PATHS = frozenset({
    "Engine/Source/Runtime/Core/Private/WoTUserDataRoot.cpp",
    "Engine/Source/Runtime/Core/Public/WoTUserDataRoot.h",
    "Engine/Source/Runtime/Engine/Public/WoTWorldMaterialization.h",
})


class PatchRejected(Exception):
    """A patch operation was not proven safe and exact."""


@dataclass(frozen=True)
class FileRecord:
    path: str
    created: bool
    pristine: str | None
    patched: str | None


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _read_json(path: Path, description: str) -> dict:
    try:
        value = json.loads(path.read_bytes().decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise PatchRejected(f"cannot read {description} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise PatchRejected(f"{description} {path} is not a JSON object")
    return value


def load_manifest(path: Path, *, allow_unrecorded: bool = False) -> tuple[dict, list[FileRecord]]:
    document = _read_json(path, "patch manifest")
    if document.get("format") != MANIFEST_FORMAT:
        raise PatchRejected(
            f"patch manifest format is {document.get('format')!r}, expected {MANIFEST_FORMAT!r}")
    raw_files = document.get("files")
    if not isinstance(raw_files, list):
        raise PatchRejected(
            f"MANIFEST COVERAGE: files must list all {len(PATCHED_PATHS)} owned paths")

    records: list[FileRecord] = []
    for raw in raw_files:
        if not isinstance(raw, dict) or not isinstance(raw.get("path"), str):
            raise PatchRejected("MANIFEST COVERAGE: every file entry needs a path")
        path = raw["path"]
        created = raw.get("created") is True
        pristine = raw.get("pristine_sha256")
        patched = raw.get("patched_sha256")
        if created:
            if pristine is not None:
                raise PatchRejected(f"MANIFEST COVERAGE: created file {path} must not pin pristine bytes")
        elif pristine is None and allow_unrecorded:
            pass
        elif not isinstance(pristine, str) or not HEX_256.fullmatch(pristine):
            raise PatchRejected(
                f"MANIFEST COVERAGE: existing file {path} needs a pristine SHA-256")
        if patched is not None and (not isinstance(patched, str) or not HEX_256.fullmatch(patched)):
            raise PatchRejected(f"invalid patched SHA-256 for {path}")
        if patched is None and not allow_unrecorded:
            raise PatchRejected(
                f"UNRECORDED PATCH HASH: {path}; generate hashes with --record after Wave B creates the patch")
        records.append(FileRecord(path, created, pristine, patched))

    actual = [record.path for record in records]
    if Counter(actual) != Counter(PATCHED_PATHS):
        missing = sorted(set(PATCHED_PATHS) - set(actual))
        extra = sorted(set(actual) - set(PATCHED_PATHS))
        raise PatchRejected(
            f"MANIFEST COVERAGE: expected exactly all {len(PATCHED_PATHS)} "
            "guarded-patch files; "
            f"missing={missing or 'none'}, extra={extra or 'none'}, duplicates="
            f"{sorted(path for path, count in Counter(actual).items() if count != 1) or 'none'}")
    for record in records:
        if record.created != (record.path in CREATED_PATHS):
            raise PatchRejected(
                f"MANIFEST COVERAGE: {record.path} has the wrong created/existing classification")
    return document, records


def source_manifest_path(document: dict, patch_manifest: Path) -> Path:
    relative = document.get("source_manifest")
    if not isinstance(relative, str) or not relative:
        raise PatchRejected("patch manifest does not name its source_manifest")
    candidate = Path(relative)
    return candidate if candidate.is_absolute() else patch_manifest.parent / candidate


def verify_pinned_engine(engine: Path, document: dict, patch_manifest: Path) -> dict:
    source_path = source_manifest_path(document, patch_manifest)
    try:
        source_document = source_verifier.load_manifest(source_path)
        # require_pristine=False is intentional.  The imported verifier remains
        # the single definition of installed/source provenance and dependency
        # identity, while this manifest accepts either pristine OR patched bytes.
        source_verifier.verify_source_engine(engine, source_path, False)
    except source_verifier.VerificationFailed as exc:
        raise PatchRejected(str(exc)) from exc
    return source_document


def verify_manifest_agrees_with_source(records: list[FileRecord], source_document: dict) -> None:
    source_pristine = source_document.get("pristine_sources", {})
    # source-toolchain.json predates later reviewed additions to the patch. Its
    # pinned commit still authenticates those inputs; --record captures their
    # pristine bytes from that exact checkout. Paths already byte-pinned by the
    # source manifest must continue to agree exactly.
    expected_paths = ((set(PATCHED_PATHS) - CREATED_PATHS)
                      & set(source_pristine))
    expected = {path: source_pristine.get(path) for path in expected_paths}
    manifest_pristine = {record.path: record.pristine for record in records
                         if record.path in expected_paths}
    if manifest_pristine != expected:
        raise PatchRejected(
            "PRISTINE MANIFEST DISAGREEMENT: patch pristine hashes must exactly reuse "
            "the 4 existing-file hashes pinned by source-toolchain.json")


def _state(engine: Path, records: list[FileRecord]) -> tuple[str, dict[str, str]]:
    observed: dict[str, str] = {}
    pristine = []
    patched = []
    unknown = []
    missing_created = []
    for record in records:
        path = engine / record.path
        if not path.is_file():
            if record.created:
                observed[record.path] = "absent"
                pristine.append(record.path)
                if record.patched is not None:
                    missing_created.append(record.path)
            else:
                observed[record.path] = "missing"
                unknown.append(record.path)
            continue
        digest = sha256_file(path)
        observed[record.path] = digest
        if record.created:
            if record.patched is not None and digest == record.patched:
                patched.append(record.path)
            else:
                unknown.append(record.path)
        elif digest == record.pristine:
            pristine.append(record.path)
        elif record.patched is not None and digest == record.patched:
            patched.append(record.path)
        else:
            unknown.append(record.path)

    if len(pristine) == len(records):
        return "pristine", observed
    if len(patched) == len(records):
        return "patched", observed
    existing_records = [record for record in records if not record.created]
    existing_all_patched = all(record.path in patched for record in existing_records)
    if missing_created and existing_all_patched:
        raise PatchRejected(
            "MISSING CREATED FILE: correctly patched existing inputs are missing "
            + ", ".join(missing_created))
    if unknown and not patched:
        details = ", ".join(f"{path}={observed[path]}" for path in unknown)
        raise PatchRejected(f"PRISTINE DRIFT: {details}")
    if pristine or patched:
        raise PatchRejected(
            "PARTIALLY APPLIED PATCH: manifest files are a mixture of pristine, patched, "
            f"and unknown bytes (pristine={len(pristine)}, patched={len(patched)}, "
            f"unknown={len(unknown)})")
    raise PatchRejected("PATCHED TREE DRIFT: no complete pristine or patched state exists")


def _patch_paths(patch: Path) -> list[str]:
    try:
        text = patch.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise PatchRejected(f"cannot read patch {patch}: {exc}") from exc
    paths = []
    for match in re.finditer(r"^diff --git a/(\S+) b/(\S+)$", text, re.MULTILINE):
        if match.group(1) != match.group(2):
            raise PatchRejected("PATCH COVERAGE: renames are not allowed in the guarded patch")
        paths.append(match.group(1))
    if Counter(paths) != Counter(PATCHED_PATHS):
        raise PatchRejected(
            "PATCH COVERAGE: patch must contain exactly one diff for every manifest-owned path")
    return paths


def _git_apply(engine: Path, patch: Path, *options: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(engine), "apply", *options, str(patch.resolve())],
        capture_output=True, text=True)


def verify_patch_identity(document: dict, patch: Path, *, allow_unrecorded: bool) -> None:
    _patch_paths(patch)
    expected = document.get("patch_sha256")
    # In --record mode the digest is an OUTPUT, not an input. Enforcing a
    # previously recorded value here would make the manifest write-once: a
    # legitimately corrected patch could never be re-recorded, and the only way
    # forward would be to hand-edit the digest — the exact thing --record exists
    # to prevent.
    if allow_unrecorded:
        return
    if not isinstance(expected, str) or not HEX_256.fullmatch(expected):
        raise PatchRejected("UNRECORDED PATCH HASH: patch_sha256 must be generated by --record")
    actual = sha256_file(patch)
    if actual != expected:
        raise PatchRejected(f"PATCH DIGEST MISMATCH: patch hashes {actual}, expected {expected}")


_BUILD_OUTPUT_DIRECTORIES = ("uatobj", "uatbin", "Intermediate", "Binaries",
                             "obj", "bin", "Saved")


def _is_build_output(path: str) -> bool:
    """True for compiler/tool output that lives under Engine/Source."""
    return any(part in _BUILD_OUTPUT_DIRECTORIES for part in path.split("/"))


def unowned_engine_drift(engine: Path) -> list[str]:
    """Changes under Engine/Source that this manifest does not own.

    Shrinking the patch's file set ORPHANS every file the previous manifest
    owned and the new one does not, and owned-path checks cannot see them:
    restore restores only owned paths and verify inspects only owned paths, so
    the gate printed "verified patched" over a tree whose orphaned
    LaunchEngineLoop.cpp still called a deleted symbol (found at LINK time on
    Win64, 2026-07-26). The engine is a git checkout, so git itself is the
    authority on what changed; anything it reports outside the owned set is
    drift this tool must refuse to bless.
    """
    # --untracked-files=all is load-bearing: without it git collapses a
    # directory of untracked files into one "?? dir/" line, which can never
    # match an owned file path — the created header would read as drift.
    result = subprocess.run(
        ["git", "-C", str(engine), "-c", "core.hooksPath=", "status",
         "--porcelain", "--untracked-files=all", "--", "Engine/Source"],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise PatchRejected(
            f"cannot inspect the engine checkout for drift: {result.stderr.strip()}")
    drift = []
    for line in result.stdout.splitlines():
        if len(line) < 4:
            continue
        status, path = line[:2], line[3:].strip().strip('"')
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        if path in PATCHED_PATHS:
            continue
        # UNTRACKED build output is not drift. Compiling UnrealBuildTool and
        # AutomationTool writes .NET intermediates into uatobj/ and uatbin/
        # UNDER Engine/Source, and Epic does not ignore them, so every box that
        # has ever built the engine tools carries hundreds of them: one machine had
        # 53 such directories before any patch was applied. Flagging them made
        # apply and verify refuse to run at all on Win64. A tracked MODIFICATION
        # is still drift wherever it lives — orphans of a previous patch
        # generation are tracked edits, which is the case this check exists for.
        if status.strip() == "??" and _is_build_output(path):
            continue
        drift.append(line.strip())
    return drift


def _require_no_unowned_drift(engine: Path) -> None:
    drift = unowned_engine_drift(engine)
    if drift:
        raise PatchRejected(
            "UNOWNED ENGINE DRIFT: the engine checkout carries changes under "
            "Engine/Source that this manifest does not own — likely orphans of "
            "a previous patch generation. Restore tracked files with "
            "`git -c core.hooksPath= checkout -- <path>` and delete untracked "
            "ones, then rerun. Drift: " + "; ".join(drift))


def restore_owned_files(engine: Path, records: list[FileRecord], commit: str) -> None:
    existing = [record.path for record in records if not record.created]
    result = subprocess.run(
        # core.hooksPath= disables the engine checkout's post-checkout hook
        # (GitDependencies.sh). It is a bash script, and restoring from a host
        # whose /bin/sh is dash makes it fail and take the restore down with it.
        # Restoring pinned bytes must not depend on the host shell.
        ["git", "-C", str(engine), "-c", "core.hooksPath=", "checkout", commit, "--", *existing],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise PatchRejected(f"RESTORE FAILED: {result.stderr.strip()}")
    for record in records:
        if record.created:
            path = engine / record.path
            if path.exists():
                if not path.is_file() and not path.is_symlink():
                    raise PatchRejected(f"RESTORE FAILED: created path {record.path} is not a file")
                path.unlink()


def _apply_from_pristine(engine: Path, records: list[FileRecord], patch: Path) -> None:
    check = _git_apply(engine, patch, "--check")
    if check.returncode != 0:
        reversed_check = _git_apply(engine, patch, "--reverse", "--check")
        if reversed_check.returncode == 0:
            raise PatchRejected("REVERSED PATCH: the supplied diff removes the reviewed changes")
        raise PatchRejected(f"PATCH DOES NOT APPLY CLEANLY: {check.stderr.strip()}")
    applied = _git_apply(engine, patch)
    if applied.returncode != 0:
        raise PatchRejected(f"PATCH APPLY FAILED after a successful check: {applied.stderr.strip()}")


def _atomic_write_manifest(path: Path, document: dict) -> None:
    data = json.dumps(document, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    # newline="\n": the manifest is a -text byte-pinned file. Text mode on
    # Windows otherwise writes CRLF, turning a Win64 --record into a whole-file
    # rewrite that no longer matches the LF bytes every other host reads.
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n",
                                     dir=path.parent,
                                     prefix=path.name + ".", delete=False) as handle:
        temporary = Path(handle.name)
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def _require_private_patch(patch: Path | None) -> Path:
    if patch is None or not patch.is_file():
        detail = str(patch) if patch is not None else "guarded-bootstrap.patch"
        raise PatchRejected(
            f"the guarded engine patch is absent ({detail}). Apply and record "
            "operations require guarded-bootstrap.patch from the licensee-only "
            "EpicGames fork linked under 'Building from source' in README.md")
    return patch


def operate(engine: Path, manifest: Path, patch: Path | None = None, *,
            verify_only: bool = False, restore: bool = False,
            record: bool = False, require_patched: bool = False) -> str:
    if sum((verify_only, restore, record)) > 1:
        raise PatchRejected("--verify-only, --restore, and --record are mutually exclusive")
    if require_patched and not verify_only:
        raise PatchRejected("--require-patched is valid only with --verify-only")

    # Provenance rejection comes first by design.  In particular, an installed
    # engine or wrong commit must not be obscured by a later patch-coverage error.
    document = _read_json(manifest, "patch manifest")
    if document.get("format") != MANIFEST_FORMAT:
        raise PatchRejected(
            f"patch manifest format is {document.get('format')!r}, expected {MANIFEST_FORMAT!r}")
    source_document = verify_pinned_engine(engine, document, manifest)
    # Drift rejection comes before any owned-path answer: "the files I own are
    # correct" must never print as "the engine is correct".
    _require_no_unowned_drift(engine)
    document, records = load_manifest(manifest, allow_unrecorded=record)
    if record:
        refreshed = []
        raw_by_path = {raw["path"]: raw for raw in document["files"]}
        for item in records:
            pristine = item.pristine
            if not item.created and pristine is None:
                source = engine / item.path
                if not source.is_file():
                    raise PatchRejected(
                        f"RECORD FAILED: missing pristine input {item.path}")
                pristine = sha256_file(source)
                raw_by_path[item.path]["pristine_sha256"] = pristine
            refreshed.append(FileRecord(
                item.path, item.created, pristine, item.patched))
        records = refreshed
    verify_manifest_agrees_with_source(records, source_document)

    if restore:
        commit = source_document["engine"]["commit"]
        restore_owned_files(engine, records, commit)
        state, _ = _state(engine, records)
        if state != "pristine":
            raise PatchRejected("RESTORE FAILED: owned files did not return to pristine bytes")
        return "restored to pristine"

    # Public clones deliberately carry only the hashes. Verification and restore
    # do not need the private diff; when one is supplied, keep the private-tree
    # identity check exactly as strict as before.
    if patch is not None and patch.is_file():
        verify_patch_identity(document, patch, allow_unrecorded=record)
    elif not verify_only:
        patch = _require_private_patch(patch)
    state, _ = _state(engine, records)
    if verify_only:
        if require_patched and state != "patched":
            raise PatchRejected(
                "PATCHED STATE REQUIRED: the manifest-owned files are pristine")
        return f"verified {state}"
    if state == "patched":
        return "already correctly patched"
    if state != "pristine":
        raise PatchRejected(f"unexpected engine state {state}")

    patch = _require_private_patch(patch)
    _apply_from_pristine(engine, records, patch)
    if record:
        for raw in document["files"]:
            output = engine / raw["path"]
            if not output.is_file():
                restore_owned_files(engine, records, source_document["engine"]["commit"])
                raise PatchRejected(f"MISSING CREATED FILE after apply: {raw['path']}")
            raw["patched_sha256"] = sha256_file(output)
        document["patch_sha256"] = sha256_file(patch)
        _atomic_write_manifest(manifest, document)
        _, recorded = load_manifest(manifest)
        state, _ = _state(engine, recorded)
        if state != "patched":
            raise PatchRejected("RECORD FAILED: generated hashes do not verify")
        return "applied patch and recorded patched hashes"

    try:
        final_state, observed = _state(engine, records)
    except PatchRejected as exc:
        restore_owned_files(engine, records, source_document["engine"]["commit"])
        raise PatchRejected(f"WRONG PATCHED HASH: {exc}") from exc
    if final_state != "patched":
        restore_owned_files(engine, records, source_document["engine"]["commit"])
        wrong = [record.path for record in records
                 if observed.get(record.path) != record.patched]
        raise PatchRejected("WRONG PATCHED HASH: " + ", ".join(wrong))
    return "patch applied and patched hashes verified"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--patch", type=Path,
                        help="licensee-only patch; required for apply/record, "
                             "optional for verify/restore")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--require-patched", action="store_true",
                        help="with --verify-only, reject a pristine engine")
    parser.add_argument("--restore", action="store_true")
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = operate(args.engine, args.manifest, args.patch,
                         verify_only=args.verify_only, restore=args.restore,
                         record=args.record, require_patched=args.require_patched)
    except PatchRejected as exc:
        print(f"ENGINE PATCH REJECTED: {exc}", file=sys.stderr)
        return 1
    print(f"ENGINE PATCH OK: {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
