#!/usr/bin/env python3
"""Guards for the shared texture sampling policy (texture_sampling.py).

No editor: an AST check that every texture import or load site in the content
pipeline goes through the shared policy with an explicit family, and mocked
behaviour tests of the policy and of the importer paths that used to skip it
(fresh import, existing wrong asset, already correct, stripped reimport,
early-return paths, idempotence, missing textures).
"""
from __future__ import annotations

import ast
import importlib
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

IMPORT_DIR = Path(__file__).resolve().parent
TOOLS_DIR = IMPORT_DIR.parent
REPO = TOOLS_DIR.parent
for _path in (IMPORT_DIR, TOOLS_DIR / "convert", TOOLS_DIR):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

import texture_sampling as ts  # noqa: E402
from texture_sampling import (  # noqa: E402
    EFFECT,
    LIGHTMAP,
    LIGHTMAP_RESPONSE,
    UI,
    WORLD,
    TextureSamplingError,
)
import converge_texture_sampling  # noqa: E402
import hud_texture_manifest  # noqa: E402
import stripped_texture_import  # noqa: E402


# --------------------------------------------------------------------------
# A minimal editor: an asset store, PNG imports (srgb on, like UE's PNG
# factory; a replace keeps the existing settings unless told otherwise) and
# saves. Anything else an importer touches at import time is a MagicMock.

class FakeTexture:
    def __init__(self, path, srgb=True, source=None):
        self.path = path
        self.properties = {"srgb": srgb, "source": source,
                           "mip_gen_settings": "from-texture-group",
                           "never_stream": False, "asset_import_data": None}
        self.set_calls = []

    def get_editor_property(self, name):
        return self.properties[name]

    def set_editor_property(self, name, value):
        self.properties[name] = value
        self.set_calls.append((name, value))

    def get_path_name(self):
        return f"{self.path}.{self.path.rsplit('/', 1)[1]}"


class FakeImportTask:
    def __init__(self):
        self.filename = ""
        self.destination_path = ""
        self.destination_name = ""
        self.automated = False
        self.save = True
        self.replace_existing = False
        self.replace_existing_settings = False


class FakeEditor:
    Texture2D = FakeTexture
    AssetImportTask = FakeImportTask

    def __init__(self, saved_dir="."):
        self.assets = {}
        self.imports = []
        self.saved = []
        self.reset_srgb_on_replace = False
        editor = self

        class EditorAssetLibrary:
            @staticmethod
            def does_asset_exist(path):
                return ts.asset_package_path(path).casefold() in editor.assets

            @staticmethod
            def save_asset(path, only_if_is_dirty=True):
                editor.saved.append(ts.asset_package_path(path))
                return True

            @staticmethod
            def list_assets(root, recursive=True, include_folder=False):
                prefix = root.rstrip("/").casefold() + "/"
                return [item.get_path_name() for key, item in editor.assets.items()
                        if key.startswith(prefix)]

        self.EditorAssetLibrary = EditorAssetLibrary
        self._asset_tools = SimpleNamespace(
            import_asset_tasks=self._import_tasks,
            create_asset=mock.MagicMock(name="create_asset"))
        self.AssetToolsHelpers = SimpleNamespace(
            get_asset_tools=lambda: self._asset_tools)
        self.Paths = SimpleNamespace(
            project_saved_dir=lambda: str(saved_dir),
            project_dir=lambda: str(REPO),
            project_content_dir=lambda: str(REPO / "Content"),
            convert_relative_path_to_full=lambda path: path)
        self._fallback = mock.MagicMock(name="unreal")

    def __getattr__(self, name):
        return getattr(self._fallback, name)

    def add(self, path, srgb=True):
        texture = FakeTexture(path, srgb)
        self.assets[path.casefold()] = texture
        return texture

    def add_object(self, path, value):
        self.assets[path.casefold()] = value
        return value

    def load_asset(self, path):
        return self.assets.get(ts.asset_package_path(path).casefold())

    def srgb(self, path):
        return self.assets[path.casefold()].properties["srgb"]

    def _import_tasks(self, tasks):
        for task in tasks:
            self.imports.append(task)
            name = task.destination_name or Path(task.filename).stem
            path = f"{task.destination_path}/{name}"
            existing = self.assets.get(path.casefold())
            if existing is not None and not task.replace_existing:
                continue
            srgb = True
            if (existing is not None and not task.replace_existing_settings
                    and not self.reset_srgb_on_replace):
                srgb = existing.properties["srgb"]
            self.assets[path.casefold()] = FakeTexture(path, srgb, task.filename)


def _png(directory, name):
    path = Path(directory) / f"{name}.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(stripped_texture_import.solid_placeholder_png(2, 2))
    return path


def _import_with_editor(name, argv, editor):
    """Import one importer module against ``editor`` without running main()."""
    sys.modules.pop(name, None)
    with mock.patch.dict(sys.modules, {"unreal": editor}), \
            mock.patch.object(sys, "argv", list(argv)):
        return importlib.import_module(name)


# --------------------------------------------------------------------------
# The policy table.

