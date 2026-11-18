from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cache_v3_production_closure import check_descriptor
from generate_cache_v3_production_descriptor import (
    build_descriptor, canonical_bytes)

ROOT = Path(__file__).resolve().parents[2]


class CacheV3ProductionClosureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.spec = root / "outputs.json"
        self.target = root / "Source/WoT.Target.cs"
        self.source = root / "Source/WoT"
        self.launch = root / "Source/WoTLaunch"
        self.config = root / "Config"
        self.cook = root / "cook.json"
        self.descriptor = root / "descriptor.json"
        self.source.mkdir(parents=True)
        self.launch.mkdir(parents=True)
        self.config.mkdir()
        self.domains = (
            "texture", "sound", "mesh", "animation", "bsp",
            "world", "font", "procedural", "registry", "localization",
            "class_default", "front_end_world",
        )
        spec = {"format": "cache-v3-importer-outputs-v1", "importers": [{
            "audit_rule": f"audit.{domain}", "domain": domain,
            "importer": f"{domain}-v1", "max_decoded_size": 1024,
            "output_schema": f"wot-{domain}-v3",
            "runtime_consumer": f"U{domain.title()}Consumer",
        } for domain in self.domains]}
        self.spec.write_bytes(canonical_bytes(spec))
        self.target.write_text(
            'ExtraModuleNames.Add("WoT");\n'
            'if (Target.Configuration == UnrealTargetConfiguration.Shipping)\n'
            '{\n\tExtraModuleNames.Add("WoTLaunch");\n}\n',
            encoding="utf-8")
        (self.source / "WoT.Build.cs").write_text(
            'PrivateDependencyModuleNames.Add("WoTLaunch");\n', encoding="utf-8")
        (self.launch / "WoTLaunch.Build.cs").write_text(
            'PublicDependencyModuleNames.Add("Core");\n', encoding="utf-8")
        definitions = "\n".join(
            f"class U{domain.title()}Consumer {{}};" for domain in self.domains)
        (self.source / "Consumer.h").write_text(definitions + "\n", encoding="utf-8")
        (self.source / "Consumer.cpp").write_text(
            '#include "Consumer.h"\nconst TCHAR* P=TEXT("/Game/WoT/T.T");\n',
            encoding="utf-8")
        (self.config / "DefaultGame.ini").write_text(
            "[/Script/Engine.AssetManagerSettings]\n"
            "+PrimaryAssetTypesToScan=(PrimaryAssetType=Map)\n", encoding="utf-8")
        self.write_cook()
        self.refresh()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def cook_value(self) -> dict[str, object]:
        spec = json.loads(self.spec.read_text(encoding="utf-8"))
        outputs = [{
            "audit_rule": entry["audit_rule"],
            "importer": entry["importer"],
            "logical_id": f"cache/{entry['domain']}/synthetic",
            "runtime_consumer": entry["runtime_consumer"],
            "schema": entry["output_schema"],
        } for entry in spec["importers"]]
        return {"outputs": outputs, "references": [{
            "consumer": "/Game/WoT/Maps/Arena_01",
            "reference": outputs[0]["logical_id"],
        }]}

    def write_cook(self, value: dict[str, object] | None = None) -> None:
        self.cook.write_bytes(canonical_bytes(value or self.cook_value()))

    def refresh(self, cook: Path | None = None) -> None:
        value = build_descriptor(
            self.spec, self.target, self.source, self.config, cook)
        self.descriptor.write_bytes(canonical_bytes(value))

    def problems(self, *, cook: Path | None = None,
                 reject: bool = False) -> list[str]:
        return check_descriptor(self.descriptor, self.spec, self.target,
                                self.source, self.config, cook, reject)

    def test_checked_in_descriptor_matches_every_input(self) -> None:
        self.assertEqual([], self.problems())
        descriptor = json.loads(self.descriptor.read_text(encoding="utf-8"))
        self.assertEqual("not_yet_cooked",
                         descriptor["graph"]["cook_reference_graph"]["status"])
        self.assertTrue(descriptor["graph"]["source"]["includes"])
        self.assertTrue(descriptor["graph"]["source"]["soft_references"])
        self.assertTrue(descriptor["graph"]["source"]["symbols"])
        self.assertTrue(descriptor["graph"]["config"]["primary_asset_rules"])
        self.assertTrue(all("logical_id" in output and "schema" in output
                            for output in descriptor["importer_outputs"]))

    def test_repository_descriptor_covers_the_real_shipping_graph(self) -> None:
        expected = build_descriptor(
            ROOT / "Tools/dist/cache-v3-importer-outputs-v1.json",
            ROOT / "Source/WoT.Target.cs", ROOT / "Source/WoT", ROOT / "Config")
        actual = (ROOT / "Build/WoTRelease/cache-v3-production-descriptor.json").read_bytes()
        self.assertEqual(canonical_bytes(expected), actual)
        # The walked closure is an inventory digest, not per-file content
        # hashes: hashing contents made an unrelated comment edit invalidate a
        # release-integrity artifact. The count still proves the walk reached
        # the real Shipping tree rather than a stub.
        self.assertGreater(expected["graph"]["source"]["file_inventory"]["count"], 100)
        self.assertRegex(
            expected["graph"]["source"]["file_inventory"]["sha256"], r"^[0-9a-f]{64}$")
        module_names = {entry["name"]
                        for entry in expected["graph"]["source"]["modules"]}
        self.assertIn("WoTLaunch", module_names)
        self.assertIn("WoTRetailReader", module_names)
        balefire_references = [
            entry["reference"] for entry in expected["graph"]["source"]["soft_references"]
            if entry["consumer"] == "Source/WoT/WoTBalefireFX.cpp"]
        joined = ("/Game/WoT/Materials/M_WoT_MeshTranslucent."
                  "M_WoT_MeshTranslucent")
        self.assertEqual(1, balefire_references.count(joined))
        self.assertNotIn("/Game/WoT/Materials/M_WoT_MeshTranslucent.",
                         balefire_references)

    def test_repository_registry_declares_no_music_output(self) -> None:
        spec = json.loads((
            ROOT / "Tools/dist/cache-v3-importer-outputs-v1.json"
        ).read_text(encoding="utf-8"))
        serialized = json.dumps(spec["importers"], sort_keys=True)
        self.assertFalse(any(entry["domain"] == "music"
                             for entry in spec["importers"]))
        self.assertNotIn("music-v1", serialized)
        self.assertNotIn("wot-music-v3", serialized)

    def test_repository_registry_declares_the_separate_front_end_world_output(self) -> None:
        spec = json.loads((
            ROOT / "Tools/dist/cache-v3-importer-outputs-v1.json"
        ).read_text(encoding="utf-8"))
        entries = [entry for entry in spec["importers"]
                   if entry["importer"] == "front-end-world-v1"]
        self.assertEqual(entries, [{
            "audit_rule": "audit.front-end-world-payload-v1",
            "domain": "front_end_world",
            "importer": "front-end-world-v1",
            "max_decoded_size": 4194304,
            "output_schema": "wot-front-end-world-v3",
            "runtime_consumer": "FWoTCacheGenerationLease",
        }])

    def test_removing_an_output_makes_descriptor_stale(self) -> None:
        spec = json.loads(self.spec.read_text(encoding="utf-8"))
        spec["importers"].clear()
        self.spec.write_bytes(canonical_bytes(spec))
        self.assertTrue(self.problems())

    def test_adding_code_config_or_package_consumer_makes_it_stale(self) -> None:
        additions = (
            (self.source / "Added.cpp", '#include "Added.h"\n'),
            (self.source / "Package.cpp",
             'const TCHAR* P=TEXT("/Game/WoT/New.New");\n'),
            (self.config / "Added.ini", "MapsToCook=/Game/WoT/New\n"),
        )
        for path, content in additions:
            with self.subTest(path=path.name):
                path.write_text(content, encoding="utf-8")
                self.assertTrue(self.problems())
                path.unlink()
                self.assertEqual([], self.problems())

    def test_shipping_launch_module_reference_makes_descriptor_stale(self) -> None:
        added = self.launch / "Added.cpp"
        added.write_text(
            'const TCHAR* P = TEXT("/Game/WoT/LaunchOnly.LaunchOnly");\n',
            encoding="utf-8")
        self.assertTrue(self.problems())

    def test_adjacent_cpp_literals_are_joined_once(self) -> None:
        joined = "/Game/WoT/Materials/M.M"
        (self.source / "Joined.cpp").write_text(
            'const TCHAR* P = TEXT("/Game/WoT/Materials/M." /* join */ "M");\n',
            encoding="utf-8")
        value = build_descriptor(self.spec, self.target, self.source, self.config)
        references = [entry["reference"]
                      for entry in value["graph"]["source"]["soft_references"]]
        self.assertEqual(1, references.count(joined))
        self.assertNotIn("/Game/WoT/Materials/M.", references)

    def test_declaring_either_side_without_counterpart_is_stale(self) -> None:
        value = json.loads(self.descriptor.read_text(encoding="utf-8"))
        value["importer_outputs"].clear()
        self.descriptor.write_bytes(canonical_bytes(value))
        self.assertTrue(self.problems())
        self.refresh()
        spec = json.loads(self.spec.read_text(encoding="utf-8"))
        spec["importers"][0]["output_schema"] = "wot-texture-v4"
        self.spec.write_bytes(canonical_bytes(spec))
        self.assertTrue(self.problems())

    def test_reject_undeclared_is_a_real_cook_gate(self) -> None:
        self.assertTrue(self.problems(reject=True))
        self.assertTrue(self.problems(cook=self.cook, reject=True),
                        "the not-yet-cooked descriptor must be stale")
        self.refresh(self.cook)
        self.assertEqual([], self.problems(cook=self.cook, reject=True))

    def test_orphan_consumer_fails_after_descriptor_regeneration(self) -> None:
        spec = json.loads(self.spec.read_text(encoding="utf-8"))
        spec["importers"][0]["runtime_consumer"] = "UOrphanConsumer"
        self.spec.write_bytes(canonical_bytes(spec))
        self.write_cook()
        self.refresh(self.cook)
        problems = self.problems(cook=self.cook, reject=True)
        self.assertTrue(any("UOrphanConsumer" in problem for problem in problems))

    def test_orphan_output_fails_after_descriptor_regeneration(self) -> None:
        cook = self.cook_value()
        cook["outputs"].append({
            "audit_rule": "audit.orphan",
            "importer": "orphan-v1",
            "logical_id": "cache/orphan/synthetic",
            "runtime_consumer": "UTextureConsumer",
            "schema": "wot-orphan-v3",
        })
        self.write_cook(cook)
        self.refresh(self.cook)
        problems = self.problems(cook=self.cook, reject=True)
        self.assertTrue(any("cache/orphan/synthetic" in problem
                            for problem in problems))

    def test_missing_cooked_output_and_unmapped_reference_fail_after_refresh(self) -> None:
        cook = self.cook_value()
        cook["outputs"].pop()
        cook["references"].append({
            "consumer": "/Game/WoT/Maps/Arena_01",
            "reference": "cache/texture/not-declared",
        })
        self.write_cook(cook)
        self.refresh(self.cook)
        problems = self.problems(cook=self.cook, reject=True)
        self.assertTrue(any("absent from the cooked outputs" in problem
                            for problem in problems))
        self.assertTrue(any("undeclared cache output" in problem
                            for problem in problems))


if __name__ == "__main__":
    unittest.main()
