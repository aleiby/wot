#!/usr/bin/env python3
"""Build deterministic, loopback-only update funnel fixtures.

The fixture keys are the native Test-only seeds: a guarded Test build trusts
exactly their public halves (WoTBootstrapCore TestOverrides), so any other key fails SIGNATURE-INVALID in the update-apply
cell. The seeds are already public in the native tests. Private PEM files
exist only inside a temporary directory and are removed before this function
returns.
The output contains public signed artifacts and a derivation receipt; it is not
a production publish set and does not use ``sign_release.sign_release``.
"""
from __future__ import annotations

import hashlib
import json
import os
import stat
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Dict, Iterable, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402


DERIVATION_FORMAT = "wot-update-funnel-derivation-v1"
RELEASE_IDENTITY_RELATIVES = {
    "Win64": "WoT/Build/WoTRelease/release-identity.json",
    "Mac": "Contents/UE/WoT/Build/WoTRelease/release-identity.json",
}
# Kept for callers/tests that use the historical Win64 constant.
RELEASE_IDENTITY_RELATIVE = RELEASE_IDENTITY_RELATIVES["Win64"]
# The bad variant's fail-after-started marker, where WoT.cpp looks for it.
# codesign refuses loose files at a Mac bundle root ("unsealed contents
# present in the bundle root"), so the Mac marker lives in sealed Resources.
BAD_BUILD_MARKERS = {
    "Win64": "wot-test-fail-after-started",
    "Mac": "Contents/Resources/wot-test-fail-after-started",
}


def bad_build_marker(platform: str) -> str:
    return BAD_BUILD_MARKERS[platform]
ANNOUNCEMENT_PUBLIC_HEX = (
    "ed54a59fb1ac3a51239351362941b868e85a60e3d7b2485d828821dc7a69c279")
RELEASE_PUBLIC_HEX = (
    "14c70c7e0c4c7712756ebbdfd33317be8fdf76358824e636098912ced81c1fb1")
ROOT_PUBLIC_HEX = (
    "ba3c735cdbe4235e6cadf81c412b883a3472d1045faea189f584f8beca46e343")
_PKCS8_ED25519_SEED_PREFIX = bytes.fromhex("302e020100300506032b657004220420")


class FixtureError(RuntimeError):
    """The base package cannot produce trustworthy update fixtures."""


def _write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".partial")
    temporary.write_bytes(data)
    temporary.replace(path)


def _reference(path: Path, *, signature: Path | None = None) -> dict:
    digest, size = formats.sha256_file(path)
    result = {"name": path.name, "sha256": digest, "size": size}
    if signature is not None:
        signature_digest, signature_size = formats.sha256_file(signature)
        result["signature"] = {
            "name": signature.name,
            "sha256": signature_digest,
            "size": signature_size,
        }
    return result


def _seed(start: int) -> bytes:
    return bytes(start + index for index in range(32))


def _write_seed_pem(path: Path, start: int, openssl: str) -> None:
    # openssl writes the PEM from the PKCS#8 DER, so no key armor lives here.
    der = path.with_suffix(".der")
    der.write_bytes(_PKCS8_ED25519_SEED_PREFIX + _seed(start))
    try:
        result = subprocess.run(
            [openssl, "pkey", "-inform", "DER", "-in", str(der),
             "-out", str(path)],
            capture_output=True, text=True)
    finally:
        der.unlink()
    if result.returncode != 0:
        raise FixtureError(
            f"openssl could not encode the test seed: {result.stderr.strip()}")


def _make_test_keys(directory: Path, openssl: str) \
        -> tuple[Dict[str, Path], Dict[str, bytes]]:
    specs = {
        "announcement": (0x11, ANNOUNCEMENT_PUBLIC_HEX),
        "release": (0x51, RELEASE_PUBLIC_HEX),
        "offline_root": (0x91, ROOT_PUBLIC_HEX),
    }
    paths = {}
    public_keys = {}
    for role, (start, expected_hex) in specs.items():
        path = directory / f"{role}.pem"
        _write_seed_pem(path, start, openssl)
        public = crypto.public_key_from_private_pem(openssl, path)
        if public.hex() != expected_hex:
            raise FixtureError(
                f"deterministic {role} seed derived {public.hex()}, not {expected_hex}")
        paths[role] = path
        public_keys[role] = public
    return paths, public_keys


