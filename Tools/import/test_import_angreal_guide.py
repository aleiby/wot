import csv
import io
import json
import tempfile
import unittest
from pathlib import Path

import import_angreal_guide
from import_angreal_guide import GenerationError, Override, RosterEntry

# Synthetic fixtures: invented class names and invented text only. The real
# export is exercised solely by the reference-gated pins below, which assert
# identifiers (class and property names), never retail prose. Derivation
# fixtures reuse the structural anchor names the rules key on
# (ProjectileLauncher, ReturnToSenderReflector, IgnoreFireElementReflector);
# those are identifiers, and every item built around them is invented.

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


# Derivation fixtures: an invented roster around the structural anchors.
# CinderWard's reflector has no fixture file (the name-fallback path);
# BounceWard's reflector file exists (the self-inclusive first chain link);
# ForkWard's SplitBounce expands ReturnToSenderReflector (the ancestry path
# that carries the Fork semantics on real data).
DERIVE_FIXTURES = {
    "RelicBase": "class RelicBase expands Stockpile;\n",
    "ProjectileLauncher": "class ProjectileLauncher expands RelicBase;\n",
    "ReturnToSenderReflector":
        "class ReturnToSenderReflector expands WispBase;\n",
    "SplitBounce": "class SplitBounce expands ReturnToSenderReflector;\n",
    "EmberLob": """\
class EmberLob expands ProjectileLauncher;

defaultproperties
{
     bElementFire=True
     bOffensive=True
     bCommon=True
     MaxInitialCharges=4
     MaxCharges=9
}
""",
    "GustLob": """\
class GustLob expands ProjectileLauncher;

defaultproperties
{
     bElementAir=True
     bOffensive=True
     bUncommon=True
     MaxInitialCharges=2
     MaxCharges=6
}
""",
    "EmberPuff": """\
class EmberPuff expands RelicBase;

defaultproperties
{
     bElementFire=True
     bOffensive=True
     bRare=True
     MaxInitialCharges=1
     MaxCharges=2
}
""",
    "CinderWard": """\
class CinderWard expands RelicBase;

defaultproperties
{
     bDefensive=True
     bRare=True
     ReflectorClasses(0)=Class'FixtureDerive.IgnoreFireElementReflector'
     MaxInitialCharges=3
     MaxCharges=5
}
""",
    "BounceWard": """\
class BounceWard expands RelicBase;

defaultproperties
{
     bDefensive=True
     bUncommon=True
     ReflectorClasses(0)=Class'FixtureDerive.ReturnToSenderReflector'
     MaxInitialCharges=1
     MaxCharges=3
}
""",
    "ForkWard": """\
class ForkWard expands RelicBase;

defaultproperties
{
     bDefensive=True
     bCommon=True
     ReflectorClasses(0)=Class'FixtureDerive.SplitBounce'
     MaxInitialCharges=1
     MaxCharges=3
}
""",
    "MuddleRelic": """\
class MuddleRelic expands RelicBase;

defaultproperties
{
     bCommon=True
     bRare=True
}
""",
    "BlankRelic": "class BlankRelic expands RelicBase;\n",
    "SnareInventory": "class SnareInventory expands WOTInventory;\n",
}

BASE_ROSTER = (
    RosterEntry("EmberLob", "FxEmberLob"),
    RosterEntry("GustLob", "FxGustLob"),
    RosterEntry("EmberPuff", "FxEmberPuff"),
    RosterEntry("CinderWard", "FxCinderWard"),
    RosterEntry("BounceWard", "FxBounceWard"),
    RosterEntry("ForkWard", "FxForkWard"),
)

BASE_HINTS = {
    "EmberLob": "{title} - {counter} snuffs the embers.",
    "GustLob": "{title} - {counter} stills the gust.",
    "EmberPuff": "{title} - {counter} smothers the puff.",
}

CITED = "exports/script/FixtureDerive/Invented.uc:1 (synthetic citation)"


class DeriveFixtureMixin:
    """Parsed derivation fixtures plus a build helper, shared across cases."""

    classes = None

    @classmethod
    def setUpClass(cls):
        tree = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tree.cleanup)
        root = Path(tree.name)
        for name, text in DERIVE_FIXTURES.items():
            path = root / "FixtureDerive" / ("%s.uc" % name)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="latin-1")
        cls.classes = import_angreal_guide.parse_script_classes(root)

    @classmethod
    def build(cls, roster=BASE_ROSTER, overrides=(), hints=None):
        rows = import_angreal_guide.build_guide_rows(
            cls.classes, list(roster), list(overrides),
            dict(BASE_HINTS if hints is None else hints))
        return {row.class_name: row for row in rows}


