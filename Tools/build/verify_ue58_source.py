#!/usr/bin/env python3
"""Prove an engine tree is the pinned, pristine, source-built UE 5.8.

Every release binary is compiled by this engine, so "which engine was that?" has
to have exactly one answer that is checkable after the fact. An installed engine
cannot give that answer: it ships prebuilt binaries whose provenance is Epic's
launcher rather than a commit, and it cannot build the Program targets this project
needs at all.

So the checks below are deliberately unforgiving. They fail on:

  * `Engine/Build/InstalledBuild.txt` — an installed build, not a source build;
  * a HEAD that is not EXACTLY the pinned 40-hex commit (an abbreviated or
    tag-only answer is not provenance: tags move, commits do not);
  * a shallow or grafted clone, whose history cannot be audited later;
  * a changed Setup dependency manifest, which silently changes the binaries
    linked into the engine;
  * ANY drift in the source files the guarded patch consumes, because patching
    drifted bytes produces an engine nobody reviewed;
  * a missing or mismatched compiler/SDK identity, since the same source with a
    different toolchain is a different binary.

Stdlib only. This runs before UBT, so a bad tree costs seconds rather than an
hour of compilation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import (ConfigurationError, installed_engine_root,
                        local_path)  # noqa: E402

MANIFEST_FORMAT = "wot-ue58-source-toolchain-v1"
PATCH_MANIFEST_FORMAT = "wot-ue58-engine-patch-v1"
FULL_COMMIT = re.compile(r"^[0-9a-f]{40}$")
HEX_256 = re.compile(r"^[0-9a-f]{64}$")


class VerificationFailed(Exception):
    """The tree is not the pinned engine. There is no partial pass."""


class VerificationReceipt:
    """What was checked, and what it resolved to. Written next to a build."""

    def __init__(self, engine: Path, manifest_path: Path, platform_name: str):
        self.engine = engine
        self.manifest_path = manifest_path
        self.platform_name = platform_name
        self.checks: list[str] = []
        self.commit = ""
        self.tag = ""
        self.toolchain: dict = {}

    def record(self, check: str) -> None:
        self.checks.append(check)

    def as_dict(self) -> dict:
        return {
            "format": "wot-ue58-source-receipt-v1",
            "engine": str(self.engine),
            "manifest": str(self.manifest_path),
            "platform": self.platform_name,
            "commit": self.commit,
            "tag": self.tag,
            "toolchain": self.toolchain,
            "checks": sorted(self.checks),
        }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_manifest(path: Path) -> dict:
    try:
        document = json.loads(path.read_bytes().decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise VerificationFailed(f"cannot read the manifest {path}: {exc}") from exc
    if document.get("format") != MANIFEST_FORMAT:
        raise VerificationFailed(
            f"manifest format is {document.get('format')!r}, expected {MANIFEST_FORMAT!r}")
    return document


def _git(engine: Path, *arguments: str) -> str:
    result = subprocess.run(["git", "-C", str(engine), *arguments],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise VerificationFailed(
            f"git {' '.join(arguments)} failed in {engine}: {result.stderr.strip()}")
    return result.stdout.strip()


def check_not_installed_build(engine: Path, receipt: VerificationReceipt) -> None:
    marker = engine / "Engine" / "Build" / "InstalledBuild.txt"
    if marker.exists():
        raise VerificationFailed(
            f"{engine} is an INSTALLED build ({marker.name} present). Installed "
            "engines cannot build this project's Program targets and carry no "
            "commit provenance; use the source tree named in the manifest.")
    receipt.record("not-an-installed-build")


def check_commit_provenance(engine: Path, manifest: dict,
                            receipt: VerificationReceipt) -> None:
    expected = manifest.get("engine", {}).get("commit", "")
    if not FULL_COMMIT.match(expected or ""):
        raise VerificationFailed(
            "the manifest does not pin a full 40-hex engine commit; an "
            "abbreviated commit or a bare tag is not provenance")

    if not (engine / ".git").exists():
        raise VerificationFailed(
            f"{engine} is not a git checkout, so its provenance cannot be verified")

    # A shallow or grafted clone cannot be audited later: the history that would
    # show what this commit actually contains is simply not present.
    if (engine / ".git" / "shallow").exists():
        raise VerificationFailed(
            f"{engine} is a SHALLOW clone; full history is required for provenance")

    head = _git(engine, "rev-parse", "HEAD")
    if head != expected:
        raise VerificationFailed(
            f"{engine} is at commit {head}, expected the pinned {expected}")
    receipt.commit = head
    receipt.record("commit-matches-pin")

    # The tag is recorded for humans, but it is the COMMIT that is authoritative:
    # a tag can be moved, and this check is what would catch it.
    tag = manifest.get("engine", {}).get("tag", "")
    if tag:
        try:
            resolved = _git(engine, "rev-list", "-n", "1", tag)
        except VerificationFailed:
            resolved = ""
        if resolved and resolved != expected:
            raise VerificationFailed(
                f"tag {tag} now resolves to {resolved}, not the pinned {expected}; "
                "the tag has moved and the pin is authoritative")
        receipt.tag = tag
        receipt.record("tag-agrees-with-pin")


def check_dependencies(engine: Path, manifest: dict,
                       receipt: VerificationReceipt) -> None:
    dependencies = manifest.get("dependencies", {})
    if not dependencies:
        raise VerificationFailed(
            "the manifest pins no Setup dependency manifest; the engine's "
            "downloaded binaries would be unpinned")
    for relative, expected in sorted(dependencies.items()):
        path = engine / relative
        if not path.is_file():
            raise VerificationFailed(f"the dependency manifest {relative} is missing")
        actual = sha256_file(path)
        if actual != expected:
            # Name the overwhelmingly likely cause instead of leaving an operator
            # staring at two hashes: a CRLF checkout produces exactly this failure
            # on a tree that is otherwise correct.
            hint = ""
            try:
                # A bounded prefix: this file is ~36 MB and one CRLF proves it.
                with path.open("rb") as handle:
                    prefix = handle.read(1 << 20)
                if b"\r\n" in prefix:
                    hint = (" This file contains CRLF line endings, so the clone "
                            "was almost certainly made with core.autocrlf=true. "
                            "Re-clone with -c core.autocrlf=false -c core.eol=lf; "
                            "the pin records the upstream LF bytes.")
            except OSError:
                pass
            raise VerificationFailed(
                f"{relative} hashes {actual}, expected {expected}; the engine's "
                f"downloaded dependencies are not the pinned set.{hint}")
    receipt.record(f"dependency-manifests-pinned({len(dependencies)})")


def _patch_records(patch_manifest: Path | None) -> dict[str, tuple[str | None, str, bool]]:
    """Recorded pristine/patched hashes, keyed by engine-relative path.

    A guarded engine has TWO legitimate states: pristine, and correctly patched.
    The public tree deliberately has this manifest but not the licensee-only
    patch, so verification must depend on these hashes rather than patch bytes.
    """
    if patch_manifest is None:
        return {}
    try:
        document = json.loads(patch_manifest.read_bytes().decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise VerificationFailed(
            f"cannot read the patch manifest {patch_manifest}: {exc}") from exc
    if document.get("format") != PATCH_MANIFEST_FORMAT:
        raise VerificationFailed(
            f"patch manifest format is {document.get('format')!r}, expected "
            f"{PATCH_MANIFEST_FORMAT!r}")
    files = document.get("files")
    if not isinstance(files, list) or not files:
        raise VerificationFailed(f"{patch_manifest} records no patch-owned files")
    recorded: dict[str, tuple[str | None, str, bool]] = {}
    for record in files:
        if not isinstance(record, dict) or not isinstance(record.get("path"), str):
            raise VerificationFailed(
                f"{patch_manifest} has a patch file record without a path")
        relative = record["path"]
        candidate = Path(relative)
        if candidate.is_absolute() or ".." in candidate.parts or relative in recorded:
            raise VerificationFailed(
                f"{patch_manifest} has an invalid or duplicate patch path {relative!r}")
        created = record.get("created") is True
        pristine = record.get("pristine_sha256")
        patched = record.get("patched_sha256")
        if created:
            if pristine is not None:
                raise VerificationFailed(
                    f"created patch file {relative} must not record pristine bytes")
        elif not isinstance(pristine, str) or not HEX_256.fullmatch(pristine):
            raise VerificationFailed(
                f"patch file {relative} has no valid pristine SHA-256")
        if not isinstance(patched, str) or not HEX_256.fullmatch(patched):
            raise VerificationFailed(
                f"patch file {relative} has no valid patched SHA-256")
        recorded[relative] = (pristine, patched, created)
    if not recorded:
        raise VerificationFailed(
            f"{patch_manifest} records no patched hashes; run apply_engine_patch.py "
            "--record first")
    return recorded


def check_patch_state(engine: Path, source_manifest: dict,
                      patch_manifest: Path,
                      records: dict[str, tuple[str | None, str, bool]],
                      receipt: VerificationReceipt,
                      require_patched: bool) -> None:
    """Verify every patch-owned engine file without reading the private patch."""
    source_pristine = source_manifest.get("pristine_sources", {})
    for relative, expected in source_pristine.items():
        if relative not in records:
            raise VerificationFailed(
                f"{patch_manifest} does not cover source-manifest file {relative}")
        pristine, _, created = records[relative]
        if created or pristine != expected:
            raise VerificationFailed(
                f"{patch_manifest} disagrees with the source manifest for {relative}")

    pristine_count = 0
    patched_count = 0
    for relative, (pristine, patched, created) in sorted(records.items()):
        path = engine / relative
        if not path.is_file():
            if created and not path.exists():
                pristine_count += 1
                continue
            raise VerificationFailed(f"patch-owned engine file {relative} is missing")
        actual = sha256_file(path)
        if not created and actual == pristine:
            pristine_count += 1
        elif actual == patched:
            patched_count += 1
        else:
            expected = f"patched {patched}"
            if pristine is not None:
                expected = f"pristine {pristine} or {expected}"
            raise VerificationFailed(
                f"patch-owned engine file {relative} hashes {actual}, expected {expected}")

    if pristine_count and patched_count:
        raise VerificationFailed(
            "patch-owned engine files are only partially patched "
            f"({pristine_count} pristine, {patched_count} patched)")
    state = "patched" if patched_count else "pristine"
    if require_patched and state != "patched":
        raise VerificationFailed(
            "the engine is pristine, but this build requires the fully patched "
            "state recorded in the patch manifest")
    receipt.record(f"patch-manifest-{state}({len(records)})")


def check_pristine_sources(engine: Path, manifest: dict,
                           receipt: VerificationReceipt,
                           patched: dict[str, str] | None = None) -> None:
    sources = manifest.get("pristine_sources", {})
    if not sources:
        raise VerificationFailed(
            "the manifest pins no pristine source hashes; the guarded patch would "
            "be applied to unknown bytes")
    patched_count = 0
    for relative, expected in sorted(sources.items()):
        path = engine / relative
        if not path.is_file():
            raise VerificationFailed(f"the patched-source file {relative} is missing")
        actual = sha256_file(path)
        if actual == expected:
            continue
        # Accept the recorded PATCHED bytes for a manifest-owned file, and say so
        # in the receipt: "verified" must never be ambiguous about which state.
        expected_patched = (patched or {}).get(relative)
        if expected_patched and actual == expected_patched:
            patched_count += 1
            continue
        raise VerificationFailed(
            f"{relative} has drifted (hashes {actual}, expected {expected}"
            + (f" pristine or {expected_patched} patched" if expected_patched else "")
            + "). Patching drifted bytes produces an engine nobody reviewed.")
    if patched_count:
        receipt.record(f"patch-inputs-patched({patched_count})")
        receipt.record(f"patch-inputs-pristine({len(sources) - patched_count})")
    else:
        receipt.record(f"patch-inputs-pristine({len(sources)})")


def _windows_sdk_selected_by_ubt(engine: Path, include_root: Path) -> str:
    """Return the installed SDK selected by this source engine's UBT policy."""
    installed = {
        child.name for child in include_root.iterdir() if child.is_dir()
    } if include_root.is_dir() else set()
    policy = engine / "Engine" / "Config" / "Windows" / "Windows_SDK.json"
    if policy.is_file():
        try:
            main_version = json.loads(
                policy.read_text(encoding="utf-8"))["MainVersion"]
        except (OSError, json.JSONDecodeError, KeyError, TypeError) as exc:
            raise VerificationFailed(
                f"cannot read UBT Windows SDK policy {policy}: {exc}") from exc
        if not isinstance(main_version, str) or not main_version:
            raise VerificationFailed(
                f"UBT Windows SDK policy {policy} has no MainVersion")
        if main_version not in installed:
            raise VerificationFailed(
                f"UBT-selected Windows SDK {main_version} is not installed "
                f"under {include_root}")
        return main_version
    if not installed:
        return ""
    return max(installed, key=lambda value: tuple(
        int(part) if part.isdigit() else -1 for part in value.split(".")))