def _tree_entries(root: Path, *, include_modes: bool = False) -> Dict[str, tuple]:
    if not root.is_dir():
        raise FixtureError(f"extracted release tree is missing: {root}")
    result = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        path_stat = path.lstat()
        mode = stat.S_IMODE(path_stat.st_mode)
        if path.is_symlink():
            target = os.readlink(str(path))
            result[relative] = (("symlink", mode, 0, "", target)
                                if include_modes else
                                ("symlink", 0, "", target))
        elif path.is_dir():
            result[relative] = (("directory", mode, 0, "") if include_modes
                                else ("directory", 0, ""))
        elif path.is_file():
            digest, size = formats.sha256_file(path)
            result[relative] = (("file", mode, size, digest) if include_modes
                                else ("file", size, digest))
        else:
            raise FixtureError(
                f"extracted release tree contains a non-file entry: {relative}")
    return result


def _verify_extracted_tree(base_tree: Path,
                           projected: formats.RepackResult,
                           platform: str = "Win64") -> None:
    include_modes = platform == "Mac"
    expected = {
        entry.path: ((entry.type, entry.mode, entry.size, entry.sha256,
                      entry.link_target)
                     if include_modes and entry.type == "symlink" else
                     (entry.type, entry.mode, entry.size, entry.sha256)
                     if include_modes else
                     (entry.type, entry.size, entry.sha256, entry.link_target)
                     if entry.type == "symlink" else
                     (entry.type, entry.size, entry.sha256))
        for entry in projected.entries
    }
    actual = _tree_entries(base_tree, include_modes=include_modes)
    if actual != expected:
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        changed = sorted(path for path in set(actual) & set(expected)
                         if actual[path] != expected[path])
        raise FixtureError(
            "extracted release tree does not match the verified guarded archive "
            f"(missing={missing[:3]}, extra={extra[:3]}, changed={changed[:3]})")


def _identity_bytes(sequence: int, commit: str) -> bytes:
    document = {
        "format": "wot-release-identity-v1",
        "release_commit": commit,
        "release_sequence": sequence,
        "version": 1,
    }
    return formats.canonical_bytes(document, "release identity")


