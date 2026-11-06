from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cache_contract_v3 import (
    ContractError, DOMAINS, GENERIC_IMPORTER, MAX_CONTAINER_DEPTH,
    MAX_CONTAINER_MEMBERS, MAX_DOCUMENT_BYTES, MAX_STRING_UTF8_BYTES,
    canonical_bytes, emit_synthetic_fixture, load_contract_grammar, parse_canonical,
    parse_canonical_value, validate_document,
)
from generate_cache_contract_header import generate, main as generate_header_main

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "Tools/dist/fixtures/cache-contract-v3-synthetic.json"


class CacheContractV3Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.document = json.loads(FIXTURE.read_text(encoding="utf-8"))

    def assert_path(self, document: object, path: str) -> None:
        with self.assertRaises(ContractError) as raised:
            validate_document(document)
        self.assertEqual(path, raised.exception.path)

    def test_fixture_is_canonical_and_covers_every_domain(self) -> None:
        raw = FIXTURE.read_bytes()
        parsed = parse_canonical(raw)
        self.assertEqual(raw, canonical_bytes(parsed))
        self.assertEqual(set(DOMAINS),
                         {fact["domain"] for fact in parsed["payload"]["facts"]})
        self.assertEqual(len(DOMAINS), len(parsed["payload"]["facts"]))

    def test_fixture_reproduces_with_music_reserved_outside_production(self) -> None:
        importer_spec = ROOT / "Tools/dist/cache-v3-importer-outputs-v1.json"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "synthetic.json"
            emit_synthetic_fixture(importer_spec, output)
            self.assertEqual(FIXTURE.read_bytes(), output.read_bytes())

    def test_required_fact_field_paths_are_stable(self) -> None:
        mutations = (
            (("importer",), "contract.payload.facts[0].importer"),
            (("runtime_consumer",), "contract.payload.facts[0].runtime_consumer"),
            (("audit_rule",), "contract.payload.facts[0].audit_rule"),
            (("source", "package"), "contract.payload.facts[0].source.package"),
            (("outputs", 0, "schema"), "contract.payload.facts[0].outputs[0].schema"),
            (("outputs", 0, "max_decoded_size"),
             "contract.payload.facts[0].outputs[0].max_decoded_size"),
        )
        for selectors, expected in mutations:
            with self.subTest(path=expected):
                changed = copy.deepcopy(self.document)
                target = changed["payload"]["facts"][0]
                for selector in selectors[:-1]:
                    target = target[selector]
                del target[selectors[-1]]
                self.assert_path(changed, expected)

    def test_closed_shapes_reject_unknown_fields(self) -> None:
        for target_path in ("contract", "contract.payload",
                            "contract.payload.facts[0]",
                            "contract.payload.facts[0].source",
                            "contract.payload.facts[0].outputs[0]"):
            with self.subTest(target=target_path):
                changed = copy.deepcopy(self.document)
                target = changed
                for part in target_path.split(".")[1:]:
                    if part == "facts[0]":
                        target = target["facts"][0]
                    elif part == "outputs[0]":
                        target = target["outputs"][0]
                    else:
                        target = target[part]
                target["unknown"] = True
                self.assert_path(changed, f"{target_path}.unknown")

    def test_duplicate_fact_ids_are_rejected(self) -> None:
        changed = copy.deepcopy(self.document)
        changed["payload"]["facts"][1]["fact_id"] = \
            changed["payload"]["facts"][0]["fact_id"]
        self.assert_path(changed, "contract.payload.facts[1].fact_id")

    def test_generic_fact_cannot_emit_output(self) -> None:
        changed = copy.deepcopy(self.document)
        changed["payload"]["facts"][0]["importer"] = GENERIC_IMPORTER
        self.assert_path(changed, "contract.payload.facts[0].outputs")

    def test_consumer_must_be_in_matching_audit_set_entry(self) -> None:
        changed = copy.deepcopy(self.document)
        changed["payload"]["facts"][0]["runtime_consumer"] = "MissingConsumer"
        self.assert_path(changed, "contract.payload.facts[0].runtime_consumer")

    def test_source_identity_components_have_stable_rejection_paths(self) -> None:
        cases = (
            ("export", ".Synthetic.texture",
             "contract.payload.facts[0].source.export"),
            ("export", "Synthetic..texture",
             "contract.payload.facts[0].source.export"),
            ("export", "Synthetic.texture.",
             "contract.payload.facts[0].source.export"),
            ("package", "Synthetic//texture.u",
             "contract.payload.facts[0].source.package"),
            ("package", "Syn.thetic/texture.u",
             "contract.payload.facts[0].source.package"),
            ("package", "Synthetic/tex.ture.u",
             "contract.payload.facts[0].source.package"),
        )
        for field, value, expected_path in cases:
            with self.subTest(field=field, value=value):
                changed = copy.deepcopy(self.document)
                changed["payload"]["facts"][0]["source"][field] = value
                self.assert_path(changed, expected_path)

    def test_noncanonical_bytes_are_rejected(self) -> None:
        with self.assertRaisesRegex(ContractError, "canonical JSON"):
            parse_canonical(FIXTURE.read_bytes() + b"\n")

    def test_nul_and_lone_surrogates_have_stable_field_paths(self) -> None:
        for value in ("embedded\0nul", "lone-high\ud800", "lone-low\udc00"):
            with self.subTest(value=ascii(value)):
                changed = copy.deepcopy(self.document)
                changed["payload"]["facts"][0]["importer"] = value
                self.assert_path(changed, "contract.payload.facts[0].importer")

    def test_canonical_json_limits_match_native_boundaries(self) -> None:
        base_string_size = MAX_DOCUMENT_BYTES // 4 - 4
        exact_document_values = ["a" * (base_string_size + 3)] + [
            "a" * base_string_size] * 3
        exact_document = canonical_bytes(exact_document_values)
        self.assertEqual(MAX_DOCUMENT_BYTES, len(exact_document))
        self.assertEqual(exact_document_values,
                         parse_canonical_value(exact_document))
        with self.assertRaisesRegex(ContractError, "document byte bound"):
            parse_canonical_value(exact_document[:-2] + b'a"]')

        exact_depth: object = 0
        for _ in range(MAX_CONTAINER_DEPTH):
            exact_depth = [exact_depth]
        self.assertEqual(exact_depth,
                         parse_canonical_value(canonical_bytes(exact_depth)))
        over_depth = [exact_depth]
        with self.assertRaisesRegex(ContractError, "container-depth"):
            parse_canonical_value(canonical_bytes(over_depth))

        exact_members = [0] * MAX_CONTAINER_MEMBERS
        self.assertEqual(MAX_CONTAINER_MEMBERS,
                         len(parse_canonical_value(canonical_bytes(exact_members))))
        with self.assertRaisesRegex(ContractError, "array-element"):
            parse_canonical_value(canonical_bytes(exact_members + [0]))

        exact_string = "a" * MAX_STRING_UTF8_BYTES
        self.assertEqual(exact_string,
                         parse_canonical_value(canonical_bytes(exact_string)))
        with self.assertRaisesRegex(ContractError, "UTF-8 string bound"):
            parse_canonical_value(canonical_bytes(exact_string + "a"))

        for number in (-9223372036854775808, 9223372036854775807):
            self.assertEqual(number, parse_canonical_value(canonical_bytes(number)))
        for number in (-9223372036854775809, 9223372036854775808):
            with self.assertRaisesRegex(ContractError, "signed 64-bit"):
                parse_canonical_value(canonical_bytes(number))

    def test_generated_native_schema_header_is_current(self) -> None:
        schema = ROOT / "Tools/dist/cache_contract_v3.schema.json"
        header = (ROOT / "Source/WoTRetailReader/Public/WoTRetailReader/Generated/"
                  "CacheContractV3.generated.h")
        self.assertEqual(generate(schema), header.read_bytes())

    def test_schema_domain_change_drives_header_and_python_grammar(self) -> None:
        schema_path = ROOT / "Tools/dist/cache_contract_v3.schema.json"
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        changed_domain = "texture_changed_for_test"
        schema["$defs"]["fact"]["properties"]["domain"]["enum"][0] = \
            changed_domain
        with tempfile.TemporaryDirectory() as directory:
            changed_schema = Path(directory) / "schema.json"
            changed_schema.write_text(json.dumps(schema), encoding="utf-8")
            self.assertNotEqual(generate(schema_path), generate(changed_schema))
            header = (ROOT / "Source/WoTRetailReader/Public/WoTRetailReader/Generated/"
                      "CacheContractV3.generated.h")
            self.assertEqual(1, generate_header_main([
                "--schema", str(changed_schema), "--output", str(header), "--check",
            ]))
            grammar = load_contract_grammar(changed_schema)
            changed_document = copy.deepcopy(self.document)
            changed_document["payload"]["facts"][0]["domain"] = changed_domain
            validate_document(changed_document, grammar=grammar)
            self.assert_path(changed_document,
                             "contract.payload.facts[0].domain")


if __name__ == "__main__":
    unittest.main()
