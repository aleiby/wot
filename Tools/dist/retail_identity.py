#!/usr/bin/env python3
"""Exact cache identities for retail typed exports (cache-contract-v3).

A cache logical id is ``cache/<domain>/<Package>__<Name>``. UE1 names are
case-insensitive (the original FName table uses ``appStrihash`` +
``appStricmp``), so "the same name" below means the case-folded name.

* A name that is UNIQUE among one package's exports of a domain's classes
  keeps the plain identity ``<Package>__<Name>`` (nothing else churns).
* When one package exports several such objects with the same name in
  different groups (retail: AesSedaiT.Ornament.PaprWall1441 vs
  AesSedaiT.SurfaceWall.PaprWall1441), EVERY one of them gets its exact
  identity ``<Package>.<Group>[.<Group>...]__<Name>``: the complete outer
  chain, dotted, then ``__`` and the name. A top-level colliding export's
  outer chain is the package alone, so its exact identity is the plain form -
  which is exactly its UE1 path ``<Package>.<Name>``.

Every logical id therefore denotes exactly one export. The previous rule kept
only the LAST same-named export ("later duplicate wins, like UCC export"),
which silently aliased different images/sounds (PaprWall1441, Rustle3).

Runtime consumers resolve an object path ``Pkg.G1.G2.Name`` exact-first: the
exact base when the authenticated set holds it, else the plain base
(Source/WoT/Assets/WoTRetailIdentity.h mirrors this module). A consumer that
verifies the source export compares case-insensitively (FName semantics).

UE1 package and group names never contain ``.`` or ``__``; object names
never contain ``.`` but may contain ``__`` (AmbientA.Water.Dive__2). Both forms
are therefore injective and the FIRST ``__`` still splits a logical base into
(outer path, name).

A group name may contain characters the cache logical-id grammar refuses
(retail: FootStubA's "(All)" group; FrontEndWorld.cpp WoTFrontEndSafeLogicalId
admits only [A-Za-z0-9_.-] per component). Group components are therefore
escaped reversibly: every byte outside [A-Za-z0-9_] becomes "-XX" (upper-case
hex), so "(All)" is "-28All-29" and a literal "-" is "-2D".
"""
from __future__ import annotations

from typing import Any, Callable, Iterable, NamedTuple

_EXPORT_SUFFIX_MARKERS = ("#export-", "#import-")


class TypedExportIdentity(NamedTuple):
    """One typed export and its cache identity inside its package."""
    index: int                  # 1-based export index
    export: dict
    name: str                   # the export's own name (package spelling)
    groups: tuple[str, ...]     # outer groups below the package, outermost first
    colliding: bool             # another export of the domain shares the folded name
    base: str                   # logical base (no "cache/<domain>/" prefix)
    fact_key: str               # the fact-id tail after "retail.<domain>."


def fold(value: str) -> str:
    """UE1 FName comparison key (ASCII case fold; retail identities are ASCII)."""
    return value.casefold()


def safe_identity(value: str) -> str:
    """The fact-id component spelling (generate_production_cache_contract)."""
    return value.casefold().replace(" ", "_")


def strip_reference_suffix(component: str) -> str:
    """Drop a builder-owned ``#export-N`` / ``#import-N`` disambiguator."""
    for marker in _EXPORT_SUFFIX_MARKERS:
        at = component.rfind(marker)
        if at > 0 and component[at + len(marker):].isdigit():
            return component[:at]
    return component


def split_object_path(path: str) -> tuple[str, tuple[str, ...], str]:
    """``Pkg.G1.G2.Name[#export-N]`` -> (Pkg, (G1, G2), Name)."""
    parts = [part for part in path.split(".")]
    if len(parts) < 2 or any(not part for part in parts):
        raise ValueError(f"not a package-qualified UE1 object path: {path!r}")
    return parts[0], tuple(parts[1:-1]), strip_reference_suffix(parts[-1])


def encode_group(component: str) -> str:
    """Reversible logical-id spelling of one group name (see module doc)."""
    out = []
    for byte in component.encode("utf-8"):
        character = chr(byte)
        if character.isascii() and (character.isalnum() or character == "_"):
            out.append(character)
        else:
            out.append(f"-{byte:02X}")
    return "".join(out)


def plain_base(package: str, name: str) -> str:
    return f"{package}__{name}"


def exact_base(package: str, groups: Iterable[str], name: str) -> str:
    outer = ".".join((package, *(encode_group(group) for group in groups)))
    return f"{outer}__{name}"


def resolve_base(package: str, groups: Iterable[str], name: str,
                 exists: Callable[[str], bool]) -> str:
    """Exact-first resolution: the exact base when ``exists(base)`` (the
    authenticated/contract set holds it), else the plain base."""
    groups = tuple(groups)
    if groups:
        exact = exact_base(package, groups, name)
        if exists(exact):
            return exact
    return plain_base(package, name)


def _export_groups(package: Any, export_index: int) -> tuple[str, ...]:
    path = package.objpath(export_index)
    if not isinstance(path, str) or not path:
        raise ValueError("a retail export has no object path")
    parts = path.split(".")
    return tuple(parts[:-1])


def typed_export_identities(package: Any, classes: Iterable[str]
                            ) -> list[TypedExportIdentity]:
    """Every export of ``classes`` in ``package`` with its exact identity.

    Sorted by (folded name, name, export index) - the order the contract
    generator previously emitted its (last-duplicate) rows in."""
    wanted = frozenset(classes)
    typed: list[tuple[int, dict]] = [
        (index, export) for index, export in enumerate(package.exports, 1)
        if package.objname(export["cls"]) in wanted]
    counts: dict[str, int] = {}
    for _index, export in typed:
        key = fold(export["name"])
        counts[key] = counts.get(key, 0) + 1
    result: list[TypedExportIdentity] = []
    seen_bases: set[str] = set()
    for index, export in typed:
        name = export["name"]
        groups = _export_groups(package, index)
        colliding = counts[fold(name)] > 1
        if colliding:
            if any("." in part or "__" in part or not part
                   for part in (package.name, *(encode_group(g) for g in groups))):
                raise ValueError(
                    f"{package.name}: a colliding export's outer path cannot "
                    f"form an exact identity: {groups!r}")
            base = exact_base(package.name, groups, name)
            fact_key = ".".join(safe_identity(part) for part in (
                package.name, *(encode_group(group) for group in groups), name))
        else:
            base = plain_base(package.name, name)
            fact_key = f"{safe_identity(package.name)}.{safe_identity(name)}"
        folded_base = fold(base)
        if folded_base in seen_bases:
            # Two exports with the same full path and class family: the path
            # itself is ambiguous and no identity can be exact.
            raise ValueError(
                f"{package.name}: typed exports share the exact identity {base!r}")
        seen_bases.add(folded_base)
        result.append(TypedExportIdentity(
            index, export, name, groups, colliding, base, fact_key))
    result.sort(key=lambda item: (fold(item.name), item.name, item.index))
    return result


def colliding_identities(package: Any, classes: Iterable[str]
                         ) -> list[TypedExportIdentity]:
    return [item for item in typed_export_identities(package, classes)
            if item.colliding]
