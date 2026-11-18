#!/usr/bin/env python3
"""Generate BSP texture facts through the canonical bsp2obj resolver.

Only textures referenced by runtime multiplayer and entry BSP surfaces are
emitted.  Every row is
keyed by the source object's exact package-relative identity and carries the
result of ``resolve_tex`` unchanged.  The relative-first/leaf-second fallback
therefore happens once, here, while the native reader performs one unambiguous
lookup.  This makes the generated table a projection of the existing canonical
resolver, not a second resolver.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
CONVERT = REPO / "Tools" / "convert"
IMPORT = REPO / "Tools" / "import"
sys.path[:0] = [str(CONVERT), str(IMPORT)]

from bsp2obj import load_tex_sizes, resolve_tex  # noqa: E402
from import_all import MP_MAPS  # noqa: E402
from ue1pkg import (  # noqa: E402
    Package, PayloadReader, decode_brush_model, decode_model)
from world_fractal_split import runtime_fractal_texture_keys  # noqa: E402


# The entry world shares the native BSP decoder but remains separate from the
# exact nineteen-map multiplayer contract.
RUNTIME_WORLD_MAPS = tuple(MP_MAPS) + ("WheelOfTime",)

# Mover classes the native world encoder treats as movers
# (BSPWorld.cpp WoTWorldIsMoverClass). Their Brush FPolys are the rendered
# geometry (UE1's dynamic BSP builds one surf per FPoly), so a texture that
# appears only on a mover FPoly must still be registered.
MOVER_CLASSES = frozenset({
    "Mover", "DoorMover", "LockableMover", "PortcullisMover",
    "ExplodingMover", "RotatingMover", "LoopMover", "TimedMover",
    "AssertMover", "ElevatorMover", "AttachMover", "GradualMover",
    "MixMover"})
PF_INVISIBLE = 0x00000001
PF_PORTAL = 0x04000000


def _mover_poly_texture_references(package: Package) -> list[int]:
    """Texture refs of the drawn FPolys of every mover actor's Brush model."""
    references = []
    for export in package.exports:
        if package.objname(export["cls"]) not in MOVER_CLASSES:
            continue
        brush = PayloadReader(package, export).properties().get("Brush")
        if not isinstance(brush, int) or brush <= 0:
            continue
        for poly in decode_brush_model(package, package.exports[brush - 1]):
            if poly["flags"] & (PF_INVISIBLE | PF_PORTAL):
                continue
            references.append(poly["texture"])
    return references


def _fold(value: str) -> str:
    """Match resolve_tex's ``str.lower`` key spelling.

    Retail UE1 object identities used by the native reader are ASCII.  Reject
    anything else instead of silently letting Python and the native ASCII fold
    disagree about a generated lookup key.
    """
    if not value.isascii():
        raise ValueError(f"non-ASCII world texture lookup identity {value!r}")
    return value.lower()


def _source_identity(package: Package, reference: int) -> tuple[str, str, str]:
    """Return the native package-relative key plus the source leaf identity.

    ``Package.objpath`` includes the package for imports and for package-local
    exports. Native always removes that one package prefix. ``resolve_tex``
    only does so for imports, but its positive-export path then falls back to
    the same leaf. Keeping the native key distinct from resolver tie-breaking
    lets grouped legacy manifests resolve without a runtime fallback that
    could alias another generated row.
    """
    source_ref = package.objref(reference)
    if source_ref is None:
        raise ValueError("a null BSP texture has no resolver identity")
    full = package.objpath(reference) or source_ref[1]
    parts = full.split(".")
    if parts and _fold(parts[0]) == _fold(source_ref[0]):
        parts = parts[1:]
    relative = ".".join(parts) if parts else source_ref[1]
    return source_ref[0], relative, source_ref[1]


def _record_lookup(used: dict, key: tuple[str, str], canonical: tuple,
                   source: str) -> None:
    previous = used.get(key)
    if previous is not None and previous != canonical:
        raise ValueError(
            "ambiguous folded world texture lookup "
            f"{key[0]}.{key[1]}: {previous!r} versus {canonical!r} ({source})")
    used[key] = canonical


def _assert_unique_lookup_rows(rows: list[tuple]) -> None:
    seen: set[tuple[str, str]] = set()
    for row in rows:
        key = row[:2]
        if key in seen:
            raise ValueError(
                f"duplicate folded world texture lookup {key[0]}.{key[1]}")
        seen.add(key)


