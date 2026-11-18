#!/usr/bin/env python3
"""Generate the signed production cache-contract-v3 document and native header.

Closure source decision
-----------------------
The production descriptor's ``importer_outputs`` array is the authority for
every domain/importer/schema/limit/runtime-consumer/audit binding.  Retail
source facts use the same production censuses as the import tooling:
``build_payload_cache.source_packages`` for texture, sound, and mesh packages,
its System/Textures package scope for the remaining package domains,
``import_all.MP_MAPS`` (through the checked BSP parity census) for the nineteen
worlds, and the complete System ``.u``/``.int`` sets used by the native retail
pin.  Typed identities come from ``retail_identity.typed_export_identities``:
a name unique in its package keeps ``<Package>__<Name>``; exports that share a
(case-folded) name inside one package each get their exact identity
``<Package>.<Group...>__<Name>`` instead of the former last-export-wins alias.

Test-material re-signing ceremony
---------------------------------
The checked signature and public key below were made from one fresh ephemeral
Ed25519 key.  The private half existed only in a temporary directory and was
destroyed without ever being written to this repository.  If the canonical
payload changes, generation MUST fail until a deliberate ceremony creates a
fresh key, signs ``signature_frame(payload)``, and replaces PUBLIC_KEY_HEX,
PAYLOAD_SHA256, and SIGNATURE_HEX together.  The eventual release-key ceremony
replaces this verify-only test material in exactly the same way.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Any, Iterable

try:
    from .cache_contract_v3 import (
        FORMAT, SIGNATURE_CONTEXT, VERSION, canonical_bytes, validate_document)
    from .generate_cache_v3_production_descriptor import canonical_bytes as descriptor_bytes
    from .generate_frontend_entry_descriptor import EXPECTED_RESOURCE_GRAPH
    from .retail_identity import typed_export_identities
except ImportError:
    from cache_contract_v3 import (  # type: ignore
        FORMAT, SIGNATURE_CONTEXT, VERSION, canonical_bytes, validate_document)
    from generate_cache_v3_production_descriptor import (  # type: ignore
        canonical_bytes as descriptor_bytes)
    from generate_frontend_entry_descriptor import EXPECTED_RESOURCE_GRAPH  # type: ignore
    from retail_identity import typed_export_identities  # type: ignore


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools"))
from wot_config import original_dir  # noqa: E402
DEFAULT_DESCRIPTOR = REPOSITORY_ROOT / "Build/WoTRelease/cache-v3-production-descriptor.json"
DEFAULT_RETAIL_ROOT = original_dir(required=False)
DEFAULT_BSP_CENSUS = REPOSITORY_ROOT / "Tools/dist/fixtures/bsp-native-parity-v1.json"
DEFAULT_DOCUMENT = REPOSITORY_ROOT / "Build/WoTRelease/cache-contract-v3-production.json"
DEFAULT_HEADER = (REPOSITORY_ROOT /
                  "Source/WoTRetailReader/Public/WoTRetailReader/Generated/"
                  "CacheContractProduction.generated.h")

KEY_ID = "cache-contract-production-test-material"
RELEASE_SEQUENCE = 1
# Filled only by the one-time ceremony described above.  These are public,
# verify-only values; no seed/private scalar has a representation in this tool.
PUBLIC_KEY_HEX = "8f458986fb59e701dbe4d861a272c31ac11afd0d984421e73e4f9b62b06d1cfb"
PAYLOAD_SHA256 = "2f7bfbc1fe3761dd8bef60bb647312acd2ccaaefcb28936368b5808c7be39bf2"
SIGNATURE_HEX = (
	"b45b598d2aeb241b55d57ef7db371995f32027a07dbd15858232bb5d14be58930"
	"52af921b6db7f83a2873f3ee9cbfc319a034b3ab1ecc86febfad55d1b9bae03")

DESCRIPTOR_OUTPUT_FIELDS = frozenset({
    "audit_rule", "domain", "importer", "logical_id", "max_decoded_size",
    "runtime_consumer", "schema",
})
PROCEDURAL_CLASSES = frozenset({
    "FireTexture", "WaterTexture", "WaveTexture", "WetTexture", "IceTexture",
})
MAX_EMITTED_LITERAL_CHARS = 16_000
STARTUP_CRITICAL_DECLARATION_FIELDS = frozenset({
    "domain", "runtime_consumer", "source_export", "source_package",
})
STARTUP_CRITICAL_GAMEPLAY_ONLY_DOMAINS = frozenset({"class_default"})
# Authored source identity plus its checked runtime consumer is the authority.
# Logical IDs and generated blob hashes are deliberately not declared here.
_STARTUP_CONSUMERS = {
    "animation": "UWoTVertexAnimComponent",
    "bsp": "UWoTPayloadSubsystem",
    "font": "FWoTFrontEndFontOwner",
    "localization": "FWoTLocalizationRegistry",
    "mesh": "UWoTPayloadSubsystem",
    "procedural": "UWoTFractalSubsystem",
    "sound": "UWoTPayloadSubsystem",
    "texture": "UWoTPayloadSubsystem",
    "world": "UWoTPayloadSubsystem",
}
STARTUP_CRITICAL_SOURCE_DECLARATIONS = tuple({
    "domain": domain,
    "runtime_consumer": _STARTUP_CONSUMERS[domain],
    "source_export": source_export,
    "source_package": source_package,
} for domain, source_package, source_export in (*EXPECTED_RESOURCE_GRAPH, (
    "bsp", "Maps/WheelOfTime.wot", "WheelOfTime.Model"), (
    "world", "Maps/WheelOfTime.wot", "WheelOfTime.Level")))


class ProductionContractError(ValueError):
    pass


def startup_critical_logical_ids_sha256(logical_ids: list[str]) -> str:
    digest = hashlib.sha256()
    for logical_id in logical_ids:
        encoded = logical_id.encode("utf-8")
        digest.update(struct.pack("<Q", len(encoded)))
        digest.update(encoded)
    return digest.hexdigest()


def derive_startup_critical_logical_ids(
        facts: list[dict[str, Any]],
        declarations: Iterable[dict[str, str]]) -> list[str]:
    declarations_value = list(declarations)
    if not declarations_value:
        raise ProductionContractError("startup-critical declarations are empty")
    seen_declarations: set[tuple[str, str, str, str]] = set()
    selected: list[str] = []
    for index, declaration in enumerate(declarations_value):
        if (not isinstance(declaration, dict)
                or set(declaration) != STARTUP_CRITICAL_DECLARATION_FIELDS
                or not all(isinstance(declaration[field], str)
                           and declaration[field]
                           for field in STARTUP_CRITICAL_DECLARATION_FIELDS)):
            raise ProductionContractError(
                f"startup-critical declaration {index} is not closed")
        identity = tuple(declaration[field] for field in (
            "source_package", "source_export", "domain", "runtime_consumer"))
        if identity in seen_declarations:
            raise ProductionContractError(
                f"startup-critical declaration {index} is duplicate")
        seen_declarations.add(identity)
        if declaration["domain"] in STARTUP_CRITICAL_GAMEPLAY_ONLY_DOMAINS:
            raise ProductionContractError(
                f"startup-critical declaration {index} is gameplay-only")
        if (declaration["domain"] in {"bsp", "world"}
                and (declaration["source_package"], declaration["source_export"])
                not in {("Maps/WheelOfTime.wot", "WheelOfTime.Model"),
                        ("Maps/WheelOfTime.wot", "WheelOfTime.Level")}):
            raise ProductionContractError(
                f"startup-critical declaration {index} is gameplay-only")
        matches = [fact for fact in facts
                   if fact.get("domain") == declaration["domain"]
                   and fact.get("source", {}).get("package")
                   == declaration["source_package"]
                   and fact.get("source", {}).get("export")
                   == declaration["source_export"]]
        if len(matches) != 1:
            raise ProductionContractError(
                f"startup-critical declaration {index} does not resolve exactly once")
        fact = matches[0]
        if fact.get("runtime_consumer") != declaration["runtime_consumer"]:
            raise ProductionContractError(
                f"startup-critical declaration {index} runtime consumer drifted")
        outputs = fact.get("outputs")
        if not isinstance(outputs, list) or not outputs:
            raise ProductionContractError(
                f"startup-critical declaration {index} has no outputs")
        for output in outputs:
            logical_id = output.get("logical_id") if isinstance(output, dict) else None
            if (not isinstance(logical_id, str) or not logical_id.startswith("cache/")
                    or "\\" in logical_id or "/../" in f"/{logical_id}/"):
                raise ProductionContractError(
                    f"startup-critical declaration {index} has an invalid logical ID")
            selected.append(logical_id)
    if len(selected) != len(set(selected)):
        raise ProductionContractError("startup-critical closure has duplicate logical IDs")
    all_outputs = [output.get("logical_id") for fact in facts
                   for output in fact.get("outputs", [])
                   if isinstance(output, dict)]
    for logical_id in selected:
        if all_outputs.count(logical_id) != 1:
            raise ProductionContractError(
                f"startup-critical logical ID {logical_id!r} is ambiguous")
    return sorted(selected)


def _load_canonical_object(path: Path, what: str) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
        value = json.loads(raw.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ProductionContractError(f"cannot read {what} {path}: {exc}") from exc
    if not isinstance(value, dict) or descriptor_bytes(value) != raw:
        raise ProductionContractError(f"{what} is not a canonical JSON object: {path}")
    return value


def load_descriptor_outputs(path: Path) -> list[dict[str, Any]]:
    descriptor = _load_canonical_object(path, "production descriptor")
    if descriptor.get("format") != "cache-v3-production-descriptor-v1":
        raise ProductionContractError("production descriptor has the wrong format")
    outputs = descriptor.get("importer_outputs")
    if not isinstance(outputs, list) or not outputs:
        raise ProductionContractError("production descriptor has no importer closure")
    seen_domains: set[str] = set()
    result: list[dict[str, Any]] = []
    for index, output in enumerate(outputs):
        if not isinstance(output, dict) or set(output) != DESCRIPTOR_OUTPUT_FIELDS:
            raise ProductionContractError(
                f"production descriptor importer_outputs[{index}] is not closed")
        domain = output.get("domain")
        if (not isinstance(domain, str) or not domain or domain in seen_domains
                or not all(isinstance(output[field], str) and output[field]
                           for field in DESCRIPTOR_OUTPUT_FIELDS
                           - {"max_decoded_size"})
                or isinstance(output["max_decoded_size"], bool)
                or not isinstance(output["max_decoded_size"], int)
                or output["max_decoded_size"] <= 0):
            raise ProductionContractError(
                f"production descriptor importer_outputs[{index}] is invalid")
        seen_domains.add(domain)
        result.append(output)
    return result


def _import_retail_modules() -> tuple[Any, Any, Any]:
    convert = REPOSITORY_ROOT / "Tools/convert"
    import_root = REPOSITORY_ROOT / "Tools/import"
    dist = REPOSITORY_ROOT / "Tools/dist"
    for path in (convert, import_root, dist):
        value = str(path)
        if value not in sys.path:
            sys.path.insert(0, value)
    try:
        from ue1pkg import Package  # type: ignore
        from build_payload_cache import source_packages  # type: ignore
        from import_all import MP_MAPS  # type: ignore
    except ImportError as exc:
        raise ProductionContractError(f"cannot load the production retail census: {exc}") \
            from exc
    return Package, source_packages, tuple(MP_MAPS)


def _relative_source(retail_root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(retail_root.resolve()).as_posix()
    except ValueError as exc:
        raise ProductionContractError(f"retail source escapes its root: {path}") from exc


def _export_path(package: Any, export_index: int) -> str:
    path = package.objpath(export_index)
    if not isinstance(path, str) or not path:
        raise ProductionContractError("a retail export has no canonical object path")
    return f"{package.name}.{path}"


def _safe_identity(value: str) -> str:
    return value.casefold().replace(" ", "_")


def _typed_exports(package: Any, classes: set[str] | frozenset[str]):
    """Every typed export with its exact cache identity (retail_identity)."""
    try:
        return typed_export_identities(package, classes)
    except ValueError as exc:
        raise ProductionContractError(str(exc)) from exc


def build_payload(descriptor_path: Path = DEFAULT_DESCRIPTOR,
                  retail_root: Path = DEFAULT_RETAIL_ROOT,
                  bsp_census_path: Path = DEFAULT_BSP_CENSUS) -> dict[str, Any]:
    outputs = load_descriptor_outputs(descriptor_path)
    registry = {entry["domain"]: entry for entry in outputs}
    expected_domains = {
        "texture", "sound", "mesh", "animation", "bsp", "world", "font",
        "front_end_world", "procedural", "registry", "localization", "class_default",
    }
    if set(registry) != expected_domains:
        raise ProductionContractError("descriptor is not the complete production domain set")
    Package, source_packages, mp_maps = _import_retail_modules()
    if len(mp_maps) != 19 or len(set(mp_maps)) != 19:
        raise ProductionContractError("production MP_MAPS is not the nineteen-world set")
    if not retail_root.is_dir():
        raise ProductionContractError(f"retail source root is absent: {retail_root}")

    facts: list[dict[str, Any]] = []

    def add_fact(domain: str, fact_id: str, source_path: str,
                 source_export: str, logical_ids: Iterable[str]) -> None:
        binding = registry[domain]
        outputs_value = [{
            "logical_id": logical_id,
            "max_decoded_size": binding["max_decoded_size"],
            "schema": binding["schema"],
        } for logical_id in logical_ids]
        facts.append({
            "audit_rule": binding["audit_rule"],
            "domain": domain,
            "fact_id": fact_id,
            "importer": binding["importer"],
            "outputs": outputs_value,
            "runtime_consumer": binding["runtime_consumer"],
            "source": {"export": source_export, "package": source_path},
        })

    typed_domains = (
        ("texture", "textures", frozenset({"Texture"})),
        ("sound", "sounds", frozenset({"Sound"})),
        ("mesh", "meshes", frozenset({"Mesh", "LodMesh"})),
    )
    for domain, package_kind, classes in typed_domains:
        for path in source_packages(retail_root, package_kind):
            package = Package(path)
            source_path = _relative_source(retail_root, path)
            for identity in _typed_exports(package, classes):
                source_export = _export_path(package, identity.index)
                add_fact(domain, f"retail.{domain}.{identity.fact_key}",
                         source_path, source_export,
                         (f"cache/{domain}/{identity.base}",))
                if domain == "mesh":
                    add_fact("animation", f"retail.animation.{identity.fact_key}",
                             source_path, source_export,
                             (f"cache/animation/{identity.base}",))

    remaining_paths = sorted(
        set(source_packages(retail_root, "textures")),
        key=lambda path: (_relative_source(retail_root, path).casefold(),
                          _relative_source(retail_root, path)))
    for path in remaining_paths:
        package = Package(path)
        source_path = _relative_source(retail_root, path)
        for classes in (frozenset({"Font"}), PROCEDURAL_CLASSES):
            domain = "font" if "Font" in classes else "procedural"
            for identity in _typed_exports(package, classes):
                add_fact(domain, f"retail.{domain}.{identity.fact_key}",
                         source_path, _export_path(package, identity.index),
                         (f"cache/{domain}/{identity.base}",))

    system_packages = sorted(
        (retail_root / "System").glob("*.u"),
        key=lambda path: (path.name.casefold(), path.name))
    if len(system_packages) != 20:
        raise ProductionContractError(
            f"production System package census is {len(system_packages)}, expected 20")
    for path in system_packages:
        package = Package(path)
        source_path = _relative_source(retail_root, path)
        for domain in ("registry", "class_default"):
            add_fact(domain,
                     f"retail.{domain}.{_safe_identity(package.name)}",
                     source_path, f"{package.name}.Package",
                     (f"cache/{domain}/{package.name}",))

    localizations = sorted(
        (retail_root / "System").glob("*.int"),
        key=lambda path: (path.name.casefold(), path.name))
    if len(localizations) != 29:
        raise ProductionContractError(
            f"production localization census is {len(localizations)}, expected 29")
    for path in localizations:
        source_path = _relative_source(retail_root, path)
        add_fact("localization",
                 f"retail.localization.{_safe_identity(path.name)}",
                 source_path, path.name,
                 (f"cache/localization/{path.name}",))

    census = _load_canonical_object(bsp_census_path, "BSP production census")
    maps = census.get("maps")
    if (not isinstance(maps, list)
            or tuple(entry.get("identity") for entry in maps
                     if isinstance(entry, dict)) != mp_maps):
        raise ProductionContractError(
            "BSP census does not match import_all.MP_MAPS in canonical order")
    for entry in maps:
        identity = entry["identity"]
        parts = entry.get("parts")
        if (not isinstance(parts, list) or not parts
                or any(not isinstance(part, dict)
                       or not isinstance(part.get("part_identity"), str)
                       or not part["part_identity"] for part in parts)):
            raise ProductionContractError(f"BSP census for {identity} has no part closure")
        source_path = f"Maps/{identity}.wot"
        add_fact("bsp", f"retail.bsp.{identity}", source_path,
                 f"{identity}.Model",
                 (f"cache/bsp/{identity}/{part['part_identity']}" for part in parts))
        add_fact("world", f"retail.world.{identity}", source_path,
                 f"{identity}.Level", (f"cache/world/{identity}",))

    entry_source = "Maps/WheelOfTime.wot"
    add_fact("bsp", "retail.bsp.WheelOfTime", entry_source,
             "WheelOfTime.Model", ("cache/bsp/WheelOfTime/WheelOfTime",))
    add_fact("world", "retail.world.WheelOfTime", entry_source,
             "WheelOfTime.Level", ("cache/world/WheelOfTime",))
    add_fact("front_end_world", "retail.front_end_world.WheelOfTime",
             entry_source, "WheelOfTime.Level",
             ("cache/front_end_world/WheelOfTime",))

    facts.sort(key=lambda fact: (
        fact["domain"], fact["source"]["package"].casefold(),
        fact["source"]["package"], fact["source"]["export"].casefold(),
        fact["source"]["export"], fact["fact_id"]))
    fact_ids = [fact["fact_id"] for fact in facts]
    logical_ids = [output["logical_id"] for fact in facts for output in fact["outputs"]]
    if len(fact_ids) != len(set(fact_ids)):
        duplicates = sorted({value for value in fact_ids if fact_ids.count(value) > 1})
        raise ProductionContractError(
            f"production census contains duplicate fact ids: {duplicates[0]}")
    if len(logical_ids) != len(set(logical_ids)):
        duplicates = sorted({value for value in logical_ids
                             if logical_ids.count(value) > 1})
        raise ProductionContractError(
            f"production census contains duplicate logical outputs: {duplicates[0]}")

    startup_critical_logical_ids = derive_startup_critical_logical_ids(
        facts, STARTUP_CRITICAL_SOURCE_DECLARATIONS)
    payload = {
        "audit_set": [{
            "audit_rule": entry["audit_rule"],
            "runtime_consumer": entry["runtime_consumer"],
        } for entry in outputs],
        "contract_id": "wot-production-cache-contract-v3",
        "contract_version": VERSION,
        "facts": facts,
        "format": FORMAT,
        "key_id": KEY_ID,
        "release_sequence": RELEASE_SEQUENCE,
        "startup_critical_logical_ids": startup_critical_logical_ids,
        "startup_critical_logical_ids_sha256":
            startup_critical_logical_ids_sha256(startup_critical_logical_ids),
    }
    # The envelope validator is the schema's executable closed grammar.
    validate_document({"payload": payload, "signature": "0" * 128})
    return payload


def signature_frame(payload_bytes: bytes) -> bytes:
    if SIGNATURE_CONTEXT != "com.aleiby.wot.cache-contract.v3":
        raise ProductionContractError("cache-contract signature context drifted")
    # EWoTSignatureContext::CacheContractV3 is the append-only ordinal 7.
    return b"WOT-SIGNATURE\0" + struct.pack("<HQ", 7, len(payload_bytes)) + payload_bytes


def _byte_values(hex_value: str, expected_bytes: int, what: str) -> list[str]:
    if (len(hex_value) != expected_bytes * 2
            or any(character not in "0123456789abcdef" for character in hex_value)):
        raise ProductionContractError(f"{what} is not canonical lower-case hex")
    return [f"0x{hex_value[index:index + 2]}" for index in range(0, len(hex_value), 2)]


def _raw_chunks(raw: bytes, width: int = MAX_EMITTED_LITERAL_CHARS) -> list[str]:
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        raise ProductionContractError("canonical payload is not ASCII JSON") from exc
    if ")WOTCACHE\"" in text:
        raise ProductionContractError("payload collides with the generated raw-string delimiter")
    chunks = [text[index:index + width] for index in range(0, len(text), width)]
    if (width <= 0
            or any(len(chunk) > MAX_EMITTED_LITERAL_CHARS for chunk in chunks)):
        raise ProductionContractError(
            "a generated C++ literal exceeds the 16,000-character MSVC bound")
    return chunks


def generate_header(payload_bytes: bytes, payload: dict[str, Any]) -> bytes:
    public_values = _byte_values(PUBLIC_KEY_HEX, 32, "production public key")
    signature_values = _byte_values(SIGNATURE_HEX, 64, "production signature")
    chunks = _raw_chunks(payload_bytes)
    lines = [
        "// Generated by Tools/dist/generate_production_cache_contract.py; do not edit.",
        "//",
        "// VERIFY-ONLY TEST-MATERIAL CEREMONY: the signature/public key came from a",
        "// fresh ephemeral Ed25519 key whose private half was destroyed and never",
        "// entered the repository. Any payload change requires a deliberate re-signing",
        "// ceremony replacing payload digest, signature, and public key together. The",
        "// later real release-key ceremony replaces these fields by the same procedure.",
        "#pragma once", "", "#include \"CoreMinimal.h\"", "",
        "namespace WoT::Retail::Generated", "{",
        f'inline constexpr TCHAR ProductionKeyId[] = TEXT("{KEY_ID}");',
        f"inline constexpr uint64 ProductionReleaseSequence = {RELEASE_SEQUENCE}ull;",
        f'inline constexpr TCHAR ProductionPayloadSha256[] = TEXT("{PAYLOAD_SHA256}");',
        f'inline constexpr TCHAR ProductionSignatureContext[] = TEXT("{SIGNATURE_CONTEXT}");',
        f'inline constexpr TCHAR ProductionStartupCriticalLogicalIdsSha256[] = TEXT("{payload["startup_critical_logical_ids_sha256"]}");',
        "inline constexpr const TCHAR* ProductionStartupCriticalLogicalIds[] = {",
        *(f'\tTEXT("{logical_id}"),' for logical_id in
          payload["startup_critical_logical_ids"]),
        "};",
        "inline constexpr uint8 ProductionPublicKey[] = {",
        "\t" + ", ".join(public_values), "};",
        "inline constexpr uint8 ProductionSignature[] = {",
        "\t" + ", ".join(signature_values), "};",
    ]
    for index, chunk in enumerate(chunks):
        lines.extend((
            f"inline constexpr ANSICHAR ProductionPayloadFragment{index}[] =",
            f'R"WOTCACHE({chunk})WOTCACHE";',
        ))
    lines.extend((
        "struct FProductionPayloadFragment",
        "{",
        "\tconst ANSICHAR* Data;",
        "\tuint32 Size;",
        "};",
        "inline constexpr FProductionPayloadFragment ProductionPayloadFragments[] = {",
    ))
    lines.extend(
        f"\t{{ProductionPayloadFragment{index}, "
        f"UE_ARRAY_COUNT(ProductionPayloadFragment{index}) - 1}},"
        for index in range(len(chunks)))
    size_expression = " + ".join(
        f"UE_ARRAY_COUNT(ProductionPayloadFragment{index}) - 1"
        for index in range(len(chunks)))
    lines.extend(("};", "inline constexpr uint32 ProductionPayloadSize =",
                  f"\t{size_expression};",
                  "}  // namespace WoT::Retail::Generated", ""))
    return "\n".join(lines).encode("utf-8")


def generate(descriptor_path: Path = DEFAULT_DESCRIPTOR,
             retail_root: Path = DEFAULT_RETAIL_ROOT,
             bsp_census_path: Path = DEFAULT_BSP_CENSUS
             ) -> tuple[bytes, bytes, dict[str, Any]]:
    payload = build_payload(descriptor_path, retail_root, bsp_census_path)
    payload_bytes = canonical_bytes(payload)
    digest = hashlib.sha256(payload_bytes).hexdigest()
    if digest != PAYLOAD_SHA256:
        raise ProductionContractError(
            "production payload changed and requires the documented fresh-key "
            f"re-signing ceremony (observed {digest})")
    document = {"payload": payload, "signature": SIGNATURE_HEX}
    validate_document(document)
    document_bytes = canonical_bytes(document)
    return document_bytes, generate_header(payload_bytes, payload), payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptor", type=Path, default=DEFAULT_DESCRIPTOR)
    parser.add_argument("--retail-root", type=Path)
    parser.add_argument("--bsp-census", type=Path, default=DEFAULT_BSP_CENSUS)
    parser.add_argument("--document", type=Path, default=DEFAULT_DOCUMENT)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    args.retail_root = args.retail_root or original_dir()
    try:
        document, header, _payload = generate(
            args.descriptor, args.retail_root, args.bsp_census)
        if args.check:
            if args.document.read_bytes() != document:
                raise ProductionContractError(
                    f"production cache contract is stale: {args.document}")
            if args.header.read_bytes() != header:
                raise ProductionContractError(
                    f"production cache contract header is stale: {args.header}")
        else:
            args.document.parent.mkdir(parents=True, exist_ok=True)
            args.header.parent.mkdir(parents=True, exist_ok=True)
            args.document.write_bytes(document)
            args.header.write_bytes(header)
    except (OSError, ProductionContractError, ValueError) as exc:
        print(f"production cache contract: {exc}", file=sys.stderr)
        return 1
    print(f"production cache contract OK: {args.document}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
