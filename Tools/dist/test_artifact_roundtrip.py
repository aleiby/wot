#!/usr/bin/env python3
"""Stdlib specs for the canonical-JSON round-trip driver's own mechanics.

The native runner is not built here.  Each case below is a scripted fake runner
that speaks the wire protocol and then misbehaves in one specific way, proving
the driver reports the disagreement instead of passing silently or hanging.  A
driver that cannot fail is not evidence.
"""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import artifact_roundtrip as driver

DIST = str(Path(__file__).resolve().parent)

FAKE_PREAMBLE = f'''
import struct, sys
sys.path.insert(0, {DIST!r})
import artifact_roundtrip as driver

ACCEPTED = dict(driver.ACCEPTED)
REJECTED = {{candidate: code for _name, candidate, code in driver.REJECTED}}

def read_exactly(count):
    data = sys.stdin.buffer.read(count)
    if data is None or len(data) != count:
        raise SystemExit(0)
    return data

def respond(status, payload):
    sys.stdout.buffer.write(struct.pack("<BI", status, len(payload)) + payload)
    sys.stdout.buffer.flush()

def serve(handle):
    while True:
        opcode, length = struct.unpack("<BI", read_exactly(5))
        if opcode == driver.OPCODE_SHUTDOWN:
            return
        handle(opcode, read_exactly(length))
'''

# A faithful runner: echo accepted fixtures verbatim, reject the rest by code.
FAKE_CONFORMING = FAKE_PREAMBLE + '''
def handle(opcode, candidate):
    if candidate in REJECTED:
        respond(driver.STATUS_REJECTED,
                (REJECTED[candidate] + ": rejected").encode("utf-8"))
    else:
        respond(driver.STATUS_ACCEPTED, candidate)
serve(handle)
'''

# Re-emits one accepted fixture with a flipped byte.
FAKE_CORRUPTING = FAKE_PREAMBLE + '''
TARGET = ACCEPTED["sorted-keys"]
def handle(opcode, candidate):
    if candidate in REJECTED:
        respond(driver.STATUS_REJECTED,
                (REJECTED[candidate] + ": rejected").encode("utf-8"))
    elif candidate == TARGET:
        respond(driver.STATUS_ACCEPTED, candidate[:-1] + b" ")
    else:
        respond(driver.STATUS_ACCEPTED, candidate)
serve(handle)
'''

# Accepts everything, including bytes that must be rejected.
FAKE_PERMISSIVE = FAKE_PREAMBLE + '''
def handle(opcode, candidate):
    respond(driver.STATUS_ACCEPTED, candidate)
serve(handle)
'''

# Rejects with the wrong error code.
FAKE_WRONG_CODE = FAKE_PREAMBLE + '''
def handle(opcode, candidate):
    if candidate in REJECTED:
        respond(driver.STATUS_REJECTED, b"Io: wrong code")
    else:
        respond(driver.STATUS_ACCEPTED, candidate)
serve(handle)
'''

# Announces a payload longer than it writes, then exits.
FAKE_TRUNCATING = FAKE_PREAMBLE + '''
def handle(opcode, candidate):
    sys.stdout.buffer.write(struct.pack("<BI", driver.STATUS_ACCEPTED, 4096))
    sys.stdout.buffer.write(b"short")
    sys.stdout.buffer.flush()
    raise SystemExit(0)
serve(handle)
'''

# Reads requests and never answers.
FAKE_SILENT = FAKE_PREAMBLE + '''
def handle(opcode, candidate):
    pass
serve(handle)
'''

# Faithful answers, then a non-zero exit status.
FAKE_BAD_EXIT = FAKE_PREAMBLE + '''
def handle(opcode, candidate):
    if candidate in REJECTED:
        respond(driver.STATUS_REJECTED,
                (REJECTED[candidate] + ": rejected").encode("utf-8"))
    else:
        respond(driver.STATUS_ACCEPTED, candidate)
serve(handle)
raise SystemExit(3)
'''

# Answers with a status byte outside the protocol.
FAKE_BAD_STATUS = FAKE_PREAMBLE + '''
def handle(opcode, candidate):
    respond(9, b"nonsense")
serve(handle)
'''