class PolicyTableTests(unittest.TestCase):
    def test_family_settings(self):
        self.assertFalse(ts.desired_srgb(WORLD))
        self.assertFalse(ts.desired_srgb(EFFECT))
        self.assertTrue(ts.desired_srgb(UI))
        self.assertFalse(ts.desired_srgb(LIGHTMAP_RESPONSE))
        self.assertFalse(ts.desired_srgb(LIGHTMAP, gamma_space=True))

    def test_lightmap_rule_matches_import_map(self):
        # Composed atlas and legacy endpoints: srgb = not GAMMA_SPACE.
        self.assertTrue(ts.desired_srgb(LIGHTMAP, gamma_space=False))
        self.assertFalse(ts.desired_srgb(LIGHTMAP, gamma_space=True))
        # Response atlases stay raw in either pipeline; UI stays sRGB.
        self.assertFalse(ts.desired_srgb(LIGHTMAP_RESPONSE, gamma_space=False))
        self.assertTrue(ts.desired_srgb(UI, gamma_space=False))

    def test_unknown_family_and_bad_gamma_flag_fail(self):
        with self.assertRaises(TextureSamplingError):
            ts.desired_srgb("hud")
        with self.assertRaises(TextureSamplingError):
            ts.desired_srgb(WORLD, gamma_space=None)

    def test_asset_package_path(self):
        self.assertEqual(ts.asset_package_path("/Game/A/B.B"), "/Game/A/B")
        self.assertEqual(ts.asset_package_path("/Game/A/B"), "/Game/A/B")
        self.assertEqual(
            ts.asset_package_path(FakeTexture("/Game/A/Stop__5")),
            "/Game/A/Stop__5")


# --------------------------------------------------------------------------
# Mocked behaviour of the shared helpers.

class PolicyBehaviourTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.editor = FakeEditor(self.directory.name)
        self.png = _png(self.directory.name, "decaydrop")
        self.path = "/Game/WoT/Textures/ParticleSystems/decaydrop"

    def test_fresh_import_takes_the_family_and_saves_once(self):
        texture, changed = ts.import_texture(
            self.editor, self.png, self.path, EFFECT)
        self.assertTrue(changed)
        self.assertFalse(texture.get_editor_property("srgb"))
        self.assertEqual(len(self.editor.imports), 1)
        task = self.editor.imports[0]
        self.assertEqual(
            (task.destination_path, task.destination_name, task.save),
            ("/Game/WoT/Textures/ParticleSystems", "decaydrop", False))
        self.assertEqual(self.editor.saved, [self.path])

    def test_fresh_ui_import_keeps_srgb(self):
        texture, changed = ts.import_texture(
            self.editor, self.png, "/Game/WoT/UI/Crosshair1", UI)
        self.assertTrue(changed)          # imported
        self.assertTrue(texture.get_editor_property("srgb"))

    def test_existing_wrong_asset_converges_without_reimport(self):
        self.editor.add(self.path, srgb=True)
        missing_png = Path(self.directory.name) / "absent.png"
        texture, changed = ts.import_texture(
            self.editor, missing_png, self.path, EFFECT)
        self.assertTrue(changed)
        self.assertFalse(texture.get_editor_property("srgb"))
        self.assertEqual(self.editor.imports, [])
        self.assertEqual(self.editor.saved, [self.path])

    def test_already_correct_asset_is_neither_modified_nor_saved(self):
        texture = self.editor.add(self.path, srgb=False)
        result, changed = ts.import_texture(
            self.editor, self.png, self.path, EFFECT)
        self.assertIs(result, texture)
        self.assertFalse(changed)
        self.assertEqual(texture.set_calls, [])
        self.assertEqual((self.editor.imports, self.editor.saved), ([], []))

    def test_idempotent_second_pass(self):
        ts.import_texture(self.editor, self.png, self.path, WORLD)
        imports, saves = len(self.editor.imports), len(self.editor.saved)
        _, changed = ts.import_texture(self.editor, self.png, self.path, WORLD)
        _, changed_again = ts.load_texture(self.editor, self.path, WORLD)
        self.assertFalse(changed or changed_again)
        self.assertEqual(
            (len(self.editor.imports), len(self.editor.saved)), (imports, saves))

    def test_save_false_leaves_the_save_to_the_caller(self):
        self.editor.add(self.path, srgb=True)
        texture, changed = ts.load_texture(
            self.editor, self.path, EFFECT, save=False)
        self.assertTrue(changed)
        self.assertFalse(texture.get_editor_property("srgb"))
        self.assertEqual(self.editor.saved, [])

    def test_replace_existing_reimports_then_converges(self):
        self.editor.add(self.path, srgb=True)
        texture, changed = ts.import_texture(
            self.editor, self.png, self.path, LIGHTMAP, gamma_space=True,
            save=False, replace_existing=True)
        self.assertTrue(changed)
        self.assertTrue(self.editor.imports[0].replace_existing)
        self.assertFalse(texture.get_editor_property("srgb"))

    def test_missing_textures_fail(self):
        with self.assertRaisesRegex(TextureSamplingError, "missing"):
            ts.load_texture(self.editor, self.path, WORLD)
        with self.assertRaisesRegex(TextureSamplingError, "source PNG is missing"):
            ts.import_texture(self.editor, Path(self.directory.name) / "nope.png",
                              self.path, WORLD)
        with self.assertRaisesRegex(TextureSamplingError, "missing"):
            ts.verify_texture_sampling(self.editor, self.path, WORLD)
        self.editor.add_object(self.path, object())
        with self.assertRaisesRegex(TextureSamplingError, "not a Texture2D"):
            ts.load_texture(self.editor, self.path, WORLD)

    def test_verify_never_writes(self):
        texture = self.editor.add(self.path, srgb=False)
        self.assertIs(ts.verify_texture_sampling(self.editor, self.path, WORLD),
                      texture)
        texture.properties["srgb"] = True
        with self.assertRaisesRegex(TextureSamplingError, "requires srgb=False"):
            ts.verify_texture_sampling(self.editor, self.path, WORLD)
        self.assertEqual((texture.set_calls, self.editor.saved), ([], []))

    def test_stripped_reimport_applies_the_family(self):
        spec = {"source_png": self.png, "asset_path": self.path,
                "package": "ParticleSystems", "source_name": "decaydrop",
                "width": 2, "height": 2, "destination_mip_count": 1}
        self.editor.add(self.path, srgb=True)   # not the current placeholder
        with mock.patch.object(stripped_texture_import,
                               "ensure_texture_payload_binding",
                               return_value=(None, False)), \
                mock.patch.object(stripped_texture_import, "_texture_dimensions",
                                  return_value=(2, 2)):
            texture, changed = stripped_texture_import.import_stripped_texture(
                self.editor, spec, family=EFFECT)
            self.assertTrue(changed)
            task = self.editor.imports[-1]
            self.assertTrue(task.replace_existing)
            self.assertFalse(task.replace_existing_settings)
            self.assertFalse(texture.get_editor_property("srgb"))
            # Rerun on the now-current placeholder: no import, no save.
            saves = len(self.editor.saved)
            with mock.patch.object(stripped_texture_import,
                                   "stripped_texture_is_current",
                                   return_value=True):
                again, changed = stripped_texture_import.import_stripped_texture(
                    self.editor, spec, family=EFFECT)
            self.assertIs(again, texture)
            self.assertFalse(changed)
            self.assertEqual(len(self.editor.saved), saves)
            self.assertEqual(len(self.editor.imports), 1)
            # A current placeholder with the wrong flag still converges.
            texture.properties["srgb"] = True
            with mock.patch.object(stripped_texture_import,
                                   "stripped_texture_is_current",
                                   return_value=True):
                _, changed = stripped_texture_import.import_stripped_texture(
                    self.editor, spec, family=EFFECT)
            self.assertTrue(changed)
            self.assertFalse(texture.get_editor_property("srgb"))

    def test_stripped_ui_placeholder_stays_srgb(self):
        path = "/Game/WoT/UI/Crosshair1"
        spec = {"source_png": self.png, "asset_path": path, "package": "WOT",
                "source_name": "Crosshair1", "width": 2, "height": 2,
                "destination_mip_count": 1}
        with mock.patch.object(stripped_texture_import,
                               "ensure_texture_payload_binding",
                               return_value=(None, False)), \
                mock.patch.object(stripped_texture_import, "_texture_dimensions",
                                  return_value=(2, 2)):
            texture, _ = stripped_texture_import.import_stripped_texture(
                self.editor, spec, family=UI)
        self.assertTrue(texture.get_editor_property("srgb"))

    def test_payload_strip_keeps_the_owner_setting(self):
        self.editor.add(self.path, srgb=False)
        self.editor.reset_srgb_on_replace = True   # a factory that resets it
        texture = ts.replace_texture_source(self.editor, self.png, self.path)
        task = self.editor.imports[0]
        self.assertTrue(task.replace_existing)
        self.assertFalse(task.replace_existing_settings)
        self.assertFalse(texture.get_editor_property("srgb"))
        ui_path = "/Game/WoT/UI/Warn"
        self.editor.add(ui_path, srgb=True)
        self.assertTrue(ts.replace_texture_source(
            self.editor, self.png, ui_path).get_editor_property("srgb"))


# --------------------------------------------------------------------------
# Claims, resolution, convergence and the scan.

