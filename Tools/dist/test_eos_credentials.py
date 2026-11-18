#!/usr/bin/env python3
"""Specs for the packaging-time EOS credential injection.

Every value is an obviously fake string in a temp file; no real EOS id or
secret is read, printed or committed.
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eos_credentials as eos  # noqa: E402

FAKE = {
    "ProductId": "FAKEproduct0000000000000000000000",
    "SandboxId": "FAKEsandbox0000000000000000000000",
    "DeploymentIdTest": "FAKEdeployTEST000000000000000000",
    "DeploymentIdPublic": "FAKEdeployPUBLIC0000000000000000",
    "ClientId": "FAKEclient00000000000000000000000",
    "ClientSecret": "FAKE+secret/value=000000000000000",
}
REPO_CONFIG = Path(__file__).resolve().parents[2] / "Config" / "DefaultEngine.ini"


class EOSCredentialFileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def write(self, document, name="eos-credentials.json") -> Path:
        path = self.root / name
        path.write_text(document if isinstance(document, str)
                        else json.dumps(document), encoding="utf-8")
        return path

    def test_default_path_is_the_user_profile_secret_file(self):
        profile = "/synthetic/user-profile"
        path, source = eos.credentials_path({"USERPROFILE": profile})
        self.assertEqual(source, "default")
        self.assertEqual(path, Path(profile) / ".wot-secrets"
                         / "eos-credentials.json")

    def test_environment_variable_overrides_the_path(self):
        path, source = eos.credentials_path(
            {eos.ENV_FILE: str(self.root / "x.json"), "USERPROFILE": "ignored"})
        self.assertEqual((path, source), (self.root / "x.json", "env"))

    def test_missing_file_is_absent_and_disables_cleanly(self):
        result = eos.load({"USERPROFILE": str(self.root)})
        self.assertEqual(result.state, "absent")
        record = eos.receipt(result, "Test")
        self.assertEqual(record["internet_play"], "disabled")
        self.assertIsNone(record["artifact"])
        self.assertEqual(eos.stage_identity(record), {"state": "disabled"})

    def test_valid_file_loads_through_the_environment_variable(self):
        path = self.write(FAKE)
        result = eos.load({eos.ENV_FILE: str(path)})
        self.assertEqual(result.state, "present")
        self.assertEqual(result.source, "env")
        self.assertEqual(result.keys, tuple(sorted(eos.KEYS)))
        self.assertEqual(result.file_sha256, eos.canonical_sha256(FAKE))
        self.assertEqual(result.credentials.deployment_test, FAKE["DeploymentIdTest"])

    def test_invalid_files_disable_and_never_echo_values(self):
        cases = {
            "not-json": "{not json",
            "array": json.dumps([1, 2]),
            "missing-key": {k: v for k, v in FAKE.items() if k != "ClientSecret"},
            "extra-key": {**FAKE, "Extra": "x"},
            "empty": {**FAKE, "ProductId": ""},
            "placeholder": {**FAKE, "ClientId": "REPLACE_WITH_CLIENT_ID"},
            "angle": {**FAKE, "SandboxId": "<SandboxId>"},
            "number": {**FAKE, "DeploymentIdTest": 7},
            "space": {**FAKE, "ClientSecret": "has space"},
            "quote": {**FAKE, "ClientSecret": 'q"uote'},
            "paren": {**FAKE, "ClientSecret": "paren("},
            "comma": {**FAKE, "ClientSecret": "comma,"},
            "nonascii": {**FAKE, "ProductId": "caf\u00e9"},
        }
        for label, document in cases.items():
            with self.subTest(label):
                path = self.write(document, f"{label}.json")
                result = eos.load(explicit=path)
                self.assertEqual(result.state, "invalid")
                record = json.dumps(eos.receipt(result, "Shipping"))
                for value in FAKE.values():
                    self.assertNotIn(value, record)
                self.assertNotIn("has space", record)
                self.assertEqual(eos.stage_identity(json.loads(record)),
                                 {"state": "disabled"})

    def test_unexpected_keys_are_counted_never_echoed(self):
        secret_like = "FAKEsecretUsedAsAKey000000000000"
        swapped = {**{k: v for k, v in FAKE.items() if k != "ClientSecret"},
                   secret_like: "ClientSecret"}
        result = eos.load(explicit=self.write(swapped, "swapped.json"))
        self.assertEqual(result.state, "invalid")
        record = json.dumps(eos.receipt(result, "Test"))
        self.assertNotIn(secret_like, record)
        self.assertIn("unknown_keys=1", record)
        self.assertIn("ClientSecret", record)  # the allow-listed missing name
        for key in json.loads(record)["keys"]:
            self.assertIn(key, eos.KEYS)

    def test_utf8_bom_is_tolerated(self):
        path = self.root / "bom.json"
        path.write_bytes(b"\xef\xbb\xbf" + json.dumps(FAKE).encode("utf-8"))
        self.assertEqual(eos.load(explicit=path).state, "present")


class EOSConfigInjectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.config = Path(self.temp.name) / "DefaultEngine.ini"
        self.base = ("[OnlineSubsystem]\nDefaultPlatformService=Null\n\n"
                     "[/Script/OnlineSubsystemEOS.EOSSettings]\n"
                     "; the +Artifacts rows are injected at packaging time\n"
                     "DefaultArtifactName=WoT\n")
        self.config.write_text(self.base, encoding="utf-8")
        path = Path(self.temp.name) / "creds.json"
        path.write_text(json.dumps(FAKE), encoding="utf-8")
        self.present = eos.load(explicit=path)

    def tearDown(self):
        self.temp.cleanup()

    def text(self) -> str:
        return self.config.read_text(encoding="utf-8")

    def test_test_package_gets_the_test_deployment_only(self):
        record = eos.apply_to_config(self.config, self.present, "Test")
        text = self.text()
        self.assertIn('ArtifactName="WoTTest"', text)
        self.assertIn(f'DeploymentId="{FAKE["DeploymentIdTest"]}"', text)
        self.assertNotIn(FAKE["DeploymentIdPublic"], text)
        self.assertEqual(text.count("+Artifacts="), 1)
        self.assertEqual(record["artifact"], "WoTTest")
        self.assertEqual(record["internet_play"], "configured")

    def test_shipping_package_gets_the_public_deployment_only(self):
        eos.apply_to_config(self.config, self.present, "Test")
        record = eos.apply_to_config(self.config, self.present, "Shipping")
        text = self.text()
        self.assertIn(
            '+Artifacts=(ArtifactName="WoT",ClientId="FAKEclient00000000000000000000000",'
            'ClientSecret="FAKE+secret/value=000000000000000",'
            'ProductId="FAKEproduct0000000000000000000000",'
            'SandboxId="FAKEsandbox0000000000000000000000",'
            'DeploymentId="FAKEdeployPUBLIC0000000000000000")', text)
        self.assertNotIn(FAKE["DeploymentIdTest"], text)
        self.assertEqual(text.count("+Artifacts="), 1)
        self.assertEqual(record["artifact"], "WoT")

    def test_rewrites_are_idempotent_and_removable(self):
        eos.apply_to_config(self.config, self.present, "Test")
        once = self.text()
        eos.apply_to_config(self.config, self.present, "Test")
        self.assertEqual(self.text(), once)
        absent = eos.LoadResult("absent", "default")
        record = eos.apply_to_config(self.config, absent, "Test")
        self.assertEqual(self.text(), self.base)
        self.assertEqual(record["internet_play"], "disabled")

    def test_receipts_and_output_carry_no_value(self):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            record = eos.apply_to_config(self.config, self.present, "Shipping")
        serialized = json.dumps(record)
        for value in FAKE.values():
            self.assertNotIn(value, serialized)
        self.assertEqual(out.getvalue() + err.getvalue(), "")
        self.assertEqual(record["keys"], sorted(eos.KEYS))
        self.assertEqual(record["file_sha256"], eos.canonical_sha256(FAKE))
        self.assertEqual(eos.stage_identity(record)["state"], "present")

    def test_credentials_outside_the_block_are_refused(self):
        self.config.write_text(
            "[/Script/OnlineSubsystemEOS.EOSSettings]\n"
            '+Artifacts=(ArtifactName="WoT",ClientId="x")\n', encoding="utf-8")
        with self.assertRaisesRegex(eos.CredentialError, "outside the injected block"):
            eos.apply_to_config(self.config, self.present, "Test")

    def test_unbalanced_blocks_are_refused(self):
        self.config.write_text(eos.BEGIN + "\nfoo\n", encoding="utf-8")
        with self.assertRaises(eos.CredentialError):
            eos.apply_to_config(self.config, self.present, "Test")

    def test_pre_eos_stamps_keep_their_cache(self):
        self.assertEqual(eos.stage_identity(None), {"state": "disabled"})
        self.assertEqual(eos.stage_identity({}), {"state": "disabled"})

    def test_repository_config_carries_no_credentials(self):
        text = REPO_CONFIG.read_text(encoding="utf-8")
        self.assertFalse(eos._has_artifact_rows(text))
        self.assertNotIn(eos.BEGIN, text)
        self.assertIn("bUseEOSConnect=True", text)
        self.assertIn("DefaultArtifactName=WoT", text)


if __name__ == "__main__":
    unittest.main()
