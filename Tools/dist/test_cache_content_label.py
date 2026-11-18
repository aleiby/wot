from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_content_label as content_label


GOLDEN_LABELS = {
    1: "1a7e19b367523c11d9ad6c7fda42a39472a68d4b8c52233b6b2d568ca3daa58e",
    2: "e6e3ef1fa015fcfdef584ffa59b5badff4993d1f3fab2fb35e7743ed522e968e",
    3: "089c6c7258ecb8c9499919ae0f211f0a6477189784a94c39df1ca677ec6cd6d6",
}


class CacheContentLabelTests(unittest.TestCase):
    def test_golden_labels(self) -> None:
        for revision, expected in GOLDEN_LABELS.items():
            with self.subTest(revision=revision):
                self.assertEqual(content_label.label(revision), expected)
        self.assertEqual(content_label.REVISION_1_LABEL, GOLDEN_LABELS[1])

    def test_domain_is_thirty_bytes_with_its_nul(self) -> None:
        self.assertEqual(len(content_label.DOMAIN), 30)
        self.assertEqual(content_label.DOMAIN[-1:], b"\x00")

    def test_compiled_revision_and_label_match_the_cpp_source(self) -> None:
        self.assertTrue(content_label.CACHE_BUILDER.is_file())
        self.assertEqual(content_label.compiled_revision(), 1)
        self.assertEqual(content_label.compiled_revision_1_label(),
                         content_label.REVISION_1_LABEL)

    def test_compiled_values_require_exactly_one_definition(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "CacheBuilder.cpp"
            source.write_text("// no definitions\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                content_label.compiled_revision(source)
            with self.assertRaises(ValueError):
                content_label.compiled_revision_1_label(source)
            source.write_text(
                "constexpr uint32 WoTCacheContentRevision = 2;\n"
                "constexpr uint32 WoTCacheContentRevision = 3;\n",
                encoding="utf-8")
            with self.assertRaises(ValueError):
                content_label.compiled_revision(source)
            source.write_text(
                "constexpr uint32 WoTCacheContentRevision = 0;\n",
                encoding="utf-8")
            with self.assertRaises(ValueError):
                content_label.compiled_revision(source)
            source.write_text(
                "constexpr uint32  WoTCacheContentRevision =\n\t4 ;\n"
                "constexpr TCHAR WoTCacheRevision1Label[] =\n"
                f"\tTEXT(\"{GOLDEN_LABELS[1]}\");\n",
                encoding="utf-8")
            self.assertEqual(content_label.compiled_revision(source), 4)
            self.assertEqual(content_label.compiled_revision_1_label(source),
                             GOLDEN_LABELS[1])

    def test_revision_for_label_round_trips(self) -> None:
        for revision in range(1, 6):
            with self.subTest(revision=revision):
                self.assertEqual(content_label.revision_for_label(
                    content_label.label(revision), 5), revision)
        self.assertIsNone(content_label.revision_for_label(
            content_label.label(6), 5))
        self.assertIsNone(content_label.revision_for_label("0" * 64, 5))
        self.assertIsNone(content_label.revision_for_label(
            GOLDEN_LABELS[1].upper(), 5))

    def test_invalid_revisions_raise(self) -> None:
        for revision in (0, -1, 2 ** 64, True, 1.0, "1", None):
            with self.subTest(revision=revision):
                with self.assertRaises(ValueError):
                    content_label.label(revision)  # type: ignore[arg-type]
        with self.assertRaises(ValueError):
            content_label.revision_for_label(GOLDEN_LABELS[1], 0)
        with self.assertRaises(ValueError):
            content_label.revision_for_label(None, 1)  # type: ignore[arg-type]


if __name__ == "__main__":
    unittest.main()
