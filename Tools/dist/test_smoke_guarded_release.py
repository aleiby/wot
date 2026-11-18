#!/usr/bin/env python3
"""Stdlib tests for the bounded guarded-release smoke receipt."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import smoke_guarded_release as smoke


class GuardedSmokeReceiptTests(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name)
        self.addCleanup(self._temporary.cleanup)

    def valid_sidecar(self) -> dict:
        return {
            "etag": "\"fixture\"",
            "expected_sha256": "a" * 64,
            "expected_size": 1_000_000_000,
            "format": smoke.SIDECAR_FORMAT,
            "last_modified": "Wed, 29 Jul 2026 00:00:00 GMT",
            "url": "https://archive.org/download/example/example.exe",
        }

    def test_sidecar_digest_and_parsed_response_decision(self):
        installer = self.root / "profile" / "UpdaterState" / "installer"
        installer.mkdir(parents=True)
        part = installer / "original.exe.part"
        part.write_bytes(b"")
        sidecar = installer / "original.exe.part.json"
        sidecar.write_text(json.dumps(self.valid_sidecar()))

        observed = smoke._observe(self.root / "profile")

        self.assertEqual(observed["kind"], "response_start_sidecar")
        self.assertEqual(observed["parsed"], self.valid_sidecar())
        self.assertEqual(observed["decision"]["response_start"], "accepted")
        self.assertEqual(observed["part_size"], 0)
        self.assertEqual(observed["sha256"], smoke._sha256_file(sidecar))

    def test_first_nonzero_partial_is_a_success_condition(self):
        installer = self.root / "profile" / "installer"
        installer.mkdir(parents=True)
        part = installer / "original.exe.part"
        part.write_bytes(b"first bytes")

        observed = smoke._observe(self.root / "profile")

        self.assertEqual(observed["kind"], "first_nonzero_partial")
        self.assertEqual(observed["part_size"], len(b"first bytes"))

    def test_boot_failure_receipt_preempts_network_observation(self):
        profile = self.root / "profile"
        installer = profile / "installer"
        state = profile / "UpdaterState"
        installer.mkdir(parents=True)
        state.mkdir()
        (installer / "original.exe.part").write_bytes(b"first bytes")
        failure = state / smoke.BOOT_FAILURE_LEAF
        failure.write_text(json.dumps({
            "diagnostic_code": "GUARDED-SESSION-ABSENT",
            "format": "wot-bootstrap-boot-failure-v1",
            "phase": "EstablishingIdentity",
            "timestamp_utc": "2026-07-29T00:00:00.000Z",
        }))

        observed = smoke._observe(profile)

        self.assertEqual(observed["kind"], "boot_failure")
        self.assertEqual(
            observed["receipt"]["diagnostic_code"],
            "GUARDED-SESSION-ABSENT")

    def test_malformed_sidecar_is_rejected(self):
        sidecar = self.root / "bad.part.json"
        value = self.valid_sidecar()
        value["unexpected"] = "field"
        sidecar.write_text(json.dumps(value))
        with self.assertRaisesRegex(smoke.SmokeFailure, "malformed"):
            smoke._sidecar_decision(sidecar)

    def test_receipt_publication_is_atomic_and_leaves_no_temp(self):
        path = self.root / "evidence" / "smoke-receipt.json"
        smoke._atomic_write_json(path, {
            "format": smoke.SMOKE_FORMAT,
            "verdict": "pass",
        })
        self.assertEqual(
            json.loads(path.read_text())["format"], smoke.SMOKE_FORMAT)
        self.assertEqual(
            list(path.parent.glob(".smoke-receipt.json.*.tmp")), [])

    def test_scratch_root_is_canonicalized_before_guarded_launch(self):
        physical = self.root / "physical"
        scratch = physical / "scratch"
        scratch.mkdir(parents=True)
        logical = self.root / "logical"
        logical.symlink_to(physical, target_is_directory=True)
        with mock.patch.object(
                smoke.tempfile, "mkdtemp",
                return_value=str(logical / "scratch")):
            result = smoke._make_scratch_root()
        self.assertEqual(result, scratch.resolve(strict=True))
        self.assertNotIn("logical", result.parts)


if __name__ == "__main__":
    unittest.main()
