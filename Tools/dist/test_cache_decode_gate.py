#!/usr/bin/env python3
"""Specs for cache_decode_gate.py: input selection, the working copy, the
program run and every decode-receipt rule.

The guarded program is replaced by a fake .py program (run through this
interpreter, as on Windows a .py has no shebang). It reads its behaviour from
the JSON beside it, records its argv, counts the copied manifest's schemas and
writes the decode receipt the real spec would, then applies the plan's damage.
"""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_decode_gate as gate  # noqa: E402
import cache_lineage  # noqa: E402
import release_test_fixtures as fixtures  # noqa: E402

FAKE_PROGRAM = r'''
import argparse
import hashlib
import json
import sys
import time
from collections import Counter
from pathlib import Path

plan = json.loads(Path(__file__).with_suffix(".json").read_text(encoding="utf-8"))
parser = argparse.ArgumentParser()
parser.add_argument("--suite", required=True)
parser.add_argument("--cache-root", required=True)
parser.add_argument("--decode-receipt", required=True)
args, rest = parser.parse_known_args()
receipt_path = Path(args.decode_receipt)
(receipt_path.parent / "argv.json").write_text(json.dumps(sys.argv[1:]), encoding="utf-8")
if plan.get("sleep"):
    time.sleep(plan["sleep"])
if plan.get("write", True):
    manifest_path = Path(args.cache_root) / "current" / "manifest.json"
    raw = manifest_path.read_bytes()
    counts = Counter(blob["schema"] for blob in json.loads(raw)["blobs"])
    per_schema = {schema: {"decoded": count, "manifest": count}
                  for schema, count in counts.items()}
    for schema, delta in plan.get("decoded_delta", {}).items():
        per_schema[schema]["decoded"] += delta
    for schema, delta in plan.get("manifest_delta", {}).items():
        per_schema[schema]["manifest"] += delta
    for schema in plan.get("extra_schemas", []):
        per_schema[schema] = {"decoded": 1, "manifest": 1}
    receipt = {
        "configuration": "Development",
        "contract_sha256": "1" * 64,
        "failures": plan.get("failures", []),
        "format": plan.get("format", "wot-cache-decode-receipt-v1"),
        "guard": plan.get("guard", 1),
        "installer_sha256": "2" * 64,
        "label": "3" * 64,
        "manifest_sha256": plan.get("manifest_sha256")
            or hashlib.sha256(raw).hexdigest(),
        "per_schema": per_schema,
        "platform": plan.get("platform", "Win64"),
        "seconds": 4,
    }
    encoded = json.dumps(receipt, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True)
    if plan.get("pretty"):
        encoded = json.dumps(receipt, sort_keys=True, indent=1)
    receipt_path.write_text(encoded, encoding="utf-8")
sys.exit(plan.get("exit", 0))
'''

MANIFEST = {
    "blobs": [
        {"logical_id": "cache/font/Engine__BigFont", "satisfies": ["f"],
         "schema": "wot-font-v3", "sha256": "4" * 64, "size": 3},
        {"logical_id": "cache/sound/A__B", "satisfies": ["s"],
         "schema": "wot-sound-v3", "sha256": "5" * 64, "size": 2},
        {"logical_id": "cache/texture/A__B", "satisfies": ["t"],
         "schema": "wot-texture-v3", "sha256": "6" * 64, "size": 1},
        {"logical_id": "cache/texture/A__C", "satisfies": ["t"],
         "schema": "wot-texture-v3", "sha256": "7" * 64, "size": 1},
    ],
    "format": "wot-payload-cache",
}


def _repo_tmp() -> Path:
    """The ignored .tmp folder in the checkout (kept off the system temp
    volume); a fresh clone does not have it yet."""
    path = Path.cwd() / ".tmp"
    path.mkdir(exist_ok=True)
    return path


def _canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("ascii")


