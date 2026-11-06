#!/usr/bin/env python3
"""Find text in the public-bound tree that was copied from the private reference corpora.

The scan covers exactly the files ``Tools/public/export_public_tree.py`` would
publish from committed HEAD (same allow and deny rules), or any directory tree
given with ``--tree``. It compares them with the private reference root
(``Tools/wot_config.py``: CLI argument, then ``WOT_REFERENCE_ROOT``, then the
repository root):

* ``devtree/``         the 2003 development snapshot: C++, headers,
                       UnrealScript, localization, loose text;
* ``exports/script``   the batch-exported shipped UnrealScript;
* ``original/System``  the retail localization (``*.int``) and config files.

Three passes:

* ``code``      token shingles over code with comments removed, against
                reference code;
* ``prose``     word shingles over comments, Markdown prose and string
                literals, against reference comments, loose text and retail
                localization values;
* ``literals``  whole string literals against retail localization values and
                reference string literals.

Every hit is extended token by token in both directions, so a finding is the
maximal copied run with both locations. The scanner reports; the audit
document decides what each finding means (behavioral port, pasted code,
pasted comment, retail prose, derived table, boilerplate).

Detection guarantee: any shared run of at least ``ngram + window - 1`` tokens
(default 15 code tokens, 11 prose words) is found, because the reference index
is winnowed and the candidate side is checked at every position.

Python 3.9 compatible, standard library only.
"""
from __future__ import annotations

import argparse
import array
import bisect
import json
import os
import re
import subprocess
import sys
import time
from collections import OrderedDict, deque
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Optional, Sequence, Tuple

_HERE = Path(__file__).resolve().parent
_TOOLS = _HERE.parent
for _entry in (str(_HERE), str(_TOOLS)):
    if _entry not in sys.path:
        sys.path.insert(0, _entry)

import export_public_tree  # noqa: E402  (Tools/public)
import wot_config  # noqa: E402  (Tools)

SCANNER_VERSION = 1

# ---------------------------------------------------------------------------
# File classification
# ---------------------------------------------------------------------------

KIND_C = "c"          # C, C++, Objective-C++, C#, UnrealScript, shaders
KIND_PY = "py"        # Python and other hash-comment languages
KIND_MD = "md"        # Markdown and loose text: fenced blocks are code
KIND_PLAIN = "plain"  # config, data, patches: code tokens and prose words

_KIND_BY_SUFFIX = {
    ".c": KIND_C, ".cc": KIND_C, ".cpp": KIND_C, ".cxx": KIND_C, ".h": KIND_C,
    ".hh": KIND_C, ".hpp": KIND_C, ".inl": KIND_C, ".m": KIND_C, ".mm": KIND_C,
    ".cs": KIND_C, ".uc": KIND_C, ".usf": KIND_C, ".ush": KIND_C, ".glsl": KIND_C,
    ".hlsl": KIND_C, ".java": KIND_C, ".js": KIND_C, ".ts": KIND_C,
    ".py": KIND_PY, ".pyi": KIND_PY, ".sh": KIND_PY, ".bash": KIND_PY,
    ".ps1": KIND_PY, ".psm1": KIND_PY, ".toml": KIND_PY, ".yml": KIND_PY,
    ".yaml": KIND_PY, ".cmake": KIND_PY, ".rb": KIND_PY, ".pl": KIND_PY,
    ".md": KIND_MD, ".markdown": KIND_MD, ".txt": KIND_MD, ".rst": KIND_MD,
    ".html": KIND_MD, ".htm": KIND_MD,
    ".ini": KIND_PLAIN, ".int": KIND_PLAIN, ".json": KIND_PLAIN, ".cfg": KIND_PLAIN,
    ".csv": KIND_PLAIN, ".bat": KIND_PLAIN, ".cmd": KIND_PLAIN, ".patch": KIND_PLAIN,
    ".diff": KIND_PLAIN, ".uproject": KIND_PLAIN, ".uplugin": KIND_PLAIN,
    ".xml": KIND_PLAIN, ".plist": KIND_PLAIN, ".def": KIND_PLAIN, ".rc": KIND_PLAIN,
    ".gitignore": KIND_PLAIN, ".gitattributes": KIND_PLAIN, ".editorconfig": KIND_PLAIN,
}
_KIND_BY_NAME = {
    "LICENSE": KIND_MD, "NOTICE": KIND_MD, "README": KIND_MD, "Makefile": KIND_PY,
    ".gitignore": KIND_PLAIN, ".gitattributes": KIND_PLAIN, ".editorconfig": KIND_PLAIN,
}
MAX_FILE_BYTES = 16 * 1024 * 1024
LFS_POINTER_PREFIX = b"version https://git-lfs.github.com/spec/"


def file_kind(path: str) -> Optional[str]:
    """Return the scan kind for a path, or None when the file is not text we scan."""
    name = Path(path).name
    if name in _KIND_BY_NAME:
        return _KIND_BY_NAME[name]
    suffix = Path(name).suffix.lower()
    if suffix in _KIND_BY_SUFFIX:
        return _KIND_BY_SUFFIX[suffix]
    if not suffix and name.isupper():
        return KIND_MD
    return None


