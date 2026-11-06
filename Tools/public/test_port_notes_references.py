"""Validate public references to the curated port-notes entries."""

import re
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_FILES = REPO_ROOT / "Tools" / "public" / "public-files.txt"
MARKER = "PORT" + "_NOTES"

TITLE_REFERENCE_RE = re.compile(
    re.escape(MARKER) + r'(?:\.md)?[ \t]+"([^"\r\n]+)"'
)
MARKER_RE = re.compile(re.escape(MARKER))
FORBIDDEN_AFTER_REFERENCE = (
    ("section marker", re.compile(r"(?:Â§|§)")),
    ("slice number", re.compile(r"\bslice\s+\d+", re.IGNORECASE)),
    ("task number", re.compile(r"\bTask\s+\d+", re.IGNORECASE)),
    ("ledger ID", re.compile(r"\b[A-Z]{1,2}\d+\b")),
    ("finding number", re.compile(r"\bfinding\s+#\d+", re.IGNORECASE)),
    ("date", re.compile(r"\b20\d\d-\d\d-\d\d\b")),
    ("version", re.compile(r"\bv\d+\.\d+\b", re.IGNORECASE)),
    ("line number", re.compile(r"\.md:\d+\b")),
)


def read_titles(path):
    """Return the exact third-level headings in the curated notes."""
    return {
        line[4:]
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.startswith("### ")
    }


def decode_text(path):
    """Decode a UTF-8 public text file, returning None for binary files."""
    data = path.read_bytes()
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return None


def scan_text(display_path, text, titles):
    """Return human-readable reference errors for one decoded file."""
    errors = []
    private_name = MARKER + ".private"
    for line_number, line in enumerate(text.splitlines(), 1):
        location = "{}:{}".format(display_path, line_number)

        if private_name in line:
            errors.append("{}: mentions the private notes".format(location))

        for match in TITLE_REFERENCE_RE.finditer(line):
            title = match.group(1)
            if title not in titles:
                errors.append(
                    '{}: unknown port-notes title "{}"'.format(location, title)
                )

        for marker in MARKER_RE.finditer(line):
            suffix = line[marker.end():]
            for label, pattern in FORBIDDEN_AFTER_REFERENCE:
                match = pattern.search(suffix)
                if match is not None:
                    errors.append(
                        "{}: forbidden {} after port-notes reference: {}".format(
                            location, label, match.group(0)
                        )
                    )
    return errors


def scan_file(path, display_path, titles):
    """Scan one file, skipping missing and binary inputs."""
    if not path.is_file():
        return []
    text = decode_text(path)
    if text is None:
        return []
    return scan_text(display_path, text, titles)


def public_source_paths(root, list_path):
    """Yield listed files under the public source/tool/config roots."""
    for raw_line in list_path.read_text(encoding="utf-8").splitlines():
        relative = raw_line.strip()
        if relative.startswith(("Source/", "Tools/", "Config/")):
            yield relative, root / Path(relative)


class PortNotesScannerTests(unittest.TestCase):
    TITLES = {"Known title"}
    TEMP_PARENT = Path(__file__).resolve().parent

    def scan_temporary(self, text):
        with tempfile.NamedTemporaryFile(
            dir=str(self.TEMP_PARENT), delete=False
        ) as temporary:
            path = Path(temporary.name)
            temporary.write(text.encode("utf-8"))
        try:
            return scan_file(path, "sample.txt", self.TITLES)
        finally:
            path.unlink()

    def test_accepts_exact_title_references(self):
        text = 'See {} "Known title" and {}.md "Known title".\n'.format(
            MARKER, MARKER
        )
        self.assertEqual([], self.scan_temporary(text))

    def test_rejects_unknown_title(self):
        errors = self.scan_temporary(
            'See {} "Old working-note heading".\n'.format(MARKER)
        )
        self.assertEqual(1, len(errors))
        self.assertIn("sample.txt:1", errors[0])
        self.assertIn("unknown port-notes title", errors[0])

    def test_rejects_each_forbidden_internal_form(self):
        forms = (
            "§8",
            "slice 3",
            "Task 5-4",
            "G35",
            "C02",
            "P04",
            "N7",
            "finding #3",
            "2026-07-18",
            "v2.6",
            ".md:3728",
        )
        for form in forms:
            with self.subTest(form=form):
                errors = self.scan_temporary("{} {}\n".format(MARKER, form))
                self.assertTrue(errors, form)
                self.assertTrue(all("sample.txt:1" in error for error in errors))

    def test_rejects_private_notes_name(self):
        errors = self.scan_temporary(MARKER + ".private.md\n")
        self.assertEqual(1, len(errors))
        self.assertIn("mentions the private notes", errors[0])

    def test_skips_binary_files(self):
        with tempfile.NamedTemporaryFile(
            dir=str(self.TEMP_PARENT), delete=False
        ) as temporary:
            path = Path(temporary.name)
            temporary.write(b"\0" + MARKER.encode("ascii") + b" G35")
        try:
            self.assertEqual([], scan_file(path, "sample.bin", self.TITLES))
        finally:
            path.unlink()


class PublicTreePortNotesTests(unittest.TestCase):
    def test_public_tree_references(self):
        titles = read_titles(REPO_ROOT / (MARKER + ".md"))
        errors = []
        for display_path, path in public_source_paths(REPO_ROOT, PUBLIC_FILES):
            errors.extend(scan_file(path, display_path, titles))
        self.assertEqual([], errors, "\n" + "\n".join(errors))


if __name__ == "__main__":
    unittest.main()
