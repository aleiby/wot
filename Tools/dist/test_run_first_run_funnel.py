#!/usr/bin/env python3
"""Unit tests for the closed first-run funnel harness and receipts."""
from __future__ import annotations

import ctypes
import contextlib
import copy
import hashlib
import http.client
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import unittest
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path, PurePosixPath
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_first_run_funnel as funnel
import release_test_fixtures as release_fixtures

# The fixtures follow the pipeline-owned closed neutral-support set.
NEUTRAL_PACKAGES = (*funnel.NEUTRAL_SUPPORT_ROOTS,
                    *funnel.NEUTRAL_SUPPORT_HARD_DEPENDENCIES)
# WoTBootstrap + WoTFrontEnd + 19 neutral shells + the support packages.
COOKED_PATH_COUNT = 21 + len(NEUTRAL_PACKAGES)
# Plugin descriptors in the census: the neutral-shell one plus Mac-style
# mount-rooted engine descriptors (Win64 stages none).
COOKED_DESCRIPTOR_PATHS = (
    "../../../WoT/Plugins/WoTNeutralShells/WoTNeutralShells.uplugin",
    "../../../Engine/Plugins/2D/Paper2D/Paper2D.uplugin",
    "../../../Engine/Plugins/Runtime/Metasound/Metasound.uplugin",
)
COOKED_ENGINE_DESCRIPTOR_COUNT = 2
COOKED_CENSUS_COUNT = COOKED_PATH_COUNT + len(COOKED_DESCRIPTOR_PATHS)


def _repo_tmp() -> Path:
    """The ignored .tmp folder in the checkout (kept off the system temp
    volume); a fresh clone does not have it yet."""
    path = Path.cwd() / ".tmp"
    path.mkdir(exist_ok=True)
    return path


class ReducedFunnelContractTests(unittest.TestCase):
    def test_front_end_evidence_requires_prior_ready_in_same_process_and_epoch(self):
        ready = {"event": "phase-ready", "phase": "Ready", "pid": 42, "epoch": 7}
        handoff = {**ready, "event": "frontend-handoff"}
        for rows in ([handoff], [handoff, ready],
                     [{**ready, "epoch": 6}, handoff],
                     [{**ready, "pid": 41}, handoff],
                     [{**ready, "phase": "StartingGame"}, handoff]):
            with self.subTest(rows=rows):
                self.assertIsNone(funnel.front_end_evidence(rows, 42))
        self.assertEqual(funnel.front_end_evidence([ready, handoff], 42),
                         {"ready": ready, "frontend_handoff": handoff, "epoch": 7})

    def test_platform_cell_matrices_are_the_retained_player_paths(self):
        self.assertEqual(funnel._cell_names("Win64"), (
            "fresh-first-run", "second-launch", "damaged-cache-rebuild",
            "downloads-pickup", "host-join", "update-apply",
            "update-bad-build-rolls-back", "interrupted-swap",
        ))
        self.assertEqual(funnel._cell_names("Mac"), (
            "fresh-first-run", "second-launch", "damaged-cache-rebuild",
            "downloads-pickup", "host-join", "update-apply",
            "update-bad-build-rolls-back", "interrupted-swap", "mac-uninstall",
        ))

    def test_boot_failure_poll_never_lists_the_content_cache(self):
        # Listing ContentCache while the game builds it holds a handle under
        # `building`, which makes Windows refuse the game's publish rename.
        with tempfile.TemporaryDirectory(dir=_repo_tmp()) as temp:
            state = Path(temp)
            receipt = state / "UpdaterState" / funnel.BOOT_FAILURE_LEAF
            receipt.parent.mkdir(parents=True)
            receipt.write_text("{}", encoding="utf-8")
            decoy = (state / "ContentCache" / "building" / "blobs"
                     / funnel.BOOT_FAILURE_LEAF)
            decoy.parent.mkdir(parents=True)
            decoy.write_text("{}", encoding="utf-8")
            listed = []
            real_walk = os.walk

            def recording_walk(top, *args, **kwargs):
                for directory, subdirectories, files in real_walk(top, *args, **kwargs):
                    listed.append(Path(directory))
                    yield directory, subdirectories, files

            with mock.patch.object(funnel.os, "walk", recording_walk):
                self.assertEqual(funnel._boot_failures(state), [receipt])
            self.assertFalse(any("ContentCache" in path.parts for path in listed),
                             listed)
            self.assertEqual(funnel._boot_failures(state / "absent"), [])

    def test_process_heartbeat_reader_uses_only_new_complete_valid_records(self):
        with tempfile.TemporaryDirectory(dir=_repo_tmp()) as temp:
            state = Path(temp)
            timestamp = datetime.now(timezone.utc).isoformat()
            heartbeat_root, before = funnel._heartbeat_launch_boundary(state)
            heartbeat_root.mkdir(parents=True)
            stale = heartbeat_root / "bootstrap-heartbeat-v1.42.stale.jsonl"
            stale.write_text(
                json.dumps({"epoch": 1, "event": "phase-ready",
                            "phase": "Ready", "pid": 42,
                            "ts_utc": timestamp}) + "\n",
                encoding="utf-8")
            heartbeat_root, before = funnel._heartbeat_launch_boundary(state)
            current = heartbeat_root / "bootstrap-heartbeat-v1.42.current.jsonl"
            rows = [
                {"epoch": 7, "event": "phase-begin",
                 "phase": "CheckingOriginalGameData", "pid": 42,
                 "ts_utc": timestamp},
                {"epoch": 8, "event": "phase-ready",
                 "phase": "Ready", "pid": 42, "ts_utc": timestamp},
            ]
            current.write_bytes(
                (json.dumps(rows[0]) + "\n").encode()
                + b"{malformed}\n"
                + (json.dumps({**rows[0], "pid": 99}) + "\n").encode()
                + (json.dumps(rows[1]) + "\n").encode()
                + b'{"epoch":8,"event":"frontend-handoff"')
            foreign = heartbeat_root / "bootstrap-heartbeat-v1.99.other.jsonl"
            foreign.write_text(json.dumps({**rows[0], "pid": 99}) + "\n")

            self.assertEqual(
                funnel._read_heartbeat_records(
                    heartbeat_root, before, expected_pid=42), rows)
            self.assertEqual(
                funnel._read_heartbeat_records(
                    heartbeat_root, before, expected_pid=42,
                    expected_epoch=8), [rows[1]])

            malformed = heartbeat_root / "bootstrap-heartbeat-v1.1.bad.jsonl"
            invalid_rows = [
                {**rows[0], "pid": True},
                {**rows[0], "pid": 1, "epoch": True},
                {**rows[0], "pid": 1, "epoch": -1},
                {**rows[0], "pid": 1, "event": ""},
                {**rows[0], "pid": 1, "phase": ""},
                {**rows[0], "pid": 1, "ts_utc": "not-a-timestamp"},
                {**rows[0], "pid": 1, "ts_utc": "2026-10-15T12:00:00"},
                {**rows[0], "pid": 1, "detail": 1},
                {**rows[0], "pid": 1, "completed": True},
                {**rows[0], "pid": 1, "total": -1},
            ]
            malformed.write_text(
                "".join(json.dumps(row) + "\n" for row in invalid_rows),
                encoding="utf-8")
            self.assertEqual(funnel._read_heartbeat_records(
                heartbeat_root, before, expected_pid=1), [])

    def test_process_heartbeat_reader_accepts_epoch_zero_world_events(self):
        with tempfile.TemporaryDirectory(dir=_repo_tmp()) as temp:
            state = Path(temp)
            root, before = funnel._heartbeat_launch_boundary(state)
            root.mkdir(parents=True)
            event = {"epoch": 0, "event": "world-hosted",
                     "phase": "Ready", "pid": 42,
                     "ts_utc": datetime.now(timezone.utc).isoformat()}
            (root / "bootstrap-heartbeat-v1.42.host.jsonl").write_text(
                json.dumps(event) + "\n", encoding="utf-8")
            self.assertEqual(funnel._read_heartbeat_records(
                root, before, expected_pid=42, expected_epoch=0), [event])

    def test_cached_handoff_over_three_seconds_warns_without_passing_mutation(self):
        started = datetime.now(timezone.utc)
        def row(event, phase, seconds, epoch):
            return {
                "epoch": epoch, "event": event, "phase": phase, "pid": 42,
                "ts_utc": (started + timedelta(seconds=seconds)).isoformat(),
            }
        records = [
            row("phase-begin", "CheckingOriginalGameData", 0, 1),
            row("phase-ready", "Ready", 2, 2),
            row("frontend-handoff", "Ready", 4, 2),
        ]
        receipt = funnel._cached_handoff_receipt(
            records, expected_pid=42,
            process_started_epoch=started.timestamp(), network_bytes=0,
            cache_writes=0, cold_cache_provenance={
                "classification": "warm-repeat",
                "evidence": "same-invocation-profile",
                "normal_reboot_utc": None,
            })
        self.assertEqual(receipt["latency_warning"],
                         "frontend-handoff exceeded 3.0 seconds")
        with self.assertRaisesRegex(funnel.FunnelFailure, "cache_writes"):
            funnel._cached_handoff_receipt(
                records, expected_pid=42,
                process_started_epoch=started.timestamp(), network_bytes=0,
                cache_writes=1, cold_cache_provenance={
                    "classification": "warm-repeat",
                    "evidence": "same-invocation-profile",
                    "normal_reboot_utc": None,
                })

    def test_damage_target_is_manifest_listed_and_startup_critical(self):
        manifest = {"blobs": [
            {"logical_id": "cache/world/lazy", "sha256": "1" * 64},
            {"logical_id": "cache/localization/WoT.int", "sha256": "2" * 64},
        ]}
        row = funnel._select_startup_critical_blob(
            manifest, ["cache/localization/WoT.int"])
        self.assertEqual(row, manifest["blobs"][1])


def _cpp_strict_test_loopback_url(candidate: str) -> bool:
    """Port of WoTParseStrictTestLoopbackUrl (WoTBootstrapCore TestOverrides.cpp)."""
    prefix = "http://127.0.0.1:"
    if (not candidate.startswith(prefix)
            or any(mark in candidate for mark in ("?", "#", "@", "\\"))):
        return False
    rest = candidate[len(prefix):]
    slash = rest.find("/")
    if slash <= 0 or slash == len(rest) - 1:
        return False
    port = rest[:slash]
    if not port.isdigit() or not 0 < int(port) <= 65535:
        return False
    return all(0x20 < ord(character) != 0x7f for character in candidate)


def _cpp_rewrite_release_url(original: str, base: str) -> str | None:
    """Port of WoTPlatformHttpHopWindowsRewriteReleaseUrl (Win64 Test transport)."""
    notes = ("https://github.com/aleiby/wot/releases/latest/download/"
             "WoT-release-notes-v1.json")
    prefix = "https://github.com/aleiby/wot/releases/download/"
    if original == notes:
        route = "/releases/latest/download/WoT-release-notes-v1.json"
    else:
        if not original.startswith(prefix):
            return None
        suffix = original[len(prefix):]
        separator = suffix.find("/")
        if (separator <= 0 or separator == len(suffix) - 1
                or "/" in suffix[separator + 1:]
                or "?" in suffix or "#" in suffix):
            return None
        route = "/releases/download/" + suffix
    if not _cpp_strict_test_loopback_url(base):
        return None
    return base.rstrip("/") + route


