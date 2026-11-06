import argparse
import hashlib
import importlib
import json
import os
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock


generator = importlib.import_module("Tools.import.generate_bootstrap_map")


def _bootstrap_fixture(package=generator.PACKAGE_NAME,
                       game_mode=generator.GAME_MODE_PATH,
                       extra_package=None, *, bind_game_mode=True,
                       bound_game_mode=None):
    """Build UE 5.8 tables plus a tagged WorldSettings mode property."""
    bound_game_mode = bound_game_mode or game_mode
    names = [
        "/Script/CoreUObject", "/Script/Engine", "/Script/WoT", "Class",
        "Package", "World", "WorldSettings", package,
        game_mode.rsplit(".", 1)[1], bound_game_mode.rsplit(".", 1)[1],
        "None", "DefaultGameMode", "ObjectProperty",
    ]
    if extra_package:
        names.append(extra_package)
    names = list(dict.fromkeys(names))

    def fstring(value):
        raw = value.encode("ascii") + b"\0"
        return struct.pack("<i", len(raw)) + raw

    def fname(value):
        return struct.pack("<ii", names.index(value), 0)

    name_map = b"".join(fstring(name) + b"\0\0\0\0" for name in names)
    imports = []

    def add_import(class_package, class_name, outer, object_name):
        imports.append(
            fname(class_package) + fname(class_name) + struct.pack("<i", outer)
            + fname(object_name) + fname("None") + struct.pack("<I", 0))

    add_import("/Script/CoreUObject", "Package", 0, "/Script/Engine")  # -1
    add_import("/Script/CoreUObject", "Package", 0, "/Script/WoT")  # -2
    add_import("/Script/CoreUObject", "Class", -1, "WorldSettings")  # -3
    # Import indices use -(zero-based index)-1 for their outer chain.
    add_import("/Script/CoreUObject", "Class", -2,
               game_mode.rsplit(".", 1)[1])  # -4, always present
    bound_mode_index = 3
    if bound_game_mode != game_mode:
        add_import("/Script/CoreUObject", "Class", -2,
                   bound_game_mode.rsplit(".", 1)[1])
        bound_mode_index = len(imports) - 1
    if extra_package:
        add_import("/Script/CoreUObject", "Package", 0, extra_package)
    import_map = b"".join(imports)

    tagged_properties = b"\0"  # class serialization-control extensions
    if bind_game_mode:
        tagged_properties += fname("DefaultGameMode")
        tagged_properties += fname("ObjectProperty") + struct.pack("<i", 0)
        tagged_properties += struct.pack("<iB", 4, 0)
        tagged_properties += struct.pack("<i", -bound_mode_index - 1)
    tagged_properties += fname("None")

    custom_versions = b""
    prefix = struct.pack("<Iiiiii", 0x9E2A83C1, -9, 864, 522, 1018, 0)
    prefix += b"\0" * 20 + struct.pack("<i", 0)  # saved hash + patched below
    prefix += struct.pack("<i", 0)  # custom-version count
    fixed_before_tables = fstring(package) + struct.pack("<I", 0x00020000)
    summary_tail_size = (
        8 + 8 + len(fstring("")) + 8 + 8 + 8 + 16 + 4 + 4 + 8 + 4 + 4
        + 8
    )
    name_offset = len(prefix) + len(custom_versions) + len(fixed_before_tables) + summary_tail_size
    import_offset = name_offset + len(name_map)
    export_offset = import_offset + len(import_map)
    export_size = 112
    total_header_size = export_offset + export_size
    summary_tail = struct.pack("<ii", len(names), name_offset)
    summary_tail += struct.pack("<ii", 0, 0)  # soft object paths
    summary_tail += fstring("")  # localization id
    summary_tail += struct.pack("<ii", 0, 0)  # gatherable text
    summary_tail += struct.pack("<ii", 1, export_offset)  # exports
    summary_tail += struct.pack("<ii", len(imports), import_offset)
    summary_tail += struct.pack("<iiii", 0, export_offset, 0, export_offset)
    summary_tail += struct.pack("<i", 0)  # metadata
    summary_tail += struct.pack("<i", total_header_size)
    summary_tail += struct.pack("<ii", 0, 0)  # soft package refs
    summary_tail += struct.pack("<i", 0)  # searchable names
    summary_tail += struct.pack("<i", 0)  # thumbnails
    summary_tail += struct.pack("<ii", 0, 0)  # import type hierarchies
    export = struct.pack("<iiii", -3, 0, 0, 0)
    export += fname("WorldSettings") + struct.pack("<I", 0)
    export += struct.pack(
        "<qq", len(tagged_properties), total_header_size)
    export += struct.pack("<IIII", 0, 0, 0, 0)  # export booleans + inherited
    export += struct.pack("<I", 0)  # package flags
    export += struct.pack("<III", 1, 0, 0)  # editor-game, asset, public hash
    export += struct.pack("<iiiii", -1, 0, 0, 0, 0)
    export += struct.pack("<qq", 0, len(tagged_properties))
    assert len(export) == export_size
    data = bytearray(prefix + custom_versions + fixed_before_tables
                     + summary_tail + name_map + import_map + export
                     + tagged_properties)
    struct.pack_into("<i", data, 44, total_header_size)
    return bytes(data)


