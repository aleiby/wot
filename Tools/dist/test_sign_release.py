#!/usr/bin/env python3
"""Specs for sign_release.py.

Keys are minted by `openssl genpkey` inside a temporary directory
(release_test_fixtures) and destroyed with it. The suite skips cleanly on a host
without an OpenSSL 3 CLI; Git for Windows and Homebrew both provide one.
"""
from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import release_test_fixtures as fixtures  # noqa: E402
import sign_release  # noqa: E402
import verify_release  # noqa: E402

DOMAIN = "com.aleiby.wot.release.v1"


class SignReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = fixtures.ReleaseFixture()
        cls.output = cls.fixture.root / "set"
        cls.receipt = cls.fixture.sign(cls.output)

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()

    def read(self, name):
        return (self.output / name).read_bytes()

    def test_funnel_problems_warn_and_supply_no_validated_proof(self):
        releases = {platform: formats.load_guarded_release(path, platform)
                    for platform, path in self.fixture.releases.items()}
        malformed = self.fixture.root / "malformed-funnel.json"
        malformed.write_text("{broken", encoding="utf-8")
        for label, receipts in (
                ("missing", {}),
                ("unreadable", {"Win64": self.fixture.root / "absent.json"}),
                ("malformed", {"Win64": malformed}),
                ("unknown", {"Linux": malformed})):
            with self.subTest(label=label), contextlib.redirect_stderr(io.StringIO()) as errors:
                proofs = sign_release._check_funnel_receipts(
                    releases, receipts, False, require_update_cells=True)
                self.assertEqual(proofs, {"Win64": None, "Mac": None})
                self.assertIn("WARNING", errors.getvalue())
                self.assertIn("Win64", errors.getvalue())
                self.assertIn("Mac", errors.getvalue())
                if label == "unknown":
                    self.assertIn("Linux", errors.getvalue())

    def test_advisory_funnel_contains_nested_schema_exceptions(self):
        releases = {platform: formats.load_guarded_release(path, platform)
                    for platform, path in self.fixture.releases.items()}
        for exception in (KeyError("blobs"), TypeError("bad nesting"),
                          ValueError("bad value")):
            with self.subTest(exception=type(exception).__name__), \
                    mock.patch.object(
                        sign_release.formats, "validate_funnel_receipt",
                        side_effect=exception), \
                    contextlib.redirect_stderr(io.StringIO()) as errors:
                proofs = sign_release._check_funnel_receipts(
                    releases, self.fixture.funnel_receipts, False)
                self.assertEqual(proofs, {"Win64": None, "Mac": None})
                self.assertIn("no validated funnel proof", errors.getvalue())

    def test_publish_set_has_the_native_shapes(self):
        expected = {formats.KEY_RING_ASSET, formats.KEY_RING_ASSET + ".sig"}
        for platform in formats.PLATFORMS:
            expected |= {formats.announcement_asset(platform),
                         formats.announcement_asset(platform) + ".sig",
                         formats.manifest_asset(platform),
                         formats.manifest_asset(platform) + ".sig",
                         formats.cache_lineage_asset(platform),
                         formats.cache_lineage_asset(platform) + ".sig",
                         formats.update_archive_asset(platform, 0, fixtures.COMMIT),
                         f"WoT-{platform}-guarded-shipping-{fixtures.COMMIT}.zip"}
        dmg = formats.guarded_install_dmg_asset(
            fixtures.COMMIT, formats.SHIPPING_CONFIGURATION)
        expected |= {dmg, dmg + ".verify.json"}
        self.assertEqual({path.name for path in self.output.iterdir()}, expected)
        tag_prefix = "https://github.com/aleiby/wot/releases/download/v0.1.0/"
        names = {
            "Win64": ("Game",
                      "WoT/Binaries/Win64/WoT-Win64-Shipping.exe"),
            "Mac": ("WoT.app", "Contents/MacOS/WoT-Mac-Shipping"),
        }
        for platform, (root, main) in names.items():
            announcement = json.loads(self.read(formats.announcement_asset(platform)))
            self.assertEqual(announcement["release_sequence"], 0)
            self.assertEqual(announcement["key_ring_epoch"], 1)
            self.assertEqual(announcement["key_id"], fixtures.RELEASE_KEY_ID)
            self.assertEqual(announcement["announcement_domain"], DOMAIN)
            self.assertEqual(announcement["key_ring"]["url"],
                             tag_prefix + formats.KEY_RING_ASSET)
            self.assertEqual(announcement["manifest"]["url"],
                             tag_prefix + formats.manifest_asset(platform))
            manifest = json.loads(self.read(formats.manifest_asset(platform)))
            self.assertEqual(manifest["installed_tree_root"]["name"], root)
            self.assertEqual(manifest["main_executable"]["name"], main)
            self.assertNotIn("updater_executable", manifest)
            # The schema const (release-manifest-v2.schema.json): B4 resolved.
            self.assertEqual(manifest["target_attestation"]["configuration"], "Shipping")
            self.assertEqual(manifest["generated_utc"], fixtures.GENERATED_UTC[platform])
            self.assertEqual(manifest["build_commit"], fixtures.COMMIT)
            self.assertEqual(manifest["cache_contract_id"],
                             "wot-production-cache-contract-v3")
            self.assertTrue(manifest["archive"]["url"].startswith(tag_prefix))
            for name in (formats.announcement_asset(platform),
                         formats.manifest_asset(platform),
                         formats.cache_lineage_asset(platform)):
                raw = self.read(name)
                self.assertEqual(formats.canonical_bytes(json.loads(raw)), raw)
                self.assertEqual(len(self.read(name + ".sig")), 64)

    def test_lineage_repeats_the_gate_receipt_at_this_sequence(self):
        for platform in formats.PLATFORMS:
            with self.subTest(platform=platform):
                gate = json.loads((self.fixture.root / "cache-gates" / "set" / platform
                                   / "cache-gate-a.json").read_text(encoding="utf-8"))
                lineage = formats.parse_cache_lineage(
                    self.read(formats.cache_lineage_asset(platform)), platform)
                self.assertEqual(lineage, {
                    "format": "wot-cache-lineage-v1", "release_sequence": 0,
                    **gate["lineage"]})
                row = self.receipt["platforms"][platform]["cache_lineage"]
                self.assertEqual(row["name"], formats.cache_lineage_asset(platform))
                self.assertEqual(row["sha256"], formats.sha256_hex(
                    self.read(formats.cache_lineage_asset(platform))))
                self.assertEqual(row["signature"]["sha256"], formats.sha256_hex(
                    self.read(formats.cache_lineage_asset(platform) + ".sig")))
                self.assertIn(formats.cache_lineage_asset(platform),
                              self.receipt["set"]["assets"])

    def test_cache_gate_receipts_are_required_and_bound_to_the_signed_zip(self):
        releases = self.fixture.releases
        gates = self.fixture.write_cache_gates(releases, "gate-cases")
        error = self.assertRefused(cache_gates={"Win64": gates["Win64"]})
        self.assertIn("--cache-gate", str(error))
        swapped = self.assertRefused(cache_gates={"Win64": gates["Mac"],
                                                  "Mac": gates["Win64"]})
        self.assertIn("is for 'Win64', not Mac", str(swapped))
        other = fixtures.write_guarded_release(self.fixture.root / "gate-other-zip",
                                               "Win64", commit="fedcba987")
        foreign = self.fixture.write_cache_gates({"Win64": other}, "gate-foreign")
        error = self.assertRefused(cache_gates={"Win64": foreign["Win64"],
                                                "Mac": gates["Mac"]})
        self.assertIn("not the WoT-Win64-guarded-shipping-0a1b2c3d4.zip being signed",
                      str(error))
        failing = gates["Win64"].parent / "failing.json"
        failing.write_text(json.dumps({"format": "wot-cache-gate-a-v1",
                                       "platform": "Win64", "reason": "x",
                                       "verdict": "fail"}), encoding="utf-8")
        error = self.assertRefused(cache_gates={"Win64": failing, "Mac": gates["Mac"]})
        self.assertIn("verdict is 'fail', not 'pass'", str(error))
        tampered = gates["Win64"].parent / "tampered.json"
        document = json.loads(gates["Win64"].read_text(encoding="utf-8"))
        document["lineage"]["manifest_sha256"] = "0" * 64
        tampered.write_text(json.dumps(document), encoding="utf-8")
        error = self.assertRefused(cache_gates={"Win64": tampered, "Mac": gates["Mac"]})
        self.assertIn("lineage does not repeat the candidate", str(error))
        # The reference must be the immediately previous release: an older one
        # could let a past revision bump hide changed cache bytes.
        stale = gates["Win64"].parent / "stale-reference.json"
        document = json.loads(gates["Win64"].read_text(encoding="utf-8"))
        document["reference"]["release_sequence"] = 3
        stale.write_text(json.dumps(document), encoding="utf-8")
        error = self.assertRefused(cache_gates={"Win64": stale, "Mac": gates["Mac"]})
        self.assertIn("compared against release sequence 3, not the previous release (0)",
                      str(error))
        missing = gates["Win64"].parent / "no-reference-sequence.json"
        document = json.loads(gates["Win64"].read_text(encoding="utf-8"))
        del document["reference"]["release_sequence"]
        missing.write_text(json.dumps(document), encoding="utf-8")
        error = self.assertRefused(cache_gates={"Win64": missing, "Mac": gates["Mac"]})
        self.assertIn("release_sequence", str(error))

    def test_a_higher_revision_gate_signs_its_new_label(self):
        gates = self.fixture.write_cache_gates(
            self.fixture.releases, "gate-revision-two", revision=2)
        output = self.fixture.root / "revision-two-set"
        self.fixture.sign(output, cache_gates=gates)
        for platform in formats.PLATFORMS:
            lineage = formats.parse_cache_lineage(
                (output / formats.cache_lineage_asset(platform)).read_bytes(), platform)
            self.assertEqual(lineage["revision"], 2)
            self.assertEqual(lineage["label"], fixtures.content_label.label(2))

    def test_every_signature_is_accepted_by_openssl_and_domain_bound(self):
        openssl = self.fixture.openssl
        ring = self.read(formats.KEY_RING_ASSET)
        checks = [(self.fixture.root_public, crypto.CONTEXT_RELEASE_KEY_RING_V1,
                   ring, self.read(formats.KEY_RING_ASSET + ".sig"))]
        for platform in formats.PLATFORMS:
            name = formats.manifest_asset(platform)
            checks.append((self.fixture.release_public, crypto.CONTEXT_RELEASE_MANIFEST_V2,
                           self.read(name), self.read(name + ".sig")))
            name = formats.announcement_asset(platform)
            checks.append((self.fixture.announcement_public, crypto.CONTEXT_ANNOUNCEMENT_V1,
                           self.read(name), self.read(name + ".sig")))
            name = formats.cache_lineage_asset(platform)
            checks.append((self.fixture.release_public, crypto.CONTEXT_CACHE_LINEAGE_V1,
                           self.read(name), self.read(name + ".sig")))
        for key, context, payload, signature in checks:
            self.assertTrue(crypto.openssl_verify_framed(openssl, key, context, payload,
                                                         signature))
            self.assertTrue(crypto.verify_framed(key, context, payload, signature))
            other = (context + 1) % len(crypto.CONTEXT_NAMES)
            self.assertFalse(crypto.openssl_verify_framed(openssl, key, other, payload,
                                                          signature))
            self.assertFalse(crypto.verify_framed(key, other, payload, signature))

    def test_signing_is_deterministic(self):
        again = self.fixture.root / "set-again"
        self.fixture.sign(again)
        for path in self.output.iterdir():
            self.assertEqual(path.read_bytes(), (again / path.name).read_bytes(), path.name)

    def test_receipt_records_public_material_only(self):
        text = json.dumps(self.receipt)
        self.assertNotIn("PRIVATE", text.upper())
        self.assertNotIn(str(self.fixture.root_key), text)
        self.assertEqual(self.receipt["release_key"]["public_key"],
                         self.fixture.release_public.hex())
        self.assertEqual(self.receipt["set"]["tag"], "v0.1.0")
        self.assertEqual(self.receipt["platforms"]["Mac"]["dropped_source_metadata"],
                         [])
        self.assertEqual(self.receipt["configuration"], "Shipping")
        self.assertEqual(
            self.receipt["platforms"]["Mac"]["dmg"]["name"],
            formats.guarded_install_dmg_asset(
                fixtures.COMMIT, formats.SHIPPING_CONFIGURATION))
        self.assertIsNone(self.receipt["platforms"]["Win64"]["dmg"])
        for platform, release in self.fixture.releases.items():
            row = self.receipt["platforms"][platform]
            twin = json.loads(release.read_text(encoding="utf-8"))["test_twin"]
            self.assertEqual(row["test_twin_release_json_sha256"],
                             twin["release_json_sha256"])
            self.assertEqual(row["funnel_receipt"]["manifest_sha256"],
                             twin["release_json_sha256"])
            self.assertEqual(row["funnel_receipt"]["cells"]["host-join"], "pass")

    def test_unnotarized_shipping_dmg_requires_recorded_override(self):
        mac = fixtures.write_guarded_release(
            self.fixture.root / "unnotarized-sign-mac", "Mac",
            notarized=False)
        releases = {"Win64": self.fixture.releases["Win64"], "Mac": mac}
        funnels = dict(self.fixture.funnel_receipts)
        funnels["Mac"] = fixtures.write_funnel_receipt(
            self.fixture.root / "unnotarized-sign-funnel.json", mac)
        error = self.assertRefused(
            release_jsons=releases, funnel_receipts=funnels)
        self.assertIn("--allow-unnotarized", str(error))
        receipt = self.fixture.sign(
            self.fixture.root / "unnotarized-sign-allowed",
            release_jsons=releases, funnel_receipts=funnels,
            allow_unnotarized=True)
        self.assertTrue(receipt["allow_unnotarized"])

    def assertRefused(self, **overrides):
        output = self.fixture.root / f"refused-{len(list(self.fixture.root.iterdir()))}"
        with self.assertRaises((sign_release.SignError, crypto.CryptoError,
                                formats.FormatError, verify_release.VerifyError)) as caught:
            self.fixture.sign(output, **overrides)
        if output.exists():
            self.assertEqual(list(output.iterdir()), [], "a refused set left files behind")
        return caught.exception

    def assertFunnelWarning(self, **overrides):
        output = self.fixture.root / f"advisory-{len(list(self.fixture.root.iterdir()))}"
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            signed = self.fixture.sign(output, **overrides)
        self.assertIn("WARNING", errors.getvalue())
        self.assertTrue(any(row["funnel_receipt"] is None
                            for row in signed["platforms"].values()))
        return errors.getvalue()

    def test_refuses_a_key_that_is_not_the_compiled_announcement_key(self):
        error = self.assertRefused(announcement_key_path=self.fixture.release_key)
        self.assertIn("not the expected signer", str(error))

    def test_refuses_a_release_key_the_ring_does_not_name(self):
        self.assertRefused(release_key_path=self.fixture.announcement_key)
        self.assertRefused(release_key_id="release-missing")

    def test_positive_sequence_warns_for_incomplete_update_funnel_evidence(self):
        releases = {
            platform: fixtures.write_guarded_release(
                self.fixture.root / f"gate-release-{platform}", platform,
                release_sequence=1)
            for platform in formats.PLATFORMS}

        def receipt(platform, label, *, include=True, cells=None):
            return fixtures.write_funnel_receipt(
                self.fixture.root / "gate-funnel" / platform / label / "receipt.json",
                releases[platform], include_update_cells=include, cells=cells)

        complete = {platform: receipt(platform, "complete")
                    for platform in formats.PLATFORMS}
        missing_translocation = receipt("Mac", "missing-cell")
        missing_document = json.loads(
            missing_translocation.read_text(encoding="utf-8"))
        missing_document["cells"].pop()
        missing_translocation.write_text(
            json.dumps(missing_document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        cases = (
            ("missing receipt", {}, "missing --funnel-receipt"),
            ("receipt without update cells",
             {platform: receipt(platform, "base-only", include=False)
              for platform in formats.PLATFORMS}, "cell update-apply is None"),
            ("failed update cell",
             {"Win64": receipt("Win64", "failed", cells={"update-apply": "fail"}),
              "Mac": complete["Mac"]}, "cell update-apply is 'fail'"),
            ("one platform missing", {"Win64": complete["Win64"]},
             "missing --funnel-receipt Mac="),
            ("mac cell missing",
             {"Win64": complete["Win64"],
              "Mac": missing_translocation},
             "ordered, duplicate-free platform matrix"),
        )
        for label, receipts, fragment in cases:
            with self.subTest(label=label):
                error = self.assertFunnelWarning(
                    release_sequence=1, release_jsons=releases,
                    funnel_receipts=receipts, tag="v0.1.1")
                self.assertIn(fragment, str(error))

        output = self.fixture.root / "sequence-one-proven"
        signed = self.fixture.sign(
            output, release_sequence=1, release_jsons=releases,
            funnel_receipts=complete, tag="v0.1.1")
        self.assertEqual(signed["sequence_gate"]["mode"], "task7-update-funnel")
        self.assertEqual(
            signed["sequence_gate"]["platforms"]["Mac"]["cells"]
            ["update-apply"], "pass")

        base_only = {platform: receipt(platform, "override", include=False)
                     for platform in formats.PLATFORMS}
        rehearsal = self.fixture.sign(
            self.fixture.root / "sequence-one-override", release_sequence=1,
            release_jsons=releases, funnel_receipts=base_only,
            allow_staging_sequence=True, tag="v0.1.1-rehearsal")
        self.assertEqual(rehearsal["sequence_gate"], {
            "mode": "unproven", "platforms": {},
        })

    def test_sequence_evidence_cannot_be_replayed_into_the_next_release(self):
        sequence_one = {
            platform: fixtures.write_guarded_release(
                self.fixture.root / f"replay-one-{platform}", platform,
                release_sequence=1)
            for platform in formats.PLATFORMS}
        one_receipts = {
            platform: fixtures.write_funnel_receipt(
                self.fixture.root / f"replay-one-{platform}-funnel.json", release,
                include_update_cells=True)
            for platform, release in sequence_one.items()}

        # Even with an honest sequence-two Test twin, a sequence-one funnel
        # receipt cannot be replayed.
        sequence_two = {
            platform: fixtures.write_guarded_release(
                self.fixture.root / f"replay-two-{platform}", platform,
                release_sequence=2)
            for platform in formats.PLATFORMS}
        receipt_error = self.assertFunnelWarning(
            release_sequence=2, release_jsons=sequence_two,
            funnel_receipts=one_receipts, tag="v0.1.2-replayed-receipt")
        self.assertIn("different release sequence", str(receipt_error))

        # Copying the Test-twin provenance along with the old receipt is also
        # refused before any output survives.
        replayed = {}
        for platform in formats.PLATFORMS:
            old = json.loads(sequence_one[platform].read_text(encoding="utf-8"))
            replayed[platform] = fixtures.write_guarded_release(
                self.fixture.root / f"replay-two-both-{platform}", platform,
                release_sequence=2, test_twin=old["test_twin"])
        twin_error = self.assertRefused(
            release_sequence=2, release_jsons=replayed,
            funnel_receipts=one_receipts, tag="v0.1.2-replayed-both")
        self.assertIn("test_twin release_sequence", str(twin_error))

    def test_signed_update_and_manual_archives_are_the_same_release_tree(self):
        for platform in formats.PLATFORMS:
            with self.subTest(platform=platform):
                install_root = formats.GUARDED_ZIP_ROOTS[platform]
                tree_root = formats.GUARDED_TREE_ROOTS[platform]
                update_root = formats.UPDATE_TREE_ROOTS[platform]
                manual = self.output / formats.guarded_install_zip_asset(
                    platform, fixtures.COMMIT, formats.SHIPPING_CONFIGURATION)
                update = self.output / formats.update_archive_asset(
                    platform, 0, fixtures.COMMIT)

                def file_tree(path, prefix, *, inventory=False):
                    with zipfile.ZipFile(path) as archive:
                        roots = {info.filename.rstrip("/").split("/", 1)[0]
                                 for info in archive.infolist() if info.filename.rstrip("/")}
                        files = {
                            info.filename[len(prefix) + 1:]: archive.read(info)
                            for info in archive.infolist()
                            if not info.is_dir() and info.filename.startswith(prefix + "/")
                            and (inventory or not info.filename.endswith(
                                "/" + formats.INVENTORY_NAME))}
                    return roots, files

                manual_roots, manual_files = file_tree(manual, tree_root)
                update_roots, update_files = file_tree(update, update_root)
                self.assertEqual((manual_roots, update_roots),
                                 ({install_root}, {update_root}))
                self.assertEqual(update_files, manual_files)
                projected = formats.project_guarded_archive_tree(manual, platform)
                staged = formats.stage_archive(update, platform)
                manifest = json.loads(self.read(formats.manifest_asset(platform)))
                self.assertEqual((projected.root, staged.root),
                                 (update_root, update_root))
                self.assertEqual(projected.entries, staged.entries)
                self.assertEqual({entry.path for entry in staged.entries
                                  if entry.type == "file"}, set(manual_files))
                self.assertEqual((projected.tree_root, staged.tree_root),
                                 (manifest["installed_tree_root"]["sha256"],) * 2)

    def test_release_identity_sequence_mismatch_is_refused_for_either_platform(self):
        for platform in formats.PLATFORMS:
            with self.subTest(platform=platform):
                releases = {
                    name: fixtures.write_guarded_release(
                        self.fixture.root / f"identity-{platform}-{name}", name,
                        release_sequence=1,
                        files=(fixtures.platform_files(name, release_sequence=2)
                               if name == platform else None))
                    for name in formats.PLATFORMS}
                receipts = {
                    name: fixtures.write_funnel_receipt(
                        self.fixture.root / f"identity-{platform}-funnel-{name}.json",
                        release, include_update_cells=True)
                    for name, release in releases.items()}
                error = self.assertRefused(
                    release_sequence=1, release_jsons=releases,
                    funnel_receipts=receipts, tag=f"v0.1.1-{platform.lower()}")
                self.assertIn(f"{platform}: release identity sequence 2", str(error))

    def test_refuses_missing_malformed_mismatched_or_empty_release_identity(self):
        identity_path = fixtures.release_identity_relative("Win64")
        cases = {
            "missing release identity": None,
            "malformed release identity": b"{",
            "release identity sequence": fixtures.release_identity_bytes(4),
            "empty release commit": fixtures.release_identity_bytes(0, ""),
            "release identity commit": fixtures.release_identity_bytes(0, "fedcba987"),
        }
        for index, (fragment, identity) in enumerate(cases.items()):
            with self.subTest(fragment=fragment):
                files = fixtures.platform_files("Win64")
                if identity is None:
                    files.pop(identity_path)
                else:
                    _data, mode = files[identity_path]
                    files[identity_path] = (identity, mode)
                release = fixtures.write_guarded_release(
                    self.fixture.root / f"bad-identity-{index}", "Win64",
                    files=files)
                funnel = fixtures.write_funnel_receipt(
                    self.fixture.root / f"bad-identity-{index}-funnel.json",
                    release)
                refusal_arguments = {
                    "release_jsons": {
                        "Win64": release,
                        "Mac": self.fixture.releases["Mac"],
                    },
                    "funnel_receipts": {
                        "Win64": funnel,
                        "Mac": self.fixture.funnel_receipts["Mac"],
                    },
                }
                error = self.assertRefused(**refusal_arguments)
                self.assertIsInstance(error, sign_release.SignError)
                self.assertIn(fragment, str(error))
                if fragment == "release identity sequence":
                    for allow_option in ({"allow_staging_sequence": True},
                                         {"allow_test": True}):
                        flagged_error = self.assertRefused(
                            **refusal_arguments, **allow_option)
                        self.assertIsInstance(flagged_error, sign_release.SignError)
                        self.assertIn(fragment, str(flagged_error))

    def test_refuses_dirty_or_mismatched_builds(self):
        dirty = fixtures.write_guarded_release(self.fixture.root / "dirty", "Win64",
                                               dirty=True)
        self.assertRefused(release_jsons={"Win64": dirty,
                                          "Mac": self.fixture.releases["Mac"]})
        other = fixtures.write_guarded_release(self.fixture.root / "other", "Win64",
                                               commit="fedcba987")
        self.assertRefused(release_jsons={"Win64": other,
                                          "Mac": self.fixture.releases["Mac"]})
        self.assertRefused(release_jsons={"Win64": self.fixture.releases["Win64"]})

    def test_refuses_appledouble_before_signing(self):
        mac = fixtures.write_guarded_release(
            self.fixture.root / "appledouble-mac", "Mac")
        document = json.loads(mac.read_text(encoding="utf-8"))
        archive = mac.parent / document["asset"]
        with zipfile.ZipFile(archive, "a") as handle:
            handle.writestr("WoT.app/Contents/Resources/._Global.pak", b"metadata")
        document["sha256"], document["size"] = formats.sha256_file(archive)
        mac.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                       encoding="utf-8")
        funnel = fixtures.write_funnel_receipt(
            self.fixture.root / "appledouble-mac-funnel.json", mac)
        error = self.assertRefused(
            release_jsons={"Win64": self.fixture.releases["Win64"], "Mac": mac},
            funnel_receipts={
                "Win64": self.fixture.funnel_receipts["Win64"], "Mac": funnel})
        self.assertIn("AppleDouble metadata", str(error))

    def test_refuses_test_releases_unless_explicitly_allowed(self):
        test_releases = {
            platform: fixtures.write_guarded_release(
                self.fixture.root / f"test-{platform}", platform,
                configuration="Test")
            for platform in formats.PLATFORMS}
        error = self.assertRefused(release_jsons=test_releases, funnel_receipts={})
        self.assertIn("--allow-test", str(error))
        self.assertIn("'Test'", str(error))
        output = self.fixture.root / "deliberate-test"
        receipt = self.fixture.sign(output, release_jsons=test_releases,
                                    funnel_receipts={}, allow_test=True)
        self.assertEqual(receipt["configuration"], "Test")
        manifest = json.loads((output / formats.manifest_asset("Win64")).read_bytes())
        self.assertEqual(manifest["target_attestation"]["configuration"], "Test")
        self.assertIn(f"WoT-Win64-guarded-{fixtures.COMMIT}.zip",
                      {path.name for path in output.iterdir()})
        # verify_release flags the Test set unless told it is deliberate.
        with self.assertRaises(verify_release.VerifyError) as caught:
            verify_release.verify_publish_set(
                output, trust=self.fixture.trust, compiled=self.fixture.compiled)
        self.assertIn("FLAGGED Test build", "\n".join(caught.exception.failures))
        mixed = dict(self.fixture.releases)
        mixed["Mac"] = test_releases["Mac"]
        error = self.assertRefused(release_jsons=mixed, allow_test=True,
                                   allow_unproven_funnel=True)
        self.assertIn("different configurations", str(error))

    def test_shipping_warns_when_test_twin_funnel_evidence_is_unproven(self):
        error = self.assertFunnelWarning(funnel_receipts={
            "Win64": self.fixture.funnel_receipts["Win64"]})
        self.assertIn("--funnel-receipt Mac=", str(error))
        releases = self.fixture.releases
        cases = {
            "different Test release.json": {"manifest_sha256": "0" * 64},
            "different commit": {"git_commit": "fedcba987"},
            "different source snapshot": {"source_snapshot_sha256": "1" * 64},
            "different Test zip": {"asset_sha256": "2" * 64},
        }
        for fragment, overrides in cases.items():
            with self.subTest(fragment=fragment):
                receipt = fixtures.write_funnel_receipt(
                    self.fixture.root / "funnel-bad" / f"{len(fragment)}.json",
                    releases["Mac"], **overrides)
                error = self.assertFunnelWarning(funnel_receipts={
                    "Win64": self.fixture.funnel_receipts["Win64"], "Mac": receipt})
                self.assertIn(fragment, str(error))
        for label, options, fragment in (
                ("failed", {"verdict": "fail"}, "verdict is 'fail'"),
                ("skipped host", {"cells": {"host-join": "skip"}},
                 "cell host-join is 'skip'")):
            with self.subTest(label=label):
                receipt = fixtures.write_funnel_receipt(
                    self.fixture.root / "funnel-bad" / f"{label}.json",
                    releases["Win64"], **options)
                error = self.assertFunnelWarning(funnel_receipts={
                    "Win64": receipt, "Mac": self.fixture.funnel_receipts["Mac"]})
                self.assertIn(fragment, str(error))
        wrong_platform = self.assertFunnelWarning(funnel_receipts={
            "Win64": self.fixture.funnel_receipts["Mac"],
            "Mac": self.fixture.funnel_receipts["Mac"]})
        self.assertIn("is not Win64", str(wrong_platform))
        rehearsal = self.fixture.sign(self.fixture.root / "rehearsal",
                                      funnel_receipts={}, allow_unproven_funnel=True)
        self.assertIsNone(rehearsal["platforms"]["Win64"]["funnel_receipt"])

    def test_refuses_a_shipping_image_that_carries_test_seams(self):
        files = fixtures.platform_files("Win64")
        game = "WoT/Binaries/Win64/WoT-Win64-Shipping.exe"
        files[game] = (files[game][0]
                       + "WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL".encode("utf-16-le"),
                       files[game][1])
        seamed = fixtures.write_guarded_release(self.fixture.root / "seamed", "Win64",
                                                files=files)
        funnel = fixtures.write_funnel_receipt(
            self.fixture.root / "seamed-funnel.json", seamed)
        error = self.assertRefused(
            release_jsons={"Win64": seamed, "Mac": self.fixture.releases["Mac"]},
            funnel_receipts={"Win64": funnel, "Mac": self.fixture.funnel_receipts["Mac"]})
        self.assertIn("carries compiled test seams", str(error))

    def test_refuses_a_nonempty_output_and_bad_tags(self):
        occupied = self.fixture.root / "occupied"
        occupied.mkdir()
        (occupied / "stale.json").write_bytes(b"{}")
        with self.assertRaises(sign_release.SignError):
            self.fixture.sign(occupied)
        self.assertTrue((occupied / "stale.json").exists())
        self.assertRefused(tag="latest/../x")

    def test_refuses_a_ring_the_embedded_roots_did_not_sign(self):
        forged = self.fixture.root / "forged-ring"
        with self.assertRaises(sign_release.SignError):
            sign_release.sign_ring(
                trust=self.fixture.trust, openssl=self.fixture.openssl,
                root_key_path=self.fixture.release_key, root_key_id="offline-root-a",
                keys=[formats.ReleaseKey(fixtures.RELEASE_KEY_ID,
                                         self.fixture.release_public, 0,
                                         formats.INT64_MAX)],
                revoked=[], epoch=1, output=forged)
        self.assertFalse((forged / formats.KEY_RING_ASSET).exists())


class SignRingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = fixtures.ReleaseFixture()

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()

    def sign(self, output, **overrides):
        arguments = dict(trust=self.fixture.trust, openssl=self.fixture.openssl,
                         root_key_path=self.fixture.root_key, root_key_id="offline-root-a",
                         keys=[formats.ReleaseKey("release-v2", self.fixture.release_public,
                                                  5, 900)],
                         revoked=["release-v1"], epoch=2, output=output)
        arguments.update(overrides)
        return sign_release.sign_ring(**arguments)

    def test_ring_is_canonical_root_signed_and_deterministic(self):
        first = self.fixture.root / "ring-a"
        second = self.fixture.root / "ring-b"
        receipt = self.sign(first)
        self.sign(second)
        raw = (first / formats.KEY_RING_ASSET).read_bytes()
        self.assertEqual(raw, (second / formats.KEY_RING_ASSET).read_bytes())
        self.assertEqual(raw, formats.canonical_bytes(json.loads(raw)))
        ring = formats.parse_key_ring(json.loads(raw), formats.RELEASE_KEY_RING_FORMAT)
        self.assertEqual((ring.epoch, ring.revoked_key_ids), (2, ("release-v1",)))
        self.assertTrue(crypto.verify_framed(
            self.fixture.root_public, crypto.CONTEXT_RELEASE_KEY_RING_V1, raw,
            (first / (formats.KEY_RING_ASSET + ".sig")).read_bytes()))
        self.assertEqual(receipt["signer"]["key_id"], "offline-root-a")

    def test_ring_refuses_role_reuse_and_unknown_roots(self):
        with self.assertRaises(sign_release.SignError):
            self.sign(self.fixture.root / "reuse", keys=[formats.ReleaseKey(
                "release-v2", self.fixture.announcement_public, 0, 10)])
        with self.assertRaises(sign_release.SignError):
            self.sign(self.fixture.root / "unknown", root_key_id="offline-root-z")
        with self.assertRaises(sign_release.SignError):
            sign_release._parse_key_spec("release=" + "ab" * 32 + ":9:3")
        parsed = sign_release._parse_key_spec("release=" + "ab" * 32)
        self.assertEqual((parsed.first_sequence, parsed.last_sequence),
                         (0, formats.INT64_MAX))

    def test_test_keys_cannot_be_minted_outside_the_temp_directory(self):
        with self.assertRaises(crypto.CryptoError):
            crypto.generate_test_key(self.fixture.openssl,
                                     formats.REPOSITORY_ROOT / "Build" / "should-not-exist.pem")
        self.assertFalse((formats.REPOSITORY_ROOT / "Build" / "should-not-exist.pem").exists())


class CommandLineTests(unittest.TestCase):
    def test_sign_then_verify_through_main(self):
        fixture = fixtures.ReleaseFixture()
        try:
            output = fixture.root / "cli-set"
            receipt = fixture.root / "evidence" / "sign-receipt.json"
            gates = fixture.write_cache_gates(fixture.releases, "cli-set")
            arguments = ["sign", "--release", f"Win64={fixture.releases['Win64'].parent}",
                         "--cache-gate", f"Win64={gates['Win64']}",
                         "--cache-gate", f"Mac={gates['Mac']}",
                         "--release", f"Mac={fixture.releases['Mac']}",
                         "--key-ring", str(fixture.ring_dir / formats.KEY_RING_ASSET),
                         "--announcement-key", str(fixture.announcement_key),
                         "--release-key", str(fixture.release_key),
                         "--release-key-id", fixtures.RELEASE_KEY_ID,
                         "--release-sequence", "0", "--human-version", "0.1.0",
                         "--tag", "v0.1.0", "--output", str(output),
                         "--receipt", str(receipt), "--openssl", fixture.openssl,
                         "--funnel-receipt", f"Win64={fixture.funnel_receipts['Win64']}",
                         "--funnel-receipt", f"Mac={fixture.funnel_receipts['Mac']}",
                         *fixture.trust_arguments()]
            with mock.patch.object(verify_release.mac_dmg, "verify_dmg"), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(sign_release.main(arguments), 0)
            document = json.loads(receipt.read_text(encoding="utf-8"))
            self.assertEqual(document["format"], sign_release.SIGN_RECEIPT_FORMAT)
            verify_receipt = fixture.root / "evidence" / "verify-receipt.json"
            with mock.patch.object(verify_release.mac_dmg, "verify_dmg"), \
                    contextlib.redirect_stdout(io.StringIO()):
                verified = verify_release.main(
                    ["--set", str(output), "--receipt", str(verify_receipt),
                     "--release", f"Win64={fixture.releases['Win64']}",
                     "--release", f"Mac={fixture.releases['Mac']}",
                     "--openssl-cross-check", "--openssl", fixture.openssl,
                     *fixture.trust_arguments()])
            self.assertEqual(verified, 0)
            self.assertEqual(json.loads(verify_receipt.read_text())["verdict"], "pass")
            # Repeating into the same directory is refused, not merged.
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(sign_release.main(arguments), 1)
            # --cache-gate is a required input of every signing.
            ungated = [argument for index, argument in enumerate(arguments)
                       if argument != "--cache-gate"
                       and (index == 0 or arguments[index - 1] != "--cache-gate")]
            ungated[ungated.index("--output") + 1] = str(fixture.root / "cli-ungated")
            with contextlib.redirect_stderr(io.StringIO()) as errors, \
                    self.assertRaises(SystemExit):
                sign_release.main(ungated)
            self.assertIn("--cache-gate", errors.getvalue())
            self.assertFalse((fixture.root / "cli-ungated").exists())
        finally:
            fixture.cleanup()


if __name__ == "__main__":
    unittest.main()
