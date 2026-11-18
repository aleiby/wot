from __future__ import annotations

import unittest
import tempfile
from pathlib import Path

from Tools.dist.check_shipping_consumer_allowlist import check


ALLOWLIST = [
    {
        "path": r"Source/WoT/Tests/.*",
        "symbol": r"DA_WoTHUDFonts",
        "justification": "Editor-only oracle fixture.",
    }
]


class ShippingConsumerAllowlistTests(unittest.TestCase):
    def test_allowlisted_editor_oracle_passes(self) -> None:
        self.assertEqual(
            [],
            check(
                "Source/WoT/Tests/FontOracle.cpp:12:DA_WoTHUDFonts\n",
                ALLOWLIST,
            ),
        )

    def test_runtime_legacy_consumer_fails(self) -> None:
        failures = check(
            "Source/WoT/WoTHUD.cpp:9:DA_WoTHUDFonts\n",
            ALLOWLIST,
        )
        self.assertTrue(any("Shipping legacy consumer" in item for item in failures))

    def test_stale_exception_fails(self) -> None:
        self.assertTrue(check("", ALLOWLIST))

    def test_comment_only_provenance_is_not_a_consumer(self) -> None:
        self.assertEqual(
            [],
            check(
                "Source/WoT/WoTAnimData.h:8:// DT_WoTAnimSequences provenance\n",
                [],
            ),
        )

    def test_nonshipping_guarded_oracle_is_not_a_shipping_consumer(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Source/WoT/WoTHUD.cpp"
            source.parent.mkdir(parents=True)
            source.write_text(
                "#if !WOT_GUARDED_BOOTSTRAP\n"
                "auto Font = DA_WoTHUDFonts;\n"
                "#endif\n",
                encoding="utf-8",
            )
            self.assertEqual(
                [],
                check(
                    "Source/WoT/WoTHUD.cpp:2:auto Font = DA_WoTHUDFonts;\n",
                    [],
                    source_root=root,
                ),
            )

    def test_exception_must_be_unique(self) -> None:
        failures = check(
            "Source/WoT/Tests/FontOracle.cpp:12:DA_WoTHUDFonts\n",
            ALLOWLIST + ALLOWLIST,
        )
        self.assertTrue(any("not uniquely" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
