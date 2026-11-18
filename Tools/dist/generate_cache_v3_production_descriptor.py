#!/usr/bin/env python3
"""Generate the mechanical Shipping consumer/importer closure descriptor."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable

FORMAT = "cache-v3-production-descriptor-v1"
IMPORTER_FORMAT = "cache-v3-importer-outputs-v1"
IMPORTER_FIELDS = {
    "audit_rule", "domain", "importer", "max_decoded_size", "output_schema",
    "runtime_consumer",
}
REQUIRED_DOMAINS = {
    "texture", "sound", "mesh", "animation", "bsp", "world", "font",
    "front_end_world", "procedural", "registry", "localization", "class_default",
}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
STATIC_RE = re.compile(
    r'\b([A-Za-z_][A-Za-z0-9_:]*)::(StaticClass|StaticStruct|Get|Load|Find)\b')
TYPED_LOAD_RE = re.compile(
    r'\b(LoadObject|LoadClass|FindObject|FindObjectFast|GetDefault|GetMutableDefault|'
    r'ConstructorHelpers::FObjectFinder|ConstructorHelpers::FClassFinder)\s*'
    r'<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*>')
UNTYPED_LOAD_RE = re.compile(
    r'\b(StaticLoadObject|StaticLoadClass|StaticFindObject|ResolveObject)\s*\(')
SOFT_PREFIXES = ("/Game/", "/Script/", "/Engine/")
CONFIG_SUFFIXES = {".ini", ".cfg", ".json"}
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp"}
MODULE_CALL_RE = re.compile(
    r'ExtraModuleNames\s*\.\s*Add(?:Range)?\s*\((.*?)\)\s*;', re.DOTALL)
DEPENDENCY_CALL_RE = re.compile(
    r'(?:Public|Private)DependencyModuleNames\s*\.\s*Add(?:Range)?\s*'
    r'\((.*?)\)\s*;', re.DOTALL)
CS_STRING_RE = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')
SYMBOL_RE = re.compile(
    r'\b(?:class|struct)\s+(?:[A-Za-z_][A-Za-z0-9_]*_API\s+)?'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:final\s*)?(?=[:{])')
COOK_OUTPUT_FIELDS = {
    "audit_rule", "importer", "logical_id", "runtime_consumer", "schema",
}


class DescriptorError(ValueError):
    pass


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("utf-8")


def sha256_bytes(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


# Labels are what the descriptor RECORDS, so they must not depend on the
# invocation directory: the same tree must produce the same bytes from any
# cwd. The repository root is derived from this tool's own location
# (Tools/dist/ -> two parents up); a path outside the repository stays
# absolute, which makes a foreign input loudly visible in the descriptor.
_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


def _label(path: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(_REPOSITORY_ROOT).as_posix()
    except ValueError:
        return resolved.as_posix()


def _read(path: Path, what: str) -> bytes:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise DescriptorError(f"cannot read {what} {path}: {exc}") from exc
    # CRLF -> LF before anything hashes this. The descriptor pins the identity of
    # repository SOURCE, and a line ending is a checkout artifact, not content:
    # Windows checks these files out CRLF, so without normalising, every one of
    # the 100+ source hashes differs from the committed descriptor and the
    # production-closure gate can never pass off macOS. Verified equivalent, not
    # merely plausible: the committed descriptor's entry for
    # Source/WoT/AI/WoTAIUtil.cpp is 3ed78440..., which IS the LF hash, while the
    # CRLF working copy hashes a4bed59b... So normalising reproduces the existing
    # committed values exactly and no regeneration is required.
    return raw.replace(b"\r\n", b"\n")


def load_importer_spec(path: Path) -> tuple[dict[str, Any], bytes]:
    raw = _read(path, "importer spec")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise DescriptorError(f"importer spec {path} is not UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict) or set(value) != {"format", "importers"}:
        raise DescriptorError("importer spec must contain exactly format and importers")
    if value["format"] != IMPORTER_FORMAT or not isinstance(value["importers"], list):
        raise DescriptorError(f"importer spec format must be {IMPORTER_FORMAT!r}")
    seen_importers: set[str] = set()
    seen_schemas: set[str] = set()
    seen_domains: set[str] = set()
    for index, entry in enumerate(value["importers"]):
        if not isinstance(entry, dict) or set(entry) != IMPORTER_FIELDS:
            raise DescriptorError(f"importers[{index}] has a non-closed field set")
        for field in IMPORTER_FIELDS - {"max_decoded_size"}:
            if not isinstance(entry[field], str) or not entry[field]:
                raise DescriptorError(f"importers[{index}].{field} must be non-empty")
        limit = entry["max_decoded_size"]
        if isinstance(limit, bool) or not isinstance(limit, int) or limit <= 0:
            raise DescriptorError(
                f"importers[{index}].max_decoded_size must be a positive integer")
        for field, seen in (("importer", seen_importers),
                            ("output_schema", seen_schemas), ("domain", seen_domains)):
            item = entry[field]
            if item in seen:
                raise DescriptorError(f"importers[{index}].{field} duplicates {item!r}")
            seen.add(item)
    if seen_domains != REQUIRED_DOMAINS:
        missing = ", ".join(sorted(REQUIRED_DOMAINS - seen_domains)) or "none"
        extra = ", ".join(sorted(seen_domains - REQUIRED_DOMAINS)) or "none"
        raise DescriptorError(
            f"importer domains are not the closed production cache-v3 set; "
            f"missing: {missing}; extra: {extra}")
    return value, raw


def _file_inventory(paths: Iterable[Path]) -> dict[str, object]:
    """Digest of WHICH files exist, not of their contents.

    Content hashes belong to git. What this descriptor must notice is a file
    entering or leaving the walked closure, because that changes which
    consumers exist.
    """
    labels = sorted(_label(path) for path in paths)
    joined = "\n".join(labels).encode("utf-8")
    return {"count": len(labels), "sha256": sha256_bytes(joined)}


def _file_records(paths: Iterable[Path]) -> list[dict[str, str]]:
    records = []
    for path in sorted(paths, key=lambda item: item.as_posix()):
        raw = _read(path, "graph input")
        records.append({"path": _label(path), "sha256": sha256_bytes(raw)})
    return records


def _strip_comments(text: str) -> str:
    """Blank C/C++/C# comments while preserving strings and line structure."""
    result = list(text)
    index = 0
    quote = ""
    while index < len(text):
        character = text[index]
        if quote:
            if character == "\\":
                index += 2
                continue
            if character == quote:
                quote = ""
            index += 1
            continue
        if character in ('"', "'"):
            quote = character
            index += 1
            continue
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            end = len(text) if end < 0 else end
            for offset in range(index, end):
                result[offset] = " "
            index = end
            continue
        if text.startswith("/*", index):
            end_marker = text.find("*/", index + 2)
            end = len(text) if end_marker < 0 else end_marker + 2
            for offset in range(index, end):
                if result[offset] not in "\r\n":
                    result[offset] = " "
            index = end
            continue
        index += 1
    return "".join(result)