FAKE_CONTRACT_CONFORMING = FAKE_PREAMBLE + f'''
import json
from cache_contract_v3 import ContractError, canonical_bytes, validate_payload
with open({str(Path(DIST) / "fixtures" / "cache-contract-v3-synthetic.json")!r},
          "rb") as stream:
    SIGNED = stream.read()
def handle(opcode, candidate):
    if opcode == driver.OPCODE_EVALUATE_SIGNED_CONTRACT:
        if candidate == SIGNED:
            respond(driver.STATUS_ACCEPTED, candidate)
        else:
            respond(driver.STATUS_REJECTED, b"Crypto: contract.signature")
        return
    if opcode == driver.OPCODE_EVALUATE_CONTRACT_PAYLOAD:
        try:
            value = json.loads(candidate)
            if canonical_bytes(value) != candidate:
                raise ContractError("payload", "not canonical")
            validate_payload(value)
        except ContractError as exc:
            respond(driver.STATUS_REJECTED,
                    ("UnknownField: " + exc.path).encode("utf-8"))
        else:
            respond(driver.STATUS_ACCEPTED, candidate)
        return
    respond(driver.STATUS_REJECTED, b"UnsupportedProtocol: opcode")
serve(handle)
'''


class FixtureSetTests(unittest.TestCase):
    def test_canonical_matches_the_frozen_release_oracle_rules(self):
        self.assertEqual(driver.canonical({"b": 1, "a": [1, 2]}),
                         b'{"a":[1,2],"b":1}')
        self.assertEqual(driver.canonical("café"), b'"caf\\u00e9"')
        self.assertEqual(driver.canonical("\U0001f600"), b'"\\ud83d\\ude00"')

    def test_every_fixture_name_is_unique_and_non_empty(self):
        names = [name for name, _ in driver.ACCEPTED]
        names += [name for name, _, _ in driver.REJECTED]
        self.assertEqual(len(names), len(set(names)))
        self.assertTrue(all(names))

    def test_accepted_and_rejected_fixture_bytes_are_disjoint(self):
        accepted = {payload for _name, payload in driver.ACCEPTED}
        rejected = {payload for _name, payload, _code in driver.REJECTED}
        self.assertEqual(accepted & rejected, set())

    def test_depth_fixtures_straddle_the_documented_limit(self):
        self.assertEqual(dict(driver.ACCEPTED)["max-depth"].count(b"{"), 32)
        over = dict((name, payload)
                    for name, payload, _ in driver.REJECTED)["depth-over-limit"]
        self.assertEqual(over.count(b"{"), 33)

    def test_size_fixtures_straddle_every_native_limit(self):
        accepted = dict(driver.ACCEPTED)
        rejected = {name: payload for name, payload, _code in driver.REJECTED}
        self.assertEqual(len(driver.json.loads(accepted["max-container-members"])),
                         65536)
        self.assertEqual(len(driver.json.loads(
            rejected["container-members-over-limit"])), 65537)
        self.assertEqual(len(driver.json.loads(accepted["max-string-bytes"])),
                         1024 * 1024)
        self.assertEqual(len(driver.json.loads(
            rejected["string-bytes-over-limit"])), 1024 * 1024 + 1)
        self.assertEqual(len(accepted["max-document-bytes"]), 4 * 1024 * 1024)
        self.assertEqual(len(rejected["document-bytes-over-limit"]),
                         4 * 1024 * 1024 + 1)


class DriverBehaviorTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def _run(self, source, *, timeout=20.0):
        script = self.root / "fake_runner.py"
        script.write_text(source, encoding="utf-8")
        return driver.run_fixtures([sys.executable, str(script)],
                                   timeout=timeout)

    def test_a_conforming_runner_produces_no_problems(self):
        self.assertEqual(self._run(FAKE_CONFORMING), [])

    def test_a_single_flipped_byte_is_reported(self):
        problems = self._run(FAKE_CORRUPTING)
        self.assertEqual(len(problems), 1)
        self.assertIn("accept/sorted-keys", problems[0])
        self.assertIn("re-emitted", problems[0])

    def test_accepting_a_rejected_fixture_is_reported(self):
        problems = self._run(FAKE_PERMISSIVE)
        self.assertEqual(len(problems), len(driver.REJECTED))
        self.assertTrue(all("runner accepted" in problem
                            for problem in problems))

    def test_the_wrong_error_code_is_reported(self):
        problems = self._run(FAKE_WRONG_CODE)
        self.assertEqual(len(problems), len(driver.REJECTED))
        self.assertIn("expected NonCanonicalJson", problems[0])

    def test_a_truncated_frame_is_reported_without_hanging(self):
        problems = self._run(FAKE_TRUNCATING, timeout=10.0)
        self.assertEqual(len(problems), 1)
        self.assertIn("before closing stdout", problems[0])

    def test_a_silent_runner_fails_on_the_deadline(self):
        problems = self._run(FAKE_SILENT, timeout=1.0)
        self.assertEqual(len(problems), 1)
        self.assertIn("within 1s", problems[0])

    def test_a_non_zero_runner_exit_is_reported(self):
        problems = self._run(FAKE_BAD_EXIT)
        self.assertEqual(problems, ["runner exited 3"])

    def test_an_out_of_protocol_status_byte_is_reported(self):
        problems = self._run(FAKE_BAD_STATUS)
        self.assertEqual(len(problems), 1)
        self.assertIn("returned status 9", problems[0])

    def test_a_missing_runner_executable_is_reported(self):
        with self.assertRaises(OSError):
            driver.run_fixtures([str(self.root / "absent-runner")], timeout=5.0)

    def test_main_requires_a_runner_unless_listing(self):
        self.assertEqual(driver.main(["--list"]), 0)
        with self.assertRaises(SystemExit):
            driver.main([])

    def test_main_reports_the_exit_code_of_a_conforming_runner(self):
        script = self.root / "fake_runner.py"
        script.write_text(FAKE_CONFORMING, encoding="utf-8")
        self.assertEqual(driver.main(
            ["--runner", sys.executable, "--runner-arg", str(script),
             "--timeout", "20"]), 0)
        script.write_text(FAKE_PERMISSIVE, encoding="utf-8")
        self.assertEqual(driver.main(
            ["--runner", sys.executable, "--runner-arg", str(script),
             "--timeout", "20"]), 1)


