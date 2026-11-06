"""Tests for the provenance scanner. Every fixture is synthetic: no retail or
development-tree text appears here."""
from __future__ import annotations

import json
import os
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import provenance_scan as scan  # noqa: E402


def synthetic_function(name: str, count: int = 12) -> str:
    """A function body with distinct identifiers so random overlap is impossible."""
    lines = ["int %s(int seed)" % name, "{"]
    for index in range(count):
        lines.append("    int %s_v%d = seed * %d + %s_w%d;" % (name, index, index + 3, name, index))
    lines.append("    return %s_v0;" % name)
    lines.append("}")
    return "\n".join(lines)


class SplitTests(unittest.TestCase):
    def test_c_split_blanks_comments_and_keeps_literals(self):
        text = 'int a = 1; // trailing note\n/* block\nspan */ const char* s = "a /* not */ b";\n'
        code, comments, literals = scan.split_c(text)
        self.assertEqual(code.count("\n"), text.count("\n"))
        self.assertNotIn("trailing", code)
        self.assertNotIn("block", code)
        self.assertIn('"a /* not */ b"', code)
        self.assertEqual([c[0] for c in comments], [1, 2])
        self.assertEqual(comments[1][1], " block\nspan ")
        self.assertEqual(literals, [(3, "a /* not */ b")])

    def test_py_split_handles_hash_in_strings_and_docstrings(self):
        text = 'x = "# not a comment"  # real\n"""doc\nstring"""\n'
        code, comments, literals = scan.split_py(text)
        self.assertIn('"# not a comment"', code)
        self.assertEqual(comments, [(1, " real")])
        self.assertEqual(literals, [(1, "# not a comment"), (2, "doc\nstring")])

    def test_markdown_fences_become_code(self):
        text = "prose one\n```uc\nfunction Foo()\n```\nprose two\n~~~\nx = 1\n~~~\n"
        code_lines, prose_lines = scan.split_markdown(text)
        self.assertEqual(code_lines, [(3, "function Foo()"), (7, "x = 1")])
        self.assertEqual([line for _, line in prose_lines], ["prose one", "prose two", ""])

    def test_code_stream_line_numbers(self):
        stream = scan.code_stream("a = 1;\n\nb += c;")
        self.assertEqual(list(stream.texts), ["a", "=", "1", ";", "b", "+=", "c", ";"])
        self.assertEqual(list(stream.lines), [1, 1, 1, 1, 3, 3, 3, 3])

    def test_word_stream_multiline_chunks_and_tags(self):
        stream = scan.word_stream([(5, "First <b>line</b>\nsecond's line")])
        self.assertEqual(list(stream.texts), ["first", "line", "second's", "line"])
        self.assertEqual(list(stream.lines), [5, 5, 6, 6])

    def test_int_values(self):
        values = scan.int_values('[sec]\nKey="Quoted value"\nOther=plain text\n; comment\n')
        self.assertEqual(values, [(2, "Quoted value"), (3, "plain text")])

    def test_file_kind(self):
        self.assertEqual(scan.file_kind("Source/A/B.cpp"), scan.KIND_C)
        self.assertEqual(scan.file_kind("Tools/x.py"), scan.KIND_PY)
        self.assertEqual(scan.file_kind("PORT_NOTES.md"), scan.KIND_MD)
        self.assertEqual(scan.file_kind("LICENSE"), scan.KIND_MD)
        self.assertEqual(scan.file_kind("Config/DefaultEngine.ini"), scan.KIND_PLAIN)
        self.assertIsNone(scan.file_kind("Tools/fixtures/x.bin"))


