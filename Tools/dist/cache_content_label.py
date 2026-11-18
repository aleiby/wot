#!/usr/bin/env python3
"""The cache content label a production binary records in its cache.

A cache build records the label of the cache content revision compiled into
the binary that built it (``WoTCacheContentRevision`` in
``Source/WoTRetailReader/Private/CacheBuilder.cpp``). The label is stored in the
manifest field ``production_descriptor_sha256``, whose name is kept for format
compatibility. Revision 1 is the digest the first public release recorded, so
its caches stay valid byte for byte. Every later revision ``n`` is labelled
``SHA-256(b"WOT-CACHE-CONTENT-IDENTITY-V1" + NUL + u64-little-endian(n))``.
"""
from __future__ import annotations

import hashlib
import re
import struct
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parents[2]
CACHE_BUILDER = ROOT / "Source/WoTRetailReader/Private/CacheBuilder.cpp"
DOMAIN = b"WOT-CACHE-CONTENT-IDENTITY-V1\x00"
REVISION_1_LABEL = (
    "1a7e19b367523c11d9ad6c7fda42a39472a68d4b8c52233b6b2d568ca3daa58e")
MAX_REVISION = 2 ** 64 - 1

_REVISION_PATTERN = re.compile(
    r"constexpr\s+uint32\s+WoTCacheContentRevision\s*=\s*(\d+)\s*;")
_REVISION_1_LABEL_PATTERN = re.compile(
    r"constexpr\s+TCHAR\s+WoTCacheRevision1Label\[\]\s*=\s*"
    r"TEXT\(\s*\"([0-9a-f]{64})\"\s*\)\s*;")


def _checked_revision(revision: object) -> int:
    if isinstance(revision, bool) or not isinstance(revision, int):
        raise ValueError(f"a cache content revision must be an int, not {revision!r}")
    if revision < 1 or revision > MAX_REVISION:
        raise ValueError(
            f"a cache content revision must be in 1..2**64-1, not {revision}")
    return revision


def _label_for(revision: int) -> str:
    if revision == 1:
        return REVISION_1_LABEL
    return hashlib.sha256(DOMAIN + struct.pack("<Q", revision)).hexdigest()


def label(revision: int) -> str:
    """Return the lowercase hex content label of ``revision``."""
    return _label_for(_checked_revision(revision))


def _single_definition(pattern: "re.Pattern[str]", path: Path, name: str) -> str:
    text = Path(path).read_text(encoding="utf-8")
    matches = pattern.findall(text)
    if len(matches) != 1:
        raise ValueError(
            f"{Path(path).name} must define {name} exactly once; "
            f"found {len(matches)} definitions")
    return matches[0]


def compiled_revision(path: Path = CACHE_BUILDER) -> int:
    """Read ``WoTCacheContentRevision`` from the C++ source."""
    return _checked_revision(int(_single_definition(
        _REVISION_PATTERN, path, "WoTCacheContentRevision")))


def compiled_revision_1_label(path: Path = CACHE_BUILDER) -> str:
    """Read the revision-1 label literal ``WoTCacheRevision1Label``."""
    return _single_definition(
        _REVISION_1_LABEL_PATTERN, path, "WoTCacheRevision1Label")


def revision_for_label(label: str, max_revision: int) -> Optional[int]:
    """Return the revision in ``1..max_revision`` whose label is ``label``."""
    if not isinstance(label, str):
        raise ValueError(f"a cache content label must be a str, not {label!r}")
    for revision in range(1, _checked_revision(max_revision) + 1):
        if _label_for(revision) == label:
            return revision
    return None
