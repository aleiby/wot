#!/usr/bin/env python3
"""Specs for the release formats, cross-checked against the native vectors.

The cross-check suite reads signed vectors straight out of the C++ automation
specs (cpp_vectors) and requires the Python verifier + parsers to reach the same
verdict the native specs assert. Passing it proves the Python framing, canonical
JSON, and closed schemas are byte-compatible with what the game accepts:

  * CryptoTests.cpp            RFC 8032, synthetic ring/manifest/wrong-domain, and a
                               ring signed by the PRODUCTION offline-root-a
  * OrderingTests.cpp          a complete announcement -> ring -> manifest chain
  * AnnouncementPolicyTests.cpp 8 announcement vectors (valid + 3 malformed x 2)
  * ContractTests.cpp          a cache contract whose signature covers the
                               canonical RE-serialization of its payload
  * cache-contract-v3-production.json, verified with its checked public key
"""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
import json
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path, PurePosixPath
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cpp_vectors as vectors  # noqa: E402
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import release_test_fixtures as fixtures  # noqa: E402
import run_first_run_funnel as funnel  # noqa: E402
from release_formats import FormatError  # noqa: E402

DOMAIN = "com.aleiby.wot.release.v1"


def _hex(path, name):
    return bytes.fromhex(vectors.literal(path, name))


class NativeVectorCrossCheckTests(unittest.TestCase):
    def test_rfc8032_vectors_verify(self):
        for index, message in enumerate((b"", bytes.fromhex("72"),
                                         bytes.fromhex("af82")), 1):
            key = _hex(vectors.CRYPTO_TESTS, f"RfcPublicKey{index}")
            signature = _hex(vectors.CRYPTO_TESTS, f"RfcSignature{index}")
            self.assertTrue(crypto.ed25519_verify(key, message, signature), index)
            tampered = bytearray(signature)
            tampered[0] ^= 1
            self.assertFalse(crypto.ed25519_verify(key, message, bytes(tampered)))
            self.assertFalse(crypto.ed25519_verify(key, message + b"x", signature))

    def test_framing_bytes_match_the_native_pin(self):
        # CryptoTests.cpp:196-226: "{}" framed under ReleaseManifestV2.
        framed = crypto.frame_signature_input(crypto.CONTEXT_RELEASE_MANIFEST_V2, b"{}")
        self.assertEqual(framed, b"WOT-SIGNATURE\x00" + bytes([5, 0, 2, 0, 0, 0, 0, 0,
                                                               0, 0]) + b"{}")
        self.assertEqual(len(framed), 24 + 2)
        frames = {crypto.frame_signature_input(context, b"{}")
                  for context in crypto.CONTEXT_NAMES}
        self.assertEqual(len(frames), 9)

    def test_context_ordinals_and_names_mirror_the_native_enum(self):
        # ArtifactTypes.h EWoTSignatureContext declaration order is the ordinal;
        # Ed25519.cpp LexToString is the spelling. Neither is ever renumbered.
        header = formats.ARTIFACT_TYPES_HEADER.read_text(encoding="utf-8")
        body = re.search(r"enum class EWoTSignatureContext : uint8\s*\{([^}]*)\}",
                         header).group(1)
        enumerators = re.findall(r"^\s*(\w+),", body, re.MULTILINE)
        self.assertEqual(enumerators, [crypto.CONTEXT_NAMES[ordinal]
                                       for ordinal in range(len(crypto.CONTEXT_NAMES))])
        self.assertEqual(crypto.CONTEXT_CACHE_LINEAGE_V1, 8)
        self.assertEqual(crypto.CONTEXT_NAMES[8], "CacheLineageV1")
        source = (formats.REPOSITORY_ROOT / "Source" / "WoTBootstrapCore" / "Private"
                  / "Ed25519.cpp").read_text(encoding="utf-8")
        spellings = dict(re.findall(
            r'case EWoTSignatureContext::(\w+):\s*return TEXT\("(\w+)"\);', source))
        self.assertEqual(spellings, {name: name for name in crypto.CONTEXT_NAMES.values()})

    def test_synthetic_ring_manifest_and_domain_separation(self):
        ring = vectors.literal(vectors.CRYPTO_TESTS, "SynthRingJson").encode()
        manifest = vectors.literal(vectors.CRYPTO_TESTS, "SynthManifestJson").encode()
        root = _hex(vectors.CRYPTO_TESTS, "SynthRootPublicKey")
        key_a = _hex(vectors.CRYPTO_TESTS, "SynthKeyAPublicKey")
        key_b = _hex(vectors.CRYPTO_TESTS, "SynthKeyBPublicKey")
        ring_signature = _hex(vectors.CRYPTO_TESTS, "SynthRingSignature")
        manifest_signature = _hex(vectors.CRYPTO_TESTS, "SynthManifestSignature")
        wrong_domain = _hex(vectors.CRYPTO_TESTS, "SynthWrongDomainSignature")
        self.assertTrue(crypto.verify_framed(root, crypto.CONTEXT_RELEASE_KEY_RING_V1,
                                             ring, ring_signature))
        self.assertFalse(crypto.verify_framed(key_a, crypto.CONTEXT_RELEASE_KEY_RING_V1,
                                              ring, ring_signature))
        self.assertTrue(crypto.verify_framed(key_a, crypto.CONTEXT_RELEASE_MANIFEST_V2,
                                             manifest, manifest_signature))
        self.assertFalse(crypto.verify_framed(key_b, crypto.CONTEXT_RELEASE_MANIFEST_V2,
                                              manifest, manifest_signature))
        self.assertFalse(crypto.verify_framed(key_a, crypto.CONTEXT_RELEASE_MANIFEST_V2,
                                              manifest, wrong_domain))
        self.assertTrue(crypto.verify_framed(key_a, crypto.CONTEXT_ANNOUNCEMENT_V1,
                                             manifest, wrong_domain))
        parsed = formats.parse_key_ring(formats.parse_canonical(ring, "ring"),
                                        formats.RELEASE_KEY_RING_FORMAT)
        self.assertEqual(parsed.epoch, 7)
        self.assertEqual(formats.select_release_key(parsed, "release-2026a", 42)
                         .public_key, key_a)
        for key_id, sequence in (("release-2026a", 101), ("release-2026a", 0),
                                 ("release-nope", 1), ("release-2025z", 1)):
            with self.assertRaises(FormatError):
                formats.select_release_key(parsed, key_id, sequence)
        with self.assertRaises(FormatError):
            formats.parse_key_ring(formats.parse_canonical(ring, "ring"),
                                   formats.OFFLINE_ROOT_RING_FORMAT)

    def test_ring_signed_by_the_production_offline_root(self):
        # The ring in CryptoTests.cpp was signed at the real key ceremony; the key
        # it verifies under is read from the CHECKED-IN public-root-ring.json.
        trust = formats.load_trust()
        ring = vectors.literal(vectors.CRYPTO_TESTS,
                               "ProductionSignedReleaseRingJson").encode()
        signature = _hex(vectors.CRYPTO_TESTS, "ProductionRootASignature")
        signers = [key.key_id for key in trust.root_ring.keys
                   if crypto.verify_framed(key.public_key,
                                           crypto.CONTEXT_RELEASE_KEY_RING_V1,
                                           ring, signature)]
        self.assertEqual(signers, ["offline-root-a"])

    def test_ordering_chain_announcement_ring_manifest(self):
        announcement_raw = vectors.literal(vectors.ORDERING_TESTS,
                                           "OrderAnnouncementJson").encode()
        self.assertTrue(crypto.verify_framed(
            _hex(vectors.ORDERING_TESTS, "OrderAnnouncementKeyHex"),
            crypto.CONTEXT_ANNOUNCEMENT_V1, announcement_raw,
            _hex(vectors.ORDERING_TESTS, "OrderAnnouncementSignature")))
        announcement = formats.parse_announcement(announcement_raw, "Mac", DOMAIN)
        with self.assertRaises(FormatError):
            formats.parse_announcement(announcement_raw, "Win64", DOMAIN)
        with self.assertRaises(FormatError):
            formats.parse_announcement(announcement_raw, "Mac", "com.example.other")
        ring_raw = vectors.literal(vectors.ORDERING_TESTS, "OrderRingJson").encode()
        self.assertEqual((len(ring_raw), formats.sha256_hex(ring_raw)),
                         (announcement.key_ring.size, announcement.key_ring.sha256))
        ring = formats.parse_key_ring(formats.parse_canonical(ring_raw, "ring"),
                                      formats.RELEASE_KEY_RING_FORMAT)
        self.assertEqual(ring.epoch, announcement.key_ring_epoch)
        key = formats.select_release_key(ring, announcement.key_id,
                                         announcement.release_sequence)
        manifest_raw = vectors.literal(vectors.ORDERING_TESTS, "OrderManifestJson").encode()
        self.assertEqual((len(manifest_raw), formats.sha256_hex(manifest_raw)),
                         (announcement.manifest.size, announcement.manifest.sha256))
        self.assertTrue(crypto.verify_framed(
            key.public_key, crypto.CONTEXT_RELEASE_MANIFEST_V2, manifest_raw,
            _hex(vectors.ORDERING_TESTS, "OrderManifestSignature")))
        manifest = formats.parse_manifest(manifest_raw, announcement)
        self.assertEqual(manifest.installed_tree_root.name, "WoT.app")
        self.assertTrue(formats.supports_manifest_protocols(
            manifest, formats.load_compiled_constants()))
        # The Python writer spells the native vectors exactly.
        for raw in (announcement_raw, ring_raw, manifest_raw):
            self.assertEqual(formats.canonical_bytes(json.loads(raw)), raw)

    def test_announcement_policy_vectors(self):
        key = _hex(vectors.POLICY_TESTS, "WoTAnnouncementPolicySpecPublicKeyHex")
        verdicts = {"Valid": True, "Unknown": False, "AbsentManifest": False,
                    "InvalidManifest": False}
        for name, parses in verdicts.items():
            messages = vectors.literals(vectors.POLICY_TESTS,
                                        f"WoTAnnouncementPolicySpec{name}Json")
            signatures = vectors.literals(vectors.POLICY_TESTS,
                                          f"WoTAnnouncementPolicySpec{name}SignatureHex")
            self.assertEqual(len(messages), 2)   # #if PLATFORM_WINDOWS / #elif PLATFORM_MAC
            for platform, message, signature in zip(("Win64", "Mac"), messages,
                                                    signatures):
                raw = message.encode()
                self.assertTrue(crypto.verify_framed(key, crypto.CONTEXT_ANNOUNCEMENT_V1,
                                                     raw, bytes.fromhex(signature)),
                                (name, platform))
                if parses:
                    parsed = formats.parse_announcement(raw, platform, DOMAIN)
                    self.assertEqual(parsed.release_sequence, 7)
                else:
                    with self.assertRaises(FormatError, msg=(name, platform)):
                        formats.parse_announcement(raw, platform, DOMAIN)

    def test_contract_signatures_cover_the_canonical_reserialization(self):
        key = _hex(vectors.CONTRACT_TESTS, "RetailContractPublicKeyHex")
        document = json.loads(vectors.literal(vectors.CONTRACT_TESTS,
                                              "RetailContractSignedJson"))
        payload = formats.canonical_bytes(document["payload"])
        self.assertTrue(crypto.verify_framed(key, crypto.CONTEXT_CACHE_CONTRACT_V3,
                                             payload, bytes.fromhex(document["signature"])))
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import generate_production_cache_contract as production
        document = json.loads(formats.DEFAULT_CACHE_CONTRACT.read_text(encoding="utf-8"))
        payload = formats.canonical_bytes(document["payload"])
        self.assertEqual(formats.sha256_hex(payload), production.PAYLOAD_SHA256)
        self.assertTrue(crypto.verify_framed(bytes.fromhex(production.PUBLIC_KEY_HEX),
                                             crypto.CONTEXT_CACHE_CONTRACT_V3, payload,
                                             bytes.fromhex(document["signature"])))

    def test_checked_in_trust_and_generated_header_agree(self):
        trust = formats.load_trust()   # includes the BootstrapTrust.generated.h check
        self.assertEqual(trust.recovery_url.rsplit("/", 1)[1], formats.KEY_RING_ASSET)
        for platform in formats.PLATFORMS:
            self.assertTrue(trust.announcement_urls[platform].endswith(
                "/latest/download/" + formats.announcement_asset(platform)))
        compiled = formats.load_compiled_constants()
        self.assertEqual(compiled.announcement_domain, DOMAIN)
        self.assertEqual(compiled.release_manifest_version, 2)


