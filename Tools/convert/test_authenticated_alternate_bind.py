"""Hostile tests for the one authenticated transient alternate mesh bind."""
import copy
import json
import tempfile
import unittest
from pathlib import Path

try:
    import authenticated_alternate_bind as alternate
except ImportError:
    alternate = None


class AuthenticatedAlternateBindContractTests(unittest.TestCase):
    def setUp(self):
        if alternate is None:
            self.fail("authenticated_alternate_bind contract module is missing")
        self.raw_positions = [(0.1, 0.0, 0.0)] * 38
        self.raw_normals = [(0.0, 0.0, 0.0)] * 38
        self.bind_positions = [(0.0, 0.0, 0.0)] * 38
        self.bind_normals = [(0.0, 0.0, 0.0)] * 38
        # These vectors are overridden per test with the real pinned fixture by
        # producer tests; contract-hostile cases only need a known-good object.
        self.value = {
            "format": "wot-authenticated-alternate-bind-v1",
            "source_identity": "mesh:Angreal__TaintExpGeoB",
            "source_frame_count": 13,
            "bind_frame": 6,
            "target_frames": [0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12],
            "raw_base_positions_sha256":
                "f4a3958685c579d73a7928d3f04fc61d473e7d634341fe795f40bd9d09310aaf",
            "raw_base_normals_sha256":
                "a3e19ecacea8735ce8e283ca6910fc3caa8667b357c4a3a585a374337cc4d81a",
            "bind_positions_sha256":
                "d740b8e7f68faad60a336f5e8a75d0b9081160d3eabc05b6d5e7a3554c15d79c",
            "bind_normals_sha256":
                "85dd91ebb82da4b8a58db98b710c2dcff702d45d85bf2e954b12e3aa5f603f60",
            "bind_positions": self.bind_positions,
            "bind_normals": self.bind_normals,
        }

    def test_exact_contract_rejects_wrong_identity_frame_and_target_order(self):
        for key, bad in (
                ("source_identity", "mesh:Angreal__TaintExpGeoA"),
                ("bind_frame", 5),
                ("target_frames", [0, 1, 2, 3, 4, 5, 8, 7, 9, 10, 11, 12])):
            value = copy.deepcopy(self.value)
            value[key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                alternate.validate_metadata_shape(value)

    def test_exact_contract_rejects_numeric_type_aliases(self):
        variants = []
        for key, bad in (("source_frame_count", 13.0),
                         ("bind_frame", 6.0)):
            value = copy.deepcopy(self.value)
            value[key] = bad
            variants.append(value)
        bool_targets = copy.deepcopy(self.value)
        bool_targets["target_frames"][:2] = [False, True]
        variants.append(bool_targets)
        float_targets = copy.deepcopy(self.value)
        float_targets["target_frames"] = [
            float(frame) for frame in float_targets["target_frames"]]
        variants.append(float_targets)
        for value in variants:
            with self.subTest(value=value), self.assertRaises(ValueError):
                alternate.validate_metadata_shape(value)

    def test_exact_contract_rejects_extra_missing_and_tampered_hash(self):
        variants = []
        extra = copy.deepcopy(self.value)
        extra["unexpected"] = True
        variants.append(extra)
        missing = copy.deepcopy(self.value)
        del missing["bind_normals"]
        variants.append(missing)
        tampered = copy.deepcopy(self.value)
        tampered["bind_positions_sha256"] = "0" * 64
        variants.append(tampered)
        for value in variants:
            with self.subTest(keys=sorted(value)), self.assertRaises(ValueError):
                alternate.validate_metadata_shape(value)

    def test_use_is_forbidden_for_canonical_or_noncarrier_import(self):
        for stripped, carriers in ((False, True), (True, False), (False, False)):
            with self.subTest(stripped=stripped, carriers=carriers), \
                    self.assertRaises(ValueError):
                alternate.require_for_import(
                    self.value, payload_key="mesh:Angreal__TaintExpGeoB",
                    layout={}, stripped=stripped, mapping_carriers=carriers)

    def test_vector_hash_rejects_boolean_alias_for_zero(self):
        with self.assertRaisesRegex(ValueError, "finite xyz triples"):
            alternate.vector_sha256("positions", [(False, 0.0, 0.0)])

    def test_import_receipt_requires_exact_asset_and_stripped_absence(self):
        authority = alternate.authority_receipt()
        receipt = alternate.make_import_receipt(
            authority,
            asset=("/Game/WoT/Meshes/TaintExpGeoB/"
                   "TaintExpGeoB/SkeletalMeshes/"
                   "TaintExpGeoB.TaintExpGeoB"),
            native_morph_target_count=0, has_vertex_colors=False)
        self.assertEqual(alternate.validate_import_receipt(receipt), receipt)
        for key, bad in (("native_morph_target_count", 1),
                         ("has_vertex_colors", True),
                         ("asset", "/Game/WoT/Meshes/TaintExpGeoB/"
                                   "TaintExpGeoB.TaintExpGeoB"),
                         ("asset", "/Game/WoT/Meshes/Other/Other.Other")):
            hostile = copy.deepcopy(receipt)
            hostile[key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                alternate.validate_import_receipt(hostile)

    def test_import_receipt_rejects_legacy_extra_and_tampered_authority(self):
        receipt = alternate.make_import_receipt(
            alternate.authority_receipt(),
            asset=("/Game/WoT/Meshes/TaintExpGeoB/"
                   "TaintExpGeoB/SkeletalMeshes/"
                   "TaintExpGeoB.TaintExpGeoB"),
            native_morph_target_count=0, has_vertex_colors=False)
        variants = []
        legacy = copy.deepcopy(receipt)
        legacy["format"] = "wot-authenticated-alternate-bind-import-v0"
        variants.append(legacy)
        extra = copy.deepcopy(receipt)
        extra["vectors"] = []
        variants.append(extra)
        tampered = copy.deepcopy(receipt)
        tampered["authority"]["bind_frame"] = 5
        variants.append(tampered)
        for value in variants:
            with self.subTest(value=value), self.assertRaises(ValueError):
                alternate.validate_import_receipt(value)

    def test_import_receipt_publication_is_atomic_canonical_json(self):
        receipt = alternate.make_import_receipt(
            alternate.authority_receipt(),
            asset=("/Game/WoT/Meshes/TaintExpGeoB/"
                   "TaintExpGeoB/SkeletalMeshes/"
                   "TaintExpGeoB.TaintExpGeoB"),
            native_morph_target_count=0, has_vertex_colors=False)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "receipts" / "alternate.json"
            alternate.write_import_receipt(path, receipt)
            self.assertEqual(
                path.read_bytes(),
                (json.dumps(receipt, indent=2, sort_keys=True) + "\n")
                .encode("utf-8"))
            self.assertFalse(path.with_name(path.name + ".tmp").exists())


if __name__ == "__main__":
    unittest.main()