class SyntheticDerivationTests(DeriveFixtureMixin, unittest.TestCase):
    def test_element_shield_edge_derivation(self):
        rows = self.build()
        self.assertEqual(rows["CinderWard"].counters,
                         ["EmberLob", "EmberPuff"])
        ember = rows["EmberLob"]
        self.assertEqual(ember.elements, ["Fire"])
        self.assertEqual(ember.rarity, "Common")
        self.assertEqual(ember.roles, ["Offensive"])
        self.assertEqual(ember.max_initial_charges, "4")
        self.assertEqual(ember.max_charges, "9")
        self.assertEqual(ember.counters, [])
        self.assertEqual(ember.hint_key, "EmberLob")
        self.assertEqual(rows["CinderWard"].hint_key, "")

    def test_return_to_sender_edge_via_self_inclusive_ancestors(self):
        rows = self.build()
        # EmberPuff is offensive fire but no ProjectileLauncher descendant;
        # the match of BounceWard's reflector relies on the chain including
        # the reflector class itself (self-inclusive ancestors).
        self.assertEqual(rows["BounceWard"].counters, ["EmberLob", "GustLob"])

    def test_reflector_subclass_carries_return_to_sender_edges(self):
        rows = self.build()
        self.assertEqual(rows["ForkWard"].counters, ["EmberLob", "GustLob"])

    def test_rarity_conflict_is_a_hard_error_forcing_an_override(self):
        muddled = BASE_ROSTER + (RosterEntry("MuddleRelic", "FxMuddle"),)
        with self.assertRaises(GenerationError) as caught:
            self.build(roster=muddled)
        self.assertIn("MuddleRelic", str(caught.exception))
        self.assertIn("Common/Rare", str(caught.exception))
        rows = self.build(roster=muddled, overrides=[
            Override("MuddleRelic", "Rarity", "Uncommon", False, CITED,
                     "test")])
        self.assertEqual(rows["MuddleRelic"].rarity, "Uncommon")

    def test_missing_rarity_is_a_hard_error_and_override_may_blank_it(self):
        blank = BASE_ROSTER + (RosterEntry("BlankRelic", "FxBlank"),)
        with self.assertRaises(GenerationError) as caught:
            self.build(roster=blank)
        self.assertIn("BlankRelic", str(caught.exception))
        self.assertIn("none", str(caught.exception))
        rows = self.build(roster=blank, overrides=[
            Override("BlankRelic", "Rarity", "", False, CITED, "test")])
        self.assertEqual(rows["BlankRelic"].rarity, "")


