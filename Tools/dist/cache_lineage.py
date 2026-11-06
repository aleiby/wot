#!/usr/bin/env python3
"""Release gate A: a platform's cache bytes change only with a new content revision.

Every cache records the content label of the ``WoTCacheContentRevision`` its
binary was built with (cache_content_label). Two caches with the same label,
installer and contract must be byte-identical, so a release whose Shipping cache
differs from the previous release's must carry a strictly higher revision. The
reference is the previous published release's cache, taken from outside the
candidate tree:

  reference-from-lineage  the previous release's signed
                          WoT-<P>-cache-lineage-v1.json, authenticated with
                          verify_release (every release from sequence 1 on
                          publishes one);
  rederive                a one-time reconstruction for a release that published
                          no lineage: authenticate its signed manifest, prove the
                          packaged game is the signed main executable, run that
                          Shipping build fresh against the pinned installer with
                          the Shipping smoke harness (boot only), and record the
                          cache it builds. The work root keeps that cache.

``gate`` then compares the candidate's Shipping smoke cache with the reference:

  * the reference revision is recovered from its label; an unknown label fails;
  * the candidate label must be the label of the revision compiled into this
    source (CacheBuilder.cpp), or the binary does not match the source;
  * equal revisions require the identical manifest (canonical SHA-256);
  * a higher candidate revision passes; a lower one fails.

The passing gate receipt carries the lineage values sign_release signs into
this release's own lineage asset (sign_release --cache-gate). Every JSON digest
here is SHA-256 over release_formats.canonical_bytes; files that are not
canonical JSON documents (the smoke receipt) are hashed as files.

Stdlib only; runs on Python 3.9.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

DIST = Path(__file__).resolve().parent
if str(DIST) not in sys.path:
    sys.path.insert(0, str(DIST))

import cache_content_label as content_label  # noqa: E402
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import verify_release  # noqa: E402
from release_formats import FormatError  # noqa: E402

REFERENCE_FORMAT = "wot-cache-reference-v1"
GATE_FORMAT = "wot-cache-gate-a-v1"
# smoke_shipping_release.SMOKE_FORMAT and its receipt name; restated so this
# module does not import the launch harness.
SMOKE_FORMAT = "wot-shipping-smoke-v1"
SMOKE_RECEIPT_NAME = "shipping-smoke-receipt.json"
SMOKE_SCRIPT = DIST / "smoke_shipping_release.py"
FRESH_FIRST_RUN = "fresh-first-run"
# Labels are recovered by searching revisions 1..N; every label is one SHA-256.
REVISION_SEARCH_LIMIT = 4096

IDENTITY_KEYS = ("installer_sha256", "contract_sha256", "label")
LINEAGE_VALUE_KEYS = ("revision", "label", "installer_sha256", "contract_sha256",
                      "manifest_sha256")
REFERENCE_KEYS = frozenset({
    "format", "source", "platform", "release_sequence", "revision", "label",
    "installer_sha256", "contract_sha256", "manifest_sha256"})
REDERIVED_KEYS = REFERENCE_KEYS | {
    "signed_manifest_sha256", "main_executable_sha256", "smoke_receipt_sha256",
    "content_cache_dir"}
GATE_KEYS = frozenset({
    "format", "verdict", "platform", "identity_unchanged", "reference",
    "candidate", "lineage"})
GATE_REFERENCE_KEYS = frozenset({
    "source", "release_sequence", "manifest_sha256", "label", "revision",
    "installer_sha256", "contract_sha256"})
GATE_CANDIDATE_KEYS = frozenset({
    "manifest_sha256", "label", "revision", "installer_sha256", "contract_sha256",
    "release_asset_sha256"})
GATE_LINEAGE_KEYS = frozenset({"platform", *LINEAGE_VALUE_KEYS})

Runner = Callable[[List[str]], int]


class GateError(Exception):
    """Gate A or a reference derivation refused; the message is the reason."""


# ---------------------------------------------------------------------------
# Small checked readers
# ---------------------------------------------------------------------------

def _is_sha256(value) -> bool:
    return (isinstance(value, str) and len(value) == 64
            and all(character in "0123456789abcdef" for character in value))


def _require_sha256(value, what: str) -> str:
    if not _is_sha256(value):
        raise GateError(f"{what} is not 64 lower-case hex characters")
    return value


def _require_int(value, what: str, minimum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) \
            or value < minimum or value > formats.INT64_MAX:
        raise GateError(f"{what} is not an integer in [{minimum}, int64 max]")
    return value


def _require_keys(document, keys, what: str) -> dict:
    if not isinstance(document, dict):
        raise GateError(f"{what} is not a JSON object")
    if set(document) != set(keys):
        unknown = sorted(set(document) - set(keys))
        missing = sorted(set(keys) - set(document))
        raise GateError(f"{what} fields differ: unknown={unknown} missing={missing}")
    return document


def _platform(value: str) -> str:
    if value not in formats.PLATFORMS:
        raise GateError(f"unknown platform {value!r}")
    return value


def load_json(path: Path, what: str):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        raise GateError(f"cannot read {what} {path}: {exc}") from exc


def document_sha256(document, what: str) -> str:
    """SHA-256 of a JSON document's canonical bytes."""
    try:
        return formats.sha256_hex(formats.canonical_bytes(document, what))
    except FormatError as exc:
        raise GateError(str(exc)) from exc