class ShingleTests(unittest.TestCase):
    def test_rolling_hash_matches_direct_hash(self):
        ids = [5, 9, 2, 7, 7, 3, 1]
        n = 3
        hashes = scan.shingle_hashes(ids, n)
        self.assertEqual(len(hashes), len(ids) - n + 1)
        for index, value in enumerate(hashes):
            direct = 0
            for token in ids[index:index + n]:
                direct = (direct * scan._BASE + token) % scan._MOD
            self.assertEqual(value, direct)

    def test_winnow_selects_window_minimum(self):
        hashes = [50, 20, 30, 10, 40, 60, 5]
        selected = scan.winnow(hashes, 3)
        self.assertEqual(selected, [(1, 20), (3, 10), (6, 5)])
        self.assertEqual(scan.winnow([7, 8], 1), [(0, 7), (1, 8)])
        self.assertEqual(scan.winnow([9], 4), [(0, 9)])
        self.assertEqual(scan.winnow([], 4), [])

    def test_detection_guarantee_on_random_streams(self):
        rng = random.Random(1234)
        n, w = 10, 6
        guaranteed = n + w - 1
        for trial in range(20):
            reference_tokens = ["r%d_%d" % (trial, rng.randrange(10 ** 6)) for _ in range(300)]
            shared = ["s%d_%d" % (trial, i) for i in range(guaranteed)]
            insert_at = rng.randrange(0, 280)
            reference_tokens[insert_at:insert_at] = shared
            candidate_tokens = ["c%d_%d" % (trial, rng.randrange(10 ** 6)) for _ in range(300)]
            at = rng.randrange(0, 280)
            candidate_tokens[at:at] = shared
            vocab = scan.Vocabulary()
            index = scan.ShingleIndex(n, w)
            index.add(0, vocab.ids(reference_tokens))
            corpus = scan.Corpus()
            reference_stream = scan.Stream()
            for token in reference_tokens:
                reference_stream.add(token, 1)
            corpus.paths.append(Path("x"))
            corpus.labels.append("x")
            corpus._cache[0] = scan.Extracted(reference_stream, scan.Stream(), [], [])
            candidate_stream = scan.Stream()
            for token in candidate_tokens:
                candidate_stream.add(token, 1)
            spans = scan.match_stream(candidate_stream, vocab, index, corpus, "code", guaranteed)
            self.assertEqual(len(spans), 1, "trial %d" % trial)
            self.assertEqual((spans[0].start, spans[0].length), (at, guaranteed))
            self.assertEqual(spans[0].ref_start, insert_at)

    def test_richness_ignores_keywords_and_short_names(self):
        self.assertEqual(scan.richness(["for", "int", "i", "Alpha", "Alpha", "beta_1", "x"]), 2)

    def test_span_kind(self):
        self.assertEqual(scan.span_kind(["#", "include", "<", "sys", "/", "socket", ".", "h", ">",
                                         "#", "include", "<", "netinet", "/", "in", ".", "h", ">"]),
                         "include")
        self.assertEqual(scan.span_kind(["0x1", ",", "0x2", ",", "0x3", ",", "0x4", ",", "0x5", ","]),
                         "table")
        self.assertEqual(scan.span_kind(['"a"', ",", '"b"', ",", '"c"', ",", "{", "1", "}"]), "table")
        self.assertEqual(scan.span_kind(["Odds", "=", "1", "-", "(", "Dist", "-", "Near", ")", "/",
                                         "Span", ";"]), "code")


