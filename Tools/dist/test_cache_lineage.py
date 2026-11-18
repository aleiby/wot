#!/usr/bin/env python3
"""Specs for cache_lineage.py: the gate A rules, the one-time rederivation and
the reference taken from a signed lineage asset.

Nothing here launches a game or touches a real key or release: ``rederive``
runs a fake smoke script (through this interpreter, as on Windows a ``.py`` has
no shebang), and signed publish sets come from release_test_fixtures, whose
Ed25519 keys are minted in a temporary directory and destroyed with it.
"""
from __future__ import annotations

import contextlib
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_content_label as content_label  # noqa: E402
import cache_lineage  # noqa: E402
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import release_test_fixtures as fixtures  # noqa: E402
import verify_release  # noqa: E402

ASSET_SHA256 = "6" * 64

# A stand-in for smoke_shipping_release.py. It reads its behaviour from the JSON
# beside it, records the argv it was given, writes a ContentCache under the work
# root and a smoke receipt into the evidence directory, and exits as told.
FAKE_SMOKE = r'''
import argparse
import hashlib
import json
import sys
from pathlib import Path

plan = json.loads(Path(__file__).with_suffix(".json").read_text(encoding="utf-8"))
parser = argparse.ArgumentParser()
for name in ("--release", "--installer", "--host-trigger", "--work-root", "--evidence"):
    parser.add_argument(name, required=True)
parser.add_argument("--allow-source-drift", action="store_true")
args = parser.parse_args()
evidence = Path(args.evidence)
evidence.mkdir(parents=True, exist_ok=True)
(evidence / "argv.json").write_text(json.dumps(sys.argv[1:]), encoding="utf-8")
if plan["exit"]:
    sys.exit(plan["exit"])


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("ascii")


release_json = Path(args.release)
release = json.loads(release_json.read_text(encoding="utf-8"))
manifest = plan["manifest"]
cache = Path(args.work_root) / "extracted" / "WoT Reborn" / "Data" / "ContentCache"
(cache / "current").mkdir(parents=True)
(cache / "current" / "manifest.json").write_bytes(canonical(manifest))
digest = hashlib.sha256(canonical(manifest)).hexdigest()
if plan.get("tamper_snapshot"):
    digest = "0" * 64
receipt = {
    "checks": {"boot": {"cache": {"manifests": [{
        "exists": True, "json": manifest,
        "path": str(cache / "current" / "manifest.json"), "sha256": digest,
        "size": len(canonical(manifest))}]}}},
    "format": "wot-shipping-smoke-v1",
    "platform": release["platform"],
    "release": {
        "asset_sha256": release["sha256"],
        "manifest_sha256": hashlib.sha256(release_json.read_bytes()).hexdigest(),
    },
    "verdict": "pass",
}
(evidence / "shipping-smoke-receipt.json").write_text(
    json.dumps(receipt, indent=2), encoding="utf-8")
'''


def _repo_tmp() -> Path:
    """The ignored .tmp folder in the checkout (kept off the system temp
    volume); a fresh clone does not have it yet."""
    path = Path.cwd() / ".tmp"
    path.mkdir(exist_ok=True)
    return path


def _snapshot(manifest: dict) -> dict:
    return {"exists": True, "json": manifest,
            "path": "Data/ContentCache/current/manifest.json",
            "sha256": fixtures.manifest_digest(manifest)}


