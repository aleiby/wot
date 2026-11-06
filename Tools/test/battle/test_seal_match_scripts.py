import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")


@unittest.skipUnless(POWERSHELL, "PowerShell is required for the harness contract")
class SealMatchScriptTests(unittest.TestCase):
    def run_ps(self, script, *arguments):
        return subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
             str(HERE / script), *map(str, arguments)],
            check=False, capture_output=True, text=True, timeout=30)

    def test_plan_is_exactly_two_isolated_instances_and_two_required_maps(self):
        result = self.run_ps("run_seal_match.ps1", "-PlanOnly")
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(result.stdout)
        self.assertEqual(plan["process_count"], 2)
        self.assertEqual(plan["topology"], "listen-host-plus-client")
        self.assertTrue(plan["private_stage_copy"])
        self.assertEqual(plan["private_stage_copies_per_map"], 2)
        self.assertEqual(plan["isolated_profiles"], ["host", "client"])
        self.assertIsNone(plan["launch_url"])
        self.assertEqual(plan["channel_flow"], [
            "wait-front-end-ready", "host-map", "wait-listen-world",
            "join-address", "wait-client-world", "drive-match"])
        self.assertIn("UpdaterState/bootstrap-heartbeat-v1.<pid>.<guid>.jsonl",
                      plan["heartbeat_source"])
        self.assertIn("matching the build descriptor", plan["seed_install_root"])
        self.assertEqual([item["map"] for item in plan["maps"]],
                         ["Battle2_01", "Battle3_01"])
        self.assertEqual(plan["maps"][0]["mode"], "full-seal-objective")
        self.assertEqual(plan["maps"][1]["mode"], "projection-smoke")

    def test_gameplay_scripts_discover_only_the_launched_process_file(self):
        for script in (HERE / "run_seal_match.ps1",
                       HERE.parent / "angreal" / "run_angreal_matrix.ps1",
                       HERE.parent / "sweep" / "run_map_sweep.ps1"):
            with self.subTest(script=script), tempfile.TemporaryDirectory() as scratch:
                source = script.read_text(encoding="utf-8")
                function = re.search(
                    r"(?ms)^function Get-ProcessHeartbeat\b.*?^}", source)
                self.assertIsNotNone(function)
                root = Path(scratch)
                stale = root / "bootstrap-heartbeat-v1.42.old.jsonl"
                fresh = root / "bootstrap-heartbeat-v1.42.new.jsonl"
                foreign = root / "bootstrap-heartbeat-v1.43.foreign.jsonl"
                for path in (stale, fresh, foreign):
                    path.write_text("{}\n", encoding="utf-8")
                quoted = lambda path: str(path).replace("'", "''")
                probe = root / "probe.ps1"
                probe.write_text(function.group() + "\n" +
                    f"Get-ProcessHeartbeat @{{ processId = 42; heartbeatRoot = '{quoted(root)}'; "
                    f"heartbeatBefore = @('{quoted(stale)}') }}\n", encoding="utf-8")
                result = self.run_ps(probe)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(Path(result.stdout.strip()), fresh)

    def test_driver_uses_guarded_front_end_host_then_join_flow(self):
        source = (HERE / "seal_match_driver.py").read_text(encoding="utf-8")
        host_at = source.index('"host", map=self.map')
        listen_at = source.index('host listen world')
        join_at = source.index('"join", address=')
        joined_at = source.index('client joined')
        self.assertLess(host_at, listen_at)
        self.assertLess(listen_at, join_at)
        self.assertLess(join_at, joined_at)

    def test_launcher_seeds_private_install_roots_without_updater_state(self):
        source = (HERE / "run_seal_match.ps1").read_text(encoding="utf-8")
        self.assertIn("[string]$SeedInstallRoot", source)
        self.assertIn("-wottestinstallroot=", source)
        self.assertIn("$parts -contains 'UpdaterState'", source)
        self.assertIn("$_.Name -like '*.lock'", source)
        self.assertIn("$_.Name -match '(?i)lease'", source)
        self.assertIn("AppData/Local/WoT/UpdaterState", source)
        # Only the ContentCache generations the active pointers name are seeded.
        self.assertIn("'^generation=(.+)$'", source)
        self.assertIn("$activeGenerations -notcontains $parts[1]", source)

    def test_launcher_never_assigns_powershell_automatic_variables(self):
        # $host is read-only in PowerShell (the first real launch died on
        # `$host = Start-IsolatedGame ...`); $profile shadows the user's.
        source = (HERE / "run_seal_match.ps1").read_text(encoding="utf-8")
        for name in ("host", "profile", "input", "args", "error"):
            self.assertIsNone(
                re.search(rf"(?im)^\s*\${name}\s*=", source),
                f"run_seal_match.ps1 assigns automatic variable ${name}")

    def test_driver_requires_both_channels_to_see_the_phase_flip_and_match_end(self):
        source = (HERE / "seal_match_driver.py").read_text(encoding="utf-8")
        self.assertIn("client sees EnableTeleporters", source)
        self.assertIn("client match-ended", source)
        self.assertIn("seal budget not spent on ready-up", source)

    def test_verifier_accepts_a_complete_fixture_and_rejects_missing_frame(self):
        fixtures = HERE / "fixtures"
        accepted = self.run_ps("verify_seal_match.ps1", "-EvidenceRoot",
                               fixtures / "complete")
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        rejected = self.run_ps("verify_seal_match.ps1", "-EvidenceRoot",
                               fixtures / "missing-frame")
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("missing client-frame.png", rejected.stderr)

    def test_verifier_requires_projection_and_physics_completion_records(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch) / "run"
            shutil.copytree(HERE / "fixtures" / "complete", root)
            beat = root / "Battle3_01" / "client-heartbeat.jsonl"
            kept = [line for line in beat.read_text(encoding="utf-8").splitlines()
                    if "world-physics-completed" not in line]
            beat.write_text("\n".join(kept) + "\n", encoding="utf-8")
            rejected = self.run_ps("verify_seal_match.ps1", "-EvidenceRoot", root)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("no world-physics-completed record", rejected.stderr)
            summary = root / "Battle2_01" / "summary.json"
            data = json.loads(summary.read_text(encoding="utf-8"))
            data["events"] = [e for e in data["events"] if e["event"] != "seal-banked"]
            summary.write_text(json.dumps(data), encoding="utf-8")
            shutil.copy(HERE / "fixtures" / "complete" / "Battle3_01" / "client-heartbeat.jsonl", beat)
            rejected = self.run_ps("verify_seal_match.ps1", "-EvidenceRoot", root)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("lacks the seal-banked phase", rejected.stderr)


if __name__ == "__main__":
    unittest.main()
