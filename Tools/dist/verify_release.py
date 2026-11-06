#!/usr/bin/env python3
"""Independently verify a release publish set against the checked-in public trust.

A publish set is the flat directory of GitHub release assets that sign_release.py
writes and publish_release.py uploads:

  WoT-root-key-ring-v1.json(.sig)                 release key ring, offline-root signed
  WoT-<P>-release-manifest-v2.json(.sig)          per platform, release-key signed
  WoT-<P>-update-r<seq>-<commit>.zip              per platform, the updater archive
  WoT-<P>-announcement-v1.json(.sig)              per platform, announcement-key signed
  WoT-<P>-guarded-shipping-<commit>.zip           optional manual-install download
                                                  (WoT-<P>-guarded-<commit>.zip for
                                                  an --allow-test Test publication)
  WoT-Mac-guarded-shipping-<commit>.dmg           required Mac first-install image
  WoT-Mac-guarded-shipping-<commit>.dmg.verify.json
                                                  canonical cross-host verification data

The signed update chain is checked the way the shipped game/updater checks it
(release_formats cites each native reader), using public trust material: the
announcement key and offline-root ring that bootstrap-trust-v1.json binds and
BootstrapTrust.generated.h embeds.  The manual-install Mac DMG and sidecar also
require the packaging lane's unsigned Mac release.json; neither that manifest
nor the sidecar is an authenticated trust source.  Signatures are verified by
the pure-Python Ed25519
in release_crypto, which shares no code with the OpenSSL signer;
`--openssl-cross-check` adds a second opinion from the same library family the
game links.

Beyond native acceptance it enforces publish policy the game cannot see:
platform coverage, one tag and one release across platforms, role separation,
no test markers, and monotonic sequence versus the currently published announcements.
Funnel receipts are advisory: unproven evidence warns. A publish set must attest the Shipping
configuration (release-manifest-v2.schema.json const) and its signed game and
updater images must carry none of the compiled test seams
(release_formats.TEST_SEAM_MARKERS); a Test set is FLAGGED and refused unless
`--allow-test` names it a deliberate Test publication.

Uses the stdlib and Pillow through mac_dmg; runs on the Mac host's Python 3.9.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import zipfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import mac_dmg  # noqa: E402
from release_formats import FormatError  # noqa: E402

VERIFY_RECEIPT_FORMAT = "wot-release-verify-receipt-v1"
TEST_MARKERS = ("test-keys", "test-key", "localhost", "127.0.0.1", "example.invalid",
                "WOT_BOOTSTRAP_TEST")
_INSTALL_ZIP = re.compile(
    r"^WoT-(Win64|Mac)-guarded-(?:(shipping)-)?([0-9a-f]{7,40})\.zip$")

class VerifyError(Exception):
    def __init__(self, failures: Sequence[str]):
        super().__init__("\n".join(failures))
        self.failures = list(failures)


@dataclass
class PlatformResult:
    announcement: formats.Announcement
    manifest: formats.Manifest
    archive_name: str
    tree_root: str
    entries: List[formats.TreeEntry] = field(default_factory=list)
    install_zip: Optional[str] = None
    dmg: Optional[str] = None
    dmg_inspection: Optional[str] = None
    dmg_notarized: Optional[bool] = None
    configuration: str = ""


@dataclass
class VerifiedSet:
    directory: Path
    tag: str
    release_sequence: int
    key_ring_epoch: int
    key_id: str
    build_commit: str
    ring: formats.KeyRing
    ring_signer: str
    platforms: Dict[str, PlatformResult]
    sequence_gate: dict
    assets: Dict[str, Dict[str, object]] = field(default_factory=dict)

    def summary(self) -> dict:
        return {
            "assets": self.assets,
            "build_commit": self.build_commit,
            "key_id": self.key_id,
            "key_ring_epoch": self.key_ring_epoch,
            "platforms": {
                platform: {
                    "announcement_sha256": formats.sha256_hex(
                        (self.directory / formats.announcement_asset(platform))
                        .read_bytes()),
                    "archive": result.archive_name,
                    "configuration": result.configuration,
                    "install_zip": result.install_zip,
                    "dmg": result.dmg,
                    "dmg_inspection": result.dmg_inspection,
                    "dmg_notarized": result.dmg_notarized,
                    "installed_tree_root": result.tree_root,
                    "main_executable": result.manifest.main_executable.name,
                    "manifest_sha256": result.announcement.manifest.sha256,
                } for platform, result in sorted(self.platforms.items())},
            "release_sequence": self.release_sequence,
            "ring_signer": self.ring_signer,
            "sequence_gate": self.sequence_gate,
            "tag": self.tag,
        }


def _read(directory: Path, name: str, failures: List[str]) -> Optional[bytes]:
    path = directory / name
    if not path.is_file():
        failures.append(f"missing asset {name}")
        return None
    return path.read_bytes()


def _read_signature(directory: Path, name: str, failures: List[str]) -> Optional[bytes]:
    signature = _read(directory, name + formats.SIGNATURE_SUFFIX, failures)
    if signature is not None and len(signature) != crypto.SIGNATURE_BYTES:
        failures.append(f"{name}{formats.SIGNATURE_SUFFIX} is {len(signature)} bytes; "
                        "the game requires exactly 64 (WoTGameBootstrapWorker.cpp:183-197)")
        return None
    return signature


def _check_markers(name: str, raw: bytes, failures: List[str]) -> None:
    text = raw.decode("ascii", "replace")
    for marker in TEST_MARKERS:
        if marker in text:
            failures.append(f"{name} carries the test marker {marker!r}")


def _tag_from_url(url: str, asset: str) -> Optional[str]:
    prefix = f"https://github.com/{formats.REPOSITORY}/releases/download/"
    if not url.startswith(prefix) or not url.endswith("/" + asset):
        return None
    tag = url[len(prefix):-len(asset) - 1]
    try:
        formats.validate_tag(tag)
    except FormatError:
        return None
    return tag


def _require_url(url: str, asset: str, what: str, failures: List[str]) -> Optional[str]:
    if not formats.is_allowed_initial_release_url(url):
        failures.append(f"{what} url {url!r} fails WoTIsAllowedInitialReleaseUrl "
                        "(WoTUpdateTransportPolicy.cpp:241-248)")
        return None
    tag = _tag_from_url(url, asset)
    if tag is None:
        failures.append(
            f"{what} url {url!r} is not the immutable per-tag download URL for {asset}; "
            "/latest/ redirects through github.com, which the native redirect judge "
            "refuses (WoTUpdateTransportPolicy.cpp:210-217)")
    return tag


def _openssl_second_opinion(openssl: Optional[str], key: bytes, context: int,
                            payload: bytes, signature: bytes, what: str,
                            failures: List[str]) -> None:
    if openssl and not crypto.openssl_verify_framed(openssl, key, context, payload,
                                                    signature):
        failures.append(f"{what}: OpenSSL disagrees with the independent verifier")


def _verify_ring(directory: Path, trust: formats.Trust, openssl: Optional[str],
                 failures: List[str]):
    raw = _read(directory, formats.KEY_RING_ASSET, failures)
    signature = _read_signature(directory, formats.KEY_RING_ASSET, failures)
    if raw is None or signature is None:
        return None, None, None
    _check_markers(formats.KEY_RING_ASSET, raw, failures)
    try:
        ring = formats.parse_key_ring(formats.parse_canonical(raw, "key ring"),
                                      formats.RELEASE_KEY_RING_FORMAT)
    except FormatError as exc:
        failures.append(f"{formats.KEY_RING_ASSET}: {exc}")
        return None, None, None
    # The game accepts a ring signed by ANY embedded root key and ignores the root
    # ring's windows and revocations (WoTGameBootstrapWorker.cpp:331-340). A
    # publish gate is stricter: a revoked root must not be the one vouching.
    signers = [key.key_id for key in trust.root_ring.keys
               if crypto.verify_framed(key.public_key, crypto.CONTEXT_RELEASE_KEY_RING_V1,
                                       raw, signature)]
    if not signers:
        failures.append(f"{formats.KEY_RING_ASSET} is not signed by any embedded "
                        "offline root under ReleaseKeyRingV1")
        return ring, raw, None
    live = [key_id for key_id in signers if key_id not in trust.root_ring.revoked_key_ids]
    if not live:
        failures.append(f"{formats.KEY_RING_ASSET} is signed only by revoked root(s) "
                        f"{signers}")
    root_key = next(key.public_key for key in trust.root_ring.keys
                    if key.key_id == signers[0])
    _openssl_second_opinion(openssl, root_key, crypto.CONTEXT_RELEASE_KEY_RING_V1, raw,
                            signature, formats.KEY_RING_ASSET, failures)
    # Role separation, the same audit generate_bootstrap_trust.py applies.
    generator = formats._dist_module("generate_bootstrap_trust")
    try:
        generator.audit_role_separation(
            trust.announcement_key.hex(),
            {key.key_id: key.public_key.hex() for key in trust.root_ring.keys},
            {f"release:{key.key_id}": key.public_key.hex() for key in ring.keys})
    except generator.TrustError as exc:
        failures.append(f"{formats.KEY_RING_ASSET}: {exc}")
    return ring, raw, signers[0]


def _archive_member_markers(archive_path: Path, root: str, relative: str) -> List[str]:
    """Scan one signed archive member for compiled test seams."""
    with zipfile.ZipFile(archive_path) as archive:
        with archive.open(f"{root}/{relative}") as handle:
            data = handle.read()
    return formats.find_test_seam_markers(data)


def _check_configuration(manifest_name: str, platform: str, manifest: formats.Manifest,
                         archive_path: Path, root: str, allow_test: bool,
                         failures: List[str]) -> str:
    """FLAG a Test (or unknown) configuration; prove a Shipping set has no seams."""
    configuration = str(manifest.document["target_attestation"]["configuration"])
    if configuration not in formats.GUARDED_CONFIGURATIONS:
        failures.append(f"{manifest_name}: target_attestation.configuration "
                        f"{configuration!r} is not a guarded configuration")
        return configuration
    if configuration != formats.RELEASE_CONFIGURATION and not allow_test:
        failures.append(
            f"{manifest_name}: FLAGGED {configuration} build -- "
            "target_attestation.configuration must be 'Shipping' "
            "(release-manifest-v2.schema.json const); a Test image compiles the "
            "WOT_BOOTSTRAP_TEST_* seams (WoT.Target.cs:41-49). Pass --allow-test "
            "only for a deliberate Test publication (RELEASE-RUNBOOK B4)")
    if not formats.main_executable_matches_configuration(
            manifest.main_executable.name, platform, configuration):
        failures.append(f"{manifest_name}: main_executable "
                        f"{manifest.main_executable.name!r} is not the "
                        f"{configuration} game image its attestation claims")
    if configuration == formats.SHIPPING_CONFIGURATION:
        for label, component in (("main_executable", manifest.main_executable),):
            try:
                markers = _archive_member_markers(archive_path, root, component.name)
            except (OSError, KeyError, zipfile.BadZipFile) as exc:
                failures.append(f"{manifest_name}: cannot scan {label} for test "
                                f"seams: {exc}")
                continue
            if markers:
                failures.append(f"{manifest_name}: {label} {component.name!r} attests "
                                f"Shipping but carries compiled test seams {markers}")
    return configuration


def _check_release_identity(archive_path: Path, platform: str, tree_root: str,
                            release_sequence: int, release_commit: str,
                            failures: List[str]) -> None:
    try:
        identity = formats.read_release_identity_from_zip(
            archive_path, platform, tree_root)
    except FormatError as exc:
        failures.append(str(exc))
        return
    if not identity.release_commit:
        failures.append(
            f"{archive_path.name}: empty release commit in release identity")
    if identity.release_sequence != release_sequence:
        failures.append(
            f"{archive_path.name}: release identity sequence "
            f"{identity.release_sequence} does not match release sequence "
            f"{release_sequence}")
    if identity.release_commit != release_commit:
        failures.append(
            f"{archive_path.name}: release identity commit "
            f"{identity.release_commit!r} does not match build commit "
            f"{release_commit!r}")


def _verify_platform(directory: Path, platform: str, trust: formats.Trust,
                     compiled: formats.CompiledConstants, ring: formats.KeyRing,
                     ring_raw: bytes, openssl: Optional[str],
                     failures: List[str], allow_test: bool = False
                     ) -> Optional[PlatformResult]:
    local: List[str] = []
    name = formats.announcement_asset(platform)
    raw = _read(directory, name, local)
    signature = _read_signature(directory, name, local)
    if raw is None or signature is None:
        failures.extend(local)
        return None
    _check_markers(name, raw, local)
    if not crypto.verify_framed(trust.announcement_key, crypto.CONTEXT_ANNOUNCEMENT_V1,
                                raw, signature):
        failures.append(f"{name}: the signature does not verify under the embedded "
                        "announcement key (Announcement.cpp:75-85)")
        return None
    _openssl_second_opinion(openssl, trust.announcement_key,
                            crypto.CONTEXT_ANNOUNCEMENT_V1, raw, signature, name, local)
    compiled_url = trust.announcement_urls[platform]
    if not compiled_url.endswith("/" + name):
        local.append(f"the compiled {platform} announcement URL {compiled_url!r} does "
                     f"not name {name}")
    try:
        announcement = formats.parse_announcement(raw, platform,
                                                  compiled.announcement_domain)
    except FormatError as exc:
        failures.append(f"{name}: {exc}")
        return None
    if announcement.minimum_gate_protocol > compiled.gate_protocol:
        local.append(f"{name}: minimum_gate_protocol {announcement.minimum_gate_protocol} "
                     f"exceeds the compiled GateProtocol {compiled.gate_protocol}; every "
                     "current client would StayBlocked (Ordering.cpp:91-94)")
    if announcement.key_ring_epoch != ring.epoch:
        local.append(f"{name}: key_ring_epoch {announcement.key_ring_epoch} != ring epoch "
                     f"{ring.epoch} (WoTGameBootstrapWorker.cpp:299-304)")
    if announcement.key_ring.size != len(ring_raw) \
            or announcement.key_ring.sha256 != formats.sha256_hex(ring_raw):
        local.append(f"{name}: key_ring sha256/size do not match {formats.KEY_RING_ASSET}")
    _require_url(announcement.key_ring.url, formats.KEY_RING_ASSET,
                 f"{name} key_ring", local)

    manifest_name = formats.manifest_asset(platform)
    _require_url(announcement.manifest.url, manifest_name, f"{name} manifest", local)
    manifest_raw = _read(directory, manifest_name, local)
    manifest_signature = _read_signature(directory, manifest_name, local)
    if manifest_raw is None or manifest_signature is None:
        failures.extend(local)
        return None
    _check_markers(manifest_name, manifest_raw, local)
    if announcement.manifest.size != len(manifest_raw) \
            or announcement.manifest.sha256 != formats.sha256_hex(manifest_raw):
        local.append(f"{name}: manifest sha256/size do not match {manifest_name}")
    try:
        key = formats.select_release_key(ring, announcement.key_id,
                                         announcement.release_sequence)
    except FormatError as exc:
        failures.extend(local + [f"{manifest_name}: {exc}"])
        return None
    if not crypto.verify_framed(key.public_key, crypto.CONTEXT_RELEASE_MANIFEST_V2,
                                manifest_raw, manifest_signature):
        failures.extend(local + [f"{manifest_name}: the signature does not verify under "
                                 f"release key {key.key_id!r} (ReleaseManifest.cpp:207-218)"])
        return None
    _openssl_second_opinion(openssl, key.public_key, crypto.CONTEXT_RELEASE_MANIFEST_V2,
                            manifest_raw, manifest_signature, manifest_name, local)
    try:
        manifest = formats.parse_manifest(manifest_raw, announcement)
    except FormatError as exc:
        failures.extend(local + [f"{manifest_name}: {exc}"])
        return None
    if not formats.supports_manifest_protocols(manifest, compiled):
        local.append(f"{manifest_name}: requires protocols newer than this build "
                     "(ReleaseManifest.cpp:180-189)")
    archive_name = manifest.archive_name
    try:
        formats.validate_asset_name(archive_name)
    except FormatError as exc:
        local.append(f"{manifest_name}: {exc}")
    if not archive_name.endswith(".zip") or archive_name.endswith(".dmg"):
        local.append(
            f"{manifest_name}: update archive must be a zip and must exclude the DMG")
    _require_url(manifest.archive.url, archive_name, f"{manifest_name} archive", local)
    archive_path = directory / archive_name
    if not archive_path.is_file():
        failures.extend(local + [f"missing asset {archive_name}"])
        return None
    digest, size = formats.sha256_file(archive_path)
    if digest != manifest.archive.sha256 or size != manifest.archive.size:
        failures.extend(local + [f"{archive_name}: sha256/size differ from {manifest_name}"])
        return None
    _check_release_identity(
        archive_path, platform, formats.UPDATE_TREE_ROOTS[platform],
        announcement.release_sequence, manifest.document["build_commit"], local)
    try:
        staged = formats.stage_archive(archive_path, platform)
    except FormatError as exc:
        failures.extend(local + [f"{archive_name}: {exc}"])
        return None
    expected_root = formats.UPDATE_TREE_ROOTS[platform]
    if staged.root != manifest.installed_tree_root.name or staged.root != expected_root:
        local.append(f"{archive_name}: root {staged.root!r} must equal "
                     f"installed_tree_root.name and the activation leaf {expected_root!r} "
                     "(ReleaseStaging.cpp:1389-1393; ActivationTransaction.cpp:29-34)")
    if len(staged.inventory_raw) != manifest.installed_tree_root.size:
        local.append(f"{archive_name}: inventory size differs from installed_tree_root.size "
                     "(ReleaseStaging.cpp:1536-1541)")
    if staged.tree_root != manifest.installed_tree_root.sha256:
        local.append(f"{archive_name}: rehashed tree root differs from the manifest "
                     "(ReleaseStaging.cpp:1642-1648)")
    by_path = {entry.path: entry for entry in staged.entries}
    for label, component in (("main_executable", manifest.main_executable),):
        entry = by_path.get(component.name)
        if entry is None or entry.type != "file" or entry.size != component.size \
                or entry.sha256 != component.sha256:
            local.append(f"{manifest_name}: {label} {component.name!r} does not match a "
                         "file in the closed tree (ReleaseStaging.cpp:1650-1666)")
    configuration = _check_configuration(manifest_name, platform, manifest,
                                         archive_path, staged.root, allow_test, local)
    failures.extend(local)
    return PlatformResult(announcement=announcement, manifest=manifest,
                          archive_name=archive_name, tree_root=staged.tree_root,
                          entries=staged.entries, configuration=configuration)


def _check_install_zip(directory: Path, name: str, platform: str,
                       result: PlatformResult, failures: List[str]) -> None:
    """The manual-install zip must carry exactly the signed tree's files."""
    _check_release_identity(
        directory / name, platform, formats.GUARDED_TREE_ROOTS[platform],
        result.announcement.release_sequence,
        result.manifest.document["build_commit"], failures)
    signed = {entry.path: entry for entry in result.entries if entry.type == "file"}
    source_root = formats.GUARDED_TREE_ROOTS[platform]
    install_root = formats.GUARDED_ZIP_ROOTS[platform]
    seen = set()
    try:
        with zipfile.ZipFile(directory / name) as archive:
            for info in archive.infolist():
                path = info.filename.rstrip("/")
                try:
                    formats.reject_appledouble_path(path)
                except FormatError as exc:
                    failures.append(f"{name}: {exc}")
                    continue
                if info.is_dir() or (platform == "Mac" and formats._is_mac_metadata(path)):
                    continue
                if (platform == "Win64"
                        and path == install_root + "/WoT Reborn.exe"):
                    continue
                if not path.startswith(source_root + "/"):
                    failures.append(f"{name}: {path!r} is outside {source_root}/")
                    continue
                relative = path[len(source_root) + 1:]
                entry = signed.get(relative)
                if entry is None:
                    failures.append(f"{name}: {relative!r} is not in the signed tree")
                    continue
                with archive.open(info) as handle:
                    digest = hashlib.sha256()
                    for chunk in iter(lambda: handle.read(1 << 20), b""):
                        digest.update(chunk)
                if digest.hexdigest() != entry.sha256 or info.file_size != entry.size:
                    failures.append(f"{name}: {relative!r} differs from the signed tree")
                seen.add(relative)
    except (OSError, zipfile.BadZipFile) as exc:
        failures.append(f"{name}: {exc}")
        return
    missing = sorted(set(signed) - seen)
    if missing:
        failures.append(f"{name}: lacks signed files {missing[:5]}"
                        + (" ..." if len(missing) > 5 else ""))


