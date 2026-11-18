#!/usr/bin/env python3
"""Generate native UE1 actor-property reflection facts for runtime worlds.

The generator deliberately delegates field lookup, inheritance, tag parsing,
and property-class resolution to ue1pkg._ScriptResolver/_property_tags.  The
native importer consumes only this frozen projection; it does not implement a
second UnrealScript reflection parser.
"""
from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
CONVERT = REPO / "Tools" / "convert"
IMPORT = REPO / "Tools" / "import"
sys.path[:0] = [str(CONVERT), str(IMPORT)]

from import_all import MP_MAPS  # noqa: E402
from ue1pkg import (  # noqa: E402
    Package,
    PayloadReader,
    Reader,
    _ScriptResolver,
    _level_actor_refs,
    _property_tags,
)


_KIND_TO_TAG = {
    "byte": 1,
    "int": 2,
    "bool": 3,
    "float": 4,
    "object": 5,
    "name": 6,
    "array": 9,
    "struct": 10,
    "string": 13,
}


# Keep the multiplayer contract sourced only from import_all.MP_MAPS while
# admitting the dedicated entry world to the property-type census consumed by
# the same native decoder.
PROPERTY_MAPS = tuple(MP_MAPS) + ("WheelOfTime",)

# Single-player Grunts carry three live per-instance event hooks that no
# multiplayer actor authors.  Keep the multiplayer/property census above
# unchanged and admit only those fields from the two maps that exercise them.
AUTHORED_NPC_EVENT_PROPERTIES = frozenset({
    "initthreatgoalevent",
    "takedamageevent",
    "btakedamageeventonce",
})
AUTHORED_NPC_EVENT_MAPS = ("Mission_03", "Mission_16")


def _identity(package: Package, reference: int) -> str:
    path = package.objpath(reference)
    if not path:
        raise ValueError(
            f"{package.path}: reference {reference} has no stable identity")
    return path


def _object_class(package: Package, reference: int) -> str:
    if reference == 0:
        return ""
    if reference < 0:
        class_package, class_name, _outer, _name = package.imports[-reference - 1]
        return f"{class_package}.{class_name}"
    return _identity(package, package.exports[reference - 1]["cls"])


def _reflected_class_identity(
        resolver: _ScriptResolver, package: Package, reference: int) -> str:
    resolved = resolver.resolve(package, reference)
    if resolved is not None:
        target, index = resolved
        path = target.objpath(index)
        if not path:
            raise ValueError(
                f"{target.path}: reflected class {index} has no identity")
        return path if path.casefold().startswith(
            target.name.casefold() + ".") else f"{target.name}.{path}"
    return _identity(package, reference)


