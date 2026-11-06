#!/usr/bin/env python3
"""Headless specs for the split-aware multiplayer BSP payload contract."""
from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
REPO = TOOLS.parent
CONVERT = HERE.parent / "convert"
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(CONVERT))
sys.path.insert(0, str(TOOLS))

import bsp_payload as bsp
import build_payload_cache as cache
import generate_world_property_registry as property_registry
import generate_world_texture_registry as texture_registry
import import_all
from Tools.dist import runtime_map_projection_cache as projection_cache
from gltf import write_glb
from wot_config import original_dir


ORIGINAL = original_dir(required=False)
HAS_RETAIL_WORLDS = any((ORIGINAL / "Maps").glob("*.wot"))
CONVERSION_ROOT = REPO / "Intermediate" / "Conv"
HAS_WORLD_TEXTURE_CENSUS = (
    HAS_RETAIL_WORLDS
    and any((CONVERSION_ROOT / "tex").glob("*/_manifest.json"))
)


class WorldTextureRegistrySpecs(unittest.TestCase):
    def test_runtime_world_scope_adds_front_end_without_expanding_mp_maps(self):
        self.assertEqual(len(import_all.MP_MAPS), 19)
        self.assertEqual(
            texture_registry.RUNTIME_WORLD_MAPS,
            (*import_all.MP_MAPS, "WheelOfTime"),
        )

    def test_collect_preserves_resolve_tex_facts_without_redecoding(self):
        canonical = ("AesSedai_T", "MarbRed1910_T", 128, 64, 1.0)
        package = mock.Mock()
        package.exports = [{"cls": -1, "size": 123}]
        package.objname.return_value = "Model"
        package.objref.return_value = ("AesSedai_T", "MarbRed1910_T")
        package.objpath.return_value = "AesSedai_T.MarbRed1910_T"
        sizes = {("aessedai_t", "marbred1910_t"): canonical}
        with mock.patch.object(texture_registry, "MP_MAPS", ("Arena_03",)), \
                mock.patch.object(texture_registry, "Package",
                                  return_value=package), \
                mock.patch.object(texture_registry, "decode_model",
                                  return_value={"surfs": [{"tex": -1}]}), \
                mock.patch.object(texture_registry, "load_tex_sizes",
                                  return_value=sizes), \
                mock.patch.object(texture_registry, "_texture_frictions",
                                  return_value=({}, {("aessedai_t", "marbred1910_t"): 10.0},
                                                {"aessedai_t"})), \
                mock.patch.object(texture_registry,
                                  "runtime_fractal_texture_keys",
                                  return_value=set()):
            rows = texture_registry.collect(Path("retail"), Path("conversion"))
        self.assertEqual(rows, [
            ("aessedai_t", "marbred1910_t", "AesSedai_T",
             "MarbRed1910_T", 128, 64, 1.0, 10.0, False,
             "", "", "", 0, 0.0, "", "", "", 0, 0, 0.0),
        ])

    def test_collect_carries_canonical_overlay_facts_for_indirect_cache_binding(self):
        package = mock.Mock()
        package.exports = [{"cls": -1, "size": 123}]
        package.objname.return_value = "Model"
        package.objref.return_value = ("BaseT", "Stone")
        package.objpath.return_value = "BaseT.Walls.Stone"
        with tempfile.TemporaryDirectory() as temporary:
            conversion = Path(temporary)
            for folder, textures in {
                    "BaseT": {
                        "Stone": {
                            "w": 256, "h": 128, "drawscale": 2.0,
                            "object_path": "BaseT.Walls.Stone",
                            "friction": 1.0,
                            "macro": ["MacroT", "BigStone"],
                            "detail": ["DetailT", "Grain"],
                        },
                    },
                    # A non-square detail keeps its height: the V ratio
                    # divides by it (Glide driver SetTexture/CopyVerts).
                    "DetailT": {
                        "Grain": {
                            "w": 32, "h": 16, "drawscale": 0.5,
                            "object_path": "DetailT.Detail.Grain",
                            "friction": 1.0,
                        },
                    },
                    "MacroT": {
                        "BigStone": {
                            "w": 64, "h": 64, "drawscale": 4.0,
                            "object_path": "MacroT.Macro.BigStone",
                            "friction": 1.0,
                        },
                    },
            }.items():
                manifest = conversion / "tex" / folder / "_manifest.json"
                manifest.parent.mkdir(parents=True)
                manifest.write_text(json.dumps({"textures": textures}),
                                    encoding="utf-8")
            with mock.patch.object(texture_registry, "RUNTIME_WORLD_MAPS",
                                   ("Arena_03",)), \
                    mock.patch.object(texture_registry, "Package",
                                      return_value=package), \
                    mock.patch.object(texture_registry, "decode_model",
                                      return_value={"surfs": [{"tex": -1}]}), \
                    mock.patch.object(texture_registry,
                                      "runtime_fractal_texture_keys",
                                      return_value=set()):
                rows = texture_registry.collect(Path("retail"), conversion)
        self.assertEqual(rows, [
            ("baset", "walls.stone", "BaseT", "Stone", 256, 128, 2.0,
             1.0, False, "MacroT", "BigStone",
             "MacroT.Macro.BigStone", 64, 4.0, "DetailT", "Grain",
             "DetailT.Detail.Grain", 32, 16, 0.5),
        ])

    def test_collect_uses_resolve_tex_relative_before_leaf(self):
        package = mock.Mock()
        package.exports = [{"cls": -1, "size": 123}]
        package.objname.return_value = "Model"
        package.objref.return_value = ("WorldT", "Shared")
        package.objpath.return_value = "WorldT.Group.Shared"
        sizes = {
            ("worldt", "group.shared"): ("WorldT", "Grouped", 64, 64, 2.0),
            ("worldt", "shared"): ("WorldT", "Flat", 128, 128, 1.0),
        }
        with mock.patch.object(texture_registry, "MP_MAPS", ("Arena_03",)), \
                mock.patch.object(texture_registry, "Package",
                                  return_value=package), \
                mock.patch.object(texture_registry, "decode_model",
                                  return_value={"surfs": [{"tex": -1}]}), \
                mock.patch.object(texture_registry, "load_tex_sizes",
                                  return_value=sizes), \
                mock.patch.object(texture_registry, "_texture_frictions",
                                  return_value=({}, {}, {"worldt"})), \
                mock.patch.object(texture_registry,
                                  "runtime_fractal_texture_keys",
                                  return_value=set()):
            rows = texture_registry.collect(Path("retail"), Path("conversion"))
        self.assertEqual(rows, [
            ("worldt", "group.shared", "WorldT", "Grouped",
             64, 64, 2.0, 1.0, False, "", "", "", 0, 0.0,
             "", "", "", 0, 0, 0.0),
        ])

    def test_grouped_source_key_stays_exact_when_resolver_falls_back_to_leaf(self):
        canonical = ("WorldT", "Flat", 128, 128, 1.0)
        package = mock.Mock()
        package.exports = [{"cls": -1, "size": 123}]
        package.objname.return_value = "Model"
        package.objref.return_value = ("WorldT", "Shared")
        package.objpath.return_value = "WorldT.Missing.Shared"
        sizes = {("worldt", "shared"): canonical}
        with mock.patch.object(texture_registry, "MP_MAPS", ("Arena_03",)), \
                mock.patch.object(texture_registry, "Package",
                                  return_value=package), \
                mock.patch.object(texture_registry, "decode_model",
                                  return_value={"surfs": [{"tex": -1}]}), \
                mock.patch.object(texture_registry, "load_tex_sizes",
                                  return_value=sizes), \
                mock.patch.object(texture_registry, "_texture_frictions",
                                  return_value=({}, {}, {"worldt"})), \
                mock.patch.object(texture_registry,
                                  "runtime_fractal_texture_keys",
                                  return_value=set()):
            rows = texture_registry.collect(Path("retail"), Path("conversion"))
        self.assertEqual(rows[0][:2], ("worldt", "missing.shared"))

    def test_folded_lookup_collision_fails_loudly(self):
        used = {}
        texture_registry._record_lookup(
            used, ("worldt", "group.shared"),
            ("WorldT", "First", 64, 64, 2.0), "Arena_01:first")
        with self.assertRaisesRegex(
                ValueError, "ambiguous folded world texture lookup"):
            texture_registry._record_lookup(
                used, ("worldt", "group.shared"),
                ("WorldT", "Second", 128, 128, 1.0), "Arena_02:second")

    def test_generated_rows_reject_duplicate_folded_keys(self):
        rows = [
            ("worldt", "shared", "WorldT", "First", 64, 64, 1.0, False),
            ("worldt", "shared", "WorldT", "First", 64, 64, 1.0, False),
        ]
        with self.assertRaisesRegex(
                ValueError, "duplicate folded world texture lookup"):
            texture_registry._assert_unique_lookup_rows(rows)

    def test_collect_includes_mover_model_surfaces_and_exact_friction(self):
        package = mock.Mock()
        package.exports = [
            {"cls": -1, "size": 200}, {"cls": -1, "size": 50}]
        package.objname.return_value = "Model"
        package.objref.side_effect = lambda reference: {
            -1: ("WorldT", "Floor"), -2: ("WorldT", "Door")}[reference]
        package.objpath.side_effect = lambda reference: {
            -1: "WorldT.Floor", -2: "WorldT.Detail.Door"}[reference]
        sizes = {
            ("worldt", "floor"): ("WorldT", "Floor", 64, 64, 1.0),
            ("worldt", "detail.door"): ("WorldT", "Door", 128, 256, 2.0),
        }
        models = iter(({"surfs": [{"tex": -1}]}, {"surfs": [{"tex": -2}]}))
        with mock.patch.object(texture_registry, "RUNTIME_WORLD_MAPS", ("Arena_03",)), \
                mock.patch.object(texture_registry, "Package", return_value=package), \
                mock.patch.object(texture_registry, "decode_model",
                                  side_effect=lambda *_args: next(models)), \
                mock.patch.object(texture_registry, "load_tex_sizes", return_value=sizes), \
                mock.patch.object(texture_registry, "_texture_frictions",
                                  return_value=({"worldt.detail.door": 0.25},
                                                {("worldt", "floor"): 10.0},
                                                {"worldt"})), \
                mock.patch.object(texture_registry,
                                  "runtime_fractal_texture_keys",
                                  return_value=set()):
            rows = texture_registry.collect(Path("retail"), Path("conversion"))
        self.assertEqual([row[1] for row in rows], ["detail.door", "floor"])
        self.assertEqual({row[1]: row[7] for row in rows}, {
            "detail.door": 0.25, "floor": 10.0})

    def test_surface_friction_rejects_a_missing_source_manifest(self):
        with self.assertRaisesRegex(FileNotFoundError,
                                    "texture package 'MissingT'"):
            texture_registry._surface_friction(
                "MissingT", "Group.Floor", "Floor", ({}, {}, {"worldt"}))

    @unittest.skipUnless(
        HAS_WORLD_TEXTURE_CENSUS,
        "retail maps or generated texture manifests are absent",
    )
    def test_checked_in_world_texture_source_covers_every_collected_lookup(self):
        self.maxDiff = None
        rows = texture_registry.collect(ORIGINAL, CONVERSION_ROOT)
        header = (REPO /
            "Source/WoTRetailReader/Public/WoTRetailReader/Generated/"
            "WorldTextures.generated.h").read_text(encoding="utf-8")
        missing = [row for row in rows
                   if f'TEXT("{row[0]}"), TEXT("{row[1]}")' not in header]
        self.assertEqual([(row[0], row[1], row[2], row[3], row[4], row[5],
                           row[6], row[8]) for row in missing], [])
        nondefault = [(row[0], row[1], row[7]) for row in rows
                      if row[7] != 1.0]
        self.assertEqual(nondefault, [
            ("developert", "special.typemask_005clm", 10.0),
            ("scottt", "climb", 10.0),
            ("trapst", "pittop01", 0.0),
            ("trapst", "pitwall01", 10.0),
        ])
        macros = texture_registry._texture_macros(
            CONVERSION_ROOT / "tex",
            texture_registry.load_tex_sizes(CONVERSION_ROOT / "tex"))
        self.assertEqual(len(macros), 49)
        self.assertEqual(len({value[2].casefold() for value in macros.values()}), 10)
        macro_rows = [row for row in rows if row[10]]
        self.assertEqual(len(macro_rows), 15)
        self.assertEqual(len({row[11].casefold() for row in macro_rows}), 5)
        self.assertTrue(all(row[9] and row[11] and row[12] > 0 and row[13] > 0
                            for row in macro_rows))
        # UE1 DetailTexture: 637 runtime world textures over ten targets, 73
        # of them non-square (the per-axis ratio's reason to exist).
        detail_rows = [row for row in rows if row[15]]
        self.assertEqual(len(detail_rows), 637)
        self.assertEqual(len({row[16].casefold() for row in detail_rows}), 10)
        self.assertTrue(all(row[14] and row[16] and row[17] > 0
                            and row[18] > 0 and row[19] > 0
                            for row in detail_rows))
        self.assertEqual(sum(1 for row in detail_rows if row[4] != row[5]), 73)