def revision_of(label: str, what: str, limit: int = REVISION_SEARCH_LIMIT) -> int:
    """The content revision whose label is ``label``; an unknown label fails."""
    _require_sha256(label, f"{what} label")
    revision = content_label.revision_for_label(label, limit)
    if revision is None:
        raise GateError(f"{what} label {label} is not the label of any content "
                        f"revision 1..{limit}")
    return revision


# ---------------------------------------------------------------------------
# Captured cache manifests
# ---------------------------------------------------------------------------

def manifest_identity(manifest, what: str) -> dict:
    """installer, contract, label and canonical digest of one cache manifest."""
    if not isinstance(manifest, dict):
        raise GateError(f"{what} is not a JSON object")
    if (manifest.get("format") != formats.CACHE_MANIFEST_FORMAT
            or manifest.get("cache_version") != formats.CACHE_MANIFEST_VERSION
            or manifest.get("hash_algorithm") != "sha256"):
        raise GateError(f"{what} is not a {formats.CACHE_MANIFEST_FORMAT} "
                        f"version {formats.CACHE_MANIFEST_VERSION} manifest")
    return {
        "installer_sha256": _require_sha256(
            manifest.get("installer_sha256"), f"{what}.installer_sha256"),
        "contract_sha256": _require_sha256(
            manifest.get("contract_sha256"), f"{what}.contract_sha256"),
        "label": _require_sha256(manifest.get("production_descriptor_sha256"),
                                 f"{what}.production_descriptor_sha256"),
        "manifest_sha256": document_sha256(manifest, what),
    }


def _captured_manifest(manifests, what: str) -> Tuple[dict, dict]:
    """manifests[0]: its JSON, checked against the digest the capture recorded."""
    if not isinstance(manifests, list) or not manifests:
        raise GateError(f"{what} captured no cache manifest")
    snapshot = manifests[0]
    manifest = snapshot.get("json") if isinstance(snapshot, dict) else None
    if not isinstance(manifest, dict):
        raise GateError(f"{what} captured no cache manifest JSON")
    if "sha256" in snapshot and snapshot["sha256"] != document_sha256(manifest, what):
        raise GateError(f"{what}: the captured manifest's canonical SHA-256 is not "
                        "the digest recorded with it")
    return manifest, snapshot


def smoke_manifest(receipt, platform: str) -> Tuple[dict, dict]:
    """checks.boot.cache.manifests[0] of a passing Shipping smoke receipt."""
    what = "Shipping smoke receipt"
    if not isinstance(receipt, dict) or receipt.get("format") != SMOKE_FORMAT:
        raise GateError(f"{what} is not {SMOKE_FORMAT}")
    if receipt.get("verdict") != "pass":
        raise GateError(f"{what} verdict is {receipt.get('verdict')!r}, not 'pass'")
    if receipt.get("platform") != platform:
        raise GateError(f"{what} platform {receipt.get('platform')!r} is not {platform}")
    checks = receipt.get("checks")
    boot = checks.get("boot") if isinstance(checks, dict) else None
    cache = boot.get("cache") if isinstance(boot, dict) else None
    manifests = cache.get("manifests") if isinstance(cache, dict) else None
    return _captured_manifest(manifests, f"{what} checks.boot.cache")


