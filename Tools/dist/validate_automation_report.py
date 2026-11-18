#!/usr/bin/env python3
"""Accept or reject a UE 5.8 Automation `index.json` report against a filter.

Editor exit zero is not evidence: `-ExecCmds "Automation RunTest <filter>;Quit"`
exits zero when the filter selects nothing, when a test is skipped, and when the
run never started.  This tool is the only acceptance mechanism the bootstrap plan
uses.  It fails closed: every requested `+`-separated filter prefix must have
selected at least one test, every reported test must belong to the requested
set, every selected test must have completed in `Success`, the aggregate
`failed`/`notRun`/`inProcess` counters must all be zero, and the counters must
add up to the reported test list.

Schema reference (UE 5.8 `FAutomatedTestPassResults` serialized by
FJsonObjectConverter, so keys are lowerCamel):
  succeeded, succeededWithWarnings, failed, notRun, inProcess, tests[]
  tests[].fullTestPath, tests[].state in
    {NotRun, InProcess, Fail, Success, Skipped}   (EAutomationState)

Stdlib only.  Usage:
  validate_automation_report.py --report <index.json> --filter <expression>
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# EAutomationState: the ONLY state that counts as a completed, passing test.
SUCCESS_STATE = "Success"
COUNTER_KEYS = ("succeeded", "succeededWithWarnings", "failed", "notRun",
                "inProcess")
FORBIDDEN_COUNTER_KEYS = ("failed", "notRun", "inProcess")


class ReportRejected(Exception):
    """The report does not prove the requested filter ran and passed."""


def parse_filter(expression: str) -> tuple[str, ...]:
    """Split an `Automation RunTest` expression into exact filter rules.

    UE's legacy bare arguments are substring filters, so ``WoT.Bootstrap``
    also selects ``WoT.BootstrapCore``.  ``StartsWith:`` is the engine's
    public descendant-prefix syntax: UE preserves the filter text and appends
    one dot only when it has no trailing dot. Preserve the explicit rule kind
    and that exact dotted value so report acceptance matches the editor run.
    """
    if not isinstance(expression, str) or not expression.strip():
        raise ReportRejected("empty filter expression")
    prefixes = []
    for part in expression.split("+"):
        prefix = part.strip()
        marker = "StartsWith:"
        if prefix.startswith(marker):
            value = prefix[len(marker):].strip()
            if not value:
                prefix = ""
            else:
                if not value.endswith("."):
                    value += "."
                prefix = marker + value
        prefixes.append(prefix)
    prefixes = tuple(prefixes)
    if any(not prefix for prefix in prefixes):
        raise ReportRejected(f"filter has an empty prefix: {expression!r}")
    return prefixes


def matches_prefix(full_test_path: str, prefix: str) -> bool:
    """UE filter semantics: the exact path, or a dot-delimited descendant.

    `WoT.BootstrapCore.CanonicalJsonExtra` must NOT match the prefix
    `WoT.BootstrapCore.CanonicalJson` — only `...CanonicalJson.Accept` does.
    """
    marker = "StartsWith:"
    if prefix.startswith(marker):
        return full_test_path.startswith(prefix[len(marker):])
    return (full_test_path == prefix
            or full_test_path.startswith(prefix + "."))


def load_report(path: Path) -> dict:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise ReportRejected(f"cannot read report {path}: {exc}") from exc
    try:
        # UE 5.8 writes index.json as UTF-8 WITH a BOM, so utf-8-sig is required;
        # plain utf-8 fails on the very first character of a real report.
        document = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ReportRejected(f"malformed report {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise ReportRejected(f"report {path} is not a JSON object")
    return document


def validate_report(document: dict, expression: str) -> dict:
    """Return an accepted-run summary, or raise ReportRejected."""
    prefixes = parse_filter(expression)

    tests = document.get("tests")
    if not isinstance(tests, list):
        raise ReportRejected("report has no `tests` array")
    if not tests:
        raise ReportRejected("report selected zero tests")

    counters = {}
    for key in COUNTER_KEYS:
        value = document.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise ReportRejected(f"report counter `{key}` is not a "
                                 f"non-negative integer: {value!r}")
        counters[key] = value

    for key in FORBIDDEN_COUNTER_KEYS:
        if counters[key]:
            raise ReportRejected(f"report counter `{key}` is {counters[key]}, "
                                 "expected 0")

    total = sum(counters.values())
    if total != len(tests):
        raise ReportRejected(
            f"counters total {total} but the report lists {len(tests)} tests")
    if counters["succeeded"] + counters["succeededWithWarnings"] == 0:
        raise ReportRejected("aggregate reports zero succeeded tests")

    selected: dict[str, list[str]] = {prefix: [] for prefix in prefixes}
    for index, test in enumerate(tests):
        if not isinstance(test, dict):
            raise ReportRejected(f"tests[{index}] is not an object")
        path = test.get("fullTestPath")
        if not isinstance(path, str) or not path:
            raise ReportRejected(f"tests[{index}] has no `fullTestPath`")
        state = test.get("state")
        if state != SUCCESS_STATE:
            raise ReportRejected(
                f"{path} state is {state!r}, expected {SUCCESS_STATE!r}")
        if "errors" in test:
            errors = test["errors"]
            if (not isinstance(errors, int) or isinstance(errors, bool)
                    or errors < 0):
                raise ReportRejected(
                    f"{path} errors is not a non-negative integer: {errors!r}")
        else:
            errors = 0
        if errors != 0:
            raise ReportRejected(
                f"{path} reports {errors} errors but state {SUCCESS_STATE!r}")
        owners = [prefix for prefix in prefixes if matches_prefix(path, prefix)]
        if not owners:
            raise ReportRejected(
                f"{path} is outside the requested filter {expression!r}")
        for prefix in owners:
            selected[prefix].append(path)

    unmatched = [prefix for prefix in prefixes if not selected[prefix]]
    if unmatched:
        raise ReportRejected(
            "filter prefixes selected no test: " + ", ".join(unmatched))

    return {
        "filter": expression,
        "prefixes": list(prefixes),
        "tests": len(tests),
        "succeeded": counters["succeeded"],
        "succeeded_with_warnings": counters["succeededWithWarnings"],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Accept a UE Automation index.json for an exact filter.")
    parser.add_argument("--report", required=True, type=Path,
                        help="path to the exported index.json")
    parser.add_argument("--filter", required=True,
                        help="the identical `+`-separated filter expression "
                             "passed to Automation RunTest")
    args = parser.parse_args(argv)
    try:
        summary = validate_report(load_report(args.report), args.filter)
    except ReportRejected as exc:
        print(f"REJECTED {args.report}: {exc}", file=sys.stderr)
        return 1
    print(f"ACCEPTED {args.report}: {summary['tests']} tests passed for "
          f"{summary['filter']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
