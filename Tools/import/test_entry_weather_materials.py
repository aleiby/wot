import argparse
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import entry_weather_materials as ewm


class EntryWeatherMaterialContractTests(unittest.TestCase):
    @staticmethod
    def _generation_fixture(root: Path, *, target_versions=None,
                            source_defaults=None, target_defaults=None):
        target_versions = target_versions or {}
        source_defaults = source_defaults or {}
        target_defaults = target_defaults or {}
        content_root = root / "Content"
        material_root = content_root / "WoT" / "Materials"
        material_root.mkdir(parents=True)
        expected_hashes = {}
        for source_name in {spec.source for spec in ewm.material_specs()}:
            data = f"approved-source:{source_name}".encode("ascii")
            (material_root / f"{source_name}.uasset").write_bytes(data)
            expected_hashes[source_name] = hashlib.sha256(data).hexdigest()

        def object_path(package):
            return f"{package}.{package.rsplit('/', 1)[-1]}"

        def material(package, texture_default, version=None):
            texture = mock.Mock()
            texture.get_path_name.return_value = object_path(texture_default)
            expression = mock.Mock()
            expression.get_class.return_value.get_name.return_value = (
                "MaterialExpressionTextureSampleParameter2D")
            expression.get_editor_property.side_effect = (
                lambda name: texture if name == "texture" else None)
            asset = mock.Mock()
            asset.get_path_name.return_value = object_path(package)
            asset.test_expressions = [expression]
            asset.test_version = version
            return asset

        assets = {}
        neutral = mock.Mock()
        neutral.get_path_name.return_value = object_path(ewm.NEUTRAL_TEXTURE)
        assets[ewm.NEUTRAL_TEXTURE] = neutral
        for spec in ewm.material_specs():
            source_path = f"{ewm.MATERIAL_ROOT}/{spec.source}"
            assets.setdefault(source_path, material(
                source_path,
                source_defaults.get(spec.source, ewm.NEUTRAL_TEXTURE)))
            target_path = f"{ewm.MATERIAL_ROOT}/{spec.name}"
            assets[target_path] = material(
                target_path,
                target_defaults.get(spec.name, ewm.NEUTRAL_TEXTURE),
                target_versions.get(spec.name, ewm.ENTRY_GRAPH_VERSION))

        unreal = mock.Mock()
        unreal.Paths.project_content_dir.return_value = str(content_root)
        unreal.EditorAssetLibrary.does_asset_exist.return_value = True
        unreal.EditorAssetLibrary.get_metadata_tag.side_effect = (
            lambda asset, _tag: asset.test_version)
        unreal.MaterialEditingLibrary.get_material_expressions.side_effect = (
            lambda asset: asset.test_expressions)
        unreal.load_asset.side_effect = lambda package: assets[package]
        return unreal, expected_hashes

    def test_pan_uses_scene_time(self):
        self.assertEqual(
            ewm.pan_uv(0.25, 0.5, 0.01, -0.02, 10.0),
            (0.35, 0.3))

    def test_exact_nine_entry_only_master_specs(self):
        self.assertEqual(
            tuple(spec.name for spec in ewm.material_specs()),
            (
                "M_WoT_EntryWorld",
                "M_WoT_EntryWorld_TwoSided",
                "M_WoT_EntryMasked",
                "M_WoT_EntryMasked_TwoSided",
                "M_WoT_EntryTranslucent",
                "M_WoT_EntryTranslucent_TwoSided",
                "M_WoT_EntryModulated",
                "M_WoT_EntryModulated_TwoSided",
                "M_WoT_EntryTranslucent_SceneBlend",
            ))
        self.assertEqual(len({spec.name for spec in ewm.material_specs()}), 9)
        self.assertEqual(
            tuple(spec.source for spec in ewm.material_specs()),
            (
                "M_WoT_World", "M_WoT_World",
                "M_WoT_WorldMasked", "M_WoT_WorldMasked",
                "M_WoT_WorldTranslucent", "M_WoT_WorldTranslucent",
                "M_WoT_WorldModulated", "M_WoT_WorldModulated",
                "M_WoT_WorldT_SceneBlend",
            ))
        self.assertEqual(
            tuple(spec.two_sided for spec in ewm.material_specs()),
            (False, True, False, True, False, True, False, True, True))

    def test_graph_contract_has_only_neutral_defaults_and_scene_time_pan(self):
        for spec in ewm.material_specs():
            graph = ewm.graph_contract(spec)
            with self.subTest(material=spec.name):
                self.assertEqual(graph["pan_time_parameter"], "WoTSceneTime")
                self.assertNotIn("Time", graph["expression_types"])
                self.assertEqual(graph["lightmap_multiplier"], 2.0)
                self.assertEqual(graph["dynamic_light_default"], 0.0)
                self.assertEqual(
                    graph["macro_offset_parameters"],
                    ("MacroOffsetU", "MacroOffsetV"))
                self.assertEqual(graph["lightmap_uv_channel"], 1)
                self.assertTrue(graph["unlit_uses_lightmap_uv1_fullbright"])
                self.assertEqual(
                    graph["source_graph_policy"],
                    "duplicate-and-targeted-rewire")
                self.assertEqual(
                    graph["allowed_graph_mutations"],
                    ("Time->WoTSceneTime", "remove-unreachable-Time-orphans",
                     "DynLightEnable=0", "two_sided"))
                self.assertEqual(
                    set(graph["texture_defaults"]), {ewm.NEUTRAL_TEXTURE})
                self.assertEqual(
                    set(graph["sampler_types"]), {"linear_color"})

    def test_blend_and_two_sided_contract_is_exact(self):
        expected = {
            "M_WoT_EntryWorld": ("opaque", False),
            "M_WoT_EntryWorld_TwoSided": ("opaque", True),
            "M_WoT_EntryMasked": ("masked", False),
            "M_WoT_EntryMasked_TwoSided": ("masked", True),
            "M_WoT_EntryTranslucent": ("alpha_composite", False),
            "M_WoT_EntryTranslucent_TwoSided": ("alpha_composite", True),
            "M_WoT_EntryModulated": ("modulate", False),
            "M_WoT_EntryModulated_TwoSided": ("modulate", True),
            "M_WoT_EntryTranslucent_SceneBlend": ("translucent", True),
        }
        self.assertEqual(
            {spec.name: (spec.blend, spec.two_sided)
             for spec in ewm.material_specs()}, expected)

    def test_output_root_and_default_references_fail_closed(self):
        self.assertEqual(
            ewm.validate_output_root("/Game/WoT/Materials"),
            "/Game/WoT/Materials")
        for invalid in (
                "/Game/WoT", "/Game/WoT/Materials/Entry", "Content/WoT",
                "/Game/WoT/Materials/../Retail"):
            with self.subTest(output_root=invalid):
                with self.assertRaisesRegex(ValueError, "output root"):
                    ewm.validate_output_root(invalid)
        with self.assertRaisesRegex(ValueError, "retail default"):
            ewm.validate_default_references(
                (ewm.NEUTRAL_TEXTURE, "/Game/WoT/Textures/Retail/Sky_440"))

    def test_metadata_version_makes_generation_idempotent(self):
        self.assertFalse(ewm.needs_rebuild(ewm.ENTRY_GRAPH_VERSION))
        self.assertTrue(ewm.needs_rebuild(None))
        self.assertTrue(ewm.needs_rebuild("older"))

    def test_generate_skips_current_graphs_and_rebuilds_stale_graphs(self):
        versions = {
            spec.name: (ewm.ENTRY_GRAPH_VERSION
                        if spec.name == "M_WoT_EntryWorld" else "older")
            for spec in ewm.material_specs()
        }
        with tempfile.TemporaryDirectory() as temporary:
            unreal, expected_hashes = self._generation_fixture(
                Path(temporary), target_versions=versions)
            with (mock.patch.dict(
                    ewm._SOURCE_MASTER_SHA256, expected_hashes, clear=True),
                    mock.patch.object(ewm, "_rebuild_material") as rebuild):
                receipt = ewm.generate(
                    argparse.Namespace(output_root="/Game/WoT/Materials"),
                    unreal)
        self.assertEqual(receipt["format"], ewm.RECEIPT_FORMAT)
        self.assertEqual(receipt["graph_version"], ewm.ENTRY_GRAPH_VERSION)
        self.assertEqual(len(receipt["materials"]), 9)
        self.assertEqual(receipt["materials"][0]["action"], "current")
        self.assertEqual(
            [row["action"] for row in receipt["materials"][1:]],
            ["rebuilt"] * 8)
        self.assertEqual(rebuild.call_count, 8)

    def test_generate_rejects_poisoned_source_default_before_mutation(self):
        poisoned = "/Game/WoT/Textures/Retail/Sky_440"
        with tempfile.TemporaryDirectory() as temporary:
            unreal, expected_hashes = self._generation_fixture(
                Path(temporary),
                source_defaults={"M_WoT_WorldMasked": poisoned})
            mutations = []
            with (mock.patch.dict(
                    ewm._SOURCE_MASTER_SHA256, expected_hashes, clear=True),
                    mock.patch.object(
                        ewm, "_rebuild_material",
                        side_effect=lambda *_args: mutations.append("rebuild"))):
                with self.assertRaisesRegex(ValueError, "retail default"):
                    ewm.generate(argparse.Namespace(
                        output_root="/Game/WoT/Materials"), unreal)
        self.assertEqual(mutations, [])

    def test_generate_rejects_poisoned_current_target_before_mutation(self):
        poisoned = "/Game/WoT/Textures/Retail/Sky_440"
        with tempfile.TemporaryDirectory() as temporary:
            unreal, expected_hashes = self._generation_fixture(
                Path(temporary),
                target_defaults={"M_WoT_EntryWorld": poisoned})
            mutations = []
            with (mock.patch.dict(
                    ewm._SOURCE_MASTER_SHA256, expected_hashes, clear=True),
                    mock.patch.object(
                        ewm, "_rebuild_material",
                        side_effect=lambda *_args: mutations.append("rebuild"))):
                with self.assertRaisesRegex(ValueError, "retail default"):
                    ewm.generate(argparse.Namespace(
                        output_root="/Game/WoT/Materials"), unreal)
        self.assertEqual(mutations, [])

    def test_generate_preflights_late_rows_before_rebuilding_earlier_targets(self):
        specs = ewm.material_specs()
        versions = {spec.name: "older" for spec in specs}
        versions[specs[-1].name] = ewm.ENTRY_GRAPH_VERSION
        poisoned = "/Game/WoT/Textures/Retail/Sky_440"
        with tempfile.TemporaryDirectory() as temporary:
            unreal, expected_hashes = self._generation_fixture(
                Path(temporary), target_versions=versions,
                target_defaults={specs[-1].name: poisoned})
            mutations = []
            with (mock.patch.dict(
                    ewm._SOURCE_MASTER_SHA256, expected_hashes, clear=True),
                    mock.patch.object(
                        ewm, "_rebuild_material",
                        side_effect=lambda *_args: mutations.append("rebuild"))):
                with self.assertRaisesRegex(ValueError, "retail default"):
                    ewm.generate(argparse.Namespace(
                        output_root="/Game/WoT/Materials"), unreal)
        self.assertEqual(mutations, [])

    def test_generate_rejects_changed_source_hash_before_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            unreal, expected_hashes = self._generation_fixture(root)
            changed = root / "Content" / "WoT" / "Materials" / (
                "M_WoT_WorldMasked.uasset")
            changed.write_bytes(b"poisoned-source-bytes")
            mutations = []
            with (mock.patch.dict(
                    ewm._SOURCE_MASTER_SHA256, expected_hashes, clear=True),
                    mock.patch.object(
                        ewm, "_rebuild_material",
                        side_effect=lambda *_args: mutations.append("rebuild"))):
                with self.assertRaisesRegex(RuntimeError, "source_sha256"):
                    ewm.generate(argparse.Namespace(
                        output_root="/Game/WoT/Materials"), unreal)
        self.assertEqual(mutations, [])

    @staticmethod
    def _graph(edges, outputs):
        """A fake material graph: edges maps a node name to its connected
        input node names; outputs maps an output property name to a node."""
        nodes = {}

        def node(name, cls="MaterialExpressionMultiply", parameter=""):
            expression = mock.Mock(name=name)
            expression.get_path_name.return_value = f"M:{name}"
            expression.get_class.return_value.get_name.return_value = cls
            expression.get_editor_property.side_effect = (
                lambda key: parameter if key == "parameter_name" else None)
            nodes[name] = expression
            return expression

        for name, (cls, parameter) in edges["_classes"].items():
            node(name, cls, parameter)
        unreal = mock.Mock()
        unreal.MaterialProperty = mock.Mock()
        for prop in ewm._OUTPUT_PROPERTY_NAMES:
            setattr(unreal.MaterialProperty, prop, prop)
        mel = unreal.MaterialEditingLibrary
        mel.get_material_property_input_node.side_effect = (
            lambda _material, prop: nodes.get(outputs.get(prop)))
        mel.get_material_expression_input_names.side_effect = (
            lambda expression: [f"In{index}" for index, _ in enumerate(
                edges.get(expression.get_path_name()[2:], ()))])
        mel.get_inputs_for_material_expression.side_effect = (
            lambda _material, expression: [nodes[name] for name in edges.get(
                expression.get_path_name()[2:], ())])
        return unreal, nodes

    def test_live_graph_ignores_orphan_duplicate_parameters(self):
        # M_WoT_WorldT_SceneBlend's committed bytes: a live Tex chain plus an
        # unreachable orphan Tex from an older bulk rebuild.
        sampler = "MaterialExpressionTextureSampleParameter2D"
        edges = {
            "_classes": {
                "OrphanTex": (sampler, "Tex"), "OrphanMul": ("MaterialExpressionMultiply", ""),
                "Tex": (sampler, "Tex"), "Lightmap": (sampler, "Lightmap"),
                "Uv": ("MaterialExpressionAdd", ""), "Out": ("MaterialExpressionAdd", ""),
                "Mul": ("MaterialExpressionMultiply", ""),
            },
            "OrphanMul": ("OrphanTex",), "Tex": ("Uv",), "Mul": ("Tex", "Lightmap"),
            "Out": ("Mul",),
        }
        unreal, nodes = self._graph(edges, {"MP_EMISSIVE_COLOR": "Out",
                                            "MP_OPACITY": "Mul"})
        live = ewm._live_expressions(unreal, object())
        self.assertEqual({node.get_path_name() for node in live},
                         {"M:Out", "M:Mul", "M:Tex", "M:Lightmap", "M:Uv"})
        self.assertIs(ewm._find_parameter(live, sampler, "Tex", "M"), nodes["Tex"])
        all_nodes = list(nodes.values())
        with self.assertRaisesRegex(RuntimeError, "expected one .* Tex, found 2"):
            ewm._find_parameter(all_nodes, sampler, "Tex", "M")
        # Two LIVE Tex samplers remain an error: the contract is one sampler.
        edges["Out"] = ("Mul", "OrphanMul")
        unreal, _nodes = self._graph(edges, {"MP_EMISSIVE_COLOR": "Out"})
        with self.assertRaisesRegex(RuntimeError, "found 2"):
            ewm._find_parameter(ewm._live_expressions(unreal, object()),
                                sampler, "Tex", "M")

    def test_editor_validation_pins_source_hashes_and_actual_graph_assertions(self):
        self.assertEqual(
            ewm.source_master_sha256s(),
            {
                "M_WoT_World":
					"c065f242f77fc79c5dcb79361f9e6cb54bb24d50e0e7b36ab248451a506294a7",
                "M_WoT_WorldMasked":
					"dc818bb3d00f7b14d7af8ebfedb49c8eee50268222d71ef1de8a249d447f8757",
                "M_WoT_WorldTranslucent":
                    "9520fa9116e4286de539b88779476a1f65af6bee9c7cb05e3c033e31416efc2f",
                "M_WoT_WorldModulated":
                    "da734a325a0c7a925ecf4eb5c818a0ded41417e1ab8cdf35811831862686f508",
                "M_WoT_WorldT_SceneBlend":
                    "00241fdd485b5f146037ca8eebcb1b26ea2ae5d0bac9937dc8c8730d6147cd1b",
            })
        self.assertEqual(
            set(ewm.required_editor_assertions()),
            {
                "source_sha256", "metadata", "blend_source_parity",
                "shading_source_parity", "two_sided", "pan_uv_connections",
                "no_global_time", "neutral_texture_defaults",
                "linear_samplers", "lightmap_uv1_times_two",
                "dynamic_light_default_zero", "expression_inventory_parity",
                "material_outputs", "shader_compile",
            })


if __name__ == "__main__":
    unittest.main()