class OverrideAndHintEnforcementTests(DeriveFixtureMixin, unittest.TestCase):
    def test_override_strips_a_derived_edge(self):
        rows = self.build(overrides=[
            Override("CinderWard", "Counters", "-EmberLob", True, CITED,
                     "test")])
        self.assertEqual(rows["CinderWard"].counters, ["EmberPuff"])
        self.assertEqual(rows["BounceWard"].counters, ["EmberLob", "GustLob"])

    def test_contradiction_without_supersedes_fails(self):
        with self.assertRaises(GenerationError) as caught:
            self.build(overrides=[
                Override("CinderWard", "Counters", "-EmberLob", False, CITED,
                         "test")])
        self.assertIn("Supersedes=yes", str(caught.exception))

    def test_noop_and_unknown_overrides_fail(self):
        for value, message in (
                ("-GustLob", "does not derive"),
                ("+EmberLob", "already derived"),
                ("EmberLob", "+Name or -Name")):
            with self.assertRaises(GenerationError, msg=value) as caught:
                self.build(overrides=[
                    Override("CinderWard", "Counters", value, True, CITED,
                             "test")])
            self.assertIn(message, str(caught.exception))
        with self.assertRaises(GenerationError) as caught:
            self.build(overrides=[
                Override("NoSuchRelic", "Counters", "-EmberLob", True, CITED,
                         "test")])
        self.assertIn("unknown roster class", str(caught.exception))

    def test_override_adds_an_edge_only_with_supersedes_and_roster_member(self):
        rows = self.build(overrides=[
            Override("CinderWard", "Counters", "+GustLob", True, CITED,
                     "test")])
        self.assertEqual(rows["CinderWard"].counters,
                         ["EmberLob", "EmberPuff", "GustLob"])
        with self.assertRaises(GenerationError):
            self.build(overrides=[
                Override("CinderWard", "Counters", "+GustLob", False, CITED,
                         "test")])
        with self.assertRaises(GenerationError) as caught:
            self.build(overrides=[
                Override("CinderWard", "Counters", "+Stranger", True, CITED,
                         "test")])
        self.assertIn("not a roster row", str(caught.exception))

    def test_offensive_row_without_a_hint_fails(self):
        hints = dict(BASE_HINTS)
        del hints["GustLob"]
        with self.assertRaises(GenerationError) as caught:
            self.build(hints=hints)
        self.assertIn("GustLob", str(caught.exception))
        self.assertIn("not in the hints CSV", str(caught.exception))

    def test_unused_hint_fails(self):
        hints = dict(BASE_HINTS, Stray="{title} - never used.")
        with self.assertRaises(GenerationError) as caught:
            self.build(hints=hints)
        self.assertIn("Stray", str(caught.exception))

    def test_counter_placeholder_requires_a_countered_row(self):
        # Nothing counters the trap in this roster, so its template must not
        # say {counter}; {title}-only wording passes (the Balefire shape).
        snared = BASE_ROSTER + (
            RosterEntry("SnareInventory", "FxSnareInventory", is_trap=True),)
        rarity_fill = Override("SnareInventory", "Rarity", "", False, CITED,
                               "test")
        bad = dict(BASE_HINTS, Snare="{title} - {counter} shows the snare.")
        with self.assertRaises(GenerationError) as caught:
            self.build(roster=snared, overrides=[rarity_fill], hints=bad)
        self.assertIn("nothing counters the row", str(caught.exception))
        good = dict(BASE_HINTS, Snare="{title} - tread wide of it.")
        rows = self.build(roster=snared, overrides=[rarity_fill], hints=good)
        self.assertEqual(rows["SnareInventory"].hint_key, "Snare")

    def test_hint_keys_stay_one_per_row(self):
        with self.assertRaises(GenerationError) as caught:
            self.build(overrides=[
                Override("GustLob", "HintKey", "EmberLob", True, CITED,
                         "test")])
        self.assertIn("shared by", str(caught.exception))


