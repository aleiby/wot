#!/usr/bin/env python3
"""Offline census of every MP map's world-BSP texture identities.

Derives, from the retail maps (original/Maps/*.wot) and the production cache
contract (Build/WoTRelease/cache-contract-v3-production.json), what the
guarded runtime BSP (WoTWorldMaterializer.cpp MakeSectionMaterial) does with
each drawn material slot of the level's world Model (main AND portal
surfaces; PF_Invisible surfaces are not emitted and PF_FakeBackdrop sky
windows are dropped before any texture is resolved):

* ``resolved``        the contract holds an object for the section's identity
                      whose source export IS the map's reference. Identity is
                      exact-first (Tools/dist/retail_identity.py, runtime
                      WoTRetailIdentity.h): the grouped exact identity
                      ``Pkg.Group__Name`` when the contract holds it (a name
                      that collides inside its package), else ``Pkg__Name``.
                      The source comparison is case-insensitive: UE1 names
                      are case-insensitive FNames, and a map
                      may spell a package differently from the package's own
                      export table (Arena_06 imports ``Naturet.Sky.Sky_340a``).
                      ``case_variant`` marks such rows - the case-SENSITIVE
                      source check an earlier runtime had refused
                      every one of them (the 19-map sweep's Arena_06,
                      Battle2_02, Battle2_05 and Battle4_01 slots);
* ``group-alias``     the contract holds only another same-named export of the
                      package (the earlier last-duplicate rule). None remain;
                      the sweep allowlist of these is therefore empty;
* ``dangling``        no contract object in the texture or procedural domain:
                      the map imports a texture no shipped package exports
                      (retail loads NULL and renders Level->DefaultTexture);
* ``null``            the surface has no texture (retail DefaultTexture).

The slot name follows the native BSPWorld.cpp WoTWorldMaterialSlot rule
(canonical package/name + flag suffixes); the canonical spelling is the
resolved contract export's own.

``sound_reference_census`` does the same for every Sound import of the MP
maps (AmbientSound / DynamicAmbientSound / SoundDispatcher references): each
must resolve exactly (Battle2_04 AmbientA.Nature.Rustle3 vs Battle2_05
AmbientA.Bush.Rustle3 are different recordings).

  python Tools/test/sweep/texture_identity_census.py [--json OUT]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "Tools" / "convert"))
sys.path.insert(0, str(REPO / "Tools" / "dist"))
sys.path.insert(0, str(REPO / "Tools"))
sys.path.insert(0, str(HERE))
import ue1pkg  # noqa: E402  pylint: disable=wrong-import-position
import retail_identity  # noqa: E402  pylint: disable=wrong-import-position
from sweep_checks import MP_MAPS  # noqa: E402  pylint: disable=wrong-import-position
from wot_config import original_dir  # noqa: E402  pylint: disable=wrong-import-position

CONTRACT = REPO / "Build" / "WoTRelease" / "cache-contract-v3-production.json"
RETAIL = original_dir()

PF_INVISIBLE = 0x00000001
PF_MASKED = 0x00000002
PF_TRANSLUCENT = 0x00000004
PF_MODULATED = 0x00000040
PF_FAKEBACKDROP = 0x00000080
PF_TWOSIDED = 0x00000100
PF_SPECIALLIT = 0x00100000
PF_UNLIT = 0x00400000

TEXTURE_DOMAINS = ("cache/texture/", "cache/procedural/")


def material_slot(package: str, name: str | None, flags: int) -> str:
    """BSPWorld.cpp WoTWorldMaterialSlot."""
    slot = f"{package}__{name}" if name else "__None"
    for bit, suffix in ((PF_MASKED, "_M"), (PF_TRANSLUCENT, "_T"),
                        (PF_MODULATED, "_Mod"), (PF_TWOSIDED, "_2S"),
                        (PF_UNLIT, "_U"), (PF_FAKEBACKDROP, "_Sky"),
                        (PF_SPECIALLIT, "_SL")):
        if flags & bit:
            slot += suffix
    return slot


def contract_sources(path: Path = CONTRACT, prefixes=TEXTURE_DOMAINS) -> dict[str, str]:
    """logical id (case-folded) -> source export, for the given domains."""
    document = json.loads(path.read_text(encoding="utf-8"))
    sources: dict[str, str] = {}
    for fact in document["payload"]["facts"]:
        for output in fact["outputs"]:
            logical = output.get("logical_id", "")
            if logical.startswith(prefixes):
                sources[logical.casefold()] = fact["source"]["export"]
    return sources


def resolve_reference(object_path: str, sources: dict[str, str],
                      prefixes=TEXTURE_DOMAINS) -> tuple[str | None, str | None]:
    """Runtime resolution of a retail object path: (logical id, source export)
    of the exact-first identity in the first domain that holds it."""
    package, groups, name = retail_identity.split_object_path(object_path)
    for prefix in prefixes:
        base = retail_identity.resolve_base(
            package, groups, name,
            lambda candidate, prefix=prefix: (prefix + candidate).casefold() in sources)
        logical = prefix + base
        cached = sources.get(logical.casefold())
        if cached is not None:
            return logical, cached
    return None, None


def level_model(package: "ue1pkg.Package") -> dict:
    """The ULevel.Model the world BSP is compiled from (not a mover brush)."""
    reader, _ = ue1pkg._level_reader_after_actor_refs(package)  # pylint: disable=protected-access
    ue1pkg._skip_level_url(reader)  # pylint: disable=protected-access
    reference = reader.cindex()
    if reference <= 0:
        raise ValueError(f"{package.path}: ULevel.Model is not an export")
    return ue1pkg.decode_model(package, package.exports[reference - 1])


def _texture_digest(export_path: str, retail: Path) -> str | None:
    """Pixel + palette-colour digest of a retail texture export, for reporting
    whether two same-named exports are content-identical."""
    package_name, _, relative = export_path.partition(".")
    for folder, suffix in (("Textures", ".utx"), ("System", ".u")):
        path = retail / folder / f"{package_name}{suffix}"
        if path.is_file():
            break
    else:
        return None
    package = ue1pkg.Package(path)
    for index, export in enumerate(package.exports, 1):
        if (package.objpath(index) or "").casefold() != relative.casefold():
            continue
        if package.objname(export["cls"]) != "Texture":
            continue
        texture = ue1pkg.decode_texture(package, export)
        palette = ue1pkg.decode_palette(package, package.exports[texture["palette"] - 1])
        digest = hashlib.sha256()
        for width, height, data in texture["mips"]:
            digest.update(f"{width}x{height}".encode())
            digest.update(bytes(data))
        digest.update(bytes(channel for colour in palette for channel in colour))
        return digest.hexdigest()[:16]
    return None


def census_map(map_name: str, sources: dict[str, str], retail: Path = RETAIL) -> dict:
    package = ue1pkg.Package(retail / "Maps" / f"{map_name}.wot")
    model = level_model(package)
    slots: dict[str, dict] = {}
    for _vertex_pool, surface_index, vertex_count, _normal in model["nodes"]:
        if vertex_count < 3:
            continue
        surface = model["surfs"][surface_index]
        flags = surface["flags"]
        if flags & (PF_INVISIBLE | PF_FAKEBACKDROP):
            continue
        reference = surface["tex"]
        if reference == 0:
            slots.setdefault(material_slot("", None, flags), {"kind": "null"})
            continue
        source = package.objpath(reference)
        logical, cached = resolve_reference(source, sources)
        if cached is not None:
            canonical_package, _groups, canonical_name = \
                retail_identity.split_object_path(cached)
        else:
            canonical_package, canonical_name = package.objref(reference)
        slot = material_slot(canonical_package, canonical_name, flags)
        if slot in slots:
            continue
        if cached is None:
            slots[slot] = {"kind": "dangling", "reference": source}
        elif cached.casefold() != source.casefold():
            slots[slot] = {"kind": "group-alias", "reference": source, "cached": cached,
                           "logical_id": logical}
        else:
            row = {"kind": "resolved", "logical_id": logical}
            if cached != source:
                row["case_variant"] = {"reference": source, "cached": cached}
            if "." in logical.split("/", 2)[2].split("__", 1)[0]:
                row["exact"] = True
            slots[slot] = row
    result = {
        "unresolved_slots": sorted(slot for slot, row in slots.items()
                                   if row["kind"] == "group-alias"),
        "default_texture_slots": sorted(slot for slot, row in slots.items()
                                        if row["kind"] in ("dangling", "null")),
        "case_variant_slots": sorted(slot for slot, row in slots.items()
                                     if "case_variant" in row),
        "exact_identity_slots": sorted(slot for slot, row in slots.items()
                                       if row.get("exact")),
        "details": {slot: row for slot, row in sorted(slots.items())
                    if row["kind"] != "resolved" or "case_variant" in row
                    or row.get("exact")},
    }
    return result


def census(retail: Path = RETAIL, contract: Path = CONTRACT) -> dict:
    sources = contract_sources(contract)
    return {name: census_map(name, sources, retail) for name in MP_MAPS}


def colliding_reference_details(retail: Path = RETAIL) -> dict:
    """Content comparison of the colliding AesSedaiT exports the MP maps use."""
    out = {}
    for name in ("OrnaTrim1505", "PaprWall1441"):
        digests = {}
        for group in ("Ornament", "DetailArchitect", "SurfaceWall"):
            digest = _texture_digest(f"AesSedaiT.{group}.{name}", retail)
            if digest is not None:
                digests[group] = digest
        out[name] = digests
    return out


def sound_reference_census(retail: Path = RETAIL, contract: Path = CONTRACT) -> dict:
    """Every Sound import of the 19 MP maps -> its exact-first cache object."""
    sources = contract_sources(contract, ("cache/sound/",))
    result = {}
    for map_name in MP_MAPS:
        package = ue1pkg.Package(retail / "Maps" / f"{map_name}.wot")
        rows = {}
        for index, entry in enumerate(package.imports):
            if entry[1] != "Sound":
                continue
            path = package.objpath(-(index + 1))
            logical, cached = resolve_reference(path, sources, ("cache/sound/",))
            rows[path] = {"logical_id": logical, "cached": cached,
                          "resolved": cached is not None
                          and cached.casefold() == path.casefold()}
        result[map_name] = rows
    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="MP world-BSP texture identity census")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)
    result = census()
    sounds = sound_reference_census()
    text = json.dumps({"textures": result, "sounds": sounds}, indent=2, sort_keys=True)
    if args.json:
        args.json.write_text(text + "\n", encoding="utf-8")
    for name, row in result.items():
        if row["details"]:
            print(f"{name}: unresolved={row['unresolved_slots']} "
                  f"default_texture={row['default_texture_slots']} "
                  f"case_variant={row['case_variant_slots']} "
                  f"exact={row['exact_identity_slots']}")
    bad = {name: [path for path, row in rows.items() if not row["resolved"]]
           for name, rows in sounds.items()}
    for name, paths in bad.items():
        if paths:
            print(f"{name}: unresolved sounds {paths}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
