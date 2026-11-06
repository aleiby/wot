#!/usr/bin/env python3
"""Drive a native canonical-JSON runner and compare exact bytes.

Python supplies every fixture; the native runner parses and re-emits it; this
driver compares raw bytes.  There is deliberately no second production parser
here: `json.dumps(value, sort_keys=True, separators=(",", ":"),
ensure_ascii=True)` is the canonical form by definition, so agreement is proved
by byte equality, not by a Python re-implementation of the grammar.

Wire protocol (length-prefixed, little-endian, over the runner's stdin/stdout):

    request   u8  Opcode            0 = evaluate canonical JSON
                                    1 = shut down
                                    2 = evaluate contract payload
                                    3 = evaluate signed contract
              u32 Length
              u8[Length] Candidate
    response  u8  Status            0 = accepted, 1 = rejected
              u32 Length
              u8[Length] Payload    accepted: the re-emitted canonical bytes
                                    rejected: "<ErrorCode>: <message>" in UTF-8

The opcode exists because the empty document is itself a fixture: a zero-length
candidate must not be confused with "stop".

Usage:
    artifact_roundtrip.py --runner <exe> [--runner-arg ARG ...] [--list]
    artifact_roundtrip.py --contract-fixture <signed.json> --cpp-runner <exe>
"""
from __future__ import annotations

import argparse
import copy
import json
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

try:
    from .cache_contract_v3 import (
        ContractError, startup_critical_logical_ids_sha256,
        validate_document, validate_payload)
except ImportError:
    from cache_contract_v3 import (  # type: ignore
        ContractError, startup_critical_logical_ids_sha256,
        validate_document, validate_payload)

OPCODE_EVALUATE = 0
OPCODE_SHUTDOWN = 1
OPCODE_EVALUATE_CONTRACT_PAYLOAD = 2
OPCODE_EVALUATE_SIGNED_CONTRACT = 3
STATUS_ACCEPTED = 0
STATUS_REJECTED = 1
DEFAULT_RUNNER_ARG = "--artifact-roundtrip"
DEFAULT_TIMEOUT_SECONDS = 120


