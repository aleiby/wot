#!/usr/bin/env python3
"""Build and verify the guarded macOS first-install disk image.

The image is deliberately outside the updater archive.  Its payload is copied
from the same already-audited ``WoT.app`` that is written to the guarded zip.
Only macOS can inspect the builder-produced HFS+ image; other hosts verify its
unsigned release.json identity as well as the canonical sidecar through
``verify_release.py``.

Uses the stdlib and Pillow at import time; the vendored Finder-layout wheels
are loaded only after their checked-in SHA-256 digests have been verified.
This module runs on the Mac host's Python 3.9.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import release_formats as formats  # noqa: E402
import dmg_background  # noqa: E402
import dmg_layout  # noqa: E402
import mac_signing  # noqa: E402
from wot_config import local_value  # noqa: E402

DMG_SIDECAR_FORMAT = "wot-mac-dmg-v2"
VOLUME_NAME = "WoT Reborn"
APP_NAME = dmg_layout.APP_NAME
APPLICATIONS_TARGET = "/Applications"
SOURCE_ROOT_ALLOWLIST = frozenset({APP_NAME, "Applications"})
DMG_ROOT_ALLOWLIST = frozenset({
    APP_NAME, "Applications", ".DS_Store", ".background", ".VolumeIcon.icns",
})
CREATED_VOLUME_METADATA = frozenset({
    ".DS_Store", ".fseventsd", ".Trashes", ".Spotlight-V100",
})

Runner = Callable[..., subprocess.CompletedProcess]


class DmgError(Exception):
    pass


@contextlib.contextmanager
def _private_temporary(prefix: str, directory: Optional[Path] = None):
    """Use a 0700 directory without traversing a mount that failed to detach."""
    path = Path(tempfile.mkdtemp(
        prefix=prefix, dir=None if directory is None else str(directory)))
    os.chmod(str(path), 0o700)
    try:
        yield path
    finally:
        mounted = [child for child in path.iterdir() if os.path.ismount(str(child))]
        if mounted:
            residue = DmgError(
                "private DMG temp still contains mounted path(s): "
                + ", ".join(str(item) for item in mounted))
            primary = sys.exc_info()[1]
            if primary is not None:
                raise DmgError(f"{primary}; cleanup also failed: {residue}") from primary
            raise residue
        shutil.rmtree(path)


def _run(command: Sequence[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(list(command), capture_output=True, text=True, **kwargs)


def sidecar_name(dmg_name: str) -> str:
    return dmg_name + ".verify.json"


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("utf-8")


def _command(runner: Runner, command: Sequence[str], what: str
             ) -> subprocess.CompletedProcess:
    result = runner(list(command))
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise DmgError(f"{what} failed ({result.returncode}): {detail}")
    return result


def _zip_tree_root(app: Path, temporary: Path, runner: Runner) -> str:
    """Project an on-disk app through the guarded-zip rules.

    This intentionally uses the same ditto flags as build_stripped_pak's Mac
    zip.  Resource forks and extended attributes remain present in the DMG,
    but are not members of the installed-tree contract or updater archive.
    """
    archive = temporary / "mounted-app.zip"
    _command(runner, ["ditto", "-c", "-k", "--norsrc", "--noextattr",
                      "--keepParent", str(app), str(archive)],
             "ditto installed-tree projection")
    try:
        return formats.project_guarded_archive_tree(archive, "Mac").tree_root
    except (OSError, formats.FormatError) as exc:
        raise DmgError(f"cannot project the mounted app's installed tree: {exc}") from exc


def _find_app_icon(app: Path) -> Path:
    """Return the app icon selected by the bundle's Info.plist."""
    app = Path(app)
    resources = app / "Contents" / "Resources"
    try:
        candidates = sorted(resources.glob("*.icns"))
    except OSError as exc:
        raise DmgError(f"cannot enumerate app icons: {exc}") from exc
    if not candidates:
        raise DmgError(f"{app} has no Contents/Resources/*.icns app icon")
    if any(not candidate.is_file() or candidate.is_symlink()
           for candidate in candidates):
        raise DmgError(f"{app} app icon is not a regular file")
    if len(candidates) == 1:
        return candidates[0]

    info_path = app / "Contents" / "Info.plist"
    try:
        info = plistlib.loads(info_path.read_bytes())
    except (OSError, ValueError, plistlib.InvalidFileException) as exc:
        raise DmgError(f"cannot read {info_path} to select the app icon: {exc}") from exc
    icon_name = info.get("CFBundleIconFile") if isinstance(info, dict) else None
    if not isinstance(icon_name, str) or not icon_name:
        raise DmgError(
            f"{app} has multiple .icns files but no CFBundleIconFile selection")
    if Path(icon_name).name != icon_name:
        raise DmgError(f"{app} CFBundleIconFile is not a resource file name")
    if not icon_name.endswith(".icns"):
        icon_name += ".icns"
    selected = resources / icon_name
    if selected not in candidates:
        raise DmgError(
            f"{app} CFBundleIconFile {icon_name!r} is not one of its .icns files")
    return selected