def funnel_manifest(receipt, platform: str) -> dict:
    """The fresh-first-run cell's captured manifest of a Test-twin funnel receipt."""
    what = "funnel receipt"
    if not isinstance(receipt, dict):
        raise GateError(f"{what} is not a JSON object")
    if receipt.get("platform") != platform:
        raise GateError(f"{what} platform {receipt.get('platform')!r} is not {platform}")
    cells = receipt.get("cells")
    matches = [cell for cell in cells or () if isinstance(cell, dict)
               and cell.get("name") == FRESH_FIRST_RUN] \
        if isinstance(cells, list) else []
    if len(matches) != 1:
        raise GateError(f"{what} has no single {FRESH_FIRST_RUN} cell")
    cell = matches[0]
    if cell.get("verdict") != "pass":
        raise GateError(f"{what} {FRESH_FIRST_RUN} verdict is "
                        f"{cell.get('verdict')!r}, not 'pass'")
    evidence = cell.get("evidence")
    manifest, _ = _captured_manifest(
        evidence.get("manifests") if isinstance(evidence, dict) else None,
        f"{what} {FRESH_FIRST_RUN}")
    return manifest


# ---------------------------------------------------------------------------
# References
# ---------------------------------------------------------------------------

def load_reference(document, platform: str) -> dict:
    """A wot-cache-reference-v1 document, rederived or taken from a lineage."""
    what = "cache reference"
    if not isinstance(document, dict):
        raise GateError(f"{what} is not a JSON object")
    source = document.get("source")
    if source == "rederived":
        _require_keys(document, REDERIVED_KEYS, what)
        for key in ("signed_manifest_sha256", "main_executable_sha256",
                    "smoke_receipt_sha256"):
            _require_sha256(document[key], f"{what}.{key}")
        if not isinstance(document["content_cache_dir"], str) \
                or not document["content_cache_dir"]:
            raise GateError(f"{what}.content_cache_dir is not a path")
    elif source == "lineage":
        _require_keys(document, REFERENCE_KEYS, what)
    else:
        raise GateError(f"{what}.source {source!r} is neither 'rederived' nor 'lineage'")
    if document["format"] != REFERENCE_FORMAT:
        raise GateError(f"{what}.format is not {REFERENCE_FORMAT}")
    if document["platform"] != platform:
        raise GateError(f"{what}.platform {document['platform']!r} is not {platform}")
    _require_int(document["release_sequence"], f"{what}.release_sequence", 0)
    _require_int(document["revision"], f"{what}.revision", 1)
    for key in ("label", "installer_sha256", "contract_sha256", "manifest_sha256"):
        _require_sha256(document[key], f"{what}.{key}")
    return document


def reference_from_lineage(directory: Path, platform: str, *, trust, compiled,
                           openssl: Optional[str] = None) -> dict:
    """The reference recorded by a previous release's signed lineage asset."""
    _platform(platform)
    try:
        lineage = verify_release.load_verified_cache_lineage(
            Path(directory), platform, trust=trust, compiled=compiled,
            openssl=openssl)
    except verify_release.VerifyError as exc:
        raise GateError("the previous release's cache lineage does not verify: "
                        + "; ".join(exc.failures)) from exc
    return {
        "contract_sha256": lineage["contract_sha256"],
        "format": REFERENCE_FORMAT,
        "installer_sha256": lineage["installer_sha256"],
        "label": lineage["label"],
        "manifest_sha256": lineage["manifest_sha256"],
        "platform": platform,
        "release_sequence": lineage["release_sequence"],
        "revision": lineage["revision"],
        "source": "lineage",
    }


def _script_command(script: Path) -> List[str]:
    """A .py runner is spawned through this interpreter (Windows has no shebangs)."""
    if Path(script).suffix.lower() == ".py":
        return [sys.executable, str(script)]
    return [str(script)]


def _run(command: List[str]) -> int:
    return subprocess.run(command).returncode


def _require_absent_or_empty(path: Path, what: str) -> None:
    if path.exists() and (not path.is_dir() or any(path.iterdir())):
        raise GateError(f"{what} {path} must be absent or empty")


