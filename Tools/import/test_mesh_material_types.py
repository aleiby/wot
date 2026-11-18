"""Pure-Python pins for James mesh material-slot routing."""
import unittest

from mesh_material_types import (
    GAMMA_SLOT_MASTERS,
    IMPORT_SLOT_MASTERS,
    actor_style_suffix,
    effective_material_type_suffix,
    material_type_suffix,
    slot_zero_skin_reference,
    split_slot_name,
)


class MeshMaterialTypeTests(unittest.TestCase):
    def test_suffix_binding_tables_cover_every_rendered_base_type(self):
        self.assertEqual(IMPORT_SLOT_MASTERS, {
            "": "M_WoT_Master",
            "_2S": "M_WoT_MeshLit2S",
            "_M": "M_WoT_MeshLitMasked",
            "_T": "M_WoT_MeshLitT",
            "_Mod": "M_WoT_MeshModulate",
        })
        self.assertEqual(GAMMA_SLOT_MASTERS, {
            "": "M_WoT_MeshLit",
            "_2S": "M_WoT_MeshLit2S",
            "_M": "M_WoT_MeshLitMasked",
            "_T": "M_WoT_MeshLitT",
            "_Mod": "M_WoT_MeshModulate",
        })

    def test_slot_suffix_parser_preserves_texture_names_with_underscores(self):
        self.assertEqual(split_slot_name("Aes_Base_2S"), ("Aes_Base", "_2S"))
        self.assertEqual(split_slot_name("Aes_Base"), ("Aes_Base", ""))
        self.assertEqual(split_slot_name("Some_Mod_Name_M"),
                         ("Some_Mod_Name", "_M"))

    def test_material_roots_recover_slot_type_for_skin_overrides(self):
        expected = {
            "M_WoT_Master": "",
            "M_WoT_MeshLit": "",
            "M_WoT_Masked": "_M",
            "M_WoT_MeshLitMasked": "_M",
            "M_WoT_MeshLit2S": "_2S",
            "M_WoT_MeshLitT": "_T",
            "M_WoT_MeshTranslucent": "_T",
            "M_WoT_MeshModulate": "_Mod",
        }
        self.assertEqual({root: material_type_suffix(root) for root in expected},
                         expected)
        self.assertEqual(material_type_suffix("M_Default"), "")

    def test_actor_style_suffix_accepts_retail_enum_names_and_bytes(self):
        self.assertEqual(actor_style_suffix("STY_Masked"), "_M")
        self.assertEqual(actor_style_suffix("STY_Translucent"), "_T")
        self.assertEqual(actor_style_suffix("STY_Modulated"), "_Mod")
        self.assertEqual(actor_style_suffix(2), "_M")
        self.assertEqual(actor_style_suffix("4"), "_Mod")
        self.assertEqual(actor_style_suffix("STY_Normal"), "")

    def test_actor_and_slot_blends_use_retail_precedence(self):
        cases = (
            ("_M", "STY_Modulated", "_Mod"),
            ("_Mod", "STY_Masked", "_Mod"),
            ("_M", "STY_Translucent", "_T"),
            ("_T", "STY_Masked", "_T"),
            ("_Mod", "STY_Translucent", "_T"),
            ("_T", "STY_Modulated", "_T"),
            ("_2S", "STY_Masked", "_M"),
            ("_2S", "STY_Normal", "_2S"),
        )
        for slot_suffix, style, expected in cases:
            with self.subTest(slot_suffix=slot_suffix, style=style):
                self.assertEqual(
                    effective_material_type_suffix(slot_suffix, style),
                    expected)

    def test_slot_zero_override_prefers_skin_then_multiskins(self):
        self.assertEqual(slot_zero_skin_reference({
            "Skin": "Texture'Pkg.Skin'",
            "MultiSkins(0)": "Texture'Pkg.Multi'",
        }), "Texture'Pkg.Skin'")
        self.assertEqual(slot_zero_skin_reference({
            "MultiSkins(0)": "WaveTexture'JHPFireEng.RiverWave01'",
        }), "WaveTexture'JHPFireEng.RiverWave01'")
        self.assertIsNone(slot_zero_skin_reference({"Style": "STY_Masked"}))


if __name__ == "__main__":
    unittest.main()
