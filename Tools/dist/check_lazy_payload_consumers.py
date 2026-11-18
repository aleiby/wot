#!/usr/bin/env python3
"""Enforce the complete cache-v3 first-use reader registry."""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

FORMAT = "lazy-payload-consumers-v1"
REQUIRED_DOMAINS = {
    "texture": "wot-texture-v3",
    "sound": "wot-sound-v3",
    "music": "wot-music-v3",
    "mesh": "wot-mesh-v3",
    "animation": "wot-animation-v3",
    "bsp": "wot-bsp-v3",
    "world": "wot-world-v3",
    "font": "wot-font-v3",
    "procedural": "wot-procedural-v3",
    "registry": "wot-registry-v3",
    "localization": "wot-localization-v3",
    "class_default": "wot-class-default-v3",
}
SENSITIVE = re.compile(
    r"\b(?:OpenRead|LoadFileToArray|LoadFileToString|ReadAll|mmap|"
    r"MapViewOfFile|WoTPayload_ReadFileBounded|LoadSynchronous|TryLoad|"
    r"CreateFileReader|FArchiveFileReader(?:Generic)?)\s*\(")
ALLOWED_BOUNDARIES = {
    "Payload/WoTVerifiedBlobReader.cpp",
    "Payload/WoTCacheStore.cpp",
}


class LazyConsumerCheckFailed(Exception):
    """A lazy consumer is absent, unbounded, or bypasses verification."""


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise LazyConsumerCheckFailed(f"cannot read {path}: {exc}") from exc


def _mask(text: str) -> str:
    pattern = re.compile(
        r"//[^\n]*|/\*.*?\*/|R\"([^ ()\\\t\r\n]{0,16})\(.*?\)\1\""
        r"|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.S,
    )
    return pattern.sub(
        lambda match: re.sub(r"[^\n]", " ", match.group(0)), text)


def _function_body(masked: str, function: str) -> str:
    function_pattern = re.escape(function).replace(r"\:\:", r"\s*::\s*")
    matches = list(re.finditer(
        function_pattern + r"\s*\([^;{}]*\)\s*(?:const\s*)?(?:&&\s*)?\{",
        masked))
    if len(matches) != 1:
        raise LazyConsumerCheckFailed(
            f"registered function {function} was found {len(matches)} times")
    opening = masked.find("{", matches[0].start())
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return masked[opening + 1:index]
    raise LazyConsumerCheckFailed(f"unterminated function {function}")


def _load_registry(path: Path) -> list[dict]:
    try:
        document = json.loads(_read(path))
    except json.JSONDecodeError as exc:
        raise LazyConsumerCheckFailed(f"invalid registry JSON: {exc}") from exc
    if not isinstance(document, dict) or set(document) != {
            "format", "consumers"} or document["format"] != FORMAT:
        raise LazyConsumerCheckFailed(f"registry format must be {FORMAT}")
    fields = {
        "id", "domain", "source", "function", "schema",
        "maximum_decoded_bytes", "parser", "mutation_vector",
        "status", "blocker",
    }
    rows = document["consumers"]
    if not isinstance(rows, list):
        raise LazyConsumerCheckFailed("registry consumers must be an array")
    ids: set[str] = set()
    domains: set[str] = set()
    sites: set[tuple[str, str]] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict) or set(row) != fields:
            raise LazyConsumerCheckFailed(
                f"consumer {index} has a non-canonical shape")
        if (not all(isinstance(row[field], str) for field in fields
                    - {"maximum_decoded_bytes"})
                or not isinstance(row["maximum_decoded_bytes"], int)
                or isinstance(row["maximum_decoded_bytes"], bool)
                or row["maximum_decoded_bytes"] <= 0):
            raise LazyConsumerCheckFailed(f"consumer {index} has invalid fields")
        if row["status"] not in {"active", "blocked"}:
            raise LazyConsumerCheckFailed(
                f"consumer {row['id']} has invalid status {row['status']!r}")
        if ((row["status"] == "active" and row["blocker"])
                or (row["status"] == "blocked" and not row["blocker"])):
            raise LazyConsumerCheckFailed(
                f"consumer {row['id']} has inconsistent blocker metadata")
        if (row["status"] == "active"
                and row["function"] == "FWoTRuntimeRegistry::Read"):
            raise LazyConsumerCheckFailed(
                f"consumer {row['id']} claims the generic registry boundary "
                "instead of an actual semantic first-use consumer")
        if not row["mutation_vector"]:
            raise LazyConsumerCheckFailed(
                f"consumer {row['id']} lacks a same-size mutation vector")
        if row["id"] in ids:
            raise LazyConsumerCheckFailed(f"duplicate consumer id {row['id']}")
        site = (row["source"], row["function"])
        if site in sites:
            raise LazyConsumerCheckFailed(
                "multiple domains claim the same first-use site "
                f"{row['source']}::{row['function']}")
        if row["domain"] not in REQUIRED_DOMAINS:
            raise LazyConsumerCheckFailed(
                f"unknown consumer domain {row['domain']}")
        expected_schema = REQUIRED_DOMAINS[row["domain"]]
        if row["schema"] != expected_schema:
            raise LazyConsumerCheckFailed(
                f"schema mismatch for {row['domain']}: expected {expected_schema}")
        ids.add(row["id"])
        domains.add(row["domain"])
        sites.add(site)
    missing = sorted(set(REQUIRED_DOMAINS) - domains)
    if missing:
        raise LazyConsumerCheckFailed(
            "missing required domain(s): " + ", ".join(missing))
    return rows


