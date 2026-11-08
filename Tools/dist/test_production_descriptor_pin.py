"""The compiled production-descriptor pin must name the committed descriptor.

CacheBuilder.cpp pins the SHA-256 of Build/WoTRelease/cache-v3-production-
descriptor.json; the cache builder refuses a descriptor that doesn't match, and
build_stripped_pak refuses to package one. A merge can take one file from each
side (it did: #45 landed a regenerated descriptor with #47's pin), and the
closure test only checks the descriptor against the sources, so this compares
the two committed files directly.
"""
from __future__ import annotations

import hashlib
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DESCRIPTOR = REPO / "Build/WoTRelease/cache-v3-production-descriptor.json"
PIN_SOURCE = REPO / "Source/WoTRetailReader/Private/CacheBuilder.cpp"
PIN_RE = re.compile(
    r"WoTCacheExpectedProductionDescriptorSha256\[\]\s*=\s*TEXT\(\"([0-9a-f]{64})\"\)")


class ProductionDescriptorPinTests(unittest.TestCase):
    def test_compiled_pin_matches_the_committed_descriptor(self):
        match = PIN_RE.search(PIN_SOURCE.read_text(encoding="utf-8"))
        self.assertIsNotNone(match, f"no production descriptor pin in {PIN_SOURCE}")
        actual = hashlib.sha256(DESCRIPTOR.read_bytes()).hexdigest()
        self.assertEqual(
            match.group(1), actual,
            "CacheBuilder.cpp pins a different production descriptor than the "
            "committed one; re-pin WoTCacheExpectedProductionDescriptorSha256 to "
            f"{actual} (or regenerate the descriptor if the sources changed)")


if __name__ == "__main__":
    unittest.main()