def _decode_cpp_string(body: str) -> str:
    decoded: list[str] = []
    index = 0
    escapes = {
        "'": "'", '"': '"', "?": "?", "\\": "\\", "a": "\a",
        "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t",
        "v": "\v",
    }
    while index < len(body):
        character = body[index]
        if character != "\\" or index + 1 >= len(body):
            decoded.append(character)
            index += 1
            continue
        escape = body[index + 1]
        if escape in escapes:
            decoded.append(escapes[escape])
            index += 2
            continue
        if escape in "xXuU":
            widths = {"u": 4, "U": 8}
            if escape in widths:
                digits = body[index + 2:index + 2 + widths[escape]]
            else:
                match = re.match(r"[0-9a-fA-F]+", body[index + 2:])
                digits = match.group(0) if match else ""
            if digits:
                decoded.append(chr(int(digits, 16)))
                index += 2 + len(digits)
                continue
        if escape in "01234567":
            match = re.match(r"[0-7]{1,3}", body[index + 1:])
            assert match is not None
            decoded.append(chr(int(match.group(0), 8)))
            index += 1 + len(match.group(0))
            continue
        if escape == "\n":
            index += 2
            continue
        decoded.append(escape)
        index += 2
    return "".join(decoded)


def _cpp_string_literals(text: str) -> list[str]:
    """Tokenize normal literals and concatenate C++-adjacent string tokens."""
    stripped = _strip_comments(text)
    tokens: list[tuple[int, int, str]] = []
    index = 0
    while index < len(stripped):
        if stripped[index] == "'":
            index += 1
            while index < len(stripped):
                if stripped[index] == "\\":
                    index += 2
                elif stripped[index] == "'":
                    index += 1
                    break
                else:
                    index += 1
            continue
        if stripped[index] != '"':
            index += 1
            continue
        start = index
        index += 1
        body: list[str] = []
        while index < len(stripped):
            if stripped[index] == "\\" and index + 1 < len(stripped):
                body.extend((stripped[index], stripped[index + 1]))
                index += 2
            elif stripped[index] == '"':
                index += 1
                break
            else:
                body.append(stripped[index])
                index += 1
        tokens.append((start, index, _decode_cpp_string("".join(body))))
    literals: list[str] = []
    current = ""
    previous_end = -1
    for start, end, value in tokens:
        if previous_end >= 0 and not stripped[previous_end:start].strip():
            current += value
        else:
            if previous_end >= 0:
                literals.append(current)
            current = value
        previous_end = end
    if previous_end >= 0:
        literals.append(current)
    return literals