class ContractFixtureTests(unittest.TestCase):
    """The --contract-fixture mode derives its accepted set from a signed
    cache-contract-v3 document instead of the built-in fixtures."""

    CHECKED_IN = Path(DIST) / "fixtures" / "cache-contract-v3-synthetic.json"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_the_checked_in_fixture_is_canonical_and_covers_every_fact(self):
        fixtures = driver.contract_fixture_set(self.CHECKED_IN)
        document = driver.json.loads(self.CHECKED_IN.read_bytes())
        names = [name for name, _opcode, _payload in fixtures]
        self.assertEqual(names[0], "document")
        self.assertEqual(names[1], "payload")
        # one fact per EWoTFactDomain value, and every fixture is canonical
        self.assertEqual(len(names), 2 + len(document["payload"]["facts"]))
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(fixtures[0][1], driver.OPCODE_EVALUATE_SIGNED_CONTRACT)
        self.assertTrue(all(opcode == driver.OPCODE_EVALUATE_CONTRACT_PAYLOAD
                            for _name, opcode, _payload in fixtures[1:]))
        for _name, _opcode, payload in fixtures:
            self.assertEqual(driver.canonical(driver.json.loads(payload)),
                             payload)
        for _name, _opcode, payload in fixtures[2:]:
            projected = driver.json.loads(payload)
            fact = projected["facts"][0]
            self.assertEqual(projected["startup_critical_logical_ids"],
                             [fact["outputs"][0]["logical_id"]])

    def test_contract_mode_uses_native_structure_and_python_paths(self):
        script = self.root / "fake_contract_runner.py"
        script.write_text("#!/usr/bin/env python3\n" + FAKE_CONTRACT_CONFORMING,
                          encoding="utf-8")
        script.chmod(0o755)
        self.assertEqual(driver.run_contract_fixture(
            self.CHECKED_IN, script, timeout=20.0), [])
        document = driver.json.loads(self.CHECKED_IN.read_bytes())
        paths = [path for _name, _candidate, path
                 in driver.contract_mutation_set(document)]
        self.assertEqual(paths, [
            "payload.facts[0].importer",
            "payload.facts[0].runtime_consumer",
            "payload.facts[0].audit_rule",
            "payload.facts[0].source.package",
            "payload.facts[0].outputs[0].schema",
            "payload.facts[0].outputs[0].max_decoded_size",
        ])

    def test_a_non_canonical_fixture_file_is_refused(self):
        crooked = self.root / "crooked.json"
        crooked.write_bytes(b'{ "payload": {"facts": []}, "signature": "00" }')
        with self.assertRaisesRegex(driver.RoundTripFailure, "not canonical"):
            driver.contract_fixture_set(crooked)

    def test_an_unusable_fixture_is_one_problem_not_a_crash(self):
        missing_facts = self.root / "nofacts.json"
        missing_facts.write_bytes(driver.canonical({"payload": {}, "signature": "00"}))
        problems = driver.run_contract_fixture(
            missing_facts, Path("unused-runner"), timeout=5.0)
        self.assertEqual(len(problems), 1)
        self.assertIn("contract fixture unusable", problems[0])

    def test_main_wires_contract_mode_and_rejects_mixed_modes(self):
        with self.assertRaises(SystemExit):
            driver.main(["--contract-fixture", str(self.CHECKED_IN)])
        with self.assertRaises(SystemExit):
            driver.main(["--contract-fixture", str(self.CHECKED_IN),
                         "--cpp-runner", "runner",
                         "--input", str(self.root)])
        with self.assertRaises(SystemExit):
            driver.main(["--contract-fixture", str(self.root / "absent.json"),
                         "--cpp-runner", "runner"])


if __name__ == "__main__":
    unittest.main()