class CanonicalJsonTests(unittest.TestCase):
    def test_accepts_only_the_canonical_spelling(self):
        self.assertEqual(formats.parse_canonical(b'{"a":1,"b":[true,null,"\\u00e9"]}', "x"),
                         {"a": 1, "b": [True, None, "\u00e9"]})
        refused = [b'{"b":1,"a":2}', b'{ "a":1}', b'{"a":1}\n', b'{"a":1,"a":1}',
                   b'{"a":1.0}', b'{"a":1e3}', b'{"a":-0}', b'{"a":01}',
                   b'{"a":"\\u00E9"}', '{"a":"\u00e9"}'.encode("utf-8"),
                   b'{"a":9223372036854775808}', b'{"a":"\\ud800"}', b'{"a":NaN}',
                   b'{"a":"\\/"}', b'{"a":"\\u000a"}']
        for raw in refused:
            with self.assertRaises(FormatError, msg=raw):
                formats.parse_canonical(raw, "x")
        self.assertEqual(formats.parse_canonical(b'{"a":-9223372036854775808}', "x"),
                         {"a": -9223372036854775808})

    def test_writer_refuses_values_without_a_canonical_form(self):
        for value in ({"a": 0.5}, {"a": 2 ** 63}, {"a": "\ud800"}, {1: 2}):
            with self.assertRaises(FormatError):
                formats.canonical_bytes(value)