def _main_executable_member(archive_path: Path, relative: str) -> Tuple[str, str, int]:
    """The single zip member ending in /<relative>: its name, SHA-256 and size."""
    suffix = "/" + relative
    try:
        with zipfile.ZipFile(archive_path) as archive:
            members = [info for info in archive.infolist()
                       if not info.is_dir() and info.filename.endswith(suffix)]
            if len(members) != 1:
                raise GateError(f"{archive_path.name} holds {len(members)} members "
                                f"ending in {suffix}, not exactly one")
            digest = hashlib.sha256()
            size = 0
            with archive.open(members[0]) as handle:
                for chunk in iter(lambda: handle.read(1 << 20), b""):
                    digest.update(chunk)
                    size += len(chunk)
            return members[0].filename, digest.hexdigest(), size
    except (OSError, zipfile.BadZipFile) as exc:
        raise GateError(f"cannot read {archive_path}: {exc}") from exc


def _content_cache_dir(smoke_root: Path, manifest_sha256: str) -> Path:
    """The retained ContentCache whose current manifest is the captured one."""
    found = sorted(path for path in smoke_root.rglob("manifest.json")
                   if path.parent.name == "current"
                   and path.parent.parent.name == "ContentCache")
    if len(found) != 1:
        raise GateError(f"{smoke_root} holds {len(found)} ContentCache/current/"
                        "manifest.json files, not exactly one")
    manifest = load_json(found[0], "retained cache manifest")
    if document_sha256(manifest, "retained cache manifest") != manifest_sha256:
        raise GateError(f"the retained {found[0]} is not the captured manifest")
    return found[0].parent.parent


def rederive(*, platform: str, previous_set: Path, release_json: Path,
             installer: Path, work_root: Path, trust, compiled,
             openssl: Optional[str] = None, smoke_script: Path = SMOKE_SCRIPT,
             runner: Optional[Runner] = None) -> dict:
    """Rebuild a published release's cache reference from its signed binary."""
    _platform(platform)
    previous_set = Path(previous_set)
    release_json = Path(release_json)
    work_root = Path(work_root).resolve()
    # 1. The previous release's signed chain for this platform.
    try:
        signed = verify_release.authenticate_platform(
            previous_set, platform, trust=trust, compiled=compiled, openssl=openssl)
    except verify_release.VerifyError as exc:
        raise GateError(f"the previous {platform} release does not verify: "
                        + "; ".join(exc.failures)) from exc
    main = signed.manifest.main_executable
    signed_manifest_sha256, _ = formats.sha256_file(
        previous_set / formats.manifest_asset(platform))

    # 2. The packaged game the smoke will run is the signed main executable.
    try:
        release = formats.load_guarded_release(release_json, platform)
    except (OSError, FormatError) as exc:
        raise GateError(f"{release_json}: {exc}") from exc
    if release.document.get("git_commit") != signed.manifest.document["build_commit"]:
        raise GateError(f"{release_json} names commit "
                        f"{release.document.get('git_commit')!r}, not the signed "
                        f"build_commit {signed.manifest.document['build_commit']!r}")
    member, digest, size = _main_executable_member(release.zip_path, main.name)
    if (digest, size) != (main.sha256, main.size):
        raise GateError(f"{release.zip_path.name} member {member} is not the signed "
                        f"main_executable {main.name} ({main.sha256})")

    # 3. Run it fresh against the pinned installer, boot only.
    smoke_root = work_root / "smoke"
    evidence = work_root / "evidence"
    _require_absent_or_empty(smoke_root, "smoke work root")
    _require_absent_or_empty(evidence, "smoke evidence directory")
    command = _script_command(Path(smoke_script)) + [
        "--release", str(release_json), "--installer", str(installer),
        "--host-trigger", "none", "--allow-source-drift",
        "--work-root", str(smoke_root), "--evidence", str(evidence)]
    returncode = (runner or _run)(command)
    if returncode != 0:
        raise GateError(f"the Shipping smoke exited {returncode}")

    # 4. The cache it built.
    receipt_path = evidence / SMOKE_RECEIPT_NAME
    receipt = load_json(receipt_path, "Shipping smoke receipt")
    manifest, _ = smoke_manifest(receipt, platform)
    run = receipt.get("release") if isinstance(receipt.get("release"), dict) else {}
    if (run.get("asset_sha256"), run.get("manifest_sha256")) != (
            release.document.get("sha256"), release.release_json_sha256):
        raise GateError("the Shipping smoke receipt names a different release.json "
                        "or asset than the one given")
    identity = manifest_identity(manifest, "rederived cache manifest")
    revision = revision_of(identity["label"], "rederived cache manifest")
    content_cache = _content_cache_dir(smoke_root, identity["manifest_sha256"])
    return {
        "content_cache_dir": str(content_cache),
        "contract_sha256": identity["contract_sha256"],
        "format": REFERENCE_FORMAT,
        "installer_sha256": identity["installer_sha256"],
        "label": identity["label"],
        "main_executable_sha256": main.sha256,
        "manifest_sha256": identity["manifest_sha256"],
        "platform": platform,
        "release_sequence": signed.manifest.document["release_sequence"],
        "revision": revision,
        "signed_manifest_sha256": signed_manifest_sha256,
        "smoke_receipt_sha256": formats.sha256_file(receipt_path)[0],
        "source": "rederived",
    }