class FixtureScanTests(unittest.TestCase):
    """End-to-end over a synthetic reference root and candidate tree."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.reference = self.root / "reference"
        self.candidate = self.root / "candidate"
        self.pasted = synthetic_function("Legend_Pasted")
        self.other = synthetic_function("Legend_Other")
        comment = ("// The warder waits for the channeler to finish the weave before the "
                   "hound pack is released into the citadel courtyard at dusk\n")
        self._write(self.reference / "devtree" / "Game" / "Src" / "Thing.cpp",
                    comment + self.pasted + "\n\n" + self.other + "\n")
        self._write(self.reference / "exports" / "script" / "Pkg" / "Thing.uc",
                    'function Speak()\n{\n    Say("the quick brown fox jumps over the lazy dog");\n}\n')
        self._write(self.reference / "original" / "System" / "Game.int",
                    "[menu]\nTitle=A long retail sentence that nobody should compile in\n")

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def _reference(self, **overrides) -> scan.Reference:
        options = scan.Options(**overrides)
        reference, stats = scan.build_reference(self.reference, options)
        self.assertGreaterEqual(stats["files"], 3)
        return reference

    def _scan(self, label: str, text: str, **overrides):
        options = scan.Options(**overrides)
        reference = self._reference(**overrides)
        return scan.scan_file(label, text, reference, options)

    def test_verbatim_code_is_found_and_a_rewrite_is_not(self):
        rewritten = "\n".join(
            "int Port_v%d = seed * %d + Port_w%d;" % (i, i + 3, i) for i in range(12))
        text = "// our own header\n" + rewritten + "\n\n" + self.pasted.replace("Legend_Pasted(", "Ported(") + "\n"
        result = self._scan("Source/WoT/Thing.cpp", text)
        self.assertEqual(len(result["code"]), 1, result["code"])
        finding = result["code"][0]
        self.assertEqual(finding["ref"], "devtree/Game/Src/Thing.cpp")
        self.assertGreaterEqual(finding["tokens"], 100)
        # The renamed function name is not part of the run: the match starts at
        # the parameter list on the first pasted line (15) and ends at the brace.
        self.assertEqual(finding["start_line"], 15)
        self.assertEqual(finding["end_line"], 30)
        self.assertGreater(finding["rich"], 10)
        self.assertIn("Legend_Pasted_v0", finding["snippet"])
        self.assertIn("Legend_Pasted_v0", finding["ref_snippet"])
        self.assertEqual(result["covered"], finding["tokens"])

    def test_pasted_comment_and_retail_text_are_prose_findings(self):
        text = ("/* The warder waits for the channeler to finish the weave before the hound\n"
                "   pack is released into the citadel courtyard at dusk */\n"
                "int x = 1;\n"
                "// A long retail sentence that nobody should compile in\n")
        result = self._scan("Source/WoT/Prose.cpp", text, min_words=8)
        refs = sorted(finding["ref"] for finding in result["prose"])
        self.assertEqual(refs, ["devtree/Game/Src/Thing.cpp", "original/System/Game.int"])
        first = [f for f in result["prose"] if f["ref"].startswith("devtree")][0]
        self.assertEqual((first["start_line"], first["end_line"]), (1, 2))
        self.assertGreaterEqual(first["words"], 20)

    def test_literal_pass_matches_retail_value_and_script_string(self):
        text = ('const TCHAR* A = TEXT("A long retail sentence that nobody should compile in");\n'
                'const char* B = "The quick brown FOX jumps over the lazy dog";\n'
                'const char* C = "short one";\n')
        result = self._scan("Source/WoT/Strings.cpp", text)
        found = {(f["ref"], f["line"]) for f in result["literals"]}
        self.assertEqual(found, {("original/System/Game.int", 1), ("exports/script/Pkg/Thing.uc", 2)})

    def test_literal_pass_ignores_dotted_identifiers(self):
        self._write(self.reference / "exports" / "script" / "Pkg" / "Names.uc",
                    'Texture=Texture\'pkg.group.name_a01\'\nvar string S; S = "pkg.group.name_a01";\n')
        result = self._scan("Source/WoT/Names.cpp", 'const char* N = "pkg.group.name_a01";\n')
        self.assertEqual(result["literals"], [])

    def test_markdown_fenced_paste_is_a_code_finding(self):
        text = "# Notes\n\nSome prose.\n\n```cpp\n" + self.other + "\n```\n"
        result = self._scan("PORT_NOTES.md", text)
        self.assertEqual(len(result["code"]), 1)
        self.assertEqual(result["code"][0]["start_line"], 6)
        self.assertEqual(result["code"][0]["end_line"], 6 + 15)
        self.assertEqual(result["prose"], [])

    def test_unrelated_file_has_no_findings(self):
        result = self._scan("Tools/x.py", "import os\n\nprint('hello')\n")
        self.assertEqual((result["code"], result["prose"], result["literals"]), ([], [], []))
        self.assertEqual(result["tokens"], 6)

    def test_scan_summary_and_markdown_report(self):
        reference = self._reference()
        results = scan.scan([("Source/A.cpp", self.pasted + "\n"), ("Source/B.cpp", "int y;\n")],
                            reference, scan.Options())
        self.assertEqual(results["summary"]["files_scanned"], 2)
        self.assertEqual(results["summary"]["files_with_findings"], 1)
        self.assertEqual(results["summary"]["code_findings"], 1)
        report = {"scanner": {}, "candidate": {"mode": "tree", "root": "r"},
                  "reference": {"root": "ref"}, "results": results}
        markdown = scan.markdown_report(report)
        self.assertIn("| strong | code |", markdown)
        self.assertIn("## Substantive code runs", markdown)
        self.assertIn("`Source/A.cpp:1-", markdown)
        self.assertIn("100.0% | `Source/A.cpp`", markdown)

    def test_cli_tree_mode_writes_reports_and_gates(self):
        self._write(self.candidate / "Source" / "Pasted.cpp", self.pasted + "\n")
        self._write(self.candidate / "Source" / "Clean.cpp", "int z = 3;\n")
        self._write(self.candidate / "Tools" / "blob.bin", "\0\0binary")
        json_path = self.root / "report.json"
        md_path = self.root / "report.md"
        code = scan.run(["--tree", str(self.candidate), "--reference-root", str(self.reference),
                         "--json-output", str(json_path), "--markdown-output", str(md_path),
                         "--quiet"])
        self.assertEqual(code, 0)
        report = json.loads(json_path.read_text(encoding="utf-8"))
        self.assertEqual(report["candidate"]["mode"], "tree")
        self.assertEqual(report["results"]["summary"]["files_scanned"], 2)
        self.assertEqual(report["results"]["summary"]["code_findings"], 1)
        self.assertEqual(report["scanner"]["options"]["code"]["guaranteed_tokens"], 15)
        self.assertIn("Provenance scan", md_path.read_text(encoding="utf-8"))
        gated = scan.run(["--tree", str(self.candidate), "--reference-root", str(self.reference),
                          "--fail-on-findings", "--quiet", "--json-output", str(json_path)])
        self.assertEqual(gated, 1)

    def test_cli_only_filter_and_missing_reference(self):
        self._write(self.candidate / "Source" / "Pasted.cpp", self.pasted + "\n")
        json_path = self.root / "only.json"
        code = scan.run(["--tree", str(self.candidate), "--reference-root", str(self.reference),
                         "--only", "Tools/", "--quiet", "--json-output", str(json_path)])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(json_path.read_text())["results"]["summary"]["files_scanned"], 0)
        empty = self.root / "empty-reference"
        empty.mkdir()
        self.assertEqual(scan.run(["--tree", str(self.candidate), "--reference-root", str(empty),
                                   "--quiet"]), 2)


class RepoModeTests(unittest.TestCase):
    """Repository mode scans exactly the exporter's public set from HEAD."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "repo"
        self.repo.mkdir()
        self._git("init", "-q")
        self._git("config", "user.email", "t@example.invalid")
        self._git("config", "user.name", "t")
        self.pasted = synthetic_function("Legend_Pasted")
        self._write("Source/WoT/Public.cpp", self.pasted + "\n")
        self._write("original/System/Game.u", "retail bytes")
        self._write("Tools/test/sweep/fixtures/x.json", self.pasted)
        self._write("docs/HANDOFF-private.md", self.pasted)
        self._write("Tools/dist/fixtures/pointer.bin",
                    "version https://git-lfs.github.com/spec/v1\noid sha256:0\nsize 1\n")
        self._write("Tools/public/public-files.txt",
                    "Source/WoT/Public.cpp\n"
                    "Tools/dist/fixtures/pointer.bin\n"
                    "Tools/public/public-files.txt\n")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "fixture")
        self.reference = Path(self.tmp.name) / "reference"
        (self.reference / "devtree" / "Src").mkdir(parents=True)
        (self.reference / "devtree" / "Src" / "Thing.cpp").write_text(self.pasted + "\n")

    def tearDown(self):
        self.tmp.cleanup()

    def _git(self, *args):
        subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def _write(self, relative, text):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_public_paths_follow_export_rules(self):
        paths = scan.public_paths_from_head(self.repo)
        self.assertEqual(paths, [
            "Source/WoT/Public.cpp",
            "Tools/dist/fixtures/pointer.bin",
            "Tools/public/public-files.txt",
        ])

    def test_repo_scan_reports_commit_and_skips_pointers(self):
        json_path = Path(self.tmp.name) / "repo.json"
        self._write("Source/WoT/Public.cpp", self.pasted + "\n// dirty\n")
        code = scan.run(["--repo", str(self.repo), "--reference-root", str(self.reference),
                         "--quiet", "--json-output", str(json_path)])
        self.assertEqual(code, 0)
        report = json.loads(json_path.read_text(encoding="utf-8"))
        self.assertEqual(report["candidate"]["mode"], "repo")
        self.assertEqual(len(report["candidate"]["commit"]), 40)
        self.assertEqual(report["candidate"]["dirty_paths"], ["Source/WoT/Public.cpp"])
        scanned = [entry["path"] for entry in report["results"]["files"]]
        self.assertEqual(scanned, [
            "Source/WoT/Public.cpp", "Tools/public/public-files.txt",
        ])
        self.assertEqual(report["results"]["summary"]["code_findings"], 1)


if __name__ == "__main__":
    unittest.main()