def read_text(path: Path) -> Optional[str]:
    """Read a text file; None for binaries, LFS pointers and oversized files."""
    try:
        size = path.stat().st_size
        if size > MAX_FILE_BYTES:
            return None
        data = path.read_bytes()
    except OSError:
        return None
    if data.startswith(LFS_POINTER_PREFIX):
        return None
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8", errors="replace")


# ---------------------------------------------------------------------------
# Splitting code from comments and literals
# ---------------------------------------------------------------------------

# Code text with comments blanked (newlines preserved) plus the comment and
# literal texts with their start lines.
Pieces = Tuple[str, List[Tuple[int, str]], List[Tuple[int, str]]]

_C_SPLIT_RE = re.compile(
    r'"(?:\\.|[^"\\\n])*"?'
    r"|'(?:\\.|[^'\\\n])*'?"
    r"|//[^\n]*"
    r"|/\*.*?(?:\*/|\Z)"
    r"|[^\"'/]+"
    r"|/",
    re.S,
)
_PY_SPLIT_RE = re.compile(
    r'"""(?:\\.|[^\\])*?(?:"""|\Z)'
    r"|'''(?:\\.|[^\\])*?(?:'''|\Z)"
    r'|"(?:\\.|[^"\\\n])*"?'
    r"|'(?:\\.|[^'\\\n])*'?"
    r"|#[^\n]*"
    r"|[^\"'#]+"
    r"|#",
    re.S,
)
_FENCE_RE = re.compile(r"^\s{0,3}(`{3,}|~{3,})")


def _blank(segment: str) -> str:
    return re.sub(r"[^\n]", " ", segment)


def _strip_quotes(literal: str) -> str:
    if len(literal) >= 6 and literal[:3] in ('"""', "'''"):
        body = literal[3:]
        return body[:-3] if body.endswith(literal[:3]) else body
    if len(literal) >= 2 and literal[-1] == literal[0]:
        return literal[1:-1]
    return literal[1:]


def split_c(text: str) -> Pieces:
    """Split C-family source: code with comments blanked, comments, string literals."""
    code: List[str] = []
    comments: List[Tuple[int, str]] = []
    literals: List[Tuple[int, str]] = []
    line = 1
    for match in _C_SPLIT_RE.finditer(text):
        segment = match.group()
        head = segment[:2]
        if head == "//":
            comments.append((line, segment[2:]))
            code.append(_blank(segment))
        elif head == "/*":
            body = segment[2:-2] if segment.endswith("*/") else segment[2:]
            comments.append((line, body))
            code.append(_blank(segment))
        elif segment[0] in "\"'":
            if len(segment) > 2 or (len(segment) == 2 and segment[0] != segment[1]):
                literals.append((line, _strip_quotes(segment)))
            code.append(segment)
        else:
            code.append(segment)
        line += segment.count("\n")
    return "".join(code), comments, literals


def split_py(text: str) -> Pieces:
    """Split hash-comment source (Python, shell, PowerShell)."""
    code: List[str] = []
    comments: List[Tuple[int, str]] = []
    literals: List[Tuple[int, str]] = []
    line = 1
    for match in _PY_SPLIT_RE.finditer(text):
        segment = match.group()
        if segment[0] == "#":
            comments.append((line, segment[1:]))
            code.append(_blank(segment))
        elif segment[0] in "\"'":
            body = _strip_quotes(segment)
            if body.strip():
                literals.append((line, body))
            code.append(segment)
        else:
            code.append(segment)
        line += segment.count("\n")
    return "".join(code), comments, literals


def split_markdown(text: str) -> Tuple[List[Tuple[int, str]], List[Tuple[int, str]]]:
    """Return (code_lines, prose_lines) with line numbers; fenced blocks are code."""
    code_lines: List[Tuple[int, str]] = []
    prose_lines: List[Tuple[int, str]] = []
    fence: Optional[str] = None
    for number, raw in enumerate(text.split("\n"), start=1):
        match = _FENCE_RE.match(raw)
        if match:
            marker = match.group(1)
            if fence is None:
                fence = marker[0] * 3
                continue
            if marker.startswith(fence):
                fence = None
                continue
        if fence is not None:
            code_lines.append((number, raw))
        else:
            prose_lines.append((number, raw))
    return code_lines, prose_lines


# ---------------------------------------------------------------------------
# Token and word streams
# ---------------------------------------------------------------------------

_CODE_TOKEN_RE = re.compile(
    r'"(?:\\.|[^"\\\n])*"?'
    r"|'(?:\\.|[^'\\\n])*'?"
    r"|[A-Za-z_][A-Za-z0-9_]*"
    r"|0[xX][0-9A-Fa-f]+[uUlL]*"
    r"|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?[fFuUlL]*"
    r"|\.\d+(?:[eE][+-]?\d+)?[fF]?"
    r"|::|->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=|\*\*|//"
    r"|[^\sA-Za-z0-9_]"
)
_WORD_RE = re.compile(r"[a-z0-9]+(?:'[a-z]+)?")
_TAG_RE = re.compile(r"<[^>]{0,200}>")
_INT_VALUE_RE = re.compile(r"^\s*[^=\[;]+=(.*)$")