def _check_release_json(release_json: Path, platform: str, result: PlatformResult,
                        directory: Path, failures: List[str]
                        ) -> Optional[formats.GuardedRelease]:
    try:
        release = formats.load_guarded_release(release_json, platform)
    except (OSError, FormatError) as exc:
        failures.append(f"{release_json}: {exc}")
        return None
    manifest = result.manifest
    if release.document.get("git_commit") != manifest.document["build_commit"]:
        failures.append(f"{release_json}: git_commit differs from the manifest build_commit")
    if release.release_sequence != result.announcement.release_sequence:
        failures.append(
            f"{release_json}: release_sequence differs from the signed manifest")
    if release.configuration != result.configuration:
        failures.append(f"{release_json}: configuration {release.configuration!r} differs "
                        f"from the manifest attestation {result.configuration!r}")
    for target, component in (("WoT", manifest.main_executable),):
        product = release.products[target]
        relative = release.main_relative
        if (relative, product.get("sha256"), product.get("size")) != (
                component.name, component.sha256, component.size):
            failures.append(f"{release_json}: {target} product differs from the manifest")
    try:
        projected = formats.project_guarded_archive_tree(release.zip_path, platform)
    except (OSError, zipfile.BadZipFile, FormatError) as exc:
        failures.append(f"{release_json}: cannot project guarded release tree: {exc}")
    else:
        if (projected.root != manifest.installed_tree_root.name
                or projected.entries != result.entries
                or projected.tree_root != result.tree_root
                or len(projected.inventory_raw) != manifest.installed_tree_root.size):
            failures.append(
                f"{release_json}: guarded release tree differs from the signed update tree")
    install = directory / release.zip_path.name
    if install.is_file():
        digest, size = formats.sha256_file(install)
        if (digest, size) != (release.document["sha256"], release.document["size"]):
            failures.append(f"{install.name}: differs from {release_json}")
    if platform == "Mac":
        if release.dmg is None:
            failures.append(f"{release_json}: has no bound Mac DMG")
        elif release.dmg.get("installed_tree_root") != result.tree_root:
            failures.append(
                f"{release_json}: DMG installed-tree root differs from the Shipping zip")
        else:
            for source in (release.dmg_path, release.dmg_sidecar_path):
                if source is None:
                    continue
                published = directory / source.name
                if published.is_file() and formats.sha256_file(published) != \
                        formats.sha256_file(source):
                    failures.append(f"{published.name}: differs from {release_json}")
    return release