class CsvRoundTripTests(DeriveFixtureMixin, unittest.TestCase):
    def test_render_is_deterministic_and_exact(self):
        first = import_angreal_guide.render_facts_csv(
            list(self.build().values()))
        second = import_angreal_guide.render_facts_csv(
            list(self.build().values()))
        self.assertEqual(first, second)
        lines = first.splitlines()
        self.assertEqual(lines[0], ",".join(
            import_angreal_guide.FACTS_COLUMNS))
        self.assertIn("EmberLob,FxEmberLob,Fire,Common,Offensive,4,9,,EmberLob",
                      lines)
        self.assertIn(
            "CinderWard,FxCinderWard,,Rare,Defensive,3,5,EmberLob;EmberPuff,",
            lines)
        self.assertTrue(first.endswith("\n"))
        self.assertNotIn("\r", first)

    def test_render_quotes_like_the_menu_registry(self):
        row = import_angreal_guide.GuideRow(
            class_name='Odd"Relic', cpp_class="Fx,Comma", elements=["Fire"],
            rarity="Common", roles=[], max_initial_charges="1",
            max_charges="2", counters=[], hint_key="")
        text = import_angreal_guide.render_facts_csv([row])
        parsed = list(csv.reader(io.StringIO(text)))
        self.assertEqual(parsed[1][0], 'Odd"Relic')
        self.assertEqual(parsed[1][1], "Fx,Comma")

    def test_loaders_enforce_shape_and_citations(self):
        tree = tempfile.TemporaryDirectory()
        self.addCleanup(tree.cleanup)
        root = Path(tree.name)

        def write(name, text):
            path = root / name
            # Path.write_text only grew `newline` in Python 3.10; the suite
            # must run on 3.9 (the CI floor), so write through open().
            with path.open("w", encoding="utf-8", newline="") as handle:
                handle.write(text)
            return path

        good = write("overrides.csv", (
            "Class,Field,Value,Supersedes,Citation\n"
            'CinderWard,Counters,-EmberLob,yes,"exports/script/Fx/A.uc:1, '
            'with a comma"\n'))
        loaded = import_angreal_guide.load_overrides_csv(good)
        self.assertEqual(len(loaded), 1)
        self.assertTrue(loaded[0].supersedes)
        self.assertIn("comma", loaded[0].citation)
        for name, text, message in (
                ("h.csv", "Class,Field,Value,Citation,Supersedes\nx,y,z,c,s\n",
                 "header"),
                ("c.csv",
                 "Class,Field,Value,Supersedes,Citation\na,Counters,-b,yes,\n",
                 "citation"),
                ("s.csv", "Class,Field,Value,Supersedes,Citation\n"
                          "a,Counters,-b,maybe,exports/script/x.uc:1\n",
                 "Supersedes"),
                ("f.csv", "Class,Field,Value,Supersedes,Citation\n"
                          "a,Sparkle,-b,yes,exports/script/x.uc:1\n",
                 "field")):
            with self.assertRaises(GenerationError, msg=name) as caught:
                import_angreal_guide.load_overrides_csv(write(name, text))
            self.assertIn(message, str(caught.exception))
        hints = write("hints.csv",
                      "HintKey,Template\nSnare,{title} - fine.\n")
        self.assertEqual(import_angreal_guide.load_hints_csv(hints),
                         {"Snare": "{title} - fine."})
        for name, text, message in (
                ("hd.csv", "HintKey,Template\nA,{title} - x.\nA,{title} - y.\n",
                 "duplicate"),
                ("hp.csv", "HintKey,Template\nA,{title} {boom} x.\n",
                 "placeholder"),
                ("ha.csv", "HintKey,Template\nA,{title} — x.\n",
                 "ASCII")):
            with self.assertRaises(GenerationError, msg=name) as caught:
                import_angreal_guide.load_hints_csv(write(name, text))
            self.assertIn(message, str(caught.exception))


