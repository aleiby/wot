import tempfile
import unittest
from pathlib import Path

import import_angreal_guide

# Synthetic fixtures: invented class names and invented text only. The real
# export is exercised solely by the reference-gated pins below, which assert
# identifiers (class and property names), never retail prose.

FIXTURE_GADGET_BASE = """\
//------------------------------------------------------------------------------
// GadgetBase.uc - synthetic fixture.
//------------------------------------------------------------------------------
class GadgetBase expands RelicRoot
\tabstract;

#exec TEXTURE IMPORT FILE=Fixtures\\G_Base.pcx GROUP=Icons MIPS=Off

// defaultproperties in a comment must not open the block.

function int Tally()
{
\tlocal int Total;
\tTotal = 3;
\treturn Total;
}

defaultproperties
{
     Durability=7
     Durability=12
     bWidgetFlag=True
     Label="plain label"
}
"""

FIXTURE_GLOW_GADGET = """\
class GlowGadget expands GadgetBase;

defaultproperties
{
     bShinyFlag=True
     Label="  padded label  "
     Notes=""
     Charms(0)=Class'FixtureItems.FirstCharm'
     Charms(2)=Class'FixtureItems.ThirdCharm'
     Offset=(X=1.500000,Y=2.000000)
     Badge=Texture'FixtureItems.Icons.I_Glow'
}
"""

FIXTURE_STURDY_GADGET = """\
class SturdyGadget expands GlowGadget;

defaultproperties
{
     Durability=99
}
"""

FIXTURE_HOLLOW_GADGET = """\
class HollowGadget expands MissingParent;
"""

FIXTURE_LOOP_A = """\
class LoopA extends LoopB;
"""

FIXTURE_LOOP_B = """\
class LoopB expands LoopA;
"""

# Built from parts so the trailing syntax space after the closing quote is
# explicit rather than invisible trailing whitespace in this file.
FIXTURE_TRIM_GADGET = "\n".join([
    "class TrimGadget expands GadgetBase;",
    "",
    "defaultproperties",
    "{",
    '     Motto="steady hands" ',
    "}",
    "",
])

FIXTURES = {
    ("FixtureCore", "GadgetBase"): FIXTURE_GADGET_BASE,
    ("FixtureItems", "GlowGadget"): FIXTURE_GLOW_GADGET,
    ("FixtureItems", "SturdyGadget"): FIXTURE_STURDY_GADGET,
    ("FixtureItems", "HollowGadget"): FIXTURE_HOLLOW_GADGET,
    ("FixtureItems", "TrimGadget"): FIXTURE_TRIM_GADGET,
    ("FixtureLoops", "LoopA"): FIXTURE_LOOP_A,
    ("FixtureLoops", "LoopB"): FIXTURE_LOOP_B,
}


class SyntheticParseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tree = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tree.cleanup)
        root = Path(tree.name)
        for (package, name), text in FIXTURES.items():
            path = root / package / ("%s.uc" % name)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="latin-1")
        cls.classes = import_angreal_guide.parse_script_classes(root)

    def test_headers_and_packages(self):
        self.assertEqual(sorted(self.classes),
                         ["gadgetbase", "glowgadget", "hollowgadget", "loopa",
                          "loopb", "sturdygadget", "trimgadget"])
        base = self.classes["gadgetbase"]
        self.assertEqual(base.name, "GadgetBase")
        self.assertEqual(base.package, "FixtureCore")
        self.assertEqual(base.parent, "RelicRoot")
        glow = self.classes["glowgadget"]
        self.assertEqual(glow.package, "FixtureItems")
        self.assertEqual(glow.parent, "GadgetBase")
        self.assertEqual(self.classes["loopa"].parent, "LoopB")

    def test_scalar_props_last_value_wins_and_code_is_ignored(self):
        base = self.classes["gadgetbase"]
        self.assertEqual(base.props["Durability"], "12")
        self.assertEqual(base.props["bWidgetFlag"], "True")
        self.assertNotIn("Total", base.props)
        self.assertNotIn("FILE", base.props)
        self.assertEqual(self.classes["hollowgadget"].props, {})

    def test_array_props_keep_index_order_with_gaps(self):
        glow = self.classes["glowgadget"]
        self.assertEqual(glow.array_props["Charms"],
                         ["Class'FixtureItems.FirstCharm'", "",
                          "Class'FixtureItems.ThirdCharm'"])
        self.assertNotIn("Charms", glow.props)

    def test_unquoting(self):
        base = self.classes["gadgetbase"]
        glow = self.classes["glowgadget"]
        self.assertEqual(base.props["Label"], "plain label")
        self.assertEqual(glow.props["Label"], "  padded label  ")
        self.assertEqual(glow.props["Notes"], "")
        self.assertEqual(glow.props["Offset"], "(X=1.500000,Y=2.000000)")
        self.assertEqual(glow.props["Badge"], "Texture'FixtureItems.Icons.I_Glow'")
        self.assertEqual(self.classes["trimgadget"].props["Motto"], "steady hands")

    def test_resolved_prop_walks_the_parent_chain(self):
        classes = self.classes
        self.assertEqual(
            import_angreal_guide.resolved_prop(classes, "GlowGadget", "Durability"),
            "12")
        self.assertEqual(
            import_angreal_guide.resolved_prop(classes, "SturdyGadget", "Durability"),
            "99")
        self.assertEqual(
            import_angreal_guide.resolved_prop(classes, "STURDYGADGET", "durability"),
            "99")
        self.assertIsNone(
            import_angreal_guide.resolved_prop(classes, "GadgetBase", "bShinyFlag"))
        self.assertIsNone(
            import_angreal_guide.resolved_prop(classes, "NoSuchGadget", "Durability"))

    def test_resolved_bool(self):
        classes = self.classes
        self.assertTrue(
            import_angreal_guide.resolved_bool(classes, "SturdyGadget", "bWidgetFlag"))
        self.assertTrue(
            import_angreal_guide.resolved_bool(classes, "GlowGadget", "bShinyFlag"))
        self.assertFalse(
            import_angreal_guide.resolved_bool(classes, "GadgetBase", "bShinyFlag"))
        self.assertFalse(
            import_angreal_guide.resolved_bool(classes, "SturdyGadget", "bAbsentFlag"))
        self.assertFalse(
            import_angreal_guide.resolved_bool(classes, "SturdyGadget", "Durability"))

    def test_ancestors(self):
        classes = self.classes
        self.assertEqual(
            import_angreal_guide.ancestors(classes, "SturdyGadget"),
            ["sturdygadget", "glowgadget", "gadgetbase"])
        self.assertEqual(
            import_angreal_guide.ancestors(classes, "HollowGadget"),
            ["hollowgadget"])
        self.assertEqual(import_angreal_guide.ancestors(classes, "NoSuchGadget"), [])
        self.assertEqual(
            import_angreal_guide.ancestors(classes, "LoopA"), ["loopa", "loopb"])


@unittest.skipUnless(import_angreal_guide.reference_root_available(),
                     "WOT_REFERENCE_ROOT with exports/script is not configured")
class ReferencePinTests(unittest.TestCase):
    """Pins against the real export: identifiers only, never retail prose."""

    classes = None

    @classmethod
    def setUpClass(cls):
        cls.classes = import_angreal_guide.parse_script_classes(
            import_angreal_guide.default_script_root())

    def test_fireball_element_rarity_role(self):
        classes = self.classes
        self.assertTrue(
            import_angreal_guide.resolved_bool(
                classes, "AngrealInvFireball", "bElementFire"))
        for other in ("bElementAir", "bElementEarth", "bElementWater",
                      "bElementSpirit"):
            self.assertFalse(
                import_angreal_guide.resolved_bool(
                    classes, "AngrealInvFireball", other), other)
        self.assertTrue(
            import_angreal_guide.resolved_bool(
                classes, "AngrealInvFireball", "bCommon"))
        for other in ("bUncommon", "bRare"):
            self.assertFalse(
                import_angreal_guide.resolved_bool(
                    classes, "AngrealInvFireball", other), other)
        self.assertTrue(
            import_angreal_guide.resolved_bool(
                classes, "AngrealInvFireball", "bOffensive"))
        self.assertFalse(
            import_angreal_guide.resolved_bool(
                classes, "AngrealInvFireball", "bDefensive"))

    def test_fireball_chain_crosses_packages_and_stops_at_stripped_engine(self):
        self.assertEqual(
            import_angreal_guide.ancestors(self.classes, "AngrealInvFireball"),
            ["angrealinvfireball", "projectilelauncher", "angrealinventory",
             "wotinventory"])

    def test_fire_shield_reflector_names_the_fire_ignore_reflector(self):
        fire_shield = self.classes["angrealinvfireshield"]
        self.assertEqual(fire_shield.array_props["ReflectorClasses"],
                         ["Class'Angreal.IgnoreFireElementReflector'"])


if __name__ == "__main__":
    unittest.main()
