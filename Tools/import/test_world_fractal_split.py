import json
from pathlib import Path
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
CONVERT = HERE.parent / "convert"
for path in (HERE, CONVERT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from gltf import write_glb  # noqa: E402
from world_fractal_split import (  # noqa: E402
    _read_glb,
    record_bearing_slots,
    runtime_fractal_texture_keys,
    split_world_glb,
)


class WorldFractalSplitTests(unittest.TestCase):
    def make_glb(self, root):
        groups = {
            "Stone__Wall": (
                [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 1.0] * 3,
                [0.0, 0.0, 1.0, 0.0, 0.0, 1.0], [0, 1, 2],
                [0.1, 0.1, 0.2, 0.1, 0.1, 0.2]),
            "Water__Live_T": (
                [2.0, 0.0, 0.0, 3.0, 0.0, 0.0, 2.0, 1.0, 0.0],
                [0.0, 0.0, 1.0] * 3,
                [0.0, 0.0, 1.0, 0.0, 0.0, 1.0], [0, 1, 2],
                [0.3, 0.3, 0.4, 0.3, 0.3, 0.4]),
        }
        source = root / "Fixture.glb"
        write_glb(source, "Fixture", groups)
        return source

    def test_no_records_uses_original_without_writing(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            source = self.make_glb(root)
            before = source.read_bytes()
            output = root / "parts"
            parts = split_world_glb(source, output, [], "Fixture")
            self.assertEqual(parts[0]["path"], source)
            self.assertEqual(source.read_bytes(), before)
            self.assertFalse(output.exists())

    def test_split_is_deterministic_and_reuses_binary_and_accessors(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            source = self.make_glb(root)
            source_doc, source_chunks = _read_glb(source)
            first = split_world_glb(
                source, root / "first", ["Water__Live_T"], "Fixture")
            second = split_world_glb(
                source, root / "second", ["Water__Live_T"], "Fixture")
            self.assertEqual([part["material_slot"] for part in first],
                             [None, "Water__Live_T"])
            self.assertEqual([part["asset_name"] for part in first],
                             ["Fixture", "Fixture_Fractal_000"])
            self.assertEqual([part["path"].read_bytes() for part in first],
                             [part["path"].read_bytes() for part in second])

            source_non_json = [chunk for chunk in source_chunks
                               if chunk[0] != 0x4E4F534A]
            source_primitives = source_doc["meshes"][0]["primitives"]
            for part in first:
                part_doc, part_chunks = _read_glb(part["path"])
                self.assertEqual(
                    [chunk for chunk in part_chunks if chunk[0] != 0x4E4F534A],
                    source_non_json)
                primitive = part_doc["meshes"][0]["primitives"][0]
                source_index = 0 if part["material_slot"] is None else 1
                self.assertEqual(primitive["attributes"],
                                 source_primitives[source_index]["attributes"])
                self.assertEqual(primitive["indices"],
                                 source_primitives[source_index]["indices"])
                self.assertIn("TEXCOORD_1", primitive["attributes"])

    def test_runtime_records_select_package_qualified_slots(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            package = root / "JHPFireEng"
            package.mkdir()
            (package / "_flipbooks.json").write_text(json.dumps({
                "runtime": {
                    "SLWater02": {
                        "object_path": "JHPFireEng.Group.SLWater02"
                    }
                }
            }), encoding="utf-8")
            keys = runtime_fractal_texture_keys(root)
            slots = record_bearing_slots({
                "JHPFireEng__SLWater02_T": {
                    "package": "jhpfireeng", "texture": "slwater02"},
                "Other__SLWater02": {
                    "package": "Other", "texture": "SLWater02"},
            }, keys)
            self.assertEqual(slots, ["JHPFireEng__SLWater02_T"])


if __name__ == "__main__":
    unittest.main()