def verify_attached_volume(mountpoint: Path, expected_tree_root: str,
                           expected_background_sha256: str,
                           *, runner: Runner = _run,
                           temporary: Optional[Path] = None,
                           require_gatekeeper: bool = False) -> None:
    """Verify the exact mounted layout, app signature, and installed tree.

    The final compressed image is immutable and must contain exactly the two
    player-facing entries plus the three allowlisted Finder layout entries.
    """
    mountpoint = Path(mountpoint)
    try:
        root_names = {entry.name for entry in os.scandir(mountpoint)}
    except OSError as exc:
        raise DmgError(f"cannot enumerate attached DMG volume: {exc}") from exc
    unexpected = root_names - DMG_ROOT_ALLOWLIST
    missing = DMG_ROOT_ALLOWLIST - root_names
    if missing or unexpected:
        raise DmgError(
            f"DMG volume root layout differs: missing={sorted(missing)}, "
            f"unexpected={sorted(unexpected)}")
    applications = mountpoint / "Applications"
    if not applications.is_symlink() or os.readlink(str(applications)) != APPLICATIONS_TARGET:
        raise DmgError("DMG Applications entry is not a symlink to /Applications")
    app = mountpoint / APP_NAME
    if not app.is_dir() or app.is_symlink():
        raise DmgError(f"DMG {APP_NAME} is not a real app bundle")
    ds_store = mountpoint / ".DS_Store"
    if not ds_store.is_file() or ds_store.is_symlink():
        raise DmgError("DMG .DS_Store is not a regular file")
    background_dir = mountpoint / ".background"
    if not background_dir.is_dir() or background_dir.is_symlink():
        raise DmgError("DMG .background is not a real directory")
    try:
        background_children = {entry.name for entry in os.scandir(background_dir)}
    except OSError as exc:
        raise DmgError(f"cannot enumerate the DMG background directory: {exc}") from exc
    if background_children != {"background.tiff"}:
        raise DmgError(
            "DMG background directory must contain only background.tiff, found "
            f"{sorted(background_children)}")
    background = background_dir / "background.tiff"
    if not background.is_file() or background.is_symlink():
        raise DmgError("DMG background.tiff is not a regular file")
    background_digest, _background_size = formats.sha256_file(background)
    if background_digest != expected_background_sha256:
        raise DmgError("DMG background differs from sidecar background sha256")
    volume_icon = mountpoint / ".VolumeIcon.icns"
    if not volume_icon.is_file() or volume_icon.is_symlink():
        raise DmgError("DMG volume icon is not a regular file")
    app_icon = _find_app_icon(app)
    if formats.sha256_file(volume_icon) != formats.sha256_file(app_icon):
        raise DmgError("DMG volume icon differs from app icon")
    _command(runner, ["codesign", "--verify", "--deep", "--strict", str(app)],
             "codesign verification of attached WoT.app")
    if require_gatekeeper:
        result = _command(
            runner, ["spctl", "-a", "-vv", "-t", "exec", str(app)],
            "Gatekeeper assessment of attached WoT.app")
        output = "%s\n%s" % (result.stdout or "", result.stderr or "")
        if re.search(r"^.+: accepted\s*$", output, flags=re.MULTILINE) is None:
            raise DmgError("Gatekeeper assessment did not report accepted")
    if temporary is None:
        with _private_temporary("wot-dmg-tree-") as work:
            actual_tree_root = _zip_tree_root(app, work, runner)
    else:
        actual_tree_root = _zip_tree_root(app, Path(temporary), runner)
    if actual_tree_root != expected_tree_root:
        raise DmgError(
            "DMG app installed-tree root differs from the Shipping zip: "
            f"{actual_tree_root} != {expected_tree_root}")


