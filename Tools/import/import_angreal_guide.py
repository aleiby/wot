#!/usr/bin/env python3
"""Parse the exported script tree for the angreal field guide.

Parse half of the guide importer (docs/plans/2026-11-06-onboarding-aids.md,
Task 1). Walks the private reference export `exports/script/<Package>/*.uc`,
records each class header (`class X expands Y`) with its defaultproperties
scalars and `Name(index)=` arrays, and resolves inherited values through the
parent chain the way the original property system did. Later tasks derive the
facts CSV and the guide data asset from these results.

The angreal census (Tools/test/angreal/angreal_census.py) walks the same tree
with the same conventions (latin-1 reads, `class X expands|extends Y`
headers, lowercase class keys), but its parsing is census-specific — it
resolves the reference root at import time and only extracts spawn-pool
properties — so this module carries the standalone parser the plan specifies
instead of importing it.

Standalone smoke run (no Unreal):
  WOT_REFERENCE_ROOT=... python3 Tools/import/import_angreal_guide.py
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import (  # noqa: E402
    ConfigurationError, PathLike, exports_dir, require_path)

CLASS_RE = re.compile(
    r"^\s*class\s+(\w+)\s+(?:expands|extends)\s+(\w+)", re.I | re.M)
PROP_RE = re.compile(r"^([A-Za-z_]\w*)(?:\((\d+)\))?=(.*)$")


@dataclass
class UcClass:
    """One exported class: header identity plus its defaultproperties."""

    name: str
    package: str
    parent: str
    props: Dict[str, str] = field(default_factory=dict)
    array_props: Dict[str, List[str]] = field(default_factory=dict)


def default_script_root(explicit: Optional[PathLike] = None) -> Path:
    """The exported-script root, resolved through wot_config precedence."""
    return require_path(exports_dir(explicit) / "script",
                        "exported script reference directory", directory=True)


def reference_root_available() -> bool:
    """True when the exported script tree is readable; gated tests skip otherwise."""
    try:
        return default_script_root().is_dir()
    except ConfigurationError:
        return False


def _unquote(value: str) -> str:
    # Whitespace around the value is syntax; a quoted value keeps its interior
    # exactly. The exported defaultproperties use no escape convention, so the
    # quotes are simply stripped.
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1]
    return value


def parse_script_classes(script_root: PathLike) -> Dict[str, UcClass]:
    """Map lowercase class name -> UcClass for every .uc under the root.

    Packages are the first directory level under the root
    (`exports/script/<Package>/`). Files are read as latin-1, the exporter's
    encoding. A file without a `class X expands Y` header carries no exported
    class and is skipped. Scalar duplicates keep the last value in file order;
    sparse `Name(index)=` arrays keep empty strings in the gaps. Files are
    visited in sorted path order, so the result is deterministic.
    """
    root = Path(script_root)
    classes: Dict[str, UcClass] = {}
    for uc in sorted(root.rglob("*.uc")):
        text = uc.read_text(encoding="latin-1", errors="replace")
        header = CLASS_RE.search(text)
        if not header:
            continue
        relative = uc.relative_to(root)
        parsed = UcClass(
            name=header.group(1),
            package=relative.parts[0] if len(relative.parts) > 1 else "",
            parent=header.group(2))
        in_defaults = False
        in_block = False
        for raw in text.splitlines():
            line = raw.strip()
            if not in_defaults:
                in_defaults = line.lower() == "defaultproperties"
                continue
            if not in_block:
                in_block = line == "{"
                continue
            if line == "}":
                break
            match = PROP_RE.match(line)
            if not match:
                continue
            prop, index, value = match.groups()
            if index is None:
                parsed.props[prop] = _unquote(value)
            else:
                entries = parsed.array_props.setdefault(prop, [])
                position = int(index)
                while len(entries) <= position:
                    entries.append("")
                entries[position] = _unquote(value)
        classes[parsed.name.lower()] = parsed
    return classes


def ancestors(classes: Dict[str, UcClass], name: str) -> List[str]:
    """Lowercase inheritance chain: the class itself, then each parent.

    The walk stops at the first class missing from the map (engine packages
    ship stripped, so every chain ends where the export does), at a cycle, or
    immediately — an empty list — when the class itself is unknown.
    """
    chain: List[str] = []
    current = name.lower()
    while current in classes and current not in chain:
        chain.append(current)
        current = classes[current].parent.lower()
    return chain


def resolved_prop(classes: Dict[str, UcClass], name: str,
                  prop: str) -> Optional[str]:
    """The nearest defaultproperties value for `prop` along the parent chain.

    Property names compare case-insensitively, matching the original property
    system; None when no class in the chain sets the property.
    """
    wanted = prop.lower()
    for link in ancestors(classes, name):
        for key, value in sorted(classes[link].props.items()):
            if key.lower() == wanted:
                return value
    return None


def resolved_bool(classes: Dict[str, UcClass], name: str, prop: str) -> bool:
    """resolved_prop read as an UnrealScript bool: only a literal True is True."""
    value = resolved_prop(classes, name, prop)
    return value is not None and value.strip().lower() == "true"


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="parse the exported script tree and report what it holds")
    parser.add_argument("--script-root",
                        help="exported script root (default: exports/script "
                             "under the configured reference root)")
    args = parser.parse_args(argv)
    root = Path(args.script_root) if args.script_root else default_script_root()
    classes = parse_script_classes(root)
    packages = sorted({cls.package for cls in classes.values()})
    print("parsed %d classes across %d packages from %s"
          % (len(classes), len(packages), root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