def _quoted_call_values(text: str, expression: re.Pattern[str]) -> list[str]:
    stripped = _strip_comments(text)
    values: list[str] = []
    for call in expression.finditer(stripped):
        values.extend(match.group(1) for match in CS_STRING_RE.finditer(call.group(1)))
    return values


def _shipping_module_closure(shipping_target: Path,
                             source_root: Path) -> list[tuple[str, Path, Path]]:
    target_text = _read(shipping_target, "Shipping target").decode("utf-8", "replace")
    roots = _quoted_call_values(target_text, MODULE_CALL_RE)
    if not roots:
        raise DescriptorError(f"Shipping target {shipping_target} names no modules")
    source_base = source_root.parent
    pending = list(dict.fromkeys(roots))
    visited: set[str] = set()
    modules: list[tuple[str, Path, Path]] = []
    while pending:
        module = pending.pop(0)
        if module in visited or "tests" in module.lower():
            continue
        visited.add(module)
        module_root = source_base / module
        build_file = module_root / f"{module}.Build.cs"
        if not module_root.is_dir() or not build_file.is_file():
            continue
        modules.append((module, module_root, build_file))
        build_text = _read(build_file, "module rules").decode("utf-8", "replace")
        for dependency in _quoted_call_values(build_text, DEPENDENCY_CALL_RE):
            if dependency not in visited and "tests" not in dependency.lower():
                pending.append(dependency)
    missing_roots = sorted(set(roots) - {name for name, _root, _build in modules})
    if missing_roots:
        raise DescriptorError(
            "Shipping target modules are absent below Source/: " + ", ".join(missing_roots))
    return sorted(modules, key=lambda entry: entry[0])