class Stream:
    """Parallel token texts and 1-based line numbers."""

    __slots__ = ("texts", "lines")

    def __init__(self) -> None:
        self.texts: List[str] = []
        self.lines = array.array("I")

    def __len__(self) -> int:
        return len(self.texts)

    def add(self, text: str, line: int) -> None:
        self.texts.append(text)
        self.lines.append(line)


def _line_starts(text: str) -> List[int]:
    starts = [0]
    for index, char in enumerate(text):
        if char == "\n":
            starts.append(index + 1)
    return starts


def code_stream(code_text: str, first_line: int = 1) -> Stream:
    """Tokenize comment-free code; literals stay single tokens."""
    stream = Stream()
    starts = _line_starts(code_text)
    for match in _CODE_TOKEN_RE.finditer(code_text):
        line = bisect.bisect_right(starts, match.start()) - 1 + first_line
        stream.add(match.group(), line)
    return stream


def code_stream_from_lines(lines: Iterable[Tuple[int, str]]) -> Stream:
    stream = Stream()
    for line, text in lines:
        for match in _CODE_TOKEN_RE.finditer(text):
            stream.add(match.group(), line)
    return stream


def word_stream(chunks: Iterable[Tuple[int, str]]) -> Stream:
    """Lower-case words from (start_line, text) chunks; multi-line chunks count lines."""
    stream = Stream()
    for line, text in chunks:
        for offset, part in enumerate(text.split("\n")):
            if "<" in part:
                part = _TAG_RE.sub(" ", part)
            for match in _WORD_RE.finditer(part.lower()):
                stream.add(match.group(), line + offset)
    return stream


def normalize_literal(text: str) -> str:
    text = text.replace("\\n", " ").replace("\\t", " ").replace('\\"', '"').replace("\\'", "'")
    return " ".join(text.lower().split())


def int_values(text: str) -> List[Tuple[int, str]]:
    """``key=value`` values from a localization or config file."""
    values: List[Tuple[int, str]] = []
    for number, raw in enumerate(text.split("\n"), start=1):
        match = _INT_VALUE_RE.match(raw)
        if not match:
            continue
        value = match.group(1).strip()
        if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
            value = value[1:-1]
        if value:
            values.append((number, value))
    return values


class Extracted:
    """Everything one file contributes to the three passes."""

    __slots__ = ("code", "prose", "literals", "text_lines")

    def __init__(self, code: Stream, prose: Stream,
                 literals: List[Tuple[int, str]], text_lines: List[str]) -> None:
        self.code = code
        self.prose = prose
        self.literals = literals
        self.text_lines = text_lines


def extract(path: str, text: str) -> Optional[Extracted]:
    kind = file_kind(path)
    if kind is None:
        return None
    text_lines = text.split("\n")
    if kind == KIND_C or kind == KIND_PY:
        code_text, comments, literals = (split_c if kind == KIND_C else split_py)(text)
        code = code_stream(code_text)
        prose = word_stream(comments + literals)
        return Extracted(code, prose, literals, text_lines)
    if kind == KIND_MD:
        code_lines, prose_lines = split_markdown(text)
        code_text, comments, literals = split_c("\n".join(line for _, line in code_lines))
        code = Stream()
        if code_lines:
            # Re-map the joined block's line numbers to the document's.
            numbers = [number for number, _ in code_lines]
            raw = code_stream(code_text)
            for token, line in zip(raw.texts, raw.lines):
                code.add(token, numbers[min(line - 1, len(numbers) - 1)])
            offsets = [(numbers[min(line - 1, len(numbers) - 1)], body)
                       for line, body in comments]
            prose = word_stream(prose_lines + offsets)
        else:
            prose = word_stream(prose_lines)
        return Extracted(code, prose, [], text_lines)
    # KIND_PLAIN
    numbered = list(enumerate(text_lines, start=1))
    code = code_stream_from_lines(numbered)
    prose = word_stream(numbered)
    literals = int_values(text) if Path(path).suffix.lower() in (".int", ".ini") else []
    return Extracted(code, prose, literals, text_lines)


# ---------------------------------------------------------------------------
# Shingles, winnowing, index
# ---------------------------------------------------------------------------

_MOD = (1 << 61) - 1
_BASE = 1000003


class Vocabulary:
    """Stable integer ids for token texts within one run."""

    def __init__(self) -> None:
        self._ids: Dict[str, int] = {}

    def ids(self, texts: Sequence[str]) -> List[int]:
        table = self._ids
        out = []
        for text in texts:
            value = table.get(text)
            if value is None:
                value = len(table) + 1
                table[text] = value
            out.append(value)
        return out


