#!/usr/bin/env python3
"""Release gate B: every blob of a platform's cache decodes with the guarded readers.

Gate A (cache_lineage.py) proves a release's fresh cache is byte-identical to
the previous release's whenever the content label is unchanged. Gate B proves
the candidate's guarded production readers decode every blob of that cache:
the WoTGuardedEngineTests program (built with WOT_GUARDED_BOOTSTRAP=1, as the
packaged Test and Shipping games are) runs WoT.GuardedEngine.CacheDecode, which
accepts the cache with the production identity, activates it and decodes each
manifest entry through the reader the guarded game runs for its schema. Editor
automation compiles those readers with WOT_GUARDED_BOOTSTRAP=0, so no editor
result can satisfy this gate: the receipt must say guard 1.

Input selection:
  --gate-a-receipt R --previous-cache P --candidate-cache C
      identity unchanged: decode the previous release's cache P (it must still be
      accepted and readable) and expect R's reference manifest; otherwise
      decode the candidate's fresh Shipping cache C and expect R's candidate
      manifest;
  --input-cache D --expect-manifest-sha256 H
      decode D, expecting manifest H.

Each cache directory holds current/manifest.json. The chosen current/ is
copied (real copies) into <work-root>/ContentCache so evidence stays pristine.

Stdlib only; runs on Python 3.9.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

DIST = Path(__file__).resolve().parent
if str(DIST) not in sys.path:
    sys.path.insert(0, str(DIST))

import cache_lineage  # noqa: E402
import release_formats as formats  # noqa: E402
import verify_release  # noqa: E402
from release_formats import FormatError  # noqa: E402

GATE_FORMAT = "wot-cache-gate-b-v1"
DECODE_FORMAT = "wot-cache-decode-receipt-v1"
DECODE_SUITE = "WoT.GuardedEngine.CacheDecode"
DECODE_RECEIPT_NAME = "decode-receipt.json"
PROGRAM_LOG_NAME = "program.log"
CACHE_DIR_NAME = "ContentCache"
DEFAULT_TIMEOUT_SECONDS = 3600
KNOWN_SCHEMAS = frozenset({
    "wot-texture-v3", "wot-sound-v3", "wot-animation-v3", "wot-mesh-v3",
    "wot-procedural-v3", "wot-bsp-v3", "wot-localization-v3", "wot-font-v3",
    "wot-class-default-v3", "wot-registry-v3", "wot-world-v3",
    "wot-front-end-world-v3"})
DECODE_KEYS = frozenset({
    "format", "guard", "platform", "configuration", "manifest_sha256",
    "installer_sha256", "contract_sha256", "label", "per_schema", "failures",
    "seconds"})

Runner = Callable[[List[str], Path, float], int]


class GateError(Exception):
    """Gate B refused; the message is the reason."""


def _sha256(value) -> bool:
    return (isinstance(value, str) and len(value) == 64
            and all(character in "0123456789abcdef" for character in value))


def _count(value, what: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise GateError(f"{what} is not a non-negative integer")
    return value


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _reject_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise GateError(f"duplicate JSON member {key!r}")
        result[key] = value
    return result


def _load_json(path: Path, what: str):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"),
                          object_pairs_hook=_reject_duplicates)
    except (OSError, UnicodeError, ValueError) as exc:
        raise GateError(f"cannot read {what} {path}: {exc}") from exc


# ---------------------------------------------------------------------------
# Selection and the working copy
# ---------------------------------------------------------------------------

def select_input(*, platform: str, gate_a_receipt: Optional[Path],
                 previous_cache: Optional[Path], candidate_cache: Optional[Path],
                 input_cache: Optional[Path],
                 expect_manifest_sha256: Optional[str]) -> Tuple[str, Path, str]:
    """(input kind, cache directory, expected manifest SHA-256)."""
    from_gate = (gate_a_receipt, previous_cache, candidate_cache)
    explicit = (input_cache, expect_manifest_sha256)
    if all(value is not None for value in from_gate) \
            and all(value is None for value in explicit):
        try:
            receipt = cache_lineage.load_gate_receipt(Path(gate_a_receipt), platform)
        except cache_lineage.GateError as exc:
            raise GateError(f"the gate A receipt is not usable: {exc}") from exc
        if receipt["identity_unchanged"]:
            return ("previous", Path(previous_cache),
                    receipt["reference"]["manifest_sha256"])
        return ("candidate", Path(candidate_cache),
                receipt["candidate"]["manifest_sha256"])
    if all(value is not None for value in explicit) \
            and all(value is None for value in from_gate):
        if not _sha256(expect_manifest_sha256):
            raise GateError("--expect-manifest-sha256 is not 64 lower-case hex "
                            "characters")
        return "explicit", Path(input_cache), expect_manifest_sha256
    raise GateError("give either --gate-a-receipt with --previous-cache and "
                    "--candidate-cache, or --input-cache with "
                    "--expect-manifest-sha256")


def _require_empty_work_root(work_root: Path) -> None:
    if work_root.exists() and (not work_root.is_dir() or any(work_root.iterdir())):
        raise GateError(f"work root {work_root} must be absent or empty")


def copy_cache(source: Path, work_root: Path, expected_sha256: str) -> Path:
    """Copy <source>/current to <work_root>/ContentCache/current; check the manifest."""
    current = source / "current"
    if not (current / "manifest.json").is_file():
        raise GateError(f"{source} holds no current/manifest.json")
    cache_root = work_root / CACHE_DIR_NAME
    cache_root.mkdir(parents=True)
    try:
        # symlinks=False copies the bytes a link names: the store refuses links.
        shutil.copytree(current, cache_root / "current", symlinks=False)
    except (OSError, shutil.Error) as exc:
        raise GateError(f"cannot copy {current}: {exc}") from exc
    copied = _file_sha256(cache_root / "current" / "manifest.json")
    if copied != expected_sha256:
        raise GateError(f"the copied manifest SHA-256 is {copied}, not the expected "
                        f"{expected_sha256}")
    return cache_root


def schema_counts(manifest_path: Path) -> Counter:
    """Blob count per schema, recomputed from the copied manifest."""
    manifest = _load_json(manifest_path, "copied cache manifest")
    blobs = manifest.get("blobs") if isinstance(manifest, dict) else None
    if not isinstance(blobs, list) or not blobs:
        raise GateError("the copied cache manifest has no blobs")
    counts: Counter = Counter()
    for index, blob in enumerate(blobs):
        schema = blob.get("schema") if isinstance(blob, dict) else None
        if not isinstance(schema, str) or not schema:
            raise GateError(f"copied cache manifest blob {index} has no schema")
        counts[schema] += 1
    return counts


# ---------------------------------------------------------------------------
# The program run and its receipt
# ---------------------------------------------------------------------------

def _program_command(program: Path) -> List[str]:
    """A .py program is spawned through this interpreter (Windows has no shebangs)."""
    if program.suffix.lower() == ".py":
        return [sys.executable, str(program)]
    return [str(program)]


def _run(command: List[str], log: Path, timeout: float) -> int:
    with log.open("wb") as handle:
        try:
            return subprocess.run(command, stdout=handle, stderr=subprocess.STDOUT,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired as exc:
            raise GateError(f"the program did not finish within {timeout:.0f} s") \
                from exc


def run_program(program: Path, cache_root: Path, receipt: Path, log: Path,
                timeout: float, runner: Optional[Runner] = None) -> None:
    command = _program_command(program) + [
        "--suite", DECODE_SUITE, "--cache-root", str(cache_root),
        "--decode-receipt", str(receipt), "-nullrhi", "-nosplash", "-unattended"]
    returncode = (runner or _run)(command, log, timeout)
    if returncode != 0:
        raise GateError(f"the program exited {returncode} (log {log})")


def validate_decode_receipt(path: Path, *, platform: str, expected_sha256: str,
                            counts: Counter) -> Dict[str, Dict[str, int]]:
    """The checked per-schema counts of one decode receipt."""
    if not path.is_file():
        raise GateError(f"the program wrote no decode receipt at {path}")
    raw = path.read_bytes()
    receipt = _load_json(path, "decode receipt")
    if not isinstance(receipt, dict) or receipt.get("format") != DECODE_FORMAT:
        raise GateError(f"the decode receipt is not {DECODE_FORMAT}")
    try:
        canonical = formats.canonical_bytes(receipt, "decode receipt")
    except FormatError as exc:
        raise GateError(str(exc)) from exc
    if canonical != raw:
        raise GateError("the decode receipt is not canonical JSON")
    if set(receipt) != DECODE_KEYS:
        raise GateError(f"the decode receipt fields differ: "
                        f"unknown={sorted(set(receipt) - DECODE_KEYS)} "
                        f"missing={sorted(DECODE_KEYS - set(receipt))}")
    guard = receipt["guard"]
    if isinstance(guard, bool) or guard != 1:
        raise GateError(f"the decode receipt has guard {guard!r}: only a guarded "
                        "(WOT_GUARDED_BOOTSTRAP=1) build satisfies gate B")
    if receipt["platform"] != platform:
        raise GateError(f"the decode receipt is for {receipt['platform']!r}, not "
                        f"{platform}")
    if receipt["manifest_sha256"] != expected_sha256:
        raise GateError(f"the decode receipt names manifest "
                        f"{receipt['manifest_sha256']}, not {expected_sha256}")
    failures = receipt["failures"]
    if not isinstance(failures, list):
        raise GateError("the decode receipt failures are not a list")
    if failures:
        first = failures[0] if isinstance(failures[0], dict) else {}
        raise GateError(f"{len(failures)} blobs failed to decode; first: "
                        f"{first.get('logical_id')!r} ({first.get('schema')!r}): "
                        f"{first.get('error')!r}")
    per_schema = receipt["per_schema"]
    if not isinstance(per_schema, dict):
        raise GateError("the decode receipt per_schema is not an object")
    unknown = sorted(set(per_schema) - KNOWN_SCHEMAS)
    if unknown:
        raise GateError(f"the decode receipt names unknown schemas {unknown}")
    if set(per_schema) != set(counts):
        raise GateError(f"the decode receipt schemas {sorted(per_schema)} are not the "
                        f"manifest's {sorted(counts)}")
    checked: Dict[str, Dict[str, int]] = {}
    for schema in sorted(per_schema):
        row = per_schema[schema]
        if not isinstance(row, dict) or set(row) != {"manifest", "decoded"}:
            raise GateError(f"the decode receipt row for {schema} is malformed")
        manifest = _count(row["manifest"], f"{schema}.manifest")
        decoded = _count(row["decoded"], f"{schema}.decoded")
        if manifest != counts[schema] or decoded != counts[schema]:
            raise GateError(f"{schema}: the manifest holds {counts[schema]} blobs, the "
                            f"receipt reports manifest {manifest}, decoded {decoded}")
        checked[schema] = {"decoded": decoded, "manifest": manifest}
    total = sum(counts.values())
    if sum(row["decoded"] for row in checked.values()) != total \
            or sum(row["manifest"] for row in checked.values()) != total:
        raise GateError(f"the decoded total is not the manifest's {total} blobs")
    _count(receipt["seconds"], "seconds")
    return checked


# ---------------------------------------------------------------------------
# Gate B
# ---------------------------------------------------------------------------

def run_gate(*, platform: str, program: Path, work_root: Path,
             gate_a_receipt: Optional[Path] = None,
             previous_cache: Optional[Path] = None,
             candidate_cache: Optional[Path] = None,
             input_cache: Optional[Path] = None,
             expect_manifest_sha256: Optional[str] = None,
             timeout: float = DEFAULT_TIMEOUT_SECONDS,
             runner: Optional[Runner] = None) -> dict:
    """Select, copy, decode and validate. Returns the passing gate B receipt."""
    if platform not in formats.PLATFORMS:
        raise GateError(f"unknown platform {platform!r}")
    started = time.monotonic()
    kind, source, expected = select_input(
        platform=platform, gate_a_receipt=gate_a_receipt,
        previous_cache=previous_cache, candidate_cache=candidate_cache,
        input_cache=input_cache, expect_manifest_sha256=expect_manifest_sha256)
    program = Path(program).resolve()
    if not program.is_file():
        raise GateError(f"program {program} does not exist")
    work_root = Path(work_root).resolve()
    _require_empty_work_root(work_root)
    cache_root = copy_cache(Path(source), work_root, expected)
    counts = schema_counts(cache_root / "current" / "manifest.json")
    decode_receipt = work_root / DECODE_RECEIPT_NAME
    run_program(program, cache_root, decode_receipt, work_root / PROGRAM_LOG_NAME,
                timeout, runner)
    per_schema = validate_decode_receipt(
        decode_receipt, platform=platform, expected_sha256=expected, counts=counts)
    return {
        "format": GATE_FORMAT,
        "input": kind,
        "manifest_sha256": expected,
        "per_schema": per_schema,
        "platform": platform,
        "program_sha256": _file_sha256(program),
        "seconds": int(math.ceil(time.monotonic() - started)),
        "verdict": "pass",
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = result.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="decode every blob of one cache with the "
                              "guarded readers")
    run.add_argument("--platform", required=True, choices=formats.PLATFORMS)
    run.add_argument("--program", required=True,
                     help="the guarded WoTGuardedEngineTests program")
    run.add_argument("--work-root", required=True,
                     help="new empty directory for the cache copy and the run")
    run.add_argument("--out", required=True, help="gate B receipt to write")
    run.add_argument("--gate-a-receipt", help="this platform's passing gate A receipt")
    run.add_argument("--previous-cache",
                     help="the previous release's cache (holds current/)")
    run.add_argument("--candidate-cache",
                     help="the candidate's Shipping smoke cache (holds current/)")
    run.add_argument("--input-cache", help="a cache to decode (holds current/)")
    run.add_argument("--expect-manifest-sha256",
                     help="the SHA-256 --input-cache's manifest must have")
    run.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS,
                     help="seconds the program may run")
    return result


def _optional_path(value: Optional[str]) -> Optional[Path]:
    return Path(value) if value is not None else None


def main(argv: Optional[Sequence[str]] = None, *,
         runner: Optional[Runner] = None) -> int:
    args = parser().parse_args(argv)
    try:
        receipt = run_gate(
            platform=args.platform, program=Path(args.program),
            work_root=Path(args.work_root),
            gate_a_receipt=_optional_path(args.gate_a_receipt),
            previous_cache=_optional_path(args.previous_cache),
            candidate_cache=_optional_path(args.candidate_cache),
            input_cache=_optional_path(args.input_cache),
            expect_manifest_sha256=args.expect_manifest_sha256,
            timeout=args.timeout, runner=runner)
    except GateError as exc:
        verify_release.write_receipt(Path(args.out), {
            "format": GATE_FORMAT, "platform": args.platform, "reason": str(exc),
            "verdict": "fail"})
        print(f"cache_decode_gate: FAIL: {exc}", file=sys.stderr)
        return 1
    verify_release.write_receipt(Path(args.out), receipt)
    total = sum(row["decoded"] for row in receipt["per_schema"].values())
    print(f"cache_decode_gate: PASS ({args.platform}, {receipt['input']} cache "
          f"{receipt['manifest_sha256']}, {total} blobs decoded in "
          f"{receipt['seconds']} s); receipt {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