class CacheLineageFormatTests(unittest.TestCase):
    REVISION_2 = "e6e3ef1fa015fcfdef584ffa59b5badff4993d1f3fab2fb35e7743ed522e968e"

    def values(self, **overrides):
        values = {
            "platform": "Win64", "release_sequence": 1, "revision": 2,
            "label": self.REVISION_2, "installer_sha256": "a" * 64,
            "contract_sha256": "b" * 64, "manifest_sha256": "c" * 64,
        }
        values.update(overrides)
        return values

    def test_asset_name_and_round_trip(self):
        self.assertEqual(formats.cache_lineage_asset("Mac"),
                         "WoT-Mac-cache-lineage-v1.json")
        self.assertEqual(formats.validate_asset_name(
            formats.cache_lineage_asset("Win64")), "WoT-Win64-cache-lineage-v1.json")
        document = formats.build_cache_lineage(**self.values())
        self.assertEqual(set(document), {
            "format", "platform", "release_sequence", "revision", "label",
            "installer_sha256", "contract_sha256", "manifest_sha256"})
        self.assertEqual(document["format"], "wot-cache-lineage-v1")
        raw = formats.canonical_bytes(document)
        self.assertEqual(formats.parse_cache_lineage(raw, "Win64"), document)
        revision_one = formats.build_cache_lineage(**self.values(
            revision=1, release_sequence=0,
            label="1a7e19b367523c11d9ad6c7fda42a39472a68d4b8c52233b6b2d568ca3daa58e"))
        self.assertEqual(revision_one["revision"], 1)

    def test_builder_refuses_what_the_reader_refuses(self):
        cases = {
            "label of another revision": {"revision": 3},
            "revision zero": {"revision": 0},
            "boolean revision": {"revision": True},
            "negative sequence": {"release_sequence": -1},
            "sequence beyond int64": {"release_sequence": 2 ** 63},
            "upper-case digest": {"manifest_sha256": "C" * 64},
            "short digest": {"installer_sha256": "a" * 63},
            "unknown platform": {"platform": "Linux"},
        }
        for label, overrides in cases.items():
            with self.subTest(label=label), self.assertRaises(FormatError):
                formats.build_cache_lineage(**self.values(**overrides))

    def test_reader_is_strict(self):
        document = formats.build_cache_lineage(**self.values())
        raw = formats.canonical_bytes(document)
        refused = {
            "not canonical": json.dumps(document, indent=1).encode(),
            "other platform": raw,
            "extra member": formats.canonical_bytes({**document, "human_version": "1"}),
            "missing member": formats.canonical_bytes(
                {key: value for key, value in document.items() if key != "label"}),
            "wrong format": formats.canonical_bytes(
                {**document, "format": "wot-cache-lineage-v2"}),
            "mismatched label": formats.canonical_bytes(
                {**document, "label": "d" * 64}),
        }
        for label, candidate in refused.items():
            platform = "Mac" if label == "other platform" else "Win64"
            with self.subTest(label=label), self.assertRaises(FormatError):
                formats.parse_cache_lineage(candidate, platform)


class PlayerDownloadAssetTests(unittest.TestCase):
    def test_shipping_sources_use_fixed_valid_asset_names(self):
        class Result:
            configuration = formats.SHIPPING_CONFIGURATION

        win64 = Result()
        win64.install_zip = "WoT-Win64-guarded-shipping-abc1234.zip"
        win64.dmg = None
        mac = Result()
        mac.install_zip = "WoT-Mac-guarded-shipping-abc1234.zip"
        mac.dmg = "WoT-Mac-guarded-shipping-abc1234.dmg"

        self.assertEqual(formats.player_download_asset_sources({
            "Win64": win64,
            "Mac": mac,
        }), {
            "WoT-Reborn-Windows.zip": win64.install_zip,
            "WoT-Reborn-Mac.dmg": mac.dmg,
        })
        for name in formats.PLAYER_DOWNLOAD_ASSETS.values():
            self.assertEqual(formats.validate_asset_name(name), name)

    def test_test_configuration_has_no_fixed_downloads(self):
        result = type("Result", (), {
            "configuration": formats.TEST_CONFIGURATION,
            "install_zip": "WoT-Win64-guarded-abc1234.zip",
            "dmg": None,
        })()
        self.assertEqual(
            formats.player_download_asset_sources({"Win64": result}), {})


class ReleaseNotesFormatTests(unittest.TestCase):
    def test_normalizes_and_validates_closed_canonical_asset(self):
        self.assertEqual(
            formats.normalize_release_notes_text(
                "first\u00a0\r\n\r\nsecond\u001c\r\n\r\n"),
            "first\n\nsecond")
        raw = formats.merge_release_notes(None, 2, "first  \r\nsecond\t\r\n")
        self.assertEqual(formats.parse_release_notes(raw), {
            "format": formats.RELEASE_NOTES_FORMAT,
            "notes": [{"release_sequence": 2, "text": "first\nsecond"}],
        })
        for malformed in (
                b'{"extra":0,"format":"wot-release-notes-v1","notes":[]}',
                b'{"format":"wrong","notes":[]}',
                b'{"format":"wot-release-notes-v1","notes":[{"release_sequence":0,"text":"x"}]}',
                b'{"format":"wot-release-notes-v1","notes":[{"release_sequence":2,"text":"x"},{"release_sequence":1,"text":"y"}]}',
                b'{"format":"wot-release-notes-v1","notes":[{"release_sequence":1,"text":"x "}]}',
                b'{"format":"wot-release-notes-v1","notes":[{"release_sequence":1,"text":"x\\u0000tail"}]}',
                b'{"format":"wot-release-notes-v1","notes":[{"release_sequence":1,"text":"x\\f\\ny"}]}',
        ):
            with self.subTest(malformed=malformed), self.assertRaises(
                    formats.FormatError):
                formats.parse_release_notes(malformed)

    def test_merge_replaces_sequence_zero_skips_and_oldest_entries_trim(self):
        raw = None
        for sequence in range(1, formats.RELEASE_NOTES_MAX_ENTRIES + 2):
            raw = formats.merge_release_notes(raw, sequence,
                                              f"notes {sequence}")
        document = formats.parse_release_notes(raw)
        self.assertEqual(len(document["notes"]), formats.RELEASE_NOTES_MAX_ENTRIES)
        self.assertEqual(document["notes"][0]["release_sequence"], 2)
        replaced = formats.merge_release_notes(raw, 10, "replacement")
        entry = next(row for row in formats.parse_release_notes(replaced)["notes"]
                     if row["release_sequence"] == 10)
        self.assertEqual(entry["text"], "replacement")
        self.assertEqual(formats.merge_release_notes(replaced, 0, "rc body"), replaced)

        large = None
        for sequence in range(1, 65):
            large = formats.merge_release_notes(large, sequence, "x" * 5000)
        parsed = formats.parse_release_notes(large)
        self.assertLessEqual(len(large), formats.RELEASE_NOTES_MAX_BYTES)
        self.assertEqual(parsed["notes"][-1]["release_sequence"], 64)
        self.assertGreater(parsed["notes"][0]["release_sequence"], 1)

    def test_cpp_and_python_latest_urls_are_identical(self):
        header = (formats.REPOSITORY_ROOT / "Source" / "WoT" / "UI" /
                  "WoTReleaseNotes.h").read_text(encoding="utf-8")
        match = re.search(r'LatestUrl\[\].*?TEXT\("([^"]+)"\)', header,
                          re.DOTALL)
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), formats.release_notes_latest_url())


class ReleaseIdentityTests(unittest.TestCase):
    def test_checked_in_development_identity_is_canonical_and_unstamped(self):
        path = formats.RELEASE_DIR / "release-identity.json"
        raw = path.read_bytes()
        identity = formats.parse_release_identity(raw, str(path))
        self.assertEqual((identity.release_sequence, identity.release_commit),
                         (0, ""))
        self.assertEqual(raw, b'{"format":"wot-release-identity-v1",'
                         b'"release_commit":"","release_sequence":0,"version":1}')

    def test_identity_schema_is_closed_and_sequence_is_non_negative_int64(self):
        valid = {"format": "wot-release-identity-v1", "release_commit": "abc123456",
                 "release_sequence": 5, "version": 1}
        parsed = formats.parse_release_identity(formats.canonical_bytes(valid), "fixture")
        self.assertEqual((parsed.release_sequence, parsed.release_commit),
                         (5, "abc123456"))
        cases = (
            {**valid, "extra": True},
            {**valid, "format": "wrong"},
            {**valid, "version": 2},
            {**valid, "release_sequence": -1},
            {**valid, "release_sequence": formats.INT64_MAX + 1},
            {**valid, "release_sequence": 1.5},
        )
        for document in cases:
            with self.subTest(document=document), self.assertRaises(FormatError):
                formats.parse_release_identity(
                    json.dumps(document, sort_keys=True, separators=(",", ":"))
                    .encode("ascii"), "fixture")


