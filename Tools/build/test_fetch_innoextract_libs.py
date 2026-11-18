import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_innoextract_libs as fetcher
import stage_innoextract_native as stage


REPO = Path(__file__).resolve().parents[2]
VENDOR = REPO / "ThirdParty/innoextract/1.9"


class FetchInnoextractLibsTests(unittest.TestCase):
    def _manifest(self, root: Path, win64_bytes: bytes) -> Path:
        manifest = json.loads((VENDOR / "vendor-manifest.json").read_bytes())
        manifest["artifacts"]["libraries"]["Win64"]["sha256"] = (
            hashlib.sha256(win64_bytes).hexdigest())
        path = root / "vendor-manifest.json"
        path.write_bytes(stage._canonical_json(manifest))
        return path

    def test_asset_names_carry_the_pin_prefix(self):
        digest = "ab" * 32
        self.assertEqual(fetcher.asset_name("Win64", digest),
                         "wot_innoextract-1.9-Win64-abababababab.lib")
        self.assertEqual(fetcher.asset_name("Mac", digest),
                         "libwot_innoextract-1.9-Mac-abababababab.a")

    def test_the_checked_in_pins_name_both_assets(self):
        for platform in ("Win64", "Mac"):
            destination, digest = fetcher.library_pin(platform)
            self.assertEqual(destination.name, {
                "Win64": "wot_innoextract.lib", "Mac": "libwot_innoextract.a"}[platform])
            self.assertTrue(fetcher.asset_name(platform, digest).endswith(
                digest[:12] + destination.suffix))

    def test_fetch_places_only_bytes_that_match_the_pin(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = self._manifest(root, b"reviewed library")
            requested = []

            def tampered(url):
                requested.append(url)
                return b"tampered library"

            with self.assertRaisesRegex(fetcher.FetchError, "nothing was placed"):
                fetcher.fetch("Win64", manifest_path=manifest, download=tampered)
            destination = root / "lib/Win64/wot_innoextract.lib"
            self.assertFalse(destination.exists())
            digest = hashlib.sha256(b"reviewed library").hexdigest()
            self.assertEqual(requested, [
                fetcher.DOWNLOAD_BASE + fetcher.asset_name("Win64", digest)])
            self.assertTrue(requested[0].startswith(
                "https://github.com/aleiby/wot/releases/download/vendor-innoextract-1.9/"))

            fetcher.fetch("Win64", manifest_path=manifest,
                          download=lambda url: b"reviewed library")
            self.assertEqual(destination.read_bytes(), b"reviewed library")
            self.assertEqual(sorted(p.name for p in destination.parent.iterdir()),
                             ["wot_innoextract.lib"])

    def test_fetch_skips_a_library_already_matching_its_pin(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = self._manifest(root, b"reviewed library")
            destination = root / "lib/Win64/wot_innoextract.lib"
            destination.parent.mkdir(parents=True)
            destination.write_bytes(b"reviewed library")

            def unreachable(url):
                raise AssertionError("no download expected")

            fetcher.fetch("Win64", manifest_path=manifest, download=unreachable)

    def test_fetch_replaces_a_contributor_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = self._manifest(root, b"reviewed library")
            destination = root / "lib/Win64/wot_innoextract.lib"
            destination.parent.mkdir(parents=True)
            destination.write_bytes(b"contributor build")
            fetcher.fetch("Win64", manifest_path=manifest,
                          download=lambda url: b"reviewed library")
            self.assertEqual(destination.read_bytes(), b"reviewed library")

    def test_the_manifest_cannot_redirect_the_destination(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest_path = self._manifest(root, b"reviewed library")
            for path in ("../../escaped.lib", str(root.parent / "abs.lib"),
                         "lib/Win64/other.lib", "lib/Mac/libwot_innoextract.a"):
                manifest = json.loads(manifest_path.read_bytes())
                manifest["artifacts"]["libraries"]["Win64"]["path"] = path
                manifest_path.write_bytes(stage._canonical_json(manifest))
                with self.subTest(path=path):
                    with self.assertRaisesRegex(fetcher.FetchError, "only lib/Win64"):
                        fetcher.fetch("Win64", manifest_path=manifest_path,
                                      download=lambda url: b"reviewed library")
            self.assertFalse((root.parent / "abs.lib").exists())

    def test_a_linked_library_directory_cannot_lead_outside(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "vendor"
            outside = Path(temporary) / "outside"
            root.mkdir()
            outside.mkdir()
            manifest = self._manifest(root, b"reviewed library")
            try:
                (root / "lib").symlink_to(outside, target_is_directory=True)
            except OSError:
                self.skipTest("this host cannot create directory symlinks")
            with self.assertRaisesRegex(fetcher.FetchError, "leads outside"):
                fetcher.fetch("Win64", manifest_path=manifest,
                              download=lambda url: b"reviewed library")
            self.assertEqual(list(outside.iterdir()), [])

    def test_download_failures_are_fetch_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest = self._manifest(Path(temporary), b"reviewed library")

            def offline(url):
                raise OSError("network unreachable")

            with self.assertRaisesRegex(fetcher.FetchError, "could not download"):
                fetcher.fetch("Win64", manifest_path=manifest, download=offline)

    def test_stage_assets_refuses_a_library_that_is_not_the_pinned_one(self):
        with tempfile.TemporaryDirectory() as temporary:
            # verify_library re-checks every manifest-owned file, so stage
            # from a full vendor copy (without its LFS libraries).
            root = Path(temporary) / "vendor"
            shutil.copytree(VENDOR, root, ignore=shutil.ignore_patterns("lib"))
            manifest = self._manifest(root, b"reviewed library")
            (root / "lib/Win64").mkdir(parents=True)
            (root / "lib/Win64/wot_innoextract.lib").write_bytes(b"contributor build")
            with self.assertRaisesRegex(stage.VendorError, "differs from its manifest pin"):
                fetcher.stage_assets(Path(temporary) / "out", manifest_path=manifest)
            self.assertFalse((Path(temporary) / "out").exists()
                             and any((Path(temporary) / "out").iterdir()))


if __name__ == "__main__":
    unittest.main()