def _check_dmg_assets(directory: Path, result: PlatformResult,
                      bound_release: Optional[formats.GuardedRelease],
                      inspect_contents: bool, allow_unnotarized: bool,
                      failures: List[str]) -> None:
    """Verify the Mac-only first-install assets, without adding them to update data."""
    commit = result.manifest.document["build_commit"]
    expected = formats.guarded_install_dmg_asset(commit, result.configuration)
    dmg_path = directory / expected
    sidecar_path = directory / mac_dmg.sidecar_name(expected)
    result.dmg = expected
    if bound_release is None or bound_release.dmg is None:
        failures.append(
            "a Mac publish set requires --release Mac=<bound release.json>; "
            "the DMG sidecar is not an independent trust source")
        return
    try:
        document = mac_dmg.verify_sidecar(dmg_path, sidecar_path)
    except (OSError, mac_dmg.DmgError) as exc:
        failures.append(f"{expected}: {exc}")
        return
    result.dmg_notarized = document["notarized"]
    if (result.configuration == formats.SHIPPING_CONFIGURATION
            and not document["notarized"] and not allow_unnotarized):
        failures.append(
            f"{expected}: Mac Shipping DMG is not notarized; pass "
            "--allow-unnotarized only for an intentional exception")
    binding = bound_release.dmg
    binding_failed = False
    for key in ("background_sha256", "name", "notarized", "size", "sha256",
                "volume_name", "installed_tree_root"):
        if document.get(key) != binding.get(key):
            binding_failed = True
            failures.append(
                f"{expected}: published DMG/sidecar {key} differs from the "
                "bound Mac release.json")
    sidecar_binding = binding.get("verify_sidecar")
    if not isinstance(sidecar_binding, dict) or formats.sha256_file(sidecar_path) != (
            sidecar_binding.get("sha256"), sidecar_binding.get("size")):
        binding_failed = True
        failures.append(
            f"{sidecar_path.name}: published sidecar differs from the bound "
            "Mac release.json")
    if binding_failed:
        return
    if document["installed_tree_root"] != result.tree_root:
        failures.append(
            f"{expected}: installed-tree root differs from the Shipping zip")
        return
    if inspect_contents:
        try:
            mac_dmg.verify_dmg(
                dmg_path, result.tree_root, document["background_sha256"],
                notarized=document["notarized"])
        except (OSError, mac_dmg.DmgError) as exc:
            failures.append(f"{expected}: Mac content inspection failed: {exc}")
            return
        result.dmg_inspection = "read-only-layout-alias-codesign-tree-root-pass"
    else:
        result.dmg_inspection = (
            f"name-size-sha256-pass; contents-not-inspected-on-{sys.platform}")