def _in_nonshipping_guard(text: str, offset: int) -> bool:
    values = {
        "WOT_GUARDED_BOOTSTRAP": True,
        "UE_BUILD_SHIPPING": True,
        "WOT_WITH_BOOTSTRAP_TEST_HARNESSES": False,
        "WITH_EDITOR": False,
        "WITH_EDITORONLY_DATA": False,
    }
    # Each frame records whether its parent can be compiled in Shipping and
    # whether the original condition can be true/false. Unknown expressions
    # keep both branches possible; they can never hide a sink from inventory.
    stack: list[tuple[bool, bool, bool, bool]] = []
    possible = True

    def condition_possibilities(expression: str) -> tuple[bool, bool]:
        value = expression.strip()
        defined = re.fullmatch(r"defined\s*\(\s*(\w+)\s*\)", value)
        if defined:
            known = values.get(defined.group(1))
            return ((known is not False), (known is not True))
        token = re.fullmatch(r"(!?)\s*(\w+)", value)
        if token and token.group(2) in values:
            known = values[token.group(2)]
            if token.group(1):
                known = not known
            return known, not known
        # Handle the exact guarded-Shipping conjunction used by probes.
        conjunction = [part.strip() for part in value.split("&&")]
        if len(conjunction) > 1:
            parts = [condition_possibilities(part) for part in conjunction]
            can_true = all(part[0] for part in parts)
            can_false = any(part[1] for part in parts)
            return can_true, can_false
        return True, True

    for line in text[:offset].splitlines(keepends=True):
        stripped = line.strip()
        ifdef = re.match(r"#\s*(ifdef|ifndef)\s+(\w+)\b", stripped)
        expression = re.match(r"#\s*if\s+(.+)$", stripped)
        if ifdef:
            known = values.get(ifdef.group(2))
            can_true = known is not False
            can_false = known is not True
            if ifdef.group(1) == "ifndef":
                can_true, can_false = can_false, can_true
            parent = possible
            possible = parent and can_true
            stack.append((parent, can_true, can_false, False))
        elif expression:
            can_true, can_false = condition_possibilities(expression.group(1))
            parent = possible
            possible = parent and can_true
            stack.append((parent, can_true, can_false, False))
        elif re.match(r"#\s*else\b", stripped) and stack:
            parent, can_true, can_false, _in_else = stack[-1]
            possible = parent and can_false
            stack[-1] = (parent, can_true, can_false, True)
        elif re.match(r"#\s*endif\b", stripped) and stack:
            parent, _can_true, _can_false, _in_else = stack.pop()
            possible = parent
    return not possible


def _matching_delimiter(text: str, opening: int,
                        open_char: str, close_char: str) -> int:
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == open_char:
            depth += 1
        elif text[index] == close_char:
            depth -= 1
            if depth == 0:
                return index
    raise LazyConsumerCheckFailed("unterminated expression in consumer")


def _dead_ranges(body: str) -> list[tuple[int, int]]:
    result = []
    for match in re.finditer(r"\bif\s*\(\s*(?:false|0)\s*\)\s*\{", body):
        opening = body.find("{", match.start(), match.end())
        result.append((opening, _matching_delimiter(body, opening, "{", "}")))
    return result


def _call_arguments(body: str, name: str) -> list[tuple[int, str]]:
    result = []
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", body):
        opening = body.find("(", match.start(), match.end())
        closing = _matching_delimiter(body, opening, "(", ")")
        result.append((match.start(), body[opening + 1:closing]))
    return result