def _plist(stdout: object, what: str) -> dict:
    try:
        raw = stdout.encode("utf-8") if isinstance(stdout, str) else stdout
        document = plistlib.loads(raw)
    except (AttributeError, TypeError, ValueError, plistlib.InvalidFileException) as exc:
        raise DmgError(f"{what} did not return a valid plist: {exc}") from exc
    if not isinstance(document, dict):
        raise DmgError(f"{what} did not return a plist dictionary")
    return document


def _device_from_output(result: subprocess.CompletedProcess) -> Optional[str]:
    """Recover a whole-disk device even when hdiutil reports partial failure."""
    text = f"{result.stdout or ''}\n{result.stderr or ''}"
    matches = re.findall(r"/dev/(disk[0-9]+)(?:s[0-9]+)?", text)
    return f"/dev/{matches[0]}" if matches else None


def _attach_topology(result: subprocess.CompletedProcess
                     ) -> tuple[Optional[str], Optional[str], Optional[DmgError]]:
    """Return the exact two-entity HFS+ topology emitted by our builder.

    The builder explicitly requests ``-fs HFS+``.  Accepting APFS here would
    enlarge the verifier grammar beyond the artifact this release lane can
    produce, so the verifier intentionally fails closed on every other image
    topology.
    """
    whole = _device_from_output(result)
    try:
        document = _plist(result.stdout, "hdiutil attach")
        entities = document.get("system-entities")
        if not isinstance(entities, list) or len(entities) != 2:
            count = len(entities) if isinstance(entities, list) else "invalid"
            raise DmgError(
                "only builder-produced HFS+ images are accepted: expected "
                f"exactly two attach entities, found {count}")
        whole_entities = []
        hfs_entities = []
        for entity in entities:
            if not isinstance(entity, dict):
                raise DmgError(
                    "only builder-produced HFS+ images are accepted: "
                    "malformed attach entity")
            device = entity.get("dev-entry")
            if not isinstance(device, str) or not re.fullmatch(
                    r"/dev/disk[0-9]+(?:s[0-9]+)?", device):
                raise DmgError(
                    "only builder-produced HFS+ images are accepted: "
                    "invalid device entry")
            mount = entity.get("mount-point")
            if mount not in (None, ""):
                raise DmgError(
                    f"hdiutil -nomount unexpectedly mounted {device} at {mount!r}")
            if (re.fullmatch(r"/dev/disk[0-9]+", device)
                    and entity.get("potentially-mountable") is False
                    and entity.get("content-hint") == "GUID_partition_scheme"
                    and entity.get("unmapped-content-hint")
                    == "GUID_partition_scheme"):
                whole_entities.append(entity)
            if (re.fullmatch(r"/dev/disk[0-9]+s1", device)
                    and entity.get("potentially-mountable") is True
                    and entity.get("content-hint") == "Apple_HFS"
                    and entity.get("unmapped-content-hint")
                    == "48465300-0000-11AA-AA11-00306543ECAC"
                    and entity.get("volume-kind") == "hfs"):
                hfs_entities.append(entity)
        if len(whole_entities) != 1 or len(hfs_entities) != 1:
            raise DmgError(
                "only builder-produced HFS+ images are accepted: attach "
                "topology is not one GUID whole disk plus one Apple_HFS volume")
        plist_whole = whole_entities[0]["dev-entry"]
        volume_device = hfs_entities[0]["dev-entry"]
        if not volume_device.startswith(plist_whole + "s"):
            raise DmgError(
                "only builder-produced HFS+ images are accepted: HFS+ volume "
                "does not belong to the image-backed whole disk")
        if whole is not None and whole != plist_whole:
            raise DmgError("DMG attach output disagrees on the whole-disk device")
        whole = plist_whole
        return whole, volume_device, None
    except DmgError as exc:
        return whole, None, exc


