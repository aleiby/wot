#!/usr/bin/env python3
"""Stdlib tests for the guarded Shipping smoke's evidence checks."""
from __future__ import annotations

import contextlib
import copy
import io
import json
import os
import plistlib
import sys
import tempfile
import subprocess
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_test_fixtures as fixtures  # noqa: E402
import run_first_run_funnel as funnel  # noqa: E402
import smoke_shipping_release as smoke  # noqa: E402

COMMIT = "abc123456"
SNAPSHOT_SHA = "4" * 64
PID = 4242
LAUNCHER_PID = 4141


def shipping_manifest(platform: str = "Win64") -> dict:
    twin = fixtures.synthetic_test_twin(platform, COMMIT, SNAPSHOT_SHA)
    return {
        "format": "wot-guarded-release-v1",
        "platform": platform,
        "configuration": "Shipping",
        "git_commit": COMMIT,
        "release_sequence": 0,
        "asset": f"WoT-{platform}-guarded-shipping-{COMMIT}.zip",
        "source_snapshot": {"sha256": SNAPSHOT_SHA},
        "test_twin": twin,
        "zen_lifetime": {"receipt_sha256": twin["zen_lifetime_receipt_sha256"]},
        "target_receipts": {
            "WoT": {"compiled_receipt": {
                "configuration": "Shipping",
                "definitions": {
                    "WOT_GUARDED_BOOTSTRAP": "1",
                    "WOT_BOOTSTRAP_COMPILED_COMPONENT_IDENTITY":
                        '"wot-game-production-v1"',
                    "WOT_BOOTSTRAP_LIFECYCLE_ENDPOINT": '"production"',
                    "WOT_REQUIRED_UAT_ARGUMENT_NO_BOOTSTRAP_EXE": "1",
                    "NOINITCRASHREPORTER": "1",
                    "CRASH_REPORTER_WITH_ANALYTICS": "0",
                    "USE_CRASH_REPORTER_MONITOR": "0",
                },
                "checks": ["test-only-definitions-absent"]}},
            "WoTUpdater": {"compiled_receipt": {
                "configuration": "Shipping",
                "definitions": {"WOT_GUARDED_BOOTSTRAP": "0"},
                "checks": ["unguarded-shipping-program-target"]}},
        },
        "shipping_stage": {"receipt": {
            "configuration": "Shipping",
            "exit_code": 0,
            "command": ["RunUAT.bat", "BuildCookRun", "-project=x/WoT.uproject",
                        f"-platform={platform}", "-clientconfig=Shipping",
                        "-skipbuild", "-skipcook", "-stage", "-pak",
                        "-NoDebugInfo", "-stagingdirectory=x/staged-shipping"],
            "cooked_output": {"before": {"metadata": {"a": 1}},
                              "after": {"metadata": {"a": 1}}},
        }},
    }


def record(event: str, *, phase: str = "Ready", epoch: int = 0,
           detail: str = "", pid: int = PID) -> dict:
    value = {"event": event, "phase": phase, "epoch": epoch, "pid": pid,
             "ts_utc": "2026-09-29T00:00:00Z"}
    if detail:
        value["detail"] = detail
    return value


def boot_records() -> list:
    return [
        record("phase-begin", phase="CheckingOriginalGameData", epoch=3),
        record("original-game-detected", phase="CheckingOriginalGameData",
               epoch=3, detail="source=adjacent"),
        record("phase-ready", epoch=7),
        record("span-end", epoch=7,
               detail="name=scene-preparation-handoff result=success"),
        record("frontend-handoff", epoch=7, detail="widget=1"),
    ]


def host_records(shell: str = "WoTMapShell03", net: int = 2) -> list:
    return [
        record("console-verb", detail="WoTHost frontend lan=0 subsystem=1"),
        record("host-travel",
               detail=f"url=/WoTNeutralShells/{shell}?listen route="
                      f"/WoTNeutralShells/{shell}"),
        record("host-travel-outcome",
               detail="started=1 pending=1 can_use=1 handled=0 destroy=0 "
                      "net=0 gamemode=1"),
        record("world-materialized", detail=f"map=Arena_03 slot=3"),
        # As every real guarded host logs it: exposed before the listen
        # net mode is set (Mac Shipping + Test heartbeats, 2026-10-05).
        record("world-exposed",
               detail=f"package=/WoTNeutralShells/{shell} actors=0 "
                      f"player_starts=0 lit_actors=0 game_mode=none net=0"),
        record("player-possess",
               detail=f"pawn=WoTPawn at=(0,0,0) world=/WoTNeutralShells/{shell} "
                      f"map=Arena_03 net={net}"),
    ]