# ---------------------------------------------------------------------------
# Gate A
# ---------------------------------------------------------------------------

def evaluate_gate(*, platform: str, reference: dict, smoke_receipt: dict,
                  compiled_revision: int, funnel_receipt: Optional[dict] = None
                  ) -> dict:
    """The gate A rules. Returns the passing receipt or raises GateError."""
    _platform(platform)
    reference = load_reference(reference, platform)
    limit = max(REVISION_SEARCH_LIMIT, compiled_revision)
    reference_revision = revision_of(reference["label"], "reference", limit)
    if reference_revision != reference["revision"]:
        raise GateError(f"the reference records revision {reference['revision']} but "
                        f"its label is the label of revision {reference_revision}")

    manifest, _ = smoke_manifest(smoke_receipt, platform)
    candidate = manifest_identity(manifest, "candidate cache manifest")
    run = smoke_receipt.get("release")
    asset_sha256 = run.get("asset_sha256") if isinstance(run, dict) else None
    _require_sha256(asset_sha256, "Shipping smoke receipt release.asset_sha256")
    if funnel_receipt is not None:
        funnel_sha256 = document_sha256(
            funnel_manifest(funnel_receipt, platform), "funnel cache manifest")
        if funnel_sha256 != candidate["manifest_sha256"]:
            raise GateError(
                f"the Test twin's {FRESH_FIRST_RUN} manifest {funnel_sha256} differs "
                f"from the Shipping manifest {candidate['manifest_sha256']}")

    expected_label = content_label.label(compiled_revision)
    if candidate["label"] != expected_label:
        raise GateError(
            f"the candidate cache records label {candidate['label']}, but this "
            f"source's WoTCacheContentRevision {compiled_revision} has label "
            f"{expected_label}: the smoked binary does not match the source")
    candidate_revision = compiled_revision
    if candidate_revision < reference_revision:
        raise GateError(f"content revision {candidate_revision} is lower than the "
                        f"previous release's {reference_revision}")
    if (candidate_revision == reference_revision
            and candidate["manifest_sha256"] != reference["manifest_sha256"]):
        raise GateError(
            f"content revision {candidate_revision} is unchanged but the cache "
            f"manifest changed ({reference['manifest_sha256']} -> "
            f"{candidate['manifest_sha256']}): the cache bytes changed without a "
            "WoTCacheContentRevision bump")
    gate_reference = {
        "contract_sha256": reference["contract_sha256"],
        "installer_sha256": reference["installer_sha256"],
        "label": reference["label"],
        "manifest_sha256": reference["manifest_sha256"],
        # sign_release requires this to be the immediately previous release.
        "release_sequence": reference["release_sequence"],
        "revision": reference_revision,
        "source": reference["source"],
    }
    if "content_cache_dir" in reference:
        gate_reference["content_cache_dir"] = reference["content_cache_dir"]
    return {
        "candidate": {
            "contract_sha256": candidate["contract_sha256"],
            "installer_sha256": candidate["installer_sha256"],
            "label": candidate["label"],
            "manifest_sha256": candidate["manifest_sha256"],
            "release_asset_sha256": asset_sha256,
            "revision": candidate_revision,
        },
        "format": GATE_FORMAT,
        "identity_unchanged": all(candidate[key] == reference[key]
                                  for key in IDENTITY_KEYS),
        "lineage": {
            "contract_sha256": candidate["contract_sha256"],
            "installer_sha256": candidate["installer_sha256"],
            "label": candidate["label"],
            "manifest_sha256": candidate["manifest_sha256"],
            "platform": platform,
            "revision": candidate_revision,
        },
        "platform": platform,
        "reference": gate_reference,
        "verdict": "pass",
    }