def _texture_frictions(manifest_root: Path) -> tuple[dict[str, float],
                                                      dict[tuple[str, str], float],
                                                      set[str]]:
    """Mirror zones._texture_frictions without importing converter behavior."""
    exact: dict[str, float] = {}
    flat: dict[tuple[str, str], float] = {}
    packages: set[str] = set()
    manifests = sorted(Path(manifest_root).glob("*/_manifest.json"),
                       key=lambda path: str(path).casefold())
    if not manifests:
        raise FileNotFoundError(
            f"texture manifest root has no */_manifest.json: {manifest_root}")
    for path in manifests:
        package = path.parent.name
        packages.add(package.casefold())
        document = json.loads(path.read_text(encoding="utf-8-sig"))
        for name, record in document.get("textures", {}).items():
            friction = float(record.get("friction", 1.0))
            if not math.isfinite(friction):
                raise ValueError(f"non-finite texture friction {package}.{name}")
            object_path = str(record.get("object_path", f"{package}.{name}"))
            exact[object_path.casefold()] = friction
            flat[(package.casefold(), str(name).casefold())] = friction
    return exact, flat, packages


def _texture_overlays(manifest_root: Path, sizes: dict[tuple[str, str], tuple],
                      key: str, label: str) \
        -> dict[tuple[str, str], tuple[str, str, str, int, int, float]]:
    """Canonical base texture -> canonical authored overlay facts.

    ``key`` is the utx2png manifest field (``macro`` or ``detail``) and
    ``label`` the UE1 property it records (MacroTexture, DetailTexture).
    utx2png records the UE1 object reference as ``[package, name]``. Resolve
    that pair through the same manifest table used by bsp2obj.resolve_tex,
    then retain the target's full object path so the runtime can authenticate
    grouped cache identities rather than inventing a bare-name alias.
    """
    records: dict[tuple[str, str], dict] = {}
    manifests = sorted(Path(manifest_root).glob("*/_manifest.json"),
                       key=lambda path: str(path).casefold())
    for path in manifests:
        folder = path.parent.name
        document = json.loads(path.read_text(encoding="utf-8-sig"))
        for name, record in document.get("textures", {}).items():
            records[(folder.casefold(), str(name).casefold())] = record

    result: dict[tuple[str, str], tuple[str, str, str, int, int, float]] = {}
    for base_key, record in records.items():
        overlay = record.get(key)
        if not overlay:
            continue
        if (not isinstance(overlay, list) or len(overlay) != 2
                or not all(isinstance(value, str) and value
                           for value in overlay)):
            raise ValueError(f"invalid {label} identity for {base_key}")
        source_package, source_name = overlay
        canonical = sizes.get(
            (source_package.casefold(), source_name.casefold()))
        if canonical is None:
            raise ValueError(
                f"{label} is absent from the canonical resolver: "
                f"{source_package}.{source_name}")
        folder, asset, width, height, draw_scale = canonical
        target = records.get((folder.casefold(), asset.casefold()))
        source_identity = target.get("object_path") if target else None
        if (not isinstance(source_identity, str) or not source_identity
                or not isinstance(width, int) or width <= 0
                or not isinstance(height, int) or height <= 0
                or not math.isfinite(float(draw_scale)) or draw_scale <= 0):
            raise ValueError(
                f"invalid canonical {label} facts for {folder}.{asset}")
        result[base_key] = (
            folder, asset, source_identity, width, height, float(draw_scale))
    return result


def _texture_macros(manifest_root: Path, sizes: dict[tuple[str, str], tuple]) \
        -> dict[tuple[str, str], tuple[str, str, str, int, float]]:
    """Canonical base texture -> canonical authored MacroTexture facts."""
    return {base_key: (folder, asset, source, width, draw_scale)
            for base_key, (folder, asset, source, width, _height, draw_scale)
            in _texture_overlays(manifest_root, sizes, "macro",
                                 "MacroTexture").items()}


def _texture_details(manifest_root: Path, sizes: dict[tuple[str, str], tuple]) \
        -> dict[tuple[str, str], tuple[str, str, str, int, int, float]]:
    """Canonical base texture -> canonical authored DetailTexture facts.

    Height is kept because the detail UV ratio is per axis: a non-square base
    divides V by its own height (the Glide driver's SetTexture, CopyVerts).
    """
    return _texture_overlays(manifest_root, sizes, "detail", "DetailTexture")


def _surface_friction(source_package: str, relative: str, source_name: str,
                      lookups: tuple[dict[str, float],
                                     dict[tuple[str, str], float],
                                     set[str]]) -> float:
    exact, flat, packages = lookups
    if source_package.casefold() not in packages:
        raise FileNotFoundError(
            f"texture package {source_package!r} has no _manifest.json")
    value = exact.get(f"{source_package}.{relative}".casefold())
    if value is not None:
        return value
    return flat.get((source_package.casefold(), source_name.casefold()), 1.0)