def _source_graph(modules: list[tuple[str, Path, Path]]) -> dict[str, Any]:
    files_with_modules: list[tuple[Path, str]] = []
    for module, module_root, _build_file in modules:
        files_with_modules.extend(
            (path, module) for path in module_root.rglob("*")
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES)
    files_with_modules.sort(key=lambda entry: entry[0].as_posix())
    files = [path for path, _module in files_with_modules]
    includes: set[tuple[str, str]] = set()
    statics: set[tuple[str, str]] = set()
    soft: set[tuple[str, str]] = set()
    symbols: set[tuple[str, str, str]] = set()
    for path, module in files_with_modules:
        text = _read(path, "source file").decode("utf-8", "replace")
        code = _strip_comments(text)
        consumer = _label(path)
        includes.update((consumer, match.group(1)) for match in INCLUDE_RE.finditer(code))
        statics.update((consumer, f"{match.group(1)}::{match.group(2)}")
                       for match in STATIC_RE.finditer(code))
        statics.update((consumer, f"{match.group(1)}<{match.group(2)}>")
                       for match in TYPED_LOAD_RE.finditer(code))
        statics.update((consumer, match.group(1))
                       for match in UNTYPED_LOAD_RE.finditer(code))
        symbols.update((match.group(1), module, consumer)
                       for match in SYMBOL_RE.finditer(code))
        for literal in _cpp_string_literals(text):
            if literal.startswith(SOFT_PREFIXES):
                soft.add((consumer, literal))
    pair_records = lambda pairs: [  # noqa: E731 - compact stable projection
        {"consumer": consumer, "reference": reference}
        for consumer, reference in sorted(pairs)]
    return {
        # NOT a per-file hash array. Hashing every source file made the
        # descriptor stale on any edit — a comment change or an unrelated
        # new .cpp forced a regeneration, which put a release-integrity
        # gate in the path of ordinary development. The closure this file
        # exists to pin lives in the reference sets below; the inventory
        # digest still detects files appearing or disappearing.
        "file_inventory": _file_inventory(files),
        "includes": pair_records(includes),
        "modules": [
            {"build_file": _label(build_file), "name": module,
             "sha256": sha256_bytes(_read(build_file, "module rules"))}
            for module, _root, build_file in modules],
        "soft_references": pair_records(soft),
        "static_references": pair_records(statics),
        "symbols": [
            {"module": module, "path": path, "symbol": symbol}
            for symbol, module, path in sorted(symbols)],
    }


def _config_graph(config_root: Path) -> dict[str, Any]:
    files = sorted((path for path in config_root.rglob("*")
                    if path.is_file() and path.suffix.lower() in CONFIG_SUFFIXES),
                   key=lambda item: item.as_posix()) if config_root.exists() else []
    roots: set[tuple[str, str, str]] = set()
    primary_rules: set[tuple[str, str]] = set()
    for path in files:
        text = _read(path, "config file").decode("utf-8", "replace")
        section = ""
        for raw_line in text.splitlines():
            line = raw_line.strip()
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                continue
            if not line or line.startswith((";", "#")) or "=" not in line:
                continue
            key, value = (part.strip() for part in line.split("=", 1))
            lowered = f"{section}.{key}".lower()
            if (any(token in lowered for token in
                    ("path", "dir", "map", "asset", "package", "cook"))
                    or any(prefix in value for prefix in SOFT_PREFIXES)):
                roots.add((_label(path), f"{section}.{key}", value))
            if "primaryasset" in lowered or "assetmanager" in lowered:
                primary_rules.add((_label(path), line))
    return {
        # NOT a per-file hash array. Hashing every source file made the
        # descriptor stale on any edit — a comment change or an unrelated
        # new .cpp forced a regeneration, which put a release-integrity
        # gate in the path of ordinary development. The closure this file
        # exists to pin lives in the reference sets below; the inventory
        # digest still detects files appearing or disappearing.
        "file_inventory": _file_inventory(files),
        "primary_asset_rules": [
            {"consumer": path, "rule": rule} for path, rule in sorted(primary_rules)],
        "roots": [
            {"consumer": path, "key": key, "reference": value}
            for path, key, value in sorted(roots)],
    }