def collect(retail: Path) -> list[tuple]:
    resolver = _ScriptResolver(retail / "System" / "Core.u")
    rows: dict[tuple[str, str], dict] = {}
    actual_classes: dict[tuple[str, str], set[str]] = defaultdict(set)
    sources = (
        *((map_name, None) for map_name in PROPERTY_MAPS),
        *((map_name, AUTHORED_NPC_EVENT_PROPERTIES)
          for map_name in AUTHORED_NPC_EVENT_MAPS),
    )
    for map_name, property_filter in sources:
        package = Package(retail / "Maps" / f"{map_name}.wot")
        for actor_reference in _level_actor_refs(package):
            if actor_reference == 0:
                continue
            export = package.exports[actor_reference - 1]
            actor_class = _identity(package, export["cls"])
            resolved_class = resolver.resolve(package, export["cls"])
            if resolved_class is None:
                raise ValueError(
                    f"{package.path}: cannot reflect actor class {actor_class}")
            reader = PayloadReader(package, export)
            for tag in _property_tags(reader):
                if (property_filter is not None and (
                        tag["name"].casefold() not in property_filter)):
                    continue
                reflected = resolver.find_property(resolved_class, tag["name"])
                if reflected is None:
                    raise ValueError(
                        f"{actor_class}.{tag['name']}: reflected field is missing")
                expected_tag = _KIND_TO_TAG.get(reflected["kind"])
                if expected_tag is None or expected_tag != tag["type"]:
                    raise ValueError(
                        f"{actor_class}.{tag['name']}: reflected/tag type mismatch")
                if (reflected["kind"] != "array"
                        and tag["array_index"] >= reflected["array_dim"]):
                    raise ValueError(
                        f"{actor_class}.{tag['name']}: array index is out of range")
                struct_name = ""
                if reflected["kind"] == "struct":
                    resolved_struct = resolver.resolve(
                        reflected["package"], reflected["struct_ref"])
                    if resolved_struct is None:
                        raise ValueError(
                            f"{actor_class}.{tag['name']}: struct type is unresolved")
                    struct_name = resolved_struct[0].exports[
                        resolved_struct[1] - 1]["name"]
                    if struct_name.casefold() != str(tag["struct"]).casefold():
                        raise ValueError(
                            f"{actor_class}.{tag['name']}: struct tag mismatch")
                expected_object_class = ""
                if reflected["kind"] == "object":
                    expected_object_class = _reflected_class_identity(
                        resolver, reflected["package"],
                        reflected["property_class"])
                    value_reader = Reader(tag["value"])
                    object_reference = value_reader.cindex()
                    if value_reader.o != len(value_reader.d):
                        raise ValueError(
                            f"{actor_class}.{tag['name']}: object value has a tail")
                    actual = _object_class(package, object_reference)
                    if actual:
                        actual_classes[
                            (actor_class.casefold(), tag["name"].casefold())
                        ].add(actual)
                key = (actor_class.casefold(), tag["name"].casefold())
                candidate = {
                    "actor_class": actor_class,
                    "property": tag["name"],
                    "type": expected_tag,
                    "array_dim": reflected["array_dim"],
                    "struct": struct_name,
                    "expected_object_class": expected_object_class,
                }
                prior = rows.setdefault(key, candidate)
                if prior != candidate:
                    raise ValueError(
                        f"{actor_class}.{tag['name']}: inconsistent reflection facts")
    result = []
    for key, row in sorted(rows.items()):
        allowed = sorted(actual_classes[key], key=lambda value: (
            value.casefold(), value))
        result.append((
            key[0], key[1], row["actor_class"], row["property"], row["type"],
            row["array_dim"], row["struct"], row["expected_object_class"],
            "|".join(allowed)))
    return result


def _escape(value: str) -> str:
    if any(character in value for character in '"\\\r\n'):
        raise ValueError(f"unsafe generated property identity {value!r}")
    return value


def generate(retail: Path) -> bytes:
    encoded = ",\n".join(
        f'\t{{TEXT("{_escape(actor_folded)}"), '
        f'TEXT("{_escape(property_folded)}"), '
        f'TEXT("{_escape(actor_class)}"), TEXT("{_escape(property_name)}"), '
        f'{tag_type}u, {array_dim}u, TEXT("{_escape(struct_name)}"), '
        f'TEXT("{_escape(expected_class)}"), TEXT("{_escape(allowed)}")}}'
        for (actor_folded, property_folded, actor_class, property_name,
             tag_type, array_dim, struct_name, expected_class, allowed)
        in collect(retail))
    return f"""// Generated by Tools/dist/generate_world_property_registry.py; do not edit.
#pragma once

#include "CoreMinimal.h"

namespace WoT::Retail::Generated
{{
struct FWorldProperty
{{
\tconst TCHAR* ActorClassFolded;
\tconst TCHAR* PropertyNameFolded;
\tconst TCHAR* ActorClass;
\tconst TCHAR* PropertyName;
\tuint8 TagType;
\tuint32 ArrayDim;
\tconst TCHAR* StructName;
\tconst TCHAR* ExpectedObjectClass;
\t/** Pipe-delimited concrete retail subclasses admitted for this field. */
\tconst TCHAR* AllowedObjectClasses;
}};

inline constexpr FWorldProperty WorldProperties[] = {{
{encoded}
}};
}}  // namespace WoT::Retail::Generated
""".encode("utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        expected = generate(args.retail_root)
        if args.check:
            if args.output.read_bytes() != expected:
                print(f"world property generated header is stale: {args.output}",
                      file=sys.stderr)
                return 1
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(expected)
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(f"world property header OK: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
