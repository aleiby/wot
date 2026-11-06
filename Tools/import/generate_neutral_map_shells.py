#!/usr/bin/env python3
"""Generate the nineteen retail-independent Task-28 map shells.

The destination is a same-job physical root, never persistent project Content.
UE 5.8 exposes no Python mount-point API, so maps are saved under the temporary
``/Game/WoTNeutralShellStaging`` package path, copied without replacement to the
destination, and removed from staging.  The listen harness supplies the final
``/WoTNeutralShells`` mount when it loads the files.

PythonScriptCommandlet forwards script arguments only when they are part of the
``-script`` value.  Invoke this as, for example::

    UnrealEditor-Cmd WoT.uproject -run=pythonscript -script="/abs/generate_neutral_map_shells.py --spec /abs/spec.json --output-root /abs/NeutralMapShells ... --require-empty-output --no-replace"

Engine flags such as ``-unattended`` remain outside that quoted value.  The two
receipts are deliberately separate: one records every Python-visible input open
and the denied roots, while the other binds the exact generated package bytes.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import sys
from pathlib import Path
from typing import Any


FORMAT = "neutral-map-shells-v1"
RECEIPT_VERSION = 2
PACKAGE_ROOT = "/WoTNeutralShells"
STAGING_PACKAGE_ROOT = "/Game/WoTNeutralShellStaging"
GENERATOR_IDENTITY = "generate-neutral-map-shells-v1"
SHELL_NAMES = tuple(f"WoTMapShell{slot:02d}" for slot in range(1, 20))
MANDATED_DENIED_ROOT_NAMES = ("Content", "original", "devtree", "exports")
_AUDIT_INSTALLED = False
_AUDIT_POLICY: tuple[
    tuple[Path, ...], Path, set[Path], set[Path],
] | None = None


class GenerationFailed(RuntimeError):
    """The requested root cannot be proven fresh, isolated, and complete."""


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
    ).encode("ascii")


def _digest_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _resolved(path: Path) -> Path:
    return Path(os.path.realpath(os.path.abspath(os.fspath(path))))


def _same_or_contains(left: Path, right: Path) -> bool:
    try:
        return os.path.commonpath((os.fspath(left), os.fspath(right))) in {
            os.fspath(left), os.fspath(right),
        }
    except ValueError:
        return False


def _write_new_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # O_BINARY: without it the Windows CRT opens the descriptor in text mode
    # and rewrites the byte stream (LF -> CRLF), breaking the canonical-bytes
    # receipt contract. No-op off Windows.
    flags = (os.O_WRONLY | os.O_CREAT | os.O_EXCL
             | getattr(os, "O_BINARY", 0))
    descriptor = os.open(path, flags, 0o600)
    try:
        data = _canonical_bytes(document) + b"\n"
        offset = 0
        while offset < len(data):
            offset += os.write(descriptor, data[offset:])
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _read_spec(path: Path, opened: list[dict[str, str]]) -> tuple[dict, str]:
    raw = path.read_bytes()
    opened.append({"path": os.fspath(path), "sha256": _digest_bytes(raw)})
    try:
        document = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise GenerationFailed(f"invalid neutral-shell spec: {exc}") from exc
    if raw.rstrip(b"\r\n") != _canonical_bytes(document):
        raise GenerationFailed("neutral-shell spec is not canonical JSON")
    expected_keys = {
        "format", "materialization_host", "neutral_allow_list",
        "package_root", "shells",
    }
    if not isinstance(document, dict) or set(document) != expected_keys:
        raise GenerationFailed("neutral-shell spec has a non-canonical shape")
    if document["format"] != FORMAT or document["package_root"] != PACKAGE_ROOT:
        raise GenerationFailed("neutral-shell format or package root mismatched")
    allow = document["neutral_allow_list"]
    if (allow != [
            "/Script/Engine.Brush",
            "/Script/Engine.DefaultPhysicsVolume",
            "/Script/Engine.WorldSettings",
            "/Script/NavigationSystem.AbstractNavData",
            "/Script/WoT.WoTGameEngine",
        ] or document["materialization_host"] not in allow):
        raise GenerationFailed("neutral allow-list or materialization host drifted")
    shells = document["shells"]
    if not isinstance(shells, list) or len(shells) != len(SHELL_NAMES):
        raise GenerationFailed("neutral-shell spec must contain exactly nineteen shells")
    for slot, shell in enumerate(shells, 1):
        name = SHELL_NAMES[slot - 1]
        expected = {
            "name": name,
            "package": f"{PACKAGE_ROOT}/{name}",
            "persistent_level": {
                "actors": [], "object_paths": [], "soft_object_paths": [],
            },
            "slot": slot,
        }
        if shell != expected:
            raise GenerationFailed(f"neutral shell {slot} is not the reviewed empty slot")
    # The receipt binds the checked-in artifact, including its permitted final
    # newline, rather than a second serialization of the parsed document.  The
    # opened-input row already uses this same raw-byte identity.
    return document, _digest_bytes(raw)


def _read_generator_identity(opened: list[dict[str, str]]) -> str:
    path = _resolved(Path(__file__))
    raw = path.read_bytes()
    digest = _digest_bytes(raw)
    opened.append({"path": os.fspath(path), "sha256": digest})
    return digest


def _validate_roots(output_root: Path, disjoint_roots: list[Path],
                    denied_roots: list[Path], require_empty: bool) -> None:
    if not require_empty:
        raise GenerationFailed("generation requires --require-empty-output")
    if output_root == Path(output_root.anchor):
        raise GenerationFailed("the filesystem root cannot be an output root")
    for reserved in disjoint_roots:
        if _same_or_contains(output_root, reserved):
            raise GenerationFailed(
                f"output root overlaps reserved sibling root: {reserved}")
    for denied in denied_roots:
        if _same_or_contains(output_root, denied):
            raise GenerationFailed(f"output root overlaps denied read root: {denied}")
    if output_root.exists():
        raise GenerationFailed("output root must be absent before atomic creation")
    presented = {path.name.casefold() for path in denied_roots}
    missing = [name for name in MANDATED_DENIED_ROOT_NAMES
               if name.casefold() not in presented]
    if missing:
        raise GenerationFailed(
            "missing mandated explicit denied roots: " + ", ".join(missing))


def _create_output_root(output_root: Path) -> os.stat_result:
    """Create the proven-absent leaf with one native no-replace operation."""
    output_root.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.mkdir(output_root, 0o700)
    except FileExistsError as exc:
        raise GenerationFailed(
            "output root appeared before atomic creation") from exc
    return output_root.stat()


def _install_open_audit(denied_roots: tuple[Path, ...], output_root: Path,
                        opened_paths: set[Path],
                        generated_paths: set[Path]) -> None:
    global _AUDIT_INSTALLED, _AUDIT_POLICY

    def audit(event: str, arguments: tuple[Any, ...]) -> None:
        policy = _AUDIT_POLICY
        if policy is None:
            return
        if event != "open" or not arguments:
            return
        (active_denied_roots, active_output_root, active_opened_paths,
         active_generated_paths) = policy
        candidate = arguments[0]
        if not isinstance(candidate, (str, bytes, os.PathLike)):
            return
        try:
            resolved = _resolved(Path(os.fsdecode(candidate)))
        except (OSError, TypeError, ValueError):
            return
        if (resolved == active_output_root
                or active_output_root in resolved.parents):
            return
        # These exact paths are outputs authored during this invocation.  They
        # are allowed only after the reviewed spec and generator have been
        # read, and do not weaken the denial of every other Content path.
        if resolved in active_generated_paths:
            return
        folded_parts = {part.casefold() for part in resolved.parts}
        folded_path = resolved.as_posix().casefold()
        if (folded_parts & {
                "content", "original", "devtree", "exports", "cache",
                "cache-v3", "installers", "generatedcontent",
        } or "wot-world-v3" in resolved.name.casefold()
                or "/cache/world/" in folded_path):
            raise GenerationFailed(f"denied input open (class): {resolved}")
        for denied in active_denied_roots:
            if resolved == denied or denied in resolved.parents:
                raise GenerationFailed(f"denied input open: {resolved}")
        active_opened_paths.add(resolved)

    _AUDIT_POLICY = (
        denied_roots, output_root, opened_paths, generated_paths,
    )
    if not _AUDIT_INSTALLED:
        sys.addaudithook(audit)
        _AUDIT_INSTALLED = True


def _clear_open_audit() -> None:
    global _AUDIT_POLICY
    _AUDIT_POLICY = None


def _asset_filename(output_root: Path, name: str) -> Path:
    return output_root / f"{name}.umap"


def _copy_file_new(source: Path, destination: Path) -> None:
    """Copy one generated package with O_EXCL no-replace semantics."""
    # O_BINARY on both descriptors: the Windows CRT otherwise runs text-mode
    # translation over the raw read/write loop and corrupts the binary .umap
    # (the sealed-copy byte compare below caught exactly that). No-op off
    # Windows.
    binary = getattr(os, "O_BINARY", 0)
    source_descriptor = os.open(source, os.O_RDONLY | binary)
    destination_descriptor: int | None = None
    destination_created = False
    try:
        mode = os.fstat(source_descriptor).st_mode & 0o777
        destination_descriptor = os.open(
            destination,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | binary, mode or 0o600)
        destination_created = True
        while True:
            block = os.read(source_descriptor, 1024 * 1024)
            if not block:
                break
            offset = 0
            while offset < len(block):
                offset += os.write(destination_descriptor, block[offset:])
        os.fsync(destination_descriptor)
    except BaseException:
        if destination_descriptor is not None:
            os.close(destination_descriptor)
            destination_descriptor = None
        if destination_created and destination.exists():
            destination.unlink()
        raise
    finally:
        os.close(source_descriptor)
        if destination_descriptor is not None:
            os.close(destination_descriptor)


def _project_content_root(unreal: Any) -> Path:
    relative = unreal.Paths.project_content_dir()
    return _resolved(Path(unreal.Paths.convert_relative_path_to_full(relative)))


def _remove_staged_file(filename: Path) -> None:
    """Filesystem removal seam; patched by tests to simulate cleanup failure."""
    filename.unlink()


def _cleanup_staging(unreal: Any, rows: list[tuple[str, Path]],
                     staging_root: Path, remove_root: bool) -> list[str]:
    """Release staged worlds and remove every staged file from disk.

    Measured on the UE 5.8 Mac commandlet: EditorAssetLibrary.delete_asset
    returns True while the package remains loadable, and delete_directory
    returns False outright — in-editor deletion is unreliable there.  The
    enforced census is therefore the DISK: staged .umap files must be gone
    and the staging root empty/removed.  Registry/in-memory residue is
    process-local, dies with the commandlet, and cannot contaminate the
    delivered output root, so the in-editor calls below are best-effort
    releases only.
    """
    assets = unreal.EditorAssetLibrary
    saving = unreal.EditorLoadingAndSavingUtils
    failures: list[str] = []

    # Best-effort: detach whichever staged world is current, then release
    # editor references so the files below are not write-locked.
    try:
        if not bool(saving.load_map("/Engine/Maps/Entry")):
            saving.new_blank_map(False)
    except Exception:
        try:
            saving.new_blank_map(False)
        except Exception as exc:
            failures.append(f"could not switch away from a staged map: {exc}")
    for package, _filename in rows:
        try:
            if assets.does_asset_exist(package):
                assets.delete_asset(package)
        except Exception:
            pass
    try:
        unreal.SystemLibrary.collect_garbage()
    except Exception as exc:
        failures.append(f"could not collect garbage after staging release: {exc}")

    for package, filename in rows:
        try:
            if filename.exists():
                _remove_staged_file(filename)
            if filename.exists():
                failures.append(f"staged file remains after cleanup: {package}")
        except OSError as exc:
            failures.append(f"could not remove staged file {package}: {exc}")

    if staging_root.exists():
        try:
            disk_residue = list(staging_root.iterdir())
        except OSError as exc:
            failures.append(f"could not inspect temporary staging root: {exc}")
        else:
            if disk_residue:
                failures.append(
                    f"temporary staging root is not empty: {staging_root}")
            elif remove_root:
                try:
                    staging_root.rmdir()
                except OSError as exc:
                    failures.append(
                        f"could not remove temporary staging root: {exc}")
    return failures


def _actor_class_path(actor: Any) -> str:
    """Return an actor's exact Unreal class path, failing closed."""
    try:
        actor_class = actor.get_class()
        class_path = actor_class.get_path_name()
    except Exception as exc:
        raise GenerationFailed(
            f"could not census neutral shell actor exact class: {exc}") from exc
    if not isinstance(class_path, str) or not class_path:
        raise GenerationFailed("neutral shell actor has no exact class path")
    return class_path