def _verify_update_sequence_gate(sequence: int, releases: Dict[str, Path],
                                 funnel_receipts: Dict[str, Path],
                                 allow_staging_sequence: bool,
                                 failures: List[str]) -> dict:
    # Receipt evidence is advisory. Authentication and tree checks elsewhere
    # still populate failures; a missing/bad receipt never supplies proof.
    for platform in sorted(set(funnel_receipts) - set(formats.PLATFORMS)):
        print(f"WARNING: {platform}: unknown --funnel-receipt platform; "
              "no validated funnel proof", file=sys.stderr)
    proofs = {}
    for platform in formats.PLATFORMS:
        if platform not in releases or platform not in funnel_receipts:
            print(f"WARNING: {platform}: missing --release or --funnel-receipt; "
                  "no validated funnel proof", file=sys.stderr)
            continue
        try:
            release = formats.load_guarded_release(releases[platform], platform)
            required_cells = formats.FUNNEL_REQUIRED_CELLS
            if sequence > 0:
                required_cells += formats.UPDATE_FUNNEL_CELLS_BY_PLATFORM[platform]
            proofs[platform] = formats.validate_funnel_receipt(
                funnel_receipts[platform], platform, release, required_cells)
        except (OSError, FormatError, KeyError, TypeError, ValueError) as exc:
            print(f"WARNING: {platform}: {exc}; no validated funnel proof",
                  file=sys.stderr)
    if sequence <= 0:
        return {"mode": "sequence-zero"}
    if len(proofs) != len(formats.PLATFORMS):
        return {"mode": "unproven", "platforms": proofs}
    return {"mode": "task7-update-funnel", "platforms": proofs}