def _args(root, *, output=None, allow_project_content=False):
    return argparse.Namespace(
        output_root=output or root / "job" / "BootstrapMap",
        denied_read_open_receipt=root / "evidence" / "denied.json",
        output_identity_receipt=root / "evidence" / "identity.json",
        require_empty_output=True,
        no_replace=True,
        allow_project_content=allow_project_content,
        project_root=root,
        require_disjoint_from=[],
    )


def _write_matching_receipt(output, destination, receipt):
    document = generator.identity_document_for_path(output, destination, False)
    receipt.write_bytes(generator.canonical_bytes(document) + b"\n")


class BootstrapMapGeneratorTests(unittest.TestCase):
    def test_fresh_root_generates_one_canonical_map_and_two_receipts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = _args(root)

            def author(destination):
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(_bootstrap_fixture())

            denied, identity = generator.generate(args, author=author)
            map_path = args.output_root / "WoT/Maps/WoTBootstrap.umap"
            self.assertEqual(map_path.read_bytes(), _bootstrap_fixture())
            self.assertEqual(identity["package"], generator.PACKAGE_NAME)
            self.assertEqual(identity["game_mode"], generator.GAME_MODE_PATH)
            self.assertEqual(identity["relative_path"], "WoT/Maps/WoTBootstrap.umap")
            self.assertFalse(identity["allow_project_content"])
            self.assertFalse(denied["allow_project_content"])
            self.assertEqual(identity["generator_sha256"],
                             denied["generator_sha256"])
            self.assertEqual(denied["violations"], [])
            self.assertEqual(
                identity["sha256"], hashlib.sha256(map_path.read_bytes()).hexdigest())
            self.assertEqual(
                args.output_identity_receipt.read_bytes().rstrip(b"\n"),
                generator.canonical_bytes(identity))
            self.assertEqual(
                args.denied_read_open_receipt.read_bytes().rstrip(b"\n"),
                generator.canonical_bytes(denied))
            generator.verify_bootstrap_map(
                args.output_root, args.output_identity_receipt,
                require_code_only_closure=True)

    def test_closed_generation_refusal_matrix(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cases = []
            cases.append(_args(root, output=root / "Content"))
            cases.append(_args(root, output=root / "original" / "out"))
            cases.append(_args(root, output=root / "devtree" / "out"))
            cases.append(_args(root, output=root / "exports" / "out"))
            cases.append(_args(root, output=root / "generated-cache" / "out"))
            cases.append(_args(root, output=root / "cache" / "out"))
            cases.append(_args(root, output=root / "installers" / "out"))
            cases.append(_args(root, output=root / "cache/world" / "out"))
            cases.append(_args(root, output=root / "fixture.wot-world-v3"))
            for index, args in enumerate(cases):
                with self.subTest(index=index, output=args.output_root):
                    with self.assertRaises(generator.GenerationFailed):
                        generator.generate(args, author=lambda _path: None)

            output = root / "sibling" / "GeneratedContent" / "BootstrapMap"
            for reserved in (output, output / "NeutralMapShells", output.parent):
                args = _args(root / f"overlap-{len(reserved.parts)}", output=output)
                args.require_disjoint_from = [reserved]
                with self.subTest(reserved=reserved):
                    with self.assertRaisesRegex(
                            generator.GenerationFailed, "overlaps"):
                        generator.generate(args, author=lambda _path: None)

            for field in ("denied_read_open_receipt", "output_identity_receipt"):
                args = _args(root / field)
                setattr(args, field, None)
                with self.assertRaises(generator.GenerationFailed):
                    generator.generate(args, author=lambda _path: None)

            for field in ("require_empty_output", "no_replace"):
                args = _args(root / field)
                setattr(args, field, False)
                with self.assertRaises(generator.GenerationFailed):
                    generator.generate(args, author=lambda _path: None)

    def test_project_content_requires_explicit_flag_and_records_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "Content"
            args = _args(root, output=output)
            with self.assertRaises(generator.GenerationFailed):
                generator.generate(args, author=lambda _path: None)
            args.allow_project_content = True

            def author(destination):
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(_bootstrap_fixture())

            denied, identity = generator.generate(args, author=author)
            self.assertTrue(denied["allow_project_content"])
            self.assertTrue(identity["allow_project_content"])

    def test_author_cannot_read_any_denied_input_class(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            forbidden = [
                root / "Content/secret.bin",
                root / "original/secret.bin",
                root / "devtree/secret.bin",
                root / "exports/secret.bin",
                root / "cache/secret.bin",
                root / "cache-v3/secret.bin",
                root / "installers/secret.bin",
                root / "GeneratedContent/secret.bin",
                root / "generated-cache/secret.bin",
                root / "retail-cache/secret.bin",
                root / "objects/fixture.wot-world-v3",
                root / "nested/cache/world/secret.bin",
            ]
            for index, source in enumerate(forbidden):
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(b"retail-or-generated-input")
                args = _args(root / f"case-{index}")
                args.project_root = root

                def author(_destination, source=source):
                    source.read_bytes()

                with self.subTest(source=source):
                    with self.assertRaises(generator.GenerationFailed):
                        generator.generate(args, author=author)

    @unittest.skipIf(os.name == "nt",
        "POSIX-only race: Windows os.open handles carry no"
        " FILE_SHARE_DELETE, so the competing unlink is refused by"
        " the OS while the claim descriptor is open - the race this"
        " guards against cannot occur there")
    def test_canonical_save_race_preserves_competing_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = _args(root)
            competitor = b"competing canonical writer"

            def race_author(destination):
                destination.unlink()
                destination.write_bytes(competitor)

            with self.assertRaisesRegex(
                    generator.GenerationFailed, "competing writer"):
                generator.generate(args, author=race_author)
            destination = args.output_root / generator.RELATIVE_MAP
            self.assertEqual(destination.read_bytes(), competitor)
            self.assertFalse(args.denied_read_open_receipt.exists())
            self.assertFalse(args.output_identity_receipt.exists())

    @unittest.skipIf(os.name == "nt",
        "POSIX-only race: Windows os.open handles carry no"
        " FILE_SHARE_DELETE, so the competing unlink is refused by"
        " the OS while the claim descriptor is open - the race this"
        " guards against cannot occur there")
    def test_receipt_publication_race_preserves_competing_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = _args(root)
            competitor = b"competing identity receipt"
            original = generator._write_claimed_json

            def inject(claim, document):
                if claim.path == args.output_identity_receipt.resolve():
                    claim.path.unlink()
                    claim.path.write_bytes(competitor)
                return original(claim, document)

            def author(destination):
                destination.write_bytes(_bootstrap_fixture())

            with mock.patch.object(
                    generator, "_write_claimed_json", side_effect=inject):
                with self.assertRaisesRegex(
                        generator.GenerationFailed, "competing writer"):
                    generator.generate(args, author=author)
            self.assertEqual(args.output_identity_receipt.read_bytes(), competitor)
            self.assertFalse(args.denied_read_open_receipt.exists())
            self.assertFalse((args.output_root / generator.RELATIVE_MAP).exists())

    def test_no_replace_refuses_existing_root_map_or_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for stale in ("root", "map", "denied", "identity"):
                with self.subTest(stale=stale):
                    case_root = root / stale
                    args = _args(case_root)
                    if stale == "root":
                        args.output_root.mkdir(parents=True)
                    elif stale == "map":
                        destination = args.output_root / "WoT/Maps/WoTBootstrap.umap"
                        destination.parent.mkdir(parents=True)
                        destination.write_bytes(_bootstrap_fixture())
                    else:
                        path = getattr(args, f"{stale}_read_open_receipt", None)
                        if stale == "identity":
                            path = args.output_identity_receipt
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text("stale")
                    with self.assertRaises(generator.GenerationFailed):
                        generator.generate(args, author=lambda _path: None)

    def test_verify_rejects_semantic_negatives_with_matching_receipts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "out"
            destination = output / "WoT/Maps/WoTBootstrap.umap"
            destination.parent.mkdir(parents=True)
            destination.write_bytes(_bootstrap_fixture())
            receipt = root / "identity.json"
            _write_matching_receipt(output, destination, receipt)
            generator.verify_bootstrap_map(
                output, receipt, require_code_only_closure=True)

            mutations = {
                "package": _bootstrap_fixture(package="/Game/WoT/Maps/Wrong"),
                "missing game-mode property": _bootstrap_fixture(
                    bind_game_mode=False),
                "different bound game mode": _bootstrap_fixture(
                    bound_game_mode="/Script/WoT.WrongMode"),
                "closure": _bootstrap_fixture(extra_package="/Engine/EngineMaterials/DefaultMaterial"),
            }
            for label, data in mutations.items():
                with self.subTest(label=label):
                    destination.write_bytes(data)
                    _write_matching_receipt(output, destination, receipt)
                    with self.assertRaises(generator.VerificationFailed):
                        generator.verify_bootstrap_map(
                            output, receipt, require_code_only_closure=True)

    def test_verify_rejects_exactly_one_stale_receipt_case(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "out"
            destination = output / generator.RELATIVE_MAP
            destination.parent.mkdir(parents=True)
            destination.write_bytes(_bootstrap_fixture())
            receipt = root / "identity.json"
            _write_matching_receipt(output, destination, receipt)
            destination.write_bytes(_bootstrap_fixture() + b"x")
            with self.assertRaises(generator.VerificationFailed):
                generator.verify_bootstrap_map(
                    output, receipt, require_code_only_closure=True)

    def test_package_tables_must_be_wholly_inside_total_header_size(self):
        fixture = bytearray(_bootstrap_fixture())
        struct.pack_into("<i", fixture, 44, 128)
        with self.assertRaisesRegex(
                generator.VerificationFailed, "total header size"):
            generator._parse_package(bytes(fixture))

        fixture = _bootstrap_fixture()
        total_header_size = struct.unpack_from("<i", fixture, 44)[0]
        package_fstring_size = 4 + len(generator.PACKAGE_NAME.encode("ascii")) + 1
        first_table_pair = 52 + package_fstring_size + 4
        offset_fields = {
            "name": first_table_pair + 4,
            "export": first_table_pair + 32,
            "import": first_table_pair + 40,
        }
        for label, field_offset in offset_fields.items():
            with self.subTest(table=label):
                malformed = bytearray(fixture)
                struct.pack_into("<i", malformed, field_offset,
                                 total_header_size + 1)
                with self.assertRaisesRegex(
                        generator.VerificationFailed, f"{label} table"):
                    generator._parse_package(bytes(malformed))


if __name__ == "__main__":
    unittest.main()