def canonical(value: object) -> bytes:
    """The canonical serialization the native writer must reproduce exactly."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("utf-8")


def _nested(containers: int) -> object:
    """A document whose container nesting depth is exactly `containers`.

    Depth counts containers, not the scalar inside them: FWoTCanonicalJsonLimits
    allows exactly MaxDepth (32) and must reject 33.
    """
    value: object = 0
    for _ in range(containers):
        value = {"a": value}
    return value


def _max_document_value() -> object:
    base_string_size = 4 * 1024 * 1024 // 4 - 4
    return ["a" * (base_string_size + 3)] + ["a" * base_string_size] * 3


# Accepted fixtures: (name, canonical bytes).  Built from Python values wherever
# `json.dumps` already produces the canonical form, and from literal bytes only
# where a value cannot be expressed as a Python object (there are none today).
ACCEPTED: tuple[tuple[str, bytes], ...] = tuple(
    (name, canonical(value)) for name, value in (
        ("empty-object", {}),
        ("empty-array", []),
        ("null", None),
        ("true", True),
        ("false", False),
        ("zero", 0),
        ("negative", -1),
        ("int64-min", -9223372036854775808),
        ("int64-max", 9223372036854775807),
        ("scalar-string", "wot"),
        ("sorted-keys", {"alpha": 1, "beta": {"gamma": [1, 2, 3]}, "zeta": None}),
        ("string-escapes", "quote:\" backslash:\\ nl:\n tab:\t cr:\r bs:\b ff:\f"),
        ("control-escape", "\u0001\u001f"),
        ("delete-escape", "\u007f"),
        ("non-ascii-escape", "caf\u00e9 \u4e2d"),
        ("non-bmp-surrogate-pair", "\U0001f600"),
        ("nested-array", [[], [[]], [[[1]]]]),
        ("max-depth", _nested(32)),
        ("max-container-members", [0] * 65536),
        ("max-string-bytes", "a" * (1024 * 1024)),
        ("max-document-bytes", _max_document_value()),
        ("release-shaped", {
            "asset": "WoT-Mac-stripped-abc123456.zip",
            "audit": {"cache_sha256": "c" * 64, "container_count": 4,
                      "verdict": "pass"},
            "format": "wot-stripped-release-v1",
            "generated_utc": "2026-07-24T00:00:00+00:00",
            "git_commit": "abc123456",
            "platform": "Mac",
            "sha256": "a" * 64,
            "size": 123,
        }),
    ))

# Rejected fixtures: (name, raw bytes, expected EWoTBootstrapError name).
REJECTED: tuple[tuple[str, bytes, str], ...] = (
    ("empty-input", b"", "NonCanonicalJson"),
    ("trailing-space", b"{} ", "NonCanonicalJson"),
    ("trailing-data", b"{}x", "NonCanonicalJson"),
    ("trailing-newline", b"{}\n", "NonCanonicalJson"),
    ("leading-space", b" {}", "NonCanonicalJson"),
    ("inner-whitespace", b'{"a": 1}', "NonCanonicalJson"),
    ("unsorted-keys", b'{"b":1,"a":2}', "NonCanonicalJson"),
    ("duplicate-key", b'{"a":1,"a":2}', "DuplicateField"),
    ("negative-zero", b"-0", "InvalidNumber"),
    ("exponent", b"1e5", "InvalidNumber"),
    ("fraction", b"1.0", "InvalidNumber"),
    ("leading-zero", b"01", "InvalidNumber"),
    ("bare-minus", b"-", "InvalidNumber"),
    ("int64-overflow", b"9223372036854775808", "InvalidNumber"),
    ("nan", b"NaN", "NonCanonicalJson"),
    ("infinity", b"Infinity", "NonCanonicalJson"),
    ("solidus-escape", b'"\\/"', "NonCanonicalJson"),
    ("unnecessary-escape", b'"\\u0041"', "NonCanonicalJson"),
    ("uppercase-hex-escape", b'"\\u00E9"', "NonCanonicalJson"),
    ("shorthand-required", b'"\\u000a"', "NonCanonicalJson"),
    ("unknown-escape", b'"\\x41"', "NonCanonicalJson"),
    # Legal JSON, but FString terminates at NUL: accepting it would truncate the
    # value and break the byte-exact round trip signatures depend on.
    ("nul-escape", b'"\\u0000"', "NonCanonicalJson"),
    ("raw-utf8", b'"\xc3\xa9"', "NonCanonicalJson"),
    ("raw-control-byte", b'"\x01"', "NonCanonicalJson"),
    ("overlong-utf8", b'"\xc0\xaf"', "InvalidUtf8"),
    ("truncated-utf8", b'"\xc3"', "InvalidUtf8"),
    ("unpaired-high-surrogate", b'"\\ud800"', "InvalidUtf8"),
    ("lone-low-surrogate", b'"\\udc00"', "InvalidUtf8"),
    ("high-surrogate-then-scalar", b'"\\ud800\\u0041"', "InvalidUtf8"),
    ("missing-value", b'{"a":}', "NonCanonicalJson"),
    ("unterminated-object", b'{"a":1', "NonCanonicalJson"),
    ("unterminated-string", b'"abc', "NonCanonicalJson"),
    ("depth-over-limit", canonical(_nested(33)), "SizeLimit"),
    ("container-members-over-limit", canonical([0] * 65537), "SizeLimit"),
    ("string-bytes-over-limit", canonical("a" * (1024 * 1024 + 1)), "SizeLimit"),
    ("document-bytes-over-limit",
     canonical(_max_document_value())[:-2] + b'a"]', "SizeLimit"),
)


class RoundTripFailure(Exception):
    """The runner disagreed with the canonical fixture set."""


class Runner:
    """One long-lived native process speaking the length-prefixed protocol.

    Reads go through a background thread so that a runner which stops answering
    fails the driver on a deadline instead of hanging it.  `select` is not used:
    it cannot wait on pipes on Windows, and this driver runs on both hosts.
    """

    def __init__(self, command: list[str], timeout: float):
        self._timeout = timeout
        self._process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self._buffer = bytearray()
        self._lock = threading.Lock()
        self._arrived = threading.Condition(self._lock)
        self._eof = False
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()

    def _pump(self) -> None:
        stream = self._process.stdout
        try:
            while True:
                block = stream.read1(65536) if hasattr(stream, "read1") \
                    else stream.read(1)
                if not block:
                    break
                with self._arrived:
                    self._buffer.extend(block)
                    self._arrived.notify_all()
        except (OSError, ValueError):
            pass
        finally:
            with self._arrived:
                self._eof = True
                self._arrived.notify_all()

    def _read_exactly(self, count: int) -> bytes:
        deadline = time.monotonic() + self._timeout
        with self._arrived:
            while len(self._buffer) < count:
                if self._eof:
                    raise RoundTripFailure(
                        f"runner produced {len(self._buffer)} of {count} "
                        "expected bytes before closing stdout")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RoundTripFailure(
                        f"runner produced {len(self._buffer)} of {count} "
                        f"expected bytes within {self._timeout:g}s")
                self._arrived.wait(remaining)
            data = bytes(self._buffer[:count])
            del self._buffer[:count]
            return data

    def exchange(self, candidate: bytes, *,
                 opcode: int = OPCODE_EVALUATE) -> tuple[int, bytes]:
        try:
            self._process.stdin.write(
                struct.pack("<BI", opcode, len(candidate)) + candidate)
            self._process.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as exc:
            raise RoundTripFailure(f"runner closed stdin: {exc}") from exc
        status = self._read_exactly(1)[0]
        if status not in (STATUS_ACCEPTED, STATUS_REJECTED):
            raise RoundTripFailure(f"runner returned status {status}")
        (length,) = struct.unpack("<I", self._read_exactly(4))
        return status, self._read_exactly(length)

    def close(self) -> int:
        try:
            self._process.stdin.write(struct.pack("<BI", OPCODE_SHUTDOWN, 0))
            self._process.stdin.flush()
            self._process.stdin.close()
        except (BrokenPipeError, OSError, ValueError):
            pass
        try:
            exit_code = self._process.wait(timeout=self._timeout)
            self._reader.join(timeout=self._timeout)
            if self._process.stdout is not None:
                self._process.stdout.close()
            return exit_code
        except subprocess.TimeoutExpired:
            self.abort()
            raise RoundTripFailure("runner did not exit after shutdown") from None

    def abort(self) -> None:
        try:
            self._process.kill()
        except OSError:
            pass
        try:
            self._process.wait(timeout=self._timeout)
        except subprocess.TimeoutExpired:
            pass
        for stream in (self._process.stdin, self._process.stdout):
            try:
                if stream is not None:
                    stream.close()
            except OSError:
                pass


def contract_fixture_set(path: Path) -> list[tuple[str, int, bytes]]:
    """Accepted fixtures derived from a signed cache-contract-v3 document.

    The checked-in file must itself be canonical bytes — a fixture that is not
    canonical would make byte-equality vacuous — so that is asserted here
    rather than trusted.  The set then covers the whole signed document, the
    payload alone, and a valid one-fact payload projection for every fact, so
    opcode 2 always receives the payload object its wire contract specifies
    while exercising each fact independently.
    """
    raw = path.read_bytes()
    document = json.loads(raw)
    if canonical(document) != raw:
        raise RoundTripFailure(
            f"{path} is not canonical JSON; regenerate it before comparing")
    validate_document(document)
    fixtures = [
        ("document", OPCODE_EVALUATE_SIGNED_CONTRACT, raw),
        ("payload", OPCODE_EVALUATE_CONTRACT_PAYLOAD,
         canonical(document["payload"])),
    ]
    for fact in document["payload"]["facts"]:
        one_fact_payload = copy.deepcopy(document["payload"])
        one_fact_payload["facts"] = [copy.deepcopy(fact)]
        logical_ids = [output["logical_id"] for output in fact["outputs"]]
        one_fact_payload["startup_critical_logical_ids"] = logical_ids
        one_fact_payload["startup_critical_logical_ids_sha256"] = \
            startup_critical_logical_ids_sha256(logical_ids)
        validate_payload(one_fact_payload)
        fixtures.append((f"fact/{fact['fact_id']}",
                         OPCODE_EVALUATE_CONTRACT_PAYLOAD,
                         canonical(one_fact_payload)))
    return fixtures


MANDATED_CONTRACT_REMOVALS: tuple[tuple[str, tuple[object, ...]], ...] = (
    ("importer", ("facts", 0, "importer")),
    ("runtime-consumer", ("facts", 0, "runtime_consumer")),
    ("audit-rule", ("facts", 0, "audit_rule")),
    ("source-package", ("facts", 0, "source", "package")),
    ("output-schema", ("facts", 0, "outputs", 0, "schema")),
    ("maximum-decoded-size",
     ("facts", 0, "outputs", 0, "max_decoded_size")),
)


def contract_mutation_set(document: dict[str, object]) -> list[tuple[str, bytes, str]]:
    """Generate the six parser-drift vectors and Python's exact field paths."""
    mutations = []
    for name, selectors in MANDATED_CONTRACT_REMOVALS:
        payload = copy.deepcopy(document["payload"])
        target = payload
        for selector in selectors[:-1]:
            target = target[selector]
        del target[selectors[-1]]
        try:
            validate_payload(payload)
        except ContractError as exc:
            mutations.append((name, canonical(payload), exc.path))
        else:
            raise RoundTripFailure(
                f"Python accepted mandatory contract mutation {name}")
    return mutations


