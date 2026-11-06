#!/usr/bin/env python3
"""Stdlib verification for the guarded stripped-texture import contract."""
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from import_transaction import run_isolated_import_transaction
from payload_binding import full_mip_count
from stripped_texture_import import (
    STRIPPED_PLACEHOLDER_BGRA,
    STRIPPED_PLACEHOLDER_RGBA,
    ensure_placeholder_png,
    import_stripped_texture,
    importer_positionals,
    png_dimensions,
    prevalidate_stripped_textures,
    solid_placeholder_png,
    stripped_destination,
    stripped_mode_requested,
    stripped_texture_is_current,
    sync_texture_properties,
    validate_png,
)


class _FakeImportData:
    def __init__(self, filename):
        self.filename = filename

    def get_first_filename(self):
        return self.filename


class _FakeTexture:
    def __init__(self, filename=None, width=2, height=2, **properties):
        self.import_data = _FakeImportData(filename) if filename else None
        self.width = width
        self.height = height
        self.properties = properties
        self.set_calls = []

    def blueprint_get_size_x(self):
        return self.width

    def blueprint_get_size_y(self):
        return self.height

    def get_editor_property(self, name):
        if name == "asset_import_data":
            return self.import_data
        return self.properties[name]

    def set_editor_property(self, name, value):
        self.properties[name] = value
        self.set_calls.append((name, value))


class _FakeUnreal:
    Texture2D = _FakeTexture


def _png_scanlines(data):
    offset = 8
    idat = bytearray()
    while offset < len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + size]
        if kind == b"IDAT":
            idat.extend(payload)
        offset += 12 + size
    return zlib.decompress(bytes(idat))