def _actor_path(actor: Any) -> str:
    try:
        return str(actor.get_path_name())
    except Exception:
        return repr(actor)


def _all_level_actors(unreal: Any, world: Any) -> list[Any]:
    """Enumerate every actor, including editor-hidden structural actors."""
    try:
        return list(unreal.GameplayStatics.get_all_actors_of_class(
            world, unreal.Actor))
    except Exception as exc:
        raise GenerationFailed(
            f"could not census all neutral shell actors: {exc}") from exc


def _sweep_non_neutral_actors(unreal: Any, world: Any,
                              allowed_class_paths: set[str],
                              package: str) -> Any:
    """Remove non-allow-listed actors and prove the saved census is neutral."""
    world_settings = world.get_world_settings()
    if world_settings is None:
        raise GenerationFailed(f"new shell {package} has no WorldSettings")

    for actor in _all_level_actors(unreal, world):
        # WorldSettings is structural and must never be destroyed.  Its exact
        # class is still checked by the post-sweep census below.
        if (actor != world_settings
                and _actor_class_path(actor) not in allowed_class_paths):
            try:
                actor.destroy_actor()
            except Exception as exc:
                raise GenerationFailed(
                    f"failed to destroy non-neutral shell actor "
                    f"{_actor_path(actor)}: {exc}") from exc

    remaining = _all_level_actors(unreal, world)
    world_settings_count = sum(actor == world_settings for actor in remaining)
    unexpected = []
    for actor in remaining:
        class_path = _actor_class_path(actor)
        if class_path not in allowed_class_paths:
            unexpected.append(f"{_actor_path(actor)} ({class_path})")
    if unexpected:
        raise GenerationFailed(
            f"post-sweep census for {package} contains non-allow-listed "
            f"exact classes: {', '.join(unexpected)}")
    if world_settings_count != 1:
        raise GenerationFailed(
            f"post-sweep census for {package} has {world_settings_count} "
            "WorldSettings actors")
    return world_settings


