"""Pure-Python pins for the light-wave material and spawn contracts."""

import ast
import sys
from pathlib import Path
from types import SimpleNamespace
import unittest


HERE = Path(__file__).resolve().parent
CONVERT = HERE.parent / "convert"
for directory in (HERE, CONVERT):
    if str(directory) not in sys.path:
        sys.path.insert(0, str(directory))

from light_animation import (  # noqa: E402
    MAX_RESPONSE_TERMS,
    MPC_SCALAR_DEFAULTS,
    WAVEFORM_SHAPES,
    light_wave_multiplier,
    light_wave_driver_groups,
    material_term_bindings,
)
from light_wave_material import (  # noqa: E402
    ANIMATED_MASTER_NAMES,
    EFFECT_SALT_PARAMETERS,
    RESPONSE_COEFFICIENT_HLSL,
    RESPONSE_TEXTURE_PARAMETERS,
    USE_RESPONSE_PARAMETERS,
    WAVE_INDEX_PARAMETERS,
    WORLD_RESPONSE_GRAPH_TAG,
    WORLD_RESPONSE_GRAPH_VERSION,
    bind_response_parameters,
)
from mesh_master_bootstrap import (  # noqa: E402
    LIGHT_GRAPH_VERSION,
    _ML_HLSL,
)


def _term(kind, slot, **fields):
    return dict(kind=kind, light_names=[f"Light{slot}"], light_count=1,
                response_png=f"Test_lmatlas_response_{slot}.png", **fields)


def _manifest():
    keys = [
        ("LT_Pulse", 99, 102),
        ("LT_Pulse", 134, 0),
        ("LT_SubtlePulse", 83, 0),
        ("LT_SubtlePulse", 134, 0),
    ]
    groups = [dict(key=key, mean=0.6, amplitude=0.39,
                   light_names=[f"Light{index}"], light_count=1)
              for index, key in enumerate(keys)]
    terms = [_term("waveform", index, group=key)
             for index, key in enumerate(keys)]
    terms.append(_term(
        "effect", 4, effect="LE_WateryShimmer",
        effect_mean=0.8, effect_amplitude=0.2))
    terms.append(_term(
        "intersection", 5, group=keys[1], effect="LE_WateryShimmer",
        effect_mean=0.8, effect_amplitude=0.2))
    return dict(groups=groups, terms=terms)


