import tempfile
import unittest
from pathlib import Path

import import_decoration_skins as decoration_skins


class DecorationSkinPlanTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        script = root / "script"
        (script / "WOT").mkdir(parents=True)
        (script / "WoTDecorations").mkdir(parents=True)
        (script / "WOT" / "WOTDecoration.uc").write_text(
            "class WOTDecoration expands Decoration;\n"
            "defaultproperties\n{\n     bCollideActors=True\n"
            "     bBlockActors=True\n     bBlockPlayers=True\n}\n")
        (script / "WoTDecorations" / "twotriangles.uc").write_text(
            "class TwoTriangles expands WOTDecoration;\n"
            "defaultproperties\n{\n     Mesh=Mesh'WoTDecorations.twotriangles'\n}\n")
        (script / "WoTDecorations" / "Grass.uc").write_text(
            "class Grass expands WOTDecoration;\n"
            "defaultproperties\n{\n     Style=STY_Masked\n     bCollideActors=False\n}\n")
        (root / "SpecMap.t3d").write_text("""Begin Map
Begin Actor Class=twotriangles Name=twotriangles0
    Style=STY_Translucent
    Skin=WetTexture'ScottT.S_Tele_effect'
    bCollideActors=False
    bBlockActors=False
    bBlockPlayers=False
    Name=twotriangles0
End Actor
Begin Actor Class=twotriangles Name=TwoTriangles13
    MultiSkins(0)=WaveTexture'JHPFireEng.RiverWave01'
    Name=TwoTriangles13
End Actor
Begin Actor Class=Grass Name=Grass0
    Name=Grass0
End Actor
Begin Actor Class=twotriangles Name=twotriangles5
    Skin=FireTexture'Pkg.Group.Flame-01'
    Name=twotriangles5
End Actor
End Map
""")
        self._saved = (decoration_skins.SCRIPT, decoration_skins.T3D_DIR,
                       decoration_skins._uc_files, dict(decoration_skins._uc_cache))
        decoration_skins.SCRIPT = script
        decoration_skins.T3D_DIR = root
        decoration_skins._uc_files = None
        decoration_skins._uc_cache.clear()

    def tearDown(self):
        (decoration_skins.SCRIPT, decoration_skins.T3D_DIR,
         decoration_skins._uc_files, cache) = self._saved
        decoration_skins._uc_cache.clear()
        decoration_skins._uc_cache.update(cache)
        self._tmp.cleanup()

    def test_plan_covers_procedural_skins_and_non_colliding_decorations(self):
        plan = decoration_skins.plan_map("SpecMap")
        # Arena_06's teleporter shimmer quad: WetTexture skin, collision off
        # on the instance.
        self.assertEqual(plan["twotriangles0"], {
            "skin": ("ScottT", "S_Tele_effect"),
            "style": "STY_Translucent",
            "collision_off": True,
        })
        # A WaveTexture skin keeps its imported frame and the class collides.
        self.assertNotIn("TwoTriangles13", plan)
        # Class-default collision off reaches through the shipped .uc chain.
        self.assertEqual(plan["Grass0"], {
            "skin": None, "style": "STY_Masked", "collision_off": True})
        self.assertEqual(plan["twotriangles5"], {
            "skin": ("Pkg", "Flame-01"), "style": "", "collision_off": False})

    def test_skin_instances_carry_the_fractal_identity(self):
        self.assertEqual(
            decoration_skins.skin_instance("STY_Translucent", "_2S",
                                           "ScottT", "S_Tele_effect"),
            ("MI_MeshT_ScottT__S_Tele_effect", "M_WoT_MeshLitT"))
        self.assertEqual(
            decoration_skins.skin_instance("", "_2S", "Pkg", "Flame-01"),
            ("MI_Skin_Pkg__Flame_01_2S", "M_WoT_MeshLit2S"))
        self.assertEqual(
            decoration_skins.skin_instance("STY_Modulated", "", "Pkg", "Tex"),
            ("MI_Skin_Pkg__Tex_Mod", "M_WoT_MeshModulate"))


if __name__ == "__main__":
    unittest.main()
