#!/usr/bin/env python3
"""Provision the pinned source UE 5.8 tree and record what it actually is.

Run once per machine. It clones the pinned COMMIT (not the tag — tags move), runs
Epic's Setup and project generation, and then writes `source-toolchain.json`
recording exactly what was provisioned: the commit, the tag it came from, the
Setup dependency-manifest hashes, the pristine hashes of every file the guarded
patch will touch, and this host's compiler/SDK identity.

That manifest is the thing later builds are checked against, so it is generated
from a REAL tree rather than hand-written: a hand-written hash is a guess, and a
guess that happens to be wrong would reject every future build for reasons nobody
could diagnose.

Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_ue58_source as verifier

UE_REPOSITORY = "git@github.com:EpicGames/UnrealEngine.git"
UE_TAG = "5.8.0-release"
UE_COMMIT = "7deeb413d3dc1fc034f48d1aacc0861301829d32"

# Every existing file the world-start patch touches. These are pinned pristine
# so the patch is never applied to bytes nobody reviewed.
PATCHED_SOURCES = (
    "Engine/Source/Runtime/Engine/Private/UnrealEngine.cpp",
    "Engine/Source/Runtime/Engine/Private/World.cpp",
    "Engine/Source/Runtime/Launch/Private/Mac/LaunchMac.cpp",
    "Engine/Source/Runtime/Launch/Private/Windows/LaunchWindows.cpp",
)
# WoTWorldMaterialization.h is CREATED by the patch and does not exist pristine,
# so the patch manifest owns its output hash instead.

DEPENDENCY_MANIFESTS = (
    "Engine/Build/Commit.gitdeps.xml",
)

class ProvisionFailed(Exception):
    pass


def run(command: list[str], cwd: Path | None = None) -> None:
    print(f"$ {' '.join(command)}", flush=True)
    result = subprocess.run(command, cwd=str(cwd) if cwd else None)
    if result.returncode != 0:
        raise ProvisionFailed(f"command failed ({result.returncode}): {' '.join(command)}")


def clone_if_absent(engine: Path) -> None:
    if (engine / ".git").is_dir():
        print(f"already a checkout: {engine}")
        return
    engine.parent.mkdir(parents=True, exist_ok=True)
    # Single-branch, but NOT shallow: the verifier refuses a shallow clone because
    # its history cannot be audited afterwards.
    #
    # core.autocrlf=false / core.eol=lf are REQUIRED, not tidiness. Windows git
    # defaults to autocrlf=true, which checks Engine/Build/Commit.gitdeps.xml out
    # as CRLF — 36,978,135 bytes instead of the upstream LF blob's 36,744,018, one
    # CR per line — and `dependency-manifests-pinned` then fails on a tree that is
    # otherwise exactly right. The reverse
    # hazard is worse: a manifest REGENERATED from a CRLF checkout would pin bytes
    # no other platform could reproduce.
    run(["git", "clone", "-c", "core.autocrlf=false", "-c", "core.eol=lf",
         "--single-branch", "--branch", UE_TAG, UE_REPOSITORY, str(engine)])


def require_pinned_commit(engine: Path) -> None:
    head = subprocess.run(["git", "-C", str(engine), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=True).stdout.strip()
    if head != UE_COMMIT:
        raise ProvisionFailed(
            f"{engine} is at {head}, not the pinned {UE_COMMIT}. The tag may have "
            "moved; the commit is authoritative.")
    print(f"engine is at the pinned commit {head}")


def run_setup(engine: Path, skip: bool) -> None:
    if skip:
        print("skipping Setup (--skip-setup)")
        return
    setup = engine / ("Setup.bat" if sys.platform == "win32" else "Setup.sh")
    if not setup.exists():
        raise ProvisionFailed(f"{setup} is missing")
    run([str(setup)], cwd=engine)


def generate_project_files(engine: Path, skip: bool) -> None:
    if skip:
        print("skipping GenerateProjectFiles (--skip-setup)")
        return
    generate = engine / ("GenerateProjectFiles.bat" if sys.platform == "win32"
                         else "GenerateProjectFiles.sh")
    if generate.exists():
        run([str(generate)], cwd=engine)


def build_manifest(engine: Path, existing: dict | None) -> dict:
    dependencies = {}
    for relative in DEPENDENCY_MANIFESTS:
        path = engine / relative
        if not path.is_file():
            raise ProvisionFailed(
                f"{relative} is missing; run Setup before generating the manifest")
        dependencies[relative] = verifier.sha256_file(path)

    pristine = {}
    for relative in PATCHED_SOURCES:
        path = engine / relative
        if not path.is_file():
            raise ProvisionFailed(f"patch input {relative} is missing from {engine}")
        pristine[relative] = verifier.sha256_file(path)

    # Merge rather than overwrite: each host can only observe its OWN toolchain,
    # so the other platform's pinned identity must survive a re-run here.
    toolchains = dict((existing or {}).get("toolchains", {}))
    platform_name = "Win64" if sys.platform == "win32" else "Mac"
    observed = verifier.detect_toolchain(platform_name)
    if observed:
        toolchains[platform_name] = observed
    toolchains.setdefault("Mac", {})
    toolchains.setdefault("Win64", {})
    for identity in toolchains.values():
        identity.pop("xcode_path", None)
        identity.pop("vs_install_path", None)

    return {
        "format": verifier.MANIFEST_FORMAT,
        "engine": {"repository": UE_REPOSITORY, "tag": UE_TAG, "commit": UE_COMMIT},
        "dependencies": dependencies,
        "pristine_sources": pristine,
        "toolchains": toolchains,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Clone, set up, and pin the source UE 5.8 toolchain.")
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--skip-clone", action="store_true")
    parser.add_argument("--skip-setup", action="store_true",
                        help="assume Setup and project generation already ran")
    parser.add_argument("--manifest-only", action="store_true",
                        help="only regenerate the manifest from an existing tree")
    args = parser.parse_args(argv)

    try:
        if not args.manifest_only:
            if not args.skip_clone:
                clone_if_absent(args.engine)
            require_pinned_commit(args.engine)
            run_setup(args.engine, args.skip_setup)
            generate_project_files(args.engine, args.skip_setup)
        else:
            require_pinned_commit(args.engine)

        existing = None
        if args.manifest.is_file():
            try:
                existing = json.loads(args.manifest.read_text())
            except (OSError, json.JSONDecodeError):
                existing = None

        document = build_manifest(args.engine, existing)
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        # LF bytes explicitly, same contract as the receipt writers.
        with args.manifest.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                json.dumps(document, indent=2, sort_keys=True) + "\n")
    except (ProvisionFailed, subprocess.CalledProcessError) as exc:
        print(f"PROVISION FAILED: {exc}", file=sys.stderr)
        return 1

    print(f"wrote {args.manifest}: commit {UE_COMMIT}, "
          f"{len(document['pristine_sources'])} pristine patch inputs, "
          f"{len(document['dependencies'])} dependency manifests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