class AssetPayloadTests(unittest.TestCase):
    """The editor half's pure layer: CSV read-back, hints onto rows, pins."""

    @staticmethod
    def synthetic_rows():
        make = import_angreal_guide.GuideRow
        return [
            make(class_name="CinderWard", cpp_class="FxCinderWard",
                 elements=[], rarity="Rare", roles=["Combat", "Defensive"],
                 max_initial_charges="3", max_charges="5",
                 counters=["EmberLob"], hint_key=""),
            make(class_name="EmberLob", cpp_class="FxEmberLob",
                 elements=["Fire"], rarity="Common",
                 roles=["Combat", "Offensive"], max_initial_charges="4",
                 max_charges="9", counters=[], hint_key="EmberLob"),
            make(class_name="SnareInventory", cpp_class="FxSnareInventory",
                 elements=[], rarity="", roles=[], max_initial_charges="",
                 max_charges="", counters=[], hint_key="Snare"),
        ]

    HINTS = {"EmberLob": "{title} - {counter} quenches it.",
             "Snare": "{title} - watch the floor."}

    def write(self, name, text):
        tree = tempfile.TemporaryDirectory()
        self.addCleanup(tree.cleanup)
        path = Path(tree.name) / name
        with path.open("w", encoding="utf-8", newline="") as handle:
            handle.write(text)
        return path

    def test_facts_csv_round_trips_through_the_loader(self):
        rows = self.synthetic_rows()
        path = self.write("facts.csv",
                          import_angreal_guide.render_facts_csv(rows))
        self.assertEqual(import_angreal_guide.load_facts_csv(path), rows)

    def test_loader_rejects_duplicate_and_empty_classes(self):
        header = ",".join(import_angreal_guide.FACTS_COLUMNS)
        duplicate = self.write("d.csv", header + "\n"
                               + "A,FxA,,,,,,,\n" + "A,FxA,,,,,,,\n")
        with self.assertRaises(GenerationError) as caught:
            import_angreal_guide.load_facts_csv(duplicate)
        self.assertIn("duplicate", str(caught.exception))
        empty = self.write("e.csv", header + "\n" + ",FxA,,,,,,,\n")
        with self.assertRaises(GenerationError) as caught:
            import_angreal_guide.load_facts_csv(empty)
        self.assertIn("empty Class", str(caught.exception))

    def test_asset_rows_resolve_hints_and_charges(self):
        payload = import_angreal_guide.asset_rows(self.synthetic_rows(),
                                                  dict(self.HINTS))
        by_class = {row["class_name"]: row for row in payload}
        self.assertEqual(by_class["EmberLob"]["hint_text"],
                         self.HINTS["EmberLob"])
        self.assertEqual(by_class["CinderWard"]["hint_text"], "")
        self.assertEqual(by_class["SnareInventory"]["hint_text"],
                         self.HINTS["Snare"])
        self.assertEqual(by_class["EmberLob"]["max_initial_charges"], 4)
        self.assertEqual(by_class["EmberLob"]["max_charges"], 9)
        self.assertEqual(by_class["SnareInventory"]["max_initial_charges"], 0)
        self.assertEqual(by_class["SnareInventory"]["max_charges"], 0)
        self.assertEqual(by_class["CinderWard"]["counters"], ["EmberLob"])

    def test_asset_rows_reject_unknown_hints_and_bad_charges(self):
        with self.assertRaises(GenerationError) as caught:
            import_angreal_guide.asset_rows(self.synthetic_rows(), {})
        self.assertIn("hints CSV", str(caught.exception))
        broken = self.synthetic_rows()
        broken[1].max_charges = "many"
        with self.assertRaises(GenerationError) as caught:
            import_angreal_guide.asset_rows(broken, dict(self.HINTS))
        self.assertIn("not an integer", str(caught.exception))

    def test_payload_json_round_trips(self):
        payload = [{
            "class_name": "FixtureLob", "cpp_class": "FxFixtureLob",
            "elements": ["Fire"], "rarity": "", "roles": [],
            "max_initial_charges": 0, "max_charges": 9,
            "counters": ["FixtureWard"], "hint_text": "{title} - x.",
        }]
        text = import_angreal_guide.payload_json(payload)
        self.assertEqual(import_angreal_guide.payload_from_json(text), payload)

    def test_payload_from_json_requires_a_rows_array(self):
        for bad in ("{}", "[]", '{"rows": 7}'):
            with self.assertRaises(import_angreal_guide.GenerationError):
                import_angreal_guide.payload_from_json(bad)

    def test_verify_pins_the_fireball_row_and_the_roster_floor(self):
        payload = import_angreal_guide.asset_rows(
            import_angreal_guide.load_facts_csv(),
            import_angreal_guide.load_hints_csv())
        ok, pins, counts = import_angreal_guide.verify_asset_payload(
            payload, payload)
        self.assertTrue(ok, pins)
        self.assertTrue(all(pins.values()), pins)
        self.assertGreaterEqual(counts["rows"],
                                import_angreal_guide.MIN_ASSET_ROWS)
        self.assertEqual(counts["rows"], counts["unique_classes"])

    def test_verify_fails_on_drift_tamper_and_shortfall(self):
        payload = import_angreal_guide.asset_rows(
            import_angreal_guide.load_facts_csv(),
            import_angreal_guide.load_hints_csv())
        drifted = json.loads(json.dumps(payload))
        drifted[0]["cpp_class"] = "FxTampered"
        ok, _, _ = import_angreal_guide.verify_asset_payload(drifted, payload)
        self.assertFalse(ok)
        tampered = json.loads(json.dumps(payload))
        for row in tampered:
            if row["class_name"] == import_angreal_guide.FIREBALL_CLASS:
                row["rarity"] = "Rare"
        ok, pins, _ = import_angreal_guide.verify_asset_payload(
            tampered, tampered)
        self.assertFalse(ok)
        self.assertFalse(pins["fireball_rarity"])
        short = payload[:3]
        ok, pins, _ = import_angreal_guide.verify_asset_payload(short, short)
        self.assertFalse(ok)
        self.assertFalse(pins["row_count_at_least_%d"
                              % import_angreal_guide.MIN_ASSET_ROWS])