def shingle_hashes(ids: Sequence[int], n: int) -> List[int]:
    """Rolling polynomial hash of every n-gram; result[i] covers ids[i:i+n]."""
    count = len(ids)
    if n <= 0 or count < n:
        return []
    power = pow(_BASE, n, _MOD)
    out: List[int] = []
    value = 0
    for index, token in enumerate(ids):
        value = (value * _BASE + token) % _MOD
        if index >= n:
            value = (value - ids[index - n] * power) % _MOD
        if index >= n - 1:
            out.append(value)
    return out


def winnow(hashes: Sequence[int], window: int) -> List[Tuple[int, int]]:
    """Select the minimum hash of every window (rightmost on ties) as (position, hash)."""
    if not hashes:
        return []
    if window <= 1:
        return list(enumerate(hashes))
    selected: List[Tuple[int, int]] = []
    pending: deque = deque()
    last = -1
    for index, value in enumerate(hashes):
        while pending and hashes[pending[-1]] >= value:
            pending.pop()
        pending.append(index)
        if pending[0] <= index - window:
            pending.popleft()
        if index >= window - 1 and pending[0] != last:
            last = pending[0]
            selected.append((last, hashes[last]))
    if not selected:
        best = min(range(len(hashes)), key=lambda i: (hashes[i], -i))
        selected.append((best, hashes[best]))
    return selected


class ShingleIndex:
    def __init__(self, ngram: int, window: int) -> None:
        self.ngram = ngram
        self.window = window
        self.table: Dict[int, int] = {}

    def add(self, file_id: int, ids: Sequence[int]) -> None:
        setdefault = self.table.setdefault
        for position, value in winnow(shingle_hashes(ids, self.ngram), self.window):
            setdefault(value, (file_id << 32) | position)

    def lookup(self, value: int) -> Optional[Tuple[int, int]]:
        packed = self.table.get(value)
        if packed is None:
            return None
        return packed >> 32, packed & 0xFFFFFFFF

    def __len__(self) -> int:
        return len(self.table)


# ---------------------------------------------------------------------------
# Reference corpus
# ---------------------------------------------------------------------------

REFERENCE_CODE_SUFFIXES = frozenset({
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".uc", ".cs",
    ".usf", ".ush", ".glsl", ".hlsl", ".m", ".mm",
})
REFERENCE_TEXT_SUFFIXES = frozenset({".int", ".ini", ".txt", ".html", ".htm", ".md", ".rst"})
REFERENCE_SKIP_DIRS = frozenset({"builddebug", "buildrelease", ".git"})


class Corpus:
    """Reference files with lazily rebuilt streams for match extension."""

    def __init__(self, cache_size: int = 192) -> None:
        self.paths: List[Path] = []
        self.labels: List[str] = []
        self._cache: "OrderedDict[int, Extracted]" = OrderedDict()
        self._cache_size = cache_size

    def add(self, path: Path, label: str) -> int:
        self.paths.append(path)
        self.labels.append(label)
        return len(self.paths) - 1

    def extracted(self, file_id: int) -> Extracted:
        cached = self._cache.get(file_id)
        if cached is not None:
            self._cache.move_to_end(file_id)
            return cached
        text = read_text(self.paths[file_id]) or ""
        value = extract(self.labels[file_id], text) or Extracted(Stream(), Stream(), [], [])
        self._cache[file_id] = value
        if len(self._cache) > self._cache_size:
            self._cache.popitem(last=False)
        return value


def iter_reference_files(root: Path, suffixes: Iterable[str]) -> Iterator[Path]:
    wanted = frozenset(s.lower() for s in suffixes)
    if not root.is_dir():
        return
    for directory, names, files in os.walk(root):
        names[:] = sorted(n for n in names if n.lower() not in REFERENCE_SKIP_DIRS)
        for name in sorted(files):
            if Path(name).suffix.lower() in wanted:
                yield Path(directory) / name


