#!/usr/bin/env python3
"""Stdlib checks for failure-atomic stripped mesh imports."""
import unittest

from import_transaction import run_isolated_import_transaction
from import_mesh_policy import should_skip_stripped_curvy


class StrippedMeshTransactionTests(unittest.TestCase):
    def test_stripped_mode_skips_dev_only_curvy_variants(self):
        self.assertTrue(should_skip_stripped_curvy("AMAPearl_Curvy", True))
        self.assertTrue(should_skip_stripped_curvy("absorbvisual_curvy", True))
        self.assertFalse(should_skip_stripped_curvy("AMAPearl_Curvy", False))
        self.assertFalse(should_skip_stripped_curvy("AMAPearl", True))

    def test_failure_after_intermediate_save_leaves_no_asset_directory(self):
        destination = "/Game/Generated/Meshes/Fixture"
        directories = set()
        assets = set()

        def import_then_fail():
            # Models import_glb/assign_materials having already persisted the
            # COLOR_0-bearing partial mesh before capture/rebuild rejects it.
            directories.add(destination)
            assets.add(destination + "/Fixture.Fixture")
            raise RuntimeError("post-save mapping verification failed")

        def delete_directory(path):
            directories.discard(path)
            assets.clear()
            return True

        with self.assertRaisesRegex(
                RuntimeError, "post-save mapping verification failed"):
            run_isolated_import_transaction(
                import_then_fail, destination, directories.__contains__,
                delete_directory)

        self.assertNotIn(destination, directories)
        self.assertEqual(assets, set())

    def test_success_keeps_the_fully_verified_destination(self):
        destination = "/Game/Generated/Meshes/Fixture"
        directories = set()

        def verified_import():
            directories.add(destination)
            return "verified"

        result = run_isolated_import_transaction(
            verified_import, destination, directories.__contains__,
            lambda path: directories.discard(path))

        self.assertEqual(result, "verified")
        self.assertIn(destination, directories)


if __name__ == "__main__":
    unittest.main()