class GateRuleTests(unittest.TestCase):
    PLATFORM = "Win64"

    def gate(self, reference_manifest, candidate_manifest, compiled_revision, *,
             funnel=None, reference=None, smoke=None):
        return cache_lineage.evaluate_gate(
            platform=self.PLATFORM,
            reference=reference or fixtures.synthetic_reference(
                self.PLATFORM, reference_manifest),
            smoke_receipt=smoke or fixtures.synthetic_smoke_receipt(
                self.PLATFORM, ASSET_SHA256, candidate_manifest),
            compiled_revision=compiled_revision, funnel_receipt=funnel)

    def refused(self, fragment, *arguments, **options):
        with self.assertRaises(cache_lineage.GateError) as caught:
            self.gate(*arguments, **options)
        self.assertIn(fragment, str(caught.exception))
        return str(caught.exception)

    def test_same_revision_and_bytes_pass_with_identity_unchanged(self):
        manifest = fixtures.synthetic_cache_manifest(1)
        receipt = self.gate(manifest, manifest, 1)
        digest = fixtures.manifest_digest(manifest)
        self.assertEqual(receipt["verdict"], "pass")
        self.assertIs(receipt["identity_unchanged"], True)
        self.assertEqual(receipt["candidate"], {
            "contract_sha256": "1" * 64,
            "installer_sha256": formats.PINNED_INSTALLER_SHA256,
            "label": content_label.REVISION_1_LABEL,
            "manifest_sha256": digest,
            "release_asset_sha256": ASSET_SHA256,
            "revision": 1,
        })
        self.assertEqual(receipt["lineage"], {
            "contract_sha256": "1" * 64,
            "installer_sha256": formats.PINNED_INSTALLER_SHA256,
            "label": content_label.REVISION_1_LABEL,
            "manifest_sha256": digest,
            "platform": "Win64",
            "revision": 1,
        })
        self.assertEqual(receipt["reference"]["source"], "lineage")
        self.assertNotIn("content_cache_dir", receipt["reference"])
        self.assertEqual(set(receipt), {"format", "verdict", "platform",
                                        "identity_unchanged", "reference",
                                        "candidate", "lineage"})

    def test_same_revision_with_different_bytes_fails(self):
        self.refused("without a WoTCacheContentRevision bump",
                     fixtures.synthetic_cache_manifest(1),
                     fixtures.synthetic_cache_manifest(1, blob_sha256="4" * 64), 1)
        # A contract or installer change is a manifest change too.
        self.refused("without a WoTCacheContentRevision bump",
                     fixtures.synthetic_cache_manifest(1),
                     fixtures.synthetic_cache_manifest(1, contract_sha256="2" * 64), 1)

    def test_higher_revision_passes_and_records_the_new_lineage(self):
        receipt = self.gate(fixtures.synthetic_cache_manifest(1),
                            fixtures.synthetic_cache_manifest(2, blob_sha256="4" * 64), 2)
        self.assertEqual(receipt["verdict"], "pass")
        self.assertIs(receipt["identity_unchanged"], False)
        self.assertEqual((receipt["reference"]["revision"], receipt["candidate"]["revision"],
                          receipt["lineage"]["revision"]), (1, 2, 2))
        self.assertEqual(receipt["lineage"]["label"], content_label.label(2))
        # A bump with identical bytes is allowed as well.
        self.assertEqual(self.gate(fixtures.synthetic_cache_manifest(1),
                                   fixtures.synthetic_cache_manifest(2), 2)["verdict"],
                         "pass")

    def test_lower_revision_fails(self):
        self.refused("is lower than the previous release's 3",
                     fixtures.synthetic_cache_manifest(3),
                     fixtures.synthetic_cache_manifest(2), 2)

    def test_unknown_or_inconsistent_reference_label_fails(self):
        manifest = fixtures.synthetic_cache_manifest(1)
        unknown = fixtures.synthetic_reference(self.PLATFORM, manifest, revision=1)
        unknown["label"] = "0" * 64
        self.refused("is not the label of any content revision", manifest, manifest, 1,
                     reference=unknown)
        inconsistent = fixtures.synthetic_reference(self.PLATFORM, manifest, revision=2)
        self.refused("records revision 2 but its label is the label of revision 1",
                     manifest, manifest, 1, reference=inconsistent)
        for key, value in (("platform", "Mac"), ("format", "other"), ("source", "guess"),
                           ("manifest_sha256", "A" * 64)):
            broken = fixtures.synthetic_reference(self.PLATFORM, manifest)
            broken[key] = value
            with self.subTest(key=key), self.assertRaises(cache_lineage.GateError):
                self.gate(manifest, manifest, 1, reference=broken)

    def test_candidate_label_must_be_the_compiled_revision(self):
        self.refused("the smoked binary does not match the source",
                     fixtures.synthetic_cache_manifest(1),
                     fixtures.synthetic_cache_manifest(1), 2)
        self.refused("the smoked binary does not match the source",
                     fixtures.synthetic_cache_manifest(1),
                     fixtures.synthetic_cache_manifest(2), 1)

    def test_funnel_manifest_must_equal_the_shipping_manifest(self):
        manifest = fixtures.synthetic_cache_manifest(1)

        def funnel(captured, *, verdict="pass", name="fresh-first-run"):
            return {"platform": self.PLATFORM, "cells": [
                {"name": "second-launch", "verdict": "pass", "evidence": {}},
                {"name": name, "verdict": verdict,
                 "evidence": {"manifests": [_snapshot(captured)]}}]}

        self.assertEqual(self.gate(manifest, manifest, 1,
                                   funnel=funnel(manifest))["verdict"], "pass")
        self.refused("differs from the Shipping manifest", manifest, manifest, 1,
                     funnel=funnel(fixtures.synthetic_cache_manifest(
                         1, blob_sha256="5" * 64)))
        self.refused("fresh-first-run verdict", manifest, manifest, 1,
                     funnel=funnel(manifest, verdict="fail"))
        self.refused("no single fresh-first-run cell", manifest, manifest, 1,
                     funnel=funnel(manifest, name="host-join"))

    def test_missing_or_unproven_candidate_manifest_fails(self):
        manifest = fixtures.synthetic_cache_manifest(1)
        smoke = fixtures.synthetic_smoke_receipt(self.PLATFORM, ASSET_SHA256, None)
        self.refused("captured no cache manifest", manifest, manifest, 1, smoke=smoke)
        smoke = fixtures.synthetic_smoke_receipt(self.PLATFORM, ASSET_SHA256, manifest)
        del smoke["checks"]["boot"]
        self.refused("captured no cache manifest", manifest, manifest, 1, smoke=smoke)
        smoke = fixtures.synthetic_smoke_receipt(self.PLATFORM, ASSET_SHA256, manifest,
                                                 verdict="fail")
        self.refused("verdict is 'fail'", manifest, manifest, 1, smoke=smoke)
        smoke = fixtures.synthetic_smoke_receipt("Mac", ASSET_SHA256, manifest)
        self.refused("platform 'Mac' is not Win64", manifest, manifest, 1, smoke=smoke)
        smoke = fixtures.synthetic_smoke_receipt(self.PLATFORM, ASSET_SHA256, manifest)
        smoke["checks"]["boot"]["cache"]["manifests"][0]["sha256"] = "0" * 64
        self.refused("not the digest recorded with it", manifest, manifest, 1, smoke=smoke)
        smoke = fixtures.synthetic_smoke_receipt(self.PLATFORM, "nope", manifest)
        self.refused("release.asset_sha256", manifest, manifest, 1, smoke=smoke)

    def test_a_rederived_reference_keeps_its_content_cache(self):
        manifest = fixtures.synthetic_cache_manifest(1)
        reference = fixtures.synthetic_reference(self.PLATFORM, manifest)
        reference.update({
            "source": "rederived", "signed_manifest_sha256": "a" * 64,
            "main_executable_sha256": "b" * 64, "smoke_receipt_sha256": "c" * 64,
            "content_cache_dir": "work/smoke/ContentCache"})
        receipt = self.gate(manifest, manifest, 1, reference=reference)
        self.assertEqual(receipt["reference"]["source"], "rederived")
        self.assertEqual(receipt["reference"]["content_cache_dir"],
                         "work/smoke/ContentCache")
        del reference["smoke_receipt_sha256"]
        self.refused("missing=['smoke_receipt_sha256']", manifest, manifest, 1,
                     reference=reference)

    def test_gate_receipt_loader_rechecks_the_rules(self):
        manifest = fixtures.synthetic_cache_manifest(1)
        receipt = self.gate(manifest, manifest, 1)
        with tempfile.TemporaryDirectory(dir=_repo_tmp()) as temporary:
            path = Path(temporary) / "gate.json"

            def load(document, platform="Win64"):
                path.write_text(json.dumps(document), encoding="utf-8")
                return cache_lineage.load_gate_receipt(path, platform)

            self.assertEqual(load(receipt), receipt)
            mutations = {
                "is for 'Win64', not Mac": (lambda value: None, "Mac"),
                "verdict is 'fail'": (lambda value: value.update(verdict="fail"), "Win64"),
                "lineage does not repeat the candidate": (
                    lambda value: value["lineage"].update(manifest_sha256="0" * 64),
                    "Win64"),
                "identity_unchanged contradicts": (
                    lambda value: value.update(identity_unchanged=False), "Win64"),
                "does not satisfy the gate A rules": (
                    lambda value: value["reference"].update(manifest_sha256="0" * 64),
                    "Win64"),
                "label is not the label of its revision": (
                    lambda value: value["candidate"].update(revision=2), "Win64"),
                "unknown=['note']": (lambda value: value.update(note="x"), "Win64"),
            }
            for fragment, (mutate, platform) in mutations.items():
                document = json.loads(json.dumps(receipt))
                mutate(document)
                with self.subTest(fragment=fragment), \
                        self.assertRaises(cache_lineage.GateError) as caught:
                    load(document, platform)
                self.assertIn(fragment, str(caught.exception))

    def test_lineage_document_is_the_signed_shape(self):
        manifest = fixtures.synthetic_cache_manifest(2)
        receipt = self.gate(fixtures.synthetic_cache_manifest(1), manifest, 2)
        document = cache_lineage.lineage_document(receipt, 7)
        raw = formats.canonical_bytes(document)
        self.assertEqual(formats.parse_cache_lineage(raw, "Win64"), {
            "contract_sha256": "1" * 64, "format": "wot-cache-lineage-v1",
            "installer_sha256": formats.PINNED_INSTALLER_SHA256,
            "label": content_label.label(2),
            "manifest_sha256": fixtures.manifest_digest(manifest),
            "platform": "Win64", "release_sequence": 7, "revision": 2})

    def test_command_line_gate(self):
        manifest = fixtures.synthetic_cache_manifest(1)
        with tempfile.TemporaryDirectory(dir=_repo_tmp()) as temporary:
            root = Path(temporary)
            reference = root / "reference.json"
            smoke = root / "smoke.json"
            builder = root / "CacheBuilder.cpp"
            out = root / "gate.json"
            reference.write_text(json.dumps(fixtures.synthetic_reference(
                "Win64", manifest)), encoding="utf-8")
            smoke.write_text(json.dumps(fixtures.synthetic_smoke_receipt(
                "Win64", ASSET_SHA256, manifest)), encoding="utf-8")
            arguments = ["gate", "--platform", "Win64", "--reference", str(reference),
                         "--smoke-receipt", str(smoke), "--out", str(out)]
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(cache_lineage.main(arguments), 0)
            self.assertIn("PASS", output.getvalue())
            written = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(written["verdict"], "pass")
            self.assertEqual(cache_lineage.load_gate_receipt(out, "Win64"), written)

            builder.write_text("constexpr uint32 WoTCacheContentRevision = 2;\n",
                               encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(cache_lineage.main(
                    arguments + ["--cache-builder", str(builder)]), 1)
            self.assertIn("does not match the source", errors.getvalue())
            failed = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual((failed["verdict"], failed["platform"]), ("fail", "Win64"))
            self.assertIn("does not match the source", failed["reason"])
            with self.assertRaises(cache_lineage.GateError):
                cache_lineage.load_gate_receipt(out, "Win64")

            missing = root / "absent.json"
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(cache_lineage.main(
                    ["gate", "--platform", "Win64", "--reference", str(missing),
                     "--smoke-receipt", str(smoke), "--out", str(out)]), 1)
            self.assertIn("cannot read cache reference", errors.getvalue())


class SignedSetTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = fixtures.ReleaseFixture()
        cls.counter = 0

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()

    def scratch(self, label: str) -> Path:
        type(self).counter += 1
        return self.fixture.root / f"{label}-{self.counter}"


class RederiveTests(SignedSetTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.previous = cls.fixture.root / "previous-set"
        cls.fixture.sign(cls.previous)
        cls.smoke = cls.fixture.root / "fake_smoke.py"
        cls.smoke.write_text(FAKE_SMOKE, encoding="utf-8")
        cls.manifest = fixtures.synthetic_cache_manifest(1)

    def plan(self, **overrides):
        plan = {"exit": 0, "manifest": self.manifest}
        plan.update(overrides)
        self.smoke.with_suffix(".json").write_text(json.dumps(plan), encoding="utf-8")

    def rederive(self, work_root, **overrides):
        arguments = dict(
            platform="Win64", previous_set=self.previous,
            release_json=self.fixture.releases["Win64"],
            installer=self.fixture.root / formats.PINNED_INSTALLER_NAME,
            work_root=work_root, trust=self.fixture.trust,
            compiled=self.fixture.compiled, smoke_script=self.smoke)
        arguments.update(overrides)
        return cache_lineage.rederive(**arguments)

    def refused(self, fragment, work_root=None, **overrides):
        with self.assertRaises(cache_lineage.GateError) as caught:
            self.rederive(work_root or self.scratch("refused"), **overrides)
        self.assertIn(fragment, str(caught.exception))

    def test_rederive_records_the_signed_release_and_retains_its_cache(self):
        self.plan()
        work = self.scratch("work")
        reference = self.rederive(work, openssl=self.fixture.openssl)
        signed = json.loads((self.previous / formats.manifest_asset("Win64")).read_bytes())
        receipt = work / "evidence" / "shipping-smoke-receipt.json"
        cache = work / "smoke" / "extracted" / "WoT Reborn" / "Data" / "ContentCache"
        self.assertEqual(reference, {
            "content_cache_dir": str(cache.resolve()),
            "contract_sha256": "1" * 64,
            "format": "wot-cache-reference-v1",
            "installer_sha256": formats.PINNED_INSTALLER_SHA256,
            "label": content_label.REVISION_1_LABEL,
            "main_executable_sha256": signed["main_executable"]["sha256"],
            "manifest_sha256": fixtures.manifest_digest(self.manifest),
            "platform": "Win64",
            "release_sequence": 0,
            "revision": 1,
            "signed_manifest_sha256": formats.sha256_file(
                self.previous / formats.manifest_asset("Win64"))[0],
            "smoke_receipt_sha256": formats.sha256_file(receipt)[0],
            "source": "rederived",
        })
        argv = json.loads((work / "evidence" / "argv.json").read_text(encoding="utf-8"))
        self.assertEqual(argv[argv.index("--host-trigger") + 1], "none")
        self.assertIn("--allow-source-drift", argv)
        self.assertEqual(Path(argv[argv.index("--work-root") + 1]),
                         (work / "smoke").resolve())
        self.assertEqual(Path(argv[argv.index("--evidence") + 1]),
                         (work / "evidence").resolve())
        self.assertEqual(Path(argv[argv.index("--release") + 1]),
                         self.fixture.releases["Win64"])
        # The rederived reference gates an identical candidate.
        gate = cache_lineage.evaluate_gate(
            platform="Win64", reference=reference,
            smoke_receipt=fixtures.synthetic_smoke_receipt(
                "Win64", ASSET_SHA256, self.manifest),
            compiled_revision=1)
        self.assertIs(gate["identity_unchanged"], True)
        self.assertEqual(gate["reference"]["content_cache_dir"], str(cache.resolve()))
        # A rerun needs a fresh work root.
        self.refused("must be absent or empty", work)

    def test_the_package_must_be_the_signed_main_executable(self):
        calls = []

        def runner(command):
            calls.append(command)
            return 0

        files = fixtures.platform_files("Win64")
        game = "WoT/Binaries/Win64/WoT-Win64-Shipping.exe"
        files[game] = (files[game][0] + b"rebuilt", files[game][1])
        other = fixtures.write_guarded_release(self.scratch("other-binary"), "Win64",
                                               files=files)
        self.refused("is not the signed main_executable", release_json=other,
                     runner=runner)
        foreign = fixtures.write_guarded_release(self.scratch("other-commit"), "Win64",
                                                 commit="fedcba987")
        self.refused("not the signed build_commit", release_json=foreign, runner=runner)
        self.refused("is for 'Mac', not 'Win64'", release_json=self.fixture.releases["Mac"], runner=runner)
        self.assertEqual(calls, [])

    def test_the_previous_set_must_authenticate(self):
        tampered = fixtures.copy_set(self.previous, self.scratch("tampered-set"))
        (tampered / (formats.manifest_asset("Win64") + ".sig")).write_bytes(bytes(64))
        self.plan()
        self.refused("the previous Win64 release does not verify", previous_set=tampered)

    def test_a_failed_smoke_or_bad_capture_fails(self):
        self.plan(exit=3)
        self.refused("the Shipping smoke exited 3")
        self.plan(tamper_snapshot=True)
        self.refused("not the digest recorded with it")
        unknown = dict(self.manifest, production_descriptor_sha256="0" * 64)
        self.plan(manifest=unknown)
        self.refused("is not the label of any content revision")
        self.plan()
        self.refused("names a different release.json",
                     runner=lambda command: self._fake_with_other_release(command))

    def _fake_with_other_release(self, command):
        import subprocess

        code = subprocess.run(command).returncode
        evidence = Path(command[command.index("--evidence") + 1])
        path = evidence / "shipping-smoke-receipt.json"
        receipt = json.loads(path.read_text(encoding="utf-8"))
        receipt["release"]["asset_sha256"] = "9" * 64
        path.write_text(json.dumps(receipt), encoding="utf-8")
        return code

    def test_command_line_rederive(self):
        self.plan()
        work = self.scratch("cli-work")
        out = self.scratch("cli") / "reference.json"
        arguments = ["rederive", "--platform", "Win64",
                     "--previous-set", str(self.previous),
                     "--release-json", str(self.fixture.releases["Win64"]),
                     "--installer", str(self.fixture.root / "installer.exe"),
                     "--work-root", str(work), "--out", str(out),
                     *self.fixture.trust_arguments()]
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(cache_lineage.main(arguments, smoke_script=self.smoke), 0)
        self.assertIn("revision 1", output.getvalue())
        reference = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(cache_lineage.load_reference(reference, "Win64"), reference)
        self.assertEqual(reference["source"], "rederived")
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(cache_lineage.main(arguments, smoke_script=self.smoke), 1)
        self.assertIn("must be absent or empty", errors.getvalue())


class ReferenceFromLineageTests(SignedSetTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.set = cls.fixture.root / "sequence-one-set"
        cls.fixture.sign(cls.set, release_sequence=1, tag="v0.1.1")

    def gate_lineage(self, platform):
        path = (self.fixture.root / "cache-gates" / self.set.name / platform
                / "cache-gate-a.json")
        return json.loads(path.read_text(encoding="utf-8"))["lineage"]

    def reference(self, directory, platform="Win64", **options):
        return cache_lineage.reference_from_lineage(
            directory, platform, trust=self.fixture.trust,
            compiled=self.fixture.compiled, **options)

    def test_reference_is_the_verified_lineage(self):
        for platform in formats.PLATFORMS:
            with self.subTest(platform=platform):
                reference = self.reference(self.set, platform,
                                           openssl=self.fixture.openssl)
                lineage = self.gate_lineage(platform)
                self.assertEqual(reference, {
                    "format": "wot-cache-reference-v1", "release_sequence": 1,
                    "source": "lineage", **lineage})
                self.assertEqual(cache_lineage.load_reference(reference, platform),
                                 reference)
                manifest = fixtures.synthetic_cache_manifest(1)
                gate = cache_lineage.evaluate_gate(
                    platform=platform, reference=reference,
                    smoke_receipt=fixtures.synthetic_smoke_receipt(
                        platform, ASSET_SHA256, manifest),
                    compiled_revision=1)
                self.assertIs(gate["identity_unchanged"], True)
                changed = fixtures.synthetic_cache_manifest(1, blob_sha256="7" * 64)
                with self.assertRaises(cache_lineage.GateError):
                    cache_lineage.evaluate_gate(
                        platform=platform, reference=reference,
                        smoke_receipt=fixtures.synthetic_smoke_receipt(
                            platform, ASSET_SHA256, changed),
                        compiled_revision=1)

    def test_a_tampered_or_missing_lineage_is_refused(self):
        name = formats.cache_lineage_asset("Win64")
        directory = fixtures.copy_set(self.set, self.scratch("tampered"))
        document = json.loads((directory / name).read_bytes())
        document["manifest_sha256"] = "0" * 64
        (directory / name).write_bytes(formats.canonical_bytes(document))
        with self.assertRaises(cache_lineage.GateError) as caught:
            self.reference(directory)
        self.assertIn("the signature does not verify under release key", str(caught.exception))

        directory = fixtures.copy_set(self.set, self.scratch("wrong-key"))
        raw = (directory / name).read_bytes()
        (directory / (name + ".sig")).write_bytes(crypto.sign_framed(
            self.fixture.openssl, self.fixture.announcement_key,
            crypto.CONTEXT_CACHE_LINEAGE_V1, raw, self.fixture.announcement_public))
        with self.assertRaises(cache_lineage.GateError) as caught:
            self.reference(directory)
        self.assertIn("does not verify", str(caught.exception))

        directory = fixtures.copy_set(self.set, self.scratch("missing"))
        for suffix in ("", ".sig"):
            (directory / (name + suffix)).unlink()
        with self.assertRaises(cache_lineage.GateError) as caught:
            self.reference(directory)
        self.assertIn(f"missing asset {name}", str(caught.exception))

    def test_command_line_reference_from_lineage(self):
        out = self.scratch("cli") / "reference.json"
        arguments = ["reference-from-lineage", "--platform", "Mac",
                     "--previous-set", str(self.set), "--out", str(out),
                     "--openssl-cross-check", "--openssl", self.fixture.openssl,
                     *self.fixture.trust_arguments()]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cache_lineage.main(arguments), 0)
        reference = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(reference["source"], "lineage")
        self.assertEqual(reference["manifest_sha256"],
                         self.gate_lineage("Mac")["manifest_sha256"])
        directory = fixtures.copy_set(self.set, self.scratch("cli-tampered"))
        signature = directory / (formats.cache_lineage_asset("Mac") + ".sig")
        signature.write_bytes(bytes(64))
        arguments[arguments.index("--previous-set") + 1] = str(directory)
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(cache_lineage.main(arguments), 1)
        self.assertIn("does not verify", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