class OwnershipTests(unittest.TestCase):
    def test_named_claim_outranks_blanket(self):
        path = "/Game/WoT/Textures/WOT/T32_01"
        owned = ts.resolve_claims([
            ts.claim(path, WORLD, "import_map", blanket=True),
            ts.claim(path, UI, "import_hud_textures"),
        ])[path.casefold()]
        self.assertTrue(owned.srgb)
        self.assertEqual((owned.family, owned.owners, owned.conflicts),
                         ("ui", ("import_hud_textures",), ()))

    def test_world_and_effect_agree(self):
        path = "/Game/WoT/Textures/Angreal/DC_A01"
        owned = ts.resolve_claims([
            ts.claim(path, EFFECT, "import_weave_sprites"),
            ts.claim(path, WORLD, "import_map"),
        ])[path.casefold()]
        self.assertFalse(owned.srgb)
        self.assertEqual(owned.family, "effect+world")

    def test_named_world_and_ui_conflict_is_reported_not_written(self):
        path = "/Game/WoT/Textures/WoTPawns/I_SMashadar"
        editor = FakeEditor()
        texture = editor.add(path, srgb=True)
        claims = [ts.claim(path, WORLD, "import_map"),
                  ts.claim(path, UI, "import_hud_textures")]
        report = ts.converge_claims(editor, claims)
        self.assertEqual(len(report["conflicts"]), 1)
        self.assertEqual((report["changed"], texture.set_calls, editor.saved),
                         ([], [], []))

    def test_convergence_changes_only_mismatched_srgb_and_lists_them(self):
        editor = FakeEditor()
        wrong = editor.add("/Game/WoT/Textures/ParticleSystems/decaydrop", True)
        right = editor.add("/Game/WoT/Textures/ParticleSystems/decaysplat", False)
        hud = editor.add("/Game/WoT/Textures/WOT/T32_01", False)
        claims = [
            ts.claim(wrong.path, EFFECT, "import_flame_fx"),
            ts.claim(right.path, EFFECT, "import_gore_fx"),
            ts.claim(hud.path, WORLD, "import_map", blanket=True),
            ts.claim(hud.path, UI, "import_hud_textures"),
            ts.claim("/Game/WoT/Textures/WOT/Absent", WORLD, "import_map"),
        ]
        dry = ts.converge_claims(editor, claims, apply=False)
        self.assertEqual(len(dry["changed"]), 2)
        self.assertEqual(editor.saved, [])
        report = ts.converge_claims(editor, claims)
        self.assertEqual(
            sorted((item["asset"], item["before"], item["after"])
                   for item in report["changed"]),
            [(wrong.path, True, False), (hud.path, False, True)])
        self.assertEqual(report["unchanged"], 1)
        self.assertEqual([item["asset"] for item in report["missing"]],
                         ["/Game/WoT/Textures/WOT/Absent"])
        self.assertEqual(sorted(editor.saved), sorted([wrong.path, hud.path]))
        self.assertEqual(right.set_calls, [])
        again = ts.converge_claims(editor, claims)
        self.assertEqual(again["changed"], [])

    def test_scan_reports_family_srgb_and_status(self):
        editor = FakeEditor()
        owned = editor.add("/Game/WoT/Textures/ParticleSystems/decaydrop", True)
        unowned = editor.add("/Game/WoT/FirstPerson/Hands/T_hand", True)
        assets = [SimpleNamespace(package_name=item.path, get_asset=lambda item=item: item)
                  for item in (owned, unowned)]
        registry = mock.MagicMock()
        registry.get_assets.return_value = assets
        editor._fallback.AssetRegistryHelpers.get_asset_registry.return_value = registry
        claims = [ts.claim(owned.path, EFFECT, "import_flame_fx")]
        rows = ts.scan_textures(editor, claims)
        self.assertEqual([(row["asset"], row["family"], row["srgb"], row["status"])
                          for row in rows],
                         [(unowned.path, "unowned", True, "unowned"),
                          (owned.path, "effect", True, "mismatch")])
        self.assertEqual(ts.scan_totals(rows),
                         {"total": 2, "ok": 0, "mismatch": 1, "conflict": 0,
                          "unowned": 1})


# --------------------------------------------------------------------------
# Importer regressions: the paths that used to skip the policy.

class ImporterRegressionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.editor = FakeEditor(self.root / "Saved")

    def test_flame_statics_take_the_effect_policy_not_only_the_neutral(self):
        flame = _import_with_editor(
            "import_flame_fx", ["import_flame_fx.py", str(self.root)],
            self.editor)
        for name in ("M_WoT_ParticleAdd", "M_WoT_ParticleMod", "M_WoT_Decal"):
            self.editor.add_object(f"/Game/WoT/Materials/{name}", mock.MagicMock())
        statics = [f"/Game/WoT/Textures/{pkg}/{name}" for pkg, name in flame.STATICS]
        for path in statics + [flame.NEUTRAL, flame.SMOKE]:
            self.editor.add(path, srgb=True)
        with mock.patch.object(flame, "ensure_texture_payload_binding"), \
                mock.patch.object(flame, "_material_has_scalar_param",
                                  return_value=False):
            flame.main()
        for path in statics + [flame.NEUTRAL, flame.SMOKE]:
            self.assertFalse(self.editor.srgb(path), path)
        for name in ("decaydrop", "decaydrop1", "decaysplat"):
            self.assertIn(f"/Game/WoT/Textures/ParticleSystems/{name}", statics)
        self.assertEqual(self.editor.imports, [])
        claims = {item.asset_path: item.family
                  for item in flame.texture_sampling_claims()}
        self.assertEqual(claims["/Game/WoT/Textures/ParticleSystems/decaydrop1"],
                         EFFECT)
        self.assertEqual(claims[flame.NEUTRAL], WORLD)

    def test_flipbook_static_frame_early_return_converges(self):
        flipbooks = _import_with_editor(
            "import_flipbooks", ["import_flipbooks.py", str(self.root)],
            self.editor)
        path = "/Game/WoT/Textures/Angreal/RedIce"
        self.editor.add(path, srgb=True)
        info = {"static_frame": "RedIce_frame.png", "texture_name": "RedIce"}
        flipbooks.import_static_frame("Angreal", info, self.root)
        self.assertFalse(self.editor.srgb(path))
        self.assertEqual(self.editor.imports, [])
        frame = _png(self.root, "YeloIce_frame")
        flipbooks.import_static_frame(
            "Angreal", {"static_frame": frame.name, "texture_name": "YeloIce"},
            self.root)
        self.assertFalse(self.editor.srgb("/Game/WoT/Textures/Angreal/YeloIce"))
        self.assertEqual(self.editor.imports[0].destination_name, "YeloIce")

    def test_trap_skin_early_return_converges(self):
        traps = _import_with_editor(
            "import_trap_skins", ["import_trap_skins.py", str(self.root)],
            self.editor)
        path = "/Game/WoT/Textures/WoTTraps/TrapWall2"
        self.editor.add(path, srgb=True)
        with mock.patch.object(traps, "ensure_texture_payload_binding"):
            texture, imported = traps.load_or_import_texture("WoTTraps", "TrapWall2")
        self.assertFalse(imported)
        self.assertFalse(texture.get_editor_property("srgb"))

    def test_player_and_seal_variants_use_the_world_policy(self):
        players = _import_with_editor(
            "import_player_skins", ["import_player_skins.py"], self.editor)
        seals = _import_with_editor(
            "import_seal_skins", ["import_seal_skins.py"], self.editor)
        for module in (players, seals):
            claims = module.texture_sampling_claims()
            self.assertTrue(claims)
            self.assertEqual({item.family for item in claims}, {WORLD})
        source = (IMPORT_DIR / "import_player_skins.py").read_text(encoding="utf-8")
        self.assertNotIn("base_srgb", source)
        source = (IMPORT_DIR / "import_seal_skins.py").read_text(encoding="utf-8")
        self.assertNotIn("base_srgb", source)

    def _mesh_tree(self):
        texroot = self.root / "tex"
        package = texroot / "WoTDecorations"
        _png(package, "JTree0")
        _png(package, "JTree0_key0")
        (package / "_manifest.json").write_text(json.dumps({"textures": {
            "JTree0": {"w": 2, "h": 2, "palette0": [0, 0, 0],
                       "key0": "JTree0_key0.png"}}}), encoding="utf-8")
        mesh = self.root / "mesh"
        mesh.mkdir()
        (mesh / "Tree.meta.json").write_text(json.dumps(
            {"slot_types": {"JTree0_M": 2}}), encoding="utf-8")
        return texroot, mesh

    def test_mesh_key0_variant_is_raw_and_unbound(self):
        texroot, mesh = self._mesh_tree()
        importer = _import_with_editor(
            "import_mesh", ["import_mesh.py", "Tree", str(mesh), str(texroot)],
            self.editor)
        key0 = "/Game/WoT/Textures/WoTDecorations/JTree0_key0"
        base = "/Game/WoT/Textures/WoTDecorations/JTree0"
        self.editor.add(base, srgb=True)            # existing, wrong
        with mock.patch.object(importer, "ensure_texture_payload_binding") as bind:
            path, package, source_name, _info = importer.import_texture(
                "JTree0", key0=True)
            importer.import_texture("JTree0", key0=False)
        self.assertEqual((path, package, source_name),
                         (key0, "WoTDecorations", "JTree0"))
        self.assertFalse(self.editor.srgb(key0))    # fresh import, raw
        self.assertFalse(self.editor.srgb(base))    # existing, converged
        bind.assert_called_once_with(self.editor, base, "WoTDecorations", "JTree0")
        import import_mesh_policy
        self.assertEqual(import_mesh_policy.mesh_skin_texture_assets(self.root),
                         [base, key0])

    def test_bulk_map_skin_pass_converges_existing_assets(self):
        conv = self.root / "conv"
        _png(conv / "tex" / "WOT", "AllGrey")
        _png(conv / "tex" / "WOT", "T32_01")
        _png(conv / "tex" / "Angreal", "M_Fireball")
        _png(conv / "tex" / "Angreal", "I_Fireball")
        try:
            import_map = _import_with_editor(
                "import_map", ["import_map.py", "Arena_01", str(conv), "preview"],
                self.editor)
        except (OSError, RuntimeError) as exc:   # needs the private exports
            self.skipTest(f"import_map.py needs the reference exports: {exc}")
        expected = {
            "/Game/WoT/Textures/WOT/AllGrey": False,
            "/Game/WoT/Textures/WOT/T32_01": True,
            "/Game/WoT/Textures/Angreal/M_Fireball": True,
            "/Game/WoT/Textures/Angreal/I_Fireball": False,
        }
        for path, want in expected.items():
            self.editor.add(path, srgb=not want)      # every one starts wrong
        with mock.patch.object(import_map, "ensure_texture_payload_binding"):
            created = import_map._ml_import_skin_package_textures()
        self.assertEqual(created, 0)
        self.assertEqual(self.editor.imports, [])
        for path, want in expected.items():
            self.assertEqual(self.editor.srgb(path), want, path)

    def test_hud_registry_inputs_are_one_shared_list(self):
        hud = _import_with_editor(
            "import_hud_textures", ["import_hud_textures.py"], self.editor)
        self.assertIs(hud.REGISTRY_ONLY_TEXTURES,
                      hud_texture_manifest.REGISTRY_ONLY_TEXTURES)
        claims = {item.asset_path: item.family
                  for item in hud.texture_sampling_claims()}
        for _, path in hud_texture_manifest.REGISTRY_ONLY_TEXTURES:
            self.assertEqual(claims[path], UI)
            self.assertIn(path.casefold(),
                          hud_texture_manifest.REGISTRY_ONLY_ASSET_PATHS)
        self.assertEqual(claims["/Game/WoT/UI/Crosshair1"], UI)