def signed_contract_tamper_set(document: dict[str, object]) -> list[tuple[str, bytes]]:
    signature_changed = copy.deepcopy(document)
    signature = signature_changed["signature"]
    signature_changed["signature"] = signature[:-1] + (
        "0" if signature[-1] != "0" else "1")
    payload_changed = copy.deepcopy(document)
    payload_changed["payload"]["release_sequence"] += 1
    return [
        ("signature", canonical(signature_changed)),
        ("payload", canonical(payload_changed)),
    ]


def _runner_command(runner_path: Path) -> list[str]:
    """Argv prefix for a runner, native or script.

    A .py runner cannot be exec'd directly on Windows -- there is no shebang
    handling, so CreateProcess fails with WinError 193 "not a valid Win32
    application". The specs drive a Python fake runner, so spawn those through
    the current interpreter and leave native binaries untouched.
    """
    if runner_path.suffix.lower() == ".py":
        return [sys.executable, str(runner_path)]
    return [str(runner_path)]


def run_contract_fixture(path: Path, runner_path: Path, *,
                         timeout: float = DEFAULT_TIMEOUT_SECONDS) -> list[str]:
    """Drive every contract-derived fixture through the native runner."""
    problems: list[str] = []
    try:
        fixtures = contract_fixture_set(path)
        document = json.loads(path.read_bytes())
        mutations = contract_mutation_set(document)
        signed_tampers = signed_contract_tamper_set(document)
    except (OSError, ValueError, KeyError, TypeError, RoundTripFailure) as exc:
        return [f"contract fixture unusable: {exc}"]
    runner = Runner(_runner_command(runner_path) + [DEFAULT_RUNNER_ARG], timeout)
    try:
        for name, opcode, expected in fixtures:
            status, payload = runner.exchange(expected, opcode=opcode)
            if status != STATUS_ACCEPTED:
                problems.append(
                    f"accept/{name}: runner rejected it: "
                    f"{payload.decode('utf-8', 'replace')}")
            elif payload != expected:
                problems.append(
                    f"accept/{name}: re-emitted {len(payload)} bytes that "
                    f"differ from Python's canonical {len(expected)}")
        for name, candidate, expected_path in mutations:
            status, payload = runner.exchange(
                candidate, opcode=OPCODE_EVALUATE_CONTRACT_PAYLOAD)
            text = payload.decode("utf-8", "replace")
            _code, separator, native_path = text.partition(": ")
            if status != STATUS_REJECTED:
                problems.append(f"reject/{name}: native parser accepted the mutation")
            elif not separator:
                problems.append(f"reject/{name}: malformed native error {text!r}")
            elif native_path != expected_path:
                problems.append(
                    f"reject/{name}: native path {native_path!r}, "
                    f"Python path {expected_path!r}")
        for name, candidate in signed_tampers:
            status, payload = runner.exchange(
                candidate, opcode=OPCODE_EVALUATE_SIGNED_CONTRACT)
            if status != STATUS_REJECTED:
                problems.append(
                    f"reject/signed-{name}: native parser accepted tampering")
        exit_code = runner.close()
        if exit_code != 0:
            problems.append(f"runner exited {exit_code}")
    except RoundTripFailure as exc:
        problems.append(str(exc))
        runner.abort()
    return problems