class Reference:
    """The three reference indexes built from the private root."""

    def __init__(self, code_ngram: int, code_window: int,
                 prose_ngram: int, prose_window: int, min_literal_words: int) -> None:
        self.corpus = Corpus()
        self.code_vocab = Vocabulary()
        self.prose_vocab = Vocabulary()
        self.code_index = ShingleIndex(code_ngram, code_window)
        self.prose_index = ShingleIndex(prose_ngram, prose_window)
        self.literals: Dict[str, Tuple[int, int]] = {}
        self.min_literal_words = min_literal_words
        self.code_files = 0
        self.prose_files = 0

    def add_file(self, path: Path, label: str) -> None:
        text = read_text(path)
        if text is None:
            return
        pieces = extract(label, text)
        if pieces is None:
            return
        file_id = self.corpus.add(path, label)
        if len(pieces.code):
            self.code_index.add(file_id, self.code_vocab.ids(pieces.code.texts))
            self.code_files += 1
        if len(pieces.prose):
            self.prose_index.add(file_id, self.prose_vocab.ids(pieces.prose.texts))
            self.prose_files += 1
        for line, literal in pieces.literals:
            normalized = normalize_literal(literal)
            if len(normalized.split()) >= self.min_literal_words:
                self.literals.setdefault(normalized, (file_id, line))

    def build(self, reference_root: Path, log) -> Dict[str, object]:
        devtree = wot_config.devtree_dir(reference_root, required=False)
        exports = wot_config.exports_dir(reference_root, required=False)
        original = wot_config.original_dir(reference_root, required=False)
        sources: List[Tuple[Path, str]] = []
        # Shipped script first: it wins ties over the development snapshot.
        for path in iter_reference_files(exports / "script", REFERENCE_CODE_SUFFIXES):
            sources.append((path, "exports/" + path.relative_to(exports).as_posix()))
        for path in iter_reference_files(devtree, REFERENCE_CODE_SUFFIXES | REFERENCE_TEXT_SUFFIXES):
            sources.append((path, "devtree/" + path.relative_to(devtree).as_posix()))
        for path in iter_reference_files(original / "System", {".int", ".ini"}):
            sources.append((path, "original/" + path.relative_to(original).as_posix()))
        if not sources:
            raise wot_config.ConfigurationError(
                "no reference files under %s; set WOT_REFERENCE_ROOT to the private root "
                "that contains devtree/, exports/ and original/" % reference_root)
        started = time.monotonic()
        for count, (path, label) in enumerate(sources, start=1):
            self.add_file(path, label)
            if log and count % 500 == 0:
                log("  indexed %d/%d reference files (%.0fs)" % (count, len(sources),
                                                                time.monotonic() - started))
        return {
            "root": str(reference_root),
            "files": len(sources),
            "code_files": self.code_files,
            "prose_files": self.prose_files,
            "code_index_entries": len(self.code_index),
            "prose_index_entries": len(self.prose_index),
            "literal_count": len(self.literals),
            "build_seconds": round(time.monotonic() - started, 1),
        }


# ---------------------------------------------------------------------------
# Matching
# ---------------------------------------------------------------------------

KEYWORDS = frozenset("""
alignas alignof and asm auto bool break case catch char class const constexpr
continue default delete do double else enum explicit export extern false final
float for friend goto if inline int long mutable namespace new noexcept not
nullptr operator or override private protected public register return short
signed sizeof static struct switch template this throw true try typedef typename
union unsigned using virtual void volatile while
function event state local var simulated native latent exec defaultproperties
self super none foreach ignores optional out coerce config globalconfig
def elif except finally from global import in is lambda nonlocal pass raise with
yield print self cls
int8 int16 int32 int64 uint8 uint16 uint32 uint64 float32 float64 size_t
""".split())
_IDENTIFIER_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]{3,}")


class Span:
    __slots__ = ("start", "end", "ref_file", "ref_start", "ref_end")

    def __init__(self, start: int, end: int, ref_file: int, ref_start: int, ref_end: int) -> None:
        self.start = start
        self.end = end
        self.ref_file = ref_file
        self.ref_start = ref_start
        self.ref_end = ref_end

    @property
    def length(self) -> int:
        return self.end - self.start


def match_stream(stream: Stream, vocab: Vocabulary, index: ShingleIndex, corpus: Corpus,
                 attribute: str, min_length: int) -> List[Span]:
    """Find maximal runs of ``stream`` that also occur in the indexed reference."""
    n = index.ngram
    texts = stream.texts
    hashes = shingle_hashes(vocab.ids(texts), n)
    spans: List[Span] = []
    covered_until = 0
    i = 0
    total = len(texts)
    while i < len(hashes):
        hit = index.lookup(hashes[i])
        if hit is None:
            i += 1
            continue
        file_id, position = hit
        ref_texts = getattr(corpus.extracted(file_id), attribute).texts
        if texts[i:i + n] != ref_texts[position:position + n]:
            i += 1  # hash collision
            continue
        a, b = i, position
        while a > covered_until and b > 0 and texts[a - 1] == ref_texts[b - 1]:
            a -= 1
            b -= 1
        length = n
        ref_total = len(ref_texts)
        while a + length < total and b + length < ref_total \
                and texts[a + length] == ref_texts[b + length]:
            length += 1
        if length >= min_length:
            spans.append(Span(a, a + length, file_id, b, b + length))
        covered_until = a + length
        i = max(i + 1, a + length)
    return spans


_DATA_PUNCTUATION = frozenset({",", "{", "}", "(", ")", "[", "]", ";", "-", "+"})


def span_kind(texts: Sequence[str]) -> str:
    """Classify a code run: ``include`` (a header list), ``table`` (mostly
    numbers, strings and separators) or ``code``."""
    if "#" in texts and "include" in texts and ";" not in texts:
        return "include"
    data = sum(1 for text in texts
               if text[0].isdigit() or text[0] in "\"'"
               or (text[0] == "." and len(text) > 1 and text[1].isdigit()))
    punctuation = sum(1 for text in texts if text in _DATA_PUNCTUATION)
    if texts and data >= 0.3 * len(texts) and data + punctuation >= 0.7 * len(texts):
        return "table"
    return "code"