# --------------------------------------------------------------------------
# The AST guard.

PIPELINE_ROOTS = (TOOLS_DIR, REPO / "Art")
POLICY_FILE = IMPORT_DIR / "texture_sampling.py"
# Narrowly classified import-task sites that never import a texture.
NON_TEXTURE_IMPORT_SITES = {
    "Tools/import/import_sounds.py": "WAV sound import (SoundWave factory)",
    "Art/hands/tools/ue_import_dump.py":
        "glTF skeleton dump into a scratch destination; saves nothing",
}
# Metadata-only texture loads (no sampling decision is made there).
METADATA_LOAD_SITES = {
    "Tools/import/payload_binding.py":
        "attaches payload metadata to a texture its caller just acquired",
}
# Whole-registry Texture2D scans.
REGISTRY_SCAN_SITES = {
    "Tools/import/texture_sampling.py": "the verification scan",
    "Tools/import/strip_payload_textures.py":
        "stripped payload finalizer (replace_texture_source keeps sampling)",
}
TEXTURE_ROOTS = ("/Game/WoT/Textures", "/Game/WoT/Maps/Lightmaps")
TEXTURE_PATH_NAME = re.compile(r"(^|_)tex(ture)?_path$", re.IGNORECASE)
# texture acquisition entry point -> positional index of its family argument
POLICY_CALLS = {
    "import_texture": 3,
    "load_texture": 2,
    "ensure_texture_sampling": 2,
    "verify_texture_sampling": 2,
}
OWNING_CALLS = {"import_texture", "load_texture", "import_stripped_texture"}
FAMILY_NAMES = {"WORLD", "EFFECT", "UI", "LIGHTMAP", "LIGHTMAP_RESPONSE"}
# Wrappers that forward their own ``family`` parameter to the policy.
FORWARDING_WRAPPERS = {("Tools/import/stripped_texture_import.py",
                        "import_stripped_texture")}


def _pipeline_files():
    files = []
    for root in PIPELINE_ROOTS:
        files.extend(path for path in root.rglob("*.py")
                     if not path.name.startswith("test_")
                     and "__pycache__" not in path.parts)
    return sorted(files)


def _relative(path):
    return path.relative_to(REPO).as_posix()


class _Module:
    """Parsed source plus the texture_sampling names it imports."""

    def __init__(self, path):
        self.path = path
        self.relative = _relative(path)
        self.tree = ast.parse(path.read_text(encoding="utf-8-sig"), str(path))
        self.policy_aliases = {}       # local name -> policy function
        self.family_aliases = set()    # local names bound to family constants
        self.module_aliases = set()    # local names bound to the module
        for node in ast.walk(self.tree):
            if isinstance(node, ast.ImportFrom) and node.module == "texture_sampling":
                for alias in node.names:
                    local = alias.asname or alias.name
                    if alias.name in POLICY_CALLS:
                        self.policy_aliases[local] = alias.name
                    if alias.name in FAMILY_NAMES:
                        self.family_aliases.add(local)
            elif isinstance(node, ast.ImportFrom) and \
                    node.module == "stripped_texture_import":
                for alias in node.names:
                    if alias.name == "import_stripped_texture":
                        self.policy_aliases[alias.asname or alias.name] = \
                            "import_stripped_texture"
            elif isinstance(node, ast.Import):
                for alias in node.names:
                    if alias.name == "texture_sampling":
                        self.module_aliases.add(alias.asname or alias.name)

    def policy_function(self, call):
        func = call.func
        if isinstance(func, ast.Name):
            return self.policy_aliases.get(func.id)
        if isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name) \
                and func.value.id in self.module_aliases \
                and func.attr in POLICY_CALLS:
            return func.attr
        return None

    def explicit_family(self, node):
        if isinstance(node, ast.Constant):
            return node.value in ts.FAMILIES
        if isinstance(node, ast.Name):
            return node.id in self.family_aliases
        if isinstance(node, ast.Attribute):
            return (isinstance(node.value, ast.Name)
                    and node.value.id in self.module_aliases
                    and node.attr in FAMILY_NAMES)
        if isinstance(node, ast.IfExp):
            return self.explicit_family(node.body) and \
                self.explicit_family(node.orelse)
        return False

    def may_be_lightmap(self, node):
        names = {item.id for item in ast.walk(node) if isinstance(item, ast.Name)}
        attrs = {item.attr for item in ast.walk(node)
                 if isinstance(item, ast.Attribute)}
        consts = {item.value for item in ast.walk(node)
                  if isinstance(item, ast.Constant)}
        return "LIGHTMAP" in names | attrs or LIGHTMAP in consts


def _functions_with_scopes(tree):
    """(function node or None for module scope, statements) pairs."""
    yield None, [node for node in tree.body
                 if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                                          ast.ClassDef))]
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            yield node, node.body