class StrippedTextureImportTests(unittest.TestCase):
    def test_mode_selectors_and_positionals(self):
        self.assertFalse(stripped_mode_requested([], {}))
        self.assertTrue(stripped_mode_requested(["--stripped"], {}))
        self.assertTrue(stripped_mode_requested(["--mode=stripped"], {}))
        self.assertTrue(stripped_mode_requested(
            [], {"WOT_TEXTURE_IMPORT_MODE": "STRIPPED"}))
        self.assertTrue(stripped_mode_requested(
            [], {"WOT_PAYLOAD_MODE": "stripped"}))
        with self.assertRaisesRegex(RuntimeError, "conflicting"):
            stripped_mode_requested(
                ["--mode=full"], {"WOT_PAYLOAD_MODE": "stripped"})
        self.assertEqual(importer_positionals([
            "Arena_01", "/tmp/conv", "preview", "--stripped",
            "--dest", "/Game/Generated/Textures", "--allow-canonical",
        ]), ["Arena_01", "/tmp/conv", "preview"])

    def test_isolated_destination_is_required_and_canonical_is_guarded(self):
        canonical = "/Game/WoT/Textures"
        with self.assertRaisesRegex(RuntimeError, "requires explicit"):
            stripped_destination(["--stripped"], canonical, "texture", {})
        with self.assertRaisesRegex(RuntimeError, "under canonical"):
            stripped_destination([
                "--stripped", "--dest",
                "/Game/WoT/Staging/../Textures/Generated",
            ], canonical, "texture", {})
        self.assertEqual(stripped_destination([
            "--stripped", "--dest=/Game/WoTPayload/Textures",
        ], canonical, "texture", {}), "/Game/WoTPayload/Textures")
        self.assertEqual(stripped_destination([
            "--stripped", "--dest", canonical, "--allow-canonical",
        ], canonical, "texture", {}), canonical)
        with self.assertRaisesRegex(RuntimeError, "conflicts"):
            stripped_destination([
                "--stripped", "--dest=/Game/A",
            ], canonical, "texture", {"WOT_PAYLOAD_CONTENT_ROOT": "/Game/B"})

    def test_placeholder_is_exact_opaque_magenta_and_highly_compressible(self):
        width, height = 7, 3
        data = solid_placeholder_png(width, height)
        self.assertEqual(STRIPPED_PLACEHOLDER_RGBA, (255, 0, 255, 255))
        self.assertEqual(STRIPPED_PLACEHOLDER_BGRA, (255, 0, 255, 255))
        self.assertEqual(
            _png_scanlines(data),
            (b"\0" + bytes(STRIPPED_PLACEHOLDER_RGBA) * width) * height)
        self.assertLess(len(data), width * height * 4 + 80)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "placeholder.png"
            path.write_bytes(data)
            self.assertEqual(png_dimensions(path), (width, height))
            self.assertEqual(validate_png(path), (width, height))

    def test_complete_png_validation_rejects_corrupt_chunk_crc(self):
        data = bytearray(solid_placeholder_png(2, 2))
        data[-1] ^= 1
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "corrupt.png"
            path.write_bytes(data)
            with self.assertRaisesRegex(RuntimeError, "CRC mismatch"):
                validate_png(path)

    def test_placeholder_file_and_texture_match_are_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            first = ensure_placeholder_png(directory, 2, 2)
            first_stat = first.stat()
            second = ensure_placeholder_png(directory, 2, 2)
            self.assertEqual(first, second)
            self.assertEqual(second.stat().st_mtime_ns, first_stat.st_mtime_ns)
            texture = _FakeTexture(str(first), never_stream=True)
            self.assertTrue(stripped_texture_is_current(texture, second, 2, 2))
            self.assertFalse(stripped_texture_is_current(texture, second, 4, 2))

    def test_settings_save_signal_is_empty_for_an_exact_match(self):
        texture = _FakeTexture(
            mip_gen_settings="no-mips", never_stream=True)
        changed = sync_texture_properties(texture, (
            ("mip_gen_settings", "no-mips"), ("never_stream", True)))
        self.assertEqual(changed, set())
        self.assertEqual(texture.set_calls, [])

        changed = sync_texture_properties(texture, (("never_stream", False),))
        self.assertEqual(changed, {"never_stream"})
        self.assertEqual(texture.set_calls, [("never_stream", False)])

    def test_exact_asset_rerun_does_not_reimport_or_save(self):
        with tempfile.TemporaryDirectory() as directory:
            placeholder = ensure_placeholder_png(directory, 2, 2)
            texture = _FakeTexture(str(placeholder), never_stream=True)
            imported = []
            saved = []

            class _EAL:
                @staticmethod
                def does_asset_exist(_path):
                    return True

                @staticmethod
                def save_asset(path):
                    saved.append(path)
                    return True

            fake_unreal = SimpleNamespace(
                Texture2D=_FakeTexture,
                EditorAssetLibrary=_EAL,
                AssetToolsHelpers=SimpleNamespace(
                    get_asset_tools=lambda: SimpleNamespace(
                        import_asset_tasks=lambda tasks: imported.extend(tasks))),
                Paths=SimpleNamespace(project_saved_dir=lambda: directory),
                load_asset=lambda _path: texture,
            )
            spec = {
                "source_png": placeholder,
                "asset_path": "/Game/Generated/Test",
                "package": "WOT",
                "source_name": "Test",
                "width": 2,
                "height": 2,
                "destination_mip_count": 1,
            }
            # The helper uses its policy-named subdirectory. Point the fake
            # import metadata at that exact deterministic source before rerun.
            policy_source = ensure_placeholder_png(
                Path(directory) / "StrippedTexturePlaceholders", 2, 2)
            texture.import_data.filename = str(policy_source)
            with mock.patch(
                    "stripped_texture_import.ensure_texture_payload_binding",
                    return_value=(texture, False)):
                result, changed = import_stripped_texture(fake_unreal, spec)

            self.assertIs(result, texture)
            self.assertFalse(changed)
            self.assertEqual(imported, [])
            self.assertEqual(saved, [])

    def test_batch_prevalidation_precedes_asset_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.png"
            source.write_bytes(solid_placeholder_png(2, 2))
            paths = ["/Game/Generated/Good", "/Game/Generated/WrongClass"]
            assets = {
                paths[0]: _FakeTexture(str(source)),
                paths[1]: object(),
            }
            specs = [{
                "source_png": source,
                "asset_path": path,
                "width": 2,
                "height": 2,
            } for path in paths]
            with self.assertRaisesRegex(RuntimeError, "zero assets were written"):
                prevalidate_stripped_textures(
                    _FakeUnreal, specs, assets.__contains__, assets.__getitem__)

    def test_full_chain_and_no_mip_policies_are_distinct(self):
        self.assertEqual(full_mip_count(256, 128), 9)
        self.assertEqual(full_mip_count(1, 1), 1)
        hud_destination_mips = 1
        self.assertNotEqual(full_mip_count(256, 128), hud_destination_mips)

    def test_failed_asset_phase_removes_the_isolated_destination(self):
        destination = "/Game/Generated/Textures"
        directories = set()

        def import_then_fail():
            directories.add(destination)
            raise RuntimeError("binding save failed")

        with self.assertRaisesRegex(RuntimeError, "binding save failed"):
            run_isolated_import_transaction(
                import_then_fail, destination, directories.__contains__,
                lambda path: not directories.discard(path))
        self.assertNotIn(destination, directories)


if __name__ == "__main__":
    unittest.main()
