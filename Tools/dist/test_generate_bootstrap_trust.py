#!/usr/bin/env python3
"""Stdlib specs for the bootstrap trust producer's audit rules.

Every fixture here is synthetic. The keys are made-up hex strings, never a real
public key and never anything derived from private material: this suite tests the
AUDIT, and an audit is only worth having if each of its rules is shown to fire.
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_bootstrap_trust as trust

ROOT_A = "a" * 64
ROOT_B = "b" * 64
ANNOUNCEMENT = "c" * 64
UNBOUNDED = 9223372036854775807


def _export(**overrides):
    document = {
        "ceremony_utc": "2026-07-25T00:00:00+00:00",
        "format": trust.ANNOUNCEMENT_EXPORT_FORMAT,
        "key_id": "announcement-v1",
        "public_key": ANNOUNCEMENT,
    }
    document.update(overrides)
    return document


def _ring(**overrides):
    document = {
        "epoch": 1,
        "format": trust.OFFLINE_ROOT_RING_FORMAT,
        "keys": [
            {"first_sequence": 1, "key_id": "offline-root-a",
             "last_sequence": UNBOUNDED, "public_key": ROOT_A},
            {"first_sequence": 1, "key_id": "offline-root-b",
             "last_sequence": UNBOUNDED, "public_key": ROOT_B},
        ],
        "revoked_key_ids": [],
    }
    document.update(overrides)
    return document


class TrustGeneratorTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.export_path = self.root / "announcement-public-key-v1.json"
        self.ring_path = self.root / "public-root-ring.json"
        self.json_output = self.root / "out" / "bootstrap-trust-v1.json"
        self.header_output = self.root / "out" / "BootstrapTrust.generated.h"
        self.write_export(_export())
        self.write_ring(_ring())

    def tearDown(self):
        self._tmp.cleanup()

    def write_export(self, document):
        self.export_path.write_bytes(trust.canonical_bytes(document))

    def write_ring(self, document):
        self.ring_path.write_bytes(trust.canonical_bytes(document))

    def generate(self, **overrides):
        arguments = dict(
            announcement_path=self.export_path, root_ring_path=self.ring_path,
            win64_url=trust.WIN64_ANNOUNCEMENT_URL,
            mac_url=trust.MAC_ANNOUNCEMENT_URL,
            recovery_url=trust.ROOT_KEY_RING_RECOVERY_URL,
            json_output=self.json_output, header_output=self.header_output)
        arguments.update(overrides)
        return trust.generate(**arguments)

    def assert_rejects(self, pattern, **overrides):
        with self.assertRaisesRegex(trust.TrustError, pattern):
            self.generate(**overrides)

    # --- happy path ------------------------------------------------------------

    def test_a_clean_ceremony_produces_both_artifacts(self):
        document = self.generate()
        self.assertEqual(document["format"], trust.TRUST_FORMAT)
        self.assertEqual(document["announcement_public_key"], ANNOUNCEMENT)
        self.assertTrue(self.json_output.is_file())
        self.assertTrue(self.header_output.is_file())
        # The JSON is itself canonical, so it can be signed and compared as bytes.
        self.assertEqual(self.json_output.read_bytes(),
                         trust.canonical_bytes(document))

    def test_the_header_embeds_the_exact_root_ring_bytes(self):
        self.generate()
        header = self.header_output.read_text()
        raw = self.ring_path.read_bytes()
        self.assertIn(f"OfflineRootKeyRingSize = {len(raw)}", header)
        # Every byte of the ring must be present, in order.
        expected = ", ".join(f"0x{byte:02x}" for byte in raw[:12])
        self.assertIn(expected, header)
        self.assertIn(f'TEXT("{ANNOUNCEMENT}")', header)

    def test_the_header_exposes_one_platform_url_per_branch(self):
        self.generate()
        header = self.header_output.read_text()
        self.assertEqual(
            header.count("inline constexpr TCHAR AnnouncementUrl[]"), 2)
        self.assertIn("#if PLATFORM_WINDOWS", header)
        self.assertIn("#elif PLATFORM_MAC", header)
        self.assertIn("#error", header)
        # No simultaneously addressable pair.
        self.assertNotIn("Win64AnnouncementUrl", header)
        self.assertNotIn("MacAnnouncementUrl", header)
        # Each platform URL appears only inside its own branch.
        windows_branch = header.split("#if PLATFORM_WINDOWS")[1].split("#elif")[0]
        mac_branch = header.split("#elif PLATFORM_MAC")[1].split("#else")[0]
        self.assertIn(trust.WIN64_ANNOUNCEMENT_URL, windows_branch)
        self.assertNotIn(trust.MAC_ANNOUNCEMENT_URL, windows_branch)
        self.assertIn(trust.MAC_ANNOUNCEMENT_URL, mac_branch)
        self.assertNotIn(trust.WIN64_ANNOUNCEMENT_URL, mac_branch)

    def test_check_passes_on_fresh_output_and_fails_on_drift(self):
        self.generate()
        self.generate(check=True)  # must not raise
        self.header_output.write_text(
            self.header_output.read_text().replace(ANNOUNCEMENT, "d" * 64))
        self.assert_rejects("has drifted from its inputs", check=True)
        self.header_output.unlink()
        self.assert_rejects("cannot be read", check=True)

    def test_check_detects_a_regenerated_input(self):
        self.generate()
        self.write_ring(_ring(epoch=2))
        self.assert_rejects("has drifted from its inputs", check=True)

    # --- role separation -------------------------------------------------------

    def test_an_announcement_key_equal_to_a_root_key_is_refused(self):
        self.write_export(_export(public_key=ROOT_B))
        self.assert_rejects("also offline-root key 'offline-root-b'")

    def test_an_announcement_key_equal_to_another_role_is_refused(self):
        for role in ("release", "component", "recovery", "test"):
            with self.subTest(role=role):
                self.assert_rejects(f"also the {role} key",
                                    forbidden_keys={role: ANNOUNCEMENT})

    def test_forbidden_keys_are_normalized_before_comparison(self):
        self.assert_rejects("also the release key",
                            forbidden_keys={"release": ANNOUNCEMENT.upper()})

    def test_every_role_is_pairwise_distinct(self):
        self.assert_rejects("also offline-root key 'offline-root-a'",
                            forbidden_keys={"release": ROOT_A})
        self.assert_rejects("release and component roles share",
                            forbidden_keys={"release": "d" * 64,
                                            "component": "D" * 64})

    def test_a_root_ring_reusing_one_key_under_two_ids_is_refused(self):
        ring = _ring()
        ring["keys"][1]["public_key"] = ROOT_A
        self.write_ring(ring)
        self.assert_rejects("reuses one public key under several ids")

    def test_a_duplicate_root_key_id_is_refused(self):
        ring = _ring()
        ring["keys"][1]["key_id"] = "offline-root-a"
        self.write_ring(ring)
        self.assert_rejects("appears twice")

    # --- key encoding ----------------------------------------------------------

    def test_non_canonical_key_bytes_are_refused(self):
        for bad, why in (
            ("a" * 63, "short"),
            ("a" * 65, "long"),
            ("A" * 64, "upper case"),
            ("z" * 64, "non-hex"),
            (12345, "not a string"),
        ):
            with self.subTest(why=why):
                self.write_export(_export(public_key=bad))
                with self.assertRaises(trust.TrustError):
                    self.generate()
        self.write_export(_export())

    def test_a_non_canonical_input_file_is_refused(self):
        # Valid JSON, wrong byte form: pretty-printed with a trailing newline.
        self.export_path.write_text(json.dumps(_export(), indent=2) + "\n")
        self.assert_rejects("is not in canonical form")

    def test_a_private_key_marker_in_an_input_is_refused(self):
        document = _export()
        document["key_id"] = "announcement-v1-private"
        self.write_export(document)
        self.assert_rejects("private-key marker")

    # --- schemas ---------------------------------------------------------------

    def test_unknown_and_missing_fields_are_refused(self):
        document = _export()
        document["surprise"] = 1
        self.write_export(document)
        self.assert_rejects("unknown field")

        document = _export()
        del document["ceremony_utc"]
        self.write_export(document)
        self.assert_rejects("missing field")
        self.write_export(_export())

        ring = _ring()
        ring["surprise"] = 1
        self.write_ring(ring)
        self.assert_rejects("unknown field")

        ring = _ring()
        ring["keys"][0]["surprise"] = 1
        self.write_ring(ring)
        self.assert_rejects("unknown field")

    def test_a_release_ring_is_not_a_root_ring(self):
        self.write_ring(_ring(format="wot-release-key-ring-v1"))
        self.assert_rejects("a release ring is not a root ring")

    def test_a_wrong_export_format_is_refused(self):
        self.write_export(_export(format="wot-announcement-public-key-v2"))
        self.assert_rejects("expected 'wot-announcement-public-key-v1'")

    def test_an_empty_or_malformed_ring_is_refused(self):
        self.write_ring(_ring(keys=[]))
        self.assert_rejects("at least one key")

        ring = _ring()
        ring["keys"][0]["first_sequence"] = UNBOUNDED
        ring["keys"][0]["last_sequence"] = 1
        self.write_ring(ring)
        self.assert_rejects("inverted sequence range")

        ring = _ring()
        ring["keys"][0]["first_sequence"] = -1
        self.write_ring(ring)
        self.assert_rejects("non-negative integer")

        self.write_ring(_ring(epoch=-1))
        self.assert_rejects("non-negative integer")

        ring = _ring()
        ring["keys"][0]["key_id"] = ""
        self.write_ring(ring)
        self.assert_rejects("empty key_id")

    def test_a_missing_input_file_is_refused(self):
        self.export_path.unlink()
        self.assert_rejects("cannot read")

    # --- URLs ------------------------------------------------------------------

    def test_the_recovery_url_must_be_exactly_the_fixed_constant(self):
        for bad, why in (
            ("http://github.com/aleiby/wot/releases/latest/download/"
             "WoT-root-key-ring-v1.json", "not HTTPS"),
            ("https://github.com/aleiby/wot/releases/latest/download/"
             "WoT-root-key-ring-v2.json", "a different asset"),
            ("https://evil.example/github.com/aleiby/wot/releases/latest/"
             "download/WoT-root-key-ring-v1.json", "a lookalike host"),
            (trust.ROOT_KEY_RING_RECOVERY_URL + "?x=1", "an extra query"),
            (trust.WIN64_ANNOUNCEMENT_URL, "the announcement URL"),
        ):
            with self.subTest(why=why):
                with self.assertRaises(trust.TrustError):
                    self.generate(recovery_url=bad)

    def test_a_swapped_or_wrong_platform_url_is_refused(self):
        self.assert_rejects("win64 announcement URL must be exactly",
                            win64_url=trust.MAC_ANNOUNCEMENT_URL)
        self.assert_rejects("mac announcement URL must be exactly",
                            mac_url=trust.WIN64_ANNOUNCEMENT_URL)
        self.assert_rejects("not HTTPS",
                            win64_url=trust.WIN64_ANNOUNCEMENT_URL.replace(
                                "https://", "http://"))
        self.assert_rejects("is empty", mac_url="")


class MainTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "announcement-public-key-v1.json").write_bytes(
            trust.canonical_bytes(_export()))
        (self.root / "public-root-ring.json").write_bytes(
            trust.canonical_bytes(_ring()))

    def tearDown(self):
        self._tmp.cleanup()

    def _argv(self, *extra):
        return [
            "--announcement-public-key",
            str(self.root / "announcement-public-key-v1.json"),
            "--root-ring", str(self.root / "public-root-ring.json"),
            "--win64-announcement-url", trust.WIN64_ANNOUNCEMENT_URL,
            "--mac-announcement-url", trust.MAC_ANNOUNCEMENT_URL,
            "--root-key-ring-recovery-url", trust.ROOT_KEY_RING_RECOVERY_URL,
            "--json-output", str(self.root / "bootstrap-trust-v1.json"),
            "--header-output", str(self.root / "BootstrapTrust.generated.h"),
            *extra,
        ]

    def test_main_writes_then_verifies_then_detects_drift(self):
        self.assertEqual(trust.main(self._argv()), 0)
        self.assertEqual(trust.main(self._argv("--check")), 0)
        header = self.root / "BootstrapTrust.generated.h"
        header.write_text(header.read_text() + "// tampered\n")
        self.assertEqual(trust.main(self._argv("--check")), 1)

    def test_main_reports_a_failed_audit_as_a_non_zero_exit(self):
        self.assertEqual(
            trust.main(self._argv("--forbidden-key", f"release={ANNOUNCEMENT}")), 1)

    def test_main_rejects_a_malformed_forbidden_key_argument(self):
        with self.assertRaises(SystemExit):
            trust.main(self._argv("--forbidden-key", "no-equals-sign"))

    def test_main_rejects_a_repeated_forbidden_role(self):
        with self.assertRaises(SystemExit):
            trust.main(self._argv(
                "--forbidden-key", "release=" + "d" * 64,
                "--forbidden-key", "release=" + "e" * 64))


if __name__ == "__main__":
    unittest.main()