def _verify_publish_set(directory: Path, *, trust: formats.Trust,
                        compiled: formats.CompiledConstants,
                        previous: Optional[Dict[str, formats.Announcement]],
                        releases: Optional[Dict[str, Path]],
                        funnel_receipts: Optional[Dict[str, Path]],
                        allow_staging_sequence: bool,
                        openssl: Optional[str], allow_test: bool,
                        allow_unnotarized: bool,
                        require_previous: bool,
                        inspect_mac_dmg: bool) -> VerifiedSet:
    directory = Path(directory)
    failures: List[str] = []
    if not trust.recovery_url.endswith("/" + formats.KEY_RING_ASSET):
        failures.append("the compiled recovery URL does not name the key ring asset")
    ring, ring_raw, ring_signer = _verify_ring(directory, trust, openssl, failures)
    results: Dict[str, PlatformResult] = {}
    if ring is not None:
        for platform in formats.PLATFORMS:
            result = _verify_platform(directory, platform, trust, compiled, ring,
                                      ring_raw, openssl, failures, allow_test)
            if result is not None:
                results[platform] = result
        if not failures and sorted(results) != sorted(formats.PLATFORMS):
            failures.append("a publish set must cover both Win64 and Mac: the latest "
                            "pointer serves both compiled announcement URLs from one release")

    tags = set()
    for result in results.values():
        for url, asset in ((result.announcement.key_ring.url, formats.KEY_RING_ASSET),
                           (result.announcement.manifest.url,
                            formats.manifest_asset(result.announcement.platform)),
                           (result.manifest.archive.url, result.archive_name)):
            tags.add(_tag_from_url(url, asset))
    if len(tags) > 1:
        failures.append(f"signed URLs name more than one tag: {sorted(map(str, tags))}")
    for field_name in ("release_sequence", "key_id", "key_ring_epoch", "domain"):
        values = {getattr(result.announcement, field_name) for result in results.values()}
        if len(values) > 1:
            failures.append(f"platforms disagree on announcement {field_name}: {values}")
    for field_name in ("build_commit", "human_version", "archive_root_id",
                       "cache_contract_id"):
        values = {result.manifest.document[field_name] for result in results.values()}
        if len(values) > 1:
            failures.append(f"platforms disagree on manifest {field_name}: {values}")
    configurations = {result.configuration for result in results.values()}
    if len(configurations) > 1:
        failures.append(f"platforms disagree on target_attestation.configuration: "
                        f"{sorted(configurations)}")

    sequences = {result.announcement.release_sequence for result in results.values()}
    sequence = sequences.pop() if len(sequences) == 1 else -1
    sequence_gate = _verify_update_sequence_gate(
        sequence, dict(releases or {}), dict(funnel_receipts or {}),
        allow_staging_sequence, failures)
    previous_platforms = set(previous or {})
    if (require_previous and sequence > 0
            and previous_platforms != set(formats.PLATFORMS)):
        failures.append(
            "positive release sequence requires authenticated --previous "
            "announcements for exactly Win64 and Mac")
    for platform, earlier in (previous or {}).items():
        current = results.get(platform)
        if current is None:
            continue
        if earlier.domain != current.announcement.domain:
            failures.append(f"{platform}: the published domain differs")
        if current.announcement.release_sequence < earlier.release_sequence:
            failures.append(
                f"{platform}: release_sequence {current.announcement.release_sequence} is "
                f"below the published {earlier.release_sequence} (rollback)")
        elif current.announcement.release_sequence == earlier.release_sequence \
                and earlier.release_sequence != 0:
            failures.append(
                f"{platform}: release_sequence {earlier.release_sequence} is already "
                "published; a new release needs a strictly greater sequence")
        if current.announcement.key_ring_epoch < earlier.key_ring_epoch:
            failures.append(f"{platform}: key ring epoch went backwards")

    known = {formats.KEY_RING_ASSET, formats.KEY_RING_ASSET + formats.SIGNATURE_SUFFIX}
    for platform, result in results.items():
        known |= {formats.announcement_asset(platform),
                  formats.announcement_asset(platform) + formats.SIGNATURE_SUFFIX,
                  formats.manifest_asset(platform),
                  formats.manifest_asset(platform) + formats.SIGNATURE_SUFFIX,
                  result.archive_name}
        if platform == "Mac":
            try:
                dmg_name = formats.guarded_install_dmg_asset(
                    result.manifest.document["build_commit"], result.configuration)
            except FormatError:
                # _check_configuration already records the invalid attestation;
                # keep collecting failures instead of escaping verification.
                pass
            else:
                known |= {dmg_name, mac_dmg.sidecar_name(dmg_name)}
    assets: Dict[str, Dict[str, object]] = {}
    for path in sorted(directory.iterdir()) if directory.is_dir() else []:
        if path.is_dir():
            failures.append(f"unexpected directory {path.name} in the publish set")
            continue
        match = _INSTALL_ZIP.match(path.name)
        if path.name not in known and not match:
            failures.append(f"unexpected file {path.name} in the publish set")
            continue
        try:
            formats.validate_asset_name(path.name)
        except FormatError as exc:
            failures.append(str(exc))
        if match:
            platform, flavor, commit = match.group(1), match.group(2), match.group(3)
            result = results.get(platform)
            if result is None:
                continue
            if commit != result.manifest.document["build_commit"]:
                failures.append(f"{path.name}: commit differs from the manifest")
            if (flavor == "shipping") != (
                    result.configuration == formats.SHIPPING_CONFIGURATION):
                failures.append(f"{path.name}: install zip name does not match the "
                                f"{result.configuration!r} attestation")
            result.install_zip = path.name
            _check_install_zip(directory, path.name, platform, result, failures)
        digest, size = formats.sha256_file(path)
        assets[path.name] = {"sha256": digest, "size": size}
    checked_releases: Dict[str, formats.GuardedRelease] = {}
    for platform, release_json in (releases or {}).items():
        if platform in results:
            checked = _check_release_json(
                Path(release_json), platform, results[platform], directory, failures)
            if checked is not None:
                checked_releases[platform] = checked
    if ("Mac" in results
            and results["Mac"].configuration in formats.GUARDED_CONFIGURATIONS):
        _check_dmg_assets(directory, results["Mac"], checked_releases.get("Mac"),
                          inspect_mac_dmg, allow_unnotarized, failures)
    if failures:
        raise VerifyError(failures)
    first = next(iter(results.values()))
    return VerifiedSet(
        directory=directory, tag=tags.pop(), release_sequence=sequence,
        key_ring_epoch=first.announcement.key_ring_epoch,
        key_id=first.announcement.key_id,
        build_commit=first.manifest.document["build_commit"], ring=ring,
        ring_signer=ring_signer, platforms=results, sequence_gate=sequence_gate,
        assets=assets)