def _generate(unreal: Any, output_root: Path, spec: dict[str, Any],
              generated_audit_paths: set[Path]) -> list[Path]:
    content_root = _project_content_root(unreal)
    if _same_or_contains(output_root, content_root):
        raise GenerationFailed("output root overlaps persistent project Content")
    staging_root = content_root / STAGING_PACKAGE_ROOT.removeprefix("/Game/")
    staging_root_existed = staging_root.exists()
    if staging_root_existed and not staging_root.is_dir():
        raise GenerationFailed("temporary staging root exists and is not a directory")

    staging_rows = [
        # The leaf must stay identical to its canonical receipt/load name;
        # only the temporary package root changes during authoring.
        (f"{STAGING_PACKAGE_ROOT}/{name}", staging_root / f"{name}.umap")
        for name in SHELL_NAMES
    ]
    for package, filename in staging_rows:
        if filename.exists() or unreal.EditorAssetLibrary.does_asset_exist(package):
            raise GenerationFailed(
                f"no-replace refused existing staging shell {package}")
    if staging_root_existed and any(staging_root.iterdir()):
        raise GenerationFailed(
            "no-replace refused non-empty temporary staging root")
    generated_audit_paths.update(filename for _package, filename in staging_rows)

    assets = unreal.EditorAssetLibrary
    saving = unreal.EditorLoadingAndSavingUtils
    allowed_class_paths = set(spec["neutral_allow_list"])
    generated: list[Path] = []
    world = None
    world_settings = None
    try:
        if len(spec["shells"]) != len(staging_rows):
            raise GenerationFailed("shell specification and staging rows differ")
        for shell, (staging_package, staging_filename) in zip(
                spec["shells"], staging_rows):
            package = shell["package"]
            name = shell["name"]
            filename = _asset_filename(output_root, name)
            if filename.exists():
                raise GenerationFailed(f"no-replace refused existing shell {package}")
            world = saving.new_blank_map(False)
            if world is None:
                raise GenerationFailed(f"failed to create empty shell {package}")
            world_settings = _sweep_non_neutral_actors(
                unreal, world, allowed_class_paths, package)
            if not saving.save_map(world, staging_package):
                raise GenerationFailed(f"failed to save empty shell {package}")
            if (not staging_filename.is_file()
                    or not assets.does_asset_exist(staging_package)):
                raise GenerationFailed(
                    f"saved shell {package} is not in temporary staging")
            _copy_file_new(staging_filename, filename)
            if not filename.is_file():
                raise GenerationFailed(f"saved shell {package} is not at the sealed root")
            generated.append(filename)

        if len(generated) != len(SHELL_NAMES):
            raise GenerationFailed(
                "not all sealed shell copies were verified before staging cleanup")
        if len(staging_rows) != len(generated):
            raise GenerationFailed("staging and generated shell rows differ")
        for (_package, staging_filename), filename in zip(
                staging_rows, generated):
            if (not staging_filename.is_file() or not filename.is_file()
                    or staging_filename.read_bytes() != filename.read_bytes()):
                raise GenerationFailed(
                    f"sealed shell copy does not match staging: {filename.name}")
    except BaseException as exc:
        # Do not let Python locals keep the last saved UWorld or its objects
        # referenced while EditorAssetLibrary attempts to remove its package.
        world_settings = None
        world = None
        cleanup_failures = _cleanup_staging(
            unreal, staging_rows, staging_root, not staging_root_existed)
        if cleanup_failures:
            raise GenerationFailed("; ".join(cleanup_failures)) from exc
        raise

    # All nineteen packages are now independently present at the sealed output
    # root.  Release the final saved world before switching maps and deleting
    # the staging package directory as one operation.
    world_settings = None
    world = None
    cleanup_failures = _cleanup_staging(
        unreal, staging_rows, staging_root, not staging_root_existed)
    if cleanup_failures:
        raise GenerationFailed("; ".join(cleanup_failures))
    return generated


