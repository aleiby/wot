#!/usr/bin/env python3
"""Tests for the shared private-reference and machine-local configuration."""
from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from Tools import wot_config


class ReferenceRootTests(unittest.TestCase):
    def test_explicit_root_precedes_environment_and_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            explicit = Path(temporary) / "explicit"
            with mock.patch.dict(os.environ, {"WOT_REFERENCE_ROOT": "/environment"}):
                self.assertEqual(wot_config.reference_root(explicit), explicit.resolve())

    def test_environment_precedes_repository_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with mock.patch.dict(os.environ, {"WOT_REFERENCE_ROOT": str(root)}):
                self.assertEqual(wot_config.reference_root(), root.resolve())

    def test_repository_is_the_fallback(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(wot_config.reference_root(), wot_config.REPOSITORY_ROOT)

    def test_directory_accessors_share_the_selected_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("original", "devtree", "exports"):
                (root / name).mkdir()
            self.assertEqual(wot_config.original_dir(root), (root / "original").resolve())
            self.assertEqual(wot_config.devtree_dir(root), (root / "devtree").resolve())
            self.assertEqual(wot_config.exports_dir(root), (root / "exports").resolve())

    def test_missing_requested_input_names_reference_root_setting(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(wot_config.ConfigurationError,
                                        "WOT_REFERENCE_ROOT.*original"):
                wot_config.original_dir(Path(temporary))


class LocalConfigurationTests(unittest.TestCase):
    def write_config(self, root: Path, document: object) -> Path:
        path = root / "local.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def test_local_json_path_value_is_loaded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            configured = root / "source-engine"
            path = self.write_config(root, {"source_engine_root_mac": str(configured)})
            with mock.patch.dict(os.environ, {}, clear=True):
                self.assertEqual(
                    wot_config.local_path("source_engine_root_mac", config_path=path),
                    configured.resolve())

    def test_environment_overrides_local_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = self.write_config(root, {"scratch_root": str(root / "configured")})
            overridden = root / "overridden"
            with mock.patch.dict(os.environ, {"WOT_SCRATCH_ROOT": str(overridden)}):
                self.assertEqual(
                    wot_config.local_path("scratch_root", config_path=path),
                    overridden.resolve())

    def test_relative_local_path_is_repository_relative_not_cwd_relative(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = self.write_config(root, {"scratch_root": "local/scratch"})
            unrelated = root / "unrelated-cwd"
            unrelated.mkdir()
            with mock.patch.dict(os.environ, {}, clear=True), \
                    mock.patch("os.getcwd", return_value=str(unrelated)):
                self.assertEqual(
                    wot_config.local_path("scratch_root", config_path=path),
                    (wot_config.REPOSITORY_ROOT / "local/scratch").resolve())

    def test_missing_local_key_fails_only_when_requested(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write_config(Path(temporary), {"_comment": "example"})
            with self.assertRaisesRegex(wot_config.ConfigurationError,
                                        "scratch_root.*WOT_SCRATCH_ROOT.*local.json"):
                wot_config.local_path("scratch_root", config_path=path)

    def test_malformed_local_json_has_a_clear_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "local.json"
            path.write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(wot_config.ConfigurationError,
                                        "cannot parse.*local.json"):
                wot_config.load_local_config(path)


if __name__ == "__main__":
    unittest.main()
