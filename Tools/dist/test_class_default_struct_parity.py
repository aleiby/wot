#!/usr/bin/env python3
"""Retail WOTDEF3 reflected-member parity with the native reader."""
from __future__ import annotations

import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from Tools.dist import build_payload_cache as cache
from Tools.dist import runtime_map_projection_cache as runtime
from Tools.wot_config import original_dir


REPO = Path(__file__).resolve().parents[2]
PACKAGES = (
    "Angreal", "Core", "Editor", "Engine", "Environment", "Fire",
    "IpDrv", "IpServer", "Legend", "MachinShin", "ParticleSystems",
    "UBrowser", "UWindow", "WinDrv", "WOT", "WoTBase", "WoTBrowser",
    "WoTDecorations", "WoTPawns", "WoTTraps",
)


def _payload(framed: bytes) -> bytes:
    if len(framed) < 12 or framed[:8] != b"WOTOBJ3\0":
        raise AssertionError("class-default object is not framed as WOTOBJ3")
    (header_size,) = struct.unpack_from("<I", framed, 8)
    offset = 12 + header_size
    if offset > len(framed):
        raise AssertionError("class-default object header extends past EOF")
    return framed[offset:]


def _member_projection(document: list[dict]) -> list[dict]:
    """Keep the complete member-bearing property trees, without frame metadata."""
    result = []
    for class_row in document:
        properties = []
        for prop in class_row["properties"]:
            if prop["type"] == 10:
                properties.append(prop)
        if properties:
            result.append({"identity": class_row["identity"],
                           "properties": properties})
    return result


class RetailClassDefaultStructParitySpecs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.system = original_dir(required=False) / "System"
        present = (cls.system.is_dir()
                   and all((cls.system / f"{name}.u").is_file()
                           for name in PACKAGES))
        if not present:
            raise unittest.SkipTest(
                "the private retail reference root is unavailable")

    def test_all_native_members_equal_script_resolver(self):
        runner = REPO / "Binaries" / "Win64" / "WoTRetailReaderTests.exe"
        self.assertTrue(runner.is_file(), "build WoTRetailReaderTests first")
        known_struct_comparisons = 0
        with tempfile.TemporaryDirectory() as temporary:
            dump_root = Path(temporary)
            environment = os.environ.copy()
            environment["WOT_CLASS_DEFAULT_DUMP_ROOT"] = str(dump_root)
            completed = subprocess.run(
                [str(runner), f"-UserDir={dump_root / 'runner'}", "--suite",
                 "WoT.RetailReader.RemainingDomains.RetailPins"],
                cwd=REPO, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False)
            self.assertEqual(completed.returncode, 0, completed.stdout)

            for package_name in PACKAGES:
                package_path = self.system / f"{package_name}.u"
                source_bytes = package_path.read_bytes()
                package = cache.Package(package_path)
                expected_object = cache.encode_cache_v3_class_defaults_object(
                    package, fact_id=f"parity.{package_name.casefold()}",
                    source_path=f"System/{package_name}.u",
                    source_export=package_name, source_bytes=source_bytes)
                expected = runtime.parse_class_defaults_v3(
                    _payload(expected_object))
                actual_path = dump_root / f"{package_name}.bin"
                self.assertTrue(actual_path.is_file(),
                                f"native dump missing for {package_name}")
                actual = runtime.parse_class_defaults_v3(
                    _payload(actual_path.read_bytes()))
                self.assertEqual(_member_projection(actual),
                                 _member_projection(expected), package_name)

                # The legacy hand schemas remain on the world path. Prove the
                # generic class-default reflection emits the same Resource and
                # SoundEvent members wherever retail defaults contain them.
                identity_table = cache._canonical_identity_table(package)
                resolver = cache._ScriptResolver(package_path)
                declaration_package = resolver.package(package.name)
                self.assertIsNotNone(declaration_package)
                for export_index, export, identity, placeholder in \
                        cache._class_exports(package, identity_table):
                    if placeholder:
                        continue
                    tags = cache._read_class_defaults(package, export)["defaults"]
                    by_key = {(tag["name"].casefold(), tag["array_index"]): tag
                              for tag in tags}
                    generic = cache._decoded_class_defaults(
                        package, export, identity_table=identity_table,
                        script_resolver=resolver,
                        class_ref=(declaration_package, export_index))
                    for prop in generic:
                        if (prop["type"] != 10
                                or prop["struct"].casefold()
                                not in ("resource", "soundevent")):
                            continue
                        tag = by_key[(prop["name"].casefold(),
                                      prop["array_index"])]
                        hand = cache._resolved_known_struct_members(
                            package, prop["struct"], bytes(tag["value"]),
                            identity_table=identity_table,
                            context=f"{identity}.{prop['name']}")
                        self.assertEqual(prop["members"], hand,
                                         f"{identity}.{prop['name']}")
                        known_struct_comparisons += 1
        self.assertGreater(known_struct_comparisons, 0)


if __name__ == "__main__":
    unittest.main()