class InTreeGuideDataTests(unittest.TestCase):
    """The checked-in CSVs stay complete against the in-tree expectations."""

    @classmethod
    def setUpClass(cls):
        cls.text = import_angreal_guide.FACTS_CSV_PATH.read_text(
            encoding="utf-8")
        cls.facts = list(csv.DictReader(io.StringIO(cls.text)))
        cls.by_class = {row["Class"]: row for row in cls.facts}
        cls.hints = import_angreal_guide.load_hints_csv()
        cls.overrides = import_angreal_guide.load_overrides_csv()
        document = json.loads(
            import_angreal_guide.EXPECTATIONS_JSON_PATH.read_text(
                encoding="utf-8"))
        cls.expected_cpp = {}
        for entry in document["classes"]:
            stem = Path(entry["uc"]).stem
            previous = cls.expected_cpp.setdefault(stem, entry["cpp_class"])
            assert previous == entry["cpp_class"], stem
        cls.angreal_rows = [row for row in cls.facts if
                            row["Class"].startswith("AngrealInv")]
        cls.trap_rows = [row for row in cls.facts if
                         not row["Class"].startswith("AngrealInv")]

    def test_facts_csv_parses_with_the_documented_columns(self):
        reader = csv.reader(io.StringIO(self.text))
        self.assertEqual(tuple(next(reader)),
                         import_angreal_guide.FACTS_COLUMNS)
        self.assertNotIn("\r", self.text)
        self.assertTrue(self.text.isascii())
        self.assertEqual(len(self.by_class), len(self.facts))

    def test_every_expectations_entry_has_a_facts_row_and_vice_versa(self):
        self.assertEqual(
            {row["Class"] for row in self.angreal_rows},
            set(self.expected_cpp))
        for row in self.angreal_rows:
            self.assertEqual(row["CppClass"],
                             self.expected_cpp[row["Class"]], row["Class"])

    def test_trap_rows_close_with_trap_detection(self):
        trap_names = sorted((row["Class"] for row in self.trap_rows),
                            key=str.lower)
        self.assertTrue(trap_names)
        for name in trap_names:
            self.assertTrue(name.endswith("Inventory"), name)
        detect = self.by_class["AngrealInvTrapDetect"]
        self.assertEqual(detect["Counters"].split(";"), trap_names)
        for row in self.trap_rows:
            self.assertEqual(row["Counters"], "", row["Class"])

    def test_hint_keys_cover_offensive_and_trap_rows_exactly(self):
        used = {}
        for row in self.facts:
            needs_hint = ("Offensive" in row["Roles"].split(";")
                          or not row["Class"].startswith("AngrealInv"))
            self.assertEqual(bool(row["HintKey"]), needs_hint, row["Class"])
            if row["HintKey"]:
                self.assertNotIn(row["HintKey"], used, row["Class"])
                used[row["HintKey"]] = row["Class"]
                self.assertIn(row["HintKey"], self.hints, row["Class"])
        self.assertEqual(sorted(used), sorted(self.hints))

    def test_overrides_cite_the_exports(self):
        self.assertTrue(self.overrides)
        for override in self.overrides:
            self.assertIn("exports/script/", override.citation,
                          override.source)
            if override.value.startswith(("+", "-")):
                self.assertTrue(override.supersedes, override.source)


@unittest.skipUnless(import_angreal_guide.reference_root_available(),
                     "WOT_REFERENCE_ROOT with exports/script is not configured")
class ReferenceGeneratedGuideTests(unittest.TestCase):
    """Real-data pins and the generated-equals-checked-in gate."""

    rows = None

    @classmethod
    def setUpClass(cls):
        cls.rows = import_angreal_guide.generate_rows()
        cls.by_name = {row.class_name: row for row in cls.rows}

    def test_generated_csv_matches_the_checked_in_file_byte_for_byte(self):
        self.assertEqual(
            import_angreal_guide.render_facts_csv(self.rows).encode("utf-8"),
            import_angreal_guide.FACTS_CSV_PATH.read_bytes())

    def test_fire_shield_counters_the_fireball(self):
        self.assertIn("AngrealInvFireball",
                      self.by_name["AngrealInvFireShield"].counters)

    def test_reflect_counters_the_seeker_and_the_dart(self):
        reflect = self.by_name["AngrealInvReflect"]
        self.assertIn("AngrealInvSeeker", reflect.counters)
        self.assertIn("AngrealInvDart", reflect.counters)

    def test_balefire_is_countered_by_nothing(self):
        for row in self.rows:
            self.assertNotIn("AngrealInvBalefire", row.counters,
                             row.class_name)


if __name__ == "__main__":
    unittest.main()
