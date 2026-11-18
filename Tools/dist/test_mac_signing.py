from __future__ import annotations

import json
import plistlib
import subprocess
import tempfile
import unittest
from pathlib import Path

import mac_signing


class FakeRunner:
    def __init__(self, responses=None):
        self.commands = []
        self.responses = list(responses or [])

    def __call__(self, command):
        self.commands.append(list(command))
        if self.responses:
            return self.responses.pop(0)
        return subprocess.CompletedProcess(command, 0, "", "")


def completed(stdout="", stderr="", returncode=0):
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


class MacSigningTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.app = self.root / "WoT.app"
        (self.app / "Contents" / "MacOS").mkdir(parents=True)
        (self.app / "Contents" / "Helpers" / "Deep").mkdir(parents=True)
        (self.app / "Contents" / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundleExecutable": "WoT-Mac-Test",
        }))
        self.main = self.app / "Contents" / "MacOS" / "WoT-Mac-Test"
        self.helper = self.app / "Contents" / "Helpers" / "WoTUpdater"
        self.deep = self.app / "Contents" / "Helpers" / "Deep" / "libThing.dylib"
        for path in (self.main, self.helper, self.deep):
            path.write_bytes(b"\xcf\xfa\xed\xfeMach-O")
        self.entitlements = self.root / "Sandbox.Server.entitlements"
        self.entitlements.write_text("<plist/>", encoding="utf-8")

    def tearDown(self):
        self.temporary.cleanup()

    def test_adhoc_command_order_keeps_deep_bundle_resign(self):
        runner = FakeRunner()
        result = mac_signing.sign_app(
            self.app, self.entitlements, None, runner)
        self.assertEqual(result, {"signing": "adhoc", "team_id": None})
        self.assertEqual(runner.commands, [
            ["codesign", "--force", "--deep", "--sign", "-",
             "--entitlements", str(self.entitlements), str(self.app)],
            ["codesign", "--verify", "--deep", "--strict", str(self.app)],
        ])

    def test_developer_id_signs_deepest_macho_first_and_excludes_main(self):
        runner = FakeRunner([
            completed(), completed(), completed(), completed(),
            completed(stderr="TeamIdentifier=TEAMID1234\n"),
        ])
        result = mac_signing.sign_app(
            self.app, self.entitlements,
            "Developer ID Application: Example (TEAMID1234)", runner)
        self.assertEqual(result, {
            "signing": "developer-id", "team_id": "TEAMID1234"})
        signed_targets = [command[-1] for command in runner.commands[:2]]
        self.assertEqual(signed_targets, [str(self.deep), str(self.helper)])
        self.assertNotIn(str(self.main), signed_targets)
        self.assertEqual(runner.commands[2], [
            "codesign", "--force", "--timestamp", "--options", "runtime",
            "--entitlements", str(self.entitlements), "--sign",
            "Developer ID Application: Example (TEAMID1234)", str(self.app)])
        self.assertEqual(runner.commands[3], [
            "codesign", "--verify", "--deep", "--strict", "--verbose=2",
            str(self.app)])

    def test_app_notarize_then_staple_command_order(self):
        runner = FakeRunner([
            completed(),
            completed(json.dumps({"id": "submission-1", "status": "Accepted"})),
            completed(), completed(),
        ])
        outcome = mac_signing.notarize(
            self.app, ["--keychain-profile", "profile-placeholder"], runner,
            self.root / "work")
        mac_signing.staple(self.app, runner)
        self.assertEqual(outcome, {
            "submission_id": "submission-1", "status": "Accepted"})
        self.assertEqual(runner.commands[0][:5], [
            "ditto", "-c", "-k", "--keepParent", str(self.app)])
        self.assertEqual(runner.commands[1][0:3], [
            "xcrun", "notarytool", "submit"])
        self.assertEqual(runner.commands[2], [
            "xcrun", "stapler", "staple", str(self.app)])
        self.assertEqual(runner.commands[3], [
            "xcrun", "stapler", "validate", str(self.app)])

    def test_developer_id_plus_notarized_full_order(self):
        runner = FakeRunner([
            completed(), completed(), completed(), completed(),
            completed(stderr="TeamIdentifier=TEAMID1234\n"),
            completed(),
            completed(json.dumps({"id": "submission-2", "status": "Accepted"})),
            completed(), completed(),
        ])
        mac_signing.sign_app(
            self.app, self.entitlements,
            "Developer ID Application: Example (TEAMID1234)", runner)
        mac_signing.notarize(
            self.app, ["--keychain-profile", "profile-placeholder"], runner,
            self.root / "full-work")
        mac_signing.staple(self.app, runner)
        commands = runner.commands
        bundle_sign = next(index for index, command in enumerate(commands)
                           if command[-1] == str(self.app)
                           and command[:2] == ["codesign", "--force"])
        verify = next(index for index, command in enumerate(commands)
                      if command[:2] == ["codesign", "--verify"])
        submit = next(index for index, command in enumerate(commands)
                      if command[:3] == ["xcrun", "notarytool", "submit"])
        staple = next(index for index, command in enumerate(commands)
                      if command[:3] == ["xcrun", "stapler", "staple"])
        validate = next(index for index, command in enumerate(commands)
                        if command[:3] == ["xcrun", "stapler", "validate"])
        self.assertLess(bundle_sign, verify)
        self.assertLess(verify, submit)
        self.assertLess(submit, staple)
        self.assertLess(staple, validate)

    def test_notary_auth_prefers_the_api_key_file(self):
        key = self.root / "AuthKey_TEST.p8"
        key.write_text("placeholder", encoding="utf-8")
        config = {"mac_notary_key_path": str(key), "mac_notary_key_id": "KEYID",
                  "mac_notary_issuer": "issuer-uuid",
                  "mac_notary_profile": "profile-placeholder"}
        self.assertEqual(
            mac_signing.notary_auth_args(
                lambda name, required=False: config.get(name)),
            ["--key", str(key), "--key-id", "KEYID", "--issuer", "issuer-uuid"])

    def test_notary_auth_falls_back_to_profile_then_none(self):
        profile_only = {"mac_notary_profile": "profile-placeholder"}
        self.assertEqual(
            mac_signing.notary_auth_args(
                lambda name, required=False: profile_only.get(name)),
            ["--keychain-profile", "profile-placeholder"])
        self.assertIsNone(mac_signing.notary_auth_args(
            lambda name, required=False: None))

    def test_notary_auth_refuses_partial_or_missing_key_config(self):
        partial = {"mac_notary_key_id": "KEYID"}
        with self.assertRaises(mac_signing.MacSigningError):
            mac_signing.notary_auth_args(
                lambda name, required=False: partial.get(name))
        missing = {"mac_notary_key_path": str(self.root / "absent.p8"),
                   "mac_notary_key_id": "KEYID", "mac_notary_issuer": "issuer"}
        with self.assertRaises(mac_signing.MacSigningError):
            mac_signing.notary_auth_args(
                lambda name, required=False: missing.get(name))

    def test_submit_and_log_carry_the_key_file_auth(self):
        auth = ["--key", "/k.p8", "--key-id", "KEYID", "--issuer", "issuer-uuid"]
        runner = FakeRunner([
            completed(),
            completed(json.dumps({"id": "submission-k", "status": "Accepted"})),
        ])
        mac_signing.notarize(self.app, auth, runner, self.root / "key-work")
        submit = runner.commands[1]
        self.assertEqual(submit[:4], ["xcrun", "notarytool", "submit",
                                      str(self.root / "key-work" / "WoT-app.zip")])
        self.assertEqual(submit[4:10], auth)
        self.assertNotIn("--keychain-profile", submit)

    def test_rejected_submission_lists_at_most_twenty_five_issues(self):
        issues = [{"path": "bad-%02d" % index, "message": "invalid"}
                  for index in range(30)]

        class RejectingRunner(FakeRunner):
            def __call__(runner_self, command):
                result = super(RejectingRunner, runner_self).__call__(command)
                if command[1:3] == ["notarytool", "log"]:
                    Path(command[-1]).write_text(
                        json.dumps({"issues": issues}), encoding="utf-8")
                return result

        runner = RejectingRunner([
            completed(),
            completed(json.dumps({"id": "submission-bad", "status": "Invalid"}),
                      returncode=1),
            completed(),
        ])
        with self.assertRaises(mac_signing.MacSigningError) as raised:
            mac_signing.notarize(
                self.app, ["--keychain-profile", "profile-placeholder"], runner,
                self.root / "reject")
        message = str(raised.exception)
        self.assertIn("bad-00 | invalid", message)
        self.assertIn("bad-24 | invalid", message)
        self.assertNotIn("bad-25", message)


if __name__ == "__main__":
    unittest.main()
