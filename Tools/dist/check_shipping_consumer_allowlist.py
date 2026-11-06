#!/usr/bin/env python3
"""Reject legacy derived-resource references from the Shipping source graph."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path


LINE_RE = re.compile(r"^(?P<path>.+?):(?P<line>\d+):(?P<text>.*)$")
LEGACY_RE = re.compile(
    r"DT_WoTAnimSequences|DA_WoTFractalCatalog|DA_WoTHUDFonts|"
    r"UWoTMeshPayloadLayout|UWoTBSPPayloadAssetUserData"
)


@dataclass(frozen=True)
class Hit:
    path: str
    line: int
    text: str


def _load_allowlist(path: Path) -> list[dict[str, str]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("version") != 1:
        raise ValueError("allowlist must have version 1")
    entries = document.get("entries")
    if not isinstance(entries, list):
        raise ValueError("allowlist entries must be a list")
    required = {"path", "symbol", "justification"}
    for entry in entries:
        if not isinstance(entry, dict) or not required.issubset(entry):
            raise ValueError("every allowlist entry needs path, symbol, and justification")
        if not entry["justification"].strip():
            raise ValueError("allowlist justifications must not be empty")
        re.compile(entry["path"])
        re.compile(entry["symbol"])
    return entries


def _parse_hits(lines: str) -> list[Hit]:
    hits: list[Hit] = []
    for raw in lines.splitlines():
        if not raw.strip():
            continue
        match = LINE_RE.match(raw)
        if not match:
            raise ValueError(f"cannot parse rg output: {raw}")
        text = match.group("text")
        stripped = text.lstrip()
        if (stripped.startswith("//") or stripped.startswith("/*")
                or stripped.startswith("*")):
            continue
        if LEGACY_RE.search(text):
            hits.append(Hit(match.group("path"), int(match.group("line")), text))
    return hits


def _in_nonshipping_guard(hit: Hit, source_root: Path) -> bool:
    path = source_root / hit.path
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError):
        return False
    stack: list[bool | None] = []
    for line in lines[:max(0, hit.line - 1)]:
        stripped = line.strip()
        if re.match(r"#\s*if\b", stripped):
            expression = re.sub(r"^#\s*if\s*", "", stripped)
            if re.fullmatch(
                    r"!\s*WOT_GUARDED_BOOTSTRAP"
                    r"|!\s*defined\s*\(\s*WOT_GUARDED_BOOTSTRAP\s*\)",
                    expression):
                stack.append(True)
            elif re.fullmatch(
                    r"WOT_GUARDED_BOOTSTRAP"
                    r"|defined\s*\(\s*WOT_GUARDED_BOOTSTRAP\s*\)",
                    expression):
                stack.append(False)
            else:
                stack.append(None)
        elif re.match(r"#\s*ifdef\s+WOT_GUARDED_BOOTSTRAP\b", stripped):
            stack.append(False)
        elif re.match(r"#\s*ifndef\s+WOT_GUARDED_BOOTSTRAP\b", stripped):
            stack.append(True)
        elif re.match(r"#\s*else\b", stripped) and stack:
            if stack[-1] is not None:
                stack[-1] = not stack[-1]
        elif re.match(r"#\s*elif\b", stripped) and stack:
            stack[-1] = None
        elif re.match(r"#\s*endif\b", stripped) and stack:
            stack.pop()
    return any(state is True for state in stack)


def check(lines: str, entries: list[dict[str, str]], *,
          source_root: Path = Path(".")) -> list[str]:
    failures: list[str] = []
    used: set[int] = set()
    for hit in _parse_hits(lines):
        if _in_nonshipping_guard(hit, source_root):
            continue
        matches = [
            index
            for index, entry in enumerate(entries)
            if re.fullmatch(entry["path"], hit.path)
            and re.search(entry["symbol"], hit.text)
        ]
        if len(matches) != 1:
            failures.append(
                f"{hit.path}:{hit.line}: Shipping legacy consumer is not uniquely "
                f"allowlisted: {hit.text.strip()}"
            )
            continue
        used.add(matches[0])
    for index, entry in enumerate(entries):
        if index not in used:
            failures.append(
                "stale allowlist row did not match any input: "
                f"{entry['path']} / {entry['symbol']}"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--allowlist", type=Path, required=True)
    args = parser.parse_args()
    try:
        entries = _load_allowlist(args.allowlist)
        failures = check(sys.stdin.read(), entries)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"shipping consumer allowlist error: {exc}", file=sys.stderr)
        return 2
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"shipping consumer allowlist passed: {len(entries)} exception row(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