def verify_publish_set(directory: Path, *, trust: formats.Trust,
                       compiled: formats.CompiledConstants,
                       previous: Optional[Dict[str, formats.Announcement]] = None,
                       releases: Optional[Dict[str, Path]] = None,
                       funnel_receipts: Optional[Dict[str, Path]] = None,
                       allow_staging_sequence: bool = False,
                       openssl: Optional[str] = None,
                       allow_test: bool = False,
                       allow_unnotarized: bool = False,
                       inspect_mac_dmg: Optional[bool] = None) -> VerifiedSet:
    """Verify a candidate set, including the mandatory positive-sequence history."""
    return _verify_publish_set(
        directory, trust=trust, compiled=compiled, previous=previous,
        releases=releases, funnel_receipts=funnel_receipts,
        allow_staging_sequence=allow_staging_sequence, openssl=openssl,
        allow_test=allow_test, allow_unnotarized=allow_unnotarized,
        require_previous=True,
        inspect_mac_dmg=(sys.platform == "darwin"
                         if inspect_mac_dmg is None else inspect_mac_dmg))


def _verify_signing_output(directory: Path, *, trust: formats.Trust,
                           compiled: formats.CompiledConstants,
                           releases: Dict[str, Path],
                           funnel_receipts: Dict[str, Path],
                           allow_staging_sequence: bool,
                           allow_test: bool,
                           allow_unnotarized: bool = False,
                           inspect_mac_dmg: Optional[bool] = None) -> VerifiedSet:
    """Re-verify sign_release output; signing has no previous-history option."""
    return _verify_publish_set(
        directory, trust=trust, compiled=compiled, previous=None,
        releases=releases, funnel_receipts=funnel_receipts,
        allow_staging_sequence=allow_staging_sequence, openssl=None,
        allow_test=allow_test, allow_unnotarized=allow_unnotarized,
        require_previous=False,
        inspect_mac_dmg=(sys.platform == "darwin"
                         if inspect_mac_dmg is None else inspect_mac_dmg))