def load_gate_receipt(path: Path, platform: str) -> dict:
    """A passing gate A receipt for ``platform``, re-checked in full."""
    _platform(platform)
    receipt = load_json(path, "cache gate receipt")
    what = f"cache gate receipt {path}"
    if not isinstance(receipt, dict) or receipt.get("format") != GATE_FORMAT:
        raise GateError(f"{what} is not {GATE_FORMAT}")
    if receipt.get("verdict") != "pass":
        raise GateError(f"{what} verdict is {receipt.get('verdict')!r}, not 'pass'"
                        + (f": {receipt['reason']}" if receipt.get("reason") else ""))
    if receipt.get("platform") != platform:
        raise GateError(f"{what} is for {receipt.get('platform')!r}, not {platform}")
    _require_keys(receipt, GATE_KEYS, what)
    if not isinstance(receipt["identity_unchanged"], bool):
        raise GateError(f"{what}.identity_unchanged is not a boolean")
    reference = receipt["reference"]
    keys = set(reference) if isinstance(reference, dict) else set()
    _require_keys(reference, GATE_REFERENCE_KEYS | (keys & {"content_cache_dir"}),
                  f"{what}.reference")
    candidate = _require_keys(receipt["candidate"], GATE_CANDIDATE_KEYS,
                              f"{what}.candidate")
    lineage = _require_keys(receipt["lineage"], GATE_LINEAGE_KEYS, f"{what}.lineage")
    _require_int(reference["release_sequence"], f"{what}.reference.release_sequence", 0)
    for part, document in (("reference", reference), ("candidate", candidate)):
        revision = _require_int(document["revision"], f"{what}.{part}.revision", 1)
        for key in ("manifest_sha256", "label", "installer_sha256", "contract_sha256"):
            _require_sha256(document[key], f"{what}.{part}.{key}")
        if document["label"] != content_label.label(revision):
            raise GateError(f"{what}.{part}.label is not the label of its revision")
    _require_sha256(candidate["release_asset_sha256"],
                    f"{what}.candidate.release_asset_sha256")
    if lineage != {"platform": platform,
                   **{key: candidate[key] for key in LINEAGE_VALUE_KEYS}}:
        raise GateError(f"{what}.lineage does not repeat the candidate")
    if candidate["revision"] < reference["revision"] or (
            candidate["revision"] == reference["revision"]
            and candidate["manifest_sha256"] != reference["manifest_sha256"]):
        raise GateError(f"{what} does not satisfy the gate A rules")
    if receipt["identity_unchanged"] != all(
            candidate[key] == reference[key] for key in IDENTITY_KEYS):
        raise GateError(f"{what}.identity_unchanged contradicts its values")
    return receipt