def detect_toolchain(platform_name: str, pinned: dict | None = None,
                     engine: Path | None = None) -> dict:
    """Read the compiler/SDK identity this host would actually build with.

    Machine-local paths only locate the toolchain. The returned values are the
    portable compiler and SDK identity compared with the committed manifest.
    """
    if platform_name == "Mac":
        def run(*command):
            result = subprocess.run(command, capture_output=True, text=True)
            return result.stdout.strip() if result.returncode == 0 else ""

        clang = run("clang", "--version").splitlines()
        return {
            "apple_clang": clang[0] if clang else "",
            "macos_sdk": run("xcrun", "--sdk", "macosx", "--show-sdk-version"),
        }
    if platform_name == "Win64":
        # Detected here rather than passed in by the wrapper: a value the wrapper
        # supplies is a value the wrapper could get wrong, and the whole point is
        # that the host proves its own identity.
        import os
        import subprocess as sp

        program_files = os.environ.get("ProgramFiles(x86)", "")
        if not program_files:
            raise VerificationFailed(
                "the Windows ProgramFiles(x86) environment setting is absent")
        vswhere = Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        identity: dict[str, str] = {}
        try:
            pinned_install = str(local_path("visual_studio_root_win64"))
        except ConfigurationError as exc:
            raise VerificationFailed(str(exc)) from exc
        if vswhere.is_file():
            def ask(*properties):
                selector = (["-path", pinned_install] if pinned_install
                            else ["-latest", "-products", "*",
                                  "-requires",
                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"])
                result = sp.run([str(vswhere), *selector, *properties],
                                capture_output=True, text=True)
                return result.stdout.strip() if result.returncode == 0 else ""

            identity["visual_studio"] = ask("-property", "installationVersion")
            install = ask("-property", "installationPath")
            if install:
                default = (Path(install) / "VC" / "Auxiliary" / "Build"
                           / "Microsoft.VCToolsVersion.default.txt")
                if default.is_file():
                    toolset = default.read_text().strip()
                    identity["msvc_toolset"] = toolset
                    # Ask cl.exe itself rather than inferring from the directory
                    # name: the compiler is what actually produces the binary.
                    cl = (Path(install) / "VC" / "Tools" / "MSVC" / toolset / "bin"
                          / "Hostx64" / "x64" / "cl.exe")
                    if cl.is_file():
                        probe = sp.run([str(cl)], capture_output=True, text=True)
                        banner = (probe.stderr or probe.stdout).splitlines()
                        for line in banner:
                            if "Version" in line:
                                identity["msvc_cl"] = line.split("Version", 1)[1].strip().split()[0]
                                break

        if engine is None:
            try:
                engine = local_path("source_engine_root_win64")
            except ConfigurationError as exc:
                raise VerificationFailed(str(exc)) from exc
        kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) \
            / "Windows Kits" / "10" / "Include"
        selected_sdk = _windows_sdk_selected_by_ubt(engine, kits)
        if selected_sdk:
            identity["windows_sdk"] = selected_sdk
        return identity
    raise VerificationFailed(f"unsupported platform {platform_name!r}")


def check_toolchain(manifest: dict, platform_name: str, observed: dict,
                    receipt: VerificationReceipt) -> None:
    pinned = manifest.get("toolchains", {}).get(platform_name)
    if not pinned:
        raise VerificationFailed(
            f"the manifest records no toolchain identity for {platform_name}; the "
            "same source with a different compiler is a different binary")
    missing = [key for key, value in pinned.items() if not value]
    if missing:
        raise VerificationFailed(
            f"the pinned {platform_name} toolchain has empty fields: {', '.join(missing)}")

    for key, expected in sorted(pinned.items()):
        actual = observed.get(key, "")
        if not actual:
            raise VerificationFailed(
                f"this host reports no {key}; the {platform_name} toolchain "
                "identity cannot be confirmed")
        if actual != expected:
            raise VerificationFailed(
                f"{key} is {actual!r}, expected the pinned {expected!r}")
    receipt.toolchain = dict(pinned)
    receipt.record(f"toolchain-matches-pin({platform_name})")


def check_not_a_rejected_root(engine: Path, manifest: dict,
                              receipt: VerificationReceipt) -> None:
    """Refuse the developer-baseline installed roots by name as well as by marker.

    The marker check already catches them, but naming them explicitly turns a
    confusing "InstalledBuild.txt present" into "you passed the installed engine".
    """
    resolved = str(engine.resolve())
    platform_name = {"Darwin": "mac", "Windows": "win64"}.get(platform.system())
    rejected = installed_engine_root(platform_name, required=False) if platform_name else None
    # Both sides resolved: a symlinked prefix (macOS /var -> /private/var)
    # must not hide a match.
    if rejected is not None and resolved.rstrip("/\\").lower() == \
            str(Path(rejected).resolve()).rstrip("/\\").lower():
        raise VerificationFailed(
            f"{engine} is a pre-plan developer baseline engine and may not "
            "build guarded, packaged, candidate, or release artifacts")
    receipt.record("not-a-developer-baseline-root")


def verify_source_engine(engine: Path, manifest: Path, require_pristine: bool,
                         patch_manifest: Path | None = None,
                         require_patched: bool = False) -> VerificationReceipt:
    if require_patched and not require_pristine:
        raise VerificationFailed(
            "requiring a patched engine also requires source verification")
    document = load_manifest(manifest)
    platform_name = {"Darwin": "Mac", "Windows": "Win64"}.get(platform.system(), "")
    if not platform_name and require_pristine:
        # The host only has to be a build host when the TOOLCHAIN is being checked.
        # Commit provenance, dependency pinning, and installed-build rejection are
        # pure file inspection, so a static tool (the world-start hook checker) can
        # run them from any host — including a Linux
        # machine. Gating those on the host coupled a text scan to a build machine.
        raise VerificationFailed(f"unsupported host platform {platform.system()!r}")

    receipt = VerificationReceipt(engine, manifest, platform_name)
    if not engine.is_dir():
        raise VerificationFailed(f"the engine path {engine} does not exist")

    check_not_a_rejected_root(engine, document, receipt)
    check_not_installed_build(engine, receipt)
    check_commit_provenance(engine, document, receipt)
    check_dependencies(engine, document, receipt)
    if require_pristine:
        if require_patched and patch_manifest is None:
            raise VerificationFailed(
                "--require-patched requires a patch manifest")
        patch_records = _patch_records(patch_manifest)
        check_pristine_sources(engine, document, receipt,
                               {path: values[1]
                                for path, values in patch_records.items()})
        if patch_manifest is not None:
            check_patch_state(engine, document, patch_manifest, patch_records,
                              receipt, require_patched)
        check_toolchain(
            document, platform_name,
            detect_toolchain(
                platform_name,
                (document.get("toolchains") or {}).get(platform_name),
                engine),
            receipt)
    return receipt


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify a pinned source-built UE 5.8 tree before using it.")
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--require-source", action="store_true",
                        help="also require pristine patch inputs and a matching "
                             "compiler/SDK identity")
    parser.add_argument("--patch-manifest", type=Path,
                        help="accept the recorded PATCHED bytes for manifest-owned "
                             "engine files as well as pristine ones")
    parser.add_argument("--require-patched", action="store_true",
                        help="require every patch-manifest file to have its recorded "
                             "patched hash (requires --patch-manifest)")
    parser.add_argument("--receipt", type=Path,
                        help="write the verification receipt here")
    args = parser.parse_args(argv)

    try:
        receipt = verify_source_engine(args.engine, args.manifest,
                                       args.require_source, args.patch_manifest,
                                       args.require_patched)
    except VerificationFailed as exc:
        print(f"ENGINE REJECTED: {exc}", file=sys.stderr)
        return 1

    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        # LF bytes explicitly (open(), not write_text(newline=...) which is
        # 3.10+): release verifiers recompute this file's SHA-256 from
        # canonical LF JSON, so Windows CRLF translation breaks the binding.
        with args.receipt.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                json.dumps(receipt.as_dict(), indent=2, sort_keys=True) + "\n")
    print(f"ENGINE VERIFIED {args.engine} @ {receipt.commit} "
          f"({receipt.tag or 'no tag'}) [{', '.join(sorted(receipt.checks))}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
