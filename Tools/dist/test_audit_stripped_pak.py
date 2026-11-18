#!/usr/bin/env python3
"""Stdlib unit tests for the stripped-pak absence audit."""
from __future__ import annotations

import io
import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audit_stripped_pak as audit


def _needle(data: bytes, key: str = "texture:Pkg__Name") -> audit.Needle:
    return audit.Needle(data, (audit.ProbeSource(
        key, "textures", "p8-mip0", "head"),))


def _v3_fixture(root: Path, front_end_root=None, sidecar=False) -> Path:
    palette = [(index, 255 - index, (index * 17) & 0xff, 255)
               for index in range(256)]
    texture = audit.cache.encode_cache_v3_texture_object(
        {"masked": False, "mips": [(16, 16, bytes(range(256)))]},
        palette, fact_id="synthetic.texture",
        source_path="Synthetic/texture.utx",
        source_export="Synthetic.Texture", source_bytes=b"texture-source")
    sound = audit.cache.encode_cache_v3_sound_object(
        audit.cache._synthetic_wav(bits=8, frames=256),
        fact_id="synthetic.sound", source_path="Synthetic/sound.uax",
        source_export="Synthetic.Sound", source_bytes=b"sound-source")
    mesh_body = b"WOTMSH3\0" + bytes(range(256))
    mesh = audit.cache.encode_cache_v3_object({
        "kind": "mesh", "payload_byte_length": len(mesh_body),
    }, mesh_body)
    vertex_count = 32
    index_count = 64
    bsp_header = struct.pack(
        "<8sHHIIIHH6f", b"WOTPBSP\0", 2, 0, vertex_count,
        index_count, index_count // 3, 0, 1, *([0.0] * 6))
    vertex_stream = (bytes(range(256)) * 5)[:vertex_count * 10 * 4]
    index_stream = bytes(range(256))
    bsp_body = bsp_header + vertex_stream + index_stream
    bsp = audit.cache.encode_cache_v3_object({
        "index_count": index_count, "kind": "bsp",
        "payload_byte_length": len(bsp_body), "section_count": 0,
        "vertex_count": vertex_count,
    }, bsp_body)
    blobs = [
        {"logical_id": "cache/front_end_world/WheelOfTime",
         "schema": "wot-front-end-world-v3",
         "bytes": audit.cache._canonical_json_bytes(
             {"format": "wot-front-end-descriptor-sidecar-v2",
              "descriptor": {"descriptor_root": front_end_root or "4" * 64}}
             if sidecar else {"descriptor_root": front_end_root or "4" * 64}),
         "satisfies": ["synthetic.front_end"]},
        {"logical_id": "cache/bsp/Synthetic", "schema": "wot-bsp-v3",
         "bytes": bsp, "satisfies": ["synthetic.bsp"]},
        {"logical_id": "cache/mesh/Synthetic", "schema": "wot-mesh-v3",
         "bytes": mesh, "satisfies": ["synthetic.mesh"]},
        {"logical_id": "cache/sound/Synthetic", "schema": "wot-sound-v3",
         "bytes": sound, "satisfies": ["synthetic.sound"]},
        {"logical_id": "cache/texture/Synthetic", "schema": "wot-texture-v3",
         "bytes": texture, "satisfies": ["synthetic.texture"]},
    ]
    current = root / "current"
    (current / "blobs").mkdir(parents=True)
    for blob in blobs:
        leaf = hashlib.sha256(blob["logical_id"].encode()).hexdigest() + ".blob"
        (current / "blobs" / leaf).write_bytes(blob["bytes"])
    manifest = audit.cache.cache_v3_manifest_bytes(blobs, build_identity={
        "contract_sha256": "1" * 64,
        "installer_sha256": "2" * 64,
        "production_descriptor_sha256": "3" * 64,
    }, world_descriptors=[], front_end_descriptor_root="4" * 64)
    (current / "manifest.json").write_bytes(manifest)
    return current


class AutomatonTests(unittest.TestCase):
    def test_overlaps_suffixes_and_multiple_outputs(self):
        machine = audit.AhoCorasick([b"he", b"she", b"hers", b"his"])
        self.assertEqual(machine.scan(b"ushers"), [
            (2, 0), (1, 1), (2, 2),
        ])

    def test_match_crosses_stream_chunk_boundary(self):
        pattern = bytes(range(64))
        machine = audit.AhoCorasick([pattern])
        stream = io.BytesIO(b"x" * 29 + pattern + b"tail")
        self.assertEqual(
            audit.scan_stream(stream, machine, chunk_size=31), [(29, 0)])


class ProbeTests(unittest.TestCase):
    def test_low_entropy_and_short_probes_are_skipped(self):
        self.assertEqual(audit._probe_slices(
            b"a" * 4096, "k", "sounds", "wav-pcm-data"), [])
        self.assertEqual(audit._probe_slices(
            bytes(range(63)), "k", "sounds", "wav-pcm-data"), [])
        self.assertEqual(len(audit._probe_slices(
            bytes(range(256)) * 20, "k", "sounds", "wav-pcm-data")), 2)

    def test_planted_needle_detection_and_clean_pass(self):
        pattern = bytes(range(32, 112))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            planted = root / "planted.ucas"
            clean = root / "clean.pak"
            planted.write_bytes(b"before" * 23 + pattern + b"after")
            clean.write_bytes(b"clean-container" * 20)
            reports = audit.scan_files(
                [planted, clean], [_needle(pattern)], root=root, chunk_size=37)
        by_name = {item["path"]: item for item in reports}
        self.assertEqual(len(by_name["planted.ucas"]["matches"]), 1)
        self.assertEqual(
            by_name["planted.ucas"]["matched_keys"]["textures"],
            ["texture:Pkg__Name"])
        self.assertEqual(by_name["clean.pak"]["matches"], [])

    def test_self_test(self):
        self.assertTrue(audit.self_test())

    def test_entry_retail_domain_canaries_are_classified(self):
        cases = {
            "entry-t3d": (
                b"Begin Map\nBegin Level\nBegin Actor Class=LevelInfo "
                b"Name=SyntheticEntry\n",
                "entry_t3d",
            ),
            "entry-bsp-object": (
                b"WOTOBJ3\0" + b"{\"schema\":\"wot-bsp-v3\"}" +
                b"WOTPBSP\0" + bytes(range(64)),
                "entry_bsp",
            ),
            "entry-atlas-pixels": (
                b"WOTOBJ3\0" + b"{\"schema\":\"wot-texture-v3\"}" +
                b"WOTPTEX\0" + bytes(range(64, 128)),
                "entry_atlas_pixels",
            ),
            "entry-atlas-metrics": (
                b"WOTOBJ3\0" + b"{\"schema\":\"wot-font-v3\"}" +
                b"WOTFNT3\0" + bytes(range(128, 192)),
                "entry_atlas_metrics",
            ),
            "entry-menu-audio": (
                audit.cache.encode_cache_v3_sound_object(
                    audit.cache._synthetic_wav(
                        bits=8, frames=3, channels=1),
                    fact_id="synthetic.entry-menu-audio",
                    source_path="Synthetic/menu.u",
                    source_export="sound.MenuMusic",
                    source_bytes=b"synthetic-menu-source",
                ),
                "entry_menu_audio",
            ),
            "entry-generated-world": (
                b"WOTOBJ3\0" + b"{\"schema\":\"wot-world-v3\"}" +
                b"WOTWRL3\0" + bytes(range(32, 96)),
                "entry_generated_world",
            ),
            "entry-descriptor-v1": (
                b"{\"contract_sha256\":\"synthetic\","
                b"\"format\":\"wot-front-end-descriptor-sidecar-v1\"}",
                "entry_descriptor",
            ),
            "entry-descriptor-v2": (
                b"{\"contract_sha256\":\"synthetic\","
                b"\"format\":\"wot-front-end-descriptor-sidecar-v2\"}",
                "entry_descriptor",
            ),
        }
        needles = audit.build_semantic_needles()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths = []
            for name, (payload, _kind) in cases.items():
                path = root / f"{name}.pak"
                path.write_bytes(b"prefix" + payload + b"suffix")
                paths.append(path)
            reports = audit.scan_files(paths, needles, root=root, chunk_size=17)
        by_name = {item["path"]: item for item in reports}
        for name, (_payload, kind) in cases.items():
            with self.subTest(name=name):
                report = by_name[f"{name}.pak"]
                self.assertGreater(report["match_counts"][kind], 0)
                self.assertGreater(len(report["matches"]), 0)

    def test_neutral_front_end_consumers_and_schema_are_allowed(self):
        allowed = b"\n".join((
            b"FWoTFrontEndSceneMaterializer",
            b"FWoTFrontEndFontOwner",
            b"/Game/WoT/Maps/WoTFrontEnd",
            b"/WoTNeutralShells/WoTMapShell01",
            b"wot-front-end-world-v3",
            b"font-v2",
            b"wot-font-v3",
            b"wot-texture-v3",
            b"wot-sound-v3",
        ))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "neutral-consumers.pak"
            path.write_bytes(allowed)
            report = audit.scan_files(
                [path], audit.build_semantic_needles(), root=path.parent,
                chunk_size=13,
            )[0]
        self.assertEqual(report["matches"], [])


class CacheV3Tests(unittest.TestCase):
    def test_current_manifest_authenticates_and_probes_all_required_types(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            current = _v3_fixture(root)
            needles, stats = audit.build_needles(root)
            direct_needles, direct_stats = audit.build_needles(current)
        self.assertEqual(stats, direct_stats)
        self.assertEqual(needles, direct_needles)
        self.assertEqual(stats["cache_version"], 3)
        self.assertTrue(set(audit.V3_REQUIRED_SCHEMAS) <= set(stats["entry_counts"]))
        kinds = {source.kind for needle in needles for source in needle.sources}
        self.assertTrue(set(audit.V3_REQUIRED_SCHEMAS.values()) <= kinds)

    def test_unpublished_building_manifest_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            current = _v3_fixture(root)
            current.rename(root / "building")
            with self.assertRaisesRegex(audit.AuditError, "published current manifest"):
                audit.build_needles(root)

    def test_tampered_current_blob_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generation = _v3_fixture(root)
            blob = next((generation / "blobs").glob("*.blob"))
            blob.write_bytes(blob.read_bytes() + b"tamper")
            with self.assertRaisesRegex(audit.AuditError, "identity mismatch"):
                audit.build_needles(root)

    def test_front_end_root_must_match_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            current = _v3_fixture(root)
            manifest_path = current / "manifest.json"
            manifest = audit.cache._load_json(manifest_path)
            manifest["front_end_descriptor_root"] = "5" * 64
            manifest_path.write_bytes(audit.cache._canonical_json_bytes(manifest))
            with self.assertRaisesRegex(audit.AuditError, "front-end descriptor root"):
                audit.build_needles(root)


class FrontEndSidecarV2Tests(unittest.TestCase):
    # S1 publishes the front-end world as a sidecar-v2 record whose descriptor
    # root lives under "descriptor"; the audit reads either shape.
    def test_sidecar_v2_root_matching_manifest_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _v3_fixture(root, sidecar=True)
            audit.build_needles(root)

    def test_sidecar_v2_root_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _v3_fixture(root, front_end_root="6" * 64, sidecar=True)
            with self.assertRaisesRegex(audit.AuditError, "front-end descriptor root"):
                audit.build_needles(root)


if __name__ == "__main__":
    unittest.main()