def _cleanup_device(runner: Runner, whole_device: str) -> Optional[DmgError]:
    first = runner(["hdiutil", "detach", whole_device])
    if first.returncode == 0:
        return None
    forced = runner(["hdiutil", "detach", "-force", whole_device])
    if forced.returncode == 0:
        return None
    first_detail = (first.stderr or first.stdout or "").strip()
    forced_detail = (forced.stderr or forced.stdout or "").strip()
    return DmgError(
        f"DMG detach failed ({first.returncode}): {first_detail}; "
        f"forced detach failed ({forced.returncode}): {forced_detail}")


def _raise_with_cleanup(primary: Optional[BaseException],
                        cleanup: Optional[BaseException]) -> None:
    if primary is not None and cleanup is not None:
        raise DmgError(f"{primary}; cleanup also failed: {cleanup}") from primary
    if primary is not None:
        raise primary
    if cleanup is not None:
        raise cleanup


def _with_attached_image(dmg: Path, mountpoint: Path, readonly: bool,
                         operation: Callable[[Path], None], runner: Runner) -> None:
    """Confine one filesystem to mountpoint and retain its whole disk."""
    command = ["hdiutil", "attach"]
    if readonly:
        command.append("-readonly")
    command.extend(["-nobrowse", "-noautoopen", "-nomount", "-plist", str(dmg)])
    attach = runner(command)
    whole, volume_device, topology_error = _attach_topology(attach)
    primary: Optional[BaseException] = None
    try:
        if attach.returncode != 0:
            detail = (attach.stderr or attach.stdout or "").strip()
            raise DmgError(f"DMG attach failed ({attach.returncode}): {detail}")
        if topology_error is not None:
            raise topology_error
        if whole is None or volume_device is None:
            raise DmgError("DMG attach did not identify its cleanup and volume devices")
        mount_command = ["diskutil", "mount"]
        if readonly:
            mount_command.append("readOnly")
        mount_command.extend(["-mountPoint", str(mountpoint), volume_device])
        _command(runner, mount_command, "explicit DMG volume mount")
        volume = _command(
            runner, ["diskutil", "info", "-plist", volume_device],
            "attached DMG volume inspection")
        volume_document = _plist(volume.stdout, "diskutil info")
        actual_mount = volume_document.get("MountPoint")
        if Path(str(actual_mount)).resolve() != mountpoint.resolve():
            raise DmgError(
                f"attached DMG mount point is {actual_mount!r}, not {str(mountpoint)!r}")
        if volume_document.get("VolumeName") != VOLUME_NAME:
            raise DmgError(
                f"attached DMG volume name is {volume_document.get('VolumeName')!r}, "
                f"not {VOLUME_NAME!r}")
        operation(mountpoint)
    except BaseException as exc:
        primary = exc
    cleanup = _cleanup_device(runner, whole) if whole is not None else None
    _raise_with_cleanup(primary, cleanup)


