"""Engine-free tests for the mover-wave instance generator's idempotence.

Run: PYTHONPATH=Tools/import python3 -m unittest Tools.import.test_bootstrap_mover_wave_instances
"""
import importlib
import sys
import types
import unittest


class _Asset:
    def __init__(self, path, parent=None, switches=None):
        self.path = path
        self.props = {"parent": parent}
        self.switches = dict(switches or {})

    def get_path_name(self):
        return self.path

    def get_editor_property(self, name):
        return self.props[name]

    def set_editor_property(self, name, value):
        self.props[name] = value


def _fake_unreal(assets, saves):
    module = types.ModuleType("unreal")
    module.load_asset = lambda path: assets.get(path)

    class EditorAssetLibrary:
        @staticmethod
        def save_asset(path):
            saves.append(path)
            return True

    class MaterialEditingLibrary:
        @staticmethod
        def get_material_instance_static_switch_parameter_value(instance, name):
            return instance.switches.get(name, False)

        @staticmethod
        def set_material_instance_static_switch_parameter_value(instance, name, value):
            instance.switches[name] = value

    class _Tools:
        @staticmethod
        def create_asset(name, root, _cls, _factory):
            asset = _Asset(f"{root}/{name}")
            assets[asset.path] = asset
            return asset

    class AssetToolsHelpers:
        @staticmethod
        def get_asset_tools():
            return _Tools()

    module.EditorAssetLibrary = EditorAssetLibrary
    module.MaterialEditingLibrary = MaterialEditingLibrary
    module.AssetToolsHelpers = AssetToolsHelpers
    module.MaterialInstanceConstant = object
    module.MaterialInstanceConstantFactoryNew = object
    return module


class MoverWaveInstanceIdempotenceTests(unittest.TestCase):
    def _run(self, assets):
        saves = []
        sys.modules["unreal"] = _fake_unreal(assets, saves)
        try:
            module = importlib.import_module("bootstrap_mover_wave_instances")
            module = importlib.reload(module)
            saved = module.build()
        finally:
            sys.modules.pop("unreal", None)
        return module, saved, saves

    def _parents(self):
        root = "/Game/WoT/Materials"
        return {f"{root}/{name}": _Asset(f"{root}/{name}")
                for name in ("M_WoT_WorldWave", "M_WoT_WorldWaveMasked",
                             "M_WoT_WorldWaveTranslucent", "M_WoT_WorldWaveModulated")}

    def test_matching_instances_are_not_resaved(self):
        assets = self._parents()
        on = {f"UseDelta{index}": True for index in range(6)}
        for instance, parent in (
                ("MI_WoT_MoverWaveRuntime", "M_WoT_WorldWave"),
                ("MI_WoT_MoverWaveRuntimeMasked", "M_WoT_WorldWaveMasked"),
                ("MI_WoT_MoverWaveRuntimeTranslucent", "M_WoT_WorldWaveTranslucent"),
                ("MI_WoT_MoverWaveRuntimeModulated", "M_WoT_WorldWaveModulated")):
            path = f"/Game/WoT/Materials/{instance}"
            assets[path] = _Asset(path, assets[f"/Game/WoT/Materials/{parent}"], on)

        _, saved, saves = self._run(assets)

        self.assertEqual(saved, [])
        self.assertEqual(saves, [])

    def test_missing_instances_are_created_and_saved(self):
        assets = self._parents()

        _, saved, saves = self._run(assets)

        self.assertEqual(len(saved), 4)
        self.assertEqual(saves, saved)
        made = assets["/Game/WoT/Materials/MI_WoT_MoverWaveRuntime"]
        self.assertEqual(made.get_editor_property("parent").get_path_name(),
                         "/Game/WoT/Materials/M_WoT_WorldWave")
        self.assertTrue(all(made.switches[f"UseDelta{index}"] for index in range(6)))

    def test_wrong_parent_or_missing_switch_is_repaired(self):
        assets = self._parents()
        on = {f"UseDelta{index}": True for index in range(6)}
        wrong_parent = "/Game/WoT/Materials/MI_WoT_MoverWaveRuntime"
        assets[wrong_parent] = _Asset(
            wrong_parent, assets["/Game/WoT/Materials/M_WoT_WorldWaveMasked"], on)
        missing_switch = "/Game/WoT/Materials/MI_WoT_MoverWaveRuntimeMasked"
        assets[missing_switch] = _Asset(
            missing_switch, assets["/Game/WoT/Materials/M_WoT_WorldWaveMasked"],
            {**on, "UseDelta5": False})

        _, saved, _ = self._run(assets)

        self.assertIn(wrong_parent, saved)
        self.assertIn(missing_switch, saved)
        self.assertEqual(assets[wrong_parent].get_editor_property("parent").get_path_name(),
                         "/Game/WoT/Materials/M_WoT_WorldWave")
        self.assertTrue(assets[missing_switch].switches["UseDelta5"])


if __name__ == "__main__":
    unittest.main()
