#!/usr/bin/env python3
"""Stdlib specs for the only Automation-report acceptance mechanism.

Every document here is synthetic and shaped like a UE 5.8 `index.json`.  The
suite exists because editor exit codes lie: each rejection case below is a way a
run can "succeed" while proving nothing.
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import validate_automation_report as validator

FILTER = "WoT.BootstrapCore.CanonicalJson"


def _test(path, state="Success", errors=0):
    return {
        "testDisplayName": path.rsplit(".", 1)[-1],
        "fullTestPath": path,
        "state": state,
        "deviceInstance": ["Mac"],
        "duration": 0.01,
        "warnings": 0,
        "errors": errors,
        "entries": [],
    }


def _report(tests, **overrides):
    states = [test["state"] for test in tests]
    document = {
        "devices": [{"deviceName": "mac", "platform": "Mac"}],
        "reportCreatedOn": "2026.07.25-00.00.00",
        "succeeded": states.count("Success"),
        "succeededWithWarnings": 0,
        "failed": states.count("Fail"),
        "notRun": states.count("NotRun") + states.count("Skipped"),
        "inProcess": states.count("InProcess"),
        "totalDuration": 0.1,
        "comparisonExported": False,
        "comparisonExportDirectory": "",
        "tests": tests,
    }
    document.update(overrides)
    return document


class FilterSemanticsTests(unittest.TestCase):
    def test_prefixes_are_split_and_must_be_non_empty(self):
        self.assertEqual(validator.parse_filter("A.B+C.D"), ("A.B", "C.D"))
        for bad in ("", "   ", "A.B+", "+A.B", "A.B++C.D"):
            with self.subTest(expression=bad), \
                    self.assertRaises(validator.ReportRejected):
                validator.parse_filter(bad)

    def test_a_prefix_matches_only_itself_or_a_dot_delimited_descendant(self):
        self.assertTrue(validator.matches_prefix(FILTER, FILTER))
        self.assertTrue(validator.matches_prefix(FILTER + ".Accept", FILTER))
        self.assertFalse(validator.matches_prefix(FILTER + "Extra", FILTER))
        self.assertFalse(validator.matches_prefix("WoT.BootstrapCore", FILTER))

    def test_ue_startswith_filter_is_normalized_to_dot_delimited_semantics(self):
        expression = "StartsWith:" + FILTER
        normalized = "StartsWith:" + FILTER + "."
        self.assertEqual(validator.parse_filter(expression), (normalized,))
        summary = validator.validate_report(
            _report([_test(FILTER + ".Accept")]), expression)
        self.assertEqual(summary["prefixes"], [normalized])
        with self.assertRaisesRegex(
            validator.ReportRejected, "outside the requested filter"
        ):
            validator.validate_report(
                _report([_test(FILTER + "Extra")]), expression)

    def test_ue_startswith_filter_never_selects_an_exact_leaf(self):
        expression = "StartsWith:" + FILTER
        with self.assertRaisesRegex(
            validator.ReportRejected, "outside the requested filter"
        ):
            validator.validate_report(_report([_test(FILTER)]), expression)

    def test_ue_startswith_filter_preserves_a_doubled_trailing_dot(self):
        expression = "StartsWith:" + FILTER + ".."
        self.assertEqual(validator.parse_filter(expression), (expression,))
        with self.assertRaisesRegex(
            validator.ReportRejected, "outside the requested filter"
        ):
            validator.validate_report(
                _report([_test(FILTER + ".Accept")]), expression)

    def test_ue_startswith_filter_requires_a_nonempty_value(self):
        with self.assertRaisesRegex(
            validator.ReportRejected, "empty prefix"
        ):
            validator.parse_filter("StartsWith:")


class AcceptanceTests(unittest.TestCase):
    def test_a_complete_passing_run_is_accepted(self):
        summary = validator.validate_report(
            _report([_test(FILTER + ".Accept"), _test(FILTER + ".Reject")]),
            FILTER)
        self.assertEqual(summary["tests"], 2)
        self.assertEqual(summary["prefixes"], [FILTER])

    def test_every_requested_prefix_must_select_a_test(self):
        document = _report([_test(FILTER + ".Accept")])
        validator.validate_report(document, FILTER)  # must not raise
        with self.assertRaisesRegex(validator.ReportRejected,
                                    "selected no test"):
            validator.validate_report(document, FILTER + "+WoT.Other")

    def test_success_with_warnings_still_counts_as_completed(self):
        tests = [_test(FILTER + ".Accept")]
        document = _report(tests, succeeded=0, succeededWithWarnings=1)
        self.assertEqual(validator.validate_report(document, FILTER)["tests"], 1)


class RejectionTests(unittest.TestCase):
    def _reject(self, document, pattern):
        with self.assertRaisesRegex(validator.ReportRejected, pattern):
            validator.validate_report(document, FILTER)

    def test_empty_test_list_and_missing_array_are_rejected(self):
        self._reject(_report([]), "zero tests")
        document = _report([_test(FILTER + ".Accept")])
        del document["tests"]
        self._reject(document, "no `tests` array")

    def test_a_zero_success_aggregate_is_rejected(self):
        self._reject(_report([_test(FILTER + ".Accept")], succeeded=0),
                     "counters total")
        self._reject(
            _report([_test(FILTER + ".Accept")], succeeded=0,
                    succeededWithWarnings=0, notRun=1),
            "`notRun` is 1")

    def test_a_test_outside_the_requested_filter_is_rejected(self):
        self._reject(
            _report([_test(FILTER + ".Accept"), _test("WoT.Unrelated.Spec")]),
            "outside the requested filter")
        self._reject(_report([_test(FILTER + "Extra")]),
                     "outside the requested filter")

    def test_failed_not_run_in_process_and_skipped_states_are_rejected(self):
        """Both the honest and the lying aggregate must be rejected.

        An honest report trips the forbidden aggregate counter.  A report whose
        aggregate claims a clean pass while a test record says otherwise is the
        case only the per-test state check can catch.
        """
        for state in ("Fail", "NotRun", "InProcess", "Skipped"):
            with self.subTest(state=state, aggregate="honest"):
                self._reject(_report([_test(FILTER + ".Accept", state=state)]),
                             "expected 0")
            with self.subTest(state=state, aggregate="lying"):
                self._reject(
                    _report([_test(FILTER + ".Accept", state=state)],
                            succeeded=1, failed=0, notRun=0, inProcess=0),
                    "expected 'Success'")

    def test_forbidden_aggregate_counters_are_rejected(self):
        for key in ("failed", "notRun", "inProcess"):
            with self.subTest(counter=key):
                document = _report([_test(FILTER + ".Accept")])
                document[key] = 1
                self._reject(document, f"`{key}` is 1")

    def test_inconsistent_counters_are_rejected(self):
        document = _report([_test(FILTER + ".Accept")], succeeded=2)
        self._reject(document, "counters total 2")
        document = _report([_test(FILTER + ".Accept")], succeeded="one")
        self._reject(document, "not a non-negative integer")

    def test_a_success_state_carrying_errors_is_rejected(self):
        self._reject(_report([_test(FILTER + ".Accept", errors=3)]),
                     "reports 3 errors")

    def test_errors_must_be_a_non_negative_non_boolean_integer(self):
        for errors in ("3", -1, True, 1.5, None):
            with self.subTest(errors=errors):
                self._reject(_report([_test(FILTER + ".Accept", errors=errors)]),
                             "not a non-negative integer")

    def test_a_malformed_test_record_is_rejected(self):
        document = _report([_test(FILTER + ".Accept")])
        document["tests"] = ["not-an-object"]
        self._reject(document, "not an object")
        document = _report([_test(FILTER + ".Accept")])
        del document["tests"][0]["fullTestPath"]
        self._reject(document, "no `fullTestPath`")


class LoaderTests(unittest.TestCase):
    def test_missing_and_malformed_report_files_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(validator.ReportRejected,
                                        "cannot read report"):
                validator.load_report(root / "absent.json")
            broken = root / "index.json"
            broken.write_bytes(b"{not json")
            with self.assertRaisesRegex(validator.ReportRejected, "malformed"):
                validator.load_report(broken)
            broken.write_bytes(b"[]")
            with self.assertRaisesRegex(validator.ReportRejected,
                                        "not a JSON object"):
                validator.load_report(broken)

    def test_a_real_utf8_bom_report_is_readable(self):
        """UE 5.8 exports index.json with a BOM; plain utf-8 would reject it."""
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / "index.json"
            document = _report([_test(FILTER + ".Accept")])
            report.write_bytes(b"\xef\xbb\xbf" + json.dumps(document).encode("utf-8"))
            self.assertEqual(
                validator.validate_report(
                    validator.load_report(report), FILTER)["tests"], 1)

    def test_main_exit_codes_gate_on_the_same_rules(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / "index.json"
            passing = _report([_test(FILTER + ".Accept")])
            report.write_text(json.dumps(passing), encoding="utf-8")
            self.assertEqual(
                validator.main(["--report", str(report), "--filter", FILTER]), 0)
            failing = deepcopy(passing)
            failing["tests"][0]["state"] = "Fail"
            failing["succeeded"], failing["failed"] = 0, 1
            report.write_text(json.dumps(failing), encoding="utf-8")
            self.assertEqual(
                validator.main(["--report", str(report), "--filter", FILTER]), 1)


if __name__ == "__main__":
    unittest.main()
