#!/usr/bin/env python3
"""Export the reviewed public manifest from HEAD or an explicit index tree.

``propose`` prints the broader policy's current candidates.  Its output is a
review aid only: export always reads the exact, committed manifest.  Every
candidate must also be classified: export fails while one is in neither the
public manifest nor the reviewed private list, so a new file can never drop
out of the public tree unnoticed.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import unicodedata


RECEIPT_NAME = "PUBLIC_EXPORT_RECEIPT.json"
MANIFEST_PATH = "Tools/public/public-files.txt"
# Reviewed candidates that stay private. An absent list is an empty one, which
# makes every candidate public-or-error.
PRIVATE_LIST_PATH = "Tools/public/private-files.txt"
DENIED_EXTENSIONS = frozenset({
    ".u", ".utx", ".uax", ".umx", ".unr", ".uc", ".t3d", ".wot", ".int",
})
DENIED_COMPONENTS = frozenset({
    "original", "devtree", "exports", "saved", "content", ".superpowers",
})
WINDOWS_DEVICE_NAMES = frozenset({
    "aux", "con", "conin$", "conout$", "nul", "prn",
    *(f"com{number}" for number in range(1, 10)),
    *(f"lpt{number}" for number in range(1, 10)),
})
WINDOWS_INVALID_CHARACTERS = frozenset('<>:"\\|?*')
PROPOSAL_PREFIXES = (
    "Source/",
    "Tools/",
    "Config/",
    "ThirdParty/innoextract/",
    ".github/",
)
PUBLIC_RELEASE_FILES = frozenset({
    "Build/WoTRelease/announcement-public-key-v1.json",
    "Build/WoTRelease/bootstrap-trust-v1.json",
    "Build/WoTRelease/cache-contract-v3-production.json",
    "Build/WoTRelease/cache-v3-production-descriptor.json",
    "Build/WoTRelease/public-root-ring.json",
    "Build/WoTRelease/release-identity.json",
    "Build/WoTRelease/release-ring/WoT-root-key-ring-v1.json",
    "Build/WoTRelease/release-ring/WoT-root-key-ring-v1.json.sig",
    "Build/WoTRelease/release-version.json",
    "Build/WoTRelease/schemas/announcement-v1.schema.json",
    "Build/WoTRelease/schemas/component-identity-v1.schema.json",
    "Build/WoTRelease/schemas/full-package-v1.schema.json",
    "Build/WoTRelease/schemas/installed-tree-v1.schema.json",
    "Build/WoTRelease/schemas/key-ring-v1.schema.json",
    "Build/WoTRelease/schemas/recovery-domain-v1.schema.json",
    "Build/WoTRelease/schemas/release-manifest-v2.schema.json",
})
PROPOSAL_FILES = frozenset({
    "Build/Mac/Resources/Info.Template.plist",
    "EnginePatches/UE-5.8/manifest.json",
    "EnginePatches/UE-5.8/source-toolchain.json",
    "WoT.uproject",
    "PORT_NOTES.md",
    "README.md",
    "LICENSE",
    "NOTICE",
    "CONTRIBUTING.md",
    "CODE_OF_CONDUCT.md",
    "SECURITY.md",
    "docs/LOCAL-CONFIGURATION.md",
    "docs/BUILD-PATHS.md",
    "ThirdParty/vcpkg/vcpkg.json",
}) | PUBLIC_RELEASE_FILES
# Files the public tree needs at a path the private tree already uses for
# something else. The source is exported ONLY at its public path; the receipt
# records both.
EXPORT_RENAMES = {
    "Tools/public/scaffold/gitattributes": ".gitattributes",
    "Tools/public/scaffold/gitignore": ".gitignore",
}
# Narrow, reviewed export-only rewrites: path -> ((pattern, replacement), ...).
# Each must match exactly once, so a rewrite whose source was fixed upstream
# fails closed instead of lingering. There are none at present.
PUBLIC_BYTE_REWRITES: dict = {}
RETAIL_DEPENDENT_TESTS = frozenset({
    "Tools/convert/test_presentation_census.py",
    "Tools/convert/test_bsp_linecheck.py",
    "Tools/convert/test_fractalfire_exact.py",
    "Tools/convert/test_fractalice.py",
    "Tools/convert/test_fractalwater_exact.py",
    "Tools/convert/test_lightmap_responses.py",
    "Tools/convert/test_mesh2glb.py",
    "Tools/convert/test_mover_light_census.py",
    "Tools/convert/test_reachspecs.py",
    "Tools/convert/test_runtime_light_waveform_census.py",
    "Tools/convert/test_utx2png.py",
    "Tools/convert/test_zones.py",
    "Tools/dist/test_build_payload_cache.py",
    "Tools/dist/test_apply_engine_patch.py",
    "Tools/dist/test_generate_production_cache_contract.py",
    "Tools/dist/test_generate_frontend_entry_descriptor.py",
    "Tools/dist/test_native_cache_parity.py",
    "Tools/dist/test_runtime_map_projection_cache.py",
    "Tools/dist/test_runtime_map_projection_parity.py",
    "Tools/import/test_import_andilay_roots.py",
    "Tools/import/test_import_fixed_pickups.py",
    "Tools/import/test_import_sounds.py",
    "Tools/import/test_pool_entries.py",
    "Tools/test/angreal/test_angreal_matrix.py",
    "Tools/test/gallery/test_gallery_diff.py",
    "Tools/test/retail_text/test_gen_prose_fingerprints.py",
    "Tools/test/sweep/test_invisible_collision_census.py",
    "Tools/test/sweep/test_map_sweep.py",
})
RETAIL_DERIVED_ARTIFACTS = frozenset({
    "Tools/dist/fixtures/bsp-native-parity-v1.json",
    "Tools/import/data/angreal_hands.csv",
    "Tools/import/data/menu_strings.csv",
    "Tools/test/gallery/gallery.json",
    "Tools/test/gallery/runtime-map-t5-1.json",
    "Tools/test/gallery/runtime-map-t5-2-sky.json",
    "Tools/test/sweep/sweep_expectations.json",
})
RETAIL_DERIVED_PREFIXES = (
    "Tools/migration/",
    "Tools/test/sweep/fixtures/",
)
GIT_ENVIRONMENT_TO_CLEAR = frozenset({
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_ATTR_SOURCE",
    "GIT_CEILING_DIRECTORIES",
    "GIT_COMMON_DIR",
    "GIT_DIR",
    "GIT_GRAFT_FILE",
    "GIT_INDEX_FILE",
    "GIT_NAMESPACE",
    "GIT_OBJECT_DIRECTORY",
    "GIT_PREFIX",
    "GIT_SHALLOW_FILE",
    "GIT_WORK_TREE",
})


class ExportError(RuntimeError):
    """A public export precondition or Git operation failed."""


@dataclass(frozen=True)
class HeadEntry:
    path: str
    oid: str
    mode: str
    source_path: str | None = None


def _git(
    repo: Path,
    *arguments: str,
    input_bytes: bytes | None = None,
    environment: dict[str, str] | None = None,
) -> bytes:
    env = os.environ.copy()
    for name in GIT_ENVIRONMENT_TO_CLEAR:
        env.pop(name, None)
    if environment:
        env.update(environment)
    env["GIT_NO_REPLACE_OBJECTS"] = "1"
    try:
        completed = subprocess.run(
            ["git", *arguments],
            cwd=repo,
            env=env,
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise ExportError(f"could not run Git: {exc}") from exc
    if completed.returncode:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise ExportError(detail or f"git {' '.join(arguments)} failed")
    return completed.stdout


def _is_proposal_candidate(path: str) -> bool:
    """Return whether the old broad policy would propose ``path`` for review."""
    return path in PROPOSAL_FILES or path.startswith(PROPOSAL_PREFIXES)


def _portable_key(path: str) -> str:
    parts = path.split("/")
    if path.startswith("/") or any(part in {"", ".", ".."} for part in parts):
        raise ExportError(f"nonportable path in Git tree: {path!r}")
    for part in parts:
        if (part.casefold() == ".git"
                or part.endswith((" ", "."))
                or any(character in WINDOWS_INVALID_CHARACTERS for character in part)
                or any(ord(character) < 32 or ord(character) == 127
                       or 0xD800 <= ord(character) <= 0xDFFF
                       for character in part)
                or part.rstrip(" .").split(".", 1)[0].casefold()
                in WINDOWS_DEVICE_NAMES):
            raise ExportError(f"nonportable path in Git tree: {path!r}")
    return unicodedata.normalize("NFC", path).casefold()


def _validate_portable_paths(paths: list[str]) -> None:
    seen: dict[str, str] = {}
    for path in paths:
        key = _portable_key(path)
        previous = seen.get(key)
        if previous is not None and previous != path:
            raise ExportError(
                f"portable path collision in Git tree: {previous!r} and {path!r}"
            )
        seen[key] = path


def _is_denied(path: str) -> bool:
    try:
        _portable_key(path)
    except ExportError:
        return True
    parts = path.split("/")
    lowered = [part.casefold() for part in parts]
    if parts[-1].casefold() in {"local.json", "nodes.json"}:
        return True
    if any(part in DENIED_COMPONENTS for part in lowered):
        return True
    if Path(parts[-1]).suffix.casefold() in DENIED_EXTENSIONS:
        return True
    path_lower = path.casefold()
    if path_lower.startswith("enginepatches/"):
        return path not in {
            "EnginePatches/UE-5.8/manifest.json",
            "EnginePatches/UE-5.8/source-toolchain.json",
        }
    if path_lower.startswith("build/wotrelease/"):
        release_parts = lowered[2:]
        filename = release_parts[-1]
        if (path_lower == "build/wotrelease/key-custody.md"
                or "test-keys" in release_parts
                or any("private" in part for part in release_parts)
                or filename.startswith("eos-credentials")):
            return True
    if path_lower.startswith("docs/superpowers/"):
        return True
    if (path_lower.startswith("docs/")
            and parts[-1].casefold().startswith("handoff-")):
        return True
    return (path in RETAIL_DEPENDENT_TESTS
            or path in RETAIL_DERIVED_ARTIFACTS
            or path.startswith(RETAIL_DERIVED_PREFIXES))


def _parse_path_list(data: bytes, list_path: str) -> list[str]:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ExportError(f"{list_path} is not UTF-8: {exc}") from exc
    paths = text.splitlines()
    if any(not path or path != path.strip() for path in paths):
        raise ExportError(
            f"{list_path} must contain one nonblank path per line"
        )
    if paths != sorted(paths):
        raise ExportError(f"{list_path} must be sorted")
    if len(paths) != len(set(paths)):
        raise ExportError(f"{list_path} contains duplicate paths")
    if list_path not in paths:
        raise ExportError(f"{list_path} must admit itself")
    _validate_portable_paths(paths)
    return paths


def _parse_manifest(data: bytes) -> list[str]:
    paths = _parse_path_list(data, MANIFEST_PATH)
    denied = [path for path in paths if _is_denied(path)]
    if denied:
        raise ExportError(
            f"denied path appears in {MANIFEST_PATH}: {denied[0]}"
        )
    return paths


def _reject_local_attribute_overrides(repo: Path) -> None:
    raw_path = _git(repo, "rev-parse", "--git-path", "info/attributes")
    attributes_path = Path(raw_path.decode("utf-8", errors="surrogateescape").strip())
    if not attributes_path.is_absolute():
        attributes_path = repo / attributes_path
    try:
        if attributes_path.is_file() and attributes_path.stat().st_size:
            raise ExportError(
                "repository-local info/attributes must be empty or absent because "
                "it can override committed LFS attributes: "
                f"{attributes_path}"
            )
    except OSError as exc:
        raise ExportError(f"could not inspect {attributes_path}: {exc}") from exc


def _lfs_paths(repo: Path, commit: str, paths: list[str]) -> set[str]:
    if not paths:
        return set()
    _reject_local_attribute_overrides(repo)
    encoded = b"\0".join(
        path.encode("utf-8", errors="surrogateescape") for path in paths
    ) + b"\0"
    raw = _git(
        repo,
        "-c", f"core.attributesFile={os.devnull}",
        "check-attr", f"--source={commit}", "-z", "--stdin", "filter",
        input_bytes=encoded,
        environment={"GIT_ATTR_NOSYSTEM": "1"},
    ).split(b"\0")
    fields = raw[:-1] if raw and raw[-1] == b"" else raw
    if len(fields) % 3:
        raise ExportError("git check-attr returned a malformed response")
    return {
        fields[index].decode("utf-8", errors="surrogateescape")
        for index in range(0, len(fields), 3)
        if fields[index + 2] == b"lfs"
    }


def _tree_entries(repo: Path, tree: str) -> dict[str, HeadEntry]:
    entries: dict[str, HeadEntry] = {}
    for raw in _git(repo, "ls-tree", "-r", "-z", tree).split(b"\0"):
        if not raw:
            continue
        try:
            metadata, raw_path = raw.split(b"\t", 1)
            mode, object_type, oid = metadata.decode("ascii").split(" ")
        except (ValueError, UnicodeDecodeError) as exc:
            raise ExportError("git ls-tree returned a malformed response") from exc
        path = raw_path.decode("utf-8", errors="surrogateescape")
        entries[path] = HeadEntry(path=path, oid=oid, mode=mode)
    return entries


def _manifest_paths(repo: Path, tree: str) -> list[str]:
    entries = _tree_entries(repo, tree)
    manifest = entries.get(MANIFEST_PATH)
    if manifest is None:
        raise ExportError(f"reviewed manifest is missing from source tree: {MANIFEST_PATH}")
    if manifest.mode not in {"100644", "100755"}:
        raise ExportError(f"reviewed manifest is not a regular file: {MANIFEST_PATH}")
    return _parse_manifest(_read_blobs(repo, [manifest])[manifest.oid])


def public_manifest_paths(repo: Path, tree: str = "HEAD") -> list[str]:
    """Return the reviewed source paths, validating that every one exists."""
    entries = _tree_entries(repo, tree)
    paths = _manifest_paths(repo, tree)
    for path in paths:
        entry = entries.get(path)
        if entry is None:
            raise ExportError(f"manifest path no longer exists in source tree: {path}")
        if entry.mode not in {"100644", "100755"}:
            raise ExportError(
                f"unsupported manifest entry: {entry.mode} {path}"
            )
    return paths


def _private_paths(repo: Path, tree: str) -> list[str]:
    entries = _tree_entries(repo, tree)
    private_list = entries.get(PRIVATE_LIST_PATH)
    if private_list is None:
        return []
    if private_list.mode not in {"100644", "100755"}:
        raise ExportError(
            f"reviewed private list is not a regular file: {PRIVATE_LIST_PATH}"
        )
    paths = _parse_path_list(
        _read_blobs(repo, [private_list])[private_list.oid], PRIVATE_LIST_PATH
    )
    for path in paths:
        if path not in entries:
            raise ExportError(
                f"private list path no longer exists in source tree: {path}"
            )
    return paths


def unclassified_paths(repo: Path, tree: str = "HEAD") -> list[str]:
    """Return candidates in neither the public manifest nor the private list."""
    return _unclassified(repo, tree, set(_manifest_paths(repo, tree)))


def _unclassified(repo: Path, tree: str, public: set[str]) -> list[str]:
    private = set(_private_paths(repo, tree))
    both = sorted(public & private)
    if both:
        raise ExportError(
            f"path is in both {MANIFEST_PATH} and {PRIVATE_LIST_PATH}: {both[0]}"
        )
    candidates = set(propose_paths(repo, tree))
    excluded = sorted(private - candidates)
    if excluded:
        raise ExportError(
            f"{PRIVATE_LIST_PATH} names a path the export policy already "
            f"excludes: {excluded[0]}"
        )
    return sorted(candidates - public - private)


def _require_classified(repo: Path, tree: str, public: list[str]) -> None:
    unclassified = _unclassified(repo, tree, set(public))
    if unclassified:
        shown = ", ".join(unclassified[:10])
        more = len(unclassified) - 10
        raise ExportError(
            f"{len(unclassified)} candidate path(s) are in neither "
            f"{MANIFEST_PATH} nor {PRIVATE_LIST_PATH}; classify each: {shown}"
            + (f" (and {more} more)" if more > 0 else "")
        )


def _head_entries(repo: Path, commit: str) -> list[HeadEntry]:
    all_entries = _tree_entries(repo, commit)
    manifest_paths = public_manifest_paths(repo, commit)
    _require_classified(repo, commit, manifest_paths)
    selected: list[HeadEntry] = []
    for path in manifest_paths:
        entry = all_entries[path]
        mode, oid = entry.mode, entry.oid
        if mode not in {"100644", "100755"}:
            raise ExportError(
                f"unsupported Git tree entry for public export: "
                f"{mode} {path}"
            )
        public_path = EXPORT_RENAMES.get(path)
        if public_path is not None:
            selected.append(HeadEntry(path=public_path, oid=oid, mode=mode, source_path=path))
        else:
            selected.append(HeadEntry(path=path, oid=oid, mode=mode))
    selected.sort(key=lambda entry: entry.path)
    _validate_portable_paths([entry.path for entry in selected])
    lfs_paths = _lfs_paths(
        repo, commit, [entry.source_path or entry.path for entry in selected]
    )
    if lfs_paths:
        raise ExportError(
            "manifest admits an LFS path: " + sorted(lfs_paths)[0]
        )
    return selected


def propose_paths(repo: Path, tree: str) -> list[str]:
    """Generate review candidates from the legacy broad selection policy."""
    all_entries = _tree_entries(repo, tree)
    candidates = [
        path for path, entry in all_entries.items()
        if _is_proposal_candidate(path) and not _is_denied(path)
        and entry.mode in {"100644", "100755"}
    ]
    if MANIFEST_PATH not in candidates:
        candidates.append(MANIFEST_PATH)
    candidates.sort()
    existing = [path for path in candidates if path in all_entries]
    lfs_paths = _lfs_paths(repo, tree, existing)
    return [path for path in candidates if path not in lfs_paths]


def _read_blobs(repo: Path, entries: list[HeadEntry]) -> dict[str, bytes]:
    requested_oids = list(dict.fromkeys(entry.oid for entry in entries))
    if not requested_oids:
        return {}
    raw = _git(
        repo,
        "cat-file", "--batch",
        input_bytes="".join(f"{oid}\n" for oid in requested_oids).encode("ascii"),
    )
    stream = io.BytesIO(raw)
    blobs: dict[str, bytes] = {}
    for expected_oid in requested_oids:
        header = stream.readline()
        try:
            actual_oid, object_type, raw_size = (
                header.rstrip(b"\n").decode("ascii").split(" ")
            )
            size = int(raw_size)
        except (ValueError, UnicodeDecodeError) as exc:
            raise ExportError("git cat-file returned a malformed response") from exc
        if actual_oid != expected_oid or object_type != "blob" or size < 0:
            raise ExportError(
                f"git cat-file returned an unexpected object for {expected_oid}"
            )
        data = stream.read(size)
        if len(data) != size or stream.read(1) != b"\n":
            raise ExportError("git cat-file returned a truncated response")
        blobs[expected_oid] = data
    if stream.read(1):
        raise ExportError("git cat-file returned unexpected trailing data")
    return blobs


def _public_bytes(source_path: str, data: bytes) -> tuple[bytes, bool]:
    rewritten = data
    for pattern, replacement in PUBLIC_BYTE_REWRITES.get(source_path, ()):
        rewritten, count = pattern.subn(replacement, rewritten)
        if count != 1:
            raise ExportError(
                f"reviewed public rewrite matched {count} times in {source_path}"
            )
    return rewritten, rewritten != data


def _validate_target(target: Path) -> None:
    try:
        _reject_reparse_path(target)
        if target.exists():
            if not target.is_dir():
                raise ExportError(f"target is not a directory: {target}")
            if any(target.iterdir()):
                raise ExportError(f"target directory is not empty: {target}")
    except OSError as exc:
        raise ExportError(f"could not inspect target {target}: {exc}") from exc


def _is_reparse_path(path: Path) -> bool:
    if path.is_symlink():
        return True
    try:
        attributes = getattr(path.lstat(), "st_file_attributes", 0)
    except FileNotFoundError:
        return False
    return bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))


def _reject_reparse_path(path: Path) -> None:
    current = path
    while True:
        if _is_reparse_path(current):
            raise ExportError(f"target path contains a symlink or junction: {current}")
        if current.parent == current:
            return
        current = current.parent


def _prepare_target(target: Path) -> None:
    try:
        if not target.exists():
            target.mkdir(parents=True)
        _validate_target(target)
    except FileExistsError:
        _validate_target(target)
    except OSError as exc:
        raise ExportError(f"could not create target {target}: {exc}") from exc


def _destination_parent(target: Path, relative: str) -> Path:
    parent = target
    try:
        for part in Path(relative).parts[:-1]:
            parent = parent / part
            try:
                parent.mkdir()
            except FileExistsError:
                pass
            if _is_reparse_path(parent) or not parent.is_dir():
                raise ExportError(
                    f"export destination parent is not a plain directory: {parent}"
                )
        _reject_reparse_path(parent)
    except OSError as exc:
        raise ExportError(f"could not prepare destination parent {parent}: {exc}") from exc
    return parent


def _write_exclusive(path: Path, data: bytes, mode: int) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_BINARY", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor: int | None = None
    try:
        descriptor = os.open(path, flags, mode)
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = None
            stream.write(data)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
    except FileExistsError as exc:
        raise ExportError(f"refusing to overwrite export destination: {path}") from exc
    except OSError as exc:
        raise ExportError(f"could not write export destination {path}: {exc}") from exc
    finally:
        if descriptor is not None:
            os.close(descriptor)


def _verify_export_tree(
    target: Path,
    expected_files: set[str],
) -> None:
    expected_directories: set[str] = set()
    for relative in expected_files:
        parent = Path(relative).parent
        while parent != Path("."):
            expected_directories.add(parent.as_posix())
            parent = parent.parent

    actual_files: set[str] = set()
    actual_directories: set[str] = set()
    try:
        _reject_reparse_path(target)
        for path in target.rglob("*"):
            relative = path.relative_to(target).as_posix()
            if _is_reparse_path(path):
                raise ExportError(
                    f"export tree contains a symlink or junction: {relative}"
                )
            if path.is_dir():
                actual_directories.add(relative)
            elif path.is_file():
                actual_files.add(relative)
            else:
                raise ExportError(f"export tree contains a special entry: {relative}")
    except OSError as exc:
        raise ExportError(f"could not verify export tree {target}: {exc}") from exc

    if actual_files != expected_files or actual_directories != expected_directories:
        extras = sorted(
            (actual_files - expected_files)
            | (actual_directories - expected_directories)
        )
        missing = sorted(
            (expected_files - actual_files)
            | (expected_directories - actual_directories)
        )
        detail = []
        if extras:
            detail.append(f"unexpected {extras[0]!r}")
        if missing:
            detail.append(f"missing {missing[0]!r}")
        raise ExportError("export tree changed concurrently: " + ", ".join(detail))


def export_public_tree(repo: Path, target: Path, *, use_index: bool = False) \
        -> dict[str, object]:
    try:
        repo = repo.resolve()
        target = Path(os.path.abspath(target))
    except OSError as exc:
        raise ExportError(f"could not resolve export paths: {exc}") from exc
    source_commit = _git(repo, "rev-parse", "--verify", "HEAD^{commit}").decode(
        "ascii"
    ).strip()
    source_tree = (_git(repo, "write-tree").decode("ascii").strip()
                   if use_index else source_commit)
    entries = _head_entries(repo, source_tree)
    _validate_target(target)
    blobs = _read_blobs(repo, entries)

    files = []
    try:
        _prepare_target(target)
        for entry in entries:
            source_path = entry.source_path or entry.path
            data, transformed = _public_bytes(source_path, blobs[entry.oid])
            parent = _destination_parent(target, entry.path)
            destination = parent / Path(entry.path).name
            _write_exclusive(
                destination,
                data,
                0o755 if entry.mode == "100755" else 0o644,
            )
            record = {
                "mode": entry.mode,
                "path": entry.path,
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            if entry.source_path is not None:
                record["source_path"] = entry.source_path
            if transformed:
                record["transform"] = "public-path-scrub-v1"
            files.append(record)

        paths = [entry.path for entry in entries]
        receipt: dict[str, object] = {
            "source_commit": source_commit,
            "source_kind": "index" if use_index else "commit",
            "source_tree": source_tree,
            "file_count": len(files),
            "paths": paths,
            "files": files,
        }
        expected_files = set(paths)
        _verify_export_tree(target, expected_files)
        _write_exclusive(
            target / RECEIPT_NAME,
            (json.dumps(receipt, indent=2) + "\n").encode("utf-8"),
            0o644,
        )
        _verify_export_tree(target, expected_files | {RECEIPT_NAME})
    except (KeyError, OSError) as exc:
        raise ExportError(
            "export write failed; target may contain an incomplete export without "
            f"a valid receipt: {exc}"
        ) from exc
    return receipt


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == "propose":
        proposal_parser = argparse.ArgumentParser(
            description="Propose exact public-manifest paths for human review"
        )
        proposal_parser.add_argument("propose", nargs="?")
        proposal_parser.add_argument(
            "--repo", type=Path, default=Path.cwd(),
            help="source Git worktree (default: current directory)",
        )
        proposal_parser.add_argument(
            "--index", action="store_true",
            help="propose from the staged index tree instead of HEAD",
        )
        proposal_parser.add_argument(
            "--output", type=Path,
            help="write the proposal to this file instead of stdout",
        )
        proposal_parser.add_argument(
            "--unclassified", action="store_true",
            help=("list only candidates in neither the public manifest nor "
                  "the private list (the paths export would refuse)"),
        )
        args = proposal_parser.parse_args(arguments)
        try:
            tree = (_git(args.repo, "write-tree").decode("ascii").strip()
                    if args.index else "HEAD")
            paths = (unclassified_paths(args.repo, tree) if args.unclassified
                     else propose_paths(args.repo, tree))
            rendered = "".join(path + "\n" for path in paths)
            if args.output is None:
                sys.stdout.write(rendered)
            else:
                args.output.write_text(rendered, encoding="utf-8")
                print(f"proposed {len(paths)} paths in {args.output}")
        except (ExportError, OSError) as exc:
            proposal_parser.exit(1, f"error: {exc}\n")
        return 0

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=Path, help="new or empty export directory")
    parser.add_argument(
        "--repo", type=Path, default=Path.cwd(),
        help="source Git worktree (default: current directory)",
    )
    parser.add_argument(
        "--index", action="store_true",
        help=("export the staged index tree instead of committed HEAD; the "
              "receipt records both its tree ID and the baseline HEAD commit"),
    )
    args = parser.parse_args(arguments)
    try:
        receipt = export_public_tree(args.repo, args.target, use_index=args.index)
    except ExportError as exc:
        parser.exit(1, f"error: {exc}\n")
    print(
        f"exported {receipt['file_count']} files from "
        f"{receipt['source_kind']} tree {receipt['source_tree']} "
        f"(HEAD {receipt['source_commit']}) to {args.target.resolve()}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
