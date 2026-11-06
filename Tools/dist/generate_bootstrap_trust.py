#!/usr/bin/env python3
"""Audit key-role separation and emit the compiled-in bootstrap trust.

This is the only producer of `bootstrap-trust-v1.json` and
`BootstrapTrust.generated.h`. Shipping code never hand-codes a key, a URL, or a
path to a trust file: it compiles the generated header, which embeds

  * the announcement public key,
  * the offline-root ring's exact canonical BYTES (not its path or digest alone,
    so a shipped binary can verify a root-signed ring with no file present),
  * the digests of both inputs, and
  * exactly one platform-selected announcement URL.

The audit exists because the failure modes here are silent. A key reused across
roles turns one compromise into total compromise. A recovery URL that is merely
"close" to the fixed constant is an attacker's URL. A generated file that has
drifted from its inputs is a trust root nobody reviewed. Every one of those is a
hard failure below, never a warning.

Stdlib only. Private key material is never an input and has no representation
here: this tool sees public halves only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

TRUST_FORMAT = "wot-bootstrap-trust-v1"
ANNOUNCEMENT_EXPORT_FORMAT = "wot-announcement-public-key-v1"
OFFLINE_ROOT_RING_FORMAT = "wot-offline-root-ring-v1"

ANNOUNCEMENT_EXPORT_FIELDS = frozenset(
    {"ceremony_utc", "format", "key_id", "public_key"})
ROOT_RING_FIELDS = frozenset({"epoch", "format", "keys", "revoked_key_ids"})
ROOT_KEY_FIELDS = frozenset(
    {"first_sequence", "key_id", "last_sequence", "public_key"})

# The one permitted offline-root recovery URL. It is a `/latest/` pointer on
# purpose: its CONTENT is signature-verified against an already-trusted root, so a
# mutable location is safe, and a mutable location is the only way to recover when
# every embedded root key has been rotated out. Nothing else may be substituted,
# and this constant is compared exactly rather than pattern-matched.
ROOT_KEY_RING_RECOVERY_URL = (
    "https://github.com/aleiby/wot/releases/latest/download/"
    "WoT-root-key-ring-v1.json")
WIN64_ANNOUNCEMENT_URL = (
    "https://github.com/aleiby/wot/releases/latest/download/"
    "WoT-Win64-announcement-v1.json")
MAC_ANNOUNCEMENT_URL = (
    "https://github.com/aleiby/wot/releases/latest/download/"
    "WoT-Mac-announcement-v1.json")

# Substrings that must never appear in an input this tool reads. A ceremony export
# carrying any of them is a private key that escaped its offline machine.
PRIVATE_KEY_MARKERS = (
    "private", "secret", "seed", "begin ed25519 private", "begin private key",
    "-----begin", "passphrase", "mnemonic",
)
# Markers that must never appear in a generated production artifact.
TEST_MARKERS = ("test-keys", "test-key", "localhost", "127.0.0.1", "example.invalid",
                "WOT_BOOTSTRAP_TEST")

KEY_HEX_LENGTH = 64


class TrustError(Exception):
    """An audit rule failed. There is no partial success."""


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("utf-8")


def _require_lowercase_hex_key(value: object, what: str) -> str:
    if not isinstance(value, str):
        raise TrustError(f"{what} is not a string")
    if len(value) != KEY_HEX_LENGTH:
        raise TrustError(
            f"{what} must be {KEY_HEX_LENGTH} hex characters, got {len(value)}")
    if any(character not in "0123456789abcdef" for character in value):
        raise TrustError(
            f"{what} must be lower-case hex; artifacts have one spelling")
    return value


def _normalize_hex_key(value: object, what: str) -> str:
    """Return one lower-case spelling for a supplied 32-byte public key."""
    if not isinstance(value, str):
        raise TrustError(f"{what} is not a string")
    if len(value) != KEY_HEX_LENGTH:
        raise TrustError(
            f"{what} must be {KEY_HEX_LENGTH} hex characters, got {len(value)}")
    lowered = value.lower()
    if any(character not in "0123456789abcdef" for character in lowered):
        raise TrustError(f"{what} must be hex")
    return lowered


def _require_exact_fields(document: dict, expected: frozenset[str], what: str) -> None:
    actual = set(document)
    unknown = sorted(actual - expected)
    if unknown:
        raise TrustError(f"{what} has unknown field(s): {', '.join(unknown)}")
    missing = sorted(expected - actual)
    if missing:
        raise TrustError(f"{what} is missing field(s): {', '.join(missing)}")


def _reject_private_key_markers(raw: bytes, what: str) -> None:
    lowered = raw.decode("utf-8", "replace").lower()
    for marker in PRIVATE_KEY_MARKERS:
        if marker in lowered:
            raise TrustError(
                f"{what} contains the private-key marker {marker!r}; a ceremony "
                "export must carry public halves only")


def _load_canonical_json(path: Path, what: str) -> tuple[dict, bytes]:
    """Load a document and require its bytes to already be canonical.

    Re-serialization must reproduce the file exactly. A trust input whose bytes
    differ from its canonical form cannot be embedded and verified byte-for-byte.
    """
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise TrustError(f"cannot read {what} {path}: {exc}") from exc
    _reject_private_key_markers(raw, f"{what} {path}")
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise TrustError(f"{what} {path} is not valid UTF-8 JSON: {exc}") from exc
    if not isinstance(document, dict):
        raise TrustError(f"{what} {path} is not a JSON object")
    if canonical_bytes(document) != raw:
        raise TrustError(
            f"{what} {path} is not in canonical form; regenerate it with sorted "
            "keys, minimal separators, ASCII escapes, and no trailing newline")
    return document, raw


def load_announcement_export(path: Path) -> tuple[str, bytes]:
    document, raw = _load_canonical_json(path, "announcement ceremony export")
    _require_exact_fields(document, ANNOUNCEMENT_EXPORT_FIELDS,
                          "announcement ceremony export")
    if document["format"] != ANNOUNCEMENT_EXPORT_FORMAT:
        raise TrustError(
            f"announcement ceremony export format is {document['format']!r}, "
            f"expected {ANNOUNCEMENT_EXPORT_FORMAT!r}")
    if not isinstance(document["key_id"], str) or not document["key_id"]:
        raise TrustError("announcement ceremony export has an empty key_id")
    key = _require_lowercase_hex_key(document["public_key"],
                                     "announcement public_key")
    return key, raw


def load_root_ring(path: Path) -> tuple[dict[str, str], bytes]:
    """Return {key_id: public_key} for the offline-root ring, plus its bytes."""
    document, raw = _load_canonical_json(path, "offline-root ring")
    _require_exact_fields(document, ROOT_RING_FIELDS, "offline-root ring")
    if document["format"] != OFFLINE_ROOT_RING_FORMAT:
        raise TrustError(
            f"offline-root ring format is {document['format']!r}, expected "
            f"{OFFLINE_ROOT_RING_FORMAT!r}; a release ring is not a root ring")
    if not isinstance(document["epoch"], int) or isinstance(document["epoch"], bool) \
            or document["epoch"] < 0:
        raise TrustError("offline-root ring epoch must be a non-negative integer")
    keys = document["keys"]
    if not isinstance(keys, list) or not keys:
        raise TrustError("offline-root ring must contain at least one key")
    if not isinstance(document["revoked_key_ids"], list):
        raise TrustError("offline-root ring revoked_key_ids must be an array")

    resolved: dict[str, str] = {}
    for index, entry in enumerate(keys):
        what = f"offline-root key {index}"
        if not isinstance(entry, dict):
            raise TrustError(f"{what} is not an object")
        _require_exact_fields(entry, ROOT_KEY_FIELDS, what)
        key_id = entry["key_id"]
        if not isinstance(key_id, str) or not key_id:
            raise TrustError(f"{what} has an empty key_id")
        if key_id in resolved:
            raise TrustError(f"offline-root key id {key_id!r} appears twice")
        for field in ("first_sequence", "last_sequence"):
            value = entry[field]
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise TrustError(f"{what} {field} must be a non-negative integer")
        if entry["first_sequence"] > entry["last_sequence"]:
            raise TrustError(f"{what} has an inverted sequence range")
        resolved[key_id] = _require_lowercase_hex_key(
            entry["public_key"], f"{what} public_key")

    for key_id in document["revoked_key_ids"]:
        if not isinstance(key_id, str) or not key_id:
            raise TrustError("every revoked_key_ids entry must be a non-empty string")

    return resolved, raw


def audit_role_separation(announcement_key: str, root_keys: dict[str, str],
                          forbidden_keys: dict[str, str]) -> None:
    """The announcement key must be its own role and nothing else's.

    Reuse is the whole risk: an announcement key that is also a root key means
    compromising the key that signs a routine, network-facing, frequently-rotated
    object also compromises the key that decides which keys exist.
    """
    announcement_key = _normalize_hex_key(announcement_key, "announcement public_key")
    root_keys = {
        key_id: _normalize_hex_key(key, f"offline-root key {key_id!r}")
        for key_id, key in root_keys.items()
    }
    forbidden_keys = {
        role: _normalize_hex_key(key, f"{role} public key")
        for role, key in forbidden_keys.items()
    }

    for key_id, key in root_keys.items():
        if key == announcement_key:
            raise TrustError(
                f"the announcement key is also offline-root key {key_id!r}; "
                "roles must not share key material")
    for role, key in forbidden_keys.items():
        if key == announcement_key:
            raise TrustError(
                f"the announcement key is also the {role} key; roles must not "
                "share key material")

    duplicates = [key_id for key_id, key in root_keys.items()
                  if list(root_keys.values()).count(key) > 1]
    if duplicates:
        raise TrustError(
            "offline-root ring reuses one public key under several ids: "
            + ", ".join(sorted(duplicates)))

    for role, key in forbidden_keys.items():
        for key_id, root_key in root_keys.items():
            if key == root_key:
                raise TrustError(
                    f"the {role} key is also offline-root key {key_id!r}; "
                    "roles must not share key material")

    supplied_roles = list(forbidden_keys.items())
    for index, (left_role, left_key) in enumerate(supplied_roles):
        for right_role, right_key in supplied_roles[index + 1:]:
            if left_key == right_key:
                raise TrustError(
                    f"the {left_role} and {right_role} roles share one public key")


def audit_urls(win64_url: str, mac_url: str, recovery_url: str) -> None:
    for label, url, expected in (
        ("win64 announcement", win64_url, WIN64_ANNOUNCEMENT_URL),
        ("mac announcement", mac_url, MAC_ANNOUNCEMENT_URL),
        ("root key ring recovery", recovery_url, ROOT_KEY_RING_RECOVERY_URL),
    ):
        if not isinstance(url, str) or not url:
            raise TrustError(f"the {label} URL is empty")
        if not url.startswith("https://"):
            raise TrustError(f"the {label} URL is not HTTPS: {url!r}")
        if url != expected:
            # Exact comparison, not a prefix or suffix test: a URL that merely
            # resembles the constant is somebody else's URL.
            raise TrustError(
                f"the {label} URL must be exactly {expected!r}, got {url!r}")
    if len({win64_url, mac_url, recovery_url}) != 3:
        raise TrustError("the three fixed URLs must be distinct")


def build_trust_document(*, announcement_key: str, announcement_raw: bytes,
                         root_ring_raw: bytes, win64_url: str, mac_url: str,
                         recovery_url: str) -> dict:
    return {
        "announcement_ceremony_sha256": hashlib.sha256(announcement_raw).hexdigest(),
        "announcement_public_key": announcement_key,
        "format": TRUST_FORMAT,
        "mac_announcement_url": mac_url,
        "offline_root_key_ring_sha256": hashlib.sha256(root_ring_raw).hexdigest(),
        "offline_root_key_ring_size": len(root_ring_raw),
        "root_key_ring_recovery_url": recovery_url,
        "win64_announcement_url": win64_url,
    }


def _byte_array_lines(raw: bytes, indent: str) -> str:
    lines = []
    for offset in range(0, len(raw), 12):
        chunk = raw[offset:offset + 12]
        lines.append(indent + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    return "\n".join(lines)


def build_header(trust: dict, root_ring_raw: bytes) -> str:
    """Emit the generated header.

    The announcement URL is a closed compile branch rather than a pair of named
    constants: a Shipping link must contain its own platform's literal and no way
    to address the other one. An unsupported platform is a hard #error, not a
    silent empty string.
    """
    return f'''// GENERATED FILE — DO NOT EDIT.
//
// Produced by Tools/dist/generate_bootstrap_trust.py from:
//   Build/WoTRelease/announcement-public-key-v1.json
//   Build/WoTRelease/public-root-ring.json
// Regenerate with the command recorded in Build/WoTRelease/KEY-CUSTODY.md and
// verify with --check; drift between these bytes and those inputs is a trust root
// that nobody reviewed.
//
// This header is the ONLY place a shipped binary learns what to trust. It embeds
// the offline-root ring's exact canonical bytes so a root-signed release ring can
// be verified with no file on disk and no network fetch.
#pragma once

#include "CoreMinimal.h"

namespace WoT::Bootstrap::Trust
{{
/** Dedicated announcement key. Distinct from every other role by audit. */
inline constexpr TCHAR AnnouncementPublicKeyHex[] = TEXT("{trust['announcement_public_key']}");

/** SHA-256 of the canonical announcement ceremony export these bytes came from. */
inline constexpr TCHAR AnnouncementCeremonySha256Hex[] = TEXT("{trust['announcement_ceremony_sha256']}");

/** SHA-256 and size of the embedded offline-root ring below. */
inline constexpr TCHAR OfflineRootKeyRingSha256Hex[] = TEXT("{trust['offline_root_key_ring_sha256']}");
inline constexpr int32 OfflineRootKeyRingSize = {trust['offline_root_key_ring_size']};

/**
 * The offline-root ring's exact canonical bytes.
 *
 * Embedded rather than referenced: a runtime path to a trust file would be a
 * runtime way to REPLACE the trust file.
 */
inline constexpr uint8 OfflineRootKeyRingCanonicalBytes[] = {{
{_byte_array_lines(root_ring_raw, chr(9))}
}};

static_assert(sizeof(OfflineRootKeyRingCanonicalBytes) == OfflineRootKeyRingSize,
	"the embedded root ring size must match the recorded size");

/**
 * The fixed offline-root recovery pointer.
 *
 * Invoked ONLY by the explicit recovery-key operation. It is not an announcement
 * source and can never trigger, retry, mirror, extend, or stand in for either
 * announcement fetch.
 */
inline constexpr TCHAR RootKeyRingRecoveryUrl[] = TEXT("{trust['root_key_ring_recovery_url']}");

/**
 * The one announcement URL this binary can address.
 *
 * A closed branch on purpose: there is deliberately no simultaneously addressable
 * Win64/Mac pair, so no code path can be talked into fetching the other platform's
 * announcement.
 */
#if PLATFORM_WINDOWS
inline constexpr TCHAR AnnouncementUrl[] = TEXT("{trust['win64_announcement_url']}");
#elif PLATFORM_MAC
inline constexpr TCHAR AnnouncementUrl[] = TEXT("{trust['mac_announcement_url']}");
#else
#error "WoT bootstrap trust is defined for Win64 and Mac only"
#endif
}}
'''


def audit_generated_text(json_bytes: bytes, header_text: str) -> None:
    """No generated production artifact may carry a test or private-key marker."""
    for what, text in (("bootstrap-trust JSON", json_bytes.decode("utf-8")),
                       ("generated header", header_text)):
        lowered = text.lower()
        for marker in PRIVATE_KEY_MARKERS:
            # "private" appears legitimately in no generated artifact.
            if marker in lowered:
                raise TrustError(f"{what} contains the marker {marker!r}")
        for marker in TEST_MARKERS:
            if marker.lower() in lowered:
                raise TrustError(f"{what} contains the test marker {marker!r}")

    # Exactly one AnnouncementUrl definition per platform branch, and both branches
    # present, so neither platform silently falls through to the #error.
    if header_text.count("inline constexpr TCHAR AnnouncementUrl[]") != 2:
        raise TrustError("the header must define AnnouncementUrl once per platform branch")
    for guard in ("#if PLATFORM_WINDOWS", "#elif PLATFORM_MAC", "#else", "#error"):
        if guard not in header_text:
            raise TrustError(f"the header is missing the {guard!r} guard")
    if "Win64AnnouncementUrl" in header_text or "MacAnnouncementUrl" in header_text:
        raise TrustError(
            "the header must not expose a simultaneously addressable platform pair")


def _write_or_check(path: Path, payload: bytes, check: bool,
                    problems: list[str]) -> None:
    if check:
        try:
            actual = path.read_bytes()
        except OSError as exc:
            problems.append(f"{path} cannot be read: {exc}")
            return
        if actual != payload:
            problems.append(
                f"{path} has drifted from its inputs "
                f"({len(actual)} bytes on disk, {len(payload)} bytes expected)")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def generate(*, announcement_path: Path, root_ring_path: Path, win64_url: str,
             mac_url: str, recovery_url: str, json_output: Path,
             header_output: Path, forbidden_keys: dict[str, str] | None = None,
             check: bool = False) -> dict:
    announcement_key, announcement_raw = load_announcement_export(announcement_path)
    root_keys, root_ring_raw = load_root_ring(root_ring_path)
    audit_role_separation(announcement_key, root_keys, forbidden_keys or {})
    audit_urls(win64_url, mac_url, recovery_url)

    trust = build_trust_document(
        announcement_key=announcement_key, announcement_raw=announcement_raw,
        root_ring_raw=root_ring_raw, win64_url=win64_url, mac_url=mac_url,
        recovery_url=recovery_url)
    json_bytes = canonical_bytes(trust)
    header_text = build_header(trust, root_ring_raw)
    audit_generated_text(json_bytes, header_text)

    problems: list[str] = []
    _write_or_check(json_output, json_bytes, check, problems)
    _write_or_check(header_output, header_text.encode("utf-8"), check, problems)
    if problems:
        raise TrustError("; ".join(problems))
    return trust


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Audit bootstrap key roles and emit the compiled-in trust.")
    parser.add_argument("--announcement-public-key", required=True, type=Path)
    parser.add_argument("--root-ring", required=True, type=Path)
    parser.add_argument("--win64-announcement-url", required=True)
    parser.add_argument("--mac-announcement-url", required=True)
    parser.add_argument("--root-key-ring-recovery-url", required=True)
    parser.add_argument("--json-output", required=True, type=Path)
    parser.add_argument("--header-output", required=True, type=Path)
    parser.add_argument(
        "--forbidden-key", action="append", default=[], metavar="ROLE=HEX",
        help="a public key from another role that the announcement key must "
             "differ from; repeatable")
    parser.add_argument("--check", action="store_true",
                        help="verify the outputs match their inputs instead of "
                             "writing them")
    args = parser.parse_args(argv)

    forbidden: dict[str, str] = {}
    for item in args.forbidden_key:
        role, separator, key = item.partition("=")
        if not separator or not role or not key:
            parser.error(f"--forbidden-key needs ROLE=HEX, got {item!r}")
        if role in forbidden:
            parser.error(f"--forbidden-key role {role!r} appears more than once")
        try:
            forbidden[role] = _normalize_hex_key(key, f"{role} public key")
        except TrustError as exc:
            parser.error(str(exc))

    try:
        trust = generate(
            announcement_path=args.announcement_public_key,
            root_ring_path=args.root_ring,
            win64_url=args.win64_announcement_url,
            mac_url=args.mac_announcement_url,
            recovery_url=args.root_key_ring_recovery_url,
            json_output=args.json_output, header_output=args.header_output,
            forbidden_keys=forbidden, check=args.check)
    except TrustError as exc:
        print(f"TRUST AUDIT FAILED: {exc}", file=sys.stderr)
        return 1

    verb = "verified" if args.check else "wrote"
    print(f"{verb} {args.json_output} and {args.header_output} "
          f"(root ring {trust['offline_root_key_ring_size']} bytes, "
          f"sha256 {trust['offline_root_key_ring_sha256'][:16]}...)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