class CacheDecodeGateTests(unittest.TestCase):
    PLATFORM = "Win64"

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="cache-decode-gate-", dir=_repo_tmp()))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.program = self.root / "fake_program.py"
        self.program.write_text(FAKE_PROGRAM, encoding="utf-8")
        self.plan({})

    def plan(self, plan: dict) -> None:
        self.program.with_suffix(".json").write_text(json.dumps(plan), encoding="utf-8")

    def cache(self, name: str, manifest: dict) -> tuple:
        """A cache directory holding current/manifest.json and one blob."""
        directory = self.root / name
        raw = _canonical(manifest)
        if not directory.exists():
            (directory / "current" / "blobs").mkdir(parents=True)
            (directory / "current" / "manifest.json").write_bytes(raw)
            (directory / "current" / "blobs" / "x.blob").write_bytes(b"blob")
        return directory, hashlib.sha256(raw).hexdigest()

    def run_explicit(self, *, work: str = "work", expect=None, cache=None, **extra):
        directory, digest = cache or self.cache("input", MANIFEST)
        return gate.run_gate(
            platform=self.PLATFORM, program=self.program, work_root=self.root / work,
            input_cache=directory, expect_manifest_sha256=expect or digest, **extra)

    def refused(self, fragment: str, plan: dict, **options) -> str:
        """Each refusal runs in its own new work root unless one is named."""
        self.plan(plan)
        self.runs = getattr(self, "runs", 0) + 1
        options.setdefault("work", f"refused-{self.runs}")
        with self.assertRaises(gate.GateError) as caught:
            self.run_explicit(**options)
        self.assertIn(fragment, str(caught.exception))
        return str(caught.exception)

    # --- a good run ---------------------------------------------------------

    def test_a_good_receipt_passes(self):
        directory, digest = self.cache("input", MANIFEST)
        receipt = self.run_explicit(cache=(directory, digest))
        self.assertEqual(receipt["format"], "wot-cache-gate-b-v1")
        self.assertEqual(receipt["verdict"], "pass")
        self.assertEqual(receipt["platform"], self.PLATFORM)
        self.assertEqual(receipt["input"], "explicit")
        self.assertEqual(receipt["manifest_sha256"], digest)
        self.assertEqual(receipt["per_schema"], {
            "wot-font-v3": {"decoded": 1, "manifest": 1},
            "wot-sound-v3": {"decoded": 1, "manifest": 1},
            "wot-texture-v3": {"decoded": 2, "manifest": 2},
        })
        self.assertEqual(receipt["program_sha256"],
                         hashlib.sha256(self.program.read_bytes()).hexdigest())
        self.assertIsInstance(receipt["seconds"], int)
        self.assertEqual(set(receipt), {"format", "verdict", "platform", "input",
                                        "manifest_sha256", "per_schema", "seconds",
                                        "program_sha256"})
        work = (self.root / "work").resolve()
        argv = json.loads((work / "argv.json").read_text(encoding="utf-8"))
        self.assertEqual(argv, [
            "--suite", "WoT.GuardedEngine.CacheDecode",
            "--cache-root", str(work / "ContentCache"),
            "--decode-receipt", str(work / "decode-receipt.json"),
            "-nullrhi", "-nosplash", "-unattended"])
        # The program read a real copy; the evidence stays as it was.
        copied = work / "ContentCache" / "current"
        for relative in ("manifest.json", "blobs/x.blob"):
            self.assertEqual((copied / relative).read_bytes(),
                             (directory / "current" / relative).read_bytes())
            self.assertEqual(os.stat(copied / relative).st_nlink, 1)
            self.assertFalse((copied / relative).is_symlink())
        self.assertEqual(sorted(path.name for path in directory.iterdir()), ["current"])

    def test_the_cli_writes_pass_and_fail_receipts(self):
        directory, digest = self.cache("input", MANIFEST)
        out = self.root / "gate-b.json"
        with contextlib.redirect_stdout(io.StringIO()):
            code = gate.main(["run", "--platform", self.PLATFORM, "--program",
                              str(self.program), "--work-root",
                              str(self.root / "work"), "--input-cache",
                              str(directory), "--expect-manifest-sha256", digest,
                              "--out", str(out)])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["verdict"], "pass")
        self.plan({"guard": 0})
        with contextlib.redirect_stderr(io.StringIO()):
            code = gate.main(["run", "--platform", self.PLATFORM, "--program",
                              str(self.program), "--work-root",
                              str(self.root / "work2"), "--input-cache",
                              str(directory), "--expect-manifest-sha256", digest,
                              "--out", str(out)])
        self.assertEqual(code, 1)
        failed = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(failed["verdict"], "fail")
        self.assertEqual(failed["format"], "wot-cache-gate-b-v1")
        self.assertIn("guard", failed["reason"])

    # --- receipt rules ------------------------------------------------------

    def test_guard_zero_fails(self):
        self.refused("only a guarded", {"guard": 0})
        self.refused("only a guarded", {"guard": True})

    def test_a_count_mismatch_fails(self):
        self.refused("wot-texture-v3: the manifest holds 2 blobs",
                     {"decoded_delta": {"wot-texture-v3": -1}})
        self.refused("wot-sound-v3: the manifest holds 1 blobs",
                     {"manifest_delta": {"wot-sound-v3": 1}})

    def test_an_unknown_schema_fails(self):
        self.refused("unknown schemas ['wot-music-v3']",
                     {"extra_schemas": ["wot-music-v3"]})
        manifest = json.loads(json.dumps(MANIFEST))
        manifest["blobs"][0]["schema"] = "wot-music-v3"
        self.refused("unknown schemas ['wot-music-v3']", {},
                     cache=self.cache("unknown", manifest))

    def test_a_schema_the_manifest_lacks_fails(self):
        self.refused("are not the manifest's", {"extra_schemas": ["wot-mesh-v3"]})

    def test_non_empty_failures_fail(self):
        self.refused("1 blobs failed to decode; first: 'cache/texture/A__B'", {
            "failures": [{"error": "truncated or invalid texture magic",
                          "logical_id": "cache/texture/A__B",
                          "schema": "wot-texture-v3"}]})

    def test_a_manifest_sha_mismatch_fails(self):
        # The copy is checked before the program runs ...
        self.refused("the copied manifest SHA-256", {}, expect="9" * 64, work="work")
        self.assertFalse((self.root / "work" / "argv.json").exists())
        # ... and the receipt must name the same manifest.
        self.refused("the decode receipt names manifest", {"manifest_sha256": "8" * 64})

    def test_a_wrong_platform_or_format_fails(self):
        self.refused("is for 'Mac'", {"platform": "Mac"})
        self.refused("is not wot-cache-decode-receipt-v1", {"format": "other"})
        self.refused("not canonical JSON", {"pretty": True})

    def test_a_missing_receipt_fails(self):
        self.refused("wrote no decode receipt", {"write": False})

    def test_a_failing_or_hung_program_fails(self):
        self.refused("the program exited 1", {"exit": 1})
        self.refused("did not finish within", {"sleep": 30}, timeout=1)

    def test_a_non_empty_work_root_is_refused(self):
        work = self.root / "work"
        work.mkdir()
        (work / "stale.txt").write_text("x", encoding="utf-8")
        self.refused("must be absent or empty", {}, work="work")
        self.assertFalse((work / "argv.json").exists())
        self.assertFalse((work / "ContentCache").exists())

    # --- selection ----------------------------------------------------------

    def gate_a(self, reference_manifest: dict, candidate_manifest: dict,
               compiled_revision: int) -> Path:
        receipt = cache_lineage.evaluate_gate(
            platform=self.PLATFORM,
            reference=fixtures.synthetic_reference(self.PLATFORM, reference_manifest),
            smoke_receipt=fixtures.synthetic_smoke_receipt(
                self.PLATFORM, "6" * 64, candidate_manifest),
            compiled_revision=compiled_revision)
        path = self.root / f"gate-a-{compiled_revision}.json"
        path.write_text(json.dumps(receipt), encoding="utf-8")
        return path

    def run_selected(self, receipt: Path, previous: Path, candidate: Path, work: str):
        return gate.run_gate(platform=self.PLATFORM, program=self.program,
                             work_root=self.root / work, gate_a_receipt=receipt,
                             previous_cache=previous, candidate_cache=candidate)

    def test_an_unchanged_identity_decodes_the_previous_cache(self):
        manifest = fixtures.synthetic_cache_manifest(1)
        previous, digest = self.cache("previous", manifest)
        candidate, _ = self.cache("candidate", fixtures.synthetic_cache_manifest(
            1, blob_sha256="4" * 64))
        receipt = self.run_selected(self.gate_a(manifest, manifest, 1),
                                    previous, candidate, "work")
        self.assertEqual(receipt["input"], "previous")
        self.assertEqual(receipt["manifest_sha256"], digest)
        self.assertEqual(
            (self.root / "work" / "ContentCache" / "current" / "manifest.json").read_bytes(),
            (previous / "current" / "manifest.json").read_bytes())

    def test_a_changed_identity_decodes_the_candidate_cache(self):
        reference = fixtures.synthetic_cache_manifest(1)
        changed = fixtures.synthetic_cache_manifest(2, blob_sha256="4" * 64)
        previous, _ = self.cache("previous", reference)
        candidate, digest = self.cache("candidate", changed)
        receipt = self.run_selected(self.gate_a(reference, changed, 2),
                                    previous, candidate, "work")
        self.assertEqual(receipt["input"], "candidate")
        self.assertEqual(receipt["manifest_sha256"], digest)
        self.assertEqual(
            (self.root / "work" / "ContentCache" / "current" / "manifest.json").read_bytes(),
            (candidate / "current" / "manifest.json").read_bytes())

    def test_the_wrong_cache_for_the_selection_fails(self):
        manifest = fixtures.synthetic_cache_manifest(1)
        other = fixtures.synthetic_cache_manifest(1, blob_sha256="4" * 64)
        previous, _ = self.cache("previous", other)
        candidate, _ = self.cache("candidate", manifest)
        with self.assertRaises(gate.GateError) as caught:
            self.run_selected(self.gate_a(manifest, manifest, 1),
                              previous, candidate, "work")
        self.assertIn("the copied manifest SHA-256", str(caught.exception))

    def test_a_failed_or_foreign_gate_a_receipt_is_refused(self):
        manifest = fixtures.synthetic_cache_manifest(1)
        previous, _ = self.cache("previous", manifest)
        failed = self.root / "failed.json"
        failed.write_text(json.dumps({"format": cache_lineage.GATE_FORMAT,
                                      "platform": self.PLATFORM, "reason": "x",
                                      "verdict": "fail"}), encoding="utf-8")
        win64_receipt = self.gate_a(manifest, manifest, 1)
        # A failed receipt, and a passing receipt for another platform.
        for platform, receipt, reason in ((self.PLATFORM, failed, "verdict is 'fail'"),
                                          ("Mac", win64_receipt, "is for 'Win64'")):
            with self.assertRaises(gate.GateError) as caught:
                gate.run_gate(platform=platform, program=self.program,
                              work_root=self.root / "work", gate_a_receipt=receipt,
                              previous_cache=previous, candidate_cache=previous)
            self.assertIn("the gate A receipt is not usable", str(caught.exception))
            self.assertIn(reason, str(caught.exception))
        self.assertFalse((self.root / "work").exists())

    def test_the_two_input_modes_are_exclusive_and_complete(self):
        directory, digest = self.cache("input", MANIFEST)
        for options in (
                {},
                {"input_cache": directory},
                {"expect_manifest_sha256": digest},
                {"gate_a_receipt": self.root / "x.json", "previous_cache": directory},
                {"input_cache": directory, "expect_manifest_sha256": digest,
                 "previous_cache": directory}):
            with self.assertRaises(gate.GateError) as caught:
                gate.run_gate(platform=self.PLATFORM, program=self.program,
                              work_root=self.root / "work", **options)
            self.assertIn("give either --gate-a-receipt", str(caught.exception))
        with self.assertRaises(gate.GateError) as caught:
            self.run_explicit(expect="ABC")
        self.assertIn("is not 64 lower-case hex", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
