#!/usr/bin/env python3
"""Audit that a WoT binary resolves the adapter from one fixed static archive."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Sequence


class LinkCheckError(ValueError):
    pass


ADAPTER_SYMBOLS = (
    "wot_innoextract_open",
    "wot_innoextract_entry_count",
    "wot_innoextract_entry_at",
    "wot_innoextract_extract_entry",
    "wot_innoextract_description",
    "wot_innoextract_close",
    "wot_innoextract_last_error",
)
SHELL_SYMBOLS = frozenset(
    ("system", "popen", "_popen", "CreateProcessA", "CreateProcessW",
     "ShellExecuteA", "ShellExecuteW", "WinExec")
)
# Each entry names a concrete interpreter as it would appear in a command string.
# A bare "powershell" was the one exception and matched an identifier rather than
# a command: Boost.WinAPI's FACILITY_POWERSHELL_ enumerator name reaches the
# archive's debug info, so this rejected an otherwise clean Win64 build. The
# capability itself is covered by SHELL_SYMBOLS, which a substring cannot fake.
SHELL_STRINGS = (
    b"/bin/sh", b"cmd.exe", b"powershell.exe", b"pwsh.exe", b"/usr/bin/env sh"
)


def _normalized(value: str) -> str:
    return value.replace("\\", "/").lower()


def _archive_references(map_text: str) -> set[str]:
    # Tokenize once, stopping before a Mach-O `(member.o)` suffix, then use
    # string predicates for the archive spelling. This remains linear for large
    # link maps without relying on the atomic regex groups added in Python 3.11.
    tokens = re.findall(r"[^\s\[\]()]+", map_text)
    return {
        _normalized(token)
        for token in tokens
        if "innoextract" in token.lower()
        and token.lower().endswith((".a", ".lib"))
    }


def _windows_library_members(map_text: str) -> set[str]:
    """Library stems that an MSVC /MAP attributes object code to.

    MSVC spells a library member as `<library-stem>:<object>.obj` -- no path and
    no `.lib` suffix anywhere in the file -- so the archive cannot be identified
    by path from a Windows map the way it can from a Mach-O one.
    """
    pattern = re.compile(r"(?<![\w.+-])([\w.+-]+):([\w.+-]+\.obj)\b", re.IGNORECASE)
    return {match.group(1).lower() for match in pattern.finditer(map_text)}


def _mac_expected_object_indices(map_text: str, expected_name: str) -> set[str]:
    indices: set[str] = set()
    pattern = re.compile(r"^\[\s*(\d+)\]\s+(.+)$")
    for line in map_text.splitlines():
        match = pattern.match(line.strip())
        if match and expected_name in _normalized(match.group(2)):
            indices.add(match.group(1))
    return indices


def _undefined_symbols(archive: Path, platform: str, nm_tool: str | None) -> set[str]:
    # dumpbin last, and by name rather than by position: it is the only one of
    # the three that ships with a stock MSVC install, so a Windows box with no
    # LLVM would otherwise have no way to run this audit at all.
    selected = nm_tool or shutil.which("llvm-nm") or shutil.which("nm") or shutil.which("dumpbin")
    if selected is None:
        raise LinkCheckError(
            "no llvm-nm, nm or dumpbin is available for the closed symbol audit"
        )
    name = Path(selected).name.lower()
    style = "llvm" if "llvm-nm" in name else "dumpbin" if "dumpbin" in name else "nm"

    command = {
        "llvm": [selected, "--undefined-only", "--just-symbol-name", str(archive)],
        "nm": [selected, "-u", str(archive)],
        "dumpbin": [selected, "/NOLOGO", "/SYMBOLS", str(archive)],
    }[style]
    try:
        completed = subprocess.run(
            command, check=False, capture_output=True, text=True, encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        raise LinkCheckError(f"could not execute undefined-symbol tool {selected}: {exc}") from exc
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip() or "no diagnostic"
        raise LinkCheckError(
            f"undefined-symbol tool failed with exit {completed.returncode}: {diagnostic}"
        )

    result: set[str] = set()
    for line in completed.stdout.splitlines():
        if style == "dumpbin":
            # 011 00000000 UNDEF  notype ()  External  | ?f@@YAXXZ (void __cdecl f(void))
            # The name follows the bar; anything after it is the demangling.
            head, separator, tail = line.partition("|")
            if not separator or "UNDEF" not in head.split():
                continue
            fields = tail.split()[:1]
        else:
            fields = line.strip().split()
        if not fields:
            continue
        symbol = fields[-1]
        if platform == "Win64" and symbol.startswith("__imp_"):
            symbol = symbol[len("__imp_"):]
        if platform == "Mac" and symbol.startswith("_"):
            symbol = symbol[1:]
        result.add(symbol)
    return result


def check_link(
    *,
    platform: str,
    binary: Path,
    link_map: Path,
    expected_archive: Path,
    forbid_runtime_extractor: bool,
    forbid_shell: bool,
    nm_tool: str | None = None,
) -> None:
    if platform not in {"Mac", "Win64"}:
        raise LinkCheckError("platform must be Mac or Win64")
    for path, label in (
        (binary, "binary"),
        (link_map, "link map"),
        (expected_archive, "expected archive"),
    ):
        if not path.is_file():
            raise LinkCheckError(f"{label} does not exist: {path}")

    map_text = link_map.read_text(encoding="utf-8", errors="replace")
    expected_name = expected_archive.name.lower()
    expected_paths = {
        _normalized(str(expected_archive)),
        _normalized(str(expected_archive.resolve())),
    }
    references = _archive_references(map_text)
    expected_references = references & expected_paths
    # A Windows map may name the archive either way: UE emits the full path in
    # some link maps, while MSVC's own /MAP carries only the member spelling. The
    # path is the stronger evidence, so it is still preferred when present; when
    # it is absent the stem is all a Windows map can offer, and the archive's
    # identity is bound by the vendor manifest's hash rather than by this file.
    member_stems = _windows_library_members(map_text) if platform == "Win64" else set()
    expected_stem = Path(expected_archive.name).stem.lower()
    if not expected_references and expected_stem not in member_stems:
        raise LinkCheckError(
            "link map does not resolve the exact fixed innoextract archive path"
        )
    unexpected = {
        reference
        for reference in references
        if "innoextract" in Path(reference).name and reference not in expected_references
    }
    unexpected |= {
        stem for stem in member_stems
        if "innoextract" in stem and stem != expected_stem
    }
    if unexpected:
        raise LinkCheckError(
            "link map contains an alternate innoextract archive: " + ", ".join(sorted(unexpected))
        )

    for symbol in ADAPTER_SYMBOLS:
        if symbol not in map_text:
            raise LinkCheckError(f"link map is missing adapter symbol {symbol}")

    if platform == "Mac":
        indices = _mac_expected_object_indices(map_text, expected_name)
        if not indices:
            raise LinkCheckError("Mac link map has no object member from the expected archive")

    binary_bytes = binary.read_bytes()
    archive_bytes = expected_archive.read_bytes()
    if forbid_runtime_extractor:
        lowered = binary_bytes.lower()
        dynamic_patterns = (
            b"innoextract.dylib",
            b"libinnoextract.dylib",
            b"innoextract.dll",
            "innoextract.dll".encode("utf-16le"),
            b"innoextract.exe",
            "innoextract.exe".encode("utf-16le"),
        )
        if any(pattern in lowered for pattern in dynamic_patterns):
            raise LinkCheckError("binary references a runtime innoextract executable/library")

    if forbid_shell:
        imported = _undefined_symbols(expected_archive, platform, nm_tool)
        forbidden_imports = imported & SHELL_SYMBOLS
        if forbidden_imports:
            raise LinkCheckError("innoextract archive contribution imports a shell/process API")
        lowered_archive = archive_bytes.lower()
        if any(value.lower() in lowered_archive for value in SHELL_STRINGS):
            raise LinkCheckError("innoextract archive embeds a shell command string")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("Mac", "Win64"), required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--link-map", type=Path, required=True)
    parser.add_argument("--expected-archive", type=Path, required=True)
    parser.add_argument("--forbid-runtime-extractor", action="store_true")
    parser.add_argument("--forbid-shell", action="store_true")
    parser.add_argument("--nm-tool")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        check_link(
            platform=arguments.platform,
            binary=arguments.binary,
            link_map=arguments.link_map,
            expected_archive=arguments.expected_archive,
            forbid_runtime_extractor=arguments.forbid_runtime_extractor,
            forbid_shell=arguments.forbid_shell,
            nm_tool=arguments.nm_tool,
        )
    except (OSError, LinkCheckError) as exc:
        print(f"check_innoextract_link: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