class LightWaveMaterialContractTests(unittest.TestCase):
    def test_wave4_mpc_contract_is_exactly_six_frozen_default_scalars(self):
        self.assertEqual(MPC_SCALAR_DEFAULTS, (
            ("Wave0", 0.5), ("Wave1", 0.5),
            ("Wave2", 0.5), ("Wave3", 0.5),
            ("NoiseFrame", 0.0), ("Freeze", 1.0),
        ))
        self.assertIn("if (Freeze >= 0.5)", RESPONSE_COEFFICIENT_HLSL)
        self.assertIn("return float3(0.0, 0.0, 0.0);",
                      RESPONSE_COEFFICIENT_HLSL)
        self.assertIn("int2 coords[4]", RESPONSE_COEFFICIENT_HLSL)
        self.assertIn("ResponseTexture.Load(int3(texel, 0)).rgb",
                      RESPONSE_COEFFICIENT_HLSL)
        self.assertIn("filtered += weights[i] * n * response",
                      RESPONSE_COEFFICIENT_HLSL)
        self.assertIn("& 0x00ffffffu", RESPONSE_COEFFICIENT_HLSL)
        self.assertIn("float(h >> 8)", RESPONSE_COEFFICIENT_HLSL)
        for name in ("Wave0", "Wave1", "Wave2", "Wave3"):
            self.assertIn(f"(2.0 * {name} - 1.0)",
                          RESPONSE_COEFFICIENT_HLSL)

    def test_six_manifest_terms_map_to_wave_effect_and_mixed_selectors(self):
        bindings = material_term_bindings(_manifest())
        self.assertEqual(len(bindings), MAX_RESPONSE_TERMS)
        self.assertEqual(
            [(row["wave_index"], row["effect_salt"]) for row in bindings],
            [(0, 0), (1, 0), (2, 0), (3, 0), (-1, 3), (1, 3)])

    def test_manifest_drives_wave_rows_and_keeps_shimmer_only_actor(self):
        self.assertEqual(light_wave_driver_groups(_manifest()), [
            (2, 99, 102), (2, 134, 0), (7, 83, 0), (7, 134, 0),
        ])

        shimmer_only = _manifest()
        shimmer_only["groups"] = []
        shimmer_only["terms"] = [shimmer_only["terms"][4]]
        self.assertEqual(light_wave_driver_groups(shimmer_only), [])

        static = dict(groups=[], terms=[])
        self.assertIsNone(light_wave_driver_groups(static))

    def test_wave5_per_light_ratio_is_exactly_one_at_freeze(self):
        means = (0.60, 0.60, 0.90, 0.90)
        amplitudes = (0.39, 0.39, 0.09, 0.09)
        frozen = (0.5, 0.5, 0.5, 0.5)
        self.assertEqual(WAVEFORM_SHAPES, {
            "LT_Pulse": (0.60, 0.39),
            "LT_Blink": (0.50, 0.50),
            "LT_SubtlePulse": (0.90, 0.09),
        })
        self.assertEqual(light_wave_multiplier(
            frozen, -1, means, amplitudes), 1.0)
        for group in range(4):
            self.assertEqual(light_wave_multiplier(
                frozen, group, means, amplitudes), 1.0)
        self.assertAlmostEqual(light_wave_multiplier(
            (0.0, 0.5, 0.5, 0.5), 0, means, amplitudes), 0.21 / 0.60)
        self.assertAlmostEqual(light_wave_multiplier(
            (1.0, 0.5, 0.5, 0.5), 0, means, amplitudes), 0.99 / 0.60)

    def test_wave5_mesh_and_mover_graphs_use_static_selectors_and_one_mpc(self):
        self.assertEqual(
            LIGHT_GRAPH_VERSION, "wave-groups-dynamic-g45-v2")
        for index in range(6):
            self.assertIn(f"G{index}", _ML_HLSL)
        for name in ("Wave0", "Wave1", "Wave2", "Wave3",
                     "MLWaveMeans", "MLWaveAmplitudes"):
            self.assertIn(name, _ML_HLSL)
        self.assertIn("float ratio = 1.0;", _ML_HLSL)
        self.assertIn("2.0 * waves[group] - 1.0", _ML_HLSL)
        for index in range(8):
            self.assertIn(f"DP{index}", _ML_HLSL)
            self.assertIn(f"DC{index}", _ML_HLSL)
        self.assertIn("DynEnable", _ML_HLSL)

        mover_source = (HERE / "import_map.py").read_text(encoding="utf-8")
        self.assertIn("_ML_MOVER_LIGHTS = 20", mover_source)
        self.assertIn('f"MLWaveGroup{light_index}"', mover_source)
        self.assertIn("float wg[10]", mover_source)
        self.assertIn("2.0 * waves[group] - 1.0", mover_source)

        runtime_source = (HERE.parents[1] / "Source/WoT/WoTMeshLight.cpp").read_text(
            encoding="utf-8")
        self.assertIn('TEXT("MLWaveGroup%d")', runtime_source)
        self.assertIn("MoverSurface->LightWaveGroup", runtime_source)
        self.assertIn("LightWaveGroup.IsValidIndex", runtime_source)
        self.assertIn('TEXT("MLWaveMeans")', runtime_source)
        self.assertIn('TEXT("MLWaveAmplitudes")', runtime_source)

    def test_only_the_seven_animated_masters_are_rebuild_targets(self):
        self.assertEqual(ANIMATED_MASTER_NAMES, (
            "M_WoT_WorldWave",
            "M_WoT_WorldWaveMasked",
            "M_WoT_WorldWaveTranslucent",
            "M_WoT_WorldWaveModulated",
            "M_WoT_WorldWaveTranslucentFlip",
            "M_WoT_WorldWaveModulatedFlip",
            "M_WoT_WorldWaveT_SceneBlend",
        ))
        self.assertEqual(WORLD_RESPONSE_GRAPH_TAG,
                         "WoT.WorldWave.ResponseGraph")
        self.assertEqual(WORLD_RESPONSE_GRAPH_VERSION, "point-shimmer-v3")

    def test_import_preflight_and_legacy_shared_asset_order_are_pinned(self):
        source = (HERE / "import_map.py").read_text(encoding="utf-8")
        main = source[source.index("def main():"):]
        self.assertLess(main.index("prevalidate_lightmap_atlases()"),
                        main.index("textures = import_textures(needed)"))
        self.assertLess(main.index("prevalidate_lightmap_atlases()"),
                        main.index("masters = make_master()"))
        self.assertIn('legacy_manifest = "animation" not in atlas_document',
                      main)
        self.assertIn("load_legacy_actor_masters()", main)
        self.assertIn("load_legacy_world_masters()", main)
        self.assertLess(main.index("load_legacy_world_masters()"),
                        main.index("textures = import_textures(needed)"))
        self.assertIn("import_legacy_wave_atlases(legacy_files)", main)
        self.assertNotIn("delete_asset(master_path)", source)
        self.assertIn("_make_animated_scene_blend_master()", source)
        self.assertIn("WORLD_RESPONSE_GRAPH_TAG", source)

        runtime = (HERE.parents[1] / "Source/WoT/WoTLightWave.cpp").read_text(
            encoding="utf-8")
        self.assertIn("if (Groups.IsEmpty())", runtime)
        self.assertIn('LegacyWaveParameterName(TEXT("LightWave"))', runtime)

    def test_manifest_rejects_more_than_six_terms_and_unsafe_pngs(self):
        manifest = _manifest()
        manifest["terms"].append(_term(
            "effect", 6, effect="LE_FireWaver",
            effect_mean=0.9, effect_amplitude=0.1))
        with self.assertRaisesRegex(ValueError, "maximum is 6"):
            material_term_bindings(manifest)
        for unsafe in ("../escaped.png", r"..\escaped.png"):
            manifest = _manifest()
            manifest["terms"][0]["response_png"] = unsafe
            with self.assertRaisesRegex(ValueError, "unsafe response PNG"):
                material_term_bindings(manifest)

    def test_mic_binding_enables_used_slots_and_clears_every_unused_selector(self):
        manifest = _manifest()
        manifest["groups"] = manifest["groups"][:1]
        manifest["terms"] = manifest["terms"][:1]
        texture = object()
        textures = {manifest["terms"][0]["response_png"]: texture}
        scalars = {}
        switches = {}
        bound_textures = {}

        class _Editing:
            @staticmethod
            def set_material_instance_scalar_parameter_value(_mi, name, value):
                scalars[name] = value

            @staticmethod
            def set_material_instance_static_switch_parameter_value(_mi, name, value):
                switches[name] = value

            @staticmethod
            def set_material_instance_texture_parameter_value(_mi, name, value):
                bound_textures[name] = value

        fake_unreal = SimpleNamespace(MaterialEditingLibrary=_Editing)
        count = bind_response_parameters(
            fake_unreal, object(),
            {"size": [1024, 1024], "animation": manifest}, textures)
        self.assertEqual(count, 1)
        self.assertEqual(
            [switches[name] for name in USE_RESPONSE_PARAMETERS],
            [True, False, False, False, False, False])
        self.assertEqual(bound_textures, {RESPONSE_TEXTURE_PARAMETERS[0]: texture})
        self.assertEqual(
            [scalars[name] for name in WAVE_INDEX_PARAMETERS],
            [0.0, -1.0, -1.0, -1.0, -1.0, -1.0])
        self.assertEqual(
            [scalars[name] for name in EFFECT_SALT_PARAMETERS],
            [0.0, 0.0, 0.0, 0.0, 0.0, 0.0])

    def test_blink_actor_generation_sets_the_native_wire_type(self):
        """Exercise import_map.place_lightwave without importing Unreal."""
        source_path = HERE / "import_map.py"
        source = source_path.read_text(encoding="utf-8")
        tree = ast.parse(source, filename=str(source_path))
        function = next(node for node in tree.body
                        if isinstance(node, ast.FunctionDef)
                        and node.name == "place_lightwave")

        class FakeGroup:
            def __init__(self):
                self.properties = {}

            def set_editor_property(self, name, value):
                self.properties[name] = value

        class FakeActor(FakeGroup):
            def set_actor_label(self, label):
                self.label = label

        class FakeActors:
            def __init__(self):
                self.actor = None

            def spawn_actor_from_class(self, _actor_class, _location):
                self.actor = FakeActor()
                return self.actor

        fake_unreal = SimpleNamespace(
            WoTLightWave=object(),
            WoTLightWaveGroup=FakeGroup,
            Vector=lambda *xyz: xyz,
        )
        namespace = {
            "unreal": fake_unreal,
            "MAP": "BlinkFixture",
            "light_wave_driver_groups": light_wave_driver_groups,
        }
        exec(compile(ast.Module(body=[function], type_ignores=[]),
                     str(source_path), "exec"), namespace)
        key = ("LT_Blink", 7, 3)
        animation = {
            "groups": [dict(key=key, mean=0.5, amplitude=0.5,
                            light_names=["Blink"], light_count=1)],
            "terms": [_term("waveform", 0, group=key)],
        }
        actors = FakeActors()
        self.assertEqual(namespace["place_lightwave"](
            actors, {"animation": animation}), 1)
        self.assertEqual(actors.actor.label, "LightWave_BlinkFixture")
        groups = actors.actor.properties["groups"]
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0].properties, {
            "type": 3,
            "period": 7,
            "phase": 3,
        })


if __name__ == "__main__":
    unittest.main()