def _assigned_names(target):
    """Names a target binds (subscript/attribute targets bind none)."""
    if isinstance(target, ast.Name):
        return {target.id}
    if isinstance(target, (ast.Tuple, ast.List)):
        return set().union(*(_assigned_names(item) for item in target.elts))
    if isinstance(target, ast.Starred):
        return _assigned_names(target.value)
    return set()


def _tainted(expr, names):
    """Does ``expr`` build a texture-root asset path (or name a known one)?

    Follows string building only (literals, f-strings, concatenation,
    conditionals); a call's result is not the path it was given."""
    if isinstance(expr, ast.Constant):
        return isinstance(expr.value, str) and any(
            root in expr.value for root in TEXTURE_ROOTS)
    if isinstance(expr, ast.Name):
        return expr.id in names or bool(TEXTURE_PATH_NAME.search(expr.id))
    if isinstance(expr, ast.Attribute):
        return bool(TEXTURE_PATH_NAME.search(expr.attr))
    if isinstance(expr, ast.JoinedStr):
        return any(_tainted(item, names) for item in expr.values)
    if isinstance(expr, ast.FormattedValue):
        return _tainted(expr.value, names)
    if isinstance(expr, ast.BinOp):
        return _tainted(expr.left, names) or _tainted(expr.right, names)
    if isinstance(expr, ast.IfExp):
        return _tainted(expr.body, names) or _tainted(expr.orelse, names)
    if isinstance(expr, ast.BoolOp):
        return any(_tainted(item, names) for item in expr.values)
    return False


def _collection_tainted(expr, names):
    """A path, or a literal collection/comprehension of paths (iterating it
    binds texture paths)."""
    if _tainted(expr, names):
        return True
    if isinstance(expr, (ast.List, ast.Tuple, ast.Set)):
        return any(_collection_tainted(item, names) for item in expr.elts)
    if isinstance(expr, (ast.ListComp, ast.SetComp, ast.GeneratorExp)):
        return _collection_tainted(expr.elt, names)
    if isinstance(expr, ast.Dict):
        return any(_collection_tainted(item, names)
                   for item in expr.values if item is not None)
    if isinstance(expr, ast.BinOp):
        return _collection_tainted(expr.left, names) or \
            _collection_tainted(expr.right, names)
    if isinstance(expr, ast.Call) and isinstance(expr.func, ast.Name) \
            and expr.func.id in ("sorted", "list", "tuple", "set", "reversed"):
        return any(_collection_tainted(item, names) for item in expr.args)
    return False


def _scope_taint(statements, inherited):
    """Names bound to a texture path anywhere in the scope (a flow-insensitive
    fixpoint: over-approximating is the safe direction for a guard)."""
    assignments = []
    for statement in statements:
        for node in ast.walk(statement):
            if isinstance(node, ast.Assign):
                assignments.append((node.targets, node.value))
            elif isinstance(node, (ast.AnnAssign, ast.AugAssign)) \
                    and node.value is not None:
                assignments.append(([node.target], node.value))
            elif isinstance(node, (ast.For, ast.AsyncFor, ast.comprehension)):
                assignments.append(([node.target], node.iter))
    tainted = set(inherited)
    changed = True
    while changed:
        changed = False
        for targets, value in assignments:
            if _collection_tainted(value, tainted):
                for target in targets:
                    new = _assigned_names(target) - tainted
                    if new:
                        tainted |= new
                        changed = True
    return tainted


def _load_path_argument(call):
    func = call.func
    name = func.attr if isinstance(func, ast.Attribute) else (
        func.id if isinstance(func, ast.Name) else None)
    if name == "load_asset" and call.args:
        return call.args[0]
    if name == "load_object" and len(call.args) > 1:
        return call.args[1]
    return None


def _calls(statements):
    for statement in statements:
        for node in ast.walk(statement):
            if isinstance(node, ast.Call):
                yield node


class AstGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.modules = [_Module(path) for path in _pipeline_files()]

    def test_import_tasks_exist_only_in_the_policy_module(self):
        offenders = []
        seen_allowed = set()
        for module in self.modules:
            if module.path == POLICY_FILE:
                continue
            for node in ast.walk(module.tree):
                if isinstance(node, ast.Call) and (
                        (isinstance(node.func, ast.Attribute)
                         and node.func.attr == "AssetImportTask")
                        or (isinstance(node.func, ast.Name)
                            and node.func.id == "AssetImportTask")):
                    if module.relative in NON_TEXTURE_IMPORT_SITES:
                        seen_allowed.add(module.relative)
                    else:
                        offenders.append(f"{module.relative}:{node.lineno}")
        self.assertEqual(offenders, [], "texture imports must use "
                         "texture_sampling.import_texture with a family")
        self.assertEqual(seen_allowed, set(NON_TEXTURE_IMPORT_SITES),
                         "stale non-texture import-site classification")

    def test_texture_path_loads_go_through_the_policy(self):
        offenders = []
        for module in self.modules:
            if module.path == POLICY_FILE or module.relative in METADATA_LOAD_SITES:
                continue
            module_scope = None
            for function, statements in _functions_with_scopes(module.tree):
                if function is None:
                    module_scope = _scope_taint(statements, set())
                    names = module_scope
                else:
                    assigned = set()
                    for node in ast.walk(function):
                        if isinstance(node, (ast.Assign, ast.AnnAssign,
                                             ast.AugAssign)):
                            targets = node.targets if isinstance(node, ast.Assign) \
                                else [node.target]
                            for target in targets:
                                assigned |= _assigned_names(target)
                    inherited = {name for name in (module_scope or set())
                                 if name not in assigned}
                    names = _scope_taint(statements, inherited)
                for call in _calls(statements):
                    argument = _load_path_argument(call)
                    if argument is not None and _tainted(argument, names):
                        offenders.append(f"{module.relative}:{call.lineno}")
        self.assertEqual(sorted(set(offenders)), [],
                         "load textures with texture_sampling.load_texture or "
                         "verify_texture_sampling and an explicit family")

    def test_every_policy_call_names_an_explicit_family(self):
        offenders = []
        for module in self.modules:
            if module.path == POLICY_FILE:
                continue
            for function, statements in _functions_with_scopes(module.tree):
                forwarding = function is not None and \
                    (module.relative, function.name) in FORWARDING_WRAPPERS
                for call in _calls(statements):
                    policy = module.policy_function(call)
                    if policy is None:
                        continue
                    keywords = {item.arg: item.value for item in call.keywords}
                    if policy == "import_stripped_texture":
                        family = keywords.get("family")
                    else:
                        index = POLICY_CALLS[policy]
                        family = keywords.get("family") or (
                            call.args[index] if len(call.args) > index else None)
                    ok = family is not None and (
                        module.explicit_family(family)
                        or (forwarding and isinstance(family, ast.Name)
                            and family.id == "family"))
                    if ok and family is not None and module.may_be_lightmap(family) \
                            and "gamma_space" not in keywords:
                        ok = False   # the lightmap rule needs GAMMA_SPACE
                    if not ok:
                        offenders.append(
                            f"{module.relative}:{call.lineno} {policy}")
        self.assertEqual(offenders, [])

    def test_texture_owners_declare_their_claims(self):
        providers = converge_texture_sampling.CLAIM_PROVIDERS
        owners = set()
        for module in self.modules:
            if module.path == POLICY_FILE or module.relative.endswith(
                    "stripped_texture_import.py"):
                continue
            for node in ast.walk(module.tree):
                if isinstance(node, ast.Call) and \
                        module.policy_function(node) in OWNING_CALLS:
                    owners.add(module.path.stem)
        self.assertTrue(owners)
        self.assertEqual(sorted(owners - set(providers)), [],
                         "register new texture owners in "
                         "converge_texture_sampling.CLAIM_PROVIDERS")
        for importer, provider in providers.items():
            path = IMPORT_DIR / f"{provider}.py"
            tree = ast.parse(path.read_text(encoding="utf-8-sig"))
            names = {node.name for node in tree.body
                     if isinstance(node, ast.FunctionDef)}
            self.assertIn("texture_sampling_claims", names, provider)
            self.assertTrue((IMPORT_DIR / f"{importer}.py").is_file(), importer)

    def test_family_less_paths_are_narrow(self):
        replace_users = set()
        registry_scans = set()
        for module in self.modules:
            source = module.path.read_text(encoding="utf-8-sig")
            for node in ast.walk(module.tree):
                if isinstance(node, ast.Call) and isinstance(
                        node.func, (ast.Name, ast.Attribute)) and (
                        getattr(node.func, "id", None) == "replace_texture_source"
                        or getattr(node.func, "attr", None)
                        == "replace_texture_source"):
                    replace_users.add(module.relative)
            if '"Texture2D")' in source and "TopLevelAssetPath" in source:
                registry_scans.add(module.relative)
        self.assertEqual(replace_users, {"Tools/import/strip_payload_textures.py"})
        self.assertEqual(registry_scans, set(REGISTRY_SCAN_SITES))

    def test_guard_detects_the_audited_defects(self):
        """The guard itself: the pre-fix flame/trap shapes must fail it."""
        flame_before = ast.parse(
            "import unreal\n"
            "from texture_sampling import EFFECT, import_texture\n"
            "def neutral():\n"
            "    t, _ = import_texture(unreal, png, NEUTRAL, WORLD_FAMILY)\n"
            "def statics():\n"
            "    task = unreal.AssetImportTask()\n"
            "def trap(pkg, skin):\n"
            "    tex_path = f'/Game/WoT/Textures/{pkg}/{skin}'\n"
            "    return unreal.load_asset(tex_path)\n")
        module = _Module.__new__(_Module)
        module.path, module.relative, module.tree = Path("x.py"), "x.py", flame_before
        module.policy_aliases = {"import_texture": "import_texture"}
        module.family_aliases = {"EFFECT"}
        module.module_aliases = set()
        calls = list(_calls(flame_before.body[2].body))
        family = calls[0].args[3]
        self.assertFalse(module.explicit_family(family))
        tasks = [node for node in ast.walk(flame_before)
                 if isinstance(node, ast.Call)
                 and getattr(node.func, "attr", None) == "AssetImportTask"]
        self.assertEqual(len(tasks), 1)
        trap = flame_before.body[4]
        names = _scope_taint(trap.body, set())
        loads = [call for call in _calls(trap.body)
                 if _load_path_argument(call) is not None]
        self.assertTrue(_tainted(_load_path_argument(loads[0]), names))


if __name__ == "__main__":
    unittest.main()