class ReleaseBaseContractTests(unittest.TestCase):
    """The game must route every signed release URL to a served asset (once,
    a path-less base failed the strict rule, so the game silently went to
    github.com and reported no update)."""

    def test_port_matches_the_cpp_spec_cases(self):
        self.assertFalse(_cpp_strict_test_loopback_url("http://127.0.0.1:8080"))
        self.assertFalse(_cpp_strict_test_loopback_url("http://127.0.0.1:8080/"))
        self.assertTrue(_cpp_strict_test_loopback_url(
            "http://127.0.0.1:8080/test-release-base"))
        self.assertEqual(
            _cpp_rewrite_release_url(
                "https://github.com/aleiby/wot/releases/download/v7/WoT-Win64.zip",
                "http://127.0.0.1:8080/test-release-base"),
            "http://127.0.0.1:8080/test-release-base/releases/download/v7/WoT-Win64.zip")

    def test_server_base_rewrites_release_urls_onto_registered_routes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            installer = root / "fixture.exe"
            installer.write_bytes(b"installer")
            asset = root / "WoT-Win64-release-manifest-v2.json"
            asset.write_bytes(b"{}")
            tag = "wot-funnel-win64-r1-good"
            route = (funnel.formats.FUNNEL_RELEASE_BASE_PATH
                     + f"/releases/download/{tag}/{asset.name}")
            with funnel.LoopbackInstallerServer(
                    installer, root / "requests.jsonl",
                    release_assets={route: asset}) as server:
                rewritten = _cpp_rewrite_release_url(
                    funnel.formats.release_download_url(tag, asset.name),
                    server.release_base_url)
                self.assertIsNotNone(rewritten, server.release_base_url)
                parsed = urllib.parse.urlsplit(rewritten)
                self.assertEqual(parsed.path, route)
                connection = http.client.HTTPConnection(
                    "127.0.0.1", server.server.server_port, timeout=5)
                connection.request("GET", parsed.path)
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(response.read(), b"{}")
                connection.close()

    def test_server_always_serves_exact_latest_notes_route(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            installer = root / "fixture.exe"
            installer.write_bytes(b"installer")
            with funnel.LoopbackInstallerServer(
                    installer, root / "requests.jsonl",
                    release_note_sequences=(2, 3)) as server:
                rewritten = _cpp_rewrite_release_url(
                    funnel.formats.release_notes_latest_url(),
                    server.release_base_url)
                self.assertIsNotNone(rewritten)
                parsed = urllib.parse.urlsplit(rewritten)
                self.assertEqual(parsed.path, funnel.RELEASE_NOTES_ROUTE)
                connection = http.client.HTTPConnection(
                    "127.0.0.1", server.server.server_port, timeout=5)
                connection.request("GET", parsed.path)
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                notes = funnel.formats.parse_release_notes(response.read())
                connection.close()
            self.assertEqual(
                [entry["release_sequence"] for entry in notes["notes"]],
                [2, 3])


class StagingRetryTests(unittest.TestCase):
    def _run(self, scripted):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            update_dir = root / ".wot-update"
            update_dir.mkdir()
            (update_dir / "ready").write_bytes(b"2\n")
            pids = iter(range(1, 10))
            calls = iter(scripted)
            launched = []

            def launch(*_args, **_kwargs):
                client = mock.Mock()
                client.process.pid = next(pids)
                client.terminate.return_value = {"forced": False, "returncode": 0}
                launched.append(client)
                return client

            def heartbeat(_path, _offset, _predicate, _timeout, _what):
                return next(calls)

            with mock.patch.object(funnel, "_installed_tree_root",
                                   return_value="root"),                     mock.patch.object(funnel, "_launch_reused_profile",
                                      side_effect=launch),                     mock.patch.object(funnel, "_resolve_executable",
                                      return_value=root / "game.exe"),                     mock.patch.object(funnel, "_available_port",
                                      return_value=1),                     mock.patch.object(funnel, "_wait_snapshot"),                     mock.patch.object(funnel, "_wait_heartbeat",
                                      side_effect=heartbeat),                     mock.patch.object(funnel, "_receipt_file_ref",
                                      return_value={}),                     mock.patch.object(funnel, "_process_evidence",
                                      side_effect=lambda r, role, pid, ready:
                                      {"pid": pid, "role": role}):
                stage = funnel._stage_update_candidate(
                    tree=root / "tree", profile=root / "profile",
                    update_dir=update_dir, server=mock.Mock(),
                    log=root / "log", clients=[], timeout=5, sequence=1,
                    process_observer=mock.Mock())
                retries = root / "log" / "staging-retries.json"
                return stage, (json.loads(retries.read_text())
                               if retries.is_file() else None), launched

    def test_an_intermittent_staging_failure_is_retried_and_logged(self):
        stage, retries, launched = self._run([
            [{"pid": 1, "event": "update-check-failed",
              "detail": "stage=download diagnostic=INSTALL-FAILED detail=x"},
             {"pid": 1, "event": "update-check-result",
              "detail": "download-failed"}],
            [{"pid": 2, "event": "update-staged", "detail": "sequence=2"},
             {"pid": 2, "event": "update-check-result",
              "detail": "ready status=Ready sequence=2 reason="}],
        ])
        self.assertEqual(stage["process"], {"pid": 2, "role": "A"})
        self.assertEqual(len(launched), 2)
        self.assertEqual(retries[0]["result"], "download-failed")
        self.assertIn("INSTALL-FAILED", retries[0]["failure"][0])

    def test_an_unexpected_result_is_not_retried(self):
        with self.assertRaisesRegex(funnel.FunnelFailure, "'none'"):
            self._run([[{"pid": 1, "event": "update-check-result",
                         "detail": "none"}]])


class GracefulQuitTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows graceful-quit escalation")
    def test_connected_client_gets_time_to_exit_before_tree_kill(self):
        client = funnel.ClientProcess.__new__(funnel.ClientProcess)
        connection = mock.Mock()
        client.connection = connection
        client.process = mock.Mock()
        client.process.poll.side_effect = [None, 0]
        client.process.wait.return_value = 0
        client.stdout_handle = mock.Mock(closed=False)
        client.stderr_handle = mock.Mock(closed=False)

        with mock.patch.object(funnel, "_instance_tree_kill") as tree_kill:
            result = client.terminate()

        connection.command.assert_called_once_with("quit")
        client.process.wait.assert_called_once_with(timeout=10)
        tree_kill.assert_not_called()
        self.assertFalse(result["forced"])
        self.assertEqual(result["signals"], [])


class InstalledTreeIdentityTests(unittest.TestCase):
    def test_root_is_canonical_and_ignores_runtime_saved_directories(self):
        with tempfile.TemporaryDirectory() as temp:
            tree = Path(temp) / "WoT-Win64"
            files = {"b.txt": b"b", "A/z.bin": b"z", "A/a.bin": b"a",
                     "Engine/Binaries/e.dll": b"e",
                     "WoT/Content/x.pak": b"x"}
            for relative, data in files.items():
                (tree / relative).parent.mkdir(parents=True, exist_ok=True)
                (tree / relative).write_bytes(data)
            entries = [funnel.formats.TreeEntry(path, "directory", 0o755, 0, "")
                       for path in ("A", "Engine", "Engine/Binaries", "WoT",
                                    "WoT/Content")]
            for relative, data in files.items():
                entries.append(funnel.formats.TreeEntry(
                    relative, "file", 0o644, len(data),
                    hashlib.sha256(data).hexdigest()))
            expected = funnel.formats.compute_installed_tree_root(
                "Win64", funnel.formats.sort_installed_tree_entries(entries))
            self.assertEqual(funnel._installed_tree_root(tree), expected)
            # What a launch leaves behind (measured on a real release) changes nothing.
            for runtime in ("Engine/Saved/Config/Windows/Manifest.ini",
                            "WoT/Saved/Logs/WoT.log"):
                (tree / runtime).parent.mkdir(parents=True, exist_ok=True)
                (tree / runtime).write_bytes(b"runtime")
            self.assertEqual(funnel._installed_tree_root(tree), expected)
            # A signed-looking path that merely starts with "Saved" still counts.
            (tree / "WoT" / "SavedGames.dat").write_bytes(b"s")
            self.assertNotEqual(funnel._installed_tree_root(tree), expected)


class ProcessEnumerationTests(unittest.TestCase):
    def test_system_idle_pid_zero_is_skipped(self):
        result = mock.Mock(returncode=0, stdout="[0,4,1234]", stderr="")
        with mock.patch.object(funnel.subprocess, "run", return_value=result):
            self.assertEqual(funnel._enumerate_win_process_ids(), [4, 1234])

    def test_negative_pid_is_still_refused(self):
        result = mock.Mock(returncode=0, stdout="[4,-1]", stderr="")
        with mock.patch.object(funnel.subprocess, "run", return_value=result):
            with self.assertRaises(funnel.FunnelFailure):
                funnel._enumerate_win_process_ids()


class AttemptTreeRemovalTests(unittest.TestCase):
    def test_removes_a_tree_holding_a_read_only_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "attempt"
            (root / "nested").mkdir(parents=True)
            locked = root / "nested" / "blob"
            locked.write_bytes(b"x")
            os.chmod(locked, 0o400)
            funnel._remove_attempt_tree(root)
            self.assertFalse(root.exists())


class FunnelHarnessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    @staticmethod
    def _sha(value: bytes) -> str:
        return hashlib.sha256(value).hexdigest()


    def test_real_preferences_inventory_hashes_small_files_and_detects_change(self):
        primary = self.root / "real" / "WoT"
        epic = self.root / "real" / "Epic" / "WoT"
        primary.mkdir(parents=True)
        preference = primary / "WoTSetupBackdrop.png"
        preference.write_bytes(b"before")
        before = funnel._mac_real_support_inventory((primary, epic))
        file_row = next(
            row for row in before["roots"][0]["entries"]
            if row["name"] == "WoTSetupBackdrop.png")
        self.assertEqual(file_row["sha256"], self._sha(b"before"))
        self.assertFalse(before["roots"][1]["exists"])

        preference.write_bytes(b"after")
        after = funnel._mac_real_support_inventory((primary, epic))
        self.assertNotEqual(before, after)

    def test_real_preferences_inventory_refuses_symlink_root(self):
        real_root = self.root / "real" / "WoT"
        real_root.mkdir(parents=True)
        linked_root = self.root / "linked-WoT"
        linked_root.symlink_to(real_root, target_is_directory=True)
        with self.assertRaisesRegex(funnel.FunnelFailure, "symlink"):
            funnel._mac_real_support_inventory((linked_root,))

    def test_real_preferences_inventory_refuses_symlink_file(self):
        real_root = self.root / "real" / "WoT"
        real_root.mkdir(parents=True)
        target = self.root / "outside-preference"
        target.write_bytes(b"real preferences")
        (real_root / "WoTSetupBackdrop.png").symlink_to(target)
        with self.assertRaisesRegex(funnel.FunnelFailure, "symlink"):
            funnel._mac_real_support_inventory((real_root,))

    def test_real_preferences_inventory_refuses_symlink_directory(self):
        real_root = self.root / "real" / "WoT"
        real_root.mkdir(parents=True)
        target = self.root / "outside-directory"
        target.mkdir()
        (real_root / "nested").symlink_to(target, target_is_directory=True)
        with self.assertRaisesRegex(funnel.FunnelFailure, "symlink"):
            funnel._mac_real_support_inventory((real_root,))


    def make_release(
        self, *, guarded: bool = True, include_receipts: bool = True
    ) -> Path:
        release = self.root / "release"
        release.mkdir(exist_ok=True)
        asset = release / "artifact.zip"
        with zipfile.ZipFile(asset, "w") as archive:
            archive.writestr("WoT.app/Contents/MacOS/WoT-Mac-Test", b"game")
        product_receipt = {
            "format": funnel.PRODUCT_RECEIPT_FORMAT,
            "platform": "Mac",
            "configuration": "Test",
            "stage_root": "Mac",
            "products": {
                name: {
                    "path": f"Mac/{name}",
                    "size": 4,
                    "sha256": "1" * 64,
                    "compiled_receipt_sha256": "2" * 64,
                    "engine_receipt_sha256": "3" * 64,
                }
                for name in ("WoT",)
            },
        }
        snapshot = {
            "format": funnel.SOURCE_SNAPSHOT_FORMAT,
            "git_commit": "abcdef123",
            "paths": ["Source", "Tools"],
            "tracked_diff_size": 0,
            "tracked_diff_sha256": self._sha(b""),
            "untracked": [],
        }
        snapshot["sha256"] = self._sha(
            funnel._canonical_release_bytes(snapshot)
        )
        neutral_packages = list(NEUTRAL_PACKAGES)
        neutral_assets = [{
            "package": package,
            "relative_path": (
                "Content" + package.removeprefix("/Game") + ".uasset"),
            "sha256": self._sha(package.encode("utf-8")),
            "size": index + 1,
        } for index, package in enumerate(neutral_packages)]
        neutral_support = {
            "format": "wot-neutral-support-asset-receipt-v1",
            "authority_format": "wot-neutral-support-asset-authority-v1",
            "authority_relative_path":
                "Tools/dist/neutral-support-assets-v1.json",
            "authority_sha256": "0" * 64,
            "asset_count": len(NEUTRAL_PACKAGES),
            "roots": list(funnel.NEUTRAL_SUPPORT_ROOTS),
            "hard_dependencies": list(
                funnel.NEUTRAL_SUPPORT_HARD_DEPENDENCIES),
            "assets": neutral_assets,
            "assets_sha256": self._sha(
                funnel._canonical_release_bytes(neutral_assets)),
        }
        neutral_support["authority_sha256"] = self._sha(
            funnel._pretty_json_bytes({
                "format": neutral_support["authority_format"],
                "roots": neutral_support["roots"],
                "hard_dependencies": neutral_support["hard_dependencies"],
                "assets": neutral_support["assets"],
            }))
        zen_receipt = self._zen_receipt()
        cooked_receipt = self._cooked_receipt()
        manifest = {
            "format": (
                funnel.GUARDED_RELEASE_FORMAT
                if guarded else "wot-stripped-release-v1"
            ),
            "platform": "Mac",
            "configuration": "Test",
            "asset": asset.name,
            "size": asset.stat().st_size,
            "sha256": funnel._sha256_file(asset),
            "git_commit": snapshot["git_commit"],
            "release_sequence": 0,
            "source_snapshot": snapshot,
            "products": {
                "receipt": product_receipt,
                "receipt_sha256": self._sha(
                    funnel._pretty_json_bytes(product_receipt)
                ),
            },
            "audit": {
                "verdict": "pass",
                "staged_tree_receipt_sha256": "4" * 64,
                "staged_tree_sha256": "5" * 64,
            },
            "cooked_container_inventory": {
                "receipt_sha256": self._sha(
                    funnel._pretty_json_bytes(cooked_receipt)),
                "receipt": cooked_receipt,
                "path_count": COOKED_CENSUS_COUNT,
                "validation": {
                    "project_content_count": COOKED_PATH_COUNT,
                    "retail_derived_count": 0,
                    "neutral_map_count": 21,
                    "neutral_support_package_count": len(NEUTRAL_PACKAGES),
                    "neutral_shell_plugin_descriptor_count": 1,
                    "engine_plugin_descriptor_count": COOKED_ENGINE_DESCRIPTOR_COUNT,
                },
            },
            "neutral_support_authority": neutral_support,
            "target_receipts": {
                name: self._target_pair()
                for name in ("WoT",)
            },
            "engine_identity": {"commit": "engine"},
            "generated_utc": "2026-07-29T00:00:00+00:00",
            "zen_lifetime": {
                "receipt": zen_receipt,
                "receipt_sha256": self._sha(
                    funnel._pretty_json_bytes(zen_receipt)
                ),
            },
        }
        if not include_receipts:
            del manifest["target_receipts"]
        path = release / "release.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return path

    def test_refuses_loose_alternate_bind_artifact_in_release_archive(self):
        for member, content in (
                ("WoT-Win64/Shipping/Mesh.GLB", b"renamed"),
                ("WoT-Win64/Shipping/Mesh.META.JSON", b"metadata"),
                ("WoT-Win64/Shipping/AUTHENTICATED-ALTERNATE-BIND.JSON",
                 b"receipt"),
                ("WoT-Win64/Shipping/renamed.bin", b"glTF" + b"\0" * 8),
                ("WoT-Win64/Shipping/proof.bin",
                 b"wot-authenticated-alternate-bind-v1")):
            release = self.make_release()
            manifest = json.loads(release.read_text())
            asset = release.parent / manifest["asset"]
            with zipfile.ZipFile(asset, "a") as archive:
                archive.writestr(member, content)
            manifest["size"] = asset.stat().st_size
            manifest["sha256"] = funnel._sha256_file(asset)
            release.write_text(json.dumps(manifest))
            with self.subTest(member=member), self.assertRaisesRegex(
                    funnel.FunnelFailure, "forbidden alternate-bind"):
                funnel._resolve_release(release, require_current_source=False)

    def _target_pair(self) -> dict:
        compiled = {"format": "compiled"}
        engine = {"format": "engine"}
        return {
            "compiled_receipt": compiled,
            "compiled_receipt_sha256": self._sha(
                funnel._pretty_json_bytes(compiled)
            ),
            "engine_receipt": engine,
            "engine_receipt_sha256": self._sha(
                funnel._pretty_json_bytes(engine)
            ),
        }

    def _zen_receipt(self) -> dict:
        roots = "+".join(funnel.NEUTRAL_SUPPORT_ROOTS)
        rows = []
        sponsor_command = [
            "Engine/Binaries/Mac/zenserver", "--port", "18558",
            "--allow-port-probing=false", "--http-forceloopback",
            "--data-dir", "Saved/GuardedZenSponsor-123",
            "--owner-pid", "123", "--detach=false", "--no-sentry", "--quiet",
        ]
        for stage, kind in (
                ("import", "editor"),
                ("finalize-stripped-fractal-catalog", "editor"),
                ("generate-neutral-map-shells", "editor"),
                ("cook", "cook")):
            command = (
                ["RunUAT", "-AdditionalCookerOptions=-skipzenstore "
                 "-ddc=(ZenLocal) -NoZenAutoLaunch=127.0.0.1:18558 "
                 f"-PACKAGE={roots}"]
                if kind == "cook" else
                ["UnrealEditor-Cmd", "-ddc=(ZenLocal)",
                 "-NoZenAutoLaunch=127.0.0.1:18558"])
            rows.append({
                "stage": stage,
                "kind": kind,
                "command": command,
                "log": f"logs/{stage}.log",
                "log_sha256": "b" * 64,
                "exit_code": 0,
                "stripped_ddc_environment_overrides": [],
            })
        return {
            "format": "wot-guarded-zen-lifetime-v1",
            "mechanism": "pipeline-sponsored-zen-v3",
            "unreal_isolation_switches": [
                "-ddc=(ZenLocal)",
                "-NoZenAutoLaunch=127.0.0.1:18558"],
            "endpoint": "http://127.0.0.1:18558",
            "owner": {"kind": "pipeline-process", "pid": 123},
            "sponsor": {
                "pid": 321, "command": sponsor_command,
                "log": "logs/zen-sponsor.log"},
            "version": {"path": "Engine/Binaries/Mac/zenserver", "size": 1,
                        "sha256": "a" * 64},
            "before": [],
            "endpoint_preflight": {
                "host": "127.0.0.1", "port": 18558,
                "exclusive_bind_succeeded": True,
                "checked_utc": "2026-07-29T00:00:00+00:00"},
            "health": {
                "url": "http://127.0.0.1:18558/health/ready",
                "status_code": 200, "body_sha256": "c" * 64,
                "checked_utc": "2026-07-29T00:00:01+00:00"},
            "processes_after_lifecycle": [
                {"pid": 321, "command": " ".join(sponsor_command)}],
            "monitor": {
                "interval_seconds": 0.1, "sample_count": 1,
                "started_utc": "2026-07-29T00:00:01+00:00",
                "finished_utc": "2026-07-29T00:01:00+00:00",
                "violation": None},
            "unreal_commands": rows,
            "environment_policy": "strip-all-ue-datacache-overrides-v1",
            "stripped_ddc_environment_overrides": [],
            "after": [],
            "cleanup": {
                "action": "stop-pipeline-sponsor", "target_pid": 321,
                "signal": "terminate", "forced": False,
                "wait_return_code": 0,
                "verified_stopped": True, "data_directory_removed": True},
            "lifecycle_status": "success",
        }

    @staticmethod
    def _cooked_paths() -> list[str]:
        packages = NEUTRAL_PACKAGES
        return sorted([
            "../../../WoT/Content/WoT/Maps/WoTBootstrap.umap",
            "../../../WoT/Content/WoT/Maps/WoTFrontEnd.umap",
            *[
                "../../../WoT/Plugins/WoTNeutralShells/Content/"
                f"WoTMapShell{slot:02d}.umap"
                for slot in range(1, 20)
            ],
            *[
                "../../../WoT/Content" + package.removeprefix("/Game")
                + ".uasset"
                for package in packages
            ],
            *COOKED_DESCRIPTOR_PATHS,
        ])

    def _cooked_receipt(self) -> dict:
        paths = self._cooked_paths()
        return {
            "format": "wot-guarded-cooked-container-inventory-v2",
            "platform": "Mac",
            "configuration": "Test",
            "tool": {"path": "UnrealPak", "size": 1,
                     "sha256": "1" * 64},
            "containers": [{"path": "Paks/test.utoc", "size": 1,
                            "sha256": "2" * 64, "entry_count": len(paths)}],
            "paths": paths,
            "path_count": len(paths),
            "validation": {
                "project_content_count": COOKED_PATH_COUNT,
                "retail_derived_count": 0,
                "neutral_map_count": 21,
                "neutral_support_package_count": len(NEUTRAL_PACKAGES),
                "neutral_shell_plugin_descriptor_count": 1,
                "engine_plugin_descriptor_count": COOKED_ENGINE_DESCRIPTOR_COUNT,
            },
            "log": "logs/cooked-container-inventory.log",
            "log_sha256": "3" * 64,
        }

    @staticmethod
    def _heartbeat(
        event, phase, epoch, pid, seconds, *, base="2026-08-10T12:00:00"
    ):
        timestamp = datetime.fromisoformat(base) + timedelta(seconds=seconds)
        return {
            "epoch": epoch,
            "event": event,
            "phase": phase,
            "pid": pid,
            "ts_utc": timestamp.replace(tzinfo=timezone.utc).isoformat(),
        }

    @staticmethod
    def _warm_cache_provenance():
        return {
            "classification": "warm-repeat",
            "evidence": "same-invocation-profile",
            "normal_reboot_utc": None,
        }

    def _cached_records(self, handoff_seconds=2.999, pid=4242, epoch=11):
        return [
            self._heartbeat(
                "phase-begin", "CheckingOriginalGameData", 7, pid, 10.0
            ),
            self._heartbeat("phase-ready", "Ready", epoch, pid, 12.5),
            self._heartbeat(
                "frontend-handoff", "Ready", epoch, pid,
                10.0 + handoff_seconds,
            ),
        ]

    def test_cached_handoff_receipt_accepts_only_complete_success(self):
        receipt = funnel._cached_handoff_receipt(
            self._cached_records(), expected_pid=4242,
            process_started_epoch=1786363210.0,
            network_bytes=0, cache_writes=0,
            cold_cache_provenance=self._warm_cache_provenance(),
        )
        self.assertEqual(receipt["ready_seconds"], 2.5)
        self.assertEqual(receipt["frontend_handoff_seconds"], 2.999)
        self.assertEqual(receipt["process_start_seconds"], 2.999)
        self.assertIsNone(receipt["latency_warning"])
        self.assertEqual(receipt["network_bytes"], 0)
        self.assertEqual(receipt["cache_writes"], 0)
        self.assertEqual(
            receipt["cold_cache_provenance"], self._warm_cache_provenance()
        )

    def test_release_notes_outcome_is_pid_bound_and_strict(self):
        records = [
            {**self._heartbeat("release-notes-result", "Ready", 11, 999, 1),
             "detail": "outcome=invalid sequence=7 commit=other"},
            {**self._heartbeat("release-notes-result", "Ready", 11, 4242, 2),
             "detail": "outcome=selected sequence=7 commit=abc123"},
        ]
        self.assertEqual(
            funnel._release_notes_observation(records, 4242),
            {"commit": "abc123", "outcome": "selected", "pid": 4242,
             "sequence": 7})
        records.append(copy.deepcopy(records[-1]))
        with self.assertRaisesRegex(funnel.FunnelFailure, "exactly one"):
            funnel._release_notes_observation(records, 4242)


    def test_ready_notes_wait_uses_one_remaining_timeout_budget(self):
        client = mock.Mock()
        client.heartbeat_path = Path("heartbeat.jsonl")
        client.heartbeat_offset = 0
        client.process.pid = 4242
        ready = self._heartbeat("phase-ready", "Ready", 11, 4242, 1)
        notes = {
            **self._heartbeat("release-notes-result", "Ready", 11, 4242, 2),
            "detail": "outcome=selected sequence=7 commit=abc123",
        }
        with mock.patch.object(
                funnel, "_wait_heartbeat",
                side_effect=[[ready], [ready, notes]]) as wait, \
                mock.patch.object(
                    funnel.time, "monotonic", side_effect=[100.0, 101.0, 103.0]):
            funnel._wait_client_notes_if_ready(client, 5.0)
        self.assertEqual(wait.call_args_list[0].args[3], 4.0)
        self.assertEqual(wait.call_args_list[1].args[3], 2.0)

    def test_notes_request_count_is_exactly_its_route(self):
        requests = [
            self._loopback_request(funnel.RELEASE_NOTES_ROUTE, 1.0),
            self._loopback_request("/installer", 2.0),
            self._loopback_request(
                funnel.RELEASE_NOTES_ROUTE + "?ignored=true", 3.0),
        ]
        self.assertEqual(funnel._notes_request_count(requests), 1)

    def test_cached_handoff_rejects_socket_or_map_without_complete_records(self):
        # A net-test connection and a map snapshot are intentionally absent
        # from the API: neither is complete bootstrap milestone evidence.
        records = [self._heartbeat(
            "phase-begin", "CheckingGameUpdates", 2, 4242, 1.0
        )]
        with self.assertRaisesRegex(funnel.FunnelFailure, "CheckingOriginalGameData"):
            funnel._cached_handoff_receipt(
                records, expected_pid=4242, process_started_epoch=1786363199.0,
                network_bytes=0, cache_writes=0,
                cold_cache_provenance=self._warm_cache_provenance(),
            )

    def test_cached_handoff_rejects_missing_ready_or_handoff(self):
        complete = self._cached_records()
        for records, expected in (
            ([complete[0], complete[2]], "Ready"),
            (complete[:2], "frontend-handoff"),
            (complete[:2] + [{
                **complete[2], "event": "frontend-handoff-refused",
            }], "frontend-handoff"),
        ):
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(funnel.FunnelFailure, expected):
                    funnel._cached_handoff_receipt(
                        records, expected_pid=4242,
                        process_started_epoch=1786363199.0,
                        network_bytes=0, cache_writes=0,
                        cold_cache_provenance=self._warm_cache_provenance(),
                    )

    def test_cached_handoff_rejects_wrong_order_epoch_pid_and_stale_records(self):
        complete = self._cached_records()
        cases = {
            "order": [complete[0], complete[2], complete[1]],
            "epoch": [complete[0], complete[1], {
                **complete[2], "epoch": complete[1]["epoch"] + 1,
            }],
            "pid": [complete[0], complete[1], {
                **complete[2], "pid": complete[1]["pid"] + 1,
            }],
            "stale": self._cached_records(pid=31337) + [complete[0]],
            "epoch-order": [
                {**complete[0], "epoch": complete[1]["epoch"] + 1},
                complete[1], complete[2],
            ],
            "predates-process": [
                {**complete[0], "ts_utc": "2026-08-10T11:59:58+00:00"},
                complete[1], complete[2],
            ],
        }
        for label, records in cases.items():
            with self.subTest(label=label):
                with self.assertRaises(funnel.FunnelFailure):
                    funnel._cached_handoff_receipt(
                        records, expected_pid=4242,
                        process_started_epoch=1786363199.0,
                        network_bytes=0, cache_writes=0,
                        cold_cache_provenance=self._warm_cache_provenance(),
                    )

    def test_cached_handoff_warns_only_above_three_second_boundary(self):
        accepted = funnel._cached_handoff_receipt(
            self._cached_records(handoff_seconds=2.999), expected_pid=4242,
            process_started_epoch=1786363210.0,
            network_bytes=0, cache_writes=0,
            cold_cache_provenance=self._warm_cache_provenance(),
        )
        self.assertEqual(accepted["frontend_handoff_seconds"], 2.999)
        boundary = funnel._cached_handoff_receipt(
            self._cached_records(handoff_seconds=3.000), expected_pid=4242,
            process_started_epoch=1786363210.0,
            network_bytes=0, cache_writes=0,
            cold_cache_provenance=self._warm_cache_provenance(),
        )
        self.assertIsNone(boundary["latency_warning"])
        warned = funnel._cached_handoff_receipt(
            self._cached_records(handoff_seconds=3.001), expected_pid=4242,
            process_started_epoch=1786363210.0,
            network_bytes=0, cache_writes=0,
            cold_cache_provenance=self._warm_cache_provenance(),
        )
        self.assertEqual(
            warned["latency_warning"],
            "frontend-handoff exceeded 3.0 seconds")

    def test_cached_handoff_rejects_network_generation_or_resume_mutation(self):
        for field in ("network_bytes", "cache_writes"):
            counters = {
                "network_bytes": 0, "cache_writes": 0,
            }
            counters[field] = 1
            with self.subTest(field=field):
                with self.assertRaisesRegex(funnel.FunnelFailure, field):
                    funnel._cached_handoff_receipt(
                        self._cached_records(), expected_pid=4242,
                        process_started_epoch=1786363199.0,
                        cold_cache_provenance=self._warm_cache_provenance(),
                        **counters,
                    )

    def test_cached_handoff_requires_auditable_cache_provenance(self):
        invalid = (
            None,
            {},
            {"classification": "cold-post-reboot"},
            {
                "classification": "cold-post-reboot",
                "evidence": "same-invocation-profile",
                "normal_reboot_utc": None,
            },
            {
                "classification": "unknown",
                "evidence": "manual note",
                "normal_reboot_utc": "2026-08-10T11:00:00+00:00",
            },
        )
        for provenance in invalid:
            with self.subTest(provenance=provenance):
                with self.assertRaisesRegex(funnel.FunnelFailure, "provenance"):
                    funnel._cached_handoff_receipt(
                        self._cached_records(), expected_pid=4242,
                        process_started_epoch=1786363199.0,
                        network_bytes=0, cache_writes=0,
                        cold_cache_provenance=provenance,
                    )


    @unittest.skipUnless(os.name == "nt", "Windows sharing contract")

    @unittest.skipUnless(os.name == "nt", "Windows sharing contract")

    @unittest.skipUnless(os.name == "nt", "Windows sharing contract")


    def test_cached_state_snapshot_counts_current_cache_writes(self):
        state = self.root / "state"
        current = state / "ContentCache" / "current"
        current.mkdir(parents=True)
        manifest = current / "manifest.json"
        manifest.write_text("one", encoding="utf-8")
        before = funnel._snapshot_cached_launch_state(state)
        manifest.write_text("two-and-changed", encoding="utf-8")
        after = funnel._snapshot_cached_launch_state(state)
        changed = funnel._cached_launch_mutation_counts(before, after)
        self.assertEqual(changed["cache_writes"], 1)
        self.assertEqual(
            funnel._cached_launch_mutation_counts(after, after),
            {"cache_writes": 0},
        )

    def test_network_bytes_sum_all_receipted_loopback_responses(self):
        self.assertEqual(funnel._network_bytes([
            {"response_bytes": 0}, {"response_bytes": 17},
            {"response_bytes": 25},
        ]), 42)

    @staticmethod
    def _loopback_request(
        path, seconds, *, method="GET", response_bytes=0,
        base="2026-08-10T12:00:00",
    ):
        timestamp = datetime.fromisoformat(base) + timedelta(seconds=seconds)
        return {
            "method": method,
            "path": path,
            "timestamp_utc": timestamp.replace(
                tzinfo=timezone.utc
            ).isoformat(),
            "response_bytes": response_bytes,
        }

    def test_adjacent_network_gate_allows_post_ready_announcement_get(self):
        request = self._loopback_request("/announcement.json", 12.5)
        update_checks, outside = (
            funnel._classify_post_ready_update_check_requests(
                [request], self._cached_records(), 4242
            )
        )
        self.assertEqual(update_checks, [request])
        self.assertEqual(outside, [])

    def test_second_launch_gate_allows_post_ready_announcement_signature_get(self):
        request = self._loopback_request("/announcement.json.sig", 13.0)
        update_checks, outside = (
            funnel._classify_post_ready_update_check_requests(
                [request], self._cached_records(), 4242
            )
        )
        self.assertEqual(update_checks, [request])
        self.assertEqual(outside, [])

    def test_announcement_get_before_ready_is_outside_update_check(self):
        request = self._loopback_request("/announcement.json", 12.499999)
        update_checks, outside = (
            funnel._classify_post_ready_update_check_requests(
                [request], self._cached_records(), 4242
            )
        )
        self.assertEqual(update_checks, [])
        self.assertEqual(outside, [request])

    def test_update_check_requires_ready_record_for_launched_pid(self):
        request = self._loopback_request("/announcement.json", 13.0)
        update_checks, outside = (
            funnel._classify_post_ready_update_check_requests(
                [request], self._cached_records(pid=5150), 4242
            )
        )
        self.assertEqual(update_checks, [])
        self.assertEqual(outside, [request])

    def test_only_exact_announcement_gets_are_update_checks(self):
        requests = [
            self._loopback_request("/installer", 13.0),
            self._loopback_request("/other", 13.0),
            self._loopback_request("/announcement.json?cache=0", 13.0),
            self._loopback_request(
                "/announcement.json", 13.0, method="POST"
            ),
        ]
        update_checks, outside = (
            funnel._classify_post_ready_update_check_requests(
                requests, self._cached_records(), 4242
            )
        )
        self.assertEqual(update_checks, [])
        self.assertEqual(outside, requests)

    def test_network_bytes_exclude_only_allowed_update_checks(self):
        allowed = self._loopback_request(
            "/announcement.json", 12.5, response_bytes=17
        )
        before_ready = self._loopback_request(
            "/announcement.json.sig", 12.0, response_bytes=19
        )
        installer = self._loopback_request(
            "/installer", 13.0, response_bytes=23
        )
        update_checks, outside = (
            funnel._classify_post_ready_update_check_requests(
                [allowed, before_ready, installer],
                self._cached_records(),
                4242,
            )
        )
        self.assertEqual(update_checks, [allowed])
        self.assertEqual(outside, [before_ready, installer])
        self.assertEqual(funnel._network_bytes(outside), 42)

    def test_update_check_uses_only_its_process_ready_timestamp(self):
        host_records = [
            self._heartbeat("phase-ready", "Ready", 11, 4242, 10.0)
        ]
        joiner_records = [
            self._heartbeat("phase-ready", "Ready", 12, 5150, 20.0)
        ]
        request = self._loopback_request("/announcement.json", 15.0)

        host_update_checks, host_outside = (
            funnel._classify_post_ready_update_check_requests(
                [request], host_records, 4242
            )
        )
        joiner_update_checks, joiner_outside = (
            funnel._classify_post_ready_update_check_requests(
                [request], joiner_records, 5150
            )
        )

        self.assertEqual(host_update_checks, [request])
        self.assertEqual(host_outside, [])
        self.assertEqual(joiner_update_checks, [])
        self.assertEqual(joiner_outside, [request])

    def test_source_identity_uses_task8_release_canonicalization(self):
        value = {"non_ascii": "\N{SNOWMAN}", "answer": 42}
        self.assertEqual(
            funnel._canonical_release_bytes(value),
            b'{"answer":42,"non_ascii":"\\u2603"}',
        )
        self.assertNotEqual(
            funnel._canonical_release_bytes(value),
            funnel._canonical_json_bytes(value),
        )

    def test_refuses_non_guarded_artifact(self):
        with self.assertRaisesRegex(funnel.FunnelFailure, "guarded"):
            funnel._resolve_release(
                self.make_release(guarded=False),
                require_current_source=False,
            )

    def test_refuses_missing_release_receipts(self):
        with self.assertRaisesRegex(funnel.FunnelFailure, "missing"):
            funnel._resolve_release(
                self.make_release(include_receipts=False),
                require_current_source=False,
            )

    def test_refuses_missing_cooked_container_inventory(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        del manifest["cooked_container_inventory"]
        release.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(
            funnel.FunnelFailure, "cooked_container_inventory"
        ):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_legacy_release_without_neutral_support_proof(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        del manifest["neutral_support_authority"]
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(
            funnel.FunnelFailure, "neutral_support_authority"
        ):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_reordered_neutral_support_roots(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        roots = manifest["neutral_support_authority"]["roots"]
        roots[0], roots[1] = roots[1], roots[0]
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "exact assets"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_duplicate_neutral_support_dependency(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        dependencies = manifest["neutral_support_authority"][
            "hard_dependencies"]
        dependencies[-1] = dependencies[0]
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "exact assets"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_tampered_neutral_support_asset_tree(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        manifest["neutral_support_authority"]["assets"][3]["size"] += 1
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "asset-tree digest"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_tampered_neutral_support_authority_digest(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        manifest["neutral_support_authority"]["authority_sha256"] = "d" * 64
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "authority digest"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_legacy_inventory_without_neutral_support_count(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        del manifest["cooked_container_inventory"]["validation"][
            "neutral_support_package_count"]
        receipt = manifest["cooked_container_inventory"]["receipt"]
        del receipt["validation"]["neutral_support_package_count"]
        manifest["cooked_container_inventory"]["receipt_sha256"] = self._sha(
            funnel._pretty_json_bytes(receipt))
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(
                funnel.FunnelFailure, "exact neutral|path census"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_legacy_cooked_summary_without_path_census(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        del manifest["cooked_container_inventory"]["receipt"]
        release.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(funnel.FunnelFailure, "path census"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_tampered_cooked_path_census(self):
        def replace_missing(paths):
            paths[0] = (
                "../../../WoT/Content/"
                "ShaderArchive-WoT-PCD3D_SM6-PCD3D_SM6.ushaderbytecode")

        def duplicate(paths):
            paths[-1] = paths[0]

        def extra(paths):
            paths.append("../../../WoT/Content/Unexpected/Extra.uasset")

        def retail(paths):
            paths.append(
                "../../../WoT/Content/WoT/Maps/RetailArena.umap")

        def sidecars_only(paths):
            paths[:] = [str(Path(path).with_suffix(".uexp")) for path in paths]

        for name, mutate in (
                ("missing", replace_missing),
                ("duplicate", duplicate),
                ("extra", extra),
                ("retail", retail),
                ("sidecars-only", sidecars_only)):
            release = self.make_release()
            manifest = json.loads(release.read_text(encoding="utf-8"))
            wrapper = manifest["cooked_container_inventory"]
            receipt = wrapper["receipt"]
            mutate(receipt["paths"])
            if name != "duplicate":
                receipt["paths"] = sorted(receipt["paths"])
            receipt["path_count"] = len(receipt["paths"])
            receipt["containers"][0]["entry_count"] = len(receipt["paths"])
            wrapper["path_count"] = receipt["path_count"]
            wrapper["receipt_sha256"] = self._sha(
                funnel._pretty_json_bytes(receipt))
            release.write_text(json.dumps(manifest), encoding="utf-8")

            with self.subTest(name=name), self.assertRaisesRegex(
                    funnel.FunnelFailure, "path census|full v2"):
                funnel._resolve_release(
                    release, require_current_source=False)

    def test_refuses_truncated_full_v2_cooked_receipt(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        wrapper = manifest["cooked_container_inventory"]
        receipt = wrapper["receipt"]
        for key in ("tool", "containers", "log", "log_sha256"):
            del receipt[key]
        wrapper["receipt_sha256"] = self._sha(
            funnel._pretty_json_bytes(receipt))
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "full v2"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_noncanonical_full_v2_cooked_identities(self):
        def wrong_tool(receipt):
            receipt["tool"]["path"] = "not-unrealpak"

        def wrong_log(receipt):
            receipt["log"] = "not-inventory"

        def reordered_paths(receipt):
            receipt["paths"][0], receipt["paths"][1] = (
                receipt["paths"][1], receipt["paths"][0])

        def backslash_path(receipt):
            receipt["paths"][0] = receipt["paths"][0].replace("/", "\\")

        for name, mutate in (
                ("wrong-tool", wrong_tool),
                ("wrong-log", wrong_log),
                ("reordered-paths", reordered_paths),
                ("backslash-path", backslash_path)):
            release = self.make_release()
            manifest = json.loads(release.read_text(encoding="utf-8"))
            wrapper = manifest["cooked_container_inventory"]
            receipt = wrapper["receipt"]
            mutate(receipt)
            wrapper["receipt_sha256"] = self._sha(
                funnel._pretty_json_bytes(receipt))
            release.write_text(json.dumps(manifest), encoding="utf-8")

            with self.subTest(name=name), self.assertRaisesRegex(
                    funnel.FunnelFailure, "full v2|path census"):
                funnel._resolve_release(
                    release, require_current_source=False)

    def test_refuses_legacy_zen_receipt(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        legacy = {"format": "zen"}
        manifest["zen_lifetime"] = {
            "receipt": legacy,
            "receipt_sha256": self._sha(funnel._pretty_json_bytes(legacy)),
        }
        release.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(funnel.FunnelFailure, "Zen.*v3"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_reordered_zen_cook_package_vector(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        receipt = self._zen_receipt()
        command = receipt["unreal_commands"][-1]["command"][-1]
        prefix, vector = command.split("-PACKAGE=", 1)
        roots = vector.split("+")
        roots[0], roots[1] = roots[1], roots[0]
        receipt["unreal_commands"][-1]["command"][-1] = (
            prefix + "-PACKAGE=" + "+".join(roots))
        manifest["zen_lifetime"] = {
            "receipt": receipt,
            "receipt_sha256": self._sha(funnel._pretty_json_bytes(receipt)),
        }
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "PACKAGE"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_noncanonical_win64_serial_local_cook_receipt(self):
        receipt = self._zen_receipt()
        win_zen = "Engine/Binaries/Win64/zenserver.exe"
        receipt["version"]["path"] = win_zen
        receipt["sponsor"]["command"][0] = win_zen
        receipt["processes_after_lifecycle"][0]["command"] = \
            " ".join(receipt["sponsor"]["command"])
        cook = receipt["unreal_commands"][-1]["command"]
        exact_ubt = (
            "-UbtArgs=-MaxParallelActions=1 -NoXGE -NoFASTBuild "
            "-NoSNDBS -NoUBA -UBADisableRemote")
        cook.extend(["-NoXGE", exact_ubt])

        def proof(value):
            return {
                "receipt": value,
                "receipt_sha256": self._sha(
                    funnel._pretty_json_bytes(value)),
            }

        funnel._validate_zen_lifetime_proof(proof(receipt), "Win64")
        hostile_commands = (
            [token for token in cook if token != exact_ubt],
            [token.replace("-MaxParallelActions=1",
                           "-MaxParallelActions=2")
             if token == exact_ubt else token for token in cook],
            [*cook, exact_ubt],
            [token + " -AllowRemoteExecutor" if token == exact_ubt
             else token for token in cook],
            [*cook, "-MaxParallelActions=8"],
            [*cook, "-XGE"],
            [token for token in cook if token != "-NoXGE"],
            [*cook, "-NoXGE"],
        )
        for hostile_command in hostile_commands:
            with self.subTest(command=hostile_command):
                hostile = copy.deepcopy(receipt)
                hostile["unreal_commands"][-1]["command"] = \
                    hostile_command
                with self.assertRaisesRegex(
                        funnel.FunnelFailure, "serial/local"):
                    funnel._validate_zen_lifetime_proof(
                        proof(hostile), "Win64")

    def test_refuses_truncated_zen_v3_receipt(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        receipt = manifest["zen_lifetime"]["receipt"]
        for key in (
                "owner", "version", "before", "endpoint_preflight",
                "processes_after_lifecycle", "monitor", "after"):
            del receipt[key]
        manifest["zen_lifetime"]["receipt_sha256"] = self._sha(
            funnel._pretty_json_bytes(receipt))
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "Zen.*v3"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_recomputed_zen_process_proof_tampering(self):
        def missing_sponsor(receipt):
            receipt["processes_after_lifecycle"] = []

        def sponsor_survived(receipt):
            receipt["after"] = list(receipt["processes_after_lifecycle"])

        def failed_preflight(receipt):
            receipt["endpoint_preflight"][
                "exclusive_bind_succeeded"] = False

        def monitor_violation(receipt):
            receipt["monitor"]["violation"] = "unexpected Zen process"

        for name, mutate in (
                ("missing-sponsor", missing_sponsor),
                ("sponsor-survived", sponsor_survived),
                ("failed-preflight", failed_preflight),
                ("monitor-violation", monitor_violation)):
            release = self.make_release()
            manifest = json.loads(release.read_text(encoding="utf-8"))
            receipt = manifest["zen_lifetime"]["receipt"]
            mutate(receipt)
            manifest["zen_lifetime"]["receipt_sha256"] = self._sha(
                funnel._pretty_json_bytes(receipt))
            release.write_text(json.dumps(manifest), encoding="utf-8")

            with self.subTest(name=name), self.assertRaisesRegex(
                    funnel.FunnelFailure, "Zen.*v3"):
                funnel._resolve_release(
                    release, require_current_source=False)

    def test_refuses_recomputed_non_zen_sponsor_executable(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        receipt = manifest["zen_lifetime"]["receipt"]
        receipt["version"]["path"] = "not-zen"
        command = receipt["sponsor"]["command"]
        command[0] = "not-zen"
        receipt["processes_after_lifecycle"][0]["command"] = " ".join(command)
        manifest["zen_lifetime"]["receipt_sha256"] = self._sha(
            funnel._pretty_json_bytes(receipt))
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "Zen.*v3"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_recomputed_wrong_platform_zen_sponsor_executable(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        receipt = manifest["zen_lifetime"]["receipt"]
        receipt["version"]["path"] = "Engine/Binaries/Win64/zenserver.exe"
        command = receipt["sponsor"]["command"]
        command[0] = receipt["version"]["path"]
        receipt["processes_after_lifecycle"][0]["command"] = " ".join(command)
        manifest["zen_lifetime"]["receipt_sha256"] = self._sha(
            funnel._pretty_json_bytes(receipt))
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "Zen.*v3"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_recomputed_noncanonical_zen_sponsor_data_directory(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        receipt = manifest["zen_lifetime"]["receipt"]
        command = receipt["sponsor"]["command"]
        command[6] = "arbitrary-data"
        receipt["processes_after_lifecycle"][0]["command"] = " ".join(command)
        manifest["zen_lifetime"]["receipt_sha256"] = self._sha(
            funnel._pretty_json_bytes(receipt))
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "Zen.*v3"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_recomputed_noncanonical_zen_sponsor_log(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        receipt = manifest["zen_lifetime"]["receipt"]
        receipt["sponsor"]["log"] = "logs/arbitrary.log"
        manifest["zen_lifetime"]["receipt_sha256"] = self._sha(
            funnel._pretty_json_bytes(receipt))
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "Zen.*v3"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_recomputed_zen_sponsor_pid_equal_to_owner(self):
        release = self.make_release()
        manifest = json.loads(release.read_text(encoding="utf-8"))
        receipt = manifest["zen_lifetime"]["receipt"]
        owner_pid = receipt["owner"]["pid"]
        receipt["sponsor"]["pid"] = owner_pid
        receipt["processes_after_lifecycle"][0]["pid"] = owner_pid
        receipt["cleanup"]["target_pid"] = owner_pid
        manifest["zen_lifetime"]["receipt_sha256"] = self._sha(
            funnel._pretty_json_bytes(receipt))
        release.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(funnel.FunnelFailure, "Zen.*v3"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_asset_digest_mismatch(self):
        release = self.make_release()
        manifest = json.loads(release.read_text())
        manifest["sha256"] = "0" * 64
        release.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(funnel.FunnelFailure, "differs"):
            funnel._resolve_release(release, require_current_source=False)

    def test_refuses_stale_source_snapshot_with_rebuild_instruction(self):
        release = self.make_release()
        current = {
            "format": funnel.SOURCE_SNAPSHOT_FORMAT,
            "git_commit": "different",
            "sha256": "f" * 64,
        }
        with mock.patch.object(
            funnel, "_current_source_snapshot", return_value=current
        ):
            with self.assertRaisesRegex(
                funnel.FunnelFailure, "build_stripped_pak.py --guarded"
            ):
                funnel._resolve_release(release)

    def test_development_source_drift_override_only_relaxes_snapshot_check(self):
        release = self.make_release()
        args = mock.Mock(
            release=release, development_allow_source_drift=True)
        sentinel = ({"platform": "Mac"}, release.parent / "asset.zip")
        with mock.patch.object(
                funnel, "_resolve_release", return_value=sentinel) as resolve:
            self.assertEqual(funnel._resolve_funnel_release(args), sentinel)
        resolve.assert_called_once_with(
            release, require_current_source=False)

        args.development_allow_source_drift = False
        with mock.patch.object(
                funnel, "_resolve_release", return_value=sentinel) as resolve:
            self.assertEqual(funnel._resolve_funnel_release(args), sentinel)
        resolve.assert_called_once_with(
            release, require_current_source=True)

    def test_development_source_drift_still_checks_neutral_authority(self):
        release = self.make_release()
        unrelated_repo = self.root / "unrelated-repo"
        unrelated_repo.mkdir()
        with mock.patch.object(funnel, "REPO", unrelated_repo):
            with self.assertRaisesRegex(
                    funnel.FunnelFailure, "neutral-support authority"):
                funnel._resolve_release(
                    release, require_current_source=False)

    def test_parser_exposes_explicit_development_source_drift_override(self):
        parser = funnel._build_parser()
        parsed = parser.parse_args([
            "--release", "/tmp/release.json",
            "--development-allow-source-drift",
        ])
        self.assertTrue(parsed.development_allow_source_drift)

    def test_refuses_dirty_profile_or_work_root(self):
        dirty = self.root / "dirty"
        dirty.mkdir()
        (dirty / "state").write_text("owned")
        with self.assertRaisesRegex(funnel.FunnelFailure, "dirty"):
            funnel._require_empty_directory(dirty, "profile")
        clean = self.root / "clean"
        funnel._require_empty_directory(clean, "profile")
        self.assertTrue(clean.is_dir())

    def test_guarded_tree_clone_uses_distinct_files_not_hard_links(self):
        source = self.root / "source"
        source.mkdir()
        original = source / "game"
        original.write_bytes(b"guarded")
        clone = funnel._clone_release_tree(source, self.root / "clone")
        copied = clone / original.name
        self.assertEqual(copied.read_bytes(), original.read_bytes())
        self.assertNotEqual(copied.stat().st_ino, original.stat().st_ino)
        self.assertEqual(copied.stat().st_nlink, 1)

    def test_windows_clone_of_a_used_tree_leaves_its_adjacent_installer(self):
        # host-join clones fresh-first-run's folder to reuse its Data; the
        # installer that cell placed beside Game must not come along, or
        # host-join's own _copy_installer_adjacent refuses (rel 19e978f2f).
        folder = self.root / "fresh" / "WoT Reborn"
        (folder / "Game").mkdir(parents=True)
        (folder / "Game" / "payload").write_bytes(b"game")
        (folder / "WoT Reborn.exe").write_bytes(b"launcher")
        (folder / "Data").mkdir()
        (folder / "Data" / "cache").write_bytes(b"built")
        (folder / funnel.INSTALLER_NAME).write_bytes(b"installer")
        clone = funnel._clone_release_tree(
            folder / "Game", self.root / "host-a" / "WoT Reborn" / "Game")
        copied = clone.parent
        self.assertEqual((copied / "Game" / "payload").read_bytes(), b"game")
        self.assertEqual((copied / "WoT Reborn.exe").read_bytes(), b"launcher")
        self.assertEqual((copied / "Data" / "cache").read_bytes(), b"built")
        self.assertFalse((copied / funnel.INSTALLER_NAME).exists())
        self.assertTrue((folder / funnel.INSTALLER_NAME).exists())

    @unittest.skipUnless(os.name == "nt", "Windows sharing contract")
    def test_heartbeat_reads_never_block_the_games_delete_access_open(self):
        # The game opens its heartbeat and update markers with DELETE access
        # (sharing read, write and delete). A funnel read that does not share
        # delete fails that open, and the heartbeat writer then drops records
        # (a discovery run lost a second launch's Ready that way).
        path = self.root / "bootstrap-heartbeat-v1.1.a.jsonl"
        path.write_bytes(b'{"event":"phase-ready"}\n')

        def game_style_open() -> bool:
            handle = funnel._create_file(
                str(path), 0x80000000 | 0x40000000 | 0x00010000,
                0x1 | 0x2 | 0x4, None, 3, 0x80, None)
            if handle == ctypes.c_void_p(-1).value:
                return False
            funnel._close_handle(handle)
            return True

        with open(path, "rb"):
            self.assertFalse(game_style_open(),
                             "control: a plain Python read blocks the game")
        descriptor = funnel._open_shared_read_descriptor(path)
        try:
            self.assertTrue(game_style_open())
        finally:
            os.close(descriptor)
        self.assertEqual(funnel._read_shared_bytes(path),
                         b'{"event":"phase-ready"}\n')
        self.assertEqual(
            funnel._read_shared_text(path, "ascii"), '{"event":"phase-ready"}\n')
        with self.assertRaises(FileNotFoundError):
            funnel._read_shared_bytes(self.root / "missing.jsonl")

    def test_adjacent_installer_is_single_link_never_hard_linked(self):
        # A hard-linked adjacent installer has LinkCount > 1, which the
        # guarded probe rejects fail-closed (BOOTSTRAP-FAILURE at
        # CheckingOriginalGameData), the root cause of a "boot stall".
        tree = self.root / "cells" / "adjacent-tree"
        tree.mkdir(parents=True)
        cache = self.root / "cache"
        cache.mkdir()
        installer = cache / funnel.INSTALLER_NAME
        installer.write_bytes(b"pinned-bytes")
        # Simulate the shared-cache reality: the source already has extra
        # names pointing at its inode.
        os.link(installer, cache / "extra-name")
        with mock.patch.object(
            funnel, "_verify_installer",
            side_effect=lambda path: {"path": str(path)},
        ):
            placed = funnel._copy_installer_adjacent(installer, tree, "Mac")
        self.assertEqual(placed.read_bytes(), installer.read_bytes())
        self.assertEqual(placed.stat().st_nlink, 1)
        self.assertNotEqual(placed.stat().st_ino, installer.stat().st_ino)
        with self.assertRaisesRegex(funnel.FunnelFailure, "already exists"):
            funnel._copy_installer_adjacent(installer, tree, "Mac")


    def test_update_observer_factory_selects_platform_implementation(self):
        path = self.root / "events.jsonl"
        mac_observer = mock.Mock()
        with mock.patch.object(
                funnel, "MacProcessObserver", return_value=mac_observer) as mac:
            self.assertIs(
                funnel._make_update_process_observer("Mac", path),
                mac_observer)
        mac.assert_called_once_with(path)

        win_observer = mock.Mock()
        with mock.patch.object(
                funnel, "_WinJobProcessObserver",
                return_value=win_observer) as win:
            self.assertIs(
                funnel._make_update_process_observer("Win64", path),
                win_observer)
        win.assert_called_once_with(path)


    def test_mac_observer_identity_is_stable_and_live(self):
        root = self.root / "scratch"
        executable = root / "WoT.app" / "Contents" / "MacOS" / "WoT"
        row = {
            "argv": [str(executable)], "command": "fixture",
            "parent_pid": 1, "pid": 4321, "start_time": 200.25,
            "uid": 501,
        }
        observer = funnel.MacProcessObserver(
            ps_reader=lambda: [row], proc_path=lambda _pid: str(executable),
            uid=501)
        try:
            identity = observer.observe_identity(4321, timeout=0.01)
        except AttributeError as exc:
            self.fail(f"Mac observer has no shared identity API: {exc}")
        self.assertEqual(identity["pid"], 4321)
        self.assertEqual(identity["process_start_id"], "200.250000")
        self.assertGreater(identity["observed_monotonic_seconds"], 0)


    def test_handle_termination_requires_image_under_scratch(self):
        scratch = self.root / "scratch"
        image = scratch / "tree" / "WoT-Win64-Test.exe"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"fixture")

        def query_inside(_handle, _flags, buffer, _length):
            buffer.value = str(image)
            return 1

        with mock.patch.object(funnel, "_IS_WINDOWS", True), \
                mock.patch.object(funnel, "_open_process", return_value=77,
                                  create=True), \
                mock.patch.object(funnel, "_query_full_process_image_name",
                                  side_effect=query_inside, create=True), \
                mock.patch.object(funnel, "_terminate_process", return_value=1,
                                  create=True) as terminate, \
                mock.patch.object(funnel, "_wait_for_single_object",
                                  return_value=0, create=True), \
                mock.patch.object(funnel, "_close_handle", create=True):
            result = funnel._terminate_external_pid(
                123, scratch_root=scratch)
        self.assertEqual(result["action"], "terminated")
        terminate.assert_called_once_with(77, 1)

        def query_outside(_handle, _flags, buffer, _length):
            buffer.value = str(self.root / "outside.exe")
            return 1

        with mock.patch.object(funnel, "_IS_WINDOWS", True), \
                mock.patch.object(funnel, "_open_process", return_value=77,
                                  create=True), \
                mock.patch.object(funnel, "_query_full_process_image_name",
                                  side_effect=query_outside, create=True), \
                mock.patch.object(funnel, "_terminate_process", create=True) \
                    as terminate, \
                mock.patch.object(funnel, "_close_handle", create=True):
            result = funnel._terminate_external_pid(
                123, scratch_root=scratch)
        self.assertEqual(result["reason"], "not-ours")
        terminate.assert_not_called()

    def test_handle_termination_treats_access_denied_as_not_ours(self):
        with mock.patch.object(funnel, "_IS_WINDOWS", True), \
                mock.patch.object(funnel, "_open_process", return_value=0,
                                  create=True), \
                mock.patch.object(funnel, "_query_full_process_image_name",
                                  create=True) as query:
            result = funnel._terminate_external_pid(
                123, scratch_root=self.root / "scratch")
        self.assertEqual(result, {
            "pid": 123, "action": "skip", "reason": "not-ours"})
        query.assert_not_called()

    def test_job_observer_decodes_start_and_stop_messages(self):
        observer = object.__new__(funnel._WinJobProcessObserver)
        observer._lock = threading.Lock()
        observer._events = []
        observer._processes = {}
        observer._reader_error = None
        observer._event_file = io.StringIO()

        def query_image(_handle, _flags, buffer, length):
            buffer.value = r"C:\fixture\WoTUpdater.exe"
            length._obj.value = len(buffer.value)
            return 1

        def query_parent(_handle, _kind, info, _size, returned):
            info._obj.InheritedFromUniqueProcessId = 123
            returned._obj.value = ctypes.sizeof(info._obj)
            return 0

        def query_exit(_handle, status):
            status._obj.value = 91
            return 1

        with mock.patch.object(
                funnel, "_open_process", return_value=77, create=True), \
                mock.patch.object(
                    funnel, "_query_full_process_image_name",
                    side_effect=query_image, create=True), \
                mock.patch.object(
                    funnel, "_nt_query_information_process",
                    side_effect=query_parent, create=True), \
                mock.patch.object(
                    funnel, "_get_exit_code_process",
                    side_effect=query_exit, create=True), \
                mock.patch.object(
                    observer, "_process_start_id", return_value="start-1"), \
                mock.patch.object(funnel, "_close_handle", create=True), \
                mock.patch.object(funnel, "Path", PurePosixPath):
            observer._handle_message(6, 456)
            observer._handle_message(8, 456)

        self.assertEqual(observer.events(), [
            {"kind": "start", "pid": 456, "name": "WoTUpdater.exe",
             "parent_pid": 123},
            {"kind": "stop", "pid": 456, "name": "WoTUpdater.exe",
             "parent_pid": 123, "exit_status": 91},
        ])

    def test_job_observer_rechecks_process_start_identity(self):
        observer = object.__new__(funnel._WinJobProcessObserver)
        observer._lock = threading.Lock()
        observer._reader_error = None
        observer._processes = {
            456: {"handle": 77, "name": "WoT.exe", "parent_pid": 123,
                  "process_start_id": "start-1"},
        }

        def query_active(_handle, status):
            status._obj.value = 259
            return 1

        with mock.patch.object(
                funnel, "_get_exit_code_process", side_effect=query_active,
                create=True), \
                mock.patch.object(
                    observer, "_process_start_id", return_value="start-1"):
            identity = observer.observe_identity(456, timeout=0.01)
        self.assertEqual(identity["pid"], 456)
        self.assertEqual(identity["process_start_id"], "start-1")
        self.assertGreater(identity["observed_monotonic_seconds"], 0)

    def test_job_observer_close_preserves_reader_resources_until_it_stops(self):
        observer = object.__new__(funnel._WinJobProcessObserver)
        observer._closed = False
        observer._job = None
        observer._completion_port = 88
        observer._reader_thread = mock.Mock()
        observer._reader_thread.is_alive.side_effect = [True, False]
        observer._processes = {
            456: {"handle": 77, "name": "game.exe", "parent_pid": 123},
        }
        observer._reader_error = None
        observer._event_file = io.StringIO()

        with mock.patch.object(
                funnel, "_post_queued_completion_status", return_value=1,
                create=True), \
                mock.patch.object(
                    funnel, "_close_handle", return_value=1,
                    create=True):
            with self.assertRaisesRegex(
                    funnel.FunnelFailure, "reader did not stop"):
                observer.close()
            self.assertFalse(observer._closed)
            self.assertFalse(observer._event_file.closed)
            self.assertIn(456, observer._processes)
            self.assertEqual(observer._completion_port, 88)

            observer.close()
            self.assertTrue(observer._closed)
            self.assertTrue(observer._event_file.closed)
            self.assertEqual(observer._processes, {})
            self.assertIsNone(observer._completion_port)


    @unittest.skipUnless(os.name == "nt", "Windows job-object contract")
    def test_job_observer_captures_child_and_grandchild_exit_statuses(self):
        gate = self.root / "spawn-grandchild"
        script = """
import pathlib
import subprocess
import sys
import time

gate = pathlib.Path(sys.argv[1])
deadline = time.monotonic() + 5
while not gate.exists():
    if time.monotonic() >= deadline:
        raise SystemExit(2)
    time.sleep(0.01)
subprocess.run([sys.executable, "-c", "import os; os._exit(91)"], check=False)
"""
        with funnel._WinJobProcessObserver(
                self.root / "process-events.jsonl") as observer:
            child = subprocess.Popen([sys.executable, "-c", script, str(gate)])
            observer.adopt(child)
            gate.write_text("go", encoding="utf-8")
            self.assertEqual(child.wait(timeout=5), 0)
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                rows = observer.events()
                grandchild_starts = [
                    row for row in rows
                    if row.get("kind") == "start"
                    and row.get("parent_pid") == child.pid
                ]
                if grandchild_starts and all(
                        any(row.get("kind") == "stop"
                            and row.get("pid") == pid
                            for row in rows)
                        for pid in (child.pid, grandchild_starts[0]["pid"])):
                    break
                time.sleep(0.02)
            else:
                self.fail(f"job events did not complete: {observer.events()!r}")

            grandchild_start = grandchild_starts[0]
            starts = {row["pid"]: row for row in rows
                      if row.get("kind") == "start"}
            stops = {row["pid"]: row for row in rows
                     if row.get("kind") == "stop"}
            self.assertIn(child.pid, starts)
            self.assertIn(grandchild_start["pid"], starts)
            self.assertIn(child.pid, stops)
            self.assertIn(grandchild_start["pid"], stops)
            self.assertEqual(grandchild_start["parent_pid"], child.pid)
            self.assertEqual(stops[child.pid]["exit_status"], 0)
            self.assertEqual(stops[grandchild_start["pid"]]["exit_status"], 91)

    def test_profile_environment_puts_windows_temp_under_profile(self):
        profile = self.root / "profile"
        install = self.root / "work" / "WoT Reborn"
        environment = funnel._profile_environment(profile, "Win64", install)
        expected = profile / "AppData" / "Local" / "Temp"
        self.assertTrue(expected.is_dir())
        self.assertEqual(environment["TMP"], str(expected))
        self.assertEqual(environment["TEMP"], str(expected))
        self.assertEqual(
            funnel._state_root(profile, "Win64", install), install / "Data")
        self.assertTrue((install / "Data").is_dir())

    def test_windows_profile_refuses_hidden_game_data(self):
        profile = self.root / "profile"
        (profile / "AppData" / "Local" / "Temp").mkdir(parents=True)
        self.assertEqual(
            funnel._require_win_profile_has_no_game_data(profile)["forbidden"], [])
        (profile / "AppData" / "Local" / "WoT").mkdir()
        with self.assertRaisesRegex(funnel.FunnelFailure, "redirected AppData"):
            funnel._require_win_profile_has_no_game_data(profile)

    def test_windows_profile_refuses_analytics_and_epic_product_residue(self):
        profile = self.root / "profile"
        analytics = (profile / "AppData" / "Local" / "WoT Reborn"
                     / "Common" / "Analytics" / "session")
        analytics.mkdir(parents=True)
        with self.assertRaisesRegex(funnel.FunnelFailure, "redirected AppData"):
            funnel._require_win_profile_has_no_game_data(profile)
        shutil.rmtree(profile)
        (profile / "AppData" / "Roaming" / "Epic" / "UnrealEngine").mkdir(
            parents=True)
        with self.assertRaisesRegex(funnel.FunnelFailure, "redirected AppData"):
            funnel._require_win_profile_has_no_game_data(profile)
        shutil.rmtree(profile)
        (profile / "AppData" / "Local" / "EpicGamesLauncher").mkdir(
            parents=True)
        with self.assertRaisesRegex(funnel.FunnelFailure, "redirected AppData"):
            funnel._require_win_profile_has_no_game_data(profile)

    def test_every_profile_environment_gets_its_own_empty_downloads(self):
        first_profile = self.root / "work" / "profiles" / "first"
        second_profile = self.root / "work" / "profiles" / "second"
        first = funnel._profile_environment(first_profile, "Mac")
        second = funnel._profile_environment(
            second_profile, "Win64", self.root / "work" / "WoT Reborn")
        first_downloads = Path(first[funnel.TEST_DOWNLOADS_DIR_ENV])
        second_downloads = Path(second[funnel.TEST_DOWNLOADS_DIR_ENV])
        self.assertNotEqual(first_downloads, second_downloads)
        work = (self.root / "work").resolve()
        self.assertTrue(first_downloads.is_relative_to(work))
        self.assertTrue(second_downloads.is_relative_to(work))
        self.assertEqual(list(first_downloads.iterdir()), [])
        self.assertEqual(list(second_downloads.iterdir()), [])

    def test_update_copy_space_requires_twice_profile_plus_tree(self):
        with mock.patch.object(
                funnel, "_directory_size", side_effect=[20, 30]), \
                mock.patch.object(
                    funnel.shutil, "disk_usage",
                    return_value=mock.Mock(free=99)):
            with self.assertRaisesRegex(
                    funnel.FunnelFailure,
                    r"free=99.*required=100.*profile=30.*release=20"):
                funnel._require_update_copy_space(
                    self.root / "tree", self.root / "profile", self.root)

    def test_update_copy_space_sizes_launcher_workspace_and_large_data(self):
        install = self.root / "WoT Reborn"
        game = install / "Game"
        data = install / "Data"
        game.mkdir(parents=True)
        data.mkdir()
        (install / "WoT Reborn.exe").write_bytes(b"launcher")
        (game / "game.bin").write_bytes(b"game")
        (data / "cache.bin").write_bytes(b"x" * 8192)
        (install / ".wot-update").mkdir()
        (install / ".wot-update" / "partial").write_bytes(b"workspace")
        profile = self.root / "profile"
        profile.mkdir()
        (profile / "profile.bin").write_bytes(b"profile")
        release_size = funnel._directory_size(install)
        required = 2 * (release_size + len(b"profile"))
        with mock.patch.object(
                funnel.shutil, "disk_usage",
                return_value=mock.Mock(free=required - 1)):
            with self.assertRaisesRegex(
                    funnel.FunnelFailure,
                    rf"required={required}.*release={release_size}"):
                funnel._require_update_copy_space(game, profile, self.root)

    def test_update_result_parser_uses_final_diagnostic_segment(self):
        row = {
            "event": "update-check-result", "pid": 44,
            "detail": ("pending diagnostic; available status=KnownBad "
                       "sequence=8 reason=known-bad"),
        }
        self.assertEqual(funnel._update_result_observation(row), {
            "event": "update-check-result", "pid": 44,
            "reason": "known-bad", "result": "available",
            "sequence": 8, "status": "KnownBad",
        })

    def test_update_result_parser_accepts_plain_none(self):
        row = {
            "event": "update-check-result", "pid": 44,
            "detail": "none",
        }
        self.assertEqual(funnel._update_result_observation(row), {
            "event": "update-check-result", "pid": 44,
            "reason": "", "result": "none",
            "sequence": None, "status": "",
        })

    def test_reused_profile_launch_appends_extra_arguments(self):
        executable = (self.root / "WoT Reborn" / "Game" / "WoT"
                      / "Binaries" / "Win64" / "WoT.exe")
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"fixture")
        profile = self.root / "profile"
        profile.mkdir()
        fake_process = mock.Mock(pid=123)
        with mock.patch.object(
                funnel.subprocess, "Popen", return_value=fake_process) as popen:
            client = funnel._launch_reused_profile(
                executable, profile, "Win64", self.root / "logs", "normal",
                "http://127.0.0.1:1/installer",
                extra_args=["-wotskipautoinstall"])
        self.assertEqual(popen.call_args.args[0][-1], "-wotskipautoinstall")
        client.close_logs()

    def test_reused_profile_scrubs_inherited_endpoint_overrides(self):
        # Under the one-folder layout a Win64 image lives below Game\.
        executable = self.root / "WoT Reborn" / "Game" / "WoT.exe"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"fixture")
        profile = self.root / "profile endpoint isolation"
        profile.mkdir()
        fake_process = mock.Mock(pid=124)
        explicit_announcement = "http://127.0.0.1:2345/announcement.json"
        explicit_release_base = "http://127.0.0.1:2345/release-base"
        inherited = {
            funnel.TEST_ANNOUNCEMENT_URL_ENV:
                "https://public.invalid/announcement.json",
            funnel.TEST_RELEASE_BASE_URL_ENV:
                "https://public.invalid/releases",
        }
        with mock.patch.dict(funnel.os.environ, inherited), mock.patch.object(
                funnel.subprocess, "Popen", return_value=fake_process) as popen:
            client = funnel._launch_reused_profile(
                executable, profile, "Win64", self.root / "logs", "isolated",
                explicit_announcement,
                release_base_url=explicit_release_base)
        environment = popen.call_args.kwargs["env"]
        self.assertEqual(
            environment[funnel.TEST_ANNOUNCEMENT_URL_ENV],
            explicit_announcement)
        self.assertEqual(
            environment[funnel.TEST_RELEASE_BASE_URL_ENV],
            explicit_release_base)
        client.close_logs()

    def test_receipt_schema_is_exact_and_matrix_ordered(self):
        receipt = funnel._new_receipt("Mac")
        receipt["completed_utc"] = funnel._utc_now()
        funnel._validate_receipt_schema(receipt)
        self.assertEqual(
            [cell["name"] for cell in receipt["cells"]],
            list(funnel._cell_names("Mac")),
        )
        receipt["unknown"] = True
        with self.assertRaisesRegex(funnel.FunnelFailure, "not exact"):
            funnel._validate_receipt_schema(receipt)
        win64 = funnel._new_receipt("Win64")
        self.assertEqual(
            [cell["name"] for cell in win64["cells"]],
            list(funnel._cell_names("Win64")),
        )


    def test_nested_release_and_installer_schema_is_exact(self):
        receipt = funnel._new_receipt("Mac")
        receipt["release"] = {"unexpected": True}
        with self.assertRaisesRegex(funnel.FunnelFailure, "release binding"):
            funnel._validate_receipt_schema(receipt)
        receipt = funnel._new_receipt("Mac")
        receipt["installer"] = {"path": "only-one-field"}
        with self.assertRaisesRegex(funnel.FunnelFailure, "installer binding"):
            funnel._validate_receipt_schema(receipt)


    def test_atomic_receipt_write_leaves_no_temporary(self):
        path = self.root / "evidence" / "funnel-receipt.json"
        receipt = funnel._new_receipt("Mac")
        funnel._atomic_write_json(path, receipt)
        self.assertEqual(
            json.loads(path.read_text())["format"], funnel.RECEIPT_FORMAT
        )
        self.assertEqual(
            list(path.parent.glob(".funnel-receipt.json.*.tmp")), []
        )


    def test_mac_state_root_matches_packaged_runtime_directory(self):
        profile = self.root / "profile"
        self.assertEqual(
            funnel._state_root(profile, "Mac"),
            profile / "Library" / "Application Support" / "WoT Reborn",
        )

    @unittest.skipUnless(sys.platform == "darwin", "POSIX-host Mac runner contract")
    def test_mac_uninstall_cell_removes_profile_update_temp_and_app(self):
        tree = self.root / "Install With Spaces" / "WoT Reborn.app"
        executable = tree / "Contents" / "MacOS" / "WoT-Mac-Test"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"test executable")
        executable.chmod(0o755)
        (tree / "Contents" / "Info.plist").write_bytes(
            funnel.plistlib.dumps({"CFBundleExecutable": "WoT-Mac-Test"}))
        installer = tree.parent / funnel.INSTALLER_NAME
        installer.write_bytes(b"protected adjacent installer")
        profile = self.root / "profiles" / "mac uninstall"
        state_root = funnel._state_root(profile, "Mac")
        state_root.mkdir(parents=True)
        (state_root / "payload.bin").write_bytes(b"payload")
        session_gate = state_root / "session.lock"
        session_gate.parent.mkdir(exist_ok=True)
        session_gate.touch(mode=0o600)
        log_root = self.root / "evidence" / "mac-uninstall"

        fake_process = mock.Mock()
        fake_process.pid = 7712
        fake_process.poll.return_value = 0
        fake_process.wait.return_value = 0
        fake_client = mock.Mock()
        fake_client.process = fake_process
        fake_client.command = []

        def launch(executable_arg, profile_arg, platform_name, log_arg,
                   label, announcement_url, **kwargs):
            self.assertEqual(executable_arg, executable.resolve())
            self.assertEqual(profile_arg, profile.resolve())
            self.assertEqual(platform_name, "Mac")
            self.assertRegex(
                announcement_url,
                r"^http://127\.0\.0\.1:[0-9]+/announcement\.json$")
            self.assertRegex(
                kwargs["release_base_url"],
                r"^http://127\.0\.0\.1:[0-9]+/release-base$")
            self.assertEqual(kwargs["extra_args"], [
                "-wottestuninstall=confirm"])
            command = [
                str(executable_arg),
                f"-wottestpreferencesdir={funnel._preferences_root(profile_arg, 'Mac')}",
                "-wottestuninstall=confirm",
            ]
            fake_client.command = command
            update_dir = tree.parent / ".wot-update"
            shutil.rmtree(state_root)
            shutil.rmtree(update_dir)
            shutil.rmtree(tree)
            result = {
                "answer": "confirm",
                "failure_reason": "",
                "format": "wot-uninstall-result-v1",
                "remaining_paths": [],
                "removed_paths": [
                    str(state_root.resolve()), str(update_dir.resolve()),
                    str(tree.resolve()),
                ],
            }
            result_path = state_root.parent / "wot-uninstall-result.json"
            result_path.write_text(json.dumps(result), encoding="utf-8")
            return fake_client

        with mock.patch.object(
                funnel, "_launch_reused_profile", side_effect=launch):
            evidence = funnel._run_mac_uninstall_cell(
                tree, profile, installer, log_root, timeout=1.0)

        self.assertFalse(state_root.exists())
        self.assertFalse((tree.parent / ".wot-update").exists())
        self.assertFalse(tree.exists())
        self.assertTrue(installer.is_file())
        self.assertTrue(
            (tree.parent / ".wot-uninstall-funnel-sentinel").is_file())
        self.assertTrue(evidence["postconditions"]["bundle_absent"])
        self.assertTrue(evidence["postconditions"]["installer_unchanged"])
        self.assertTrue(evidence["postconditions"]["session_gate_absent"])
        self.assertEqual(
            evidence["session_gate"]["before"]["path"],
            str(session_gate.resolve()))
        self.assertTrue(
            evidence["postconditions"]["sibling_sentinel_unchanged"])
        self.assertEqual(evidence["request_transcript"], [])
        self.assertEqual(evidence["process"]["exit_status"], 0)
        funnel.formats.validate_update_cell_evidence(
            "mac-uninstall", evidence, "Mac")

    @unittest.skipUnless(sys.platform == "darwin", "POSIX-host Mac runner contract")
    def test_mac_uninstall_timeout_terminates_client(self):
        tree = self.root / "Install With Spaces" / "WoT Reborn.app"
        executable = tree / "Contents" / "MacOS" / "WoT-Mac-Test"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"test executable")
        executable.chmod(0o755)
        (tree / "Contents" / "Info.plist").write_bytes(
            funnel.plistlib.dumps({"CFBundleExecutable": "WoT-Mac-Test"}))
        installer = tree.parent / funnel.INSTALLER_NAME
        installer.write_bytes(b"protected adjacent installer")
        profile = self.root / "profiles" / "mac uninstall timeout"
        state_root = funnel._state_root(profile, "Mac")
        session_gate = state_root / "session.lock"
        session_gate.parent.mkdir(parents=True)
        session_gate.touch(mode=0o600)

        fake_process = mock.Mock()
        fake_process.pid = 7713
        fake_process.poll.return_value = None
        fake_client = mock.Mock()
        fake_client.process = fake_process
        fake_client.command = []
        fake_client.terminate.side_effect = funnel.FunnelFailure(
            "client notes were not ready")

        with mock.patch.object(
                funnel, "_launch_reused_profile", return_value=fake_client), \
                mock.patch.object(
                    funnel, "_wait_for_mac_uninstall_result",
                    side_effect=funnel.FunnelFailure("uninstall timed out")), \
                mock.patch.object(
                    funnel, "_instance_tree_kill",
                    return_value="SIGKILL") as hard_kill:
            with self.assertRaisesRegex(funnel.FunnelFailure, "timed out"):
                funnel._run_mac_uninstall_cell(
                    tree, profile, installer, self.root / "evidence",
                    timeout=1.0)

        fake_client.terminate.assert_called_once_with(1.0)
        hard_kill.assert_called_once_with(fake_process)
        fake_process.wait.assert_called_once_with(timeout=1.0)
        fake_client.close_logs.assert_called()

    @unittest.skipUnless(sys.platform == "darwin", "POSIX-host Mac runner contract")
    def test_mac_uninstall_rejects_deleting_bundle_parent_and_installer(self):
        tree = self.root / "parent deletion" / "WoT Reborn.app"
        executable = tree / "Contents" / "MacOS" / "WoT-Mac-Test"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"test executable")
        executable.chmod(0o755)
        (tree / "Contents" / "Info.plist").write_bytes(
            funnel.plistlib.dumps({"CFBundleExecutable": "WoT-Mac-Test"}))
        installer = tree.parent / funnel.INSTALLER_NAME
        installer.write_bytes(b"protected adjacent installer")
        profile = self.root / "profiles" / "parent deletion"
        state_root = funnel._state_root(profile, "Mac")
        state_root.mkdir(parents=True)
        session_gate = state_root / "session.lock"
        session_gate.parent.mkdir(exist_ok=True)
        session_gate.touch(mode=0o600)

        fake_process = mock.Mock(pid=7714)
        fake_process.poll.return_value = 0
        fake_process.wait.return_value = 0
        fake_client = mock.Mock(process=fake_process, command=[
            str(executable),
            f"-wottestpreferencesdir={funnel._preferences_root(profile, 'Mac')}",
            "-wottestuninstall=confirm",
        ])

        def launch(*_args, **_kwargs):
            shutil.rmtree(state_root)
            shutil.rmtree(tree.parent)
            result_path = state_root.parent / "wot-uninstall-result.json"
            result_path.write_text(json.dumps({
                "answer": "confirm",
                "failure_reason": "",
                "format": "wot-uninstall-result-v1",
                "remaining_paths": [],
                "removed_paths": [str(state_root), str(tree),
                                  str(tree.parent / ".wot-update")],
            }), encoding="utf-8")
            return fake_client

        with mock.patch.object(
                funnel, "_launch_reused_profile", side_effect=launch):
            with self.assertRaisesRegex(
                    funnel.FunnelFailure, "protected directory"):
                funnel._run_mac_uninstall_cell(
                    tree, profile, installer, self.root / "parent evidence",
                    timeout=1.0)

    def test_mac_fresh_profile_library_allows_only_root_and_os_residue(self):
        profile = self.root / "profile with space"
        state = funnel._state_root(profile, "Mac")
        (state / "Preferences").mkdir(parents=True)
        (state / "Preferences" / "WoTUserPreferences").write_text("prefs")
        allowed_files = (
            profile / "Library" / "Caches" / "com.aleiby.wot" / "cache",
            profile / "Library" / "Saved Application State"
            / "com.aleiby.wot.savedState" / "state",
            profile / "Library" / "HTTPStorages" / "com.aleiby.wot" / "db",
            profile / "Library" / "Preferences" / "com.aleiby.wot.plist",
            profile / "Library" / "Logs" / "DiagnosticReports" / "WoT.crash",
        )
        for path in allowed_files:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("os")
        evidence = funnel._require_mac_profile_library_contained(profile)
        self.assertEqual(evidence["outside_root"], [])

        leaked = (profile / "Library" / "Application Support" / "Epic"
                  / "WoT" / "leak")
        leaked.parent.mkdir(parents=True)
        leaked.write_text("legacy")
        with self.assertRaisesRegex(funnel.FunnelFailure, "outside"):
            funnel._require_mac_profile_library_contained(profile)

        second_profile = self.root / "profile with fake plist directory"
        fake_plist_child = (second_profile / "Library" / "Preferences"
                            / "com.aleiby.wot.plist" / "child")
        fake_plist_child.parent.mkdir(parents=True, exist_ok=True)
        fake_plist_child.write_text("not an OS plist")
        with self.assertRaisesRegex(funnel.FunnelFailure, "outside"):
            funnel._require_mac_profile_library_contained(second_profile)

    def test_engine_machine_id_store_is_the_only_engine_exception(self):
        # UE's shared per-machine identifier file is allowed,
        # holding only [Unreal Engine/Identifiers] MachineId.
        profile = self.root / "engine identifier profile"
        (funnel._state_root(profile, "Mac") / "Preferences").mkdir(parents=True)
        store = profile / "Library" / funnel.ENGINE_IDENTIFIER_STORE
        store.parent.mkdir(parents=True)
        store.write_text("[Unreal Engine/Identifiers]\n"
                         "MachineId=0123456789ABCDEF0123456789ABCDEF\n")
        evidence = funnel._require_mac_profile_library_contained(profile)
        self.assertEqual(evidence["outside_root"], [])
        self.assertEqual(evidence["engine_identifier_store"], "machine-id-only")

        store.write_text("[Unreal Engine/Identifiers]\n"
                         "MachineId=0123456789ABCDEF0123456789ABCDEF\n"
                         "AccountId=leak\n")
        with self.assertRaisesRegex(funnel.FunnelFailure, "more than the engine MachineId"):
            funnel._require_mac_profile_library_contained(profile)

        store.write_text("[Unreal Engine/Identifiers]\n"
                         "MachineId=0123456789ABCDEF0123456789ABCDEF\n")
        (store.parent / "Other.ini").write_text("leak")
        with self.assertRaisesRegex(funnel.FunnelFailure, "outside"):
            funnel._require_mac_profile_library_contained(profile)

    def test_mac_profile_evidence_passes_the_receipt_validator(self):
        # The runner's real containment record must satisfy the receipt's
        # exact schema; a field added on one side only failed a live funnel.
        release_json = release_fixtures.write_guarded_release(
            self.root / "profile parity release", "Mac")
        receipt_path = release_fixtures.write_funnel_receipt(
            self.root / "profile parity receipt.json", release_json)
        release = funnel.formats.load_guarded_release(release_json, "Mac")
        for label, machine_id in (("absent", None),
                                  ("machine-id-only",
                                   "0123456789ABCDEF0123456789ABCDEF")):
            with self.subTest(store=label):
                profile = self.root / f"parity profile {label}"
                (funnel._state_root(profile, "Mac") / "Preferences").mkdir(
                    parents=True)
                if machine_id:
                    store = profile / "Library" / funnel.ENGINE_IDENTIFIER_STORE
                    store.parent.mkdir(parents=True)
                    store.write_text("[Unreal Engine/Identifiers]\n"
                                     f"MachineId={machine_id}\n")
                document = json.loads(receipt_path.read_text(encoding="utf-8"))
                fresh = next(cell for cell in document["cells"]
                             if cell["name"] == "fresh-first-run")
                fresh["evidence"]["mac_profile_library"] = (
                    funnel._require_mac_profile_library_contained(profile))
                receipt_path.write_text(json.dumps(document), encoding="utf-8")
                funnel.formats.validate_funnel_receipt(
                    receipt_path, "Mac", release,
                    funnel.formats.FUNNEL_REQUIRED_CELLS)

    def test_update_failed_notice_is_exactly_in_preferences(self):
        profile = self.root / "profile with space"
        notice = (funnel._preferences_root(profile, "Mac")
                  / "WoTUpdateFailedNotice")
        notice.parent.mkdir(parents=True)
        notice.write_text(
            "WUF1\nfailed=9\nrestored=8\nstate=pending\n",
            encoding="utf-8")
        self.assertEqual(
            funnel._find_update_failed_notice(profile, "Mac", 9, 8), notice)

    def test_only_current_manifest_is_published_cache_evidence(self):
        state = self.root / "state"
        building = state / "ContentCache" / "building"
        building.mkdir(parents=True)
        (building / "manifest.json").write_text("{}")
        self.assertEqual(funnel._manifests(state), [])
        current = state / "ContentCache" / "current"
        current.mkdir()
        manifest = current / "manifest.json"
        manifest.write_text("{}")
        self.assertEqual(funnel._manifests(state), [manifest])


    def test_runtime_endpoint_preflight_refuses_missing_seam(self):
        executable = self.root / "game"
        executable.write_bytes(b"ordinary binary")
        with self.assertRaisesRegex(funnel.FunnelFailure, "missing runtime"):
            funnel._require_runtime_endpoint_seam(executable)
        executable.write_bytes(
            b"prefix" + funnel.TEST_ANNOUNCEMENT_URL_ENV.encode() + b"suffix"
        )
        with self.assertRaisesRegex(funnel.FunnelFailure, "RELEASE_BASE"):
            funnel._require_runtime_endpoint_seam(executable)
        executable.write_bytes(
            funnel.TEST_ANNOUNCEMENT_URL_ENV.encode() + b"\0"
            + funnel.TEST_RELEASE_BASE_URL_ENV.encode()
        )
        funnel._require_runtime_endpoint_seam(executable)

    def test_release_base_url_requires_loopback_path_marker(self):
        funnel._validate_release_base_url(
            "http://127.0.0.1:1234/test-release-base")
        for invalid in (
                "http://127.0.0.1:1234",
                "http://127.0.0.1:1234/",
                "https://127.0.0.1:1234/test-release-base",
                "http://localhost:1234/test-release-base"):
            with self.subTest(invalid=invalid), self.assertRaises(
                    funnel.FunnelFailure):
                funnel._validate_release_base_url(invalid)

    def test_mac_observer_authenticates_uid_path_token_profile_and_start(self):
        root = self.root / "scratch"
        executable = root / "WoT.app" / "Contents" / "MacOS" / "WoT"
        profile = self.root / "profile"
        token = "wot-funnel-token"
        rows = [{
            "argv": [str(executable), f"-wotnettesttoken={token}",
                     f"-wottestpreferencesdir={profile}"],
            "command": "fixture",
            "parent_pid": 1,
            "pid": 4321,
            "start_time": 200.0,
            "uid": 501,
        }]
        observer = funnel.MacProcessObserver(
            ps_reader=lambda: rows, proc_path=lambda _pid: str(executable),
            uid=501)
        authenticated = observer.authenticate(
            4321, allowed_roots=[root], token=token, profile=profile,
            started_after=199.5)
        self.assertEqual(authenticated["executable_path"], str(executable))
        with self.assertRaisesRegex(funnel.FunnelFailure, "token"):
            observer.authenticate(
                4321, allowed_roots=[root], token="foreign", profile=profile)
        with self.assertRaisesRegex(funnel.FunnelFailure, "outside"):
            observer.authenticate(
                4321, allowed_roots=[self.root / "other"], token=token,
                profile=profile)

    def test_mac_adopt_accepts_a_designed_quick_exit_only_when_asked(self):
        # A bad build can exit right after writing "started", before ps
        # can authenticate it; KERN_PROCARGS2 then fails on the zombie.
        executable = self.root / "WoT.app" / "Contents" / "MacOS" / "WoT"

        def zombie_argv(pid):
            raise funnel.FunnelFailure(
                f"KERN_PROCARGS2 could not size pid {pid} argv (errno 22)")

        observer = funnel.MacProcessObserver(
            ps_reader=lambda: [{"argv": [], "command": "WoT", "parent_pid": 1,
                                "pid": 4321, "start_time": 200.0, "uid": 501}],
            proc_path=lambda _pid: str(executable), proc_argv=zombie_argv,
            uid=501)
        exited = mock.Mock(pid=4321, args=[str(executable)])
        exited.poll.return_value = 1
        with self.assertRaisesRegex(funnel.FunnelFailure, "could not adopt"):
            observer.adopt(exited)
        observer.adopt(exited, may_exit=True)

    def test_kern_procargs2_parser_preserves_spaces_in_one_argument(self):
        executable = b"/tmp/WoT.app/Contents/MacOS/WoT-Mac-Test"
        argv = [executable, b"-wottestpreferencesdir=/tmp/Profile With Space",
                b"-wotnettesttoken=wot-funnel-token"]
        raw = (struct.pack("=i", len(argv)) + executable + b"\0\0"
               + b"\0".join(argv) + b"\0ENV=value\0")
        self.assertEqual(
            funnel._parse_kern_procargs2(raw),
            [value.decode() for value in argv])

    def test_mac_observer_records_native_argv_not_lossy_ps_tokens(self):
        root = self.root / "scratch with space"
        executable = root / "WoT.app" / "Contents" / "MacOS" / "WoT"
        profile = self.root / "profile with space"
        exact = [str(executable), f"-wottestpreferencesdir={profile}",
                 "-wotnettesttoken=wot-funnel-token"]
        row = {
            "argv": [str(executable), "-wottestpreferencesdir="
                     + str(profile).split(" ", 1)[0], "with", "space",
                     "-wotnettesttoken=wot-funnel-token"],
            "command": "lossy", "parent_pid": 1, "pid": 4321,
            "start_time": 200.0, "uid": 501,
        }
        observer = funnel.MacProcessObserver(
            ps_reader=lambda: [row], proc_path=lambda _pid: str(executable),
            proc_argv=lambda _pid: list(exact), uid=501)
        authenticated = observer.authenticate(
            4321, allowed_roots=[root], token="wot-funnel-token",
            profile=profile)
        self.assertEqual(authenticated["argv"], exact)
        self.assertEqual(observer.recorded(4321)["argv"], exact)


    def test_mac_authenticated_wait_prefilters_unique_argv_binding(self):
        profile = self.root / "profile"
        token = "wot-funnel-target"
        rows = [
            {"argv": ["/bin/foreign"], "command": "foreign",
             "parent_pid": 1, "pid": pid, "start_time": 100.0,
             "uid": 501}
            for pid in range(10, 20)
        ]
        target = {
            "argv": ["/tmp/WoT", f"-wotnettesttoken={token}",
                     f"-wottestpreferencesdir={profile}"],
            "command": "target", "parent_pid": 1, "pid": 99,
            "start_time": 200.0, "uid": 501,
        }
        rows.append(target)
        observer = funnel.MacProcessObserver(
            ps_reader=lambda: list(rows), proc_path=lambda pid: f"/tmp/{pid}",
            uid=501)
        authenticated = {**target, "executable_path": "/tmp/WoT"}

        def authenticate(pid, **_kwargs):
            if pid != 99:
                raise funnel.FunnelFailure("foreign pid authenticated")
            return authenticated

        observer.authenticate = mock.Mock(side_effect=authenticate)
        result = observer.wait_for_authenticated(
            allowed_roots=[Path("/tmp")], token=token, profile=profile,
            started_after=150.0, timeout=0.1)
        self.assertEqual(result, authenticated)
        observer.authenticate.assert_called_once()

    def test_posix_directory_identity_uses_python39_os_stat_api(self):
        information = mock.Mock(st_dev=42, st_ino=84)
        candidate = self.root / "tree"
        with mock.patch.object(
                funnel.os, "stat", return_value=information) as stat:
            self.assertEqual(
                funnel._posix_directory_identity(candidate), (42, 84, 0))
        stat.assert_called_once_with(candidate, follow_symlinks=False)

    @unittest.skipUnless(sys.platform == "darwin", "POSIX-host Mac runner contract")
    def test_posix_owned_path_check_rejects_case_distinct_sibling(self):
        owned = self.root / "Scratch"
        sibling = self.root / "scratch" / "WoT"
        sibling.parent.mkdir()
        sibling.write_bytes(b"game")
        self.assertFalse(funnel._path_is_under(str(sibling), owned))


    def test_mac_observer_finds_detached_helper_by_wait_pid_argument(self):
        helper = self.root / "helper" / "WoTUpdater"
        row = {
            "argv": [str(helper), "--recover", "--wait-pid=4321"],
            "command": "fixture", "parent_pid": 1, "pid": 9876,
            "start_time": 200.0, "uid": 501,
        }
        observer = funnel.MacProcessObserver(
            ps_reader=lambda: [row], proc_path=lambda _pid: str(helper),
            uid=501)
        found = observer.find_start(mark=0, parent_pid=4321, timeout=0.01)
        self.assertEqual(found["pid"], 9876)
        self.assertEqual(found["parent_pid"], 1)
        self.assertEqual(found["name"], "WoTUpdater")


    def test_mac_safe_termination_reauthenticates_and_escalates_by_pid(self):
        root = self.root / "scratch"
        executable = root / "WoTUpdater"
        rows = [{
            "argv": [str(executable), "--token=owned"],
            "command": "fixture", "parent_pid": 1, "pid": 4444,
            "start_time": 200.0, "uid": 501,
        }]
        observer = funnel.MacProcessObserver(
            ps_reader=lambda: rows, proc_path=lambda _pid: str(executable),
            uid=501)
        with mock.patch.object(funnel.os, "kill") as kill, \
                mock.patch.object(
                    funnel, "_mac_pid_exists", side_effect=[True, False]), \
                mock.patch.object(funnel.time, "sleep"):
            result = funnel._terminate_mac_pid(
                4444, observer=observer, allowed_roots=[root], timeout=0.1)
        self.assertEqual(result["signals"], ["SIGTERM"])
        kill.assert_called_once_with(4444, funnel.signal.SIGTERM)


    def test_failure_cleanup_skips_authenticated_stopped_identities(self):
        scratch = self.root / "scratch"
        game = scratch / "cell-trees" / "cell" / "WoT.app" \
            / "Contents" / "MacOS" / "WoT-Mac-Test"
        rows = [{
            "argv": [str(game)], "command": "game", "parent_pid": 1,
            "pid": 100, "start_time": 100.0, "uid": 501,
        }]
        observer = funnel.MacProcessObserver(
            ps_reader=lambda: list(rows), proc_path=lambda _pid: str(game),
            uid=501)
        rows.clear()
        with mock.patch.object(
                observer, "authenticate",
                side_effect=AssertionError("stopped identity was rechecked")), \
                mock.patch.object(funnel, "_terminate_mac_pid") as terminate:
            self.assertEqual(
                funnel._cleanup_owned_mac_processes(observer, scratch), [])
        terminate.assert_not_called()

    def test_failure_cleanup_later_start_supersedes_earlier_stop(self):
        scratch = self.root / "scratch"
        game = scratch / "cell-trees" / "cell" / "WoT.app" \
            / "Contents" / "MacOS" / "WoT-Mac-Test"
        rows = [{
            "argv": [str(game)], "command": "game", "parent_pid": 1,
            "pid": 100, "start_time": 100.0, "uid": 501,
        }]
        observer = funnel.MacProcessObserver(
            ps_reader=lambda: list(rows), proc_path=lambda _pid: str(game),
            uid=501)
        rows.clear()
        observer.poll()
        rows.append({
            "argv": [str(game)], "command": "game", "parent_pid": 1,
            "pid": 100, "start_time": 100.0, "uid": 501,
        })
        observer.poll()
        with mock.patch.object(
                funnel, "_terminate_mac_pid",
                return_value={"action": "terminated", "pid": 100,
                              "signals": ["SIGTERM"]}) as terminate:
            cleanup = funnel._cleanup_owned_mac_processes(observer, scratch)
        self.assertEqual([row["pid"] for row in cleanup], [100])
        terminate.assert_called_once()

    def test_mac_quiescence_counts_complete_slow_polls_not_wall_clock(self):
        slow_clock = iter((0.0, 15.0, 15.1, 30.1, 30.2, 45.2))
        with mock.patch.object(
                funnel, "_cleanup_owned_mac_processes",
                side_effect=[[], [], []]) as cleanup, \
                mock.patch.object(
                    funnel.time, "monotonic",
                    side_effect=lambda: next(slow_clock)), \
                mock.patch.object(funnel.time, "sleep"):
            result = funnel._quiesce_owned_mac_processes(
                mock.sentinel.observer, self.root / "scratch")
        self.assertEqual(cleanup.call_count, 3)
        self.assertEqual(result, [{
            "action": "mac-quiescence-proven",
            "consecutive_empty_polls": 3,
            "polls": [
                {"actions": 0, "duration_seconds": 15.0, "pass": 1},
                {"actions": 0, "duration_seconds": 15.0, "pass": 2},
                {"actions": 0, "duration_seconds": 15.0, "pass": 3},
            ],
        }])

    def test_mac_direct_child_termination_uses_popen_handle(self):
        root = self.root / "scratch"
        executable = str(root / "WoT")
        authenticated = {
            "argv": [executable], "command": "fixture", "parent_pid": 1,
            "pid": 4444, "start_time": 200.0, "uid": 501,
            "executable_path": executable, "authenticated": True,
        }
        observer = mock.Mock()
        observer.authenticate.return_value = authenticated
        child = mock.Mock(pid=4444)
        child.poll.return_value = None
        child.wait.side_effect = [subprocess.TimeoutExpired("WoT", 0.1), 0]
        with mock.patch.object(funnel.os, "kill") as kill:
            result = funnel._terminate_mac_pid(
                4444, observer=observer, allowed_roots=[root],
                direct_process=child, timeout=0.1)
        self.assertEqual(result["signals"], ["SIGTERM", "SIGKILL"])
        child.terminate.assert_called_once_with()
        child.kill.assert_called_once_with()
        kill.assert_not_called()

    @unittest.skipUnless(sys.platform == "darwin", "POSIX-host Mac runner contract")
    def test_mac_safe_termination_forced_timeout_reaches_sigkill(self):
        root = self.root / "scratch"
        executable = root / "WoTUpdater"
        row = {"argv": [str(executable), "--token=owned"],
               "command": "fixture", "parent_pid": 1, "pid": 4444,
               "start_time": 200.0, "uid": 501,
               "executable_path": str(executable), "authenticated": True}
        observer = mock.Mock()
        observer.authenticate.return_value = row
        observer.identity.side_effect = lambda value: (
            value["pid"], value["uid"], value["start_time"],
            value["executable_path"])
        with mock.patch.object(funnel.os, "kill") as kill, \
                mock.patch.object(
                    funnel, "_mac_pid_exists", side_effect=[True, False]), \
                mock.patch.object(
                    funnel.time, "monotonic",
                    side_effect=[0.0, 0.0, 0.2, 0.2, 0.2]), \
                mock.patch.object(funnel.time, "sleep"):
            result = funnel._terminate_mac_pid(
                4444, observer=observer, allowed_roots=[root], timeout=0.1)
        self.assertEqual(result["signals"], ["SIGTERM", "SIGKILL"])
        self.assertEqual(kill.call_args_list, [
            mock.call(4444, funnel.signal.SIGTERM),
            mock.call(4444, funnel.signal.SIGKILL),
        ])
        self.assertEqual(observer.authenticate.call_count, 3)

    def test_mac_safe_termination_does_not_kill_replacement_identity(self):
        root = self.root / "scratch"
        executable = str(root / "WoTUpdater")
        original = {"pid": 4444, "uid": 501, "start_time": 200.0,
                    "executable_path": executable}
        replacement = {**original, "start_time": 201.0}
        observer = mock.Mock()
        observer.authenticate.side_effect = [original, original, replacement]
        observer.identity.side_effect = lambda value: (
            value["pid"], value["uid"], value["start_time"],
            value["executable_path"])
        with mock.patch.object(funnel.os, "kill") as kill, \
                mock.patch.object(funnel, "_mac_pid_exists", return_value=True), \
                mock.patch.object(
                    funnel.time, "monotonic", side_effect=[0.0, 0.0, 0.2]), \
                mock.patch.object(funnel.time, "sleep"):
            result = funnel._terminate_mac_pid(
                4444, observer=observer, allowed_roots=[root], timeout=0.1)
        self.assertEqual(result["action"], "aborted-identity-change")
        self.assertEqual(result["signals"], ["SIGTERM"])
        kill.assert_called_once_with(4444, funnel.signal.SIGTERM)

    def test_mac_observer_detects_pid_reuse_as_stop_then_start(self):
        executable = str(self.root / "scratch" / "WoT")
        row = {"argv": [executable], "command": "fixture", "parent_pid": 1,
               "pid": 4321, "start_time": 200.0, "uid": 501}
        rows = [row]
        observer = funnel.MacProcessObserver(
            ps_reader=lambda: [dict(value) for value in rows],
            proc_path=lambda _pid: executable, uid=501)
        rows[0] = {**row, "start_time": 300.0}
        events = observer.poll()
        self.assertEqual([event["kind"] for event in events[-2:]],
                         ["stop", "start"])
        self.assertEqual({event["start_time"] for event in events[-2:]},
                         {200.0, 300.0})

    def test_mac_proc_pidpath_rejects_errno_and_full_buffer(self):
        class Function:
            def __init__(self, result, error=0):
                self.result = result
                self.error = error

            def __call__(self, _pid, buffer, size):
                funnel.ctypes.set_errno(self.error)
                if self.result > 0:
                    buffer.raw = b"/" + b"a" * (size - 1)
                return self.result

        class Library:
            pass

        library = Library()
        library.proc_pidpath = Function(0, funnel.errno.ESRCH)
        with self.assertRaisesRegex(funnel.FunnelFailure, "errno"):
            funnel._mac_proc_pidpath(123, library)
        library.proc_pidpath = Function(4096)
        with self.assertRaisesRegex(funnel.FunnelFailure, "truncated"):
            funnel._mac_proc_pidpath(123, library)


    def test_mac_installed_tree_identity_includes_symlink_target(self):
        bundle = self.root / "WoT.app"
        contents = bundle / "Contents"
        contents.mkdir(parents=True)
        payload = contents / "payload"
        payload.write_bytes(b"payload")
        link = contents / "payload-link"
        link.symlink_to("payload")
        before = funnel._installed_tree_root(bundle, "Mac")
        link.unlink()
        link.symlink_to("other")
        after = funnel._installed_tree_root(bundle, "Mac")
        self.assertNotEqual(before, after)

    def _await_requests(self, server, count, timeout=5.0):
        """Wait for the loopback server to have recorded `count` requests.

        The handler appends its receipt in a finally block on the serving
        thread, after the response body is written, so a client that has
        finished reading can still observe a short log. Poll rather than
        assume the append already happened.
        """
        deadline = time.monotonic() + timeout
        while True:
            requests = server.requests(require_settled=False)
            if len(requests) >= count or time.monotonic() >= deadline:
                return requests
            time.sleep(0.01)

    def test_loopback_server_serves_full_and_exact_range_and_logs_all(self):
        installer = self.root / "fixture.exe"
        installer.write_bytes(b"0123456789")
        log = self.root / "requests.jsonl"
        with funnel.LoopbackInstallerServer(installer, log) as server:
            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server.server_port, timeout=5
            )
            connection.request("GET", "/installer")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), b"0123456789")
            connection.close()

            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server.server_port, timeout=5
            )
            connection.request(
                "GET", "/installer",
                headers={"Range": "bytes=4-", "If-Range": server.server.etag},
            )
            response = connection.getresponse()
            self.assertEqual(response.status, 206)
            self.assertEqual(
                response.getheader("Content-Range"), "bytes 4-9/10"
            )
            self.assertEqual(response.read(), b"456789")
            connection.close()
            requests = self._await_requests(server, 2)
        self.assertEqual([item["status"] for item in requests], [200, 206])
        self.assertEqual(requests[1]["range_start"], 4)
        self.assertEqual(requests[1]["response_bytes"], 6)

    def test_loopback_server_404_and_unsatisfied_range_are_receipted(self):
        installer = self.root / "fixture.exe"
        installer.write_bytes(b"abc")
        log = self.root / "requests.jsonl"
        with funnel.LoopbackInstallerServer(installer, log) as server:
            for path, headers, expected in (
                ("/announcement.json", {}, 404),
                ("/installer", {"Range": "bytes=3-"}, 416),
            ):
                connection = http.client.HTTPConnection(
                    "127.0.0.1", server.server.server_port, timeout=5
                )
                connection.request("GET", path, headers=headers)
                response = connection.getresponse()
                self.assertEqual(response.status, expected)
                response.read()
                connection.close()
            requests = self._await_requests(server, 2)
        self.assertEqual([item["status"] for item in requests], [404, 416])

    def test_loopback_server_rejects_query_and_non_exact_download_routes(self):
        installer = self.root / "fixture.exe"
        installer.write_bytes(b"abc")
        asset = self.root / "asset.json"
        asset.write_bytes(b"{}")
        allowed = "/release-base/releases/download/safe-tag/safe-leaf.json"
        with funnel.LoopbackInstallerServer(
            installer, self.root / "query-requests.jsonl",
            release_assets={allowed: asset},
        ) as server:
            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server.server_port, timeout=5)
            connection.request("GET", allowed + "?ignored=true")
            response = connection.getresponse()
            self.assertEqual(response.status, 404)
            response.read()
            connection.close()
            requests = self._await_requests(server, 1)
        self.assertEqual(requests[0]["path"], allowed + "?ignored=true")
        self.assertEqual(requests[0]["status"], 404)

        for invalid in (
                "/release-base/releases/download/tag/dir/leaf.json",
                "/release-base/releases/download/tag/leaf.json?query=1",
                "/release-base/releases/download/tag/../leaf.json"):
            with self.subTest(route=invalid), self.assertRaisesRegex(
                    funnel.FunnelFailure, "route is not allowed"):
                funnel.LoopbackInstallerServer(
                    installer, self.root / "invalid-route.jsonl",
                    release_assets={invalid: asset})

    def test_loopback_server_serves_update_pairs_and_ranged_archive(self):
        installer = self.root / "fixture.exe"
        installer.write_bytes(b"installer")
        announcement = self.root / "announcement.json"
        announcement.write_bytes(b'{"fixture":true}')
        announcement_signature = self.root / "announcement.json.sig"
        announcement_signature.write_bytes(b"a" * 64)
        ring = self.root / "ring.json"
        ring.write_bytes(b'{"ring":true}')
        ring_signature = self.root / "ring.json.sig"
        ring_signature.write_bytes(b"r" * 64)
        manifest = self.root / "manifest.json"
        manifest.write_bytes(b'{"manifest":true}')
        manifest_signature = self.root / "manifest.json.sig"
        manifest_signature.write_bytes(b"m" * 64)
        archive = self.root / "update.zip"
        archive.write_bytes(b"0123456789")
        tag = "fixture-r2"
        prefix = f"/release-base/releases/download/{tag}"
        assets = {
            "/announcement.json": announcement,
            "/announcement.json.sig": announcement_signature,
            f"{prefix}/ring.json": ring,
            f"{prefix}/ring.json.sig": ring_signature,
            f"{prefix}/manifest.json": manifest,
            f"{prefix}/manifest.json.sig": manifest_signature,
            f"{prefix}/update.zip": archive,
        }
        log = self.root / "requests.jsonl"
        with funnel.LoopbackInstallerServer(
            installer, log, release_assets=assets,
        ) as server:
            self.assertEqual(
                server.release_base_url,
                f"http://127.0.0.1:{server.server.server_port}/release-base",
            )
            for route, expected in assets.items():
                connection = http.client.HTTPConnection(
                    "127.0.0.1", server.server.server_port, timeout=5
                )
                headers = {"Range": "bytes=4-"} if route.endswith(".zip") else {}
                connection.request("GET", route, headers=headers)
                response = connection.getresponse()
                self.assertEqual(response.status, 206 if headers else 200)
                self.assertEqual(
                    response.read(),
                    expected.read_bytes()[4:] if headers else expected.read_bytes(),
                )
                connection.close()
            requests = self._await_requests(server, len(assets))
        self.assertCountEqual(
            [item["path"] for item in requests], list(assets)
        )
        archive_request = next(
            item for item in requests if item["path"].endswith(".zip")
        )
        self.assertEqual(archive_request["range_start"], 4)

    def test_loopback_server_refuses_unsettled_request_evidence(self):
        installer = self.root / "fixture.exe"
        installer.write_bytes(b"abc")
        with funnel.LoopbackInstallerServer(
            installer, self.root / "requests.jsonl"
        ) as server:
            server.server.active_requests = 1
            with self.assertRaisesRegex(funnel.FunnelFailure, "in-flight"):
                server.requests()
            server.server.active_requests = 0

    def test_boot_failure_preempts_process_exit_observation(self):
        client = mock.Mock()
        state = (self.root / "profile" / "Library" / "Application Support"
                 / "WoT Reborn")
        failure = state / "UpdaterState" / funnel.BOOT_FAILURE_LEAF
        failure.parent.mkdir(parents=True)
        failure.write_text(json.dumps({
            "format": "wot-bootstrap-boot-failure-v1",
            "diagnostic_code": "GUARDED-SESSION-ABSENT",
        }))
        client.state_root = state
        client.boot_failure_path = self.root / "evidence" / "boot-failure.json"
        client.process.poll.return_value = 9
        with self.assertRaisesRegex(
            funnel.FunnelFailure, "GUARDED-SESSION-ABSENT"
        ):
            funnel.ClientProcess.poll_failure(client)
        self.assertEqual(
            json.loads(client.boot_failure_path.read_text())["diagnostic_code"],
            "GUARDED-SESSION-ABSENT",
        )

    def test_safe_win_extract_refuses_path_traversal(self):
        archive = self.root / "unsafe.zip"
        with zipfile.ZipFile(archive, "w") as handle:
            handle.writestr("../escape", b"x")
        destination = self.root / "extract"
        with self.assertRaisesRegex(funnel.FunnelFailure, "unsafe"):
            funnel._extract_release(archive, destination, "Win64")

    def test_installer_request_filter_is_path_exact(self):
        requests = [
            {"path": "/announcement.json", "status": 404},
            {"path": "/installer?x=1", "status": 206},
            {"path": "/installer", "status": 200},
        ]
        self.assertEqual(
            funnel._requests_for_installer(requests), [requests[2]]
        )

    def test_hosted_snapshot_must_use_a_neutral_shell_map(self):
        # The snapshot publishes UWorld::GetMapName(), i.e. the LEAF asset
        # name. Real evidence: acceptance-8 recorded "map": "WoTBootstrap".
        for accepted in ("WoTMapShell01", "WoTMapShell19"):
            funnel._require_hosted_neutral_shell(
                {"map": accepted, "netmode": 2})
        rejected = (
            "Arena_01",                          # the un-routed raw arena
            "/WoTNeutralShells/WoTMapShell01",   # pins the real snapshot
                                                 # format: a full package path
                                                 # is never emitted, so an
                                                 # assertion expecting one can
                                                 # never pass
            "WoTMapShell20",                     # outside the synthesized 19
            "WoTMapShell01Backup",               # prefix match is not proof
            "",
            None,
        )
        for value in rejected:
            with self.assertRaisesRegex(
                funnel.FunnelFailure, "neutral shell"
            ):
                funnel._require_hosted_neutral_shell(
                    {"map": value, "netmode": 2})

    def test_leave_to_front_end_requires_guarded_front_end_leaf_and_standalone(self):
        self.assertEqual(
            funnel.SESSION_EVIDENCE_FORMAT,
            "wot-funnel-session-evidence-v2",
        )
        # The exact joiner snapshot a host-join cell timed out on 180 s
        # after a successful LAN join and a deliberate WoTLeave.
        rc3_joiner = {
            "netmode": 0, "map": "WoTBootstrap",
            "session": "authorized=1 adopted=1 frontend=1 dedicated=0 "
                       "sessions=1 create_in_flight=0 leave=0 named=0 "
                       "last=none route=none find=complete ok=1 results=1 "
                       "lan=1 join=dispatched idx=0 results=1 oss=NULL",
        }
        for accepted in (
            rc3_joiner,
            {"map": "WoTBootstrap", "netmode": 0,
             "session": "leave=0 named=0"},
        ):
            with self.subTest(accepted=accepted):
                self.assertTrue(funnel._is_returned_front_end(accepted))
                funnel._require_returned_front_end(accepted)
        for rejected in (
            # No session readout is no evidence of a completed Leave.
            {"map": "WoTBootstrap", "netmode": 0},
            {"map": "WoTBootstrap", "netmode": 0, "session": "named=0"},
            # The uncooked dev front end is never loaded by a guarded build.
            {"map": "WoTFrontEnd", "netmode": 0},
            {"map": "/Game/WoT/Maps/WoTBootstrap", "netmode": 0},
            {"map": "WoTBootstrap", "netmode": 2},
            {"map": "WoTBootstrap", "netmode": 3},
            {"map": "WoTMapShell01", "netmode": 3},
            # Still holding the joined/hosted named session: Leave never ran.
            dict(rc3_joiner, session=rc3_joiner["session"].replace(
                "named=0", "named=1")),
            # Leave still destroying the session.
            dict(rc3_joiner, session=rc3_joiner["session"].replace(
                "leave=0", "leave=1")),
        ):
            with self.subTest(rejected=rejected):
                self.assertFalse(funnel._is_returned_front_end(rejected))
                with self.assertRaisesRegex(
                    funnel.FunnelFailure, "return to front end"
                ):
                    funnel._require_returned_front_end(rejected)

    def test_guarded_front_end_leaf_matches_the_leave_policy_in_source(self):
        # FWoTLeaveStateMachine::FrontEndURLForBuild(true) is the ONE place a
        # guarded Leave's destination is chosen; the funnel must wait for its
        # leaf, never a hand-copied map name that can drift again.
        source = (Path(__file__).resolve().parents[2] / "Source" / "WoT"
                  / "WoTFrontEnd.cpp").read_text(encoding="utf-8")
        match = re.search(
            r"FrontEndURLForBuild\(bool bGuardedBuild\)\s*\{\s*return "
            r"bGuardedBuild\s*\?\s*TEXT\(\"([^\"]+)\"\)", source)
        self.assertIsNotNone(match, "guarded leave URL not found in source")
        self.assertEqual(match.group(1).rsplit("/", 1)[-1],
                         funnel.GUARDED_FRONT_END_MAP)


@unittest.skipUnless(sys.platform == "darwin", "Darwin integration coverage")
class DarwinPrimitiveIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_libproc_observer_and_authenticated_pid_termination(self):
        executable = self.root / "owned-sleep"
        shutil.copyfile("/bin/sleep", executable)
        executable.chmod(0o755)
        subprocess.run(
            ["codesign", "--force", "--sign", "-", str(executable)],
            check=True, capture_output=True, text=True)
        with funnel.MacProcessObserver() as observer:
            process = subprocess.Popen([str(executable), "30"])
            def cleanup_process():
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)
            self.addCleanup(cleanup_process)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                starts = [row for row in observer.poll()
                          if row.get("kind") == "start"
                          and row.get("pid") == process.pid]
                if starts:
                    break
                time.sleep(0.05)
            else:
                self.fail("Mac observer did not see spawned sleep")
            authenticated = observer.authenticate(
                process.pid, allowed_roots=[self.root],
                started_after=time.time() - 5)
            self.assertEqual(
                Path(authenticated["executable_path"]).resolve(),
                executable.resolve())
            result = funnel._terminate_mac_pid(
                process.pid, observer=observer, allowed_roots=[self.root])
            process.wait(timeout=5)
            self.assertEqual(result["action"], "terminated")


    def test_codesign_and_ditto_round_trip_tiny_app(self):
        app = self.root / "Tiny.app"
        macos = app / "Contents" / "MacOS"
        macos.mkdir(parents=True)
        shutil.copyfile("/usr/bin/true", macos / "Tiny")
        (macos / "Tiny").chmod(0o755)
        (app / "Contents" / "Info.plist").write_bytes(
            b'<?xml version="1.0" encoding="UTF-8"?>\n'
            b'<plist version="1.0"><dict>'
            b'<key>CFBundleExecutable</key><string>Tiny</string>'
            b'<key>CFBundleIdentifier</key><string>dev.wot.funnel.tiny</string>'
            b'<key>CFBundlePackageType</key><string>APPL</string>'
            b'</dict></plist>\n')
        subprocess.run(
            ["codesign", "--force", "--deep", "--sign", "-", str(app)],
            check=True, capture_output=True, text=True)
        subprocess.run(
            ["codesign", "--verify", "--deep", "--strict", str(app)],
            check=True, capture_output=True, text=True)
        archive = self.root / "Tiny.zip"
        subprocess.run(
            ["ditto", "-c", "-k", "--norsrc", "--noextattr",
             "--keepParent", str(app), str(archive)],
            check=True, capture_output=True, text=True)
        extracted = self.root / "extracted"
        subprocess.run(
            ["ditto", "-x", "-k", str(archive), str(extracted)],
            check=True, capture_output=True, text=True)
        subprocess.run(
            ["codesign", "--verify", "--deep", "--strict",
             str(extracted / "Tiny.app")],
            check=True, capture_output=True, text=True)


class _FakeSocket:
    """Just enough of a socket for TestConn: names, sends, and close."""

    def __init__(self, sockname, peername):
        self.sockname = sockname
        self.peername = peername
        self.closed = False
        self.sent = []

    def getsockname(self):
        return self.sockname

    def getpeername(self):
        return self.peername

    def settimeout(self, value):
        pass

    def sendall(self, data):
        self.sent.append(data)

    def close(self):
        self.closed = True


class TestChannelSelfConnectTests(unittest.TestCase):
    """Reject a net-test socket connected to itself before the game listens."""

    def test_self_connected_socket_is_refused_and_closed(self):
        echo = _FakeSocket(("127.0.0.1", 24680), ("127.0.0.1", 24680))
        with mock.patch.object(funnel.socket, "create_connection", return_value=echo):
            with self.assertRaises(ConnectionRefusedError):
                funnel.TestConn(24680, "token")
        self.assertTrue(echo.closed)
        self.assertEqual(echo.sent, [])

    def test_connect_retries_past_a_self_connection(self):
        echo = _FakeSocket(("127.0.0.1", 24680), ("127.0.0.1", 24680))
        real = _FakeSocket(("127.0.0.1", 51000), ("127.0.0.1", 24680))
        client = funnel.ClientProcess.__new__(funnel.ClientProcess)
        client.test_port = 24680
        client.test_token = "token"
        client.connection = None
        client.poll_failure = lambda: None
        with mock.patch.object(funnel.socket, "create_connection",
                               side_effect=[echo, real]), \
                mock.patch.object(funnel.time, "sleep"):
            connection = client.connect(timeout=5.0)
        self.assertTrue(echo.closed)
        self.assertIs(connection.socket, real)
        self.assertEqual(len(real.sent), 1)

    def test_test_ports_avoid_the_ephemeral_range(self):
        low, high = funnel.TEST_PORT_RANGE
        self.assertLess(high, 32768)
        for _ in range(20):
            port = funnel._available_port()
            self.assertGreaterEqual(port, low)
            self.assertLessEqual(port, high)


@unittest.skipUnless(sys.platform == "win32", "Mark-of-the-Web is an NTFS stream")
class MarkOfTheWebTests(unittest.TestCase):
    def test_every_file_gets_a_zone_identifier_and_keeps_its_bytes(self):
        with tempfile.TemporaryDirectory() as root:
            tree = Path(root) / "WoT-Win64"
            (tree / "WoT" / "Binaries").mkdir(parents=True)
            game = tree / "WoT" / "Binaries" / "game.exe"
            game.write_bytes(b"MZ-game")
            (tree / "readme.txt").write_text("hello", encoding="ascii")
            self.assertEqual(funnel._stamp_mark_of_the_web(tree), 2)
            with open(str(game) + ":Zone.Identifier", encoding="ascii", newline="") as stream:
                self.assertEqual(stream.read(), funnel.MOTW_ZONE_IDENTIFIER)
            self.assertIn("ZoneId=3", funnel.MOTW_ZONE_IDENTIFIER)
            self.assertEqual(game.read_bytes(), b"MZ-game")


class LauncherUpdateHandoffTests(unittest.TestCase):
    def test_handoff_joins_release_identity_and_frontend_on_the_game_pid(self):
        client = mock.Mock(heartbeat_path=Path("isolated-heartbeats"),
                           heartbeat_offset=frozenset())
        identity = {"event": "release-identity", "pid": 22, "detail": "sequence=8 commit=test"}
        handoff = {"event": "frontend-handoff", "pid": 22}
        other = {"event": "frontend-handoff", "pid": 23}

        def wait(_path, _offset, predicate, _timeout, _label):
            self.assertFalse(predicate([identity]))
            self.assertFalse(predicate([handoff]))
            self.assertFalse(predicate([identity, other]))
            self.assertTrue(predicate([identity, other, handoff]))
            return [identity, other, handoff]

        with mock.patch.object(funnel, "_wait_heartbeat", side_effect=wait):
            pid, records = funnel._wait_frontend_launch(client, 8, 5)
        self.assertEqual(pid, 22)
        self.assertEqual(records[-1], handoff)
        client.connect.assert_called_once_with(5)

    def test_missing_game_launch_still_uses_the_stable_launcher_and_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            install = root / "WoT Reborn"
            install.mkdir()
            profile = root / "profile"
            profile.mkdir()
            executable = install / "Game/WoT/Binaries/Win64/WoT-Win64-Test.exe"
            with mock.patch.object(funnel.subprocess, "Popen") as launch:
                client = funnel._launch_reused_profile(executable, profile, "Win64",
                    root / "logs", "B", None, through_launcher=True)
            try:
                self.assertEqual(launch.call_args.args[0][0],
                                 str((install / "WoT Reborn.exe").resolve()))
                self.assertEqual(launch.call_args.kwargs["cwd"],
                                 str(install.resolve()))
                self.assertEqual(client.state_root, (install / "Data").resolve())
            finally:
                client.close_logs()
                funnel._WIN_INSTALL_ROOTS.pop(profile.resolve(), None)

    def test_mac_update_launch_is_direct_because_mac_has_no_launcher(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "profile"
            profile.mkdir()
            executable = root / "WoT.app/Contents/MacOS/WoT-Mac-Test"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"fixture")
            with mock.patch.object(funnel.subprocess, "Popen") as launch:
                client = funnel._launch_reused_profile(
                    executable, profile, "Mac", root / "logs", "B", None,
                    through_launcher=True)
            try:
                self.assertEqual(launch.call_args.args[0][0], str(executable))
                self.assertEqual(launch.call_args.kwargs["cwd"],
                                 str(executable.parent))
            finally:
                client.close_logs()



class ReleaseCookedPathDescriptorTests(unittest.TestCase):
    """The funnel's re-derived census applies the packager's descriptor rule."""

    def _paths(self, *extra, drop_shell=False):
        paths = [path for path in FunnelHarnessTests._cooked_paths()
                 if not (drop_shell and path.endswith("WoTNeutralShells.uplugin"))]
        return paths + list(extra)

    def test_counts_neutral_shell_and_engine_descriptors(self):
        validation = funnel._validate_release_cooked_paths(self._paths())
        self.assertEqual(validation["neutral_shell_plugin_descriptor_count"], 1)
        self.assertEqual(validation["engine_plugin_descriptor_count"],
                         COOKED_ENGINE_DESCRIPTOR_COUNT)

    def test_refuses_a_missing_neutral_shell_descriptor(self):
        with self.assertRaisesRegex(funnel.FunnelFailure, "plugin descriptor"):
            funnel._validate_release_cooked_paths(self._paths(drop_shell=True))

    def test_refuses_an_extra_project_descriptor(self):
        with self.assertRaisesRegex(funnel.FunnelFailure, "plugin descriptor"):
            funnel._validate_release_cooked_paths(
                self._paths("../../../WoT/Plugins/Other/Other.uplugin"))

    ONBOARDING = "../../../WoT/Plugins/WoTOnboarding/WoTOnboarding.uplugin"

    def test_accepts_each_seeded_plugin_descriptor_once(self):
        baseline = funnel._validate_release_cooked_paths(self._paths())
        with mock.patch.object(funnel, "_current_plugin_descriptors",
                               return_value=[self.ONBOARDING]):
            self.assertEqual(funnel._validate_release_cooked_paths(
                self._paths(self.ONBOARDING)), baseline)
            rejected = {
                "duplicated": self._paths(self.ONBOARDING, self.ONBOARDING),
                "another plugin": self._paths(
                    self.ONBOARDING, "../../../WoT/Plugins/Other/Other.uplugin"),
                "case variant": self._paths(self.ONBOARDING.replace(
                    "WoTOnboarding.uplugin", "wotonboarding.uplugin")),
                "neutral shell missing": self._paths(
                    self.ONBOARDING, drop_shell=True),
            }
            for label, paths in rejected.items():
                with self.subTest(label), self.assertRaisesRegex(
                        funnel.FunnelFailure, "plugin descriptor"):
                    funnel._validate_release_cooked_paths(paths)
        with mock.patch.object(funnel, "_current_plugin_descriptors",
                               return_value=[]):
            with self.assertRaisesRegex(funnel.FunnelFailure,
                                        "plugin descriptor"):
                funnel._validate_release_cooked_paths(
                    self._paths(self.ONBOARDING))

    def test_older_census_without_plugin_descriptors_never_reads_the_tree(self):
        with mock.patch.object(funnel, "_current_plugin_descriptors",
                               side_effect=AssertionError("tree read")):
            validation = funnel._validate_release_cooked_paths(self._paths())
        self.assertEqual(validation["neutral_shell_plugin_descriptor_count"], 1)

    def test_plugin_census_and_snapshot_mirror_the_packager(self):
        import build_stripped_pak as packager
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory).resolve()

            def git(*arguments):
                subprocess.run(["git", "-C", str(repo), *arguments],
                               check=True, capture_output=True)

            def write(relative, text="{}\n"):
                path = repo / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")

            git("init", "-q")
            git("config", "user.email", "funnel@example.invalid")
            git("config", "user.name", "Funnel Test")
            write(".gitignore", "Binaries/\nIntermediate/\nContent/\n")
            write("Source/WoT/WoT.cpp", "int wot;\n")
            write("Plugins/WoTOnboarding/WoTOnboarding.uplugin")
            write("Plugins/WoTOnboarding/Source/M.cpp", "int m;\n")
            write("Plugins/Group/Nested/Nested.uplugin")
            git("add", "-A")
            git("commit", "-qm", "fixture")
            write("Plugins/WoTOnboarding/Binaries/Win64/X.uplugin")
            write("Plugins/WoTNeutralShells/WoTNeutralShells.uplugin")
            write("Plugins/Draft/Draft.uplugin")
            write("Plugins/WoTOnboarding/Source/M.cpp", "int m = 2;\n")

            with mock.patch.object(funnel, "REPO", repo):
                descriptors = funnel._current_plugin_descriptors()
                snapshot = funnel._current_source_snapshot()
            self.assertEqual(descriptors, [
                "../../../WoT/Plugins/Draft/Draft.uplugin",
                "../../../WoT/Plugins/Group/Nested/Nested.uplugin",
                "../../../WoT/Plugins/WoTOnboarding/WoTOnboarding.uplugin",
            ])
            self.assertEqual(descriptors, [
                "../../../WoT/" + relative for relative in
                packager._plugin_seed_identity(repo)["descriptors"]])
            self.assertEqual(snapshot, packager._source_snapshot(repo))
            self.assertIn("Plugins", snapshot["paths"])
            self.assertGreater(snapshot["tracked_diff_size"], 0)


if __name__ == "__main__":
    unittest.main()
