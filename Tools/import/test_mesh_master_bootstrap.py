"""Pure-Python safety pins for the direct mesh-import master preflight."""

import unittest
from types import SimpleNamespace

import mesh_master_bootstrap
from dynamic_light_material import dynamic_vector_names


class _MissingEditorAssetLibrary:
    @staticmethod
    def does_asset_exist(_path):
        return False


class _FakeUnreal:
    EditorAssetLibrary = _MissingEditorAssetLibrary


class _GraphNode:
    def __init__(self, kind, **props):
        self.kind = kind
        self.props = props
        self.nodes = []
        self.tags = {}

    def set_editor_property(self, name, value):
        self.props[name] = value

    def get_editor_property(self, name):
        return self.props.get(name)


class _GraphUnreal:
    """Record the material graph emitted at the editor API boundary."""

    def __init__(self):
        self.assets = {}
        self.edges = []
        self.collection = _GraphNode(
            "collection", scalar_parameters=[], vector_parameters=[
                _GraphNode("parameter", parameter_name=name)
                for name in dynamic_vector_names()])
        self.EditorAssetLibrary = SimpleNamespace(
            does_asset_exist=lambda path: path in self.assets or "/MPC_" in path,
            get_metadata_tag=lambda asset, tag: asset.tags.get(tag),
            set_metadata_tag=lambda asset, tag, value: asset.tags.update({tag: value}),
            save_asset=lambda path: True)
        self.AssetToolsHelpers = SimpleNamespace(get_asset_tools=lambda: self)
        self.MaterialEditingLibrary = SimpleNamespace(
            create_material_expression=self.create_material_expression,
            connect_material_expressions=lambda *edge: self.edges.append(edge),
            connect_material_property=lambda *args: None,
            recompile_material=lambda material: None,
            get_material_expressions=lambda material: material.nodes,
            delete_material_expression=lambda material, node: material.nodes.remove(node))
        self.WorldPositionIncludedOffsets = SimpleNamespace(
            WPT_EXCLUDE_ALL_SHADER_OFFSETS="WPT_EXCLUDE_ALL_SHADER_OFFSETS")
        self.CustomMaterialOutputType = SimpleNamespace(
            CMOT_FLOAT2="float2", CMOT_FLOAT3="float3")
        self.MaterialShadingModel = SimpleNamespace(MSM_UNLIT="unlit")
        self.BlendMode = SimpleNamespace(BLEND_MASKED="masked", BLEND_ALPHA_COMPOSITE="alpha")
        self.MaterialProperty = SimpleNamespace(
            MP_EMISSIVE_COLOR="emissive", MP_OPACITY_MASK="mask", MP_OPACITY="opacity")
        self.CustomInput = lambda: _GraphNode("input")
        self.LinearColor = lambda *rgba: rgba
        self.MaterialFactoryNew = lambda: None

    def __getattr__(self, name):
        if name == "Material" or name.startswith("MaterialExpression"):
            return name
        raise AttributeError(name)

    def load_asset(self, path):
        return self.collection if "/MPC_" in path else self.assets.get(path)

    def create_asset(self, name, root, *_args):
        material = _GraphNode("material")
        self.assets[root + "/" + name] = material
        return material

    def create_material_expression(self, material, kind, _x, _y):
        node = _GraphNode(kind)
        material.nodes.append(node)
        return node

    def build_lit_masters(self):
        opaque = mesh_master_bootstrap._build_opaque_family(self, gamma_space=False)
        translucent = mesh_master_bootstrap._build_translucent(self, gamma_space=False)
        return list(opaque.values()) + [translucent]


class MeshMasterBootstrapTests(unittest.TestCase):
    def test_module_import_does_not_require_unreal_or_numpy(self):
        self.assertNotIn("unreal", mesh_master_bootstrap.__dict__)
        self.assertNotIn("numpy", mesh_master_bootstrap.__dict__)

    def test_missing_prerequisites_fail_before_any_builder_runs(self):
        with self.assertRaisesRegex(
                RuntimeError, "mesh-master bootstrap prerequisites are missing"):
            mesh_master_bootstrap.ensure_mesh_masters(_FakeUnreal(), gamma_space=True)

    def test_all_lit_masters_sample_position_before_shader_offsets(self):
        unreal = _GraphUnreal()
        materials = unreal.build_lit_masters()
        self.assertEqual(len(materials), 4)
        for index, material in enumerate(materials):
            with self.subTest(master=index):
                custom = next(node for node in material.nodes
                              if node.props.get("code") == mesh_master_bootstrap._ML_HLSL)
                position, = [src for src, _pin, dst, name in unreal.edges
                             if dst is custom and name == "WP"]
                self.assertEqual(position.kind, "MaterialExpressionWorldPosition")
                self.assertEqual(position.props.get("world_position_shader_offset"),
                                 "WPT_EXCLUDE_ALL_SHADER_OFFSETS")

    def test_translucent_environment_position_keeps_shader_offsets(self):
        unreal = _GraphUnreal()
        material = mesh_master_bootstrap._build_translucent(unreal, gamma_space=False)
        environment = next(node for node in material.nodes
                           if node.props.get("description") == "UE1EnviroMap")
        position, = [src for src, _pin, dst, name in unreal.edges
                     if dst is environment and name == "WP"]
        self.assertNotIn("world_position_shader_offset", position.props)

    def test_previous_light_graph_tags_regenerate_all_lit_masters(self):
        unreal = _GraphUnreal()
        materials = unreal.build_lit_masters()
        old_nodes = []
        for material in materials:
            material.tags[mesh_master_bootstrap.LIGHT_GRAPH_TAG] = "wave-groups-dynamic-g45-v2"
            old_nodes.append(material.nodes[0])
        self.assertEqual(unreal.build_lit_masters(), materials)
        for material, old_node in zip(materials, old_nodes):
            self.assertNotIn(old_node, material.nodes)


if __name__ == "__main__":
    unittest.main()
