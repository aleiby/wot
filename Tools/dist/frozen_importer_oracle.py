#!/usr/bin/env python3
"""Frozen cache-v3 migration gate for native and external-oracle receipts.

This is verification tooling, never a production dependency.  ``run-native``
runs only the native CacheBuilder automation group, accepts the sanitized UE
report, and writes a canonical receipt without host paths, times, or retail
values.  ``compare-receipts`` requires both native platforms and the separately
held external semantic-oracle job to have proved the same closed assertion set.

The freeze records the exact Python executable plus every stdlib/project file
that defines this gate.  The project dependencies are hashed as files; no
package manager, network lookup, or ambient dependency enumeration is used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import struct
import sys
import tempfile
from pathlib import Path
from typing import Any, Sequence

try:
    from . import cache_content_label
    from .validate_automation_report import (
        ReportRejected, load_report, validate_report)
except ImportError:
    import cache_content_label  # type: ignore
    from validate_automation_report import (  # type: ignore
        ReportRejected, load_report, validate_report)


NATIVE_FORMAT = "wot-cache-v3-native-parity-receipt-v1"
EXTERNAL_FORMAT = "wot-cache-v3-external-oracle-receipt-v1"
COMPARISON_FORMAT = "wot-cache-v3-parity-comparison-v1"
DESCRIPTOR_FORMAT = "cache-v3-production-descriptor-v1"
SUITE = "WoT.RetailReader.CacheBuilder"
PLATFORMS = ("Mac", "Win64")
REQUIRED_TESTS = (
    "WoT.RetailReader.CacheBuilder.InterruptedBuildRestarts",
    "WoT.RetailReader.CacheBuilder.ProductionDescriptorClosure",
    "WoT.RetailReader.CacheBuilder.RealRetailBuildPin",
    "WoT.RetailReader.CacheBuilder.SyntheticNineteenMapParity",
)
ASSERTION_FIELDS = (
    "interruption_manifest_bytes_equal",
    "nineteen_map_event_streams_equal",
    "object_bytes_equal",
    "progress_totals_equal",
    "rejected_field_paths_equal",
    "retail_manifest_pin_equal",
    "semantic_importer_events_equal",
    "synthetic_manifest_bytes_equal",
)
ASSERTION_EVIDENCE = {
    "interruption_manifest_bytes_equal": (
        ("WoT.RetailReader.CacheBuilder.InterruptedBuildRestarts",), False),
    "nineteen_map_event_streams_equal": (
        ("WoT.RetailReader.CacheBuilder.SyntheticNineteenMapParity",), True),
    "object_bytes_equal": (
        ("WoT.RetailReader.CacheBuilder.SyntheticNineteenMapParity",), True),
    "progress_totals_equal": ((
        "WoT.RetailReader.CacheBuilder.InterruptedBuildRestarts",
        "WoT.RetailReader.CacheBuilder.SyntheticNineteenMapParity"), True),
    "rejected_field_paths_equal": ((
        "WoT.RetailReader.CacheBuilder.ProductionDescriptorClosure",
        "WoT.RetailReader.CacheBuilder.SyntheticNineteenMapParity"), True),
    "retail_manifest_pin_equal": (
        ("WoT.RetailReader.CacheBuilder.RealRetailBuildPin",), False),
    "semantic_importer_events_equal": (
        ("WoT.RetailReader.CacheBuilder.SyntheticNineteenMapParity",), True),
    "synthetic_manifest_bytes_equal": (
        ("WoT.RetailReader.CacheBuilder.SyntheticNineteenMapParity",), True),
}
HEX_LENGTH = 64
ROOT = Path(__file__).resolve().parents[2]
CHECKED_DESCRIPTOR = (
    ROOT / "Build/WoTRelease/cache-v3-production-descriptor.json")
SYNTHETIC_FIXTURE = ROOT / "Tools/dist/fixtures/map-set-v3-synthetic.json"
NATIVE_SYNTHETIC_ARTIFACT = (
    ROOT / "Saved/ReleaseEvidence/cache-v3-parity/native-synthetic.json")
SYNTHETIC_ARTIFACT_FORMAT = "wot-cache-v3-synthetic-parity-v1"
DEPENDENCY_PATHS = (
    Path(__file__).resolve(),
    Path(__file__).with_name("build_payload_cache.py").resolve(),
    Path(__file__).with_name("cache_content_label.py").resolve(),
    Path(__file__).with_name("validate_automation_report.py").resolve(),
    SYNTHETIC_FIXTURE.resolve(),
    (ROOT / "Tools/dist/fixtures/bsp-native-parity-v1.json").resolve(),
    (ROOT / "Tools/dist/fixtures/cache-contract-v3-synthetic.json").resolve(),
)


class OracleError(ValueError):
    """An input cannot prove native/frozen parity."""


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("utf-8")


def sha256_bytes(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _read(path: Path, what: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise OracleError(f"cannot read {what}: {exc}") from exc


def _is_sha256(value: object) -> bool:
    return (isinstance(value, str) and len(value) == HEX_LENGTH
            and all(character in "0123456789abcdef" for character in value))


def _closed(document: object, fields: set[str], what: str) -> dict[str, Any]:
    if not isinstance(document, dict) or set(document) != fields:
        raise OracleError(f"{what} has a missing or unknown field")
    return document


def load_canonical(path: Path, what: str) -> tuple[dict[str, Any], bytes]:
    raw = _read(path, what)
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise OracleError(f"{what} is not UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict) or canonical_bytes(value) != raw:
        raise OracleError(f"{what} is not canonical JSON")
    return value, raw


def validate_descriptor(path: Path) -> tuple[dict[str, Any], str]:
    value, raw = load_canonical(path, "production descriptor")
    _closed(value, {"format", "graph", "graph_sha256", "importer_outputs",
                    "importer_spec"}, "production descriptor")
    if value["format"] != DESCRIPTOR_FORMAT:
        raise OracleError("production descriptor has the wrong format")
    if not isinstance(value["graph"], dict):
        raise OracleError("production descriptor graph is not an object")
    if sha256_bytes(canonical_bytes(value["graph"])) != value["graph_sha256"]:
        raise OracleError("production descriptor graph digest does not match")
    outputs = value["importer_outputs"]
    if not isinstance(outputs, list) or len(outputs) != 12:
        raise OracleError("production descriptor must contain twelve outputs")
    required_output_fields = {
        "audit_rule", "domain", "importer", "logical_id",
        "max_decoded_size", "runtime_consumer", "schema",
    }
    importers: set[str] = set()
    for index, output in enumerate(outputs):
        item = _closed(output, required_output_fields,
                       f"importer_outputs[{index}]")
        importer = item["importer"]
        if not isinstance(importer, str) or not importer or importer in importers:
            raise OracleError(f"importer_outputs[{index}].importer is invalid")
        for field in required_output_fields - {"max_decoded_size"}:
            if not isinstance(item[field], str) or not item[field]:
                raise OracleError(f"importer_outputs[{index}].{field} is empty")
        maximum = item["max_decoded_size"]
        if isinstance(maximum, bool) or not isinstance(maximum, int) or maximum <= 0:
            raise OracleError(
                f"importer_outputs[{index}].max_decoded_size is invalid")
        importers.add(importer)
    return value, sha256_bytes(raw)


def freeze_identity() -> dict[str, str]:
    interpreter = Path(sys.executable).resolve()
    try:
        interpreter_bytes = _read(interpreter, "Python interpreter")
    except OracleError:
        # Microsoft Store Python reports an execution alias in sys.executable;
        # sys.prefix owns the actual immutable interpreter image.
        interpreter = Path(sys.prefix).resolve() / (
            "python.exe" if os.name == "nt" else "bin/python3")
        interpreter_bytes = _read(interpreter, "Python interpreter")
    interpreter_sha256 = sha256_bytes(interpreter_bytes)
    records: list[dict[str, str]] = []
    for dependency in DEPENDENCY_PATHS:
        try:
            label = dependency.relative_to(ROOT).as_posix()
        except ValueError:
            # The only path allowed outside the repository is the interpreter,
            # which has its own digest above and is not included here.
            raise OracleError("an oracle dependency escaped the repository")
        records.append({"path": label,
                        "sha256": sha256_bytes(_read(dependency, label))})
    records.sort(key=lambda item: item["path"])
    return {
        "dependency_manifest_sha256": sha256_bytes(canonical_bytes(records)),
        "interpreter_sha256": interpreter_sha256,
        "oracle_sha256": sha256_bytes(_read(Path(__file__).resolve(), "oracle")),
    }


def _assertions(passed_tests: set[str], *,
                synthetic_artifact_matches: bool) -> dict[str, bool]:
    """Derive every claim from its successful specs and byte parity evidence."""
    result = {}
    for field in ASSERTION_FIELDS:
        tests, needs_artifact = ASSERTION_EVIDENCE[field]
        result[field] = (all(test in passed_tests for test in tests)
                         and (synthetic_artifact_matches
                              if needs_artifact else True))
    return result


def _framed_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<Q", len(encoded)) + encoded


def _synthetic_contract_sha256(maps: list[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    digest.update(b"WOT-CACHE-CONTRACT-IDENTITY-V2\0")
    digest.update(_framed_string("wot-cache-contract-v3"))
    digest.update(struct.pack("<Q", 3))
    digest.update(_framed_string("task25-synthetic-nineteen-map-parity"))
    digest.update(_framed_string("task25-test-key"))
    digest.update(struct.pack("<Q", 25))
    audits = (
        ("UWoTPayloadSubsystem", "audit.bsp-payload-v1"),
        ("UWoTPayloadSubsystem", "audit.world-payload-v1"),
    )
    digest.update(struct.pack("<Q", len(audits)))
    for consumer, audit in audits:
        digest.update(_framed_string(consumer))
        digest.update(_framed_string(audit))
    digest.update(struct.pack("<Q", len(maps) * 2))
    for item in maps:
        identity = item["identity"]
        source = f"Synthetic/Maps/{identity}.wot"
        facts = (
            (f"synthetic.bsp.{identity}", 5, f"{identity}.Model",
             "bsp-v1", "audit.bsp-payload-v1",
             f"cache/bsp/{identity}", "wot-bsp-v3"),
            (f"synthetic.world.{identity}", 6, f"{identity}.Level",
             "world-v1", "audit.world-payload-v1",
             f"cache/world/{identity}", "wot-world-v3"),
        )
        for fact_id, domain, export, importer, audit, logical_id, schema in facts:
            digest.update(_framed_string(fact_id))
            digest.update(struct.pack("<Q", domain))
            digest.update(_framed_string(source))
            digest.update(_framed_string(export))
            digest.update(_framed_string(importer))
            digest.update(_framed_string("UWoTPayloadSubsystem"))
            digest.update(_framed_string(audit))
            digest.update(struct.pack("<Q", 1))
            digest.update(_framed_string(logical_id))
            digest.update(_framed_string(schema))
            digest.update(struct.pack("<Q", 1073741824))
    digest.update(struct.pack("<Q", 1))
    digest.update(_framed_string("cache/world/Arena_01"))
    digest.update(bytes.fromhex(
        "96f79ca528ade0bc9a7b80b0db1f1c4705417148fb150532dc4dd090cb37cbb5"))
    return digest.hexdigest()


def synthetic_parity_inputs() -> dict[str, Any]:
    fixture, fixture_raw = load_canonical(
        SYNTHETIC_FIXTURE, "synthetic nineteen-map fixture")
    # The descriptor is still validated and supplies the rejection matrix; the
    # build identity records the cache content label, as the native builder does.
    descriptor, _descriptor_sha = validate_descriptor(CHECKED_DESCRIPTOR)
    content_label = cache_content_label.label(
        cache_content_label.compiled_revision())
    maps = fixture.get("maps")
    if not isinstance(maps, list) or len(maps) != 19:
        raise OracleError("synthetic parity fixture does not contain nineteen maps")
    blobs = []
    objects = {}
    event_roots = {}
    for index, item in enumerate(maps):
        if (not isinstance(item, dict) or not isinstance(item.get("identity"), str)
                or not _is_sha256(item.get("map_root"))
                or not isinstance(item.get("bsp"), dict)):
            raise OracleError(f"synthetic parity map {index} is incomplete")
        identity = item["identity"]
        world_raw = canonical_bytes(item)
        bsp_raw = canonical_bytes(item["bsp"])
        event_roots[identity] = item["map_root"]
        for domain, raw in (("bsp", bsp_raw), ("world", world_raw)):
            logical_id = f"cache/{domain}/{identity}"
            schema = f"wot-{domain}-v3"
            fact_id = f"synthetic.{domain}.{identity}"
            blobs.append({"bytes": raw, "logical_id": logical_id,
                          "satisfies": [fact_id], "schema": schema})
            objects[logical_id] = {
                "sha256": sha256_bytes(raw), "size": len(raw)}
    if not _is_sha256(fixture.get("set_root")):
        raise OracleError("synthetic parity fixture has no world-set root")
    rejected = sorted(
        path
        for output in descriptor["importer_outputs"]
        for path in (
            f"production-descriptor.importer_outputs.{output['importer']}",
            f"production-descriptor.importer_outputs.{output['importer']}.shipping-edge",
        )
    )
    return {
        "blobs": blobs,
        "build_identity": {
            "contract_sha256": _synthetic_contract_sha256(maps),
            "installer_sha256": sha256_bytes(fixture_raw),
            "production_descriptor_sha256": content_label,
        },
        "event_roots": event_roots,
        "objects": objects,
        # Public progress is strictly fact-count units
        # (source-byte weights stay internal to importers and never surface).
        "progress": {"completed": len(blobs), "total": len(blobs)},
        "rejected_field_paths": rejected,
        "world_set_root": fixture["set_root"],
    }


def generate_synthetic_parity_artifact() -> bytes:
    inputs = synthetic_parity_inputs()
    records = []
    for blob in inputs["blobs"]:
        raw = blob["bytes"]
        records.append({
            "logical_id": blob["logical_id"],
            "satisfies": sorted(blob["satisfies"]),
            "schema": blob["schema"],
            "sha256": sha256_bytes(raw),
            "size": len(raw),
        })
    records.sort(key=lambda item: item["logical_id"])
    manifest = {
        "blobs": records,
        "cache_version": 3,
        **inputs["build_identity"],
        "format": "wot-payload-cache",
        "front_end_descriptor_root": "0" * 64,
        "hash_algorithm": "sha256",
        "world_descriptors": [],
    }
    artifact = {
        "format": SYNTHETIC_ARTIFACT_FORMAT,
        "manifest_sha256": sha256_bytes(canonical_bytes(manifest)),
        "objects": inputs["objects"],
        "progress": inputs["progress"],
        "rejected_field_paths": inputs["rejected_field_paths"],
        "world_event_roots": inputs["event_roots"],
        "world_set_root": inputs["world_set_root"],
    }
    return canonical_bytes(artifact)


def _validate_test_inventory(report: dict[str, Any]) -> set[str]:
    tests = report.get("tests")
    assert isinstance(tests, list)  # validate_report established this.
    paths = {test.get("fullTestPath") for test in tests
             if isinstance(test, dict)}
    missing = sorted(set(REQUIRED_TESTS) - paths)
    if missing:
        raise OracleError("native report omitted required CacheBuilder specs: "
                          + ", ".join(missing))
    return {path for path in paths if isinstance(path, str)}


def _atomic_write(path: Path, raw: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = None
    temporary = None
    try:
        descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.",
                                            dir=str(path.parent))
        temporary = Path(name)
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = None
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def _runner_command(runner_path: Path) -> list[str]:
    """Argv prefix for a runner, native or script.

    A .py runner cannot be exec'd directly on Windows -- there is no shebang
    handling, so CreateProcess fails with WinError 193 "not a valid Win32
    application". The specs drive a Python fake runner, so spawn those through
    the current interpreter and leave native binaries untouched. Same fix as
    artifact_roundtrip._runner_command, for the same reason.
    """
    if runner_path.suffix.lower() == ".py":
        return [sys.executable, str(runner_path)]
    return [str(runner_path)]


def run_native(platform: str, production_descriptor: Path,
               native_runner: Path, receipt: Path) -> dict[str, Any]:
    if platform not in PLATFORMS:
        raise OracleError(f"unsupported native platform {platform!r}")
    _descriptor, descriptor_sha256 = validate_descriptor(production_descriptor)
    _checked, checked_raw = load_canonical(
        CHECKED_DESCRIPTOR, "checked-in production descriptor")
    supplied_raw = _read(production_descriptor, "production descriptor")
    if supplied_raw != checked_raw:
        raise OracleError(
            "supplied production descriptor differs from the checked-in document")
    runner_raw = _read(native_runner, "native runner")
    with tempfile.TemporaryDirectory(prefix="wot-cache-v3-parity-") as temporary:
        report = Path(temporary) / "index.json"
        command = [
            *_runner_command(native_runner), "--suite", SUITE,
            "--production-descriptor", str(production_descriptor),
            "--report", str(report),
        ]
        try:
            completed = subprocess.run(
                command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False, timeout=60 * 60,
                cwd=ROOT)
        except (OSError, subprocess.SubprocessError) as exc:
            raise OracleError(f"native parity runner could not complete: {exc}") from exc
        if completed.returncode != 0:
            # Native output can contain retail-local diagnostics.  Do not echo
            # or embed it; the operator can inspect the private job log.
            raise OracleError(
                f"native parity runner failed with exit code {completed.returncode}")
        try:
            report_value = load_report(report)
            summary = validate_report(report_value, SUITE)
        except ReportRejected as exc:
            raise OracleError(f"native automation report was rejected: {exc}") from exc
        passed_tests = _validate_test_inventory(report_value)

    _native_artifact, native_artifact_raw = load_canonical(
        NATIVE_SYNTHETIC_ARTIFACT, "native synthetic comparison artifact")
    python_artifact_raw = generate_synthetic_parity_artifact()
    synthetic_matches = native_artifact_raw == python_artifact_raw
    assertions = _assertions(
        passed_tests, synthetic_artifact_matches=synthetic_matches)
    failed_assertions = [field for field, passed in assertions.items() if not passed]
    if failed_assertions:
        raise OracleError("native evidence did not prove: "
                          + ", ".join(failed_assertions))
    artifact_sha256 = sha256_bytes(native_artifact_raw)

    result = {
        "assertions": assertions,
        "format": NATIVE_FORMAT,
        "freeze": freeze_identity(),
        "native_runner_sha256": sha256_bytes(runner_raw),
        "platform": platform,
        "production_descriptor_sha256": descriptor_sha256,
        "suite": SUITE,
        "synthetic_artifact_sha256": artifact_sha256,
        "test_count": summary["tests"],
    }
    _atomic_write(receipt, canonical_bytes(result))
    return result


def _validate_freeze(value: object, what: str) -> dict[str, str]:
    freeze = _closed(value, {"dependency_manifest_sha256", "interpreter_sha256",
                             "oracle_sha256"}, f"{what}.freeze")
    for field, digest in freeze.items():
        if not _is_sha256(digest):
            raise OracleError(f"{what}.freeze.{field} is not a SHA-256")
    return freeze  # type: ignore[return-value]


def _validate_assertions(value: object, what: str) -> dict[str, bool]:
    assertions = _closed(value, set(ASSERTION_FIELDS), f"{what}.assertions")
    for field in ASSERTION_FIELDS:
        if assertions[field] is not True:
            raise OracleError(f"{what}.assertions.{field} did not pass")
    return assertions  # type: ignore[return-value]


def _native_receipt(path: Path, platform: str) -> tuple[dict[str, Any], bytes]:
    value, raw = load_canonical(path, f"{platform} native receipt")
    _closed(value, {"assertions", "format", "freeze", "native_runner_sha256",
                    "platform", "production_descriptor_sha256", "suite",
                    "synthetic_artifact_sha256", "test_count"},
            f"{platform} native receipt")
    if value["format"] != NATIVE_FORMAT or value["platform"] != platform:
        raise OracleError(f"{platform} native receipt identity is wrong")
    if value["suite"] != SUITE:
        raise OracleError(f"{platform} native receipt used the wrong suite")
    if (isinstance(value["test_count"], bool)
            or not isinstance(value["test_count"], int)
            or value["test_count"] < len(REQUIRED_TESTS)):
        raise OracleError(f"{platform} native receipt has too few tests")
    for field in ("native_runner_sha256", "production_descriptor_sha256",
                  "synthetic_artifact_sha256"):
        if not _is_sha256(value[field]):
            raise OracleError(f"{platform} native receipt {field} is invalid")
    _validate_freeze(value["freeze"], f"{platform} native receipt")
    _validate_assertions(value["assertions"], f"{platform} native receipt")
    return value, raw


def _external_receipt(path: Path) -> tuple[dict[str, Any], bytes]:
    value, raw = load_canonical(path, "external oracle receipt")
    _closed(value, {"assertions", "format", "freeze",
                    "production_descriptor_sha256", "suite",
                    "synthetic_artifact_sha256"},
            "external oracle receipt")
    if value["format"] != EXTERNAL_FORMAT or value["suite"] != SUITE:
        raise OracleError("external oracle receipt identity is wrong")
    if not _is_sha256(value["production_descriptor_sha256"]):
        raise OracleError("external oracle descriptor identity is invalid")
    if not _is_sha256(value["synthetic_artifact_sha256"]):
        raise OracleError("external oracle synthetic artifact identity is invalid")
    _validate_freeze(value["freeze"], "external oracle receipt")
    _validate_assertions(value["assertions"], "external oracle receipt")
    return value, raw


def compare_receipts(mac: Path, win64: Path, external_oracle_receipt: Path,
                     output: Path) -> dict[str, Any]:
    mac_value, mac_raw = _native_receipt(mac, "Mac")
    win_value, win_raw = _native_receipt(win64, "Win64")
    external_value, external_raw = _external_receipt(external_oracle_receipt)
    descriptor_ids = {
        mac_value["production_descriptor_sha256"],
        win_value["production_descriptor_sha256"],
        external_value["production_descriptor_sha256"],
    }
    if len(descriptor_ids) != 1:
        raise OracleError("parity receipts used different production descriptors")

    # The gate source and its project dependency manifest are platform-neutral
    # and must be identical. The interpreter binary is intentionally allowed to
    # differ between Mac and Win64 (and the external job).
    freeze_ids = {
        (value["freeze"]["oracle_sha256"],
         value["freeze"]["dependency_manifest_sha256"])
        for value in (mac_value, win_value, external_value)
    }
    if len(freeze_ids) != 1:
        raise OracleError("parity receipts used different frozen oracle identities")

    expected_artifact_sha256 = sha256_bytes(
        generate_synthetic_parity_artifact())
    artifact_ids = {
        mac_value["synthetic_artifact_sha256"],
        win_value["synthetic_artifact_sha256"],
        external_value["synthetic_artifact_sha256"],
    }
    if artifact_ids != {expected_artifact_sha256}:
        raise OracleError(
            "parity receipts do not match the regenerated synthetic artifact")
    if not (mac_value["assertions"] == win_value["assertions"]
            == external_value["assertions"]):
        raise OracleError("native and external assertion sets differ")
    result = {
        "assertions": mac_value["assertions"],
        "external_oracle_receipt_sha256": sha256_bytes(external_raw),
        "format": COMPARISON_FORMAT,
        "mac_receipt_sha256": sha256_bytes(mac_raw),
        "production_descriptor_sha256": descriptor_ids.pop(),
        "synthetic_artifact_sha256": expected_artifact_sha256,
        "verdict": "equal",
        "win64_receipt_sha256": sha256_bytes(win_raw),
    }
    _atomic_write(output, canonical_bytes(result))
    return result


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    native = subparsers.add_parser("run-native")
    native.add_argument("--platform", required=True, choices=PLATFORMS)
    native.add_argument("--production-descriptor", required=True, type=Path)
    native.add_argument("--native-runner", required=True, type=Path)
    native.add_argument("--receipt", required=True, type=Path)
    compare = subparsers.add_parser("compare-receipts")
    compare.add_argument("--mac", required=True, type=Path)
    compare.add_argument("--win64", required=True, type=Path)
    compare.add_argument("--external-oracle-receipt", required=True, type=Path)
    compare.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "run-native":
            run_native(args.platform, args.production_descriptor,
                       args.native_runner, args.receipt)
        else:
            compare_receipts(args.mac, args.win64,
                             args.external_oracle_receipt, args.output)
    except OracleError as exc:
        print(f"cache-v3 parity: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