def richness(texts: Sequence[str]) -> int:
    """Distinct identifiers of four or more characters that are not keywords."""
    found = set()
    for text in texts:
        if _IDENTIFIER_RE.fullmatch(text) and text.lower() not in KEYWORDS:
            found.add(text)
    return len(found)


def _snippet(lines: Sequence[str], start_line: int, end_line: int, limit: int = 600) -> str:
    text = "\n".join(lines[max(start_line - 1, 0):end_line])
    text = text.strip("\n")
    if len(text) > limit:
        text = text[:limit] + "…"
    return text


# ---------------------------------------------------------------------------
# Candidate enumeration
# ---------------------------------------------------------------------------

def _git(repo: Path, *arguments: str) -> str:
    completed = subprocess.run(["git", "-C", str(repo), *arguments], check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return completed.stdout.decode("utf-8", errors="replace")


def public_paths_from_head(repo: Path) -> List[str]:
    """Paths of HEAD that the public exporter would publish, in tree order."""
    return export_public_tree.public_manifest_paths(repo, "HEAD")


def tree_paths(tree: Path) -> List[str]:
    found: List[str] = []
    for directory, names, files in os.walk(tree):
        names[:] = sorted(n for n in names if n != ".git")
        for name in sorted(files):
            found.append((Path(directory) / name).relative_to(tree).as_posix())
    return found


# ---------------------------------------------------------------------------
# The scan
# ---------------------------------------------------------------------------

class Options:
    def __init__(self, code_ngram: int = 10, code_window: int = 6, min_tokens: int = 15,
                 prose_ngram: int = 8, prose_window: int = 4, min_words: int = 12,
                 min_literal_words: int = 4) -> None:
        self.code_ngram = code_ngram
        self.code_window = code_window
        self.min_tokens = min_tokens
        self.prose_ngram = prose_ngram
        self.prose_window = prose_window
        self.min_words = min_words
        self.min_literal_words = min_literal_words

    def as_dict(self) -> Dict[str, object]:
        return {
            "code": {"ngram": self.code_ngram, "window": self.code_window,
                     "min_tokens": self.min_tokens,
                     "guaranteed_tokens": self.code_ngram + self.code_window - 1},
            "prose": {"ngram": self.prose_ngram, "window": self.prose_window,
                      "min_words": self.min_words,
                      "guaranteed_words": self.prose_ngram + self.prose_window - 1},
            "literals": {"min_words": self.min_literal_words},
        }


def build_reference(reference_root: Path, options: Options, log=None) -> Tuple[Reference, Dict[str, object]]:
    reference = Reference(options.code_ngram, options.code_window,
                          options.prose_ngram, options.prose_window, options.min_literal_words)
    stats = reference.build(reference_root, log)
    return reference, stats


def scan_file(label: str, text: str, reference: Reference, options: Options) -> Dict[str, object]:
    pieces = extract(label, text)
    result: Dict[str, object] = {"path": label, "code": [], "prose": [], "literals": [],
                                 "tokens": 0, "covered": 0, "words": 0}
    if pieces is None:
        result["skipped"] = "not a scanned text kind"
        return result
    corpus = reference.corpus
    result["tokens"] = len(pieces.code)
    result["words"] = len(pieces.prose)
    covered = 0
    for span in match_stream(pieces.code, reference.code_vocab, reference.code_index,
                             corpus, "code", options.min_tokens):
        ref = corpus.extracted(span.ref_file)
        start_line, end_line = pieces.code.lines[span.start], pieces.code.lines[span.end - 1]
        ref_start, ref_end = ref.code.lines[span.ref_start], ref.code.lines[span.ref_end - 1]
        covered += span.length
        result["code"].append({
            "start_line": start_line, "end_line": end_line, "tokens": span.length,
            "rich": richness(pieces.code.texts[span.start:span.end]),
            "kind": span_kind(pieces.code.texts[span.start:span.end]),
            "ref": corpus.labels[span.ref_file],
            "ref_start_line": ref_start, "ref_end_line": ref_end,
            "snippet": _snippet(pieces.text_lines, start_line, end_line),
            "ref_snippet": _snippet(ref.text_lines, ref_start, ref_end),
        })
    result["covered"] = covered
    for span in match_stream(pieces.prose, reference.prose_vocab, reference.prose_index,
                             corpus, "prose", options.min_words):
        ref = corpus.extracted(span.ref_file)
        start_line, end_line = pieces.prose.lines[span.start], pieces.prose.lines[span.end - 1]
        ref_start, ref_end = ref.prose.lines[span.ref_start], ref.prose.lines[span.ref_end - 1]
        result["prose"].append({
            "start_line": start_line, "end_line": end_line, "words": span.length,
            "text": " ".join(pieces.prose.texts[span.start:span.end])[:400],
            "ref": corpus.labels[span.ref_file],
            "ref_start_line": ref_start, "ref_end_line": ref_end,
            "snippet": _snippet(pieces.text_lines, start_line, end_line, 400),
        })
    seen = set()
    for line, literal in pieces.literals:
        normalized = normalize_literal(literal)
        if normalized in seen:
            continue
        hit = reference.literals.get(normalized)
        if hit is None or len(normalized.split()) < options.min_literal_words:
            continue
        seen.add(normalized)
        result["literals"].append({
            "line": line, "text": normalized[:300],
            "ref": corpus.labels[hit[0]], "ref_line": hit[1],
        })
    return result


def scan(candidates: Iterable[Tuple[str, str]], reference: Reference, options: Options,
         log=None) -> Dict[str, object]:
    """Scan (label, text) pairs; return per-file results and totals."""
    files: List[Dict[str, object]] = []
    started = time.monotonic()
    for count, (label, text) in enumerate(candidates, start=1):
        files.append(scan_file(label, text, reference, options))
        if log and count % 200 == 0:
            log("  scanned %d files (%.0fs)" % (count, time.monotonic() - started))
    code_findings = sum(len(f["code"]) for f in files)
    prose_findings = sum(len(f["prose"]) for f in files)
    literal_findings = sum(len(f["literals"]) for f in files)
    with_findings = sum(1 for f in files if f["code"] or f["prose"] or f["literals"])
    return {
        "files": files,
        "summary": {
            "files_scanned": len(files),
            "files_with_findings": with_findings,
            "code_findings": code_findings,
            "prose_findings": prose_findings,
            "literal_findings": literal_findings,
            "code_tokens": sum(int(f["tokens"]) for f in files),
            "code_tokens_covered": sum(int(f["covered"]) for f in files),
        },
        "scan_seconds": round(time.monotonic() - started, 1),
    }


def read_candidates(root: Path, paths: Sequence[str]) -> Iterator[Tuple[str, str]]:
    for path in paths:
        if file_kind(path) is None:
            continue
        text = read_text(root / path)
        if text is None:
            continue
        yield path, text


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def strength(tokens: int) -> str:
    if tokens >= 40:
        return "strong"
    if tokens >= 25:
        return "medium"
    return "weak"


def markdown_report(report: Dict[str, object], top: int = 400) -> str:
    summary = report["results"]["summary"]  # type: ignore[index]
    files = report["results"]["files"]  # type: ignore[index]
    lines = ["# Provenance scan", ""]
    candidate = report["candidate"]  # type: ignore[index]
    lines.append("Candidate: `%s` (%s%s)" % (candidate["root"], candidate["mode"],
                                            ", commit " + candidate["commit"][:12]
                                            if candidate.get("commit") else ""))
    lines.append("Reference: `%s`" % report["reference"]["root"])  # type: ignore[index]
    lines.append("")
    lines.append("| files scanned | with findings | code | prose | literals | tokens covered |")
    lines.append("|---:|---:|---:|---:|---:|---:|")
    covered = summary["code_tokens_covered"]
    total = max(int(summary["code_tokens"]), 1)
    lines.append("| %d | %d | %d | %d | %d | %d / %d (%.2f%%) |" % (
        summary["files_scanned"], summary["files_with_findings"], summary["code_findings"],
        summary["prose_findings"], summary["literal_findings"], covered, total,
        100.0 * covered / total))
    lines.append("")
    rows = []
    for entry in files:
        for finding in entry["code"]:
            rows.append((finding["tokens"], finding["rich"], entry["path"], finding))
    rows.sort(key=lambda row: (-row[0], -row[1], row[2]))

    def code_table(title: str, selected) -> None:
        lines.append("## %s" % title)
        lines.append("")
        lines.append("| strength | kind | tokens | rich | candidate | reference |")
        lines.append("|---|---|---:|---:|---|---|")
        for tokens, rich, path, finding in selected[:top]:
            lines.append("| %s | %s | %d | %d | `%s:%d-%d` | `%s:%d-%d` |" % (
                strength(tokens), finding.get("kind", "code"), tokens, rich, path,
                finding["start_line"], finding["end_line"],
                finding["ref"], finding["ref_start_line"], finding["ref_end_line"]))
        if len(selected) > top:
            lines.append("")
            lines.append("… %d more in the JSON report." % (len(selected) - top))
        lines.append("")

    substantive = [row for row in rows if row[3].get("kind", "code") == "code" and row[1] >= 3]
    code_table("Substantive code runs (kind code, three or more distinct identifiers)", substantive)
    code_table("All code runs (longest first)", rows)
    lines.append("## Files by covered share (5% or more)")
    lines.append("")
    lines.append("| covered | tokens | share | path |")
    lines.append("|---:|---:|---:|---|")
    shares = [(int(f["covered"]) / int(f["tokens"]), f) for f in files if int(f["tokens"]) > 0]
    shares.sort(key=lambda pair: -pair[0])
    for share, entry in shares:
        if share < 0.05:
            break
        lines.append("| %d | %d | %.1f%% | `%s` |" % (entry["covered"], entry["tokens"],
                                                    100.0 * share, entry["path"]))
    lines.append("")
    lines.append("## Prose runs (longest first)")
    lines.append("")
    lines.append("| words | candidate | reference | text |")
    lines.append("|---:|---|---|---|")
    prose_rows = []
    for entry in files:
        for finding in entry["prose"]:
            prose_rows.append((finding["words"], entry["path"], finding))
    prose_rows.sort(key=lambda row: (-row[0], row[1]))
    for words, path, finding in prose_rows[:top]:
        lines.append("| %d | `%s:%d-%d` | `%s:%d-%d` | %s |" % (
            words, path, finding["start_line"], finding["end_line"], finding["ref"],
            finding["ref_start_line"], finding["ref_end_line"],
            finding["text"][:160].replace("|", "\\|")))
    lines.append("")
    lines.append("## Literals")
    lines.append("")
    lines.append("| candidate | reference | text |")
    lines.append("|---|---|---|")
    for entry in files:
        for finding in entry["literals"]:
            lines.append("| `%s:%d` | `%s:%d` | %s |" % (
                entry["path"], finding["line"], finding["ref"], finding["ref_line"],
                finding["text"][:160].replace("|", "\\|")))
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def run(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--repo", help="Git worktree: scan what the public exporter would publish from HEAD "
                                       "(default: the repository containing this script)")
    source.add_argument("--tree", help="scan every text file under this directory instead")
    parser.add_argument("--reference-root", help="private root containing devtree/, exports/, original/ "
                                                 "(default: WOT_REFERENCE_ROOT, then the repository root)")
    parser.add_argument("--only", action="append", default=[],
                        help="scan only candidate paths with this prefix (repeatable)")
    parser.add_argument("--json-output", help="write the full report here")
    parser.add_argument("--markdown-output", help="write the summary here")
    parser.add_argument("--code-ngram", type=int, default=10)
    parser.add_argument("--code-window", type=int, default=6)
    parser.add_argument("--min-tokens", type=int, default=15)
    parser.add_argument("--prose-ngram", type=int, default=8)
    parser.add_argument("--prose-window", type=int, default=4)
    parser.add_argument("--min-words", type=int, default=12)
    parser.add_argument("--min-literal-words", type=int, default=4)
    parser.add_argument("--fail-on-findings", action="store_true",
                        help="exit 1 when any finding is reported (for gates)")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    log = (lambda message: None) if args.quiet else _log

    options = Options(args.code_ngram, args.code_window, args.min_tokens,
                      args.prose_ngram, args.prose_window, args.min_words, args.min_literal_words)
    try:
        reference_root = wot_config.reference_root(args.reference_root)
        if args.tree:
            root = Path(args.tree).resolve()
            candidate = {"mode": "tree", "root": str(root)}
            paths = tree_paths(root)
        else:
            root = Path(args.repo).resolve() if args.repo else _TOOLS.parent
            candidate = {"mode": "repo", "root": str(root),
                         "commit": _git(root, "rev-parse", "HEAD").strip()}
            paths = public_paths_from_head(root)
            dirty = [line[3:] for line in _git(root, "status", "--porcelain").splitlines()]
            candidate["dirty_paths"] = [p for p in dirty if p in set(paths)]
        if args.only:
            paths = [p for p in paths if any(p.startswith(prefix) for prefix in args.only)]
        candidate["paths"] = len(paths)
        log("Indexing reference under %s …" % reference_root)
        reference, reference_stats = build_reference(reference_root, options, log)
        log("Reference: %d files, %d code shingles, %d prose shingles, %d literals (%.0fs)" % (
            reference_stats["files"], reference_stats["code_index_entries"],
            reference_stats["prose_index_entries"], reference_stats["literal_count"],
            reference_stats["build_seconds"]))
        log("Scanning %d candidate paths …" % len(paths))
        results = scan(read_candidates(root, paths), reference, options, log)
    except (wot_config.ConfigurationError, subprocess.CalledProcessError, OSError) as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 2
    report = {
        "scanner": {"version": SCANNER_VERSION, "options": options.as_dict()},
        "candidate": candidate,
        "reference": reference_stats,
        "results": results,
    }
    summary = results["summary"]
    log("Findings: %d code, %d prose, %d literal in %d of %d files (%.0fs)" % (
        summary["code_findings"], summary["prose_findings"], summary["literal_findings"],
        summary["files_with_findings"], summary["files_scanned"], results["scan_seconds"]))
    if args.json_output:
        Path(args.json_output).write_text(json.dumps(report, indent=1, sort_keys=True), encoding="utf-8")
    if args.markdown_output:
        Path(args.markdown_output).write_text(markdown_report(report), encoding="utf-8")
    if not args.json_output and not args.markdown_output:
        print(markdown_report(report, top=60))
    total = summary["code_findings"] + summary["prose_findings"] + summary["literal_findings"]
    return 1 if args.fail_on_findings and total else 0


if __name__ == "__main__":
    raise SystemExit(run())
