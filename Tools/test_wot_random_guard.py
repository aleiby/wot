#!/usr/bin/env python3
"""Prevent gameplay code from bypassing the platform-independent WoT RNG."""
from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT / "Source" / "WoT"
TEST_ROOT = SOURCE_ROOT / "Tests"
FMATH_RANDOM = re.compile(
    r"\bFMath::(?:RandInit|SRandInit|Rand|RandRange|RandHelper|RandBool|"
    r"FRand|FRandRange|VRand|VRandCone)\b"
)

# Keep exceptions explicit and local. The production allowlist is intentionally empty.
ALLOWLIST: set[str] = set()


class WoTRandomSourceGuardTests(unittest.TestCase):
    def test_gameplay_code_uses_wot_random(self):
        violations: list[str] = []
        for path in sorted(SOURCE_ROOT.rglob("*")):
            if path.suffix not in {".cpp", ".h"} or TEST_ROOT in path.parents:
                continue
            relative = path.relative_to(ROOT).as_posix()
            if relative in ALLOWLIST:
                continue
            for line_number, line in enumerate(
                    path.read_text(encoding="utf-8").splitlines(), start=1):
                if FMATH_RANDOM.search(line):
                    violations.append(f"{relative}:{line_number}: {line.strip()}")

        self.assertEqual(
            violations,
            [],
            "Gameplay randomness must use WoTRandom:\n" + "\n".join(violations),
        )


if __name__ == "__main__":
    unittest.main()
