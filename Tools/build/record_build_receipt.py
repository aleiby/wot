#!/usr/bin/env python3
"""Record what a guarded build ACTUALLY compiled, and refuse a wrong one.

The source/toolchain receipt answers "which engine produced this binary?".  It
cannot answer "was this really the guarded Test configuration, with exactly the
test-only definitions that configuration is allowed to have?" -- and that is the
question a release needs answered, because the difference between a Test and a
Shipping image is a set of preprocessor definitions nobody can see afterwards by
looking at the binary.

Evidence comes from UBT's own generated ``Definitions.h`` files under
``Intermediate/Build/<Platform>/<arch>/<Target>/<Configuration>/<Module>/``.
Those are the exact macros the compiler was handed, written by UBT during the
build we are recording.  Deriving the same answer by re-reading the .Target.cs
would only restate our intent; these files are the outcome.

This is a GATE, not a log: a guarded configuration missing its guard, a Shipping
build carrying a test-only definition, or a Test build missing one is rejected
with a non-zero exit, so it cannot be published as evidence.

Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

RECEIPT_FORMAT = "wot-guarded-build-receipt-v1"

# Definitions this policy gives meaning to. Anything else matching WOT_ is still
# recorded -- an unexpected one is worth seeing in the evidence.
GUARD_DEFINITIONS = ("WOT_GUARDED_BOOTSTRAP",)
TEST_ONLY_DEFINITIONS = ("WOT_BOOTSTRAP_TEST_IDENTITIES", "WOT_BOOTSTRAP_TEST_ENDPOINTS")
GUARDED_CONFIGURATIONS = ("Test", "Shipping")

# Some targets are guarded in EVERY configuration because being guarded is their
# reason to exist -- a Development build of the guarded-engine harness still has
# to exercise the guarded startup path, so its target rules set the guard
# unconditionally. Keying the policy off configuration alone made those builds
# unreceiptable: the recorder rejected the very thing the target is for.
# Unknown targets keep the configuration-driven rule, so this stays fail-closed.
GUARDED_BY_DEFINITION_TARGETS = {
    "WoTGuardedEngineTests":
        "exists to exercise the guarded startup path in every configuration",
}

# Test-harness PROGRAMS are never shipped, and some must exercise the test
# endpoint/identity paths to test them at all -- WoTUpdateTransportsTests drives
# transport rewrites against a loopback endpoint. The
# rule these definitions exist to enforce is "never in a shipped image", so the
# allowance is narrow and named, and Shipping is STILL refused for every target
# including these. An unnamed target gets no allowance.
TEST_HARNESS_TARGETS = {
    "WoTUpdateTransportsTests":
        "drives release transport rewrites against loopback; never shipped",
}

# WOT_* plus the guarded game's crash-reporter profile (WoT.Target.cs), which
# build_stripped_pak.py requires in a guarded Test receipt. Without these
# names the recorder could never prove that profile, and every guarded Test
# package failed at its product-receipt check.
PROFILE_DEFINITIONS = (
    "NOINITCRASHREPORTER",
    "CRASH_REPORTER_WITH_ANALYTICS",
    "USE_CRASH_REPORTER_MONITOR",
)
DEFINE = re.compile(
    r"^#define\s+(WOT_[A-Z0-9_]+|" + "|".join(PROFILE_DEFINITIONS)
    + r")\s*(.*)$")


class ReceiptRejected(Exception):
    """The build did not compile what its configuration requires."""


def collect_definitions(project_dir: Path, target: str, platform: str,
                        configuration: str) -> tuple[dict[str, str], int]:
    """Union the WOT_* defines UBT wrote for this target/config."""
    root = project_dir / "Intermediate" / "Build" / platform
    if not root.is_dir():
        raise ReceiptRejected(f"no build intermediates at {root}; build first")

    # <arch> varies (arm64/x64) and Win64 has no arch level, so glob both shapes.
    candidates = list(root.glob(f"*/{target}/{configuration}/*/Definitions.h"))
    candidates += list(root.glob(f"{target}/{configuration}/*/Definitions.h"))
    if not candidates and target.endswith("Editor"):
        # Editor targets share UBT's UnrealEditor build environment: modules
        # land under <arch>/UnrealEditor/<config>/<Module>/Definitions.<Module>.h
        # rather than under the project target's own directory. Only project
        # modules (WoT*) are evidence — engine-module definitions in the shared
        # tree can be stale leftovers from unrelated editor builds and must not
        # satisfy (or contradict) this target's WOT_* policy.
        candidates = list(
            root.glob(f"*/UnrealEditor/{configuration}/WoT*/Definitions.WoT*.h"))
        candidates += list(
            root.glob(f"UnrealEditor/{configuration}/WoT*/Definitions.WoT*.h"))
    if not candidates:
        raise ReceiptRejected(
            f"no Definitions.h for {target}/{platform}/{configuration} under {root}; "
            "the build did not run, or ran with a different configuration")

    found: dict[str, str] = {}
    conflicts: dict[str, set[str]] = {}
    for path in candidates:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            match = DEFINE.match(line.strip())
            if not match:
                continue
            name, value = match.group(1), match.group(2).strip()
            if name in found and found[name] != value:
                conflicts.setdefault(name, {found[name]}).add(value)
            found[name] = value

    if conflicts:
        detail = "; ".join(f"{n}={sorted(v)}" for n, v in sorted(conflicts.items()))
        raise ReceiptRejected(
            f"the same definition was compiled with different values across modules: {detail}")
    return found, len(candidates)


def enforce_policy(definitions: dict[str, str], configuration: str,
                   target: str = "") -> list[str]:
    """Apply the policy's rules. Returns the checks that passed."""
    checks: list[str] = []
    # Shipping is never excused, for any target: that is the whole point.
    harness_allowance = (target in TEST_HARNESS_TARGETS
                         and configuration != "Shipping")
    guarded_by_target = target in GUARDED_BY_DEFINITION_TARGETS
    guarded = guarded_by_target or configuration in GUARDED_CONFIGURATIONS
    if guarded_by_target:
        checks.append("guarded-by-target-definition")

    for name in GUARD_DEFINITIONS:
        present = definitions.get(name) == "1"
        if guarded and not present:
            raise ReceiptRejected(
                f"{configuration} is a guarded configuration but {name} is not 1 "
                f"(got {definitions.get(name)!r}); the build is not guarded")
        if not guarded and present:
            raise ReceiptRejected(
                f"{name} must not be set in {configuration} for target {target!r}")
    checks.append(f"guard-definitions-{'present' if guarded else 'absent'}")

    for name in TEST_ONLY_DEFINITIONS:
        present = definitions.get(name) == "1"
        if configuration == "Test" and not present:
            raise ReceiptRejected(
                f"Test must compile {name}, but it is {definitions.get(name)!r}")
        if configuration != "Test" and present and not harness_allowance:
            raise ReceiptRejected(
                f"{name} is test-only and must not appear in {configuration}"
                f" for target {target!r}")
    if harness_allowance:
        checks.append("test-harness-target-allowance")
        checks.append("test-only-definitions-permitted")
    else:
        checks.append(
            "test-only-definitions-present"
            if configuration == "Test"
            else "test-only-definitions-absent")

    checks.append(f"configuration-is-{configuration}")
    return sorted(checks)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Record and gate what a guarded build actually compiled.")
    parser.add_argument("--project", required=True, type=Path,
                        help="path to the .uproject")
    parser.add_argument("--target", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--engine-receipt", type=Path,
                        help="source/toolchain receipt to fold in")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)

    try:
        project_dir = args.project.parent if args.project.suffix else args.project
        definitions, module_count = collect_definitions(
            project_dir, args.target, args.platform, args.configuration)
        checks = enforce_policy(definitions, args.configuration, args.target)

        document: dict = {
            "format": RECEIPT_FORMAT,
            "target": args.target,
            "platform": args.platform,
            "configuration": args.configuration,
            "definitions": dict(sorted(definitions.items())),
            "modules_inspected": module_count,
            "checks": checks,
        }
        if args.engine_receipt and args.engine_receipt.is_file():
            try:
                engine = json.loads(args.engine_receipt.read_text())
            except (OSError, json.JSONDecodeError) as exc:
                raise ReceiptRejected(
                    f"cannot read engine receipt {args.engine_receipt}: {exc}") from exc
            # Carry the provenance answer forward so ONE file answers both
            # "which engine" and "what was compiled".
            document["engine"] = {
                key: engine[key]
                for key in ("commit", "tag", "engine", "toolchain", "checks")
                if key in engine
            }
        elif args.engine_receipt:
            raise ReceiptRejected(f"engine receipt {args.engine_receipt} does not exist")

        args.out.parent.mkdir(parents=True, exist_ok=True)
        # LF bytes explicitly: this receipt is hash-bound by the release
        # manifest and recomputed from canonical LF JSON by verifiers.
        with args.out.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(document, indent=2, sort_keys=True) + "\n")
    except ReceiptRejected as exc:
        print(f"BUILD RECEIPT REJECTED: {exc}", file=sys.stderr)
        return 1

    print(f"BUILD RECEIPT OK {args.out}: {args.target}/{args.platform}/"
          f"{args.configuration} [{', '.join(checks)}] "
          f"{len(definitions)} WOT_ definitions from {module_count} modules")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