def _cook_graph(path: Path | None) -> dict[str, Any]:
    if path is None:
        return {"status": "not_yet_cooked"}
    raw = _read(path, "cook reference graph")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise DescriptorError(f"cook reference graph {path} is invalid: {exc}") from exc
    if (not isinstance(value, dict) or set(value) != {"outputs", "references"}
            or not isinstance(value["outputs"], list)
            or not isinstance(value["references"], list)):
        raise DescriptorError(
            "cook reference graph must contain exactly outputs and references arrays")
    outputs = []
    for index, entry in enumerate(value["outputs"]):
        if not isinstance(entry, dict) or set(entry) != COOK_OUTPUT_FIELDS:
            raise DescriptorError(
                f"cook reference graph outputs[{index}] has a non-closed field set")
        if not all(isinstance(entry[field], str) and entry[field]
                   for field in COOK_OUTPUT_FIELDS):
            raise DescriptorError(f"cook reference graph outputs[{index}] is empty")
        outputs.append(dict(entry))
    outputs.sort(key=lambda entry: (
        entry["logical_id"], entry["schema"], entry["importer"],
        entry["runtime_consumer"], entry["audit_rule"]))
    references = []
    for index, entry in enumerate(value["references"]):
        if not isinstance(entry, dict) or set(entry) != {"consumer", "reference"}:
            raise DescriptorError(
                f"cook reference graph references[{index}] needs consumer and reference")
        if not all(isinstance(entry[field], str) and entry[field]
                   for field in ("consumer", "reference")):
            raise DescriptorError(f"cook reference graph references[{index}] is empty")
        references.append({"consumer": entry["consumer"], "reference": entry["reference"]})
    references.sort(key=lambda entry: (entry["consumer"], entry["reference"]))
    return {"outputs": outputs, "path": _label(path), "references": references,
            "sha256": sha256_bytes(raw), "status": "cooked"}


def build_descriptor(importer_spec: Path, shipping_target: Path, source_root: Path,
                     config_root: Path, cook_reference_graph: Path | None = None) -> dict[str, Any]:
    spec, spec_raw = load_importer_spec(importer_spec)
    target_raw = _read(shipping_target, "Shipping target")
    modules = _shipping_module_closure(shipping_target, source_root)
    graph = {
        "config": _config_graph(config_root),
        "cook_reference_graph": _cook_graph(cook_reference_graph),
        "shipping_target": {"path": _label(shipping_target),
                            "sha256": sha256_bytes(target_raw)},
        "source": _source_graph(modules),
    }
    outputs = []
    for entry in spec["importers"]:
        output = dict(entry)
        output["logical_id"] = f"cache/{entry['domain']}"
        output["schema"] = output.pop("output_schema")
        outputs.append(output)
    outputs.sort(key=lambda entry: entry["schema"])
    return {
        "format": FORMAT,
        "graph": graph,
        "graph_sha256": sha256_bytes(canonical_bytes(graph)),
        "importer_outputs": outputs,
        "importer_spec": {"path": _label(importer_spec),
                          "sha256": sha256_bytes(spec_raw)},
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--importer-spec", required=True, type=Path)
    parser.add_argument("--shipping-target", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--config-root", required=True, type=Path)
    parser.add_argument("--cook-reference-graph", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        expected = canonical_bytes(build_descriptor(
            args.importer_spec, args.shipping_target, args.source_root,
            args.config_root, args.cook_reference_graph))
        if args.check:
            actual = _read(args.output, "checked descriptor")
            if actual != expected:
                print(f"cache-v3 production descriptor is stale: {args.output}",
                      file=sys.stderr)
                return 1
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(expected)
    except (DescriptorError, OSError) as exc:
        print(f"cache-v3 descriptor: {exc}", file=sys.stderr)
        return 2
    print(f"cache-v3 production descriptor OK: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