class UrlPolicyTests(unittest.TestCase):
    def test_initial_release_url_policy(self):
        allowed = ["https://github.com/aleiby/wot/releases/download/v1/a.json",
                   "HTTPS://GitHub.com/aleiby/wot/releases/download/v1/a.json"]
        refused = ["http://github.com/a", "https://api.github.com/a",
                   "https://github.com:443/a", "https://u@github.com/a",
                   "https://github.com/a#frag", "https://github.com/a b",
                   "https://github.com\\a", "https://xn--github.com/a",
                   "https://127.0.0.1/a", "https://github.com/\u00e9", "github.com/a",
                   "https://github.com./a", "https://-github.com/a"]
        for url in allowed:
            self.assertTrue(formats.is_allowed_initial_release_url(url), url)
        for url in refused:
            self.assertFalse(formats.is_allowed_initial_release_url(url), url)

    def test_signed_urls_are_per_tag(self):
        self.assertEqual(formats.release_download_url("v0.1.0", "x.json"),
                         "https://github.com/aleiby/wot/releases/download/v0.1.0/x.json")
        for tag in ("", "../x", "a/b", "v 1", "-x", "v..1"):
            with self.assertRaises(FormatError, msg=tag):
                formats.release_download_url(tag, "x.json")


def _tree():
    content, game, updater = b"closed payload\n", b"game bytes\n", b"updater bytes\n"
    entries = [
        formats.TreeEntry("Content.bin", "file", 0o644, len(content),
                          hashlib.sha256(content).hexdigest()),
        formats.TreeEntry("Contents", "directory", 0o755, 0, ""),
        formats.TreeEntry("Contents/MacOS", "directory", 0o755, 0, ""),
        formats.TreeEntry("Contents/MacOS/WoT", "file", 0o644, len(game),
                          hashlib.sha256(game).hexdigest()),
        formats.TreeEntry("Contents/MacOS/WoTUpdater", "file", 0o644, len(updater),
                          hashlib.sha256(updater).hexdigest()),
    ]
    return entries, {"Content.bin": content, "Contents/MacOS/WoT": game,
                     "Contents/MacOS/WoTUpdater": updater}


class InstalledTreeTests(unittest.TestCase):
    def test_inventory_bytes_match_the_native_fixture_template(self):
        # ReleaseStagingTests.cpp:262-289 hand-writes this inventory; the native
        # ParseInstalledTreeV1 accepts it, so the Python writer must match it.
        entries, data = _tree()
        digest = {path: hashlib.sha256(value).hexdigest() for path, value in data.items()}
        expected = (
            '{"entries":['
            f'{{"link_target":"","mode":420,"path":"Content.bin","sha256":"{digest["Content.bin"]}","size":15,"type":"file"}},'
            '{"link_target":"","mode":493,"path":"Contents","sha256":"","size":0,"type":"directory"},'
            '{"link_target":"","mode":493,"path":"Contents/MacOS","sha256":"","size":0,"type":"directory"},'
            f'{{"link_target":"","mode":420,"path":"Contents/MacOS/WoT","sha256":"{digest["Contents/MacOS/WoT"]}","size":11,"type":"file"}},'
            f'{{"link_target":"","mode":420,"path":"Contents/MacOS/WoTUpdater","sha256":"{digest["Contents/MacOS/WoTUpdater"]}","size":14,"type":"file"}}'
            '],"format":"wot-installed-tree-v1","platform":"Mac","version":1}').encode()
        self.assertEqual(formats.build_installed_tree("Mac", list(reversed(entries))),
                         expected)
        platform, parsed = formats.parse_installed_tree(expected)
        self.assertEqual((platform, parsed), ("Mac", entries))

    def test_root_digest_is_the_length_prefixed_projection(self):
        entries, _ = _tree()
        stream = io.BytesIO()

        def text(value):
            stream.write(struct.pack("<Q", len(value.encode())) + value.encode())

        text("wot-installed-tree-v1")
        stream.write(struct.pack("<Q", 1))
        text("Mac")
        stream.write(struct.pack("<Q", len(entries)))
        for entry in entries:
            text(entry.path)
            text(entry.type)
            stream.write(struct.pack("<QQ", entry.mode, entry.size))
            stream.write(bytes.fromhex(entry.sha256) if entry.sha256 else b"\0" * 32)
            text(entry.link_target)
        self.assertEqual(formats.compute_installed_tree_root("Mac", entries),
                         hashlib.sha256(stream.getvalue()).hexdigest())
        with self.assertRaises(FormatError):
            formats.compute_installed_tree_root("Mac", list(reversed(entries)))

    def test_inventory_refusals(self):
        def raw(entries):
            return json.dumps({"entries": entries, "format": "wot-installed-tree-v1",
                               "platform": "Win64", "version": 1}, sort_keys=True,
                              separators=(",", ":")).encode()

        def entry(path, **fields):
            base = {"link_target": "", "mode": 420, "path": path, "sha256": "a" * 64,
                    "size": 1, "type": "file"}
            base.update(fields)
            return base

        for entries in ([entry("b"), entry("a")], [entry("A"), entry("a")],
                        [entry("installed-tree-v1.json")], [entry("a", mode=0o4755)],
                        [entry("a", type="directory")], [entry("a\\b")],
                        [entry("a.")], [entry("../a")],
                        [entry("Contents/._Info.plist")],
                        [entry("Contents/__MACOSX/file")],
                        [entry("a", sha256="A" * 64)]):
            with self.assertRaises(FormatError, msg=entries):
                formats.parse_installed_tree(raw(entries))


def _native_style_zip(members):
    """ReleaseStagingTests.cpp:118-201: made-by 0x033f, UTF-8 flag, stored."""
    archive = bytearray()
    central = bytearray()
    for name, data, kind, mode in members:
        stored = name + ("/" if kind == "directory" else "")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        external = ((0o040000 if kind == "directory" else
                     0o120000 if kind == "symlink" else 0o100000) | mode) << 16
        offset = len(archive)
        archive += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0x0800, 0, 0, 0, crc,
                               len(data), len(data), len(stored), 0) + stored.encode() + data
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 0x033F, 20, 0x0800, 0,
                               0, 0, crc, len(data), len(data), len(stored), 0, 0, 0, 0,
                               external, offset) + stored.encode()
    offset = len(archive)
    archive += central + struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(members),
                                     len(members), len(central), offset, 0)
    return bytes(archive)


class StagingArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        entries, self.data = _tree()
        self.inventory = formats.build_installed_tree("Mac", entries)
        self.root_digest = formats.compute_installed_tree_root("Mac", entries)

    def tearDown(self):
        self.temporary.cleanup()

    def members(self, root="WoT.app", mode=0o644):
        return [(root, b"", "directory", 0o755),
                (f"{root}/Content.bin", self.data["Content.bin"], "file", mode),
                (f"{root}/Contents", b"", "directory", 0o755),
                (f"{root}/Contents/MacOS", b"", "directory", 0o755),
                (f"{root}/Contents/MacOS/WoT", self.data["Contents/MacOS/WoT"], "file", mode),
                (f"{root}/Contents/MacOS/WoTUpdater",
                 self.data["Contents/MacOS/WoTUpdater"], "file", mode),
                (f"{root}/installed-tree-v1.json", self.inventory, "file", 0o644)]

    def write(self, members, name="a.zip"):
        path = self.directory / name
        path.write_bytes(_native_style_zip(members))
        return path

    def test_native_fixture_layout_stages(self):
        staged = formats.stage_archive(self.write(self.members()), "Mac")
        self.assertEqual(staged.root, "WoT.app")
        self.assertEqual(staged.tree_root, self.root_digest)

    def test_refusals_mirror_staging(self):
        good = self.members()
        cases = {
            "no parent": [m for m in good if m[0] != "WoT.app/Contents/MacOS"],
            "symlink": good + [("WoT.app/link", b"Content.bin", "symlink", 0o755)],
            "unsigned extra": good + [("WoT.app/extra.bin", b"x", "file", 0o644)],
            "missing inventory": good[:-1],
            "second root": good + [("Other", b"", "directory", 0o755)],
            "unapproved char": good + [("WoT.app/a,b", b"x", "file", 0o644)],
            "reserved device": good + [("WoT.app/con.txt", b"x", "file", 0o644)],
            "top-level AppleDouble": good + [("__MACOSX", b"", "directory", 0o755)],
            "nested AppleDouble": good + [
                ("WoT.app/Contents/._Info.plist", b"x", "file", 0o644)],
            "wrong mode": [(n, d, k, 0o755 if n.endswith("Content.bin") else m)
                           for n, d, k, m in good],
        }
        for label, members in cases.items():
            with self.assertRaises(FormatError, msg=label):
                formats.stage_archive(self.write(members, label.replace(" ", "_") + ".zip"),
                                      "Mac")
        with self.assertRaises(FormatError):   # Mac layout needs an .app root
            formats.stage_archive(self.write(self.members("WoT-Mac"), "r.zip"), "Mac")

    def test_container_features_staging_refuses(self):
        raw = bytearray(_native_style_zip(self.members()))
        descriptor = bytearray(raw)
        central = descriptor.find(struct.pack("<I", 0x02014B50))
        struct.pack_into("<H", descriptor, 6, 0x0808)            # local flags
        struct.pack_into("<H", descriptor, central + 8, 0x0808)   # central flags
        path = self.directory / "descriptor.zip"
        path.write_bytes(bytes(descriptor))
        with self.assertRaises(FormatError):
            formats.stage_archive(path, "Mac")
        path.write_bytes(bytes(raw) + b"trailing")
        with self.assertRaises(FormatError):
            formats.stage_archive(path, "Mac")

    def test_win64_modes_must_have_a_windows_projection(self):
        entries = [formats.TreeEntry(e.path, e.type, e.mode, e.size, e.sha256)
                   for e in _tree()[0]]
        inventory = formats.build_installed_tree("Win64", entries)
        members = [(n.replace("WoT.app", "WoT-Win64"), d, k, m)
                   for n, d, k, m in self.members()[:-1]]
        members.append(("WoT-Win64/installed-tree-v1.json", inventory,
                        "file", 0o644))
        staged = formats.stage_archive(self.write(members, "w.zip"), "Win64")
        self.assertEqual(staged.root, "WoT-Win64")
        widened = [(n, d, k, 0o666 if k == "file" else m) for n, d, k, m in members]
        with self.assertRaises(FormatError):
            formats.stage_archive(self.write(widened, "w2.zip"), "Win64")


class RepackTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_win64_repack_is_stageable_deterministic_and_raw_copied(self):
        source = self.directory / "guarded.zip"
        fixtures.write_guarded_zip(source, "Win64")
        first = formats.repack_update_archive(source, self.directory / "a.zip", "Win64")
        formats.repack_update_archive(source, self.directory / "b.zip", "Win64")
        self.assertEqual((self.directory / "a.zip").read_bytes(),
                         (self.directory / "b.zip").read_bytes())
        staged = formats.stage_archive(self.directory / "a.zip", "Win64")
        self.assertEqual((staged.root, staged.tree_root),
                         ("Game", first.tree_root))
        self.assertEqual({(e.type, e.mode) for e in staged.entries},
                         {("file", 0o644), ("directory", 0o755)})
        self.assertIn("WoT/Binaries/Win64", {e.path for e in staged.entries})
        with zipfile.ZipFile(source) as original, \
                zipfile.ZipFile(self.directory / "a.zip") as repacked:
            for info in original.infolist():
                if info.filename == "WoT Reborn/WoT Reborn.exe":
                    continue
                relative = info.filename[len("WoT Reborn/Game/"):]
                copy = repacked.getinfo("Game/" + relative)
                self.assertEqual((copy.CRC, copy.compress_size, copy.compress_type),
                                 (info.CRC, info.compress_size, info.compress_type))
                self.assertEqual(repacked.read(copy), original.read(info))

    def test_mac_repack_refuses_appledouble(self):
        source = self.directory / "guarded.zip"
        fixtures.write_guarded_zip(source, "Mac",
                                   extra={"Contents/._Info.plist": (b"\0\5", 0o100644)})
        with self.assertRaises(FormatError):
            formats.repack_update_archive(source, self.directory / "m.zip", "Mac")

    def test_mac_repack_keeps_modes(self):
        source = self.directory / "guarded.zip"
        fixtures.write_guarded_zip(source, "Mac")
        result = formats.repack_update_archive(source, self.directory / "m.zip", "Mac")
        self.assertEqual(result.dropped_metadata, [])
        staged = formats.stage_archive(self.directory / "m.zip", "Mac")
        modes = {e.path: e.mode for e in staged.entries}
        self.assertEqual(modes["Contents/MacOS/WoT-Mac-Shipping"], 0o755)
        self.assertEqual(modes["Contents/Info.plist"], 0o644)

    def test_repack_refuses_what_staging_cannot_materialize(self):
        cases = {
            "symlink": {"Contents/Frameworks/Current": (b"A", 0o120755)},
            "setuid": {"Contents/MacOS/helper": (b"x", 0o104755)},
            "comma": {"Contents/a,b": (b"x", 0o100644)},
            "sidecar": {"installed-tree-v1.json": (b"{}", 0o100644)},
        }
        for label, extra in cases.items():
            source = self.directory / f"{label}.zip"
            fixtures.write_guarded_zip(source, "Mac", extra=extra)
            with self.assertRaises(FormatError, msg=label):
                formats.repack_update_archive(source, self.directory / f"{label}-out.zip",
                                              "Mac")