def verify_dmg(dmg: Path, expected_tree_root: str,
               expected_background_sha256: str,
               *, notarized: bool = False, runner: Runner = _run) -> None:
    """Explicitly mount one volume read-only in a private dir and always detach."""
    dmg = Path(dmg)
    if notarized and sys.platform == "darwin":
        _command(runner, ["xcrun", "stapler", "validate", str(dmg)],
                 "stapled DMG ticket validation")
    with _private_temporary("wot-dmg-verify-") as work_path:
        mountpoint = work_path / "mount"
        mountpoint.mkdir(mode=0o700)

        def inspect(path: Path) -> None:
            verify_attached_volume(path, expected_tree_root,
                                   expected_background_sha256,
                                   runner=runner, temporary=work_path,
                                   require_gatekeeper=(notarized
                                                       and sys.platform == "darwin"))

        _with_attached_image(dmg, mountpoint, True, inspect, runner)


def _remove_created_metadata(mountpoint: Path) -> None:
    for name in CREATED_VOLUME_METADATA:
        path = mountpoint / name
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)
    remaining = {entry.name for entry in os.scandir(mountpoint)}
    expected = SOURCE_ROOT_ALLOWLIST
    if remaining != expected:
        raise DmgError(
            f"writable DMG cleanup left root entries {sorted(remaining)}; "
            f"expected {sorted(expected)}")


def _clean_and_compress(source: Path, output: Path, background: Path,
                        volume_icon: Path, runner: Runner) -> None:
    writable = output.with_name("source-rw.dmg")
    _command(runner, ["hdiutil", "create", "-ov", "-format", "UDRW",
                      "-fs", "HFS+", "-volname", VOLUME_NAME,
                      "-srcfolder", str(source), str(writable)],
             "writable DMG creation")
    mountpoint = output.with_name("metadata-mount")
    mountpoint.mkdir(mode=0o700)
    try:
        def install_layout(path: Path) -> None:
            _remove_created_metadata(path)
            dmg_layout.write_layout(
                path, background=background, volume_icon=volume_icon,
                runner=runner)

        _with_attached_image(writable, mountpoint, False, install_layout, runner)
    finally:
        if mountpoint.exists() and not os.path.ismount(str(mountpoint)):
            shutil.rmtree(mountpoint)
    _command(runner, ["hdiutil", "convert", str(writable), "-format", "UDZO",
                      "-o", str(output)], "clean UDZO DMG conversion")


def build_dmg(app: Path, shipping_zip: Path, output: Path,
              *, runner: Runner = _run) -> dict:
    """Create, read-only verify, and sidecar-bind a guarded Mac DMG."""
    app = Path(app)
    shipping_zip = Path(shipping_zip)
    output = Path(output)
    if not app.is_dir() or app.is_symlink():
        raise DmgError(f"staged app is not a real directory: {app}")
    volume_icon = _find_app_icon(app)
    try:
        projected = formats.project_guarded_archive_tree(shipping_zip, "Mac")
    except (OSError, formats.FormatError) as exc:
        raise DmgError(f"cannot project guarded Mac zip {shipping_zip}: {exc}") from exc
    output.parent.mkdir(parents=True, exist_ok=True)
    # Keep the temporary image on the destination filesystem so publication is
    # one atomic replace even when the release directory is on another volume.
    with _private_temporary(
            ".wot-dmg-build-", output.parent) as work_path:
        source = work_path / "source"
        source.mkdir()
        _command(runner, ["ditto", str(app), str(source / APP_NAME)],
                 "ditto app copy into DMG source")
        os.symlink(APPLICATIONS_TARGET, str(source / "Applications"))
        background = dmg_background.build_background_tiff(
            work_path / "background", runner=runner)
        background_digest, _background_size = formats.sha256_file(background)
        temporary_dmg = work_path / output.name
        _clean_and_compress(
            source, temporary_dmg, background, volume_icon, runner)
        verify_dmg(temporary_dmg, projected.tree_root, background_digest,
                   runner=runner)
        identity = local_value("mac_signing_identity", required=False)
        try:
            notary_auth = mac_signing.notary_auth_args(local_value)
        except mac_signing.MacSigningError as exc:
            raise DmgError(str(exc)) from exc
        notarized = False
        if identity is not None:
            _command(runner, ["codesign", "--force", "--timestamp", "--sign",
                              identity, str(temporary_dmg)],
                     "Developer ID signing of DMG")
            if notary_auth is not None:
                try:
                    mac_signing.notarize(
                        temporary_dmg, notary_auth, runner, work_path)
                    mac_signing.staple(temporary_dmg, runner)
                except mac_signing.MacSigningError as exc:
                    raise DmgError(str(exc)) from exc
                notarized = True
        os.replace(str(temporary_dmg), str(output))
    digest, size = formats.sha256_file(output)
    document = {
        "background_sha256": background_digest,
        "format": DMG_SIDECAR_FORMAT,
        "installed_tree_root": projected.tree_root,
        "name": output.name,
        "notarized": notarized,
        "sha256": digest,
        "size": size,
        "volume_name": VOLUME_NAME,
    }
    sidecar = output.with_name(sidecar_name(output.name))
    sidecar.write_bytes(_canonical_bytes(document))
    return document


