"""Exact cache identities for colliding retail export names."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(Path(__file__).resolve().parent), str(TOOLS)]
import retail_identity as identity  # noqa: E402
from wot_config import original_dir  # noqa: E402

ORIGINAL = original_dir(required=False)


class _Package:
    """The ue1pkg.Package surface typed_export_identities consumes."""

    def __init__(self, name: str, exports: list[tuple[str, str, int]]):
        self.name = name
        # (class name, object name, 1-based outer export or 0)
        self.exports = [{"cls": index + 1000, "name": object_name, "group": outer,
                         "class_name": class_name}
                        for index, (class_name, object_name, outer) in enumerate(exports)]

    def objname(self, reference: int) -> str:
        for export in self.exports:
            if export["cls"] == reference:
                return export["class_name"]
        return "Package"

    def objpath(self, reference: int) -> str:
        parts = []
        current = reference
        while current:
            export = self.exports[current - 1]
            parts.append(export["name"])
            current = export["group"]
        return ".".join(reversed(parts))


class RetailIdentityTests(unittest.TestCase):
    def test_unique_names_keep_the_plain_identity(self):
        package = _Package("NatureT", [
            ("Package", "Sky", 0), ("Texture", "Sky_340a", 1),
            ("Texture", "Watr_22", 0)])
        rows = identity.typed_export_identities(package, {"Texture"})
        self.assertEqual([(row.base, row.fact_key, row.colliding) for row in rows], [
            ("NatureT__Sky_340a", "naturet.sky_340a", False),
            ("NatureT__Watr_22", "naturet.watr_22", False)])

    def test_colliding_names_are_all_exact_and_never_aliased(self):
        package = _Package("AesSedaiT", [
            ("Package", "Ornament", 0), ("Package", "SurfaceWall", 0),
            ("Texture", "PaprWall1441", 1), ("Texture", "paprwall1441", 2),
            ("Texture", "Rain3", 0), ("Package", "Effects", 0),
            ("Texture", "Rain3", 6)])
        rows = {row.base: row for row in identity.typed_export_identities(
            package, {"Texture"})}
        self.assertEqual(sorted(rows), [
            "AesSedaiT.Effects__Rain3", "AesSedaiT.Ornament__PaprWall1441",
            "AesSedaiT.SurfaceWall__paprwall1441", "AesSedaiT__Rain3"])
        self.assertTrue(all(row.colliding for row in rows.values()))
        self.assertEqual(rows["AesSedaiT.Ornament__PaprWall1441"].fact_key,
                         "aessedait.ornament.paprwall1441")
        # A top-level colliding export's exact identity is the plain form.
        self.assertEqual(rows["AesSedaiT__Rain3"].groups, ())

    def test_group_names_are_escaped_for_the_logical_id_grammar(self):
        self.assertEqual(identity.encode_group("(All)"), "-28All-29")
        self.assertEqual(identity.encode_group("A-B c"), "A-2DB-20c")
        self.assertEqual(identity.exact_base("FootStubA", ("(All)",), "METL"),
                         "FootStubA.-28All-29__METL")

    def test_same_full_path_is_refused(self):
        package = _Package("P", [("Package", "G", 0), ("Texture", "T", 1),
                                 ("Texture", "t", 1)])
        with self.assertRaises(ValueError):
            identity.typed_export_identities(package, {"Texture"})

    def test_exact_first_resolution_and_paths(self):
        present = {"AmbientA.Nature__Rustle3", "AmbientA.Bush__Rustle3",
                   "NatureT__Sky_340a"}
        self.assertEqual(identity.resolve_base(
            "AmbientA", ("Nature",), "Rustle3", present.__contains__),
            "AmbientA.Nature__Rustle3")
        self.assertEqual(identity.resolve_base(
            "Naturet", ("Sky",), "Sky_340a", present.__contains__), "Naturet__Sky_340a")
        self.assertEqual(identity.split_object_path(
            "ParticleSystems.Fire.Fire_Torch_001#export-00000575"),
            ("ParticleSystems", ("Fire",), "Fire_Torch_001"))
        with self.assertRaises(ValueError):
            identity.split_object_path("Bare")

    @unittest.skipUnless((ORIGINAL / "Textures" / "AesSedaiT.utx").is_file(),
                         "retail packages are absent")
    def test_retail_collision_census(self):
        sys.path.insert(0, str(TOOLS / "convert"))
        from ue1pkg import Package  # pylint: disable=import-outside-toplevel
        from build_payload_cache import source_packages  # pylint: disable=import-outside-toplevel
        procedural = {"FireTexture", "WaterTexture", "WaveTexture", "WetTexture",
                      "IceTexture"}
        found = {}
        for domain, kind, classes in (
                ("texture", "textures", {"Texture"}), ("sound", "sounds", {"Sound"}),
                ("mesh", "meshes", {"Mesh", "LodMesh"}),
                ("procedural", "textures", procedural), ("font", "textures", {"Font"})):
            for path in source_packages(ORIGINAL, kind):
                for row in identity.colliding_identities(Package(path), classes):
                    found.setdefault(domain, []).append(row.base)
        self.assertEqual(sorted(found), ["procedural", "sound", "texture"])
        self.assertEqual(sorted(found["sound"]), [
            "AmbientA.Bush__Rustle3", "AmbientA.Nature__Rustle3",
            "FootStubA.-28All-29__METL", "FootStubA__METL"])
        self.assertEqual(sorted(found["procedural"]),
                         ["WarrenT.Effects__Rain3", "WarrenT__Rain3"])
        self.assertEqual(len(found["texture"]), 36)
        self.assertIn("AesSedaiT.Ornament__PaprWall1441", found["texture"])
        self.assertIn("WOTParticles.ModulatedBlood__Blood15", found["texture"])


if __name__ == "__main__":
    unittest.main()
