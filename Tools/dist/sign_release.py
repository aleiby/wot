#!/usr/bin/env python3
"""Build and sign a release publish set from the guarded packaging outputs.

Two subcommands, matching the two key-custody levels:

  ring   (rare; offline-root key)  sign a wot-release-key-ring-v1 naming the
         release keys, as WoT-root-key-ring-v1.json + .sig.
  sign   (every release; announcement + release keys)  turn one guarded
         Shipping release.json + zip per platform into the complete publish set.

Only a guarded SHIPPING release is signable by default: release-manifest-v2's
target_attestation.configuration is const "Shipping" and a Test binary carries
the WOT_BOOTSTRAP_TEST_* seams (WoT.Target.cs:41-49; Test
bytes are never release bytes). Each Shipping release must also carry the
test_twin binding to the same-ISO Test release and, per platform, the passing
first-run funnel receipt that proved that Test twin (--funnel-receipt). Signing
a Test release is possible only with the explicit --allow-test.

What the shipped code requires, and therefore what `sign` produces (each native
citation is repeated where release_formats implements it):

  * announcement  WoT-<P>-announcement-v1.json(.sig), announcement key,
    context AnnouncementV1 (Announcement.cpp:66-195). Its key_ring/manifest refs
    carry the exact size + sha256 of the referenced bytes and immutable per-tag
    URLs (the compiled `/latest/` pointer is used ONLY for the announcement pair).
  * key ring      WoT-root-key-ring-v1.json(.sig), a wot-release-key-ring-v1
    signed by an offline root, context ReleaseKeyRingV1; its epoch must equal the
    announcement's key_ring_epoch (WoTGameBootstrapWorker.cpp:283-360).
  * manifest      WoT-<P>-release-manifest-v2.json(.sig), signed by the release
    key the announcement's key_id selects from the ring for this sequence,
    context ReleaseManifestV2 (ReleaseManifest.cpp:191-387). The announcement key
    is never a release key (role audit, generate_bootstrap_trust.py:205-253).
  * archive       WoT-<P>-update-r<seq>-<commit>.zip: the guarded zip's files
    re-containered for staging (release_formats.repack_update_archive), rooted at
    the activation tree leaf and carrying the signed installed-tree-v1.json.
  * install zip   the unchanged guarded zip, for first installs by hand
    (omit with --no-install-zip).
  * Mac DMG       the guarded first-install DMG plus canonical verification
    sidecar. These are copied unchanged and are asserted never to become the
    update manifest's archive.

All JSON is canonical, every signature is produced by the OpenSSL CLI from a key
PATH (Python never reads private-key bytes) and re-verified by the independent
verifier, and the finished directory is re-verified end to end by
verify_release.verify_publish_set before the receipt is written. Given the same
inputs and keys the output bytes are identical (Ed25519 is deterministic, the
JSON is canonical, the archive copies compressed streams and fixes timestamps).

Stdlib only; runs on the Mac host's Python 3.9.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import verify_release  # noqa: E402
from release_formats import FormatError  # noqa: E402

SIGN_RECEIPT_FORMAT = "wot-release-sign-receipt-v1"
RING_RECEIPT_FORMAT = "wot-release-ring-receipt-v1"
DEFAULT_EVIDENCE = formats.REPOSITORY_ROOT / "Saved" / "ReleaseEvidence" / "release"


class SignError(Exception):
    """Signing refused; nothing partial is left behind as a publish set."""


def _write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".partial")
    temporary.write_bytes(data)
    temporary.replace(path)


def _prepare_output(directory: Path) -> None:
    if directory.exists() and (not directory.is_dir() or any(directory.iterdir())):
        raise SignError(f"{directory} must be absent or empty; a publish set is "
                        "written whole, never merged into an older one")
    directory.mkdir(parents=True, exist_ok=True)


def _parse_key_spec(spec: str) -> formats.ReleaseKey:
    """ID=HEX[:FIRST[:LAST]] -- FIRST defaults to 0 and LAST to int64 max."""
    key_id, _, rest = spec.partition("=")
    parts = rest.split(":")
    if not key_id or not parts[0] or len(parts) > 3:
        raise SignError(f"--key expects ID=HEX[:FIRST[:LAST]], got {spec!r}")
    try:
        public_key = formats.parse_lower_hex(parts[0], 32, f"--key {key_id}")
        first = int(parts[1]) if len(parts) > 1 else 0
        last = int(parts[2]) if len(parts) > 2 else formats.INT64_MAX
    except (FormatError, ValueError) as exc:
        raise SignError(str(exc)) from exc
    if not 0 <= first <= last <= formats.INT64_MAX:
        raise SignError(f"--key {key_id}: window [{first}, {last}] is not a valid "
                        "int64 range")
    return formats.ReleaseKey(key_id, public_key, first, last)


def _role_audit(trust: formats.Trust, keys: Sequence[formats.ReleaseKey]) -> None:
    generator = formats._dist_module("generate_bootstrap_trust")
    try:
        generator.audit_role_separation(
            trust.announcement_key.hex(),
            {key.key_id: key.public_key.hex() for key in trust.root_ring.keys},
            {f"release:{key.key_id}": key.public_key.hex() for key in keys})
    except generator.TrustError as exc:
        raise SignError(str(exc)) from exc


def _default_receipt(tag_or_name: str, leaf: str) -> Path:
    return DEFAULT_EVIDENCE / tag_or_name / leaf


# ---------------------------------------------------------------------------
# ring
# ---------------------------------------------------------------------------

def sign_ring(*, trust: formats.Trust, openssl: str, root_key_path: Path,
              root_key_id: str, keys: Sequence[formats.ReleaseKey],
              revoked: Sequence[str], epoch: int, output: Path) -> dict:
    root = next((key for key in trust.root_ring.keys if key.key_id == root_key_id), None)
    if root is None:
        raise SignError(f"{root_key_id!r} is not in the embedded offline-root ring")
    if root_key_id in trust.root_ring.revoked_key_ids:
        raise SignError(f"offline root {root_key_id!r} is revoked")
    if not keys:
        raise SignError("a release key ring needs at least one --key")
    if epoch < 0 or epoch > formats.INT64_MAX:
        raise SignError("the ring epoch must be a non-negative int64")
    _role_audit(trust, keys)
    try:
        document = formats.build_key_ring(epoch, keys, revoked)
        raw = formats.canonical_bytes(document, "key ring")
        signature = crypto.sign_framed(openssl, root_key_path,
                                       crypto.CONTEXT_RELEASE_KEY_RING_V1, raw,
                                       root.public_key)
    except (FormatError, crypto.CryptoError) as exc:
        raise SignError(str(exc)) from exc
    ring_path = Path(output) / formats.KEY_RING_ASSET
    for path, data in ((ring_path, raw),
                       (Path(str(ring_path) + formats.SIGNATURE_SUFFIX), signature)):
        if path.exists() and path.read_bytes() != data:
            raise SignError(f"{path} already exists with different bytes")
        _write_bytes(path, data)
    return {
        "epoch": epoch,
        "format": RING_RECEIPT_FORMAT,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "keys": [{"first_sequence": key.first_sequence, "key_id": key.key_id,
                  "last_sequence": key.last_sequence,
                  "public_key": key.public_key.hex()} for key in keys],
        "revoked_key_ids": list(revoked),
        "ring_sha256": formats.sha256_hex(raw),
        "ring_size": len(raw),
        "signature_sha256": formats.sha256_hex(signature),
        "signer": {"key_id": root_key_id, "public_key": root.public_key.hex()},
    }


# ---------------------------------------------------------------------------
# sign
# ---------------------------------------------------------------------------

def _resolve_release_json(path: Path) -> Path:
    path = Path(path)
    return path / "release.json" if path.is_dir() else path


def _check_configurations(releases: Dict[str, formats.GuardedRelease],
                          allow_test: bool) -> str:
    configurations = {release.configuration for release in releases.values()}
    if len(configurations) != 1:
        raise SignError(f"the platforms were built in different configurations: "
                        f"{sorted(configurations)}")
    configuration = configurations.pop()
    if configuration != formats.RELEASE_CONFIGURATION and not allow_test:
        raise SignError(
            f"the guarded releases are {configuration!r} builds. A public release "
            "must be a guarded Shipping package (release-manifest-v2.schema.json "
            "pins target_attestation.configuration to 'Shipping'; a Test binary "
            "carries the WOT_BOOTSTRAP_TEST_* seams, WoT.Target.cs:41-49). Build "
            "it with build_stripped_pak.py --guarded --configuration Shipping, or "
            "pass --allow-test only to publish Test bytes deliberately")
    return configuration


def _check_funnel_receipts(releases: Dict[str, formats.GuardedRelease],
                           funnel_receipts: Dict[str, Path],
                           allow_unproven_funnel: bool,
                           require_update_cells: bool = False
                           ) -> Dict[str, Optional[dict]]:
    """Report advisory funnel evidence, retaining only validated Test-twin proof."""
    unknown = sorted(set(funnel_receipts) - set(releases))
    for platform in unknown:
        print(f"WARNING: {platform}: --funnel-receipt has no matching release; "
              "no validated funnel proof", file=sys.stderr)
    bound: Dict[str, Optional[dict]] = {}
    for platform, release in sorted(releases.items()):
        path = funnel_receipts.get(platform)
        if path is None:
            print(f"WARNING: {platform}: missing --funnel-receipt {platform}=PATH; "
                  "no validated funnel proof", file=sys.stderr)
            bound[platform] = None
            continue
        required_cells = formats.FUNNEL_REQUIRED_CELLS
        if require_update_cells:
            required_cells += formats.UPDATE_FUNNEL_CELLS_BY_PLATFORM[platform]
        try:
            bound[platform] = formats.validate_funnel_receipt(
                path, platform, release, required_cells)
        except (OSError, FormatError, KeyError, TypeError, ValueError) as exc:
            print(f"WARNING: {platform}: {exc}; no validated funnel proof",
                  file=sys.stderr)
            bound[platform] = None
    return bound


def _check_releases(releases: Dict[str, formats.GuardedRelease],
                    allow_dirty_build: bool) -> str:
    if sorted(releases) != sorted(formats.PLATFORMS):
        raise SignError("sign needs one guarded release per platform (Win64 and Mac)")
    commits = {release.document["git_commit"] for release in releases.values()}
    if len(commits) != 1:
        raise SignError(f"the platforms were built from different commits: {commits}")
    for platform, release in releases.items():
        snapshot = release.document.get("source_snapshot")
        clean = isinstance(snapshot, dict) and snapshot.get("tracked_diff_size") == 0 \
            and snapshot.get("untracked") == []
        if not clean and not allow_dirty_build:
            raise SignError(
                f"{platform} was built from a tree with local changes (source_snapshot "
                "tracked_diff_size/untracked); rebuild from a clean tree or pass "
                "--allow-dirty-build")
    snapshots = {json.dumps(release.document.get("source_snapshot"), sort_keys=True)
                 for release in releases.values()}
    if len(snapshots) != 1 and not allow_dirty_build:
        raise SignError("the platforms' source snapshots differ")
    return commits.pop()


def _component(entries: Sequence[formats.TreeEntry], relative: str, product: dict,
               what: str) -> dict:
    entry = next((item for item in entries if item.path == relative), None)
    if entry is None or entry.type != "file":
        raise SignError(f"{what} {relative!r} is not a file in the release tree")
    if (entry.sha256, entry.size) != (product.get("sha256"), product.get("size")):
        raise SignError(f"{what} {relative!r} differs from its product receipt")
    return {"name": entry.path, "sha256": entry.sha256, "size": entry.size}


def _require_release_identity(archive: Path, platform: str, tree_root: str,
                              sequence: int, commit: str) -> formats.ReleaseIdentity:
    try:
        identity = formats.read_release_identity_from_zip(
            archive, platform, tree_root)
    except FormatError as exc:
        raise SignError(str(exc)) from exc
    if not identity.release_commit:
        raise SignError(f"{platform}: empty release commit in release identity")
    if identity.release_sequence != sequence:
        raise SignError(
            f"{platform}: release identity sequence "
            f"{identity.release_sequence} does not match release sequence {sequence}")
    if identity.release_commit != commit:
        raise SignError(
            f"{platform}: release identity commit {identity.release_commit!r} "
            f"does not match build commit {commit!r}")
    return identity


def sign_release(*, trust: formats.Trust, compiled: formats.CompiledConstants,
                 openssl: str, release_jsons: Dict[str, Path], key_ring: Path,
                 announcement_key_path: Path, release_key_path: Path,
                 release_key_id: str, release_sequence: int, human_version: str,
                 tag: str, output: Path, cache_contract: Path = formats.DEFAULT_CACHE_CONTRACT,
                 release_gate_enabled: bool = False, include_install_zips: bool = True,
                 allow_staging_sequence: bool = False, allow_dirty_build: bool = False,
                 allow_test: bool = False,
                 allow_unnotarized: bool = False,
                 funnel_receipts: Optional[Dict[str, Path]] = None,
                 allow_unproven_funnel: bool = False,
                 inspect_mac_dmg: Optional[bool] = None
                 ) -> dict:
    try:
        formats.validate_tag(tag)
    except FormatError as exc:
        raise SignError(str(exc)) from exc
    if not 0 <= release_sequence <= formats.INT64_MAX:
        raise SignError("--release-sequence must be a non-negative int64")
    if not human_version or not human_version.isprintable():
        raise SignError("--human-version must be a non-empty printable string")

    ring_raw = Path(key_ring).read_bytes()
    ring_signature = Path(str(key_ring) + formats.SIGNATURE_SUFFIX).read_bytes()
    failures: List[str] = []
    with _staging_ring(ring_raw, ring_signature) as ring_directory:
        ring, _, ring_signer = verify_release._verify_ring(ring_directory, trust, None,
                                                           failures)
    if failures or ring is None:
        raise SignError("the key ring does not verify: " + "; ".join(failures))
    try:
        release_key = formats.select_release_key(ring, release_key_id, release_sequence)
    except FormatError as exc:
        raise SignError(str(exc)) from exc
    contract_id, contract_version = formats.load_cache_contract_identity(cache_contract)
    if contract_version > compiled.cache_contract_version:
        raise SignError("the cache contract is newer than the compiled CacheContractVersion")

    releases = {platform: formats.load_guarded_release(_resolve_release_json(path),
                                                       platform)
                for platform, path in release_jsons.items()}
    commit = _check_releases(releases, allow_dirty_build)
    configuration = _check_configurations(releases, allow_test)
    mac_release = releases.get("Mac")
    if (configuration == formats.SHIPPING_CONFIGURATION
            and mac_release is not None
            and (mac_release.dmg or {}).get("notarized") is not True
            and not allow_unnotarized):
        raise SignError(
            "Mac Shipping DMG is not notarized; pass --allow-unnotarized only "
            "for an intentional exception")
    for platform, release in releases.items():
        if release.release_sequence != release_sequence:
            raise SignError(
                f"{platform}: release.json release_sequence "
                f"{release.release_sequence} does not match {release_sequence}")
        _require_release_identity(
            release.zip_path, platform, formats.GUARDED_TREE_ROOTS[platform],
            release_sequence, commit)
    funnels = _check_funnel_receipts(
        releases, dict(funnel_receipts or {}), allow_unproven_funnel,
        require_update_cells=(release_sequence > 0))
    output = Path(output)
    _prepare_output(output)
    try:
        return _sign_into(output, trust=trust, compiled=compiled, openssl=openssl,
                          releases=releases, ring=ring, ring_raw=ring_raw,
                          ring_signature=ring_signature, ring_signer=ring_signer,
                          release_key=release_key,
                          announcement_key_path=Path(announcement_key_path),
                          release_key_path=Path(release_key_path),
                          sequence=release_sequence, human_version=human_version,
                          tag=tag, commit=commit, contract_id=contract_id,
                          contract_version=contract_version,
                          release_gate_enabled=release_gate_enabled,
                          include_install_zips=include_install_zips,
                          allow_staging_sequence=allow_staging_sequence,
                          cache_contract=Path(cache_contract),
                          allow_test=allow_test, configuration=configuration,
                          allow_unnotarized=allow_unnotarized,
                          funnels=funnels, funnel_receipts=dict(funnel_receipts or {}),
                          inspect_mac_dmg=inspect_mac_dmg)
    except BaseException:
        # A refused publish set must not survive as something publishable.
        shutil.rmtree(output, ignore_errors=True)
        raise


class _staging_ring:
    """A throwaway directory holding just the ring pair, for the shared verifier."""

    def __init__(self, raw: bytes, signature: bytes):
        self.raw = raw
        self.signature = signature

    def __enter__(self) -> Path:
        import tempfile

        self._directory = tempfile.TemporaryDirectory(prefix="wot-ring-")
        path = Path(self._directory.name)
        (path / formats.KEY_RING_ASSET).write_bytes(self.raw)
        (path / (formats.KEY_RING_ASSET + formats.SIGNATURE_SUFFIX)).write_bytes(
            self.signature)
        return path

    def __exit__(self, *exc) -> None:
        self._directory.cleanup()


def _sign_into(output: Path, *, trust, compiled, openssl, releases, ring, ring_raw,
               ring_signature, ring_signer, release_key, announcement_key_path,
               release_key_path, sequence, human_version, tag, commit, contract_id,
               contract_version, release_gate_enabled, include_install_zips,
               allow_staging_sequence, cache_contract, allow_test=False,
               allow_unnotarized=False,
               configuration=formats.RELEASE_CONFIGURATION, funnels=None,
               funnel_receipts=None, inspect_mac_dmg=None) -> dict:
    _write_bytes(output / formats.KEY_RING_ASSET, ring_raw)
    _write_bytes(output / (formats.KEY_RING_ASSET + formats.SIGNATURE_SUFFIX),
                 ring_signature)
    ring_ref = formats.ObjectRef(formats.release_download_url(tag, formats.KEY_RING_ASSET),
                                 len(ring_raw), formats.sha256_hex(ring_raw))
    platforms: Dict[str, dict] = {}
    for platform in formats.PLATFORMS:
        release = releases[platform]
        archive_name = formats.update_archive_asset(platform, sequence, commit)
        if archive_name.endswith(".dmg") or (release.dmg is not None
                                             and archive_name == release.dmg["name"]):
            raise SignError("the Mac DMG must never be an update archive")
        repack = formats.repack_update_archive(release.zip_path, output / archive_name,
                                               platform)
        _require_release_identity(
            output / archive_name, platform, formats.UPDATE_TREE_ROOTS[platform],
            sequence, commit)
        main = _component(repack.entries, release.main_relative,
                          release.products["WoT"], "main executable")
        archive_digest, archive_size = formats.sha256_file(output / archive_name)
        manifest_document = {
            "announcement_domain": compiled.announcement_domain,
            "archive": {"name": archive_name, "sha256": archive_digest,
                        "size": archive_size,
                        "url": formats.release_download_url(tag, archive_name)},
            "archive_root_id": formats.ARCHIVE_ROOT_ID,
            "build_commit": commit,
            "cache_contract_id": contract_id,
            "cache_contract_version": contract_version,
            "format": formats.RELEASE_MANIFEST_FORMAT,
            "generated_utc": str(release.document["generated_utc"]),
            "human_version": human_version,
            "installed_tree_root": {"name": repack.root, "sha256": repack.tree_root,
                                    "size": len(repack.inventory_raw)},
            "installed_tree_version": compiled.installed_tree_version,
            "key_id": release_key.key_id,
            "main_executable": main,
            "manifest_version": compiled.release_manifest_version,
            "minimum_commit_service_protocol": compiled.commit_service_protocol,
            "minimum_gate_protocol": compiled.gate_protocol,
            "minimum_updater_protocol": compiled.updater_protocol,
            "platform": platform,
            "release_sequence": sequence,
            "stripped_audit_verdict": str(release.document["audit"]["verdict"]),
            # Claims, parse-only natively (ReleaseManifest.cpp:122-177). The
            # configuration is the TRUE one from release.json: "Shipping" for
            # a release (the schema const), "Test" only under --allow-test.
            "target_attestation": {
                "configuration": release.configuration,
                "dedicated_server_startup_available": False,
                "launch_module": "WoTLaunch",
                "listen_server_present": True,
                "release_gate_enabled": bool(release_gate_enabled),
                "target_type": "Game",
            },
        }
        manifest_raw = formats.canonical_bytes(manifest_document, "release manifest")
        manifest_name = formats.manifest_asset(platform)
        announcement_document = formats.build_announcement(
            platform=platform, domain=compiled.announcement_domain,
            key_id=release_key.key_id, release_sequence=sequence,
            key_ring_epoch=ring.epoch, minimum_gate_protocol=compiled.gate_protocol,
            key_ring=ring_ref,
            manifest=formats.ObjectRef(formats.release_download_url(tag, manifest_name),
                                       len(manifest_raw),
                                       formats.sha256_hex(manifest_raw)))
        announcement_raw = formats.canonical_bytes(announcement_document, "announcement")
        formats.parse_manifest(manifest_raw, formats.parse_announcement(
            announcement_raw, platform, compiled.announcement_domain))
        manifest_signature = crypto.sign_framed(
            openssl, release_key_path, crypto.CONTEXT_RELEASE_MANIFEST_V2, manifest_raw,
            release_key.public_key)
        announcement_signature = crypto.sign_framed(
            openssl, announcement_key_path, crypto.CONTEXT_ANNOUNCEMENT_V1,
            announcement_raw, trust.announcement_key)
        announcement_name = formats.announcement_asset(platform)
        for name, data in ((manifest_name, manifest_raw),
                           (manifest_name + formats.SIGNATURE_SUFFIX, manifest_signature),
                           (announcement_name, announcement_raw),
                           (announcement_name + formats.SIGNATURE_SUFFIX,
                            announcement_signature)):
            _write_bytes(output / name, data)
        install_zip = None
        if include_install_zips:
            install_zip = release.zip_path.name
            shutil.copyfile(release.zip_path, output / install_zip)
        dmg_receipt = None
        if platform == "Mac":
            if release.dmg_path is None or release.dmg_sidecar_path is None \
                    or release.dmg is None:
                raise SignError("Mac release has no guarded first-install DMG binding")
            shutil.copyfile(release.dmg_path, output / release.dmg_path.name)
            shutil.copyfile(release.dmg_sidecar_path,
                            output / release.dmg_sidecar_path.name)
            dmg_receipt = release.dmg
        platforms[platform] = {
            "archive": {"name": archive_name, "sha256": archive_digest,
                        "size": archive_size},
            "dropped_source_metadata": repack.dropped_metadata,
            "guarded_zip": {"name": release.zip_path.name,
                            "sha256": release.document["sha256"],
                            "size": release.document["size"]},
            "funnel_receipt": (funnels or {}).get(platform),
            "install_zip": install_zip,
            "dmg": dmg_receipt,
            "installed_tree_root": repack.tree_root,
            "installed_tree_entries": len(repack.entries),
            "mode_policy": repack.source_mode_policy,
            "release_json_sha256": release.release_json_sha256,
            "test_twin_release_json_sha256": (
                release.test_twin["release_json_sha256"]
                if release.test_twin is not None else None),
        }

    verified = verify_release._verify_signing_output(
        output, trust=trust, compiled=compiled,
        releases={platform: release.release_json for platform, release in releases.items()},
        funnel_receipts=dict(funnel_receipts or {}),
        allow_staging_sequence=allow_staging_sequence, allow_test=allow_test,
        allow_unnotarized=allow_unnotarized,
        inspect_mac_dmg=inspect_mac_dmg)
    return {
        "announcement_key": {"key_id": trust.announcement_key_id,
                             "public_key": trust.announcement_key.hex()},
        "allow_unnotarized": bool(allow_unnotarized),
        "cache_contract_sha256": formats.sha256_file(cache_contract)[0],
        "compiled_constants": dataclasses.asdict(compiled),
        "configuration": configuration,
        "format": SIGN_RECEIPT_FORMAT,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "human_version": human_version,
        "platforms": platforms,
        "release_key": {"key_id": release_key.key_id,
                        "public_key": release_key.public_key.hex()},
        "ring": {"epoch": ring.epoch, "sha256": formats.sha256_hex(ring_raw),
                 "signer": ring_signer},
        "sequence_gate": verified.sequence_gate,
        "set": verified.summary(),
        "trust_sha256": formats.sha256_hex(trust.trust_raw),
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = result.add_subparsers(dest="command", required=True)

    ring = commands.add_parser("ring", help="sign a release key ring with an offline root")
    ring.add_argument("--root-key", required=True,
                      help="offline-root private key PEM (path only; never read by Python)")
    ring.add_argument("--root-key-id", required=True,
                      help="its id in public-root-ring.json, e.g. offline-root-a")
    ring.add_argument("--key", action="append", default=[], required=True,
                      help="release key ID=HEX[:FIRST[:LAST]] (repeatable)")
    ring.add_argument("--revoke", action="append", default=[],
                      help="revoked release key id (repeatable)")
    ring.add_argument("--epoch", type=int, required=True)
    ring.add_argument("--output", required=True, help="directory for the ring pair")
    ring.add_argument("--receipt")
    ring.add_argument("--openssl")
    verify_release.add_trust_arguments(ring)

    sign = commands.add_parser("sign", help="build and sign the full publish set")
    sign.add_argument("--release", action="append", default=[], required=True,
                      help="PLATFORM=<guarded release dir or release.json> (both platforms)")
    sign.add_argument("--key-ring", required=True,
                      help="WoT-root-key-ring-v1.json (its .sig must sit beside it)")
    sign.add_argument("--announcement-key", required=True,
                      help="announcement private key PEM (path only)")
    sign.add_argument("--release-key", required=True,
                      help="release private key PEM (path only)")
    sign.add_argument("--release-key-id", required=True)
    sign.add_argument("--release-sequence", type=int, required=True)
    sign.add_argument("--human-version", required=True)
    sign.add_argument("--tag", required=True, help="GitHub release tag the URLs will name")
    sign.add_argument("--output", required=True, help="empty directory for the publish set")
    sign.add_argument("--cache-contract", default=str(formats.DEFAULT_CACHE_CONTRACT))
    sign.add_argument("--release-gate-enabled", action="store_true",
                      help="set target_attestation.release_gate_enabled (parse-only)")
    sign.add_argument("--no-install-zip", action="store_true",
                      help="omit the guarded manual-install zips from the set")
    sign.add_argument("--allow-staging-sequence", action="store_true",
                      help="compatibility no-op; funnel receipts are advisory")
    sign.add_argument("--allow-dirty-build", action="store_true",
                      help="accept releases whose source_snapshot shows local changes")
    sign.add_argument("--allow-test", action="store_true",
                      help="sign guarded TEST releases (never release bytes; B4). "
                      "Without it only guarded Shipping releases are signed")
    sign.add_argument("--allow-unnotarized", action="store_true",
                      help="sign an unnotarized Mac Shipping set intentionally; "
                      "recorded in the sign receipt")
    sign.add_argument("--funnel-receipt", action="append", default=[],
                      help="PLATFORM=<funnel-receipt.json> of the passing first-run "
                      "funnel run on the Shipping release's Test twin (both platforms)")
    sign.add_argument("--allow-unproven-funnel", action="store_true",
                      help="compatibility no-op; missing funnel receipts warn")
    sign.add_argument("--receipt")
    sign.add_argument("--openssl")
    verify_release.add_trust_arguments(sign)
    return result


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parser().parse_args(argv)
    try:
        trust, compiled = verify_release.load_trust_from_args(args)
        openssl = crypto.find_openssl(args.openssl)
        if args.command == "ring":
            receipt = sign_ring(
                trust=trust, openssl=openssl, root_key_path=Path(args.root_key),
                root_key_id=args.root_key_id,
                keys=[_parse_key_spec(spec) for spec in args.key],
                revoked=args.revoke, epoch=args.epoch, output=Path(args.output))
            receipt_path = Path(args.receipt) if args.receipt else _default_receipt(
                f"ring-epoch-{args.epoch}", "ring-receipt.json")
        else:
            receipt = sign_release(
                trust=trust, compiled=compiled, openssl=openssl,
                release_jsons=verify_release.parse_platform_paths(args.release, "release"),
                key_ring=Path(args.key_ring),
                announcement_key_path=Path(args.announcement_key),
                release_key_path=Path(args.release_key),
                release_key_id=args.release_key_id,
                release_sequence=args.release_sequence,
                human_version=args.human_version, tag=args.tag,
                output=Path(args.output), cache_contract=Path(args.cache_contract),
                release_gate_enabled=args.release_gate_enabled,
                include_install_zips=not args.no_install_zip,
                allow_staging_sequence=args.allow_staging_sequence,
                allow_dirty_build=args.allow_dirty_build,
                allow_test=args.allow_test,
                allow_unnotarized=args.allow_unnotarized,
                funnel_receipts=verify_release.parse_platform_paths(
                    args.funnel_receipt, "funnel-receipt"),
                allow_unproven_funnel=args.allow_unproven_funnel)
            receipt_path = Path(args.receipt) if args.receipt else _default_receipt(
                args.tag, "sign-receipt.json")
    except verify_release.VerifyError as exc:
        for failure in exc.failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    except (SignError, FormatError, crypto.CryptoError, OSError) as exc:
        print(f"sign_release: {exc}", file=sys.stderr)
        return 1
    verify_release.write_receipt(receipt_path, receipt)
    print(f"sign_release: {args.command} OK; receipt {receipt_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
