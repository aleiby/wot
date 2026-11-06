"""The public export drops every Git LFS path, so these inputs must be plain blobs.

.gitattributes routes *.bin, *.png and ThirdParty archives through LFS, then
overrides that for byte-frozen inputs the public tree needs: the synthetic test
oracles and the innoextract upstream archive. This pins the overrides against
the real tree, so a reordered or narrowed rule cannot quietly send them back to
LFS (and out of the export).
"""

from pathlib import Path
import shutil
import subprocess
import unittest


REPO = Path(__file__).resolve().parents[2]
EXEMPT_PATHSPECS = (
    ":(glob)Tools/dist/fixtures/*.bin",
    ":(glob)Source/**/Tests/Fixtures/*.bin",
    ":(glob)Tools/test/*/fixtures/**/*.png",
    "ThirdParty/innoextract/1.9/upstream/innoextract-1.9.tar.gz",
)
STILL_LFS = (
    "ThirdParty/innoextract/1.9/lib/Win64/wot_innoextract.lib",
    "ThirdParty/innoextract/1.9/lib/Mac/libwot_innoextract.a",
)
LFS_POINTER_PREFIX = b"version https://git-lfs"


def _git(*arguments: str, data: bytes = b"") -> bytes:
    return subprocess.run(
        ["git", "-C", str(REPO), *arguments], input=data, capture_output=True,
        check=True).stdout


def _filters(paths: list[str]) -> dict[str, str]:
    output = _git("check-attr", "-z", "--stdin", "filter",
                  data=b"".join(path.encode("utf-8") + b"\0" for path in paths))
    fields = output.split(b"\0")
    return {fields[index].decode("utf-8"): fields[index + 2].decode("utf-8")
            for index in range(0, len(fields) - 2, 3)}


@unittest.skipUnless(shutil.which("git") and (REPO / ".git").exists(),
                     "needs a git checkout")
class PublicBlobExemptionTests(unittest.TestCase):
    def test_exempt_inputs_are_plain_blobs(self):
        paths = [path for path in _git("ls-files", "-z", "--", *EXEMPT_PATHSPECS)
                 .decode("utf-8").split("\0") if path]
        self.assertGreaterEqual(len(paths), 100)
        filters = _filters(paths)
        for path in paths:
            with self.subTest(path=path):
                self.assertIn(filters[path], ("unset", "unspecified"))
                stored = _git("cat-file", "blob", f":{path}")
                self.assertFalse(stored.startswith(LFS_POINTER_PREFIX),
                                 "the index holds an LFS pointer, not the bytes")

    def test_the_promoted_libraries_stay_in_lfs(self):
        filters = _filters(list(STILL_LFS))
        for path in STILL_LFS:
            with self.subTest(path=path):
                self.assertEqual(filters[path], "lfs")


if __name__ == "__main__":
    unittest.main()
