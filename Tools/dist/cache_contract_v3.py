#!/usr/bin/env python3
"""Closed grammar and closure checks for signed cache-contract-v3 documents.

The release signature is verified by the native reader.  This stdlib-only
module owns the byte grammar used by release tooling and deliberately does not
pretend that parsing a 128-character signature authenticates it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SCHEMA_PATH = Path(__file__).with_name("cache_contract_v3.schema.json")
SIGNATURE_CONTEXT = "com.aleiby.wot.cache-contract.v3"
# OpenSSL-generated verify-only fixture material. The private half was ephemeral
# and is not in the repository; any fixture payload change requires a deliberate
# re-signing ceremony and digest update.
SYNTHETIC_PUBLIC_KEY = "4a3456a2ed48eea172c70376b6e94a2aba8fedd4fe72f5cb03246413aea6f832"
SYNTHETIC_PAYLOAD_SHA256 = "0a8c8471dfaac7123032c834160483715284afb6f253c541ceaf39fc8d52c1e9"
SYNTHETIC_SIGNATURE = (
	"9c35afac9199b8dc053d49102e03758ca94f8dbcfa50a3927beab2fbbfb70e31"
	"8c20fd06aecdcae00cad36e594ffb29fd886cb3afd74831007d82c6fbbf51808")
# Music remains part of the signed grammar fixture even though the production
# importer registry intentionally has no music output. This record is fixture
# material only; it is never added to the production closure.
RESERVED_SYNTHETIC_MUSIC_IMPORTER = {
    "audit_rule": "audit.music-payload-v1",
    "domain": "music",
    "importer": "music-v1",
    "max_decoded_size": 1024 * 1024 * 1024,
    "output_schema": "wot-music-v3",
    "runtime_consumer": "UWoTPayloadSubsystem",
}


@dataclass(frozen=True)
class ContractGrammar:
    format: str
    version: int
    generic_importer: str
    domains: tuple[str, ...]
    max_document_bytes: int
    max_container_depth: int
    max_container_members: int
    max_string_utf8_bytes: int
    integer_min: int
    integer_max: int


def load_contract_grammar(schema_path: Path) -> ContractGrammar:
    """Load the constants and hostile-input bounds from the checked grammar."""
    try:
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        definitions = schema["$defs"]
        payload = definitions["payload"]["properties"]
        limits = schema["x-wot-contract"]["canonical_json_limits"]
        grammar = ContractGrammar(
            format=payload["format"]["const"],
            version=payload["contract_version"]["const"],
            generic_importer=schema["x-wot-contract"]["generic_importer"],
            domains=tuple(definitions["fact"]["properties"]["domain"]["enum"]),
            max_document_bytes=limits["document_bytes"],
            max_container_depth=limits["container_depth"],
            max_container_members=limits["container_members"],
            max_string_utf8_bytes=limits["string_utf8_bytes"],
            integer_min=limits["integer_min"],
            integer_max=limits["integer_max"],
        )
    except (OSError, UnicodeError, json.JSONDecodeError, KeyError, TypeError) as exc:
        raise RuntimeError(f"cannot load cache-contract-v3 schema {schema_path}: {exc}") \
            from exc
    if (not grammar.format or grammar.version < 0 or not grammar.generic_importer
            or not grammar.domains or len(set(grammar.domains)) != len(grammar.domains)
            or not all(grammar.domains)
            or grammar.max_document_bytes <= 0
            or grammar.max_container_depth <= 0
            or grammar.max_container_members <= 0
            or grammar.max_string_utf8_bytes <= 0
            or grammar.integer_min != -(2**63)
            or grammar.integer_max != 2**63 - 1):
        raise RuntimeError(f"cache-contract-v3 schema constants are invalid: {schema_path}")
    return grammar


GRAMMAR = load_contract_grammar(SCHEMA_PATH)
FORMAT = GRAMMAR.format
VERSION = GRAMMAR.version
GENERIC_IMPORTER = GRAMMAR.generic_importer
DOMAINS = GRAMMAR.domains

# Mirrored from FWoTCanonicalJsonLimits in
# Source/WoTBootstrapCore/Public/WoTBootstrapCore/CanonicalJson.h.  The schema
# records the same values so Python, generated code, and native parsing cannot
# drift silently.
MAX_DOCUMENT_BYTES = GRAMMAR.max_document_bytes
MAX_CONTAINER_DEPTH = GRAMMAR.max_container_depth
MAX_CONTAINER_MEMBERS = GRAMMAR.max_container_members
MAX_STRING_UTF8_BYTES = GRAMMAR.max_string_utf8_bytes
INTEGER_MIN = GRAMMAR.integer_min
INTEGER_MAX = GRAMMAR.integer_max


@dataclass(frozen=True)
class ContractError(ValueError):
    path: str
    detail: str

    def __str__(self) -> str:
        return f"{self.path}: {self.detail}"


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("utf-8")


def startup_critical_logical_ids_sha256(logical_ids: list[str]) -> str:
    """Hash canonical logical IDs with uint64-LE UTF-8 length framing."""
    digest = hashlib.sha256()
    for logical_id in logical_ids:
        encoded = logical_id.encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "little"))
        digest.update(encoded)
    return digest.hexdigest()


def _string_utf8_size(value: str, path: str, grammar: ContractGrammar) -> int:
    size = 0
    index = 0
    while index < len(value):
        codepoint = ord(value[index])
        if codepoint == 0:
            raise ContractError(path, "must not contain NUL")
        if 0xD800 <= codepoint <= 0xDBFF:
            if index + 1 >= len(value) or not 0xDC00 <= ord(value[index + 1]) <= 0xDFFF:
                raise ContractError(path, "contains a lone UTF-16 surrogate")
            size += 4
            index += 2
            continue
        if 0xDC00 <= codepoint <= 0xDFFF:
            raise ContractError(path, "contains a lone UTF-16 surrogate")
        if codepoint <= 0x7F:
            size += 1
        elif codepoint <= 0x7FF:
            size += 2
        elif codepoint <= 0xFFFF:
            size += 3
        else:
            size += 4
        index += 1
    if size > grammar.max_string_utf8_bytes:
        raise ContractError(path, "exceeds the decoded UTF-8 string bound")
    return size


def _validate_json_limits(value: Any, path: str, grammar: ContractGrammar,
                          container_depth: int = 0) -> None:
    if isinstance(value, dict):
        depth = container_depth + 1
        if depth > grammar.max_container_depth:
            raise ContractError(path, "exceeds the container-depth bound")
        if len(value) > grammar.max_container_members:
            raise ContractError(path, "exceeds the object-member bound")
        for key, child in value.items():
            if not isinstance(key, str):
                raise ContractError(path, "object keys must be strings")
            child_path = f"{path}.{key}"
            _string_utf8_size(key, child_path, grammar)
            _validate_json_limits(child, child_path, grammar, depth)
        return
    if isinstance(value, list):
        depth = container_depth + 1
        if depth > grammar.max_container_depth:
            raise ContractError(path, "exceeds the container-depth bound")
        if len(value) > grammar.max_container_members:
            raise ContractError(path, "exceeds the array-element bound")
        for index, child in enumerate(value):
            _validate_json_limits(child, f"{path}[{index}]", grammar, depth)
        return
    if isinstance(value, str):
        _string_utf8_size(value, path, grammar)
        return
    if isinstance(value, bool) or value is None:
        return
    if isinstance(value, int):
        if value < grammar.integer_min or value > grammar.integer_max:
            raise ContractError(path, "exceeds the signed 64-bit contract bound")
        return
    raise ContractError(path, "is not a canonical JSON value")


def _object(value: Any, path: str, fields: set[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ContractError(path, "must be an object")
    unknown = sorted(set(value) - fields)
    if unknown:
        raise ContractError(f"{path}.{unknown[0]}", "unknown field")
    missing = sorted(fields - set(value))
    if missing:
        raise ContractError(f"{path}.{missing[0]}", "missing required field")
    return value


def _string(value: Any, path: str,
            grammar: ContractGrammar = GRAMMAR) -> str:
    if not isinstance(value, str):
        raise ContractError(path, "must be a string")
    if not value:
        raise ContractError(path, "must not be empty")
    _string_utf8_size(value, path, grammar)
    return value


def _unsigned(value: Any, path: str, *, nonzero: bool = False,
              grammar: ContractGrammar = GRAMMAR) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ContractError(path, "must be a non-negative integer")
    if nonzero and value == 0:
        raise ContractError(path, "must be greater than zero")
    if value > grammar.integer_max:
        raise ContractError(path, "exceeds the signed 64-bit contract bound")
    return value


def _source_export(value: Any, path: str,
                   grammar: ContractGrammar = GRAMMAR) -> str:
    identity = _string(value, path, grammar)
    if any(not component for component in identity.split(".")):
        raise ContractError(path,
                            "must not contain an empty export identity component")
    return identity


def _source_package(value: Any, path: str,
                    grammar: ContractGrammar = GRAMMAR) -> str:
    package = _string(value, path, grammar)
    if "\\" in package:
        raise ContractError(path, "must use forward-slash package components")
    components = package.split("/")
    if any(not component for component in components):
        raise ContractError(path, "must not contain an empty package component")

    # The leaf's one optional suffix is a file extension, not part of the UE1
    # package identity. Every identity-bearing component must remain dot-free.
    leaf = components[-1]
    stem, separator, suffix = leaf.rpartition(".")
    if not separator:
        stem = leaf
    elif not stem or not suffix:
        raise ContractError(path, "has an invalid package filename extension")
    identity_components = components[:-1] + [stem]
    if any("." in component for component in identity_components):
        raise ContractError(path,
                            "must not contain a dotted package identity component")
    return package


def validate_payload(payload_value: Any, *, path: str = "payload",
                     grammar: ContractGrammar = GRAMMAR,
                     _limits_checked: bool = False) -> dict[str, Any]:
    """Validate one unsigned payload with the native structural grammar."""
    if not _limits_checked:
        _validate_json_limits(payload_value, path, grammar)
    payload = _object(payload_value, path, {
        "audit_set", "contract_id", "contract_version", "facts", "format",
        "key_id", "release_sequence", "startup_critical_logical_ids",
        "startup_critical_logical_ids_sha256",
    })
    format_path = f"{path}.format"
    if _string(payload["format"], format_path, grammar) != grammar.format:
        raise ContractError(format_path, f"must be {grammar.format!r}")
    version_path = f"{path}.contract_version"
    if _unsigned(payload["contract_version"], version_path,
                 grammar=grammar) != grammar.version:
        raise ContractError(version_path, f"must be {grammar.version}")
    _string(payload["contract_id"], f"{path}.contract_id", grammar)
    _string(payload["key_id"], f"{path}.key_id", grammar)
    _unsigned(payload["release_sequence"], f"{path}.release_sequence",
              grammar=grammar)

    audits_value = payload["audit_set"]
    if not isinstance(audits_value, list):
        raise ContractError(f"{path}.audit_set", "must be an array")
    audit_pairs: set[tuple[str, str]] = set()
    audited_consumers: set[str] = set()
    for index, value in enumerate(audits_value):
        audit_path = f"{path}.audit_set[{index}]"
        audit = _object(value, audit_path, {"audit_rule", "runtime_consumer"})
        rule = _string(audit["audit_rule"], f"{audit_path}.audit_rule", grammar)
        consumer = _string(audit["runtime_consumer"],
                           f"{audit_path}.runtime_consumer", grammar)
        pair = (consumer, rule)
        if pair in audit_pairs:
            raise ContractError(audit_path, "duplicate consumer/audit pair")
        audit_pairs.add(pair)
        audited_consumers.add(consumer)

    facts_value = payload["facts"]
    if not isinstance(facts_value, list):
        raise ContractError(f"{path}.facts", "must be an array")
    seen_ids: set[str] = set()
    output_counts: dict[str, int] = {}
    for index, value in enumerate(facts_value):
        fact_path = f"{path}.facts[{index}]"
        fact = _object(value, fact_path, {
            "audit_rule", "domain", "fact_id", "importer", "outputs",
            "runtime_consumer", "source",
        })
        fact_id = _string(fact["fact_id"], f"{fact_path}.fact_id", grammar)
        if fact_id in seen_ids:
            raise ContractError(f"{fact_path}.fact_id",
                                f"duplicate fact id {fact_id!r}")
        seen_ids.add(fact_id)
        domain = _string(fact["domain"], f"{fact_path}.domain", grammar)
        if domain not in grammar.domains:
            raise ContractError(f"{fact_path}.domain",
                                f"unknown fact domain {domain!r}")
        importer = _string(fact["importer"], f"{fact_path}.importer", grammar)
        consumer = _string(fact["runtime_consumer"],
                           f"{fact_path}.runtime_consumer", grammar)
        rule = _string(fact["audit_rule"], f"{fact_path}.audit_rule", grammar)
        if consumer not in audited_consumers or (consumer, rule) not in audit_pairs:
            raise ContractError(f"{fact_path}.runtime_consumer",
                                "consumer/audit pair is absent from audit_set")

        source = _object(fact["source"], f"{fact_path}.source",
                         {"export", "package"})
        _source_package(source["package"], f"{fact_path}.source.package", grammar)
        _source_export(source["export"], f"{fact_path}.source.export", grammar)

        outputs = fact["outputs"]
        if not isinstance(outputs, list):
            raise ContractError(f"{fact_path}.outputs", "must be an array")
        if importer == grammar.generic_importer and outputs:
            raise ContractError(f"{fact_path}.outputs",
                                "generic authored facts emit no outputs")
        if importer != grammar.generic_importer and not outputs:
            raise ContractError(f"{fact_path}.outputs",
                                "typed importer must declare an output")
        seen_logical_ids: set[str] = set()
        for output_index, output_value in enumerate(outputs):
            output_path = f"{fact_path}.outputs[{output_index}]"
            output = _object(output_value, output_path,
                             {"logical_id", "max_decoded_size", "schema"})
            logical_id = _string(output["logical_id"],
                                 f"{output_path}.logical_id", grammar)
            if logical_id in seen_logical_ids:
                raise ContractError(f"{output_path}.logical_id",
                                    f"duplicate logical output {logical_id!r}")
            seen_logical_ids.add(logical_id)
            output_counts[logical_id] = output_counts.get(logical_id, 0) + 1
            _string(output["schema"], f"{output_path}.schema", grammar)
            _unsigned(output["max_decoded_size"],
                      f"{output_path}.max_decoded_size", nonzero=True,
                      grammar=grammar)
    closure_path = f"{path}.startup_critical_logical_ids"
    closure_value = payload["startup_critical_logical_ids"]
    if not isinstance(closure_value, list):
        raise ContractError(closure_path, "must be an array")
    if not closure_value:
        raise ContractError(closure_path, "must not be empty")
    closure: list[str] = []
    previous: str | None = None
    for index, value in enumerate(closure_value):
        item_path = f"{closure_path}[{index}]"
        logical_id = _string(value, item_path, grammar)
        if previous is not None and logical_id <= previous:
            detail = "is duplicated" if logical_id == previous else "must be sorted"
            raise ContractError(item_path, detail)
        if output_counts.get(logical_id, 0) != 1:
            raise ContractError(item_path,
                                "must resolve to exactly one declared fact output")
        closure.append(logical_id)
        previous = logical_id
    digest_path = f"{path}.startup_critical_logical_ids_sha256"
    digest = _string(payload["startup_critical_logical_ids_sha256"],
                     digest_path, grammar)
    if (len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)):
        raise ContractError(digest_path, "must be 64 lower-case hex characters")
    if digest != startup_critical_logical_ids_sha256(closure):
        raise ContractError(digest_path,
                            "does not match the canonical logical-ID closure")
    return payload


def validate_document(document: Any, *,
                      grammar: ContractGrammar = GRAMMAR,
                      _limits_checked: bool = False) -> dict[str, Any]:
    """Validate and return a cache-v3 envelope without authenticating it."""
    if not _limits_checked:
        _validate_json_limits(document, "contract", grammar)
    envelope = _object(document, "contract", {"payload", "signature"})
    signature = _string(envelope["signature"], "contract.signature", grammar)
    if len(signature) != 128 or any(c not in "0123456789abcdef" for c in signature):
        raise ContractError("contract.signature", "must be 128 lower-case hex characters")
    validate_payload(envelope["payload"], path="contract.payload",
                     grammar=grammar, _limits_checked=True)
    return envelope


def parse_canonical_value(raw: bytes, *, path: str = "contract",
                          grammar: ContractGrammar = GRAMMAR) -> Any:
    if len(raw) > grammar.max_document_bytes:
        raise ContractError(path, "exceeds the canonical document byte bound")
    try:
        text = raw.decode("utf-8")
        document = json.loads(text)
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ContractError(path, f"invalid UTF-8 JSON: {exc}") from exc
    if canonical_bytes(document) != raw:
        raise ContractError(path, "document is not canonical JSON")
    _validate_json_limits(document, path, grammar)
    return document


def parse_canonical(raw: bytes, *,
                    grammar: ContractGrammar = GRAMMAR) -> dict[str, Any]:
    document = parse_canonical_value(raw, grammar=grammar)
    return validate_document(document, grammar=grammar, _limits_checked=True)


def load_contract(path: Path) -> dict[str, Any]:
    try:
        return parse_canonical(path.read_bytes())
    except OSError as exc:
        raise ContractError("contract", f"cannot read {path}: {exc}") from exc


def emit_synthetic_fixture(importer_spec: Path, output: Path) -> None:
    """Emit all grammar domains from production plus reserved music material."""
    try:
        spec = json.loads(importer_spec.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ContractError("importer_spec", f"cannot parse {importer_spec}: {exc}") from exc
    if not isinstance(spec, dict) or not isinstance(spec.get("importers"), list):
        raise ContractError("importer_spec.importers", "must be an array")
    fixture_importers = list(spec["importers"])
    domains = {entry.get("domain") for entry in fixture_importers
               if isinstance(entry, dict)}
    if "music" in domains:
        raise ContractError(
            "importer_spec.importers",
            "production registry must not declare the reserved music domain")
    sound_index = next((index for index, entry in enumerate(fixture_importers)
                        if isinstance(entry, dict)
                        and entry.get("domain") == "sound"), None)
    if sound_index is None:
        raise ContractError("importer_spec.importers", "must declare sound")
    fixture_importers.insert(
        sound_index + 1, dict(RESERVED_SYNTHETIC_MUSIC_IMPORTER))

    audits = []
    facts = []
    for index, entry in enumerate(fixture_importers):
        if not isinstance(entry, dict):
            raise ContractError(f"importer_spec.importers[{index}]", "must be an object")
        try:
            domain = entry["domain"]
            rule = entry["audit_rule"]
            consumer = entry["runtime_consumer"]
            importer = entry["importer"]
            schema = entry["output_schema"]
            limit = entry["max_decoded_size"]
        except KeyError as exc:
            raise ContractError(f"importer_spec.importers[{index}].{exc.args[0]}",
                                "missing required field") from exc
        audits.append({"audit_rule": rule, "runtime_consumer": consumer})
        facts.append({
            "audit_rule": rule, "domain": domain,
            "fact_id": f"synthetic.{domain}", "importer": importer,
            "outputs": [{"logical_id": f"cache/{domain}/synthetic",
                         "max_decoded_size": limit, "schema": schema}],
            "runtime_consumer": consumer,
            "source": {"export": f"Synthetic.{domain}",
                       "package": f"Synthetic/{domain}.u"},
        })
    payload = {"audit_set": audits,
               "contract_id": "synthetic-cache-contract-v3",
               "contract_version": VERSION, "facts": facts,
               "format": FORMAT, "key_id": "cache-contract-test",
               "release_sequence": 19,
               "startup_critical_logical_ids": ["cache/localization/synthetic"],
               "startup_critical_logical_ids_sha256":
                   startup_critical_logical_ids_sha256(
                       ["cache/localization/synthetic"])}
    payload_digest = hashlib.sha256(canonical_bytes(payload)).hexdigest()
    if payload_digest != SYNTHETIC_PAYLOAD_SHA256:
        raise ContractError(
            "contract.payload",
            "synthetic payload changed and must be re-signed with a new ephemeral key")
    document = {"payload": payload, "signature": SYNTHETIC_SIGNATURE}
    validate_document(document)
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(canonical_bytes(document))
    except OSError as exc:
        raise ContractError("contract", f"cannot write {output}: {exc}") from exc


def _verify_manifest(contract: dict[str, Any], manifest_path: Path) -> None:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ContractError("manifest", f"cannot parse {manifest_path}: {exc}") from exc
    if not isinstance(manifest, dict) or not isinstance(manifest.get("blobs"), list):
        raise ContractError("manifest.blobs", "must be an array")
    satisfied: set[str] = set()
    for index, blob in enumerate(manifest["blobs"]):
        if not isinstance(blob, dict) or not isinstance(blob.get("satisfies"), list):
            raise ContractError(f"manifest.blobs[{index}].satisfies", "must be an array")
        satisfied.update(item for item in blob["satisfies"] if isinstance(item, str))
    for index, fact in enumerate(contract["payload"]["facts"]):
        if fact["importer"] != GENERIC_IMPORTER and fact["fact_id"] not in satisfied:
            raise ContractError(f"contract.payload.facts[{index}].fact_id",
                                "is not satisfied by the cache manifest")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--contract", required=True, type=Path)
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--contract", required=True, type=Path)
    verify_parser.add_argument("--manifest", required=True, type=Path)
    fixture_parser = subparsers.add_parser("emit-synthetic-fixture")
    fixture_parser.add_argument("--importer-spec", required=True, type=Path)
    fixture_parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "emit-synthetic-fixture":
            emit_synthetic_fixture(args.importer_spec, args.output)
            print(f"cache-contract-v3 fixture OK: {args.output}")
            return 0
        contract = load_contract(args.contract)
        if args.command == "verify":
            _verify_manifest(contract, args.manifest)
    except ContractError as exc:
        print(f"cache-contract-v3: {exc}", file=sys.stderr)
        return 1
    print(f"cache-contract-v3 OK: {args.contract}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