def generate(args: argparse.Namespace, unreal: Any) -> tuple[dict, dict]:
    spec_path = _resolved(args.spec)
    output_root = _resolved(args.output_root)
    denied_receipt = _resolved(args.denied_read_open_receipt)
    identity_receipt = _resolved(args.output_identity_receipt)
    denied_roots = tuple(_resolved(path) for path in args.deny_read_root)
    disjoint_roots = [_resolved(path) for path in args.require_disjoint_from]
    if (denied_receipt == identity_receipt
            or output_root == denied_receipt or output_root in denied_receipt.parents
            or output_root == identity_receipt or output_root in identity_receipt.parents
            or output_root == spec_path or output_root in spec_path.parents):
        raise GenerationFailed("spec, output root, and receipt paths must be separate")
    if denied_receipt.exists() or identity_receipt.exists():
        raise GenerationFailed("receipt no-replace refused a stale evidence file")
    if not args.no_replace:
        raise GenerationFailed("generation requires --no-replace")
    _validate_roots(output_root, disjoint_roots, list(denied_roots),
                    args.require_empty_output)

    root_began_absent = not output_root.exists()

    opened: list[dict[str, str]] = []
    audited: set[Path] = set()
    generated_audit_paths: set[Path] = set()
    _install_open_audit(
        denied_roots, output_root, audited, generated_audit_paths)
    try:
        generator_digest = _read_generator_identity(opened)
        spec, spec_digest = _read_spec(spec_path, opened)
    except BaseException:
        _clear_open_audit()
        raise
    root_stat = _create_output_root(output_root)
    try:
        packages = _generate(
            unreal, output_root, spec, generated_audit_paths)
        package_rows = []
        census_hasher = hashlib.sha256()
        for slot, package_path in enumerate(packages, 1):
            data = package_path.read_bytes()
            digest = _digest_bytes(data)
            relative = package_path.relative_to(output_root).as_posix()
            row = {
                "bytes": len(data), "package": f"{PACKAGE_ROOT}/{SHELL_NAMES[slot - 1]}",
                "relative_path": relative, "sha256": digest, "slot": slot,
            }
            package_rows.append(row)
            census_hasher.update(_canonical_bytes(row))
        if len(package_rows) != 19:
            raise GenerationFailed("generator did not seal exactly nineteen packages")
        opened_by_path = {row["path"]: row for row in opened}
        for path in sorted(audited):
            if path == output_root or output_root in path.parents or not path.is_file():
                continue
            raw = path.read_bytes()
            opened_by_path[os.fspath(path)] = {
                "path": os.fspath(path), "sha256": _digest_bytes(raw),
            }
        opened_rows = sorted(opened_by_path.values(), key=lambda row: row["path"])
        common = {
            "generator_identity": GENERATOR_IDENTITY,
            "generator_source_identity": os.fspath(_resolved(Path(__file__))),
            "generator_sha256": generator_digest,
            "platform": platform.system(),
            "root_identity": os.fspath(output_root),
            "root_native_identity": {
                "device": int(root_stat.st_dev),
                "inode": int(root_stat.st_ino),
            },
            "spec_identity": os.fspath(spec_path),
            "spec_sha256": spec_digest,
            "version": RECEIPT_VERSION,
        }
        denied_document = {
            **common,
            "denied_roots": [os.fspath(path) for path in denied_roots],
            "denied_path_classes": [
                "Content", "original", "devtree", "exports", "cache",
                "retail-installers", "prior-GeneratedContent", "world-v3",
            ],
            "format": "neutral-map-shells-denied-read-open-receipt-v2",
            "opened_inputs": opened_rows,
            "io_audit_scope": "python-audit-open-events-only",
            "native_ue_io_audited": False,
            "native_ue_io_limitation": (
                "Python audit hooks cannot observe native Unreal package, map, "
                "asset-registry, or filesystem I/O; native UE reads are not "
                "claimed by this receipt."
            ),
            "python_audited_opens": sorted(
                os.fspath(path) for path in audited
                if path != spec_path and path.is_file()),
            "violations": [],
        }
        identity_document = {
            **common,
            "format": "neutral-map-shells-output-identity-receipt-v2",
            "package_census_sha256": census_hasher.hexdigest(),
            "package_count": len(package_rows),
            "packages": package_rows,
            "reserved_disjoint_roots": [os.fspath(path) for path in disjoint_roots],
            "root_began_absent": root_began_absent,
        }
        _write_new_json(denied_receipt, denied_document)
        _write_new_json(identity_receipt, identity_document)
        _clear_open_audit()
        return denied_document, identity_document
    except BaseException:
        _clear_open_audit()
        # Both roots were proven absent/empty before generation, so cleanup is
        # narrowly scoped and cannot erase caller data.
        if output_root.exists():
            shutil.rmtree(output_root)
        for receipt in (denied_receipt, identity_receipt):
            if receipt.exists():
                receipt.unlink()
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spec", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--denied-read-open-receipt", required=True, type=Path)
    parser.add_argument("--output-identity-receipt", required=True, type=Path)
    parser.add_argument("--deny-read-root", action="append", default=[], type=Path)
    parser.add_argument("--require-disjoint-from", action="append", default=[], type=Path)
    parser.add_argument("--require-empty-output", action="store_true")
    parser.add_argument("--no-replace", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args, _unreal_arguments = build_parser().parse_known_args(argv)
    try:
        import unreal
        _denied, identity = generate(args, unreal)
    except (GenerationFailed, OSError, ValueError) as exc:
        print(f"neutral map shell generation failed: {exc}", file=sys.stderr)
        return 1
    unreal.log(
        "NEUTRAL-MAP-SHELLS-OK "
        f"count={identity['package_count']} root={identity['root_identity']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