class GuardedReleaseConfigurationTests(unittest.TestCase):
    """load_guarded_release: the Test/Shipping identity sign/verify rely on."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def rewrite(self, release_json: Path, mutate) -> Path:
        document = json.loads(release_json.read_text(encoding="utf-8"))
        mutate(document)
        release_json.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                                encoding="utf-8")
        return release_json

    def test_shipping_release_carries_its_test_twin(self):
        path = fixtures.write_guarded_release(self.directory / "ship", "Win64")
        release = formats.load_guarded_release(path, "Win64")
        self.assertEqual(release.configuration, "Shipping")
        self.assertEqual(release.main_relative,
                         "WoT/Binaries/Win64/WoT-Win64-Shipping.exe")
        self.assertEqual(release.zip_path.name,
                         f"WoT-Win64-guarded-shipping-{fixtures.COMMIT}.zip")
        self.assertEqual(set(release.test_twin), set(formats.TEST_TWIN_FIELDS))
        test = formats.load_guarded_release(fixtures.write_guarded_release(
            self.directory / "test", "Mac", configuration="Test"), "Mac")
        self.assertEqual((test.configuration, test.test_twin), ("Test", None))
        self.assertEqual(test.main_relative, "Contents/MacOS/WoT-Mac-Test")

    def test_mac_release_binds_dmg_and_canonical_sidecar_bytes(self):
        path = fixtures.write_guarded_release(self.directory / "mac-dmg", "Mac")
        release = formats.load_guarded_release(path, "Mac")
        sidecar = json.loads(release.dmg_sidecar_path.read_text(encoding="utf-8"))
        self.assertEqual(sidecar["format"], "wot-mac-dmg-v2")
        self.assertRegex(sidecar["background_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(release.dmg_path.name,
                         formats.guarded_install_dmg_asset(
                             fixtures.COMMIT, formats.SHIPPING_CONFIGURATION))
        self.assertEqual(release.dmg["installed_tree_root"],
                         formats.project_guarded_archive_tree(
                             release.zip_path, "Mac").tree_root)
        self.assertEqual(release.dmg["background_sha256"],
                         sidecar["background_sha256"])
        release.dmg_path.write_bytes(release.dmg_path.read_bytes() + b"tamper")
        with self.assertRaisesRegex(FormatError, "DMG binding"):
            formats.load_guarded_release(path, "Mac")

    def test_refuses_inconsistent_configuration_identity(self):
        cases = {
            "not a guarded configuration": lambda d: d.update(configuration="Development"),
            "product receipt configuration differs": lambda d: d["products"]["receipt"]
            .update(configuration="Test"),
            "no exact test_twin": lambda d: d.pop("test_twin"),
            "same-commit Test release": lambda d: d["test_twin"].update(git_commit="1234567"),
            "test_twin release_sequence": lambda d: d["test_twin"].update(
                release_sequence=1),
            "is not a lower-case sha256": lambda d: d["test_twin"].update(
                release_json_sha256="A" * 64),
        }
        for fragment, mutate in cases.items():
            with self.subTest(fragment=fragment):
                path = self.rewrite(fixtures.write_guarded_release(
                    self.directory / f"case-{len(fragment)}", "Win64"), mutate)
                with self.assertRaises(FormatError) as caught:
                    formats.load_guarded_release(path, "Win64")
                self.assertIn(fragment, str(caught.exception))

    def test_refuses_a_test_image_or_test_zip_name_claiming_shipping(self):
        test_image = fixtures.write_guarded_release(self.directory / "image", "Win64")
        self.rewrite(test_image, lambda d: d["products"]["receipt"]["products"]["WoT"]
                     .update(path="Windows/WoT/Binaries/Win64/WoT-Win64-Test.exe"))
        with self.assertRaises(FormatError) as caught:
            formats.load_guarded_release(test_image, "Win64")
        self.assertIn("is not the Shipping game image", str(caught.exception))
        renamed = fixtures.write_guarded_release(self.directory / "renamed", "Mac")
        document = json.loads(renamed.read_text(encoding="utf-8"))
        zip_path = renamed.parent / document["asset"]
        test_name = f"WoT-Mac-guarded-{fixtures.COMMIT}.zip"
        zip_path.rename(renamed.parent / test_name)
        self.rewrite(renamed, lambda d: d.update(asset=test_name))
        with self.assertRaises(FormatError) as caught:
            formats.load_guarded_release(renamed, "Mac")
        self.assertIn("is not the Shipping guarded zip name", str(caught.exception))

    def test_seam_markers_are_found_in_every_tchar_encoding(self):
        for encoding in ("ascii", "utf-16-le", "utf-32-le"):
            data = b"\0\1" + "wottestinstallroot=".encode(encoding) + b"\2"
            self.assertEqual(formats.find_test_seam_markers(data),
                             ["wottestinstallroot="])
        self.assertEqual(formats.find_test_seam_markers(b"WoTNetTestSubsystem"), [])
        retained_markers = {
            "WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL",
            "WOT_BOOTSTRAP_TEST_RELEASE_BASE_URL",
            "ed54a59fb1ac3a51239351362941b868e85a60e3d7b2485d828821dc7a69c279",
            "ba3c735cdbe4235e6cadf81c412b883a3472d1045faea189f584f8beca46e343",
        }
        self.assertTrue(retained_markers.issubset(formats.TEST_SEAM_MARKERS))
        self.assertTrue({
            "WOT_BOOTSTRAP_TEST_HELPER_CRASH_AT",
            "WOT_BOOTSTRAP_TEST_HELPER_TRACE",
            "journal_phase",
        }.isdisjoint(formats.TEST_SEAM_MARKERS))
        self.assertTrue(formats.main_executable_matches_configuration(
            "WoT/Binaries/Win64/WoT-Win64-Shipping.exe", "Win64", "Shipping"))
        self.assertFalse(formats.main_executable_matches_configuration(
            "WoT/Binaries/Win64/WoT-Win64-Test.exe", "Win64", "Shipping"))
        self.assertTrue(formats.main_executable_matches_configuration(
            "Contents/MacOS/WoT-Mac-Shipping", "Mac", "Shipping"))


    def test_development_source_drift_receipt_is_not_acceptance_evidence(self):
        release_json = fixtures.write_guarded_release(
            self.directory / "development-drift-release", "Mac",
            release_sequence=1)
        receipt_path = fixtures.write_funnel_receipt(
            self.directory / "development-drift-receipt.json", release_json,
            include_update_cells=True,
            development_allow_source_drift=True)
        release = formats.load_guarded_release(release_json, "Mac")
        required = (formats.FUNNEL_REQUIRED_CELLS
                    + formats.UPDATE_FUNNEL_CELLS_BY_PLATFORM["Mac"])
        with self.assertRaisesRegex(
                FormatError, "development.*source drift|source drift.*acceptance"):
            formats.validate_funnel_receipt(
                receipt_path, "Mac", release, required)

    def test_fresh_evidence_requires_platform_profile_containment(self):
        for platform, field, other in (
                ("Win64", "win_profile_appdata", "mac_profile_library"),
                ("Mac", "mac_profile_library", "win_profile_appdata")):
            with self.subTest(platform=platform):
                release_json = fixtures.write_guarded_release(
                    self.directory / f"fresh-{platform}", platform)
                receipt_path = fixtures.write_funnel_receipt(
                    self.directory / f"fresh-{platform}.json", release_json)
                release = formats.load_guarded_release(release_json, platform)
                formats.validate_funnel_receipt(
                    receipt_path, platform, release,
                    formats.FUNNEL_REQUIRED_CELLS)
                document = json.loads(receipt_path.read_text(encoding="utf-8"))
                fresh = next(cell for cell in document["cells"]
                             if cell["name"] == "fresh-first-run")
                self.assertIn(field, fresh["evidence"])
                self.assertNotIn(other, fresh["evidence"])
                fresh["evidence"][field] = {}
                receipt_path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaisesRegex(FormatError, field):
                    formats.validate_funnel_receipt(
                        receipt_path, platform, release,
                        formats.FUNNEL_REQUIRED_CELLS)

    def test_fresh_evidence_requires_its_own_frontend_handoff(self):
        release_json = fixtures.write_guarded_release(
            self.directory / "fresh-handoff", "Win64")
        receipt_path = fixtures.write_funnel_receipt(
            self.directory / "fresh-handoff.json", release_json)
        release = formats.load_guarded_release(release_json, "Win64")
        pristine = json.loads(receipt_path.read_text(encoding="utf-8"))
        cases = {
            "missing": (lambda evidence: evidence.pop("frontend_handoff"),
                        "fresh-first-run.evidence schema is not exact"),
            "other process": (lambda evidence: evidence["frontend_handoff"]
                              .update(pid=evidence["process"]["pid"] + 1),
                              "frontend-handoff"),
            "not handoff": (lambda evidence: evidence["frontend_handoff"]
                            .update(event="phase-ready"), "frontend-handoff"),
        }
        for label, (mutate, message) in cases.items():
            with self.subTest(case=label):
                document = copy.deepcopy(pristine)
                fresh = next(cell for cell in document["cells"]
                             if cell["name"] == "fresh-first-run")
                mutate(fresh["evidence"])
                receipt_path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaisesRegex(FormatError, message):
                    formats.validate_funnel_receipt(
                        receipt_path, "Win64", release,
                        formats.FUNNEL_REQUIRED_CELLS)

    def test_mac_fresh_evidence_names_the_engine_identifier_store(self):
        release_json = fixtures.write_guarded_release(
            self.directory / "fresh-engine-store", "Mac")
        receipt_path = fixtures.write_funnel_receipt(
            self.directory / "fresh-engine-store.json", release_json)
        release = formats.load_guarded_release(release_json, "Mac")
        pristine = json.loads(receipt_path.read_text(encoding="utf-8"))
        for value, accepted in (("machine-id-only", True), ("absent", True),
                                ("anything-else", False)):
            with self.subTest(value=value):
                document = copy.deepcopy(pristine)
                fresh = next(cell for cell in document["cells"]
                             if cell["name"] == "fresh-first-run")
                fresh["evidence"]["mac_profile_library"][
                    "engine_identifier_store"] = value
                receipt_path.write_text(json.dumps(document), encoding="utf-8")
                if accepted:
                    formats.validate_funnel_receipt(
                        receipt_path, "Mac", release,
                        formats.FUNNEL_REQUIRED_CELLS)
                else:
                    with self.assertRaisesRegex(
                            FormatError, "mac_profile_library is invalid"):
                        formats.validate_funnel_receipt(
                            receipt_path, "Mac", release,
                            formats.FUNNEL_REQUIRED_CELLS)

    def test_damaged_cache_missing_or_null_blobs_is_a_format_error(self):
        release_json = fixtures.write_guarded_release(
            self.directory / "damaged-nesting", "Win64")
        release = formats.load_guarded_release(release_json, "Win64")
        for label, value in (("missing", mock.sentinel.missing),
                             ("null", None)):
            with self.subTest(label=label):
                receipt_path = fixtures.write_funnel_receipt(
                    self.directory / f"damaged-{label}.json", release_json)
                document = json.loads(receipt_path.read_text(encoding="utf-8"))
                damage = next(cell for cell in document["cells"]
                              if cell["name"] == "damaged-cache-rebuild")
                manifest = damage["evidence"]["final"]["manifests"][0]["json"]
                if value is mock.sentinel.missing:
                    manifest.pop("blobs")
                else:
                    manifest["blobs"] = value
                reference = damage["evidence"]["final"]["manifests"][0]
                reference["sha256"] = hashlib.sha256(formats.canonical_bytes(
                    manifest, "malformed damaged manifest")).hexdigest()
                receipt_path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaisesRegex(FormatError, "blobs"):
                    formats.validate_funnel_receipt(
                        receipt_path, "Win64", release,
                        formats.FUNNEL_REQUIRED_CELLS)

    def test_each_passing_update_cell_requires_its_exact_evidence_schema(self):
        release_json = fixtures.write_guarded_release(
            self.directory / "update-evidence-release", "Win64",
            release_sequence=1)
        receipt_path = fixtures.write_funnel_receipt(
            self.directory / "update-evidence.json", release_json,
            include_update_cells=True)
        release = formats.load_guarded_release(release_json, "Win64")
        required = (formats.FUNNEL_REQUIRED_CELLS
                    + formats.UPDATE_FUNNEL_CELLS_BY_PLATFORM["Win64"])
        pristine = json.loads(receipt_path.read_text(encoding="utf-8"))
        formats.validate_funnel_receipt(receipt_path, "Win64", release, required)

        for name in formats.UPDATE_FUNNEL_CELLS:
            with self.subTest(cell=name, mutation="empty"):
                document = copy.deepcopy(pristine)
                cell = next(item for item in document["cells"]
                            if item["name"] == name)
                cell["evidence"] = {}
                receipt_path.write_text(
                    json.dumps(document, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
                with self.assertRaisesRegex(FormatError, name):
                    formats.validate_funnel_receipt(
                        receipt_path, "Win64", release, required)
            with self.subTest(cell=name, mutation="extra"):
                document = copy.deepcopy(pristine)
                cell = next(item for item in document["cells"]
                            if item["name"] == name)
                cell["evidence"]["unbound"] = True
                receipt_path.write_text(
                    json.dumps(document, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
                with self.assertRaisesRegex(FormatError, name):
                    formats.validate_funnel_receipt(
                        receipt_path, "Win64", release, required)


    def test_update_cells_bind_release_and_derivation_receipt(self):
        release_json = fixtures.write_guarded_release(
            self.directory / "binding-release", "Win64", release_sequence=1)
        receipt_path = fixtures.write_funnel_receipt(
            self.directory / "binding" / "funnel-receipt.json", release_json,
            include_update_cells=True)
        release = formats.load_guarded_release(release_json, "Win64")
        required = (formats.FUNNEL_REQUIRED_CELLS
                    + formats.UPDATE_FUNNEL_CELLS_BY_PLATFORM["Win64"])
        pristine = json.loads(receipt_path.read_text(encoding="utf-8"))
        derivation_path = receipt_path.parent / "update-fixture-derivation-v1.json"
        derivation = json.loads(derivation_path.read_text(encoding="utf-8"))

        for field, value in (("git_commit", "different"),
                             ("release_sequence", 99),
                             ("release_json_sha256", "f" * 64),
                             ("archive_sha256", "f" * 64)):
            with self.subTest(binding=field):
                document = copy.deepcopy(pristine)
                cell = next(row for row in document["cells"]
                            if row["name"] == "update-apply")
                cell["evidence"]["base"][field] = value
                receipt_path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaises(FormatError):
                    formats.validate_funnel_receipt(
                        receipt_path, "Win64", release, required)

        for label, mutate in (
                ("missing", lambda ref: ref.update(path="missing.json")),
                ("digest", lambda ref: ref.update(sha256="f" * 64)),
                ("absolute", lambda ref: ref.update(path=str(derivation_path)))):
            with self.subTest(derivation=label):
                document = copy.deepcopy(pristine)
                for cell in document["cells"]:
                    if cell["name"] in formats.UPDATE_EVIDENCE_KEYS:
                        mutate(cell["evidence"]["derivation_receipt"])
                receipt_path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaises(FormatError):
                    formats.validate_funnel_receipt(
                        receipt_path, "Win64", release, required)

        receipt_path.write_text(json.dumps(pristine), encoding="utf-8")
        derivation["variants"]["good"]["tree_root"] = "f" * 64
        derivation_path.write_text(json.dumps(derivation), encoding="utf-8")
        digest, size = formats.sha256_file(derivation_path)
        document = copy.deepcopy(pristine)
        for cell in document["cells"]:
            if cell["name"] in formats.UPDATE_EVIDENCE_KEYS:
                cell["evidence"]["derivation_receipt"].update(
                    sha256=digest, size=size)
        receipt_path.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(FormatError, "good variant tree"):
            formats.validate_funnel_receipt(
                receipt_path, "Win64", release, required)

        derivation["unexpected"] = True
        derivation_path.write_text(json.dumps(derivation), encoding="utf-8")
        digest, size = formats.sha256_file(derivation_path)
        for cell in document["cells"]:
            if cell["name"] in formats.UPDATE_EVIDENCE_KEYS:
                cell["evidence"]["derivation_receipt"].update(
                    sha256=digest, size=size)
        receipt_path.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(FormatError, "schema is not exact"):
            formats.validate_funnel_receipt(
                receipt_path, "Win64", release, required)


    def test_mac_update_evidence_registers_all_s4b_cells(self):
        self.assertEqual(
            set(formats.UPDATE_EVIDENCE_KEYS_BY_PLATFORM["Mac"]),
            set(formats.UPDATE_FUNNEL_CELLS) | {"mac-uninstall"})

    def test_mac_update_evidence_accepts_platform_specific_proofs(self):
        for name in formats.UPDATE_FUNNEL_CELLS:
            with self.subTest(cell=name):
                evidence = fixtures.synthetic_update_evidence(name, 7, "Mac")
                formats.validate_update_cell_evidence(name, evidence, "Mac")


    # Win64 update validation as it stood before the Mac update-funnel port.
    # The Mac lane only adds Mac branches; a deliberate Win64 rule change
    # moves this pin together with the Win64 fixtures and tests.
    WIN64_PARITY_BASELINE = "32977b2a5a2ed211936e33fa214a3886a05e22cf"


class MacUninstallEvidenceTests(unittest.TestCase):
    def evidence(self) -> dict:
        profile = "/tmp/wot funnel/profiles/mac-uninstall"
        data_root = profile + "/Library/Application Support/WoT Reborn"
        bundle = "/tmp/wot funnel/cell-trees/mac-uninstall/WoT.app"
        bundle_parent = str(PurePosixPath(bundle).parent)
        update_dir = str(PurePosixPath(bundle).parent / ".wot-update")
        installer = str(PurePosixPath(bundle_parent) / formats.PINNED_INSTALLER_NAME)
        sentinel = str(PurePosixPath(bundle_parent)
                       / ".wot-uninstall-funnel-sentinel")
        session_gate = data_root + "/session.lock"
        parent_identity = {
            "device": 17, "inode": 101, "path": bundle_parent,
        }
        installer_identity = {
            "device": 17, "inode": 102, "path": installer,
            "sha256": hashlib.sha256(b"installer").hexdigest(), "size": 1024,
        }
        sentinel_identity = {
            "device": 17, "inode": 103, "path": sentinel,
            "sha256": hashlib.sha256(b"sentinel").hexdigest(), "size": 43,
        }
        session_gate_identity = {
            "device": 17, "inode": 104, "mode": 0o600,
            "path": session_gate,
            "sha256": hashlib.sha256(b"").hexdigest(), "size": 0,
        }
        return {
            "network": {
                "announcement_url":
                    "http://127.0.0.1:28123/announcement.json",
                "inherited_endpoint_variables_scrubbed": True,
                "release_base_url":
                    "http://127.0.0.1:28123" + formats.FUNNEL_RELEASE_BASE_PATH,
            },
            "paths": {
                "bundle": bundle,
                "bundle_parent": bundle_parent,
                "data_root": data_root,
                "installer": installer,
                "profile": profile,
                "session_gate": session_gate,
                "sibling_sentinel": sentinel,
                "trash": profile + "/.Trash",
                "update_dir": update_dir,
            },
            "postconditions": {
                "bundle_absent": True,
                "bundle_parent_unchanged": True,
                "data_root_absent": True,
                "installer_unchanged": True,
                "session_gate_absent": True,
                "sibling_sentinel_unchanged": True,
                "trash_contains_bundle_or_original_absent": True,
                "update_dir_absent": True,
            },
            "protected_artifacts": {
                "bundle_parent_after": dict(parent_identity),
                "bundle_parent_before": dict(parent_identity),
                "installer_after": dict(installer_identity),
                "installer_before": dict(installer_identity),
                "sibling_sentinel_after": dict(sentinel_identity),
                "sibling_sentinel_before": dict(sentinel_identity),
            },
            "process": {
                "argv": [
                    bundle + "/Contents/MacOS/WoT-Mac-Test",
                    "-wottestpreferencesdir=" + data_root + "/Preferences",
                    "-wottestuninstall=confirm",
                ],
                "exit_status": 0,
                "pid": 8123,
            },
            "request_transcript": [],
            "result": {
                "answer": "confirm",
                "failure_reason": "",
                "format": "wot-uninstall-result-v1",
                "remaining_paths": [],
                "removed_paths": [data_root, update_dir, bundle],
            },
            "session_gate": {
                "before": session_gate_identity,
                "owner_only_before_launch": True,
            },
        }

    def test_mac_uninstall_is_the_last_mac_only_cell(self):
        self.assertEqual(
            formats.UPDATE_FUNNEL_CELLS_BY_PLATFORM["Mac"][-1],
            "mac-uninstall")
        self.assertNotIn(
            "mac-uninstall", formats.UPDATE_FUNNEL_CELLS_BY_PLATFORM["Win64"])
        self.assertIn("wottestuninstall=", formats.TEST_SEAM_MARKERS)

    def test_mac_uninstall_evidence_is_closed_and_contradiction_free(self):
        evidence = self.evidence()
        formats.validate_update_cell_evidence(
            "mac-uninstall", evidence, "Mac")

        extra = copy.deepcopy(evidence)
        extra["unexpected"] = True
        with self.assertRaisesRegex(FormatError, "schema is not exact"):
            formats.validate_update_cell_evidence(
                "mac-uninstall", extra, "Mac")

        remaining = copy.deepcopy(evidence)
        remaining["result"]["remaining_paths"] = [
            remaining["paths"]["data_root"]]
        with self.assertRaisesRegex(FormatError, "remaining_paths"):
            formats.validate_update_cell_evidence(
                "mac-uninstall", remaining, "Mac")

        permissive_gate = copy.deepcopy(evidence)
        permissive_gate["session_gate"]["before"]["mode"] = 0o644
        with self.assertRaisesRegex(FormatError, "not owner-only"):
            formats.validate_update_cell_evidence(
                "mac-uninstall", permissive_gate, "Mac")

        wrong_seam = copy.deepcopy(evidence)
        wrong_seam["process"]["argv"][-1] = "-wottestuninstall=cancel"
        with self.assertRaisesRegex(FormatError, "confirm"):
            formats.validate_update_cell_evidence(
                "mac-uninstall", wrong_seam, "Mac")

        installer_changed = copy.deepcopy(evidence)
        installer_changed["protected_artifacts"]["installer_after"][
            "sha256"] = hashlib.sha256(b"changed").hexdigest()
        with self.assertRaisesRegex(FormatError, "protected artifacts changed"):
            formats.validate_update_cell_evidence(
                "mac-uninstall", installer_changed, "Mac")

        parent_replaced = copy.deepcopy(evidence)
        parent_replaced["protected_artifacts"]["bundle_parent_after"][
            "inode"] += 1
        with self.assertRaisesRegex(FormatError, "protected artifacts changed"):
            formats.validate_update_cell_evidence(
                "mac-uninstall", parent_replaced, "Mac")

        public_endpoint = copy.deepcopy(evidence)
        public_endpoint["network"]["announcement_url"] = (
            "https://github.com/aleiby/wot/announcement.json")
        with self.assertRaisesRegex(FormatError, "endpoint isolation"):
            formats.validate_update_cell_evidence(
                "mac-uninstall", public_endpoint, "Mac")

        outside_request = copy.deepcopy(evidence)
        outside_request["request_transcript"] = [{
            "format": "wot-funnel-loopback-request-v1",
            "if_range": "",
            "method": "GET",
            "path": "/installer",
            "range": "",
            "range_end": 1,
            "range_start": 0,
            "response_bytes": 2,
            "status": 200,
            "timestamp_utc": "2026-10-13T00:00:00+00:00",
        }]
        with self.assertRaisesRegex(FormatError, "escaped loopback"):
            formats.validate_update_cell_evidence(
                "mac-uninstall", outside_request, "Mac")


if __name__ == "__main__":
    unittest.main()