def run_fixtures(command: list[str], *,
                 timeout: float = DEFAULT_TIMEOUT_SECONDS) -> list[str]:
    """Exercise every fixture; return the list of disagreements."""
    problems: list[str] = []
    runner = Runner(command, timeout)
    try:
        for name, expected in ACCEPTED:
            status, payload = runner.exchange(expected)
            if status != STATUS_ACCEPTED:
                problems.append(
                    f"accept/{name}: runner rejected it: "
                    f"{payload.decode('utf-8', 'replace')}")
            elif payload != expected:
                problems.append(
                    f"accept/{name}: re-emitted {payload!r}, expected {expected!r}")
        for name, candidate, code in REJECTED:
            status, payload = runner.exchange(candidate)
            text = payload.decode("utf-8", "replace")
            if status != STATUS_REJECTED:
                problems.append(
                    f"reject/{name}: runner accepted {len(candidate)} forbidden "
                    f"bytes as {len(payload)} bytes")
            elif not text.startswith(code):
                problems.append(
                    f"reject/{name}: expected {code}, runner said {text!r}")
        exit_code = runner.close()
        if exit_code != 0:
            problems.append(f"runner exited {exit_code}")
    except RoundTripFailure as exc:
        problems.append(str(exc))
        runner.abort()
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare a native canonical-JSON runner against Python "
                    "supplied fixtures, byte for byte.")
    parser.add_argument("--runner", type=Path,
                        help="native runner executable")
    parser.add_argument("--contract-fixture", type=Path,
                        help="signed cache-contract-v3 JSON document")
    parser.add_argument("--cpp-runner", type=Path,
                        help="native cache fixture runner (WoTRetailReaderTests)")
    parser.add_argument("--runner-arg", action="append", default=None,
                        help=f"argument passed to the runner "
                             f"(default: {DEFAULT_RUNNER_ARG})")
    parser.add_argument("--timeout", type=float,
                        default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--list", action="store_true",
                        help="print the fixture set and exit")
    args = parser.parse_args(argv)

    if args.contract_fixture is not None:
        if args.cpp_runner is None:
            parser.error("--contract-fixture needs --cpp-runner")
        if not args.contract_fixture.is_file():
            parser.error(f"contract fixture does not exist: {args.contract_fixture}")
        problems = run_contract_fixture(
            args.contract_fixture, args.cpp_runner, timeout=args.timeout)
        if problems:
            for problem in problems:
                print(f"MISMATCH {problem}", file=sys.stderr)
            return 1
        contract_document = json.loads(args.contract_fixture.read_bytes())
        accepted_count = len(contract_fixture_set(args.contract_fixture))
        rejected_count = (len(contract_mutation_set(contract_document))
                          + len(signed_contract_tamper_set(contract_document)))
        print(f"ROUND TRIP OK {accepted_count} accepted contract fixtures "
              f"agreed byte-for-byte and {rejected_count} mutation/tamper "
              "vectors rejected with matching field paths")
        return 0

    if args.list:
        for name, expected in ACCEPTED:
            print(f"accept {name}\t{len(expected)} bytes")
        for name, candidate, code in REJECTED:
            print(f"reject {name}\t{code}\t{len(candidate)} bytes")
        return 0
    if args.runner is None:
        parser.error("--runner is required unless --list is given")

    runner_args = args.runner_arg if args.runner_arg else [DEFAULT_RUNNER_ARG]
    command = [str(args.runner), *runner_args]
    problems = run_fixtures(command, timeout=args.timeout)
    if problems:
        for problem in problems:
            print(f"MISMATCH {problem}", file=sys.stderr)
        return 1
    print(f"ROUND TRIP OK {len(ACCEPTED)} accepted, {len(REJECTED)} rejected "
          f"fixtures agreed byte-for-byte")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