def build_from_release(release_json: Path, output_directory: Path,
                       *, runner: Runner = _run) -> dict:
    """Build a DMG directly from an existing guarded Mac release zip.

    This is the proof/recovery entry point; normal packaging calls
    :func:`build_dmg` with the audited staged app before ``release.json`` exists.
    """
    release_json = Path(release_json)
    try:
        document = json.loads(release_json.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        raise DmgError(f"cannot read guarded release {release_json}: {exc}") from exc
    if (not isinstance(document, dict)
            or document.get("format") != formats.GUARDED_RELEASE_FORMAT
            or document.get("platform") != "Mac"):
        raise DmgError(f"{release_json} is not a guarded Mac release")
    configuration = document.get("configuration")
    commit = document.get("git_commit")
    asset = document.get("asset")
    try:
        expected_zip = formats.guarded_install_zip_asset(
            "Mac", str(commit), str(configuration))
        output_name = formats.guarded_install_dmg_asset(
            str(commit), str(configuration))
    except formats.FormatError as exc:
        raise DmgError(str(exc)) from exc
    if asset != expected_zip:
        raise DmgError(f"{release_json} does not name the expected guarded Mac zip")
    archive = release_json.parent / expected_zip
    try:
        digest, size = formats.sha256_file(archive)
    except OSError as exc:
        raise DmgError(f"cannot hash guarded Mac zip {archive}: {exc}") from exc
    if (digest, size) != (document.get("sha256"), document.get("size")):
        raise DmgError(f"{archive} differs from {release_json}")
    output_directory = Path(output_directory)
    output_directory.mkdir(parents=True, exist_ok=True)
    with _private_temporary("wot-dmg-release-") as work:
        extracted = work / "extracted"
        extracted.mkdir()
        _command(runner, ["ditto", "-x", "-k", "--norsrc", "--noextattr",
                          str(archive), str(extracted)],
                 "guarded Mac zip extraction")
        app = extracted / APP_NAME
        if not app.is_dir() or app.is_symlink():
            raise DmgError(f"{archive} did not extract exactly WoT.app")
        unexpected = {entry.name for entry in os.scandir(extracted)} - {APP_NAME}
        if unexpected:
            raise DmgError(f"{archive} extracted unexpected roots {sorted(unexpected)}")
        return build_dmg(app, archive, output_directory / output_name, runner=runner)


def load_sidecar(path: Path) -> dict:
    path = Path(path)
    if not path.is_file() or path.is_symlink():
        raise DmgError(f"{path} is not a regular verification sidecar")
    raw = path.read_bytes()
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeError, ValueError) as exc:
        raise DmgError(f"{path} is not JSON: {exc}") from exc
    fields = {"background_sha256", "format", "installed_tree_root", "name",
              "notarized", "sha256", "size", "volume_name"}
    if not isinstance(document, dict) or set(document) != fields:
        raise DmgError(f"{path} does not have the exact DMG sidecar schema")
    if raw != _canonical_bytes(document):
        raise DmgError(f"{path} is not canonical JSON")
    if document["format"] != DMG_SIDECAR_FORMAT:
        raise DmgError(f"{path} has unknown format {document['format']!r}")
    if document["volume_name"] != VOLUME_NAME:
        raise DmgError(f"{path} volume name is not {VOLUME_NAME!r}")
    if type(document["notarized"]) is not bool:
        raise DmgError(f"{path} notarized is not a boolean")
    if not isinstance(document["size"], int) or isinstance(document["size"], bool) \
            or document["size"] <= 0:
        raise DmgError(f"{path} size is not a positive integer")
    for key in ("background_sha256", "sha256", "installed_tree_root"):
        value = document[key]
        if not isinstance(value, str) or len(value) != 64 \
                or any(character not in "0123456789abcdef" for character in value):
            raise DmgError(f"{path} {key} is not a lower-case sha256")
    expected_sidecar = sidecar_name(str(document["name"]))
    if path.name != expected_sidecar:
        raise DmgError(f"{path.name!r} is not the sidecar for {document['name']!r}")
    return document