def load_previous_announcements(directory: Path, trust: formats.Trust,
                                compiled: formats.CompiledConstants
                                ) -> Dict[str, formats.Announcement]:
    """Authenticate currently-published announcements before trusting their sequence."""
    previous = {}
    for platform in formats.PLATFORMS:
        name = formats.announcement_asset(platform)
        path = Path(directory) / name
        if not path.is_file():
            continue
        raw = path.read_bytes()
        signature_path = Path(str(path) + formats.SIGNATURE_SUFFIX)
        signature = signature_path.read_bytes() if signature_path.is_file() else b""
        if not crypto.verify_framed(trust.announcement_key, crypto.CONTEXT_ANNOUNCEMENT_V1,
                                    raw, signature):
            raise VerifyError([f"the published {name} does not verify; refusing to "
                               "compare sequences against unauthenticated bytes"])
        previous[platform] = formats.parse_announcement(raw, platform,
                                                        compiled.announcement_domain)
    return previous


def write_receipt(path: Path, document: dict) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(document, indent=2, sort_keys=True) + "\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def trust_inputs(args) -> dict:
    return {
        "trust": Path(args.trust), "root_ring_path": Path(args.root_ring),
        "announcement_path": Path(args.announcement_export),
        "header_path": None if args.no_trust_header else Path(args.trust_header),
    }