def collect(retail: Path, conversion: Path) -> list[tuple]:
    sizes = load_tex_sizes(conversion / "tex")
    friction_lookups = _texture_frictions(conversion / "tex")
    macro_lookups = _texture_macros(conversion / "tex", sizes)
    detail_lookups = _texture_details(conversion / "tex", sizes)
    runtime = runtime_fractal_texture_keys(conversion / "flipbooks")
    used: dict[tuple[str, str], tuple[str, str, int, int, float, float]] = {}
    for map_name in RUNTIME_WORLD_MAPS:
        package = Package(retail / "Maps" / f"{map_name}.wot")
        model_exports = [item for item in package.exports
                         if package.objname(item["cls"]) == "Model"]
        if not model_exports:
            raise ValueError(f"{map_name}: package contains no Model export")
        # The largest export is the Level model; other Model exports include
        # the actor Brush models consumed by the mover-record encoder.
        references = [surface["tex"]
                      for model_export in model_exports
                      for surface in decode_model(package, model_export)["surfs"]]
        references += _mover_poly_texture_references(package)
        for reference in references:
            if package.objref(reference) is None:
                continue
            source_package, relative, source_name = _source_identity(
                package, reference)
            canonical = resolve_tex(package, reference, sizes)
            friction = _surface_friction(
                source_package, relative, source_name, friction_lookups)
            lookup_key = (_fold(source_package), _fold(relative))
            _record_lookup(
                used, lookup_key, (*canonical, friction),
                f"{map_name}:{package.objpath(reference) or source_name}")

    rows = []
    for source_key, (folder, asset, width, height, draw_scale, friction) in sorted(used.items()):
        if (not isinstance(width, int) or not isinstance(height, int)
                or width <= 0 or height <= 0
                or not math.isfinite(float(draw_scale)) or draw_scale <= 0
                or not math.isfinite(float(friction))):
            raise ValueError(f"invalid canonical texture facts for {folder}.{asset}")
        macro = macro_lookups.get((folder.casefold(), asset.casefold()),
                                  ("", "", "", 0, 0.0))
        detail = detail_lookups.get((folder.casefold(), asset.casefold()),
                                    ("", "", "", 0, 0, 0.0))
        rows.append((*source_key, folder, asset, width, height, float(draw_scale),
                     float(friction),
                     (folder.casefold(), asset.casefold()) in runtime,
                     *macro, *detail))
    _assert_unique_lookup_rows(rows)
    return rows


def _escape(value: str) -> str:
    if any(character in value for character in '"\\\r\n'):
        raise ValueError(f"unsafe generated texture identity {value!r}")
    return value


def generate(retail: Path, conversion: Path) -> bytes:
    rows = collect(retail, conversion)
    encoded = ",\n".join(
        f'\t{{TEXT("{_escape(source_package)}"), '
        f'TEXT("{_escape(source_lookup)}"), TEXT("{_escape(package)}"), '
        f'TEXT("{_escape(name)}"), {width}u, {height}u, '
        f'{format(draw_scale, ".17g")}, '
        f'{"true" if procedural else "false"}, '
        f'TEXT("{_escape(macro_package)}"), '
        f'TEXT("{_escape(macro_name)}"), '
        f'TEXT("{_escape(macro_source)}"), {macro_width}u, '
        f'{format(macro_draw_scale, ".17g")}, '
        f'TEXT("{_escape(detail_package)}"), '
        f'TEXT("{_escape(detail_name)}"), '
        f'TEXT("{_escape(detail_source)}"), {detail_width}u, '
        f'{detail_height}u, {format(detail_draw_scale, ".17g")}'
        f'{", " + format(friction, ".17g") if friction != 1.0 else ""}}}'
        for (source_package, source_lookup, package, name, width, height,
             draw_scale, friction, procedural, macro_package, macro_name,
             macro_source, macro_width, macro_draw_scale, detail_package,
             detail_name, detail_source, detail_width, detail_height,
             detail_draw_scale) in rows)
    return f"""// Generated by Tools/dist/generate_world_texture_registry.py; do not edit.
#pragma once

#include "CoreMinimal.h"

namespace WoT::Retail::Generated
{{
struct FWorldTexture
{{
\tconst TCHAR* SourcePackageFolded;
\tconst TCHAR* SourceRelativeFolded;
\tconst TCHAR* CanonicalPackage;
\tconst TCHAR* CanonicalName;
\tuint32 Width;
\tuint32 Height;
\tdouble DrawScale;
\tbool bProcedural;
\tconst TCHAR* MacroCanonicalPackage;
\tconst TCHAR* MacroCanonicalName;
\tconst TCHAR* MacroSourceTextureIdentity;
\tuint32 MacroWidth;
\tdouble MacroDrawScale;
\tconst TCHAR* DetailCanonicalPackage;
\tconst TCHAR* DetailCanonicalName;
\tconst TCHAR* DetailSourceTextureIdentity;
\tuint32 DetailWidth;
\tuint32 DetailHeight;
\tdouble DetailDrawScale;
\tdouble Friction = 1.0;
}};

inline constexpr FWorldTexture WorldTextures[] = {{
{encoded}
}};
}}  // namespace WoT::Retail::Generated
""".encode("utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", required=True, type=Path)
    parser.add_argument("--conversion-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        expected = generate(args.retail_root, args.conversion_root)
        if args.check:
            if args.output.read_bytes() != expected:
                print(f"world texture generated header is stale: {args.output}",
                      file=sys.stderr)
                return 1
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(expected)
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(f"world texture header OK: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
