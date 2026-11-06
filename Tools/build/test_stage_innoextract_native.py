import copy
import hashlib
import shutil
import json
import lzma
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_innoextract_native as stage


REPO = Path(__file__).resolve().parents[2]
VENDOR = REPO / "ThirdParty/innoextract/1.9"


class StageInnoextractNativeTests(unittest.TestCase):
    def test_pinned_archive_digest_and_exact_inventory(self):
        archive = VENDOR / "upstream/innoextract-1.9.tar.gz"
        self.assertEqual(stage._sha256_file(archive), stage.PINNED_ARCHIVE_SHA256)
        inventory = stage.archive_inventory(archive)
        self.assertEqual(len(inventory), 150)
        self.assertEqual(len(inventory), len(set(inventory)))
        self.assertTrue(
            all(name == "innoextract-1.9" or name.startswith("innoextract-1.9/")
                for name in inventory)
        )

    def test_checked_in_manifest_verifies_post_patch_sources(self):
        stage.verify_source(
            VENDOR / "upstream/innoextract-1.9.tar.gz",
            stage.PINNED_ARCHIVE_SHA256,
            VENDOR / "patches/0001-wot-library-adapter.patch",
            VENDOR / "vendor-manifest.json",
        )

    def _vendor_copy_without_libraries(self, root: Path) -> Path:
        # The public export carries no LFS, so it has no promoted libraries.
        vendor = root / "ThirdParty/innoextract/1.9"
        shutil.copytree(VENDOR, vendor, ignore=shutil.ignore_patterns("lib"))
        return vendor

    def test_source_verifies_in_a_checkout_without_the_libraries(self):
        with tempfile.TemporaryDirectory() as temporary:
            vendor = self._vendor_copy_without_libraries(Path(temporary))
            self.assertFalse((vendor / "lib").exists())
            stage.verify_source(
                vendor / "upstream/innoextract-1.9.tar.gz",
                stage.PINNED_ARCHIVE_SHA256,
                vendor / "patches/0001-wot-library-adapter.patch",
                vendor / "vendor-manifest.json",
            )

    def test_verify_library_holds_a_placed_library_to_its_pin(self):
        with tempfile.TemporaryDirectory() as temporary:
            vendor = self._vendor_copy_without_libraries(Path(temporary))
            manifest_path = vendor / "vendor-manifest.json"
            manifest = json.loads(manifest_path.read_bytes())
            library = vendor / "lib/Win64/wot_innoextract.lib"
            with self.assertRaisesRegex(stage.VendorError, "library is missing"):
                stage.verify_library("Win64", manifest_path)
            library.parent.mkdir(parents=True)
            library.write_bytes(b"a contributor build")
            with self.assertRaisesRegex(stage.VendorError, "differs from its manifest pin"):
                stage.verify_library("Win64", manifest_path)
            manifest["artifacts"]["libraries"]["Win64"]["sha256"] = (
                hashlib.sha256(b"a contributor build").hexdigest())
            manifest_path.write_bytes(stage._canonical_json(manifest))
            self.assertEqual(
                stage.verify_library("Win64", manifest_path),
                hashlib.sha256(b"a contributor build").hexdigest(),
            )

    def test_patched_source_contains_reviewed_resource_and_global_state_guards(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "source"
            stage.prepare_source(
                VENDOR / "upstream/innoextract-1.9.tar.gz",
                VENDOR / "patches/0001-wot-library-adapter.patch",
                destination,
            )
            source = destination / "innoextract-1.9/src"
            lzma_source = (source / "stream/lzma.cpp").read_text()
            self.assertEqual(lzma_source.count("options.dict_size > MaxWoTDictionarySize"), 2)
            info_source = (source / "setup/info.cpp").read_text()
            self.assertNotIn("warning_suppressor warnings;", info_source)
            self.assertIn("MaxHeaderStringBytes", info_source)
            self.assertIn("preflight_budget", info_source)
            file_source = (source / "setup/file.cpp").read_text()
            self.assertIn("string_item_limit_scope path_limit(4096)", file_source)
            adapter_source = (source / "wot/adapter.cpp").read_text()
            self.assertEqual(adapter_source.count("lock(library_mutex)"), 7)
            self.assertIn("strip_app_destination", adapter_source)
            self.assertIn("wot_innoextract_extraction_cursor", adapter_source)
            self.assertIn("cursor.position > data.file.offset", adapter_source)
            self.assertEqual(
                adapter_source.count(
                    "if(package != NULL) package->cursor.invalidate();"
                ),
                4,
            )

    def test_null_is_allowed_only_for_unbuilt_platform_outputs_and_inputs(self):
        manifest = stage._read_manifest(VENDOR / "vendor-manifest.json")
        stage._validate_null_policy(manifest)
        invalid = copy.deepcopy(manifest)
        invalid["readme"]["sha256"] = None
        with self.assertRaisesRegex(stage.VendorError, "null is forbidden"):
            stage._validate_null_policy(invalid)

    def test_unified_patch_applier_checks_context_and_creates_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "old.txt").write_text("one\ntwo\n", encoding="utf-8")
            patch = root / "change.patch"
            patch.write_text(
                "diff --git a/old.txt b/old.txt\n"
                "--- a/old.txt\n+++ b/old.txt\n"
                "@@ -1,2 +1,2 @@\n one\n-two\n+changed\n"
                "diff --git a/new.txt b/new.txt\n"
                "--- /dev/null\n+++ b/new.txt\n"
                "@@ -0,0 +1 @@\n+new\n",
                encoding="utf-8",
            )
            self.assertEqual(
                stage.apply_unified_patch(root, patch), ["old.txt", "new.txt"]
            )
            self.assertEqual((root / "old.txt").read_text(), "one\nchanged\n")
            self.assertEqual((root / "new.txt").read_text(), "new\n")

    def test_fixture_is_deterministic_and_contains_stored_and_zlib_chunks(self):
        first = stage.generate_inno_fixture()
        self.assertEqual(first, stage.generate_inno_fixture())
        metadata = stage.fixture_metadata(first)
        self.assertEqual(metadata["format"], stage.FIXTURE_FORMAT)
        data_offset = int(metadata["data_offset"])
        self.assertEqual(first[data_offset : data_offset + 4], stage.CHUNK_MAGIC)
        stored = b"stored fixture bytes\n"
        second_offset = data_offset + 4 + len(stored)
        self.assertEqual(first[data_offset + 4 : second_offset], stored)
        self.assertEqual(first[second_offset : second_offset + 4], stage.CHUNK_MAGIC)
        self.assertEqual(
            zlib.decompress(first[second_offset + 4 :]),
            (b"zlib fixture bytes " * 16) + b"\n",
        )

    def test_checked_in_fixture_bytes_reproduce_and_variants_are_explicit(self):
        pristine = REPO / "Tools/dist/fixtures/innoextract-5.5.6-synthetic.bin"
        duplicate = REPO / "Tools/dist/fixtures/innoextract-5.5.6-duplicate-path.bin"
        fixtures = sorted(
            (REPO / "Tools/dist/fixtures").glob("innoextract-5.5.6-*.bin")
        )
        self.assertEqual(len(fixtures), 29)
        self.assertEqual(pristine.read_bytes(), stage.generate_inno_fixture())
        self.assertEqual(
            duplicate.read_bytes(),
            stage.generate_inno_fixture(duplicate_normalized_path=True),
        )
        self.assertIn(b"{app}\\Data\\Stored.txt", duplicate.read_bytes())
        self.assertIn(b"{app}\\data/stored.txt", duplicate.read_bytes())
        for variant, options in stage.FIXTURE_VARIANTS.items():
            fixture = REPO / f"Tools/dist/fixtures/innoextract-5.5.6-{variant}.bin"
            self.assertEqual(fixture.read_bytes(), stage.generate_inno_fixture(**options))

    def test_lzma_dictionary_boundary_and_over_limit_fixtures_are_distinct(self):
        lzma1_boundary = stage.generate_inno_fixture(**stage.FIXTURE_VARIANTS["lzma1-boundary"])
        lzma1_over = stage.generate_inno_fixture(**stage.FIXTURE_VARIANTS["lzma1-over"])
        self.assertNotEqual(lzma1_boundary, lzma1_over)
        self.assertIn(b"\x5d\x00\x00\x00\x04", lzma1_boundary)
        self.assertIn(b"\x5d\x01\x00\x00\x04", lzma1_over)

        lzma2_boundary = stage.generate_inno_fixture(**stage.FIXTURE_VARIANTS["lzma2-boundary"])
        lzma2_over = stage.generate_inno_fixture(**stage.FIXTURE_VARIANTS["lzma2-over"])
        self.assertNotEqual(lzma2_boundary, lzma2_over)
        self.assertIn(stage.CHUNK_MAGIC + bytes((28,)), lzma2_boundary)
        self.assertIn(stage.CHUNK_MAGIC + bytes((29,)), lzma2_over)

        stored_size = len(b"stored fixture bytes\n")
        expected = (b"zlib fixture bytes " * 16) + b"\n"
        for fixture in (lzma1_boundary, lzma1_over, lzma2_boundary, lzma2_over):
            metadata = stage.fixture_metadata(fixture)
            header_offset = int(metadata["header_offset"])
            # stream/block.cpp selects header-block compression from this flag.
            # Shape 2 keeps both header blocks in the known-good stored form.
            self.assertEqual(fixture[header_offset + 64 + 8], 0)

        lzma1_data_offset = int(stage.fixture_metadata(lzma1_boundary)["data_offset"])
        lzma1_chunk_offset = lzma1_data_offset + len(stage.CHUNK_MAGIC) + stored_size
        lzma1_chunk = lzma1_boundary[lzma1_chunk_offset + len(stage.CHUNK_MAGIC):]
        self.assertEqual(lzma1_chunk[:5], b"\x5d\x00\x00\x00\x04")
        self.assertEqual(
            lzma.decompress(
                lzma1_chunk[5:],
                format=lzma.FORMAT_RAW,
                filters=[{
                    "id": lzma.FILTER_LZMA1,
                    "dict_size": 64 * 1024 * 1024,
                    "lc": 3,
                    "lp": 0,
                    "pb": 2,
                }],
            ),
            expected,
        )

        lzma2_data_offset = int(stage.fixture_metadata(lzma2_boundary)["data_offset"])
        lzma2_chunk_offset = lzma2_data_offset + len(stage.CHUNK_MAGIC) + stored_size
        lzma2_chunk = lzma2_boundary[lzma2_chunk_offset + len(stage.CHUNK_MAGIC):]
        self.assertEqual(lzma2_chunk[0], 28)
        self.assertEqual(
            lzma.decompress(
                lzma2_chunk[1:],
                format=lzma.FORMAT_RAW,
                filters=[{
                    "id": lzma.FILTER_LZMA2,
                    "dict_size": 64 * 1024 * 1024,
                }],
            ),
            expected,
        )

    def test_galaxy_parts_share_one_forward_only_outer_chunk(self):
        fixture = stage.generate_inno_fixture(galaxy_multipart=True)
        metadata = stage.fixture_metadata(fixture)
        data_offset = int(metadata["data_offset"])
        first = zlib.compress(b"galaxy part one\n", level=9)
        second = zlib.compress(b"galaxy part two\n", level=9)
        self.assertEqual(
            fixture[data_offset:],
            stage.CHUNK_MAGIC + first + second,
        )

    def test_primary_header_optional_blob_matches_declared_compression(self):
        destinations = (b"{app}\\Data\\Stored.txt", b"{app}\\Data\\Compressed.txt")
        zlib_header = stage._main_header(destinations, compression=1)
        lzma1_header = stage._main_header(destinations, compression=3)
        lzma2_header = stage._main_header(destinations, compression=4)

        # setup/info.cpp consumes a third length-prefixed blob only for Zlib.
        self.assertEqual(len(zlib_header), len(lzma1_header) + 4)
        self.assertEqual(len(lzma1_header), len(lzma2_header))
        compression_differences = [
            index
            for index, values in enumerate(zip(zlib_header, lzma1_header, lzma2_header))
            if len(set(values)) != 1
        ]
        self.assertEqual(len(compression_differences), 1)
        compression_index = compression_differences[0]
        self.assertEqual(
            (zlib_header[compression_index], lzma1_header[compression_index],
             lzma2_header[compression_index]),
            (1, 3, 4),
        )

    def test_promote_accepts_only_fixed_outputs_and_updates_one_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            vendor = root / "vendor"
            stage_root = root / "stage"
            header_bytes = b"adapter-header\n"
            library_bytes = b"static-archive\n"
            (stage_root / "include/wot").mkdir(parents=True)
            (stage_root / "lib").mkdir(parents=True)
            (stage_root / "include/wot/innoextract_adapter.h").write_bytes(header_bytes)
            (stage_root / "lib/libwot_innoextract.a").write_bytes(library_bytes)
            dependencies_root = root / "dependencies"
            include_root = dependencies_root / "include"
            include_root.mkdir(parents=True)
            (include_root / "dependency.h").write_text("#define PINNED 1\n", encoding="ascii")
            resolved_records = {}
            for name in ("boost", "liblzma", "zlib", "bzip2", "iconv"):
                suffix = ".tbd" if name in {"zlib", "bzip2", "iconv"} else ".a"
                library = dependencies_root / f"lib{name}{suffix}"
                library.write_bytes((name + "-library\n").encode("ascii"))
                resolved_records[name] = {
                    "version": f"test-{name}-1",
                    "include_paths": [str(include_root)],
                    "library_paths": [str(library)],
                }
            resolved_path = root / "resolved-dependencies.json"
            resolved_path.write_text(json.dumps({
                "format": stage.RESOLVED_DEPENDENCIES_FORMAT,
                "platform": "Mac",
                "dependencies": resolved_records,
            }), encoding="ascii")
            mac_pins = {
                name: {
                    "version": record["version"],
                    "content_sha256": stage.resolved_dependency_content_hash(record),
                }
                for name, record in resolved_records.items()
            }
            win_pins = {
                name: {"version": None, "content_sha256": None}
                for name in resolved_records
            }
            manifest = {
                "artifacts": {
                    "header": {
                        "path": "include/wot/innoextract_adapter.h",
                        "sha256": hashlib.sha256(header_bytes).hexdigest(),
                    },
                    "libraries": {
                        "Mac": {"path": "lib/Mac/libwot_innoextract.a", "sha256": None},
                        "Win64": {"path": "lib/Win64/wot_innoextract.lib", "sha256": None},
                    },
                },
                "format": stage.MANIFEST_FORMAT,
                "native_inputs": {"Mac": mac_pins, "Win64": win_pins},
            }
            manifest_path = vendor / "vendor-manifest.json"
            manifest_path.parent.mkdir(parents=True)
            manifest_path.write_bytes(stage._canonical_json(manifest))
            header_destination = vendor / "include/wot/innoextract_adapter.h"
            library_destination = vendor / "lib/Mac/libwot_innoextract.a"
            stage.promote(
                platform="Mac",
                stage=stage_root,
                header_destination=header_destination,
                library_destination=library_destination,
                manifest_path=manifest_path,
                resolved_dependencies=resolved_path,
            )
            promoted = json.loads(manifest_path.read_bytes())
            self.assertEqual(
                promoted["artifacts"]["libraries"]["Mac"]["sha256"],
                hashlib.sha256(library_bytes).hexdigest(),
            )
            self.assertIsNone(promoted["artifacts"]["libraries"]["Win64"]["sha256"])
            self.assertEqual(library_destination.read_bytes(), library_bytes)

            # A contributor build places its library without re-pinning it.
            pinned_manifest = manifest_path.read_bytes()
            (stage_root / "lib/libwot_innoextract.a").write_bytes(b"contributor-archive\n")
            stage.promote(
                platform="Mac",
                stage=stage_root,
                header_destination=header_destination,
                library_destination=library_destination,
                manifest_path=manifest_path,
                resolved_dependencies=resolved_path,
                record_library_digest=False,
            )
            self.assertEqual(manifest_path.read_bytes(), pinned_manifest)
            self.assertEqual(library_destination.read_bytes(), b"contributor-archive\n")

    def test_dependency_hashes_bind_tbd_or_library_plus_sorted_include_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tbd = root / "libz.tbd"
            tbd.write_bytes(b"tbd-v1")
            sdk = {"library_paths": [str(tbd)], "include_paths": []}
            self.assertEqual(
                stage.resolved_dependency_content_hash(sdk),
                hashlib.sha256(b"tbd-v1").hexdigest(),
            )

            library = root / "libboost.a"
            includes = root / "include"
            includes.mkdir()
            library.write_bytes(b"archive-v1")
            (includes / "b.hpp").write_bytes(b"B")
            (includes / "a.hpp").write_bytes(b"A")
            homebrew = {
                "library_paths": [str(library)], "include_paths": [str(includes)]
            }
            before = stage.resolved_dependency_content_hash(homebrew)
            (includes / "a.hpp").write_bytes(b"changed")
            self.assertNotEqual(before, stage.resolved_dependency_content_hash(homebrew))

    def test_resolved_dependency_mismatch_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dependencies = {}
            pins = {}
            for name in ("boost", "liblzma", "zlib", "bzip2", "iconv"):
                library = root / f"{name}.tbd"
                library.write_bytes(name.encode("ascii"))
                record = {
                    "version": "1", "library_paths": [str(library)],
                    "include_paths": [],
                }
                dependencies[name] = record
                pins[name] = {
                    "version": "1",
                    "content_sha256": stage.resolved_dependency_content_hash(record),
                }
            resolved = root / "resolved-dependencies.json"
            resolved.write_text(json.dumps({
                "format": stage.RESOLVED_DEPENDENCIES_FORMAT,
                "platform": "Mac", "dependencies": dependencies,
            }), encoding="ascii")
            manifest = {"native_inputs": {"Mac": pins}}
            stage.verify_resolved_dependencies(
                platform="Mac", resolved_path=resolved, manifest=manifest
            )
            (root / "boost.tbd").write_bytes(b"changed")
            with self.assertRaisesRegex(stage.VendorError, "content mismatch"):
                stage.verify_resolved_dependencies(
                    platform="Mac", resolved_path=resolved, manifest=manifest
                )

    def test_versions_only_ignores_dependency_bytes_but_not_versions(self):
        # The contributor build: MSVC and vcpkg libraries are not byte-identical
        # across machines, so only the versions are held to the pins.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            resolved, manifest = self._absent_dependency_case(
                root,
                iconv_pin={"version": "Win32-codepage", "content_sha256": None},
            )
            (root / "boost.lib").write_bytes(b"rebuilt on another machine")
            with self.assertRaisesRegex(stage.VendorError, "content mismatch"):
                stage.verify_resolved_dependencies(
                    platform="Win64", resolved_path=resolved, manifest=manifest
                )
            stage.verify_resolved_dependencies(
                platform="Win64", resolved_path=resolved, manifest=manifest,
                check_content=False,
            )
            manifest["native_inputs"]["Win64"]["boost"]["version"] = "2"
            with self.assertRaisesRegex(stage.VendorError, "version mismatch"):
                stage.verify_resolved_dependencies(
                    platform="Win64", resolved_path=resolved, manifest=manifest,
                    check_content=False,
                )

    def _absent_dependency_case(self, root: Path, *, iconv_pin, iconv_record=None):
        dependencies = {}
        pins = {}
        for name in ("boost", "liblzma", "zlib", "bzip2"):
            library = root / f"{name}.lib"
            library.write_bytes(name.encode("ascii"))
            record = {
                "version": "1", "library_paths": [str(library)], "include_paths": [],
            }
            dependencies[name] = record
            pins[name] = {
                "version": "1",
                "content_sha256": stage.resolved_dependency_content_hash(record),
            }
        dependencies["iconv"] = iconv_record if iconv_record is not None else {
            "version": "Win32-codepage", "library_paths": [], "include_paths": [],
        }
        pins["iconv"] = iconv_pin
        resolved = root / "resolved-dependencies.json"
        resolved.write_text(json.dumps({
            "format": stage.RESOLVED_DEPENDENCIES_FORMAT,
            "platform": "Win64", "dependencies": dependencies,
        }), encoding="ascii")
        return resolved, {"native_inputs": {"Win64": pins}}

    def test_dependency_linking_nothing_verifies_against_a_null_pin(self):
        # Win64 links no iconv: it uses the Win32 codepage API instead.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            resolved, manifest = self._absent_dependency_case(
                root,
                iconv_pin={"version": "Win32-codepage", "content_sha256": None},
            )
            stage.verify_resolved_dependencies(
                platform="Win64", resolved_path=resolved, manifest=manifest
            )

    def test_null_pin_rejects_a_dependency_that_actually_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            library = root / "iconv.lib"
            library.write_bytes(b"iconv")
            resolved, manifest = self._absent_dependency_case(
                root,
                iconv_pin={"version": "Win32-codepage", "content_sha256": None},
                iconv_record={
                    "version": "Win32-codepage",
                    "library_paths": [str(library)],
                    "include_paths": [],
                },
            )
            with self.assertRaisesRegex(stage.VendorError, "content mismatch"):
                stage.verify_resolved_dependencies(
                    platform="Win64", resolved_path=resolved, manifest=manifest
                )

    def test_absent_dependency_rejects_a_pin_that_still_binds_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            resolved, manifest = self._absent_dependency_case(
                root,
                iconv_pin={"version": "Win32-codepage", "content_sha256": "00" * 32},
            )
            with self.assertRaisesRegex(stage.VendorError, "links nothing"):
                stage.verify_resolved_dependencies(
                    platform="Win64", resolved_path=resolved, manifest=manifest
                )


if __name__ == "__main__":
    unittest.main()