def _write_fixture(path: Path, materials=("Stone",), scale=1.0):
    groups = {}
    for index, material in enumerate(materials):
        x = float(index) * 2.0
        groups[material] = (
            [x, 0.0, 0.0, x + scale, 0.0, 0.0, x, scale, 0.0],
            [0.0, 0.0, 1.0] * 3,
            [0.0, 0.0, 1.0, 0.0, 0.0, 1.0],
            [0, 1, 2],
            [0.0, 0.0, 0.5, 0.0, 0.0, 0.5],
        )
    write_glb(path, path.stem, groups)


class BSPPayloadSpecs(unittest.TestCase):
    def test_canonical_parser_accepts_v1_through_v4_and_decodes_v4_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "Arena_07.glb"
            _write_fixture(source)
            version1 = bsp.encode_bsp_payload(bsp.decode_bsp_glb(source, "main"))

        lightmap = struct.pack("<III4B", 1, 1, 4, 0, 0, 0, 255)
        version2 = bytearray(version1 + lightmap)
        struct.pack_into("<H", version2, 8, 2)

        trailer = bytearray(struct.pack("<6I", 56, 1, 1, 0, 0, 0))
        trailer += struct.pack("<I", 0)
        trailer += struct.pack("<IIIIIff", 0, 0, 0, 0, 0, 0.0, 0.0)
        version3 = bytearray(version1 + lightmap + trailer)
        struct.pack_into("<H", version3, 8, 3)

        geometry = {
            "role": "main",
            "positions": [0.0, 0.0, 0.0, 100.0, 0.0, 0.0, 0.0, 100.0, 0.0],
            "normals": [0.0, 0.0, 1.0] * 3,
            "uv0": [0.0, 0.0, 1.0, 0.0, 0.0, 1.0],
            "uv1": [0.0, 0.0, 0.5, 0.0, 0.0, 0.5],
            "indices": [0, 1, 2],
            "bounds_min": [0.0, 0.0, 0.0],
            "bounds_max": [100.0, 100.0, 0.0],
            "sections": [{
                "material_slot": "WorldT__Stone_M", "first_index": 0,
                "index_count": 3, "min_vertex": 0, "max_vertex": 2,
                "collision_enabled": True, "canonical_package": "WorldT",
                "canonical_name": "Stone",
                "source_texture_identity": "WorldT.Detail.Stone",
                "surface_flags": 0x202,
                "pan_zone_actor_identity": "Arena_07.ZoneInfo2",
            }],
        }
        version4 = projection_cache.encode_bsp_v4(geometry)
        for expected, payload in enumerate((version1, version2, version3, version4), 1):
            with self.subTest(version=expected):
                parsed = bsp.parse_bsp_payload(payload)
                self.assertEqual(parsed["version"], expected)
        self.assertEqual(bsp.parse_bsp_payload(version4)["sections"][0],
                         geometry["sections"][0])
        null_identity = dict(geometry)
        null_identity["sections"] = [dict(geometry["sections"][0])]
        null_identity["sections"][0].update({
            "material_slot": "__None",
            "canonical_package": "", "canonical_name": "None",
            "source_texture_identity": "None", "surface_flags": 0,
            "pan_zone_actor_identity": "",
        })
        self.assertEqual(
            bsp.parse_bsp_payload(
                projection_cache.encode_bsp_v4(null_identity))["sections"][0],
            null_identity["sections"][0])

        empty_null = dict(geometry)
        empty_null["sections"] = [dict(geometry["sections"][0])]
        empty_null["sections"][0].update({
            "material_slot": "__None",
            "canonical_package": "", "canonical_name": "",
            "source_texture_identity": "", "surface_flags": 0,
            "pan_zone_actor_identity": "",
        })
        hostile = bytearray(projection_cache.encode_bsp_v4(empty_null))
        slot_offset = projection_cache._BSP_HEADER.size \
            + projection_cache._BSP_SECTION.size
        hostile[slot_offset:slot_offset + len("__None")] = b"Stone0"
        with self.assertRaisesRegex(
                bsp.BSPPayloadError, "incomplete canonical material identity"):
            bsp.parse_bsp_payload(hostile)

    def test_canonical_parser_decodes_version10_macro_and_surface_identity(self):
        geometry = {
            "role": "main",
            "positions": [0.0, 0.0, 0.0, 100.0, 0.0, 0.0,
                          0.0, 100.0, 0.0],
            "normals": [0.0, 0.0, 1.0] * 3,
            "uv0": [0.0, 0.0, 1.0, 0.0, 0.0, 1.0],
            "uv1": [0.0, 0.0, 0.5, 0.0, 0.0, 0.5],
            "indices": [0, 1, 2],
            "bounds_min": [0.0, 0.0, 0.0],
            "bounds_max": [100.0, 100.0, 0.0],
            "sections": [{
                "material_slot": "WorldT__Stone", "first_index": 0,
                "index_count": 3, "min_vertex": 0, "max_vertex": 2,
                "collision_enabled": True, "canonical_package": "WorldT",
                "canonical_name": "Stone",
                "source_texture_identity": "WorldT.Detail.Stone",
                "surface_flags": 0, "pan_zone_actor_identity": "",
            }],
        }
        payload = bytearray(projection_cache.encode_bsp_v4(geometry))
        struct.pack_into("<HH", payload, 8, 10, 66)
        section = geometry["sections"][0]
        identity_end = (projection_cache._BSP_HEADER.size
                        + projection_cache._BSP_SECTION.size
                        + len(section["material_slot"])
                        + projection_cache._BSP_IDENTITY.size
                        + len(section["canonical_package"])
                        + len(section["canonical_name"])
                        + len(section["source_texture_identity"]))
        macro_values = ("MacroT", "BigStone", "MacroT.Macro.BigStone")
        macro = struct.pack("<HHHHfff", *(len(value) for value in macro_values),
                            0, 2.0, 0.125, -0.25) \
            + "".join(macro_values).encode("ascii")
        payload[identity_end:identity_end] = macro
        payload += struct.pack("<IIII", 16, 1, 1, 0)
        parsed = bsp.parse_bsp_payload(payload)
        self.assertEqual(parsed["version"], 10)
        self.assertEqual(parsed["triangle_surfaces"], [0])
        self.assertEqual(parsed["surface_count"], 1)
        self.assertEqual(parsed["sections"][0]["macro_canonical_package"],
                         "MacroT")
        self.assertEqual(parsed["sections"][0]["macro_canonical_name"],
                         "BigStone")
        self.assertEqual(
            parsed["sections"][0]["macro_source_texture_identity"],
            "MacroT.Macro.BigStone")
        self.assertEqual(parsed["sections"][0]["macro_ratio"], 2.0)
        self.assertEqual(parsed["sections"][0]["macro_offset_u"], 0.125)
        self.assertEqual(parsed["sections"][0]["macro_offset_v"], -0.25)

        incomplete = bytearray(payload)
        struct.pack_into("<H", incomplete, identity_end + 2, 0)
        with self.assertRaisesRegex(bsp.BSPPayloadError,
                                    "invalid macro identity"):
            bsp.parse_bsp_payload(incomplete)

        old = bytearray(payload)
        struct.pack_into("<H", old, 8, 9)
        with self.assertRaisesRegex(bsp.BSPPayloadError, "payload header"):
            bsp.parse_bsp_payload(old)

    def test_canonical_parser_decodes_version5_animation_trailer(self):
        """Version 5: the v4 record set plus the animated light-map trailer
        (placements, waveform groups, response terms with sparse rects)."""
        geometry = {
            "role": "main",
            "positions": [0.0, 0.0, 0.0, 100.0, 0.0, 0.0, 0.0, 100.0, 0.0],
            "normals": [0.0, 0.0, 1.0] * 3,
            "uv0": [0.0, 0.0, 1.0, 0.0, 0.0, 1.0],
            "uv1": [0.0, 0.0, 0.5, 0.0, 0.0, 0.5],
            "indices": [0, 1, 2],
            "bounds_min": [0.0, 0.0, 0.0],
            "bounds_max": [100.0, 100.0, 0.0],
            "sections": [{
                "material_slot": "WorldT__Stone", "first_index": 0,
                "index_count": 3, "min_vertex": 0, "max_vertex": 2,
                "collision_enabled": True, "canonical_package": "WorldT",
                "canonical_name": "Stone",
                "source_texture_identity": "WorldT.Detail.Stone",
                "surface_flags": 0, "pan_zone_actor_identity": "",
            }],
            "lightmap_width": 8, "lightmap_height": 8,
            "lightmap_bgra": bytes([10, 20, 30, 255]) * 64,
        }
        base = bytearray(projection_cache.encode_bsp_v4(geometry))
        struct.pack_into("<H", base, 8, 5)
        struct.pack_into("<H", base, 10, 2 | 4 | 16)

        def trailer(groups, terms, surfaces=((1, 1, 2, 2), (0, 0, 0, 0))):
            body = bytearray(struct.pack("<I", len(surfaces)))
            for rect in surfaces:
                body += struct.pack("<IIII", *rect)
            body += struct.pack("<I", len(groups))
            for light_type, period, phase, count in groups:
                body += struct.pack("<BBBBI", light_type, period, phase, 0, count)
            body += struct.pack("<I", len(terms))
            for kind, group, effect, count, rects in terms:
                body += struct.pack("<BBBBII", kind, group, effect, 0, count, len(rects))
                for surface, rgb in rects:
                    body += struct.pack("<I", surface) + rgb
            return struct.pack("<I", len(body) + 4) + bytes(body)

        rgb = bytes(range(12))
        good = bytes(base) + trailer(
            [(2, 64, 0, 3), (7, 32, 5, 1)],
            [(0, 0, 0, 3, [(0, rgb)]), (0, 1, 0, 1, [(0, rgb)]),
             (1, 255, 3, 2, [(0, rgb)]), (2, 1, 3, 1, [(0, rgb)])])
        parsed = bsp.parse_bsp_payload(good)
        self.assertEqual(parsed["version"], 5)
        animation = parsed["animation"]
        self.assertEqual(animation["surfaces"], [(1, 1, 2, 2), (0, 0, 0, 0)])
        self.assertEqual(
            [group["key"] for group in animation["groups"]], [(2, 64, 0), (7, 32, 5)])
        self.assertEqual(
            [(t["kind"], t["group"], t["effect"], t["light_count"])
             for t in animation["terms"]],
            [(0, 0, 0, 3), (0, 1, 0, 1), (1, None, 3, 2), (2, 1, 3, 1)])
        self.assertEqual(animation["terms"][3]["rects"], [(0, rgb)])
        self.assertEqual(parsed["entry_trailer"], b"")
        # A version-4 header rejects the animation flag; the v5 flag needs the atlas.
        stale = bytearray(good)
        struct.pack_into("<H", stale, 8, 4)
        with self.assertRaisesRegex(bsp.BSPPayloadError, "invalid BSP payload header"):
            bsp.parse_bsp_payload(bytes(stale))
        for label, groups, terms in (
                ("unsorted groups", [(7, 32, 5, 1), (2, 64, 0, 3)],
                 [(0, 0, 0, 1, [(0, rgb)]), (0, 1, 0, 3, [(0, rgb)])]),
                ("waveform slot mismatch", [(2, 64, 0, 3)],
                 [(1, 255, 1, 3, [(0, rgb)])]),
                ("term order", [(2, 64, 0, 3)],
                 [(1, 255, 1, 3, [(0, rgb)]), (0, 0, 0, 3, [(0, rgb)])]),
                ("empty placement rect", [(2, 64, 0, 3)],
                 [(0, 0, 0, 3, [(1, rgb)])]),
                ("short rect", [(2, 64, 0, 3)],
                 [(0, 0, 0, 3, [(0, rgb[:-1])])]),
                ("effect out of range", [(2, 64, 0, 3)],
                 [(0, 0, 0, 3, [(0, rgb)]), (1, 255, 4, 1, [(0, rgb)])])):
            with self.subTest(label=label), self.assertRaises(bsp.BSPPayloadError):
                bsp.parse_bsp_payload(bytes(base) + trailer(groups, terms))
        overlapping = bytes(base) + trailer(
            [(2, 64, 0, 3)], [(0, 0, 0, 3, [(0, rgb)])],
            surfaces=((1, 1, 2, 2), (2, 2, 2, 2)))
        with self.assertRaisesRegex(bsp.BSPPayloadError, "overlapping"):
            bsp.parse_bsp_payload(overlapping)

    def test_canonical_parser_decodes_version6_sky_rest_trailer(self):
        """Version 6: the sky-rest trailer (flag 32) after any
        animation trailer - skybake.py bake_rest's census and skygeo's rest
        groups (native BSPWorld.cpp WoTWorldBuildSkyRestTrailer layout)."""
        geometry = {
            "role": "main",
            "positions": [0.0, 0.0, 0.0, 100.0, 0.0, 0.0, 0.0, 100.0, 0.0],
            "normals": [0.0, 0.0, 1.0] * 3,
            "uv0": [0.0, 0.0, 1.0, 0.0, 0.0, 1.0],
            "uv1": [0.0, 0.0, 0.5, 0.0, 0.0, 0.5],
            "indices": [0, 1, 2],
            "bounds_min": [0.0, 0.0, 0.0],
            "bounds_max": [100.0, 100.0, 0.0],
            "sections": [{
                "material_slot": "WorldT__Stone", "first_index": 0,
                "index_count": 3, "min_vertex": 0, "max_vertex": 2,
                "collision_enabled": True, "canonical_package": "WorldT",
                "canonical_name": "Stone",
                "source_texture_identity": "WorldT.Detail.Stone",
                "surface_flags": 0, "pan_zone_actor_identity": "",
            }],
            "lightmap_width": 8, "lightmap_height": 8,
            "lightmap_bgra": bytes([10, 20, 30, 255]) * 64,
        }
        base = bytearray(projection_cache.encode_bsp_v4(geometry))

        def header(version, flags):
            head = bytearray(base)
            struct.pack_into("<H", head, 8, version)
            struct.pack_into("<H", head, 10, flags)
            return bytes(head)

        def sky_rest(zone=2, rays=100, covered=40,
                     surfaces=((252, 60, 0, 0), (887, 0, 3, 0)),
                     groups=((2, "", 1), (1, "NatureT__SkyMod442", 2))):
            body = bytearray(struct.pack("<IIII", zone, rays, covered, len(surfaces)))
            for row in surfaces:
                body += struct.pack("<IIII", *row)
            body += struct.pack("<I", len(groups))
            for role, slot, triangles in groups:
                raw = slot if isinstance(slot, bytes) else slot.encode("utf-8")
                body += struct.pack("<BBH", role, 0, len(raw)) + raw
                body += struct.pack("<I", triangles)
                for triangle in range(triangles):
                    body += struct.pack("<fff", 0.0, 0.0, 1.0)
                    for corner in range(3):
                        body += struct.pack("<fffffff", 10.0 * (corner == 1),
                                            10.0 * (corner == 2), 500.0 + triangle,
                                            0.5, 0.25, 0.125, 0.0625)
            return struct.pack("<I", len(body) + 4) + bytes(body)

        parsed = bsp.parse_bsp_payload(header(6, 2 | 4 | 32) + sky_rest())
        self.assertEqual(parsed["version"], 6)
        rest = parsed["sky_rest"]
        self.assertEqual((rest["sky_zone"], rest["ray_count"], rest["covered_rays"]),
                         (2, 100, 40))
        self.assertEqual(rest["surfaces"], [
            {"surface": 252, "visible": 60, "blocker": 0, "fakebackdrop": 0},
            {"surface": 887, "visible": 0, "blocker": 3, "fakebackdrop": 0}])
        self.assertEqual([(g["role"], g["slot"], len(g["triangles"]))
                          for g in rest["groups"]],
                         [("blocker", "", 1), ("visible", "NatureT__SkyMod442", 2)])
        self.assertEqual(rest["groups"][1]["triangles"][1][3 + 7:3 + 14],
                         (10.0, 0.0, 501.0, 0.5, 0.25, 0.125, 0.0625))
        self.assertIsNone(parsed["animation"])
        # Version 5 cannot carry flag 32; the flag needs the atlas; hostile
        # counts, orders, roles and slots fail closed.
        for label, payload in (
                ("v5 header", header(5, 2 | 4 | 32) + sky_rest()),
                ("no atlas flag", header(6, 2 | 32) + sky_rest()),
                ("unflagged bytes", header(6, 2 | 4) + sky_rest()),
                ("zone 0", header(6, 2 | 4 | 32) + sky_rest(zone=0)),
                ("covered over rays", header(6, 2 | 4 | 32) + sky_rest(covered=101)),
                ("unsorted surfaces", header(6, 2 | 4 | 32) + sky_rest(
                    surfaces=((887, 0, 3, 0), (252, 60, 0, 0)))),
                ("empty surface row", header(6, 2 | 4 | 32) + sky_rest(
                    surfaces=((252, 0, 0, 0),))),
                ("visible without slot", header(6, 2 | 4 | 32) + sky_rest(
                    groups=((1, "", 1),))),
                ("blocker with slot", header(6, 2 | 4 | 32) + sky_rest(
                    groups=((2, "Pkg__Wall", 1),))),
                ("unknown role", header(6, 2 | 4 | 32) + sky_rest(groups=((4, "", 1),))),
                ("duplicate group", header(6, 2 | 4 | 32) + sky_rest(
                    groups=((2, "", 1), (2, "", 1)))),
                ("truncated", (header(6, 2 | 4 | 32) + sky_rest())[:-4])):
            with self.subTest(label=label), self.assertRaises(bsp.BSPPayloadError):
                bsp.parse_bsp_payload(payload)
        # Slot text parity with the native strict reader (WoTBSPPayload.cpp
        # DecodeStrictUtf8, spec WoT.LightmapAnimation.Version6SkyRestDecode).
        accented = bsp.parse_bsp_payload(header(6, 2 | 4 | 32) + sky_rest(
            groups=((1, "Sky\u00e9", 1),)))
        self.assertEqual(accented["sky_rest"]["groups"][0]["slot"], "Sky\u00e9")
        for label, raw in (
                ("truncated multi-byte", b"Sky\xc3"),
                ("overlong", b"Sky\xc0\xaf"),
                ("encoded surrogate", b"Sky\xed\xa0\x80"),
                ("stray continuation", b"Sky\x80"),
                ("control character", b"Sky\x01"),
                ("DEL", b"Sky\x7f"),
                ("embedded NUL", b"Sky\x00Nul")):
            with self.subTest(slot=label), self.assertRaises(bsp.BSPPayloadError):
                bsp.parse_bsp_payload(header(6, 2 | 4 | 32) + sky_rest(
                    groups=((1, raw, 1),)))
        # The runtime projection parser admits version 6.
        self.assertEqual(projection_cache.parse_bsp_v4(
            header(6, 2 | 4 | 32) + sky_rest())["version"], 6)

    def test_round_trip_preserves_two_uvs_sections_bounds_and_role(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "Arena_07.glb"
            _write_fixture(source, ("JCylinder0", "Trap墙1"))
            geometry = bsp.decode_bsp_glb(source, "main")
            payload = bsp.encode_bsp_payload(geometry)
            parsed = bsp.parse_bsp_payload(payload)
            self.assertEqual(parsed["vertex_count"], 6)
            self.assertEqual(parsed["index_count"], 6)
            self.assertEqual(parsed["uv_channels"], 2)
            self.assertEqual([item["material_slot"] for item in parsed["sections"]],
                             ["JCylinder0", "Trap墙1"])
            self.assertTrue(all(item["collision_enabled"]
                                for item in parsed["sections"]))
            # glTF metres and Y/Z swizzle were restored to UE centimetres.
            self.assertEqual(parsed["bounds_max"], [300.0, 0.0, 100.0])

    def test_hostile_truncation_trailing_bytes_and_indices_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "Arena_07.glb"
            _write_fixture(source)
            payload = bsp.encode_bsp_payload(
                bsp.decode_bsp_glb(source, "portal"))
            for hostile in (payload[:-1], payload + b"x"):
                with self.subTest(length=len(hostile)), \
                        self.assertRaises(bsp.BSPPayloadError):
                    bsp.parse_bsp_payload(hostile)
            bad_index = bytearray(payload)
            bad_index[-4:] = (999).to_bytes(4, "little")
            with self.assertRaises(bsp.BSPPayloadError):
                bsp.parse_bsp_payload(bad_index)
            bad_utf8 = bytearray(payload)
            slot_offset = bsp._HEADER.size + bsp._SECTION.size
            bad_utf8[slot_offset] = 0x80
            with self.assertRaisesRegex(bsp.BSPPayloadError, "invalid UTF-8 slot"):
                bsp.parse_bsp_payload(bad_utf8)

    def test_uint32_index_path_survives_above_16_bit_range(self):
        count = 65_537
        positions = [0.0] * (count * 3)
        positions[3] = 100.0
        positions[-2] = 100.0
        geometry = {
            "role": "main",
            "positions": positions,
            "normals": [0.0, 0.0, 1.0] * count,
            "uv0": [0.0, 0.0] * count,
            "uv1": [0.0, 0.0] * count,
            "indices": [0, 1, 65_536],
            "sections": [{
                "material_slot": "Stone", "first_index": 0,
                "index_count": 3, "triangle_count": 1,
                "min_vertex": 0, "max_vertex": 65_536,
                "collision_enabled": True,
            }],
            "bounds_min": [0.0, 0.0, 0.0],
            "bounds_max": [100.0, 100.0, 0.0],
            "vertex_count": count,
            "index_count": 3,
            "triangle_count": 1,
            "uv_channels": 2,
            "tangent_algorithm": bsp.BSP_TANGENT_ALGORITHM,
        }
        parsed = bsp.parse_bsp_payload(bsp.encode_bsp_payload(geometry))
        self.assertEqual(parsed["indices"], [0, 1, 65_536])

    def test_payload_canonicalizes_negative_zero_in_every_float_stream(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "Arena_07.glb"
            _write_fixture(source)
            geometry = bsp.decode_bsp_glb(source, "main")
            zero_indices = {
                "positions": 0, "normals": 0, "uv0": 0, "uv1": 0,
                "bounds_min": 0, "bounds_max": 1,
            }
            for key, index in zero_indices.items():
                self.assertEqual(geometry[key][index], 0.0)
                geometry[key][index] = -0.0
            parsed = bsp.parse_bsp_payload(bsp.encode_bsp_payload(geometry))
            for key, index in zero_indices.items():
                self.assertEqual(struct.pack("<f", parsed[key][index]),
                                 b"\0\0\0\0")

    def test_carrier_is_synthetic_tiny_and_keeps_exact_slot_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Arena_07.glb"
            carrier = root / "Arena_07_carrier.glb"
            _write_fixture(source, ("Stone", "Water"), scale=50.0)
            original = bsp.write_stripped_carrier(source, carrier, "main")
            stripped = bsp.decode_bsp_glb(carrier, "main")
            self.assertGreater(max(original["bounds_max"]), 1_000.0)
            self.assertEqual(stripped["vertex_count"], 6)
            self.assertEqual(
                [item["material_slot"] for item in stripped["sections"]],
                ["Stone", "Water"])
            self.assertLess(max(stripped["bounds_max"]), 1.0)

    def test_derived_key_invalidates_payload_engine_platform_and_format(self):
        payload = "1" * 64
        baseline = bsp.bsp_derived_cache_key(payload, "5.8.0-123+main", "Mac")
        variants = (
            bsp.bsp_derived_cache_key("2" * 64, "5.8.0-123+main", "Mac"),
            bsp.bsp_derived_cache_key(payload, "5.8.0-124+main", "Mac"),
            bsp.bsp_derived_cache_key(payload, "5.8.0-123+main", "Win64"),
        )
        self.assertTrue(all(value != baseline for value in variants))
        with self.assertRaises(bsp.BSPPayloadError):
            bsp.bsp_derived_cache_key(
                payload, "5.8.0-123+main", "Mac", format_version=2)
        with self.assertRaises(bsp.BSPPayloadError):
            bsp.bsp_derived_cache_key(
                payload, "5.8.0-123+main", "Mac",
                tangent_algorithm="unknown-tangent-v2")


class BSPCacheLifecycleSpecs(unittest.TestCase):
    def _roots(self, temporary):
        root = Path(temporary)
        retail = root / "retail"
        conversion = root / "conv"
        (retail / "Maps").mkdir(parents=True)
        (retail / "Maps" / "Arena_07.wot").write_bytes(b"retail-arena")
        (retail / "Maps" / "Battle2_02.wot").write_bytes(b"retail-battle")
        (conversion / "tex").mkdir(parents=True)
        flipbooks = conversion / "flipbooks" / "Effects"
        flipbooks.mkdir(parents=True)
        (flipbooks / "_flipbooks.json").write_text(
            '{"fire":{},"runtime":{'
            '"Live":{"object_path":"Effects.Live"}}}\n')
        for name in ("Arena_07", "Battle2_02"):
            (conversion / f"{name}_lmatlas.json").write_text("{}\n")
            (conversion / f"{name}.t3d.json").write_text("{}\n")
        (conversion / "Arena_07.bsp.json").write_text(
            '{"materials":{'
            '"Stone":{"package":"Env","texture":"Stone"},'
            '"Water":{"package":"Effects","texture":"Live"}},'
            '"portal_materials":{'
            '"Portal":{"package":"Effects","texture":"Live"}}}\n')
        (conversion / "Battle2_02.bsp.json").write_text(
            '{"materials":{'
            '"Brick":{"package":"Env","texture":"Brick"}},'
            '"portal_materials":{}}\n')
        _write_fixture(conversion / "Arena_07.glb", ("Stone", "Water"))
        _write_fixture(conversion / "Arena_07_portals.glb", ("Portal",))
        _write_fixture(conversion / "Battle2_02.glb", ("Brick",))
        return retail, conversion, root / "cache"

    def test_multi_map_split_cache_portal_optional_resumes_and_verifies(self):
        with tempfile.TemporaryDirectory() as temporary:
            retail, conversion, output = self._roots(temporary)
            scope = ("Arena_07", "Battle2_02")
            with mock.patch.object(cache, "MP_MAPS", scope):
                first, stats = cache.build_cache(
                    retail, output, ("bsps",), bsp_conversion_dir=conversion,
                    progress=lambda _line: None)
            self.assertEqual(stats, {"packages_built": 2, "packages_resumed": 0})
            self.assertEqual(set(first["entries"]), {
                "bsp:Arena_07__Arena_07",
                "bsp:Arena_07__Arena_07_Fractal_000",
                "bsp:Arena_07__Arena_07_portals_Fractal_000",
                "bsp:Battle2_02__Battle2_02",
            })
            self.assertEqual(first["summary"]["source_packages"]["bsps"], 2)
            self.assertEqual(
                first["entries"]["bsp:Arena_07__Arena_07_Fractal_000"]["role"],
                "main")
            self.assertEqual(
                first["entries"][
                    "bsp:Arena_07__Arena_07_portals_Fractal_000"]["role"],
                "portal")

            with mock.patch.object(cache, "MP_MAPS", scope):
                second, stats = cache.build_cache(
                    retail, output, ("bsps",), bsp_conversion_dir=conversion,
                    progress=lambda _line: None)
            self.assertEqual(stats, {"packages_built": 0, "packages_resumed": 2})
            self.assertEqual(second["cache_sha256"], first["cache_sha256"])

            with mock.patch.object(cache, "MP_MAPS", scope):
                verified, stats = cache.verify_cache(
                    retail, output, ("bsps",), bsp_conversion_dir=conversion,
                    progress=lambda _line: None)
            self.assertEqual(stats, {"entries_verified": 4, "packages_verified": 2})
            self.assertEqual(verified["cache_sha256"], first["cache_sha256"])

            _write_fixture(conversion / "Arena_07.glb", ("Stone", "Water"),
                           scale=2.0)
            with mock.patch.object(cache, "MP_MAPS", scope):
                third, stats = cache.build_cache(
                    retail, output, ("bsps",), bsp_conversion_dir=conversion,
                    progress=lambda _line: None)
            self.assertEqual(stats, {"packages_built": 1, "packages_resumed": 1})
            self.assertNotEqual(
                third["entries"]["bsp:Arena_07__Arena_07"]["sha256"],
                first["entries"]["bsp:Arena_07__Arena_07"]["sha256"])


class WorldSetV3Specs(unittest.TestCase):
    FIXTURE = HERE / "fixtures" / "map-set-v3-synthetic.json"

    def _fixture(self):
        return json.loads(self.FIXTURE.read_text(encoding="ascii"))

    @staticmethod
    def _re_root(document, map_index=None):
        if map_index is not None:
            document["maps"][map_index]["map_root"] = cache.world_map_root(
                document["maps"][map_index])
        document["set_root"] = cache.world_set_root(document["maps"])

    @unittest.skipUnless(
        HAS_WORLD_TEXTURE_CENSUS,
        "retail maps or generated texture manifests are absent",
    )
    def test_required_world_set_is_sourced_only_from_import_all(self):
        self.assertIs(cache.MP_MAPS, import_all.MP_MAPS)
        self.assertEqual(cache.required_world_names(), import_all.MP_MAPS)
        self.assertEqual(len(cache.required_world_names()), 19)
        generated = (cache.REPO / "Source/WoTRetailReader/Public/"
                     "WoTRetailReader/Generated/RequiredWorlds.generated.h")
        subprocess.run([
            sys.executable,
            str(cache.REPO / "Tools/dist/generate_world_registry.py"),
            "--output", str(generated), "--check",
        ], check=True, cwd=cache.REPO, capture_output=True, text=True)
        texture_registry = (
            cache.REPO / "Source/WoTRetailReader/Public/WoTRetailReader/"
            "Generated/WorldTextures.generated.h")
        subprocess.run([
            sys.executable,
            str(cache.REPO / "Tools/dist/generate_world_texture_registry.py"),
            "--retail-root", str(ORIGINAL),
            "--conversion-root", str(cache.REPO / "Intermediate/Conv"),
            "--output", str(texture_registry), "--check",
        ], check=True, cwd=cache.REPO, capture_output=True, text=True)
        property_registry = (
            cache.REPO / "Source/WoTRetailReader/Public/WoTRetailReader/"
            "Generated/WorldProperties.generated.h")
        subprocess.run([
            sys.executable,
            str(cache.REPO / "Tools/dist/generate_world_property_registry.py"),
            "--retail-root", str(ORIGINAL),
            "--output", str(property_registry), "--check",
        ], check=True, cwd=cache.REPO, capture_output=True, text=True)

    @unittest.skipUnless(HAS_RETAIL_WORLDS, "retail reference maps are absent")
    def test_property_registry_covers_front_end_without_expanding_mp_maps(self):
        self.assertEqual(len(import_all.MP_MAPS), 19)
        self.assertEqual(
            property_registry.PROPERTY_MAPS,
            (*import_all.MP_MAPS, "WheelOfTime"),
        )
        rows = property_registry.collect(ORIGINAL)
        navigation = next(
            row for row in rows
            if row[0] == "engine.levelinfo"
            and row[1] == "navigationpointlist"
        )
        self.assertEqual(
            navigation[8],
            "Engine.InventorySpot|Engine.PlayerStart",
        )
        rows_by_field = {(row[0], row[1]): row for row in rows}
        self.assertIn(
            ("wotpawns.houndbot", "initthreatgoalevent"), rows_by_field)
        self.assertIn(
            ("wotpawns.trollocmed", "takedamageevent"), rows_by_field)
        self.assertIn(
            ("wotpawns.trollocmed", "btakedamageeventonce"), rows_by_field)

    @unittest.skipUnless(
        (HERE / "fixtures/bsp-native-parity-v1.json").is_file(),
        "retail-derived BSP parity fixture is absent",
    )
    def test_native_bsp_parity_oracle_covers_every_ordered_split_part(self):
        fixture = json.loads((HERE / "fixtures/bsp-native-parity-v1.json").read_text(
            encoding="ascii"))
        self.assertEqual(set(fixture), {"format", "maps"})
        self.assertEqual(fixture["format"], "wot-native-bsp-parity-v3")
        self.assertEqual([item["identity"] for item in fixture["maps"]],
                         list(import_all.MP_MAPS))
        part_count = 0
        for world in fixture["maps"]:
            self.assertEqual(set(world), {"identity", "parts"})
            self.assertTrue(world["parts"])
            saw_portal = False
            for part in world["parts"]:
                self.assertEqual(set(part), {
                    "index_count", "part_identity", "payload_block_sha256",
                    "payload_byte_length", "payload_samples",
                    "procedural_material_slot", "role", "section_count",
                    "tangent_algorithm", "triangle_count", "vertex_count",
                })
                self.assertEqual(set(part["payload_block_sha256"]), {
                    "indices", "section_table",
                })
                for digest in part["payload_block_sha256"].values():
                    self.assertRegex(digest, r"^[0-9a-f]{64}$")
                samples = part["payload_samples"]
                self.assertEqual(set(samples), {
                    "bounds", "indices", "normals", "positions", "sections",
                    "uv0", "uv1",
                })
                self.assertEqual(len(samples["bounds"]), 6)
                self.assertTrue(all(
                    isinstance(bits, int) and 0 <= bits <= 0xffffffff
                    for bits in samples["bounds"]))
                for name, width in (("positions", 3), ("normals", 3),
                                    ("uv0", 2), ("uv1", 2)):
                    stream = samples[name]
                    self.assertEqual(set(stream), {"end", "head"})
                    expected = min(part["vertex_count"] * width, width * 3)
                    self.assertEqual(len(stream["head"]), expected)
                    self.assertEqual(len(stream["end"]), expected)
                    self.assertTrue(all(
                        isinstance(bits, int) and 0 <= bits <= 0xffffffff
                        for edge in stream.values() for bits in edge))
                index_samples = samples["indices"]
                self.assertEqual(set(index_samples), {"end", "head"})
                expected_indices = min(part["index_count"], 6)
                self.assertEqual(len(index_samples["head"]), expected_indices)
                self.assertEqual(len(index_samples["end"]), expected_indices)
                self.assertEqual(len(samples["sections"]), part["section_count"])
                for section in samples["sections"]:
                    self.assertEqual(set(section), {
                        "canonical_name", "canonical_package",
                        "collision_enabled", "first_index", "index_count",
                        "material_slot", "max_vertex", "min_vertex",
                        "pan_zone_actor_identity", "source_texture_identity",
                        "surface_flags", "triangle_count",
                    })
                    self.assertTrue(section["canonical_name"])
                    self.assertTrue(section["source_texture_identity"])
                    self.assertIsInstance(section["surface_flags"], int)
                    if not section["canonical_package"]:
                        self.assertEqual((section["canonical_name"],
                                          section["source_texture_identity"],
                                          section["material_slot"]),
                                         ("None", "None", "__None"))
                self.assertEqual(part["index_count"], part["vertex_count"])
                self.assertEqual(part["triangle_count"], part["index_count"] // 3)
                self.assertGreater(part["index_count"], 0)
                self.assertGreater(part["section_count"], 0)
                self.assertEqual(
                    part["tangent_algorithm"], bsp.BSP_TANGENT_ALGORITHM)
                self.assertIn(part["role"], ("main", "portal"))
                saw_portal |= part["role"] == "portal"
                if saw_portal:
                    self.assertEqual(part["role"], "portal")
                expected_prefix = (world["identity"] + "_portals"
                                   if part["role"] == "portal"
                                   else world["identity"])
                self.assertTrue(part["part_identity"].startswith(expected_prefix))
                if part["procedural_material_slot"]:
                    self.assertIn("_Fractal_", part["part_identity"])
                    self.assertEqual(part["section_count"], 1)
                if (world["identity"] == "Arena_03"
                        and part["part_identity"] == "Arena_03"):
                    # This pin previously held 0x423B3F3A and was described as
                    # proof of a native factor-of-two defect. That was wrong,
                    # and the story is worth keeping: the conversion GLBs had
                    # been regenerated WITHOUT bsp2obj's tex-root argument, so
                    # load_tex_sizes returned nothing and every texture fell
                    # back to DrawScale 1.0. AesSedai_T.MarbRed1910_T really is
                    # 128x64 DrawScale 2, so U must divide by w*ds = 256, not
                    # 128 -- the "expected" value was exactly twice the truth.
                    # Native was correct throughout. Reconverting with the
                    # tex root reproduces 0x41BB3F3A bit-for-bit on both sides.
                    self.assertEqual(
                        struct.pack("<I", samples["uv0"]["end"][0]),
                        struct.pack("<I", 0x41BB3F3A))
                part_count += 1
        self.assertEqual(part_count, 65)

    def test_synthetic_set_has_exact_headers_and_all_required_vectors(self):
        document = self._fixture()
        self.assertEqual(document, cache.synthetic_map_set_v3())
        self.assertEqual(cache.validate_map_set_v3(document), document["set_root"])
        self.assertEqual([item["identity"] for item in document["maps"]],
                         list(import_all.MP_MAPS))
        self.assertEqual(set(document), {"format", "maps", "set_root"})
        for world in document["maps"]:
            self.assertEqual(set(world), {
                "actors", "attachments", "bsp", "identity", "lights",
                "map_root", "navigation_seeds", "sky_zone", "sounds",
            })
            self.assertEqual(set(world["bsp"]), {
                "bounds_bits", "collision_surfaces", "nodes", "portals",
                "surfaces", "zones",
            })
            self.assertTrue(world["actors"])
            self.assertTrue(world["attachments"])
            self.assertTrue(world["lights"])
            self.assertTrue(world["sounds"])
            self.assertTrue(world["navigation_seeds"])
            self.assertTrue(world["bsp"]["nodes"])
            self.assertTrue(world["bsp"]["surfaces"])
            self.assertTrue(world["bsp"]["zones"])
            self.assertTrue(world["bsp"]["portals"])
            self.assertTrue(world["bsp"]["collision_surfaces"])
            for actor in world["actors"]:
                self.assertEqual(set(actor), {
                    "class", "identity", "properties", "references",
                })
            for node in world["bsp"]["nodes"]:
                self.assertEqual(set(node), {
                    "back", "front", "surface", "vertex_indices", "zones",
                })
            for portal in world["bsp"]["portals"]:
                self.assertEqual(set(portal), {
                    "back_zone", "front_zone", "surface",
                })
            for surface in world["bsp"]["surfaces"]:
                self.assertEqual(set(surface), {"flags", "material"})
            for zone in world["bsp"]["zones"]:
                self.assertEqual(set(zone), {
                    "actor", "connectivity", "visibility",
                })
            for attachment in world["attachments"]:
                self.assertEqual(set(attachment), {"child", "parent"})
            for sound in world["sounds"]:
                self.assertEqual(set(sound), {"actor", "asset"})

    def test_absent_map_one_ten_or_nineteen_never_returns_a_set_root(self):
        for index in (0, 9, 18):
            document = self._fixture()
            del document["maps"][index]
            with self.subTest(index=index), self.assertRaisesRegex(
                    cache.CacheError, "exactly import_all.MP_MAPS"):
                cache.validate_map_set_v3(document)

    def test_duplicate_identity_late_corruption_and_eighteenth_interrupt_fail(self):
        duplicate = self._fixture()
        duplicate["maps"][18]["identity"] = duplicate["maps"][0]["identity"]
        with self.assertRaisesRegex(cache.CacheError, "duplicate map identity"):
            cache.validate_map_set_v3(duplicate)

        corrupt = self._fixture()
        corrupt["maps"][18]["bsp"]["nodes"][0]["surface"] = 999
        self._re_root(corrupt, 18)
        with self.assertRaisesRegex(cache.CacheError, "corrupt BSP node"):
            cache.validate_map_set_v3(corrupt)

        with self.assertRaisesRegex(cache.CacheError, "before map 18"):
            cache.validate_map_set_v3(self._fixture(), interrupt_after=17)

    def test_cross_map_actor_attachment_and_registry_closure_is_atomic(self):
        mutations = []

        cycle = self._fixture()
        first = cycle["maps"][0]
        first["attachments"].append({
            "child": first["attachments"][0]["parent"],
            "parent": first["attachments"][0]["child"],
        })
        self._re_root(cycle, 0)
        mutations.append((cycle, "attachment cycle"))

        dangling = self._fixture()
        dangling["maps"][0]["actors"][0]["references"].append(
            "Arena_01.MissingActor")
        self._re_root(dangling, 0)
        mutations.append((dangling, "dangling actor reference"))

        for field, replacement, message in (
                ("lights", ["Arena_01.Mover0"], "light registry"),
                ("navigation_seeds", ["Arena_01.Light0"], "navigation seed"),
                ("sky_zone", "Arena_01.Light0", "sky/zone binding")):
            mutated = self._fixture()
            mutated["maps"][0][field] = replacement
            self._re_root(mutated, 0)
            mutations.append((mutated, message))

        bad_sound = self._fixture()
        bad_sound["maps"][0]["sounds"][0]["actor"] = "Arena_01.Light0"
        self._re_root(bad_sound, 0)
        mutations.append((bad_sound, "sound binding"))

        bad_collision = self._fixture()
        bad_collision["maps"][0]["bsp"]["collision_surfaces"] = [99]
        self._re_root(bad_collision, 0)
        mutations.append((bad_collision, "invalid collision"))

        for document, message in mutations:
            with self.subTest(message=message), self.assertRaisesRegex(
                    cache.CacheError, message):
                cache.validate_map_set_v3(document)

    @unittest.skipUnless(HAS_RETAIL_WORLDS, "retail reference maps are absent")
    def test_real_mp_census_and_arena_01_content_hash_pin(self):
        set_root, records = cache.retail_world_set_root(ORIGINAL)
        self.assertEqual(
            set_root,
            "fa7f7ab48f080aec4ef3ab00ae0c925d7bf691382477a55c9e10fa28df62aeaf")
        self.assertEqual([record["identity"] for record in records],
                         list(import_all.MP_MAPS))
        arena = records[0]
        self.assertEqual(arena, {
            "exports": 2134,
            "identity": "Arena_01",
            "imports": 145,
            "map_root":
                "484f424af7c8c6fc76e2fc389d34e6ab9c671ca5328961587e7d8ef8f38ea975",
            "nodes": 5556,
            "package_sha256":
                "eebe5698072f2571cee7f7cc41d5daf0280ef77c0520be48d8e284c8e3de7caf",
            "points": 9561,
            "portal_surfaces": 42,
            "size": 2016673,
            "surfaces": 2838,
            "version": 68,
            "vertices": 83163,
        })


if __name__ == "__main__":
    unittest.main()
