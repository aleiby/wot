"""Pure-Python safety pins for the direct mesh-import master preflight."""

import unittest

import mesh_master_bootstrap


class _MissingEditorAssetLibrary:
    @staticmethod
    def does_asset_exist(_path):
        return False


class _FakeUnreal:
    EditorAssetLibrary = _MissingEditorAssetLibrary


class MeshMasterBootstrapTests(unittest.TestCase):
    def test_module_import_does_not_require_unreal_or_numpy(self):
        self.assertNotIn("unreal", mesh_master_bootstrap.__dict__)
        self.assertNotIn("numpy", mesh_master_bootstrap.__dict__)

    def test_missing_prerequisites_fail_before_any_builder_runs(self):
        with self.assertRaisesRegex(
                RuntimeError, "mesh-master bootstrap prerequisites are missing"):
            mesh_master_bootstrap.ensure_mesh_masters(_FakeUnreal(), gamma_space=True)


if __name__ == "__main__":
    unittest.main()
