"""Behavioral tests for the standalone pose-table compiler (Python 3.9)."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("generate_held_artifact_poses.py")
ROOT = SCRIPT.resolve().parents[2]
HEADER = "Class,OffsetX,OffsetY,OffsetZ,Pitch,Yaw,Roll,ScaleMultiplier\n"
DEFAULT = "*,60,20,-16,0,0,0,1\n"


class PoseGeneratorTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "Saved"
        scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.source = pathlib.Path(self.temp.name) / "poses.csv"
        self.output = pathlib.Path(self.temp.name) / "poses.inl"

    def run_generator(self, rows, check=False):
        self.source.write_bytes(rows.encode("utf-8"))
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--input", str(self.source),
             "--output", str(self.output)] + (["--check"] if check else []),
            capture_output=True, text=True)

    def test_compiles_numeric_rows_deterministically_with_lf(self):
        rows = HEADER + DEFAULT + "Fixture,50,21,-18,10,-20,30,0.5\n"
        result = self.run_generator(rows)
        self.assertEqual(result.returncode, 0, result.stderr)
        first = self.output.read_bytes()
        self.assertIn(b'TEXT("Fixture")', first)
        self.assertIn(b"FVector(50.0, 21.0, -18.0)", first)
        self.assertIn(b"FRotator(10.0, -20.0, 30.0)", first)
        self.assertNotIn(b"\r", first)
        self.assertEqual(self.run_generator(rows).returncode, 0)
        self.assertEqual(self.output.read_bytes(), first)

    def test_row_order_does_not_change_output(self):
        row = "Fixture,50,21,-18,10,-20,30,0.5\n"
        self.assertEqual(self.run_generator(HEADER + DEFAULT + row).returncode, 0)
        first = self.output.read_bytes()
        self.assertEqual(self.run_generator(HEADER + row + DEFAULT).returncode, 0)
        self.assertEqual(self.output.read_bytes(), first)

    def test_rejects_invalid_rows_without_overwriting_output(self):
        invalid = [
            HEADER, HEADER + DEFAULT + DEFAULT,
            HEADER + "Fixture,60,20,-16,0,0,0,1\n",
            HEADER + DEFAULT + "Fixture,60,20,-16,0,0,0,0\n",
            HEADER + DEFAULT.replace("60", "nan"),
            HEADER + DEFAULT.replace("60", "inf"),
            HEADER + DEFAULT.replace("60", "0"),
            HEADER + DEFAULT + "bad\"name,60,20,-16,0,0,0,1\n",
            HEADER + DEFAULT + "Fixture,60,20,-16,0,0,0,1\nfixture,60,20,-16,0,0,0,1\n",
            HEADER + DEFAULT.replace(",1\n", "\n"),
            HEADER + DEFAULT.replace(",1\n", ",1,extra\n"),
            HEADER.replace("OffsetX", "X") + DEFAULT,
        ]
        self.output.write_bytes(b"keep me\n")
        for rows in invalid:
            with self.subTest(rows=rows):
                self.assertNotEqual(self.run_generator(rows).returncode, 0)
                self.assertEqual(self.output.read_bytes(), b"keep me\n")

    def test_check_detects_stale_output_without_writing(self):
        self.assertEqual(self.run_generator(HEADER + DEFAULT).returncode, 0)
        self.assertEqual(self.run_generator(HEADER + DEFAULT, check=True).returncode, 0)
        self.output.write_bytes(b"stale\n")
        self.assertNotEqual(self.run_generator(HEADER + DEFAULT, check=True).returncode, 0)
        self.assertEqual(self.output.read_bytes(), b"stale\n")

    def test_checked_in_table_is_current(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--check"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
