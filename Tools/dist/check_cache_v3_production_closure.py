#!/usr/bin/env python3
"""Reject drift or undeclared edges in the cache-v3 production closure."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
    from .generate_cache_v3_production_descriptor import (
        DescriptorError, build_descriptor, canonical_bytes, load_importer_spec)
except ImportError:
    from generate_cache_v3_production_descriptor import (  # type: ignore
        DescriptorError, build_descriptor, canonical_bytes, load_importer_spec)


def _cache_logical_id(reference: str) -> str | None:
    normalized = reference.replace("\\", "/").lstrip("/")
    return normalized if normalized.startswith("cache/") else None


def _closure_problems(expected: dict[str, object], actual: dict[str, object],
                      importer_spec: Path) -> list[str]:
    problems: list[str] = []
    spec, _raw = load_importer_spec(importer_spec)
    spec_entries = spec["importers"]
    registry = {entry["importer"]: entry for entry in spec_entries}
    audit_pairs = {
        (entry["runtime_consumer"], entry["audit_rule"])
        for entry in spec_entries
    }

    expected_graph = expected["graph"]
    assert isinstance(expected_graph, dict)
    source = expected_graph["source"]
    assert isinstance(source, dict)
    symbols = {entry["symbol"] for entry in source["symbols"]}
    named_consumers = {entry["runtime_consumer"] for entry in spec_entries}

    descriptor_output_keys: set[tuple[object, object]] = set()
    raw_descriptor_outputs = actual.get("importer_outputs")
    if isinstance(raw_descriptor_outputs, list):
        descriptor_output_keys = {
            (entry.get("logical_id"), entry.get("schema"))
            for entry in raw_descriptor_outputs if isinstance(entry, dict)
        }
    for entry in spec_entries:
        output_key = (f"cache/{entry['domain']}", entry["output_schema"])
        if output_key not in descriptor_output_keys:
            problems.append(
                f"descriptor omits declared output {output_key[0]!r} with schema "
                f"{output_key[1]!r}")

    cook = expected_graph["cook_reference_graph"]
    assert isinstance(cook, dict)
    outputs = cook["outputs"]
    references = cook["references"]
    named_consumers.update(output["runtime_consumer"] for output in outputs)
    for consumer in sorted(named_consumers - symbols):
        problems.append(
            f"runtime consumer {consumer!r} is not a real symbol in the Shipping "
            "module closure")

    actual_outputs: list[dict[str, str]] = []
    try:
        actual_graph = actual["graph"]
        assert isinstance(actual_graph, dict)
        actual_cook = actual_graph["cook_reference_graph"]
        assert isinstance(actual_cook, dict)
        raw_actual_outputs = actual_cook["outputs"]
        if not isinstance(raw_actual_outputs, list):
            raise TypeError
        actual_outputs = [entry for entry in raw_actual_outputs
                          if isinstance(entry, dict)]
    except (KeyError, TypeError, AssertionError):
        problems.append("descriptor does not record cooked outputs with logical_id and schema")
    actual_output_keys = {
        (entry.get("logical_id"), entry.get("schema")) for entry in actual_outputs
    }

    declared_ids: set[str] = set()
    covered_importers: set[str] = set()
    for index, output in enumerate(outputs):
        logical_id = output["logical_id"]
        if logical_id in declared_ids:
            problems.append(f"cook output {logical_id!r} is declared more than once")
        declared_ids.add(logical_id)
        counterpart = registry.get(output["importer"])
        if counterpart is None:
            problems.append(
                f"cook output {logical_id!r} names undeclared importer "
                f"{output['importer']!r}")
        else:
            covered_importers.add(output["importer"])
            logical_root = f"cache/{counterpart['domain']}"
            if (logical_id != logical_root
                    and not logical_id.startswith(logical_root + "/")):
                problems.append(
                    f"cook output {logical_id!r} is outside importer "
                    f"{output['importer']!r} logical root {logical_root!r}")
            if output["schema"] != counterpart["output_schema"]:
                problems.append(
                    f"cook output {logical_id!r} schema {output['schema']!r} does not "
                    f"match importer {output['importer']!r}")
            expected_pair = (counterpart["runtime_consumer"], counterpart["audit_rule"])
            if (output["runtime_consumer"], output["audit_rule"]) != expected_pair:
                problems.append(
                    f"cook output {logical_id!r} does not match its importer audit binding")
        if (output["runtime_consumer"], output["audit_rule"]) not in audit_pairs:
            problems.append(
                f"cook output {logical_id!r} has no audit-set counterpart")
        if (logical_id, output["schema"]) not in actual_output_keys:
            problems.append(
                f"descriptor omits cook output {logical_id!r} with schema "
                f"{output['schema']!r}")

    for importer in sorted(set(registry) - covered_importers):
        problems.append(
            f"declared importer output {importer!r} is absent from the cooked outputs")

    for index, reference in enumerate(references):
        logical_id = _cache_logical_id(reference["reference"])
        if logical_id is not None and logical_id not in declared_ids:
            problems.append(
                f"cook reference references[{index}] maps to undeclared cache output "
                f"{logical_id!r}")
    return problems


def check_descriptor(descriptor: Path, importer_spec: Path, shipping_target: Path,
                     source_root: Path, config_root: Path,
                     cook_reference_graph: Path | None = None,
                     reject_undeclared: bool = False) -> list[str]:
    problems: list[str] = []
    if reject_undeclared and cook_reference_graph is None:
        problems.append("--reject-undeclared requires --cook-reference-graph; "
                        "the descriptor records not_yet_cooked but that is not a cook gate")
        return problems
    try:
        expected_value = build_descriptor(
            importer_spec, shipping_target, source_root, config_root,
            cook_reference_graph)
        expected = canonical_bytes(expected_value)
        actual = descriptor.read_bytes()
        actual_value = json.loads(actual.decode("utf-8"))
        if not isinstance(actual_value, dict):
            return ["descriptor must be a JSON object"]
    except (DescriptorError, OSError) as exc:
        return [str(exc)]
    except (UnicodeError, json.JSONDecodeError) as exc:
        return [f"descriptor is not UTF-8 JSON: {exc}"]
    if actual != expected:
        problems.append(
            f"{descriptor} is stale: an importer output or Shipping graph consumer changed")
    if reject_undeclared:
        try:
            problems.extend(_closure_problems(
                expected_value, actual_value, importer_spec))
        except DescriptorError as exc:
            problems.append(str(exc))
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptor", required=True, type=Path)
    parser.add_argument("--importer-spec", required=True, type=Path)
    parser.add_argument("--shipping-target", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--config-root", required=True, type=Path)
    parser.add_argument("--cook-reference-graph", type=Path)
    parser.add_argument("--reject-undeclared", action="store_true")
    args = parser.parse_args(argv)
    problems = check_descriptor(
        args.descriptor, args.importer_spec, args.shipping_target,
        args.source_root, args.config_root, args.cook_reference_graph,
        args.reject_undeclared)
    for problem in problems:
        print(f"cache-v3 closure: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"cache-v3 production closure OK: {args.descriptor}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
