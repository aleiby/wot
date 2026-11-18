#!/usr/bin/env python3
"""Tests for compare_guarded_builds.py over synthetic finished ISOs."""
from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_guarded_builds as cmp  # noqa: E402


def _write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


class CompareTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.base = Path(self._tmp.name)

    def make_iso(self, name: str, *, exe=b"exe-a", ucas=b"ucas",
                 bootstrap=b"bootstrap", shell_hash="1" * 64,
                 entries=None, manifest_time="2026-01-01T00:00:00Z",
                 definitions=None, extra_member=None, mode="clean-iso-v1"):
        iso = self.base / name
        _write_json(iso / ".stamps" / "01-seed.json", {
            "step": "seed", "release_commit": "abc123456",
            "source_snapshot": {"sha256": "5" * 64}})
        _write_json(iso / ".stamps" / "04-cook.json", {
            "step": "cook", "platform": "Win64", "maps": ["/Game/M"],
            "neutral_support_authority": {"a": 1},
            "reference_closure": {"packages": ["/Game/M"],
                                  "closure_sha256": "c" * 64},
            "compile_cache": {"mode": mode}})
        members = {
            "WoT/Binaries/Win64/WoT-Win64-Test.exe": exe,
            "WoT/Binaries/Win64/WoT-Win64-Test.pdb": exe + b"pdb",
            "WoT/Content/Paks/WoT-Windows.ucas": ucas,
            "WoT/Content/Paks/WoT-Windows.utoc": b"utoc",
            "Engine/Config/StagedBuild_WoT.ini": b"[x]",
            "Manifest_UFSFiles_Win64.txt":
                f"WoT/a.uasset\t{manifest_time}\n".encode(),
        }
        if extra_member:
            members[extra_member] = b"extra"
        asset = "WoT-Win64-guarded-abc123456.zip"
        release = iso / "release"
        release.mkdir(parents=True)
        with zipfile.ZipFile(release / asset, "w") as archive:
            archive.writestr("WoT Reborn/WoT Reborn.exe", b"MZ launcher")
            for member, data in members.items():
                archive.writestr("WoT Reborn/Game/" + member, data)
        compiled = {"format": "wot-guarded-build-receipt-v1",
                    "definitions": definitions or {"WOT_GUARDED_BOOTSTRAP": "1"},
                    "checks": ["x"], "configuration": "Test",
                    "target": "WoT", "platform": "Win64",
                    "modules_inspected": 9, "engine": {"commit": "e"}}
        _write_json(release / "release.json", {
            "format": "wot-guarded-release-v1", "configuration": "Test",
            "platform": "Win64", "git_commit": "abc123456", "asset": asset,
            "source_snapshot": {"sha256": "5" * 64},
            "engine_identity": {"commit": "e"},
            "neutral_support_authority": {"a": 1},
            "target_receipts": {
                target: {"compiled_receipt": dict(compiled, target=target)}
                for target in cmp.TARGETS},
            "cooked_container_inventory": {"receipt": {
                "paths": ["../../../WoT/Content/M.umap"],
                "validation": {"retail_derived_count": 0}}},
        })
        rows = entries or {
            ("WoT-Windows.utoc",
             "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap"): "aa",
            ("WoT-Windows.utoc", "../../../WoT/Content/M.umap"): "bb",
            ("WoT-Windows.pak", "WoT/AssetRegistry.bin"): "cc",
        }
        lines = []
        for container in sorted({key[0] for key in rows}):
            lines.append(f"COMMAND UnrealPak.exe G:/x/{container} -List")
            for (owner, entry), digest in sorted(rows.items()):
                if owner == container:
                    lines.append(
                        f'LogIoStore: Display: "{entry}" offset: 0, size: 1 '
                        f"bytes, hash: {digest}, compression: Oodle.")
        log = iso / "logs" / "cooked-container-inventory.log"
        log.parent.mkdir(parents=True)
        log.write_text("\n".join(lines) + "\n")
        bootstrap_path = iso / "Content" / "WoT" / "Maps" / "WoTBootstrap.umap"
        bootstrap_path.parent.mkdir(parents=True)
        bootstrap_path.write_bytes(bootstrap)
        _write_json(iso / "Saved" / "ImportReceipts"
                    / "neutral-shells-output-identity.json",
                    {"packages": [{"relative_path": "WoTMapShell01.umap",
                                   "sha256": shell_hash}]})
        return iso

    def test_images_and_manifest_timestamps_are_expected_differences(self):
        baseline = self.make_iso("base")
        candidate = self.make_iso(
            "cand", exe=b"exe-b", manifest_time="2027-02-02T00:00:00Z",
            mode="build-root-v1")
        report = cmp.compare(baseline, candidate)
        self.assertEqual(report["verdict"], "equivalent", report["unexpected"])
        classes = {row["item"]: row["class"] for row in report["rows"]}
        self.assertEqual(
            classes["zip member WoT/Binaries/Win64/WoT-Win64-Test.exe"],
            "image")
        self.assertEqual(classes["zip member Manifest_UFSFiles_Win64.txt"],
                         "staging-manifest")
        self.assertEqual(
            classes["zip member Engine/Config/StagedBuild_WoT.ini"],
            "identical")
        self.assertEqual(classes["compile_cache.mode"], "mode")

    def test_regenerated_bootstrap_explains_its_cooked_entries(self):
        baseline = self.make_iso("base")
        entries = {
            ("WoT-Windows.utoc",
             "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap"): "ab",
            ("WoT-Windows.utoc", "../../../WoT/Content/M.umap"): "bb",
            ("WoT-Windows.pak", "WoT/AssetRegistry.bin"): "cd",
        }
        candidate = self.make_iso("cand", ucas=b"ucas-2",
                                  bootstrap=b"bootstrap-2", entries=entries)
        report = cmp.compare(baseline, candidate)
        self.assertEqual(report["verdict"], "equivalent", report["unexpected"])
        classes = {row["item"]: row["class"] for row in report["rows"]}
        self.assertEqual(
            classes["zip member WoT/Content/Paks/WoT-Windows.ucas"],
            "regenerated")

    def test_unexplained_cooked_or_receipt_differences_fail(self):
        baseline = self.make_iso("base")
        cases = {
            "cooked package": dict(ucas=b"ucas-2", entries={
                ("WoT-Windows.utoc",
                 "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap"): "aa",
                ("WoT-Windows.utoc", "../../../WoT/Content/M.umap"): "zz",
                ("WoT-Windows.pak", "WoT/AssetRegistry.bin"): "cc"}),
            "same bootstrap source": dict(ucas=b"ucas-2", entries={
                ("WoT-Windows.utoc",
                 "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap"): "ff",
                ("WoT-Windows.utoc", "../../../WoT/Content/M.umap"): "bb",
                ("WoT-Windows.pak", "WoT/AssetRegistry.bin"): "cc"}),
            "definitions": dict(definitions={"WOT_GUARDED_BOOTSTRAP": "0"}),
            "extra member": dict(extra_member="WoT/Content/extra.bin"),
            "manifest content": dict(manifest_time="x\nWoT/b.uasset\ty"),
        }
        for index, (label, overrides) in enumerate(cases.items()):
            with self.subTest(label):
                candidate = self.make_iso(f"cand{index}", **overrides)
                report = cmp.compare(baseline, candidate)
                self.assertEqual(report["verdict"], "different")

    def test_different_sources_are_refused_unless_allowed(self):
        baseline = self.make_iso("base")
        candidate = self.make_iso("cand")
        seed = candidate / ".stamps" / "01-seed.json"
        value = json.loads(seed.read_text())
        value["release_commit"] = "def999999"
        seed.write_text(json.dumps(value))
        with self.assertRaisesRegex(cmp.CompareError, "different sources"):
            cmp.compare(baseline, candidate)
        report = cmp.compare(baseline, candidate,
                             allow_different_commits=True)
        self.assertIn(report["verdict"], {"equivalent", "different"})

    def test_main_writes_report_and_exit_code(self):
        baseline = self.make_iso("base")
        candidate = self.make_iso("cand", exe=b"exe-b")
        report_path = self.base / "report.json"
        self.assertEqual(cmp.main([
            "--baseline", str(baseline), "--candidate", str(candidate),
            "--report", str(report_path)]), 0)
        self.assertEqual(json.loads(report_path.read_text())["verdict"],
                         "equivalent")
        self.assertEqual(cmp.main([
            "--baseline", str(baseline),
            "--candidate", str(self.base / "missing")]), 1)


if __name__ == "__main__":
    unittest.main()