class ReleaseChainTests(unittest.TestCase):
    def test_accepts_the_production_shipping_chain(self):
        result = smoke.validate_shipping_manifest(shipping_manifest())
        self.assertEqual(result["asset"], f"WoT-Win64-guarded-shipping-{COMMIT}.zip")
        self.assertEqual(result["test_twin_asset"], f"WoT-Win64-guarded-{COMMIT}.zip")
        self.assertIn("-skipcook", result["stage_command_proof"])

    def test_refuses_every_test_or_unproven_shape(self):
        def game(manifest):
            return manifest["target_receipts"]["WoT"]["compiled_receipt"]

        cases = {
            "not a guarded Shipping release":
                lambda m: m.update(configuration="Test"),
            "Shipping asset must be":
                lambda m: m.update(asset=f"WoT-Win64-guarded-{COMMIT}.zip"),
            "production Shipping game": lambda m: game(m)["definitions"].update(
                WOT_BOOTSTRAP_TEST_ENDPOINTS="1"),
            "production Shipping game ": lambda m: game(m)["definitions"].update(
                WOT_BOOTSTRAP_COMPILED_COMPONENT_IDENTITY='"wot-game-test-v1"'),
            "-skipbuild -skipcook": lambda m: m["shipping_stage"]["receipt"]
                ["command"].append("-cook"),
            "unchanged Test cook": lambda m: m["shipping_stage"]["receipt"]
                ["cooked_output"].update(after={"metadata": {"a": 2}}),
            "different import+cook":
                lambda m: m["zen_lifetime"].update(receipt_sha256="0" * 64),
            "no exact test_twin": lambda m: m.pop("test_twin"),
        }
        for fragment, mutate in cases.items():
            with self.subTest(fragment=fragment):
                manifest = shipping_manifest()
                mutate(manifest)
                with self.assertRaises(smoke.SmokeFailure) as caught:
                    smoke.validate_shipping_manifest(manifest)
                self.assertIn(fragment.strip(), str(caught.exception))

    def test_funnel_still_refuses_a_shipping_release(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "release.json"
            manifest = shipping_manifest()
            manifest.update({
                "size": 1, "sha256": "1" * 64, "products": {}, "audit": {},
                "engine_identity": {}, "generated_utc": "x",
                "cooked_container_inventory": {},
                "neutral_support_authority": {}})
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(funnel.FunnelFailure, "Test release manifest"):
                funnel._resolve_release(path, require_current_source=False)


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def win64_tree(self) -> Path:
        tree = self.root / "WoT Reborn" / "Game"
        for relative, (data, _mode) in fixtures.platform_files("Win64").items():
            path = tree / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return tree

    def test_resolves_the_shipping_image_and_refuses_test_or_missing_parts(self):
        tree = self.win64_tree()
        game = smoke.resolve_shipping_image(tree, "Win64")
        self.assertEqual(game.name, "WoT-Win64-Shipping.exe")
        test_image = tree / "WoT" / "Binaries" / "Win64" / "WoT-Win64-Test.exe"
        test_image.write_bytes(b"MZ test")
        with self.assertRaisesRegex(smoke.SmokeFailure, "carries a Test image"):
            smoke.resolve_shipping_image(tree, "Win64")
        test_image.unlink()
        game.unlink()
        with self.assertRaisesRegex(smoke.SmokeFailure, "cannot identify one packaged"):
            smoke.resolve_shipping_image(tree, "Win64")

    def test_mac_image_is_the_bundle_executable(self):
        app = self.root / "WoT.app"
        (app / "Contents" / "MacOS").mkdir(parents=True)
        (app / "Contents" / "Helpers").mkdir(parents=True)
        (app / "Contents" / "MacOS" / "WoT-Mac-Shipping").write_bytes(b"game")
        (app / "Contents" / "Helpers" / "WoTUpdater").write_bytes(b"updater")
        with (app / "Contents" / "Info.plist").open("wb") as handle:
            plistlib.dump({"CFBundleExecutable": "WoT-Mac-Shipping"}, handle)
        game = smoke.resolve_shipping_image(app, "Mac")
        self.assertEqual(game.name, "WoT-Mac-Shipping")

    def test_seam_report_and_debug_files(self):
        tree = self.win64_tree()
        game = smoke.resolve_shipping_image(tree, "Win64")
        report = smoke.image_seam_report((game,))
        self.assertEqual(
            {row["test_seam_markers"] == [] for row in report.values()}, {True})
        game.write_bytes(game.read_bytes() + "wotnettestport=".encode("utf-16-le"))
        self.assertEqual(
            smoke.image_seam_report((game,))[game.name]["test_seam_markers"],
            ["wotnettestport="])
        self.assertEqual(smoke.debug_files(tree), [])
        (game.parent / "WoT-Win64-Shipping.pdb").write_bytes(b"pdb")
        self.assertEqual(smoke.debug_files(tree),
                         ["WoT/Binaries/Win64/WoT-Win64-Shipping.pdb"])


class WindowsChildDiscoveryTests(unittest.TestCase):
    class FakeLauncher:
        pid = LAUNCHER_PID
        returncode = 0

        @staticmethod
        def poll():
            return 0

    class FakeCandidate:
        def __init__(self, pid, creation, image):
            self.pid = pid
            self._creation = creation
            self._image = image
            self.closed = False

        def creation_time(self):
            return self._creation

        def image_path(self):
            return self._image

        def close(self):
            self.closed = True

    def discover_rejected(self, candidate, snapshots, expected):
        with mock.patch.object(smoke.os, "name", "nt"), \
                mock.patch.object(smoke, "_windows_launcher_creation_time",
                                  return_value=100), \
                mock.patch.object(smoke, "_toolhelp_process_entries",
                                  side_effect=snapshots), \
                mock.patch.object(smoke, "_WindowsChildProcess",
                                  return_value=candidate):
            with self.assertRaisesRegex(smoke.SmokeFailure, "exited"):
                smoke._discover_windows_game_child(
                    self.FakeLauncher(), expected, 1.0)
        self.assertTrue(candidate.closed)

    def test_rejects_older_same_name_process_with_launcher_parent(self):
        expected = Path("C:/stage/Game/WoT/Binaries/Win64/") / \
            "WoT-Win64-Shipping.exe"
        candidate = self.FakeCandidate(PID, 99, str(expected))
        self.discover_rejected(candidate, [[
            (PID, LAUNCHER_PID, expected.name)]], expected)

    def test_rejects_same_name_child_from_wrong_full_path(self):
        expected = Path("C:/stage/Game/WoT/Binaries/Win64/") / \
            "WoT-Win64-Shipping.exe"
        wrong = Path("C:/other") / expected.name
        candidate = self.FakeCandidate(PID, 101, str(wrong))
        self.discover_rejected(candidate, [[
            (PID, LAUNCHER_PID, expected.name)]], expected)

    def test_rejects_snapshot_to_open_pid_reuse(self):
        expected = Path("C:/stage/Game/WoT/Binaries/Win64/") / \
            "WoT-Win64-Shipping.exe"
        cases = (
            ("opened process predates launcher", 99,
             [[(PID, LAUNCHER_PID, expected.name)]]),
            ("fresh snapshot has a different parent", 101,
             [[(PID, LAUNCHER_PID, expected.name)],
              [(PID, LAUNCHER_PID + 1, expected.name)]]),
        )
        for label, creation, snapshots in cases:
            with self.subTest(label=label):
                candidate = self.FakeCandidate(PID, creation, str(expected))
                self.discover_rejected(candidate, snapshots, expected)


class RuntimeEvidenceTests(unittest.TestCase):
    def test_front_end_needs_ready_then_handoff_in_one_epoch(self):
        evidence = smoke.front_end_evidence(boot_records(), PID)
        self.assertEqual(evidence["epoch"], 7)
        self.assertIsNone(smoke.front_end_evidence(boot_records(), PID + 1))
        wrong_epoch = boot_records()
        wrong_epoch[-1]["epoch"] = 8
        self.assertIsNone(smoke.front_end_evidence(wrong_epoch, PID))
        reordered = boot_records()
        reordered[2], reordered[4] = reordered[4], reordered[2]
        self.assertIsNone(smoke.front_end_evidence(reordered, PID))

    def test_host_needs_a_listen_shell_exposed_and_possessed(self):
        evidence = smoke.host_evidence(boot_records() + host_records(), PID)
        self.assertEqual(evidence["shell"], "WoTMapShell03")
        self.assertEqual(evidence["player_possess"]["event"], "player-possess")
        self.assertIsNone(smoke.host_evidence(host_records(net=0), PID))
        mismatched = host_records()
        mismatched[4]["detail"] = mismatched[4]["detail"].replace(
            "WoTMapShell03", "WoTMapShell04")
        self.assertIsNone(smoke.host_evidence(mismatched, PID))
        no_route = host_records()
        no_route[1]["detail"] = "url=/Game/WoT/Maps/Arena_03?listen route=none"
        self.assertIsNone(smoke.host_evidence(no_route, PID))
        not_started = host_records()
        not_started[2]["detail"] = not_started[2]["detail"].replace(
            "started=1", "started=0")
        self.assertIsNone(smoke.host_evidence(not_started, PID))
        self.assertIsNone(smoke.host_evidence(host_records()[:-1], PID))
        self.assertIsNone(smoke.host_evidence(host_records(shell="WoTMapShell20"), PID))

    def test_failure_events_end_the_smoke(self):
        self.assertIsNone(smoke.failure_event(boot_records() + host_records(), PID))
        refused = record("host-refused", detail="menu-can-use-refused")
        self.assertEqual(smoke.failure_event([refused], PID), refused)
        self.assertIsNone(smoke.failure_event([refused], PID + 1))

    def test_cache_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            state = Path(temporary)
            self.assertIsNone(smoke.cache_evidence(state))
            current = state / "ContentCache" / "current"
            current.mkdir(parents=True)
            (current / "manifest.json").write_text("{}")
            evidence = smoke.cache_evidence(state)
            self.assertEqual(len(evidence["manifests"]), 1)
            building = state / "ContentCache" / "building"
            building.mkdir()
            (building / "manifest.json").write_text("{}")
            self.assertEqual(smoke.cache_evidence(state), evidence)

    def test_launch_is_unattended_and_carries_no_test_switch(self):
        for trigger in ("manual", "none"):
            command = smoke.launch_command(Path("WoT-Win64-Shipping.exe"), trigger)
            with self.subTest(trigger=trigger):
                self.assertIn("-unattended", command)
                self.assertEqual("-RenderOffscreen" in command, trigger == "none")
                self.assertFalse(any("wotnettest" in token.casefold()
                                     or "wottestinstallroot" in token.casefold()
                                     for token in command))

    def test_installer_is_required(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                smoke.main(["--release", "release.json"])


class FakeClient:
    """A Shipping client that publishes the heartbeat and cache a real boot does."""

    def __init__(self, records, command, cwd=None, env=None, stdout=None,
                 stderr=None, start_new_session=False):
        self.pid = LAUNCHER_PID
        self.returncode = None
        self.command = command
        self.game_process = SimpleNamespace(
            pid=PID, returncode=None, poll=lambda: None,
            wait=lambda timeout=None: 0, close=lambda: None)
        # The state root the real client uses under the profile environment
        # run_first_run_funnel._profile_environment builds for each host.
        if "LOCALAPPDATA" in env:
            state = Path(command[0]).parent / "Data"
        else:
            state = (Path(env["HOME"]) / "Library" / "Application Support"
                     / "WoT Reborn")
        heartbeat = state / "UpdaterState" / f"bootstrap-heartbeat-v1.{PID}.fake.jsonl"
        heartbeat.parent.mkdir(parents=True, exist_ok=True)
        data = "".join(json.dumps(value) + "\n" for value in records).encode()
        heartbeat.write_bytes(data)
        current = state / "ContentCache" / "current"
        current.mkdir(parents=True)
        (current / "manifest.json").write_text("{}")
        (state / "Saved" / "Logs").mkdir(parents=True)

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        return 0


class MacRealProfileGuardTests(unittest.TestCase):
    def test_records_matching_post_run_inventory(self):
        before = {"new": {"exists": False}}
        checks = {}
        with mock.patch.object(
                funnel, "_mac_real_support_inventory", return_value=before):
            smoke._verify_mac_real_profile_unchanged(before, checks)
        self.assertEqual(checks["real_profile_after"], before)
        self.assertTrue(checks["real_profile_unchanged"])

    def test_any_post_run_change_is_a_hard_failure(self):
        before = {"new": {"exists": False}}
        after = {"new": {"exists": True}}
        checks = {}
        with mock.patch.object(
                funnel, "_mac_real_support_inventory", return_value=after):
            with self.assertRaisesRegex(
                    smoke.SmokeFailure, "changed the developer's real"):
                smoke._verify_mac_real_profile_unchanged(before, checks)
        self.assertEqual(checks["real_profile_after"], after)
        self.assertFalse(checks["real_profile_unchanged"])


class RunSmokeWiringTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.release_json = fixtures.write_guarded_release(
            self.root / "release-shipping", "Win64", commit=COMMIT)
        self.asset = self.release_json.parent / f"WoT-Win64-guarded-shipping-{COMMIT}.zip"
        self.installer = self.root / "installer.exe"
        self.installer.write_bytes(b"pinned installer")

    def run_smoke(self, records, trigger="manual"):
        # Only the game launch (its own session) is faked: on a Mac host the
        # funnel helpers also shell out (ditto, cp -c) through subprocess.
        real_popen = subprocess.Popen
        manifest = shipping_manifest()
        manifest.update(sha256=funnel._sha256_file(self.asset),
                        size=self.asset.stat().st_size)
        args = SimpleNamespace(
            release=self.release_json, installer=self.installer,
            funnel_receipt=None, evidence=self.root / "evidence",
            work_root=self.root / "work", host_trigger=trigger,
            boot_timeout=5.0, host_timeout=5.0, allow_source_drift=False)
        with mock.patch.object(funnel, "_resolve_release",
                               return_value=(manifest, self.asset)), \
                mock.patch.object(smoke, "_host_platform", return_value="Win64"), \
                mock.patch.object(funnel, "_verify_installer", return_value={
                    "path": str(self.installer), "size": 16, "sha256": "0" * 64}), \
                mock.patch.object(smoke.subprocess, "Popen",
                                  side_effect=lambda *a, **k: (
                                      FakeClient(records, *a, **k)
                                      if k.get("start_new_session")
                                      else real_popen(*a, **k))), \
                mock.patch.object(smoke, "_discover_windows_game_child",
                                  side_effect=lambda launcher, *_: (
                                      launcher.game_process)), \
                mock.patch.object(smoke, "_terminate",
                                  return_value={"signals": [], "returncode": 0}), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            try:
                return smoke.run_smoke(args), output.getvalue()
            finally:
                self.receipt = json.loads(
                    (self.root / "evidence" / "shipping-smoke-receipt.json")
                    .read_text(encoding="utf-8"))

    def test_boot_and_manual_host_evidence_pass(self):
        receipt, output = self.run_smoke(
            boot_records() + host_records()
            + [record("host-refused", detail="launcher-only",
                      pid=LAUNCHER_PID)])
        self.assertEqual(receipt["verdict"], "pass")
        self.assertIn("Multiplayer, pick any map, then click Host Match", output)
        self.assertNotIn("click Play", output)
        self.assertIn("closes the game itself", output)
        checks = receipt["checks"]
        self.assertEqual(checks["image"]["game"].replace("\\", "/"),
                         "WoT/Binaries/Win64/WoT-Win64-Shipping.exe")
        self.assertEqual(checks["boot"]["front_end"]["epoch"], 7)
        self.assertEqual(checks["host"]["shell"], "WoTMapShell03")
        self.assertEqual(checks["boot"]["original_game"]["event"],
                         "original-game-detected")
        self.assertTrue(checks["ue_saved_under_state_root"])
        self.assertTrue(Path(checks["adjacent_installer"]).name.startswith("setup_"))
        self.assertNotIn("-RenderOffscreen", receipt["process"]["command"])
        self.assertEqual(Path(receipt["process"]["command"][0]).name,
                         "WoT Reborn.exe")
        self.assertEqual(receipt["process"]["launcher_pid"], LAUNCHER_PID)
        self.assertEqual(receipt["process"]["game_pid"], PID)
        self.assertNotEqual(receipt["process"]["launcher_pid"],
                            receipt["process"]["game_pid"])
        self.assertTrue(all(row["pid"] == PID
                            for row in receipt["heartbeat"]["records"]))
        self.assertTrue(receipt["checks"]["launcher_matches_game_exit"])

    def test_front_end_only_mode_needs_no_host(self):
        receipt, output = self.run_smoke(boot_records(), trigger="none")
        self.assertEqual(receipt["verdict"], "pass")
        self.assertNotIn("host", receipt["checks"])
        self.assertIn("-RenderOffscreen", receipt["process"]["command"])

    def test_a_durable_refusal_fails_and_is_receipted(self):
        with self.assertRaisesRegex(smoke.SmokeFailure, "host-refused"):
            self.run_smoke(boot_records() + [record("host-refused", detail="x")])
        self.assertEqual(self.receipt["verdict"], "fail")
        self.assertIn("host-refused", self.receipt["error"])


if __name__ == "__main__":
    unittest.main()