def _write_guarded_tree(source_tree: Path, destination: Path, platform: str,
                        ditto: str = "ditto") -> None:
    if platform == "Mac":
        completed = subprocess.run(
            [ditto, "-c", "-k", "--norsrc", "--noextattr", "--keepParent",
             str(source_tree), str(destination)],
            capture_output=True, text=True)
        if completed.returncode != 0:
            raise FixtureError(
                "ditto could not archive the signed Mac fixture: "
                + (completed.stderr or completed.stdout).strip())
        return
    root = formats.GUARDED_ZIP_ROOTS[platform]
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as archive:
        launcher = zipfile.ZipInfo(
            f"{root}/WoT Reborn.exe", date_time=(1980, 1, 1, 0, 0, 0))
        launcher.create_system = 0
        launcher.external_attr = 0o100666 << 16
        launcher.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(launcher, b"MZ synthetic stable launcher")
        for path in sorted(source_tree.rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(source_tree).as_posix()
            info = zipfile.ZipInfo(
                f"{root}/Game/{relative}", date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 0
            info.external_attr = 0o100666 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with path.open("rb") as source, archive.open(info, "w") as target:
                shutil.copyfileobj(source, target, length=1 << 20)


def _resign_mac_app(app: Path,
                    codesign: str = "codesign") -> dict:
    commands = (
        [codesign, "--force", "--deep", "--sign", "-", str(app)],
        [codesign, "--verify", "--deep", "--strict", str(app)],
    )
    for command in commands:
        completed = subprocess.run(
            command, capture_output=True, text=True)
        if completed.returncode != 0:
            raise FixtureError(
                "Mac fixture code signing failed: "
                + (completed.stderr or completed.stdout).strip())
    return {
        "command": "codesign --verify --deep --strict",
        "artifact": "source-before-ditto",
        "verified": True,
    }


def _verify_archived_mac_app(archive: Path, *, codesign: str = "codesign",
                             ditto: str = "ditto") -> dict:
    """Verify the actual post-ditto app bytes consumed by release staging."""
    with tempfile.TemporaryDirectory(prefix="wot-update-verify-") as temporary:
        extracted = Path(temporary)
        completed = subprocess.run(
            [ditto, "-x", "-k", str(archive), str(extracted)],
            capture_output=True, text=True)
        if completed.returncode != 0:
            raise FixtureError(
                "ditto could not extract the Mac fixture for verification: "
                + (completed.stderr or completed.stdout).strip())
        app = extracted / formats.GUARDED_ZIP_ROOTS["Mac"]
        # The signed installed-tree inventory is an archive-only staging input;
        # the native stager consumes it before the app becomes LIVE.
        for sidecar in formats.INVENTORY_SIDECARS:
            (app / sidecar).unlink(missing_ok=True)
        completed = subprocess.run(
            [codesign, "--verify", "--deep", "--strict", str(app)],
            capture_output=True, text=True)
        if completed.returncode != 0:
            raise FixtureError(
                "post-ditto Mac fixture signature verification failed: "
                + (completed.stderr or completed.stdout).strip())
    return {
        "artifact": "extracted-post-ditto",
        "command": "codesign --verify --deep --strict",
        "verified": True,
    }


def _component(entries: Sequence[formats.TreeEntry], relative: str,
               what: str) -> dict:
    matches = [entry for entry in entries
               if entry.path == relative and entry.type == "file"]
    if len(matches) != 1:
        raise FixtureError(f"{what} is not one file in the derived tree: {relative}")
    entry = matches[0]
    return {"name": entry.path, "sha256": entry.sha256, "size": entry.size}


def _signed_pair(directory: Path, name: str, raw: bytes, signature: bytes) -> dict:
    path = directory / name
    signature_path = directory / (name + formats.SIGNATURE_SUFFIX)
    _write_bytes(path, raw)
    _write_bytes(signature_path, signature)
    return _reference(path, signature=signature_path)


def _build_variant(*, label: str, source_tree: Path, output: Path,
                   base: formats.GuardedRelease, sequence: int,
                   mutations: Iterable[dict], ring_raw: bytes,
                   ring_signature: bytes, keys: Dict[str, Path],
                   public_keys: Dict[str, bytes], openssl: str,
                   compiled: formats.CompiledConstants, contract_id: str,
                   contract_version: int, platform: str,
                   signature_verification: Optional[dict] = None,
                   codesign: str = "codesign", ditto: str = "ditto") -> dict:
    directory = output / label
    directory.mkdir(parents=True)
    guarded = output / f".{label}-guarded.zip"
    _write_guarded_tree(source_tree, guarded, platform, ditto)
    archive_name = formats.update_archive_asset(
        platform, sequence, base.document["git_commit"])
    archive_path = directory / archive_name
    repack = formats.repack_update_archive(guarded, archive_path, platform)
    guarded.unlink()
    if platform == "Mac":
        signature_verification = _verify_archived_mac_app(
            archive_path, codesign=codesign, ditto=ditto)

    identity = formats.read_release_identity_from_zip(
        archive_path, platform, repack.root)
    if (identity.release_sequence, identity.release_commit) != (
            sequence, base.document["git_commit"]):
        raise FixtureError(f"{label} archive carries the wrong release identity")
    main = _component(repack.entries, base.main_relative, "main executable")

    tag = f"wot-funnel-{platform.casefold()}-r{sequence}-{label}"
    archive_digest, archive_size = formats.sha256_file(archive_path)
    manifest_document = {
        "announcement_domain": compiled.announcement_domain,
        "archive": {
            "name": archive_name,
            "sha256": archive_digest,
            "size": archive_size,
            "url": formats.release_download_url(tag, archive_name),
        },
        "archive_root_id": formats.ARCHIVE_ROOT_ID,
        "build_commit": base.document["git_commit"],
        "cache_contract_id": contract_id,
        "cache_contract_version": contract_version,
        "format": formats.RELEASE_MANIFEST_FORMAT,
        "generated_utc": str(base.document["generated_utc"]),
        "human_version": f"update-funnel-r{sequence}-{label}",
        "installed_tree_root": {
            "name": repack.root,
            "sha256": repack.tree_root,
            "size": len(repack.inventory_raw),
        },
        "installed_tree_version": compiled.installed_tree_version,
        "key_id": "update-funnel-release-v1",
        "main_executable": main,
        "manifest_version": compiled.release_manifest_version,
        "minimum_commit_service_protocol": compiled.commit_service_protocol,
        "minimum_gate_protocol": compiled.gate_protocol,
        "minimum_updater_protocol": compiled.updater_protocol,
        "platform": platform,
        "release_sequence": sequence,
        "stripped_audit_verdict": str(base.document["audit"]["verdict"]),
        "target_attestation": {
            "configuration": formats.TEST_CONFIGURATION,
            "dedicated_server_startup_available": False,
            "launch_module": "WoTLaunch",
            "listen_server_present": True,
            "release_gate_enabled": False,
            "target_type": "Game",
        },
    }
    manifest_raw = formats.canonical_bytes(manifest_document, "release manifest")
    manifest_name = formats.manifest_asset(platform)
    ring_ref = formats.ObjectRef(
        formats.release_download_url(tag, formats.KEY_RING_ASSET),
        len(ring_raw), formats.sha256_hex(ring_raw))
    announcement_document = formats.build_announcement(
        platform=platform, domain=compiled.announcement_domain,
        key_id="update-funnel-release-v1", release_sequence=sequence,
        key_ring_epoch=sequence, minimum_gate_protocol=compiled.gate_protocol,
        key_ring=ring_ref,
        manifest=formats.ObjectRef(
            formats.release_download_url(tag, manifest_name),
            len(manifest_raw), formats.sha256_hex(manifest_raw)),
    )
    announcement_raw = formats.canonical_bytes(
        announcement_document, "announcement")
    announcement = formats.parse_announcement(
        announcement_raw, platform, compiled.announcement_domain)
    formats.parse_manifest(manifest_raw, announcement)

    manifest_signature = crypto.sign_framed(
        openssl, keys["release"], crypto.CONTEXT_RELEASE_MANIFEST_V2,
        manifest_raw, public_keys["release"])
    announcement_signature = crypto.sign_framed(
        openssl, keys["announcement"], crypto.CONTEXT_ANNOUNCEMENT_V1,
        announcement_raw, public_keys["announcement"])
    ring_reference = _signed_pair(
        directory, formats.KEY_RING_ASSET, ring_raw, ring_signature)
    manifest_reference = _signed_pair(
        directory, manifest_name, manifest_raw, manifest_signature)
    announcement_name = formats.announcement_asset(platform)
    announcement_reference = _signed_pair(
        directory, announcement_name, announcement_raw, announcement_signature)

    result = {
        "announcement": announcement_reference,
        "archive": _reference(archive_path),
        "directory": label,
        "key_ring": ring_reference,
        "main_executable": main,
        "manifest": manifest_reference,
        "mutations": list(mutations),
        "tag": tag,
        "tree_root": repack.tree_root,
    }
    if signature_verification is not None:
        result["signature_verification"] = signature_verification
    return result


def build_update_fixtures(release_json: Path, extracted_tree: Path,
                          output: Path, *, platform: str = "Win64",
                          openssl: Optional[str] = None,
                          cc: str = "cc",
                          codesign: str = "codesign",
                          ditto: str = "ditto",
                          lipo: str = "lipo") -> Path:
    """Derive signed good/bad N+1 fixtures from one verified Test N."""
    release_json = Path(release_json).resolve()
    extracted_tree = Path(extracted_tree).resolve()
    output = Path(output).resolve()
    if output.exists():
        if not output.is_dir() or any(output.iterdir()):
            raise FixtureError(f"fixture output must be absent or empty: {output}")
        output.rmdir()
    output.parent.mkdir(parents=True, exist_ok=True)
    openssl = crypto.find_openssl(openssl)

    try:
        if platform not in formats.PLATFORMS:
            raise FixtureError(f"unsupported fixture platform {platform!r}")
        base = formats.load_guarded_release(release_json, platform)
        if base.configuration != formats.TEST_CONFIGURATION:
            raise FixtureError("update funnel fixtures require a guarded Test release")
        if base.release_sequence >= formats.INT64_MAX:
            raise FixtureError("base release sequence cannot be incremented")
        projected = formats.project_guarded_archive_tree(base.zip_path, platform)
        _verify_extracted_tree(extracted_tree, projected, platform)
        product_receipt = base.document["products"]["receipt"]
        product_receipt_raw = formats.canonical_bytes(
            product_receipt, "guarded product receipt")
        next_sequence = base.release_sequence + 1
        compiled = formats.load_compiled_constants()
        contract_id, contract_version = formats.load_cache_contract_identity()

        with tempfile.TemporaryDirectory(prefix="wot-update-funnel-keys-") as key_temp:
            keys, public_keys = _make_test_keys(Path(key_temp), openssl)
            release_key = formats.ReleaseKey(
                "update-funnel-release-v1", public_keys["release"],
                next_sequence, next_sequence)
            ring_document = formats.build_key_ring(
                next_sequence, [release_key], [])
            ring_raw = formats.canonical_bytes(ring_document, "release key ring")
            ring_signature = crypto.sign_framed(
                openssl, keys["offline_root"],
                crypto.CONTEXT_RELEASE_KEY_RING_V1, ring_raw,
                public_keys["offline_root"])

            with tempfile.TemporaryDirectory(
                    prefix=".update-funnel-stage-", dir=output.parent) as stage_temp:
                stage = Path(stage_temp) / output.name
                stage.mkdir()
                identity_relative = RELEASE_IDENTITY_RELATIVES[platform]
                identity_mutation = {
                    "operation": "rewrite-release-identity",
                    "path": identity_relative,
                }

                def variant_source(label: str) -> Path:
                    container = Path(stage_temp) / (label + "-source")
                    container.mkdir()
                    source = container / extracted_tree.name
                    shutil.copytree(extracted_tree, source, symlinks=True)
                    identity_path = source / Path(identity_relative)
                    if not identity_path.is_file():
                        raise FixtureError(
                            f"base tree has no {identity_relative}")
                    identity_path.write_bytes(_identity_bytes(
                        next_sequence, base.document["git_commit"]))
                    return source

                good_source = variant_source("good")
                good_verification = (_resign_mac_app(
                    good_source, codesign)
                    if platform == "Mac" else None)
                good = _build_variant(
                    label="good", source_tree=good_source, output=stage, base=base,
                    sequence=next_sequence, mutations=[identity_mutation],
                    ring_raw=ring_raw, ring_signature=ring_signature, keys=keys,
                    public_keys=public_keys, openssl=openssl, compiled=compiled,
                    contract_id=contract_id, contract_version=contract_version,
                    platform=platform,
                    signature_verification=good_verification,
                    codesign=codesign, ditto=ditto)

                bad_source = variant_source("bad")
                marker = bad_build_marker(platform)
                (bad_source / marker).parent.mkdir(parents=True, exist_ok=True)
                (bad_source / marker).write_bytes(b"fail after shared profile hold and started\n")
                bad_mutation = {"operation": "fail-after-started", "path": marker}
                bad_verification = (_resign_mac_app(
                    bad_source, codesign)
                    if platform == "Mac" else None)
                bad = _build_variant(
                    label="bad", source_tree=bad_source, output=stage, base=base,
                    sequence=next_sequence,
                    mutations=[identity_mutation, bad_mutation],
                    ring_raw=ring_raw, ring_signature=ring_signature, keys=keys,
                    public_keys=public_keys, openssl=openssl, compiled=compiled,
                    contract_id=contract_id, contract_version=contract_version,
                    platform=platform,
                    signature_verification=bad_verification,
                    codesign=codesign, ditto=ditto)
                if good["tree_root"] == bad["tree_root"]:
                    raise FixtureError("good and bad fixture tree roots are identical")

                archive_digest, archive_size = formats.sha256_file(base.zip_path)
                base_main = _component(
                    projected.entries, base.main_relative, "base main executable")
                receipt = {
                    "base": {
                        "archive": {
                            "name": base.zip_path.name,
                            "sha256": archive_digest,
                            "size": archive_size,
                        },
                        "commit": base.document["git_commit"],
                        "main_executable": base_main,
                        "product_receipt_sha256": formats.sha256_hex(
                            product_receipt_raw),
                        "release_json_sha256": base.release_json_sha256,
                        "release_sequence": base.release_sequence,
                        "tree_root": projected.tree_root,
                    },
                    "format": DERIVATION_FORMAT,
                    "next_release": {
                        "release_commit": base.document["git_commit"],
                        "release_sequence": next_sequence,
                    },
                    "platform": platform,
                    "test_public_keys": {
                        role: public_keys[role].hex()
                        for role in ("announcement", "offline_root", "release")
                    },
                    "variants": {"bad": bad, "good": good},
                }
                receipt_path = stage / "update-fixture-derivation-v1.json"
                _write_bytes(
                    receipt_path,
                    json.dumps(receipt, indent=2, sort_keys=True).encode("utf-8")
                    + b"\n")
                stage.replace(output)
        return output / "update-fixture-derivation-v1.json"
    except FixtureError:
        raise
    except (formats.FormatError, crypto.CryptoError, OSError,
            ValueError, zipfile.BadZipFile) as exc:
        raise FixtureError(str(exc)) from exc


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="derive loopback-only Mac/Win64 N+1 update fixtures")
    parser.add_argument("--platform", choices=formats.PLATFORMS, required=True)
    parser.add_argument("--release", required=True, type=Path)
    parser.add_argument("--extracted-tree", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--openssl")
    args = parser.parse_args()
    try:
        receipt = build_update_fixtures(
            args.release, args.extracted_tree, args.output,
            platform=args.platform, openssl=args.openssl)
    except FixtureError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(receipt)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