def add_trust_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--trust", default=str(formats.DEFAULT_TRUST))
    parser.add_argument("--root-ring", default=str(formats.DEFAULT_ROOT_RING))
    parser.add_argument("--announcement-export",
                        default=str(formats.DEFAULT_ANNOUNCEMENT_EXPORT))
    parser.add_argument("--trust-header", default=str(formats.TRUST_HEADER))
    parser.add_argument("--no-trust-header", action="store_true",
                        help="skip the generated-header cross-check (tests only)")
    parser.add_argument("--artifact-types", default=str(formats.ARTIFACT_TYPES_HEADER))


def load_trust_from_args(args):
    inputs = trust_inputs(args)
    trust = formats.load_trust(inputs["trust"], inputs["root_ring_path"],
                               inputs["announcement_path"], inputs["header_path"])
    compiled = formats.load_compiled_constants(Path(args.artifact_types))
    return trust, compiled


def parse_platform_paths(values: Sequence[str], what: str) -> Dict[str, Path]:
    result = {}
    for value in values or ():
        platform, _, path = value.partition("=")
        if what == "funnel-receipt" and platform not in formats.PLATFORMS:
            print(f"WARNING: {platform}: unknown --funnel-receipt platform; "
                  "no validated funnel proof", file=sys.stderr)
            continue
        if platform not in formats.PLATFORMS or not path or platform in result:
            raise SystemExit(f"--{what} expects PLATFORM=PATH once per platform, "
                             f"got {value!r}")
        result[platform] = Path(path)
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    result.add_argument("--set", required=True, help="the publish-set directory")
    result.add_argument("--release", action="append", default=[],
                        help="PLATFORM=path/to/release.json; Mac is always required "
                        "when the set contains its DMG")
    result.add_argument("--funnel-receipt", action="append", default=[],
                        help="PLATFORM=path/to/funnel-receipt.json; unproven "
                        "evidence warns without refusing verification")
    result.add_argument("--previous", help="directory holding the currently published "
                        "WoT-<P>-announcement-v1.json(.sig) for the monotonic check")
    result.add_argument("--allow-staging-sequence", action="store_true",
                        help="compatibility no-op; funnel receipts are advisory")
    result.add_argument("--allow-test", action="store_true",
                        help="accept a set attesting the Test configuration (a "
                        "deliberate Test publication; B4). Flagged and refused otherwise")
    result.add_argument("--allow-unnotarized", action="store_true",
                        help="accept an unnotarized Mac Shipping DMG intentionally")
    result.add_argument("--openssl-cross-check", action="store_true",
                        help="also verify every signature with the OpenSSL CLI")
    result.add_argument("--openssl", help="OpenSSL 3 CLI for --openssl-cross-check")
    result.add_argument("--receipt", help="write a verify receipt here")
    add_trust_arguments(result)
    return result


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parser().parse_args(argv)
    try:
        trust, compiled = load_trust_from_args(args)
        openssl = crypto.find_openssl(args.openssl) if args.openssl_cross_check else None
        previous = load_previous_announcements(Path(args.previous), trust, compiled) \
            if args.previous else None
        verified = verify_publish_set(
            Path(args.set), trust=trust, compiled=compiled, previous=previous,
            releases=parse_platform_paths(args.release, "release"),
            funnel_receipts=parse_platform_paths(args.funnel_receipt, "funnel-receipt"),
            allow_staging_sequence=args.allow_staging_sequence, openssl=openssl,
            allow_test=args.allow_test,
            allow_unnotarized=args.allow_unnotarized)
    except VerifyError as exc:
        for failure in exc.failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"verify_release: {len(exc.failures)} failure(s)", file=sys.stderr)
        return 1
    except (FormatError, crypto.CryptoError, OSError) as exc:
        print(f"verify_release: {exc}", file=sys.stderr)
        return 1
    summary = verified.summary()
    if args.receipt:
        write_receipt(Path(args.receipt), {
            "format": VERIFY_RECEIPT_FORMAT,
            "generated_utc": datetime.now(timezone.utc).isoformat(),
            "openssl_cross_check": bool(args.openssl_cross_check),
            "set": summary,
            "trust_sha256": formats.sha256_hex(trust.trust_raw),
            "verdict": "pass",
        })
    print(json.dumps(summary, indent=2, sort_keys=True))
    mac = summary["platforms"].get("Mac")
    if mac and mac.get("dmg_inspection"):
        if "contents-not-inspected" in mac["dmg_inspection"]:
            print("verify_release: the DMG is bound only to the unsigned "
                  "release.json; its contents were not inspected on this host "
                  "(name, size, SHA-256, sidecar and tree root are consistent)")
        else:
            print("verify_release: Mac DMG contents inspected read-only; layout, "
                  "Applications link, codesign, and installed-tree root pass")
    print(f"verify_release: PASS ({summary['tag']}, sequence "
          f"{summary['release_sequence']}, {len(summary['assets'])} assets)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
