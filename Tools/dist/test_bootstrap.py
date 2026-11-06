#!/usr/bin/env python3
"""Focused fail-closed specs for bootstrap zone-export freshness."""
import json
import hashlib
import io
import os
import sys
import tempfile
import unittest
import urllib.error
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bootstrap


class _FakeResponse:
    """Minimal urlopen stand-in: status, header map, and a readable body."""

    def __init__(self, status, body, headers=None):
        self.status = status
        self.headers = headers or {}
        self._body = body
        self._read = 0
        self.closed = False

    def getcode(self):
        return self.status

    def read(self, size=-1):
        if size is None or size < 0:
            size = len(self._body) - self._read
        block = self._body[self._read:self._read + size]
        self._read += len(block)
        return block

    def close(self):
        self.closed = True

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()
        return False


class ZoneExportFreshnessTests(unittest.TestCase):
    def test_zero_manifests_is_not_fresh(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "Map.zones.json"
            output.write_text(json.dumps({
                "check_walk_surfaces": True,
                "ideal_player_count": 4,
                "friction_nodes": [],
            }), encoding="utf-8")
            manifest_root = root / "tex"
            manifest_root.mkdir()
            self.assertFalse(bootstrap._zone_export_has_walk_surfaces(
                output, manifest_root))

    def test_pre_g07_export_without_ideal_player_count_is_stale(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "Map.zones.json"
            output.write_text(json.dumps({
                "check_walk_surfaces": True,
                "friction_nodes": [],
            }), encoding="utf-8")
            manifest = root / "tex" / "Pkg" / "_manifest.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({"textures": {}}), encoding="utf-8")
            os.utime(manifest, ns=(1_000_000_000, 1_000_000_000))
            os.utime(output, ns=(2_000_000_000, 2_000_000_000))
            self.assertFalse(bootstrap._zone_export_has_walk_surfaces(
                output, root / "tex"))

    def test_nonempty_manifest_set_must_not_be_newer_than_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "Map.zones.json"
            output.write_text(json.dumps({
                "check_walk_surfaces": True,
                "ideal_player_count": 4,
                "friction_nodes": [],
            }), encoding="utf-8")
            manifest = root / "tex" / "Pkg" / "_manifest.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({"textures": {}}), encoding="utf-8")
            os.utime(manifest, ns=(1_000_000_000, 1_000_000_000))
            os.utime(output, ns=(2_000_000_000, 2_000_000_000))
            self.assertTrue(bootstrap._zone_export_has_walk_surfaces(
                output, root / "tex"))
            os.utime(manifest, ns=(3_000_000_000, 3_000_000_000))
            self.assertFalse(bootstrap._zone_export_has_walk_surfaces(
                output, root / "tex"))


class InstallerVerificationTests(unittest.TestCase):
    def test_sha256_is_an_independent_required_installer_pin(self):
        with tempfile.TemporaryDirectory() as temporary:
            installer = Path(temporary) / "installer.exe"
            payload = b"small synthetic installer"
            installer.write_bytes(payload)
            pins = {
                "INSTALLER_SIZE": len(payload),
                "INSTALLER_MD5": hashlib.md5(payload).hexdigest(),
                "INSTALLER_SHA1": hashlib.sha1(payload).hexdigest(),
                "INSTALLER_SHA256": hashlib.sha256(payload).hexdigest(),
            }
            with mock.patch.multiple(bootstrap, **pins):
                bootstrap.verify_installer(installer)
            pins["INSTALLER_SHA256"] = "0" * 64
            with mock.patch.multiple(bootstrap, **pins):
                with self.assertRaisesRegex(ValueError, "installer sha256"):
                    bootstrap.verify_installer(installer)


class DownloadResumeSemanticsTests(unittest.TestCase):
    """Frozen migration oracle for the landed resumable-download behavior.

    cache-v3's native resumable downloader inherits these exact rules, so the
    Python original pins them: a resume offers `Range` plus the recorded
    validator; a 206 at the wrong offset discards the partial; a changed
    validator (origin answers 200) truncates instead of appending; and a
    sidecar belonging to another URL never validates a foreign partial.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.destination = self.root / "payload.bin"
        self.url = "https://example.invalid/payload.bin"
        self.requests = []

    def tearDown(self):
        self._tmp.cleanup()

    def _seed_partial(self, data, *, url=None, etag=None, last_modified=None):
        bootstrap._partial_path(self.destination).write_bytes(data)
        if url is not None:
            value = {"url": url}
            if etag:
                value["etag"] = etag
            if last_modified:
                value["last_modified"] = last_modified
            bootstrap._download_metadata_path(self.destination).write_text(
                json.dumps(value, indent=2, sort_keys=True) + "\n")

    def _download(self, responses, expected_size=None):
        def fake_urlopen(request, timeout=None):
            self.requests.append(request)
            outcome = responses[len(self.requests) - 1]
            if isinstance(outcome, Exception):
                raise outcome
            return outcome

        with mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
            return bootstrap.download_resumable(
                self.url, self.destination, expected_size=expected_size)

    def test_resume_offers_range_and_the_recorded_validator(self):
        self._seed_partial(b"abcd", url=self.url, etag='"v1"')
        self._download([_FakeResponse(206, b"efgh", {
            "Content-Range": "bytes 4-7/8", "ETag": '"v1"'})],
            expected_size=8)
        self.assertEqual(self.requests[0].headers["Range"], "bytes=4-")
        self.assertEqual(self.requests[0].headers["If-range"], '"v1"')
        self.assertEqual(self.destination.read_bytes(), b"abcdefgh")
        self.assertFalse(
            bootstrap._download_metadata_path(self.destination).exists())

    def test_range_restart_at_the_wrong_offset_discards_the_partial(self):
        self._seed_partial(b"abcd", url=self.url, etag='"v1"')
        response = _FakeResponse(206, b"zzzz", {
            "Content-Range": "bytes 2-5/8", "ETag": '"v1"'})
        # The message changed when the decision moved into decide_http_body; the
        # BEHAVIOUR pinned here has not: refuse, and discard the partial.
        with self.assertRaisesRegex(ValueError, "refusing the response"):
            self._download([response], expected_size=8)
        self.assertTrue(response.closed)
        self.assertFalse(bootstrap._partial_path(self.destination).exists())
        self.assertFalse(
            bootstrap._download_metadata_path(self.destination).exists())
        self.assertFalse(self.destination.exists())

    def test_malformed_or_inconsistent_content_ranges_discard_the_partial(self):
        for content_range in (
                "bytes 4-6/8",       # does not end at total - 1
                "bytes 4-7/9",       # wrong total
                "bytes 4-garbage",   # incomplete grammar
                "bytes 4-7/8 extra", # trailing text
        ):
            with self.subTest(content_range=content_range):
                self.destination.unlink(missing_ok=True)
                self._seed_partial(b"abcd", url=self.url, etag='"v1"')
                response = _FakeResponse(206, b"efgh", {
                    "Content-Range": content_range, "Content-Length": "4",
                    "ETag": '"v1"'})
                self.requests.clear()
                with self.assertRaisesRegex(ValueError, "refusing the response"):
                    self._download([response], expected_size=8)
                self.assertFalse(bootstrap._partial_path(self.destination).exists())

    def test_changed_validator_truncates_instead_of_appending(self):
        self._seed_partial(b"abcd", url=self.url, etag='"v1"')
        self._download([_FakeResponse(200, b"ABCDEFGH", {"ETag": '"v2"'})],
                       expected_size=8)
        self.assertEqual(self.destination.read_bytes(), b"ABCDEFGH")

    def test_sidecar_for_another_url_restarts_from_zero(self):
        self._seed_partial(b"abcd", url="https://example.invalid/other.bin",
                           etag='"v1"')
        self._download([_FakeResponse(200, b"ABCDEFGH", {"ETag": '"v2"'})],
                       expected_size=8)
        self.assertNotIn("Range", self.requests[0].headers)
        self.assertEqual(self.destination.read_bytes(), b"ABCDEFGH")

    def test_complete_partial_is_promoted_when_the_origin_answers_416(self):
        self._seed_partial(b"abcdefgh", url=self.url, etag='"v1"')
        self._download([urllib.error.HTTPError(
            self.url, 416, "Range Not Satisfiable", None, None)],
            expected_size=8)
        self.assertEqual(self.destination.read_bytes(), b"abcdefgh")
        self.assertFalse(
            bootstrap._download_metadata_path(self.destination).exists())

    def test_short_body_keeps_the_partial_for_the_next_resume(self):
        with self.assertRaisesRegex(ValueError, "rerun to resume"):
            self._download([_FakeResponse(200, b"abcd", {"ETag": '"v1"'})],
                           expected_size=8)
        self.assertEqual(
            bootstrap._partial_path(self.destination).read_bytes(), b"abcd")
        self.assertEqual(
            json.loads(bootstrap._download_metadata_path(
                self.destination).read_text())["etag"], '"v1"')


class ConverterIdentityMarkerTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.source = self.root / "converter.py"
        self.source.write_text("SCHEMA = 'v1'\n", encoding="utf-8")
        self.output = self.root / "result.json"
        self.output.write_text("{}\n", encoding="utf-8")
        self.marker = self.root / "markers" / "synthetic" / "item.complete"

    def tearDown(self):
        bootstrap._converter_identity_for_phase.cache_clear()
        self._tmp.cleanup()

    def test_matching_converter_identity_keeps_the_marker_skippable(self):
        with mock.patch.dict(bootstrap.CONVERTER_STAGE_SCHEMAS, {
                "synthetic": "schema-v1"}), mock.patch.dict(
                    bootstrap.CONVERTER_STAGE_ENTRYPOINTS, {
                        "synthetic": (self.source,)}):
            bootstrap._write_completion_marker(self.marker, [self.output])
            self.assertTrue(bootstrap._completion_marker_valid(self.marker))

    def test_converter_source_change_forces_the_item_to_rerun(self):
        with mock.patch.dict(bootstrap.CONVERTER_STAGE_SCHEMAS, {
                "synthetic": "schema-v1"}), mock.patch.dict(
                    bootstrap.CONVERTER_STAGE_ENTRYPOINTS, {
                        "synthetic": (self.source,)}):
            bootstrap._write_completion_marker(self.marker, [self.output])
            self.source.write_text("SCHEMA = 'v2'\n", encoding="utf-8")
            bootstrap._converter_identity_for_phase.cache_clear()
            self.assertFalse(bootstrap._completion_marker_valid(self.marker))

    def test_converter_schema_change_forces_the_item_to_rerun(self):
        with mock.patch.dict(bootstrap.CONVERTER_STAGE_SCHEMAS, {
                "synthetic": "schema-v1"}), mock.patch.dict(
                    bootstrap.CONVERTER_STAGE_ENTRYPOINTS, {
                        "synthetic": (self.source,)}):
            bootstrap._write_completion_marker(self.marker, [self.output])
            bootstrap.CONVERTER_STAGE_SCHEMAS["synthetic"] = "schema-v2"
            bootstrap._converter_identity_for_phase.cache_clear()
            self.assertFalse(bootstrap._completion_marker_valid(self.marker))

    def test_legacy_marker_without_converter_identity_forces_rerun(self):
        self.marker.parent.mkdir(parents=True)
        self.marker.write_text(json.dumps({
            "status": "complete",
            "outputs": [str(self.output.resolve())],
        }), encoding="utf-8")
        with mock.patch.dict(bootstrap.CONVERTER_STAGE_SCHEMAS, {
                "synthetic": "schema-v1"}), mock.patch.dict(
                    bootstrap.CONVERTER_STAGE_ENTRYPOINTS, {
                        "synthetic": (self.source,)}):
            self.assertFalse(bootstrap._completion_marker_valid(self.marker))

    def test_converter_source_with_utf8_bom_is_identified(self):
        self.source.write_bytes(b"\xef\xbb\xbfSCHEMA = 'v1'\n")
        with mock.patch.dict(bootstrap.CONVERTER_STAGE_SCHEMAS, {
                "synthetic": "schema-v1"}), mock.patch.dict(
                    bootstrap.CONVERTER_STAGE_ENTRYPOINTS, {
                        "synthetic": (self.source,)}):
            bootstrap._write_completion_marker(self.marker, [self.output])
            self.assertTrue(bootstrap._completion_marker_valid(self.marker))


class RetiredBootstrapModeTests(unittest.TestCase):
    def test_release_and_update_mode_flags_are_rejected(self):
        retired = (
            ("--release", "release.json"),
            ("--release-asset", "client.zip"),
            ("--cache", "cache"),
            ("--check-update",),
            ("--release-repo", "owner/repo"),
        )
        for arguments in retired:
            with self.subTest(flag=arguments[0]):
                with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    bootstrap.parser().parse_args(arguments)

    def test_dry_run_keeps_exactly_the_seven_local_stages(self):
        args = bootstrap.parser().parse_args(["--dry-run"])
        ctx = bootstrap.build_context(args)
        output = io.StringIO()
        with redirect_stdout(output):
            bootstrap.print_plan(ctx, set(bootstrap.STAGES))
        rendered = output.getvalue()
        self.assertEqual(bootstrap.STAGES, (
            "download", "unpack", "extract", "convert", "import", "cook",
            "assemble"))
        for index, stage in enumerate(bootstrap.STAGES, 1):
            self.assertIn(f"{index}. [RUN] {stage}", rendered)
        self.assertNotIn("--release", rendered)


if __name__ == "__main__":
    unittest.main()