def lineage_document(gate: dict, release_sequence: int) -> dict:
    """This release's lineage asset document from its passing gate receipt."""
    lineage = gate["lineage"]
    return formats.build_cache_lineage(
        platform=lineage["platform"], release_sequence=release_sequence,
        **{key: lineage[key] for key in LINEAGE_VALUE_KEYS})


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _openssl(args) -> Optional[str]:
    return crypto.find_openssl(args.openssl) if args.openssl_cross_check else None


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = result.add_subparsers(dest="command", required=True)

    rederive_command = commands.add_parser(
        "rederive", help="rebuild a published release's cache reference from its "
        "signed Shipping binary (for a release without a lineage asset)")
    rederive_command.add_argument("--platform", required=True,
                                  choices=formats.PLATFORMS)
    rederive_command.add_argument("--previous-set", required=True,
                                  help="the previous release's publish set")
    rederive_command.add_argument("--release-json", required=True,
                                  help="its packager release.json (Shipping)")
    rederive_command.add_argument("--installer", required=True,
                                  help="the pinned original-game installer")
    rederive_command.add_argument("--work-root", required=True,
                                  help="retained root for the smoke run and its cache")
    rederive_command.add_argument("--out", required=True,
                                  help="reference JSON to write")

    lineage_command = commands.add_parser(
        "reference-from-lineage",
        help="take the reference from a previous release's signed lineage asset")
    lineage_command.add_argument("--platform", required=True, choices=formats.PLATFORMS)
    lineage_command.add_argument("--previous-set", required=True,
                                 help="the previous release's publish set")
    lineage_command.add_argument("--out", required=True, help="reference JSON to write")

    for command in (rederive_command, lineage_command):
        command.add_argument("--openssl-cross-check", action="store_true",
                             help="also verify every signature with the OpenSSL CLI")
        command.add_argument("--openssl", help="OpenSSL 3 CLI for --openssl-cross-check")
        verify_release.add_trust_arguments(command)

    gate = commands.add_parser("gate", help="compare a candidate's Shipping cache "
                               "with the reference")
    gate.add_argument("--platform", required=True, choices=formats.PLATFORMS)
    gate.add_argument("--reference", required=True, help="wot-cache-reference-v1 JSON")
    gate.add_argument("--smoke-receipt", required=True,
                      help="the candidate's Shipping smoke receipt")
    gate.add_argument("--funnel-receipt",
                      help="optional Test-twin funnel receipt; its fresh-first-run "
                      "manifest must equal the Shipping one")
    gate.add_argument("--cache-builder", default=str(content_label.CACHE_BUILDER),
                      help="CacheBuilder.cpp holding WoTCacheContentRevision")
    gate.add_argument("--out", required=True, help="gate receipt to write")
    return result


def _run_gate(args) -> int:
    platform = args.platform
    try:
        try:
            compiled_revision = content_label.compiled_revision(Path(args.cache_builder))
        except (OSError, ValueError) as exc:
            raise GateError(f"cannot read the compiled content revision: {exc}") from exc
        receipt = evaluate_gate(
            platform=platform,
            reference=load_json(Path(args.reference), "cache reference"),
            smoke_receipt=load_json(Path(args.smoke_receipt), "Shipping smoke receipt"),
            funnel_receipt=(load_json(Path(args.funnel_receipt), "funnel receipt")
                            if args.funnel_receipt else None),
            compiled_revision=compiled_revision)
    except GateError as exc:
        verify_release.write_receipt(Path(args.out), {
            "format": GATE_FORMAT, "platform": platform, "reason": str(exc),
            "verdict": "fail"})
        print(f"cache_lineage gate: FAIL: {exc}", file=sys.stderr)
        return 1
    verify_release.write_receipt(Path(args.out), receipt)
    print(f"cache_lineage gate: PASS ({platform}, revision "
          f"{receipt['candidate']['revision']}, manifest "
          f"{receipt['candidate']['manifest_sha256']}, identity_unchanged="
          f"{str(receipt['identity_unchanged']).lower()}); receipt {args.out}")
    return 0


def main(argv: Optional[Sequence[str]] = None, *,
         runner: Optional[Runner] = None,
         smoke_script: Path = SMOKE_SCRIPT) -> int:
    args = parser().parse_args(argv)
    if args.command == "gate":
        return _run_gate(args)
    try:
        trust, compiled = verify_release.load_trust_from_args(args)
        openssl = _openssl(args)
        if args.command == "rederive":
            reference = rederive(
                platform=args.platform, previous_set=Path(args.previous_set),
                release_json=Path(args.release_json), installer=Path(args.installer),
                work_root=Path(args.work_root), trust=trust, compiled=compiled,
                openssl=openssl, smoke_script=smoke_script, runner=runner)
        else:
            reference = reference_from_lineage(
                Path(args.previous_set), args.platform, trust=trust,
                compiled=compiled, openssl=openssl)
    except (GateError, FormatError, crypto.CryptoError, OSError) as exc:
        print(f"cache_lineage {args.command}: FAIL: {exc}", file=sys.stderr)
        return 1
    verify_release.write_receipt(Path(args.out), reference)
    print(f"cache_lineage {args.command}: {args.platform} revision "
          f"{reference['revision']}, label {reference['label']}, manifest "
          f"{reference['manifest_sha256']}; reference {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
