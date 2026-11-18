"""Behavioral tests for the first-person hand pose compiler (Python 3.9)."""
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("generate_hand_poses.py")
ROOT = SCRIPT.resolve().parents[2]


JSON_NULL = object()  # sets an explicit null; None deletes the key


def source(**changes):
    data = {
        "poses": {
            "relaxed": {"bones": {"index_proximal_r": [10]}},
            "point": {"bones": {"middle_proximal_r": [80]}},
            "rod": {"bones": {"index_proximal_r": [90, 0, 0]},
                    "hand": {"offset": [30, 18, -7], "rotator": [28, -15, 36], "scale": 1.35},
                    "item": {"axis": [0, 2, 0], "front": [-1, 0, 0], "point": [3, -3, 3.5], "half_length": 11, "lead": 0.25}},
            "orb": {"bones": {"index_proximal_r": [30]},
                    "hand": {"offset": [33, 23, -11], "rotator": [3, -16, 180], "scale": 1.3},
                    "item": {"axis": [0, 0, -1], "front": [-1, 0, 0], "point": [0, 0.2, -0.6], "half_length": 4.5, "lead": 1}},
        },
        "items": {"AngrealInvFixture": {"grip": "rod", "axis": "+Y", "front": "-X", "size": 9.5, "slide": 0.5}},
    }
    for path, value in changes.items():
        node = data
        keys = path.split("__")
        for key in keys[:-1]:
            node = node[key]
        if value is None:
            del node[keys[-1]]
        else:
            node[keys[-1]] = None if value is JSON_NULL else value
    return json.dumps(data)


class HandPoseGeneratorTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "Saved"
        scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.source = pathlib.Path(self.temp.name) / "hand_poses.json"
        self.output = pathlib.Path(self.temp.name) / "hand_poses.inl"

    def run_generator(self, text, check=False):
        self.source.write_bytes(text.encode("utf-8"))
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--input", str(self.source),
             "--output", str(self.output)] + (["--check"] if check else []),
            capture_output=True, text=True)

    def test_compiles_poses_and_items_deterministically_with_lf(self):
        result = self.run_generator(source())
        self.assertEqual(result.returncode, 0, result.stderr)
        first = self.output.read_bytes()
        # A grip row carries the hand placement and a normalised item line and front.
        self.assertIn(b'{TEXT("rod"), HandPoseBones_rod, UE_ARRAY_COUNT(HandPoseBones_rod), true, '
                      b"FVector(30.0, 18.0, -7.0), FRotator(28.0, -15.0, 36.0), 1.35f, "
                      b"FVector(0.0, 1.0, 0.0), FVector(-1.0, 0.0, 0.0), FVector(3.0, -3.0, 3.5), 0.0f, 11.0f, 0.25f}", first)
        # An item row names its grip, its leading and facing local axes, its size and slide.
        self.assertIn(b'{TEXT("AngrealInvFixture"), TEXT("rod"), FVector(0.0, 1.0, 0.0), '
                      b"FVector(-1.0, 0.0, 0.0), 9.5f, 0.5f, FVector(0.0, 0.0, 0.0)}", first)
        self.assertNotIn(b"\r", first)
        self.assertEqual(self.run_generator(source()).returncode, 0)
        self.assertEqual(self.output.read_bytes(), first)

    def test_item_axes_may_be_vectors_with_an_anchor_offset(self):
        # A handle that runs off the mesh axes: the leading axis is normalised, the front is
        # kept across it, and the offset (item-local cm) moves the anchor off the bounds centre.
        text = source(items__AngrealInvFixture={"grip": "rod", "axis": [0, 3, 4], "front": "-X", "size": 8,
                                                "offset": [0, -3, 0.5]})
        result = self.run_generator(text)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'{TEXT("AngrealInvFixture"), TEXT("rod"), FVector(0.0, 0.6, 0.8), '
                      b"FVector(-1.0, 0.0, 0.0), 8.0f, 0.0f, FVector(0.0, -3.0, 0.5)}", self.output.read_bytes())

    def test_only_the_relaxed_pose_is_required(self):
        result = self.run_generator(source(poses__point=None))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"HandPoseBones_point", self.output.read_bytes())

    def test_bone_angles_compile_to_flex_then_spread_then_twist(self):
        # Flex 90 alone is a quarter turn about Y; flex 90 with spread 90 is Rz(90) * Ry(90).
        text = source(poses__relaxed__bones={"index_proximal_r": [90], "middle_proximal_r": [90, 90, 0]})
        self.assertEqual(self.run_generator(text).returncode, 0)
        out = self.output.read_bytes()
        self.assertIn(b'{TEXT("index_proximal_r"), FQuat(0.0, 0.707107, 0.0, 0.707107)}', out)
        self.assertIn(b'{TEXT("middle_proximal_r"), FQuat(-0.5, 0.5, 0.5, 0.5)}', out)

    def test_rejects_invalid_sources_without_overwriting_output(self):
        invalid = [
            "{", source(poses__relaxed=None),
            source(poses__relaxed__bones={"elbow_r": [1]}),
            source(poses__relaxed__bones={"index_proximal_r": [1, 2, 3, 4]}),
            source(poses__rod__item=None),
            source(poses__rod__item__axis=[0, 0, 0]),
            source(poses__rod__item__front=[0, 1, 0]),
            source(poses__rod__hand__scale=0),
            source(poses__orb=None),
            source(poses__orb__hand=None, poses__orb__item=None),
            source(poses__orb__hand=JSON_NULL),
            source(poses__rod__hand=JSON_NULL),
            source(poses__orb__hand=[33, 23, -11]),
            source(poses__rod__item="rod"),
            source(poses__relaxed=[]),
            source(poses__relaxed__bones={"thumb_intermediate_r": [5]}),
            source(poses__rod__hand__scale=1e-7),
            source(poses__rod__hand__scale=1e40),
            source(poses__rod__item__point=[1e40, 0, 0]),
            source(items__AngrealInvFixture__size=1e-9),
            source(poses__rod__item__lead=float("inf")),
            source(items__AngrealInvFixture__grip="relaxed"),
            source(items__AngrealInvFixture__grip="missing"),
            source(items__AngrealInvFixture__axis="Y"),
            source(items__AngrealInvFixture__front="-Y"),
            source(items__AngrealInvFixture__axis=[-1, 0.01, 0]),
            source(items__AngrealInvFixture__axis=[0, 0, 0]),
            source(items__AngrealInvFixture__offset=[0, 1]),
            source(items__AngrealInvFixture__size=0),
            source(items__AngrealInvFixture__slide=float("nan")),
            source(items={"bad\"name": {"grip": "rod", "axis": "+Y", "front": "-X", "size": 9}}),
        ]
        self.output.write_bytes(b"keep me\n")
        for text in invalid:
            with self.subTest(text=text):
                self.assertNotEqual(self.run_generator(text).returncode, 0)
                self.assertEqual(self.output.read_bytes(), b"keep me\n")

    def test_check_detects_stale_output_without_writing(self):
        self.assertEqual(self.run_generator(source()).returncode, 0)
        self.assertEqual(self.run_generator(source(), check=True).returncode, 0)
        self.output.write_bytes(b"stale\n")
        self.assertNotEqual(self.run_generator(source(), check=True).returncode, 0)
        self.assertEqual(self.output.read_bytes(), b"stale\n")

    def test_every_granted_artifact_has_a_grip(self):
        # The artifacts the all-artifacts cheat grants are the ones a player can hold.
        character = (ROOT / "Source/WoT/WoTCharacter.cpp").read_text(encoding="utf-8")
        block = re.search(r"GWoTArtifactNameSuffixes\[\] = \{(.*?)\};", character, re.S)
        self.assertIsNotNone(block)
        suffixes = re.findall(r'TEXT\("(\w+)"\)', block.group(1))
        self.assertGreaterEqual(len(suffixes), 40)
        items = json.loads((ROOT / "Plugins/WoTFirstPerson/Data/hand_poses.json").read_text(encoding="utf-8"))["items"]
        self.assertEqual(sorted("AngrealInv" + s for s in suffixes), sorted(items))

    def test_checked_in_table_is_current(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--check"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