def verify_sidecar(dmg: Path, sidecar: Path) -> dict:
    dmg = Path(dmg)
    if not dmg.is_file() or dmg.is_symlink():
        raise DmgError(f"{dmg} is not a regular DMG asset")
    document = load_sidecar(sidecar)
    if dmg.name != document["name"]:
        raise DmgError(f"{dmg.name!r} differs from the DMG sidecar name")
    digest, size = formats.sha256_file(dmg)
    if (digest, size) != (document["sha256"], document["size"]):
        raise DmgError(f"{dmg} differs from its verification sidecar")
    return document


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build")
    build.add_argument("--app", required=True)
    build.add_argument("--shipping-zip", required=True)
    build.add_argument("--output", required=True)
    release = commands.add_parser("build-release")
    release.add_argument("--release-json", required=True)
    release.add_argument("--output-dir", required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("--dmg", required=True)
    verify.add_argument("--sidecar", required=True)
    verify.add_argument("--receipt")
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            result = build_dmg(Path(args.app), Path(args.shipping_zip), Path(args.output))
            print(json.dumps(result, indent=2, sort_keys=True))
        elif args.command == "build-release":
            result = build_from_release(
                Path(args.release_json), Path(args.output_dir))
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            result = verify_sidecar(Path(args.dmg), Path(args.sidecar))
            verify_dmg(Path(args.dmg), result["installed_tree_root"],
                       result["background_sha256"],
                       notarized=result["notarized"])
            if args.receipt:
                receipt = {
                    "checks": ["canonical-sidecar", "name-size-sha256",
                               "readonly-nomount-attach",
                               "builder-hfs-plus-topology",
                               "explicit-private-mount", "volume-name",
                               "exact-root-layout-no-hidden-metadata",
                               "background-sidecar-sha256", "volume-icon-app-binding",
                               "applications-symlink", "codesign-deep-strict",
                               "installed-tree-root", "whole-disk-detach"],
                    "dmg": result,
                    "format": "wot-mac-dmg-proof-v1",
                    "verdict": "pass",
                }
                receipt_path = Path(args.receipt)
                receipt_path.parent.mkdir(parents=True, exist_ok=True)
                receipt_path.write_bytes(_canonical_bytes(receipt))
            print(json.dumps(result, indent=2, sort_keys=True))
            print("mac_dmg: PASS (read-only contents, layout, alias, codesign, tree root)")
    except (DmgError, OSError) as exc:
        print(f"mac_dmg: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