def _verified_data_reaches_parser(body: str, parser: str,
                                  required_reader: str) -> bool:
    sources = (
        required_reader,
        "FWoTRuntimeRegistry::Get().ReadObject",
        "FWoTRuntimeRegistry::Get().Read",
    )
    dead = _dead_ranges(body)

    def live(offset: int) -> bool:
        return not any(start < offset < end for start, end in dead)

    parser_calls = [(offset, args) for offset, args
                    in _call_arguments(body, parser) if live(offset)]
    if not parser_calls:
        return False
    # The registry's semantic parser is the verified reader itself.
    if parser == required_reader:
        return any(live(offset) for offset, _args
                   in _call_arguments(body, required_reader))

    tainted: set[str] = set()
    reader_seen = False
    statement_start = 0
    for terminator in re.finditer(r";", body):
        end = terminator.end()
        statement = body[statement_start:end]
        absolute = statement_start
        statement_start = end
        if not live(absolute + max(0, len(statement) // 2)):
            continue
        source_in_statement = any(source in statement for source in sources)
        if source_in_statement:
            source_offsets = [body.find(source, absolute, end) for source in sources]
            if any(offset >= 0 and live(offset) for offset in source_offsets):
                reader_seen = True
            else:
                source_in_statement = False
        assignment = re.search(
            r"\b([A-Za-z_]\w*)\s*=(?!=)\s*(.*?);\s*$", statement, re.S)
        if assignment:
            lhs, rhs = assignment.groups()
            if (source_in_statement
                    or any(re.search(rf"\b{re.escape(name)}\b", rhs)
                           for name in tainted)):
                tainted.add(lhs)
        for parser_offset, arguments in parser_calls:
            if not (absolute <= parser_offset < end) or not reader_seen:
                continue
            if (any(source in arguments for source in sources)
                    or any(re.search(rf"\b{re.escape(name)}\b", arguments)
                           for name in tainted)):
                return True
    return False


def check_tree(source_root: Path, shipping_target: Path, registry_path: Path,
               *, required_reader: str,
               require_mutation_vector: bool) -> tuple[int, int]:
    target = _read(shipping_target)
    if "WOT_GUARDED_BOOTSTRAP=1" not in target:
        raise LazyConsumerCheckFailed(
            "Shipping target does not bind WOT_GUARDED_BOOTSTRAP=1")
    rows = _load_registry(registry_path)
    registry_source = _mask(_read(source_root / "Payload/WoTRuntimeRegistry.cpp"))
    registry_read = _function_body(
        registry_source, "FWoTRuntimeRegistry::Read")
    if required_reader not in registry_read:
        raise LazyConsumerCheckFailed(
            "FWoTRuntimeRegistry::Read does not call the required reader")
    registered_locations: set[tuple[str, str]] = set()
    blocked: list[str] = []
    for row in rows:
        path = source_root / row["source"]
        masked = _mask(_read(path))
        body = _function_body(masked, row["function"])
        if not _verified_data_reaches_parser(
                body, row["parser"], required_reader):
            raise LazyConsumerCheckFailed(
                f"{row['id']} does not carry a live {required_reader} result "
                f"to declared parser {row['parser']}")
        if require_mutation_vector and not row["mutation_vector"]:
            raise LazyConsumerCheckFailed(
                f"{row['id']} has no mutation vector")
        registered_locations.add((row["source"], row["function"]))
        if row["status"] == "blocked":
            blocked.append(f"{row['domain']}: {row['blocker']}")

    bypasses: list[str] = []
    for path in sorted(source_root.rglob("*")):
        if path.suffix not in {".h", ".cpp"}:
            continue
        relative = path.relative_to(source_root).as_posix()
        if relative in ALLOWED_BOUNDARIES or "/Tests/" in f"/{relative}/":
            continue
        text = _read(path)
        masked = _mask(text)
        for match in SENSITIVE.finditer(masked):
            if _in_nonshipping_guard(text, match.start()):
                continue
            line = text.count("\n", 0, match.start()) + 1
            bypasses.append(f"{relative}:{line}")
    # A registered implementation may contain its required lower-boundary call;
    # SENSITIVE intentionally does not include ReadVerifiedCacheBlob itself.
    problems = []
    if bypasses:
        problems.append("lazy-reader bypass(es): " + ", ".join(bypasses))
    if blocked:
        problems.append("blocked consumer(s): " + "; ".join(blocked))
    if problems:
        raise LazyConsumerCheckFailed("; ".join(problems))
    return len(rows), len(registered_locations)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--shipping-target", required=True, type=Path)
    parser.add_argument("--registry", required=True, type=Path)
    parser.add_argument("--required-reader", required=True)
    parser.add_argument(
        "--require-mutation-vector-per-consumer", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        consumers, sites = check_tree(
            args.source_root, args.shipping_target, args.registry,
            required_reader=args.required_reader,
            require_mutation_vector=args.require_mutation_vector_per_consumer)
    except LazyConsumerCheckFailed as exc:
        print(f"lazy payload consumer check failed: {exc}", file=sys.stderr)
        return 1
    print(
        f"lazy payload consumer check passed: {consumers} consumers, "
        f"{sites} verified first-use sites")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
