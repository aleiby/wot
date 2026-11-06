#!/usr/bin/env python3
"""Read signed test vectors straight out of the C++ spec sources.

The release-plumbing specs cross-check the Python formats against the vectors
the native automation specs already accept. Reading them from the .cpp files
(rather than copying them into Python) means a vector that changes natively is
re-checked here automatically, and a copy can never silently drift.

Only the literal forms those files use are supported: `NAME = TEXT("...")`,
`NAME[] = TEXT(\n "...")` (adjacent literals concatenate), and
`NAME = TEXT(R"delim(...)delim")`. Escapes other than \\" and \\\\ are refused.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import List

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
CRYPTO_TESTS = REPOSITORY_ROOT / "Source/WoTBootstrapCore/Private/Tests/CryptoTests.cpp"
ORDERING_TESTS = REPOSITORY_ROOT / "Source/WoTBootstrapCore/Private/Tests/OrderingTests.cpp"
POLICY_TESTS = (REPOSITORY_ROOT
                / "Source/WoTUpdaterCore/Private/Tests/AnnouncementPolicyTests.cpp")
CONTRACT_TESTS = (REPOSITORY_ROOT
                  / "Source/WoTRetailReader/Private/Tests/ContractTests.cpp")


class VectorError(Exception):
    pass


def _parse_literal(text: str, start: int) -> str:
    position = start
    while text[position].isspace():
        position += 1
    if text.startswith('R"', position):
        open_paren = text.index("(", position)
        delimiter = text[position + 2:open_paren]
        close = text.index(")" + delimiter + '"', open_paren)
        return text[open_paren + 1:close]
    pieces: List[str] = []
    while True:
        while text[position].isspace():
            position += 1
        if text[position] != '"':
            break
        position += 1
        piece = []
        while text[position] != '"':
            character = text[position]
            if character == "\\":
                escaped = text[position + 1]
                if escaped not in '"\\':
                    raise VectorError(f"unsupported escape \\{escaped}")
                piece.append(escaped)
                position += 2
                continue
            if character == "\n":
                raise VectorError("unterminated literal")
            piece.append(character)
            position += 1
        pieces.append("".join(piece))
        position += 1
    if text[position] != ")":
        raise VectorError("literal is not followed by the TEXT() close")
    return "".join(pieces)


def literals(path: Path, name: str) -> List[str]:
    """Every value assigned to NAME via TEXT(...), in file order."""
    text = Path(path).read_text(encoding="utf-8")
    pattern = re.compile(r"\b" + re.escape(name) + r"\s*(?:\[\])?\s*=\s*TEXT\(")
    found = [_parse_literal(text, match.end()) for match in pattern.finditer(text)]
    if not found:
        raise VectorError(f"{name} not found in {path}")
    return found


def literal(path: Path, name: str) -> str:
    values = literals(path, name)
    if len(values) != 1:
        raise VectorError(f"{name} occurs {len(values)} times in {path}")
    return values[0]
