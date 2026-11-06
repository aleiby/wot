#!/usr/bin/env python3
"""Specs for publish_release.py. NOTHING here reaches GitHub.

Every command goes through FakeRunner, which answers from a script and raises on
anything unscripted; the gh executable is named `gh-never-runs` so even a
runner bug could not invoke the real CLI. The git answers are scripted too, so
the worktree's actual state never matters.
"""
from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import types
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import release_test_fixtures as fixtures  # noqa: E402
import publish_release  # noqa: E402

GH = "gh-never-runs"
HEAD = "0a1b2c3d4" + "e" * 31


class FakeRunner:
    def __init__(self, *, dirty=False, head=HEAD, tag_exists=False, uploaded=None,
                 previous=None, previous_notes=None):
        self.calls = []
        self.dirty = dirty
        self.head = head
        self.tag_exists = tag_exists
        self.uploaded = uploaded
        self.previous = previous or {}
        self.previous_notes = previous_notes
        self.created_release_notes = None
        self.created_release_notes_asset = None
        self.created_player_downloads = {}

    def __call__(self, command):
        command = list(command)
        self.calls.append(command)

        def done(code=0, stdout="", stderr=""):
            return subprocess.CompletedProcess(command, code, stdout, stderr)

        if command[0] == "git":
            verb = command[3]
            if verb == "status":
                return done(stdout=" M Source/x.cpp\n" if self.dirty else "")
            if verb == "rev-parse" and command[4] == "HEAD":
                return done(stdout=self.head + "\n")
            if verb == "rev-parse":   # --verify <build_commit>^{commit}
                return done(stdout=HEAD + "\n")
        if command[0] == GH:
            verb = command[1:3]
            if verb == ["auth", "status"]:
                return done()
            if verb == ["release", "view"] and "tagName" == command[-1]:
                return done(0 if self.tag_exists else 1,
                            stderr="" if self.tag_exists else "release not found")
            if verb == ["release", "download"]:
                target = Path(command[command.index("--dir") + 1])
                pattern = command[command.index("--pattern") + 1]
                if pattern == formats.RELEASE_NOTES_ASSET:
                    if self.previous_notes is None:
                        return done(1, stderr="release not found")
                    (target / formats.RELEASE_NOTES_ASSET).write_bytes(
                        self.previous_notes)
                    return done()
                for name, data in self.previous.items():
                    (target / name).write_bytes(data)
                return done(0 if self.previous else 1,
                            stderr="" if self.previous else "release not found")
            if verb == ["release", "create"]:
                notes = Path(command[command.index("--notes-file") + 1])
                self.created_release_notes = notes.read_text(encoding="utf-8")
                asset = next((Path(value) for value in command
                              if Path(value).name == formats.RELEASE_NOTES_ASSET), None)
                if asset is not None:
                    self.created_release_notes_asset = asset.read_bytes()
                for value in command:
                    path = Path(value)
                    if path.name in formats.PLAYER_DOWNLOAD_ASSETS.values():
                        self.created_player_downloads[path.name] = path.read_bytes()
                return done()
            if verb == ["release", "view"]:
                return done(stdout=json.dumps({"assets": self.uploaded, "isDraft": True,
                                               "tagName": command[3]}))
            if verb == ["release", "edit"]:
                return done()
        raise AssertionError(f"unscripted command {command}")

    def gh_verbs(self):
        return [" ".join(call[1:3]) for call in self.calls if call[0] == GH]


class PublishReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dmg_inspection = mock.patch.object(
            publish_release.verify_release.mac_dmg, "verify_dmg")
        cls.dmg_inspection.start()
        cls.fixture = fixtures.ReleaseFixture()
        cls.set = cls.fixture.root / "set"
        cls.fixture.sign(cls.set)
        cls.repository = cls.fixture.root / "repo"
        cls.notes_file = cls.fixture.root / "notes.md"
        cls.notes_file.write_bytes(
            b"- Added one thing.  \r\n- Fixed another.\r\n")
        cls.assets = [{"name": path.name, "size": path.stat().st_size,
                       "digest": "sha256:" + formats.sha256_file(path)[0]}
                      for path in sorted(cls.set.iterdir())]
        cls.empty_notes = formats.merge_release_notes(
            None, 0, "- Added one thing.\n- Fixed another.")
        cls.notes_row = {
            "name": formats.RELEASE_NOTES_ASSET,
            "size": len(cls.empty_notes),
            "digest": "sha256:" + __import__("hashlib").sha256(
                cls.empty_notes).hexdigest(),
        }
        cls.player_sources = {
            formats.PLAYER_DOWNLOAD_ASSETS["Win64"]:
                cls.set / formats.guarded_install_zip_asset(
                    "Win64", fixtures.COMMIT, formats.SHIPPING_CONFIGURATION),
            formats.PLAYER_DOWNLOAD_ASSETS["Mac"]:
                cls.set / formats.guarded_install_dmg_asset(
                    fixtures.COMMIT, formats.SHIPPING_CONFIGURATION),
        }
        cls.player_rows = [
            {"name": fixed_name, "size": source.stat().st_size,
             "digest": "sha256:" + formats.sha256_file(source)[0]}
            for fixed_name, source in sorted(cls.player_sources.items())]
        cls.union_assets = [*cls.assets, *cls.player_rows, cls.notes_row]
        cls.previous_directory = cls.fixture.root / "previous-zero"
        cls.previous_directory.mkdir()
        for platform in formats.PLATFORMS:
            for suffix in ("", formats.SIGNATURE_SUFFIX):
                name = formats.announcement_asset(platform) + suffix
                (cls.previous_directory / name).write_bytes((cls.set / name).read_bytes())

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()
        cls.dmg_inspection.stop()

    def run_main(self, runner, *extra, directory=None):
        receipt = self.fixture.root / "receipts" / f"{len(runner.calls)}-{id(runner)}.json"
        release_arguments = (() if "--release" in extra else (
            "--release", f"Mac={self.fixture.releases['Mac']}"))
        arguments = ["--set", str(directory or self.set), "--human-version", "0.1.0",
                     "--notes-file", str(self.notes_file),
                     "--gh", GH, "--receipt", str(receipt),
                     "--repository-root", str(self.repository),
                     *self.fixture.trust_arguments(),
                     *release_arguments,
                     *extra]
        with contextlib.redirect_stdout(io.StringIO()) as output, \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            code = publish_release.main(arguments, runner=runner)
        return code, output.getvalue() + errors.getvalue(), json.loads(
            receipt.read_text(encoding="utf-8"))

    def test_release_notes_carry_install_steps(self):
        commit = "0" * 40
        sources = {"Mac": f"WoT-Mac-guarded-shipping-{commit}.dmg",
                   "Win64": f"WoT-Win64-guarded-shipping-{commit}.zip"}

        def verified(platforms, *, notarized=False):
            return types.SimpleNamespace(
                platforms={name: types.SimpleNamespace(
                    configuration=formats.SHIPPING_CONFIGURATION,
                    dmg_notarized=(notarized if name == "Mac" else None),
                    install_zip=(sources[name] if name == "Win64" else None),
                    dmg=(sources[name] if name == "Mac" else None))
                           for name in platforms},
                release_sequence=0, build_commit=commit,
                assets={sources[name]: {"size": 1, "sha256": "0" * 64}
                        for name in platforms})
        mac_notes = publish_release.release_notes(
            verified(("Mac", "Win64")), "0.1.0",
            "- Added one thing.\n- Fixed another.")
        self.assertTrue(mac_notes.startswith(
            "## What's new\n\n<!-- wot-whats-new:start -->\n"
            "- Added one thing.\n- Fixed another.\n"
            "<!-- wot-whats-new:end -->\n\nWoT Reborn 0.1.0"))
        # One download per platform, named; GitHub already lists every asset.
        self.assertIn("- **Mac:** WoT-Reborn-Mac.dmg", mac_notes)
        self.assertIn("- **Windows:** WoT-Reborn-Windows.zip", mac_notes)
        self.assertNotIn("| asset |", mac_notes)
        self.assertNotIn("0" * 64, mac_notes)
        self.assertNotIn("guarded-shipping", mac_notes)
        self.assertIn("https://github.com/aleiby/wot#playing", mac_notes)
        # The README is the one place that names a store.
        self.assertNotIn("gog", mac_notes.casefold())
        self.assertIn("Open WoT-Reborn-Mac.dmg", mac_notes)
        normalized_mac_notes = " ".join(mac_notes.split())
        self.assertIn(
            "drag WoT.app to Applications",
            normalized_mac_notes,
        )
        self.assertIn(
            "finds the installer in your Downloads folder by itself",
            normalized_mac_notes,
        )
        for forbidden in (
                "next to the game", "inside that folder", "beside WoT.app",
                "place the installer", "put the installer"):
            self.assertNotIn(forbidden, normalized_mac_notes.casefold())
        self.assertIn("Open Anyway", mac_notes)
        self.assertIn("Always Allow", mac_notes)
        self.assertIn("Run anyway", mac_notes)
        win_only = publish_release.release_notes(
            verified(("Win64",)), "0.1.0", "notes")
        self.assertIn("https://github.com/aleiby/wot#playing", win_only)
        self.assertNotIn("gog", win_only.casefold())
        self.assertNotIn("Open Anyway", win_only)
        self.assertIn(r"WoT Reborn\WoT Reborn.exe", win_only)
        self.assertNotIn("**Mac:**", win_only)
        self.assertIn("Run anyway", win_only)
        mac_only = publish_release.release_notes(
            verified(("Mac",)), "0.1.0", "notes")
        self.assertNotIn("Run anyway", mac_only)
        notarized_notes = publish_release.release_notes(
            verified(("Mac",), notarized=True), "0.1.0", "notes")
        self.assertNotIn("Open Anyway", notarized_notes)
        self.assertNotIn("not notarized", notarized_notes)
        self.assertIn("Always Allow", notarized_notes)
        # A deliberate Test publication has no fixed-name copies, so its notes
        # name the commit-named DMG it does carry.
        test_sources = {"Mac": f"WoT-Mac-guarded-{commit}.dmg",
                        "Win64": f"WoT-Win64-guarded-{commit}.zip"}
        test_set = types.SimpleNamespace(
            platforms={name: types.SimpleNamespace(
                configuration="Test",
                dmg_notarized=(True if name == "Mac" else None),
                install_zip=(test_sources[name] if name == "Win64" else None),
                dmg=(test_sources[name] if name == "Mac" else None))
                       for name in ("Mac", "Win64")},
            release_sequence=0, build_commit=commit,
            assets={name: {"size": 1, "sha256": "0" * 64}
                    for name in test_sources.values()})
        test_notes = publish_release.release_notes(test_set, "0.1.0", "notes")
        self.assertIn(f"Open WoT-Mac-guarded-{commit}.dmg", test_notes)
        self.assertNotIn("WoT-Reborn-", test_notes)

    def test_dry_run_only_reads_git_and_prints_the_plan(self):
        runner = FakeRunner()
        code, text, receipt = self.run_main(runner)
        self.assertEqual(code, 0, text)
        self.assertEqual(runner.gh_verbs(), ["release download"])
        self.assertIn("DRY RUN", text)
        self.assertIn("release create v0.1.0", text)
        self.assertIn("--draft", text)
        self.assertIn("WoT-Reborn-Mac.dmg", text)
        self.assertIn("WoT-Reborn-Windows.zip", text)
        self.assertIn("qualify_announcement_transport.py", text)
        self.assertEqual((receipt["mode"], receipt["outcome"]), ("dry-run", "planned"))

    def test_sequence_zero_dry_run_carries_forward_the_previous_asset(self):
        previous = formats.merge_release_notes(None, 4, "older notes")
        runner = FakeRunner(previous_notes=previous)
        code, text, receipt = self.run_main(runner)
        self.assertEqual(code, 0, text)
        self.assertEqual(runner.gh_verbs(), ["release download"])
        self.assertEqual(receipt["release_notes_asset"]["entry_sequences"], [4])
        self.assertEqual(receipt["release_notes_asset"]["size"], len(previous))

    def test_malformed_previous_notes_asset_refuses_the_dry_run(self):
        runner = FakeRunner(previous_notes=b'{"format":"wrong","notes":[]}')
        code, text, receipt = self.run_main(runner)
        self.assertEqual(code, 1)
        self.assertIn("format is not", text)
        self.assertEqual(receipt["outcome"], "failed")
        self.assertNotIn("release create", runner.gh_verbs())

    def test_sequence_zero_publish_requires_bound_mac_release(self):
        runner = FakeRunner()
        arguments = [
            "--set", str(self.set), "--human-version", "0.1.0", "--gh", GH,
            "--notes-file", str(self.notes_file),
            "--repository-root", str(self.repository),
            *self.fixture.trust_arguments(),
        ]
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            code = publish_release.main(arguments, runner=runner)
        self.assertEqual(code, 1)
        self.assertIn("requires --release Mac=", errors.getvalue())
        self.assertEqual(runner.calls, [])

    def test_refuses_dirty_trees_and_the_wrong_commit(self):
        for runner, fragment in ((FakeRunner(dirty=True), "not clean"),
                                 (FakeRunner(head="f" * 40), "not the release commit")):
            code, text, receipt = self.run_main(runner, "--publish", "--confirm-tag",
                                                "v0.1.0")
            self.assertEqual(code, 1)
            self.assertIn(fragment, text)
            self.assertEqual(runner.gh_verbs(), [])
            self.assertEqual(receipt["outcome"], "refused")

    def test_publish_requires_the_tag_to_be_confirmed(self):
        runner = FakeRunner()
        code, text, _ = self.run_main(runner, "--publish")
        self.assertEqual(code, 1)
        self.assertIn("--confirm-tag v0.1.0", text)
        self.assertEqual(runner.gh_verbs(), [])

    def test_refuses_an_unverifiable_set_before_any_command(self):
        broken = fixtures.copy_set(self.set, self.fixture.root / "broken")
        (broken / (formats.announcement_asset("Mac") + ".sig")).write_bytes(bytes(64))
        runner = FakeRunner()
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            code = publish_release.main(
                ["--set", str(broken), "--human-version", "0.1.0", "--gh", GH, "--publish",
                 "--notes-file", str(self.notes_file),
                 "--confirm-tag", "v0.1.0", *self.fixture.trust_arguments()], runner=runner)
        self.assertEqual(code, 1)
        self.assertEqual(runner.calls, [])
        self.assertIn("does not verify", errors.getvalue())

    def test_refuses_other_repositories(self):
        runner = FakeRunner()
        with contextlib.redirect_stderr(io.StringIO()):
            code = publish_release.main(["--set", str(self.set), "--human-version", "x",
                                         "--notes-file", str(self.notes_file),
                                         "--repo", "someone/fork", "--gh", GH,
                                         *self.fixture.trust_arguments()], runner=runner)
        self.assertEqual((code, runner.calls), (1, []))

    def test_publish_creates_a_draft_checks_it_then_flips_latest(self):
        runner = FakeRunner(uploaded=self.union_assets)
        code, text, receipt = self.run_main(runner, "--publish", "--confirm-tag", "v0.1.0")
        self.assertEqual(code, 0, text)
        self.assertEqual(runner.gh_verbs(), ["release download", "auth status", "release view",
                                             "release download", "release create",
                                             "release view", "release edit"])
        create = next(call for call in runner.calls if call[1:3] == ["release", "create"])
        self.assertIn("--draft", create)
        self.assertEqual(create[create.index("--target") + 1], HEAD)
        self.assertTrue(runner.created_release_notes.startswith(
            "## What's new\n\n<!-- wot-whats-new:start -->\n"
            "- Added one thing.\n- Fixed another.\n"
            "<!-- wot-whats-new:end -->\n"))
        self.assertEqual(runner.created_release_notes_asset, self.empty_notes)
        self.assertEqual(runner.created_player_downloads, {
            name: source.read_bytes()
            for name, source in self.player_sources.items()
        })
        self.assertIn(formats.RELEASE_NOTES_ASSET,
                      [Path(value).name for value in create])
        self.assertEqual(sorted(Path(arg).name for arg in create
                                if arg.startswith(str(self.set))),
                         sorted(asset["name"] for asset in self.assets))
        # The signed cache lineage pairs ride along with the verified assets.
        for platform in formats.PLATFORMS:
            for suffix in ("", formats.SIGNATURE_SUFFIX):
                self.assertIn(str(self.set / (formats.cache_lineage_asset(platform)
                                              + suffix)), create)
        self.assertEqual(
            sorted(set(Path(arg).name for arg in create)
                   & set(formats.PLAYER_DOWNLOAD_ASSETS.values())),
            sorted(formats.PLAYER_DOWNLOAD_ASSETS.values()))
        edit = runner.calls[-1]
        self.assertEqual(edit[-2:], ["--draft=false", "--latest"])
        self.assertEqual(receipt["outcome"], "published")
        self.assertEqual(receipt["player_downloads"], {
            fixed_name: {
                "source": source.name,
                "sha256": formats.sha256_file(source)[0],
                "size": source.stat().st_size,
            }
            for fixed_name, source in self.player_sources.items()
        })

    def test_public_target_tags_the_export_commit_after_proving_the_release(self):
        public = "ab" * 20
        runner = FakeRunner(uploaded=self.union_assets)
        code, text, receipt = self.run_main(runner, "--publish", "--confirm-tag", "v0.1.0",
                                            "--public-target", public)
        self.assertEqual(code, 0, text)
        create = next(call for call in runner.calls if call[1:3] == ["release", "create"])
        self.assertEqual(create[create.index("--target") + 1], public)
        self.assertEqual(receipt["outcome"], "published")

    def test_publish_copies_player_downloads_when_hard_links_are_unavailable(self):
        runner = FakeRunner(uploaded=self.union_assets)
        with mock.patch.object(publish_release.os, "link", side_effect=OSError):
            code, text, receipt = self.run_main(
                runner, "--publish", "--confirm-tag", "v0.1.0")
        self.assertEqual(code, 0, text)
        self.assertEqual(runner.created_player_downloads, {
            name: source.read_bytes()
            for name, source in self.player_sources.items()
        })
        self.assertEqual(receipt["outcome"], "published")

    def test_public_target_still_requires_the_release_commit_at_head(self):
        runner = FakeRunner(head="0" * 40)
        code, text, receipt = self.run_main(runner, "--public-target", "ab" * 20)
        self.assertEqual(code, 1)
        self.assertEqual(receipt["outcome"], "refused")

    def test_public_target_must_be_a_full_sha(self):
        runner = FakeRunner()
        code, text, receipt = self.run_main(runner, "--public-target", "main")
        self.assertEqual(code, 1)
        self.assertIn("--public-target must be a full lowercase commit sha", text)

    def test_a_mismatched_upload_is_never_made_latest(self):
        tampered = [dict(asset) for asset in self.union_assets]
        tampered[0]["size"] += 1
        for uploaded in (tampered, self.union_assets[1:]):
            runner = FakeRunner(uploaded=uploaded)
            code, text, receipt = self.run_main(runner, "--publish", "--confirm-tag",
                                                "v0.1.0")
            self.assertEqual(code, 1)
            self.assertNotIn("release edit", runner.gh_verbs())
            self.assertIn("left unpublished", text)
            self.assertEqual(receipt["outcome"], "failed")

    def test_a_mismatched_player_copy_is_never_made_latest(self):
        fixed_name = formats.PLAYER_DOWNLOAD_ASSETS["Mac"]
        fixed_index = next(index for index, row in enumerate(self.union_assets)
                           if row["name"] == fixed_name)
        for field, value in (("size", self.union_assets[fixed_index]["size"] + 1),
                             ("digest", "sha256:" + "f" * 64)):
            with self.subTest(field=field):
                uploaded = [dict(asset) for asset in self.union_assets]
                uploaded[fixed_index][field] = value
                runner = FakeRunner(uploaded=uploaded)
                code, text, receipt = self.run_main(
                    runner, "--publish", "--confirm-tag", "v0.1.0")
                self.assertEqual(code, 1)
                self.assertNotIn("release edit", runner.gh_verbs())
                self.assertIn(f"uploaded {fixed_name} differs", text)
                self.assertEqual(receipt["outcome"], "failed")

    def test_test_configuration_adds_no_fixed_player_downloads(self):
        releases = {
            platform: fixtures.write_guarded_release(
                self.fixture.root / f"publish-test-{platform}", platform,
                configuration=formats.TEST_CONFIGURATION)
            for platform in formats.PLATFORMS
        }
        funnels = {
            platform: fixtures.write_funnel_receipt(
                self.fixture.root / "publish-test-funnel" / platform
                / "funnel-receipt.json", release)
            for platform, release in releases.items()
        }
        release_set = self.fixture.root / "publish-test-set"
        self.fixture.sign(release_set, release_jsons=releases,
                          funnel_receipts=funnels, allow_test=True)
        test_assets = [
            {"name": path.name, "size": path.stat().st_size,
             "digest": "sha256:" + formats.sha256_file(path)[0]}
            for path in sorted(release_set.iterdir())]
        runner = FakeRunner(uploaded=[*test_assets, self.notes_row])
        code, text, receipt = self.run_main(
            runner, "--allow-test", "--release", f"Mac={releases['Mac']}",
            "--publish", "--confirm-tag", "v0.1.0", directory=release_set)
        self.assertEqual(code, 0, text)
        self.assertEqual(runner.created_player_downloads, {})
        self.assertEqual(receipt["player_downloads"], {})

    def test_existing_tag_or_rollback_stops_before_create(self):
        runner = FakeRunner(tag_exists=True, uploaded=self.union_assets)
        code, text, _ = self.run_main(runner, "--publish", "--confirm-tag", "v0.1.0")
        self.assertEqual(code, 1)
        self.assertIn("already exists", text)
        self.assertNotIn("release create", runner.gh_verbs())

        # A currently-published, authentic announcement at sequence 9 makes this
        # sequence-0 set a rollback.
        previous = {}
        for platform in formats.PLATFORMS:
            document = json.loads((self.set / formats.announcement_asset(platform))
                                  .read_bytes())
            document["release_sequence"] = 9
            raw = formats.canonical_bytes(document)
            previous[formats.announcement_asset(platform)] = raw
            previous[formats.announcement_asset(platform) + ".sig"] = crypto.sign_framed(
                self.fixture.openssl, self.fixture.announcement_key,
                crypto.CONTEXT_ANNOUNCEMENT_V1, raw, self.fixture.announcement_public)
        runner = FakeRunner(uploaded=self.union_assets, previous=previous)
        code, text, _ = self.run_main(runner, "--publish", "--confirm-tag", "v0.1.0")
        self.assertEqual(code, 1)
        self.assertIn("rollback", text)
        self.assertNotIn("release create", runner.gh_verbs())

    def test_check_remote_dry_run_is_read_only(self):
        runner = FakeRunner()
        code, text, receipt = self.run_main(runner, "--check-remote")
        self.assertEqual(code, 0, text)
        self.assertEqual(runner.gh_verbs(), ["release download", "auth status", "release view",
                                             "release download"])
        self.assertEqual(receipt["mode"], "dry-run")

    def test_positive_sequence_publish_warns_without_funnel_evidence(self):
        release_set = self.fixture.root / "publish-sequence-one"
        self.fixture.sign(release_set, release_sequence=1, tag="v0.1.1")
        releases = {
            platform: self.fixture.root / f"sequence-1-{platform}" / "release.json"
            for platform in formats.PLATFORMS}
        funnels = {
            platform: self.fixture.root / "sequence-1-funnel" / platform
            / "funnel-receipt.json"
            for platform in formats.PLATFORMS}
        runner = FakeRunner()
        arguments = ["--set", str(release_set), "--human-version", "0.1.0",
                     "--notes-file", str(self.notes_file),
                     "--gh", GH, "--repository-root", str(self.repository),
                     *self.fixture.trust_arguments()]
        with contextlib.redirect_stdout(io.StringIO()) as output, \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            code = publish_release.main(arguments, runner=runner)
        text = output.getvalue() + errors.getvalue()
        self.assertEqual(code, 1)
        self.assertIn("WARNING: Win64", text)

        evidence_args = [item for platform in formats.PLATFORMS for item in (
            "--release", f"{platform}={releases[platform]}",
            "--funnel-receipt", f"{platform}={funnels[platform]}")]
        evidence_args += ["--previous", str(self.previous_directory)]
        previous = {
            formats.announcement_asset(platform) + suffix:
                (self.set / (formats.announcement_asset(platform) + suffix)).read_bytes()
            for platform in formats.PLATFORMS
            for suffix in ("", formats.SIGNATURE_SUFFIX)
        }
        runner = FakeRunner(previous=previous)
        code, text, receipt = self.run_main(
            runner, *evidence_args, "--check-remote", directory=release_set)
        self.assertEqual(code, 0, text)
        self.assertEqual(receipt["set"]["sequence_gate"]["mode"],
                         "task7-update-funnel")
        self.assertEqual(runner.gh_verbs(), ["release download", "auth status", "release view",
                                             "release download"])

        win64_names = [formats.announcement_asset("Win64") + suffix
                       for suffix in ("", formats.SIGNATURE_SUFFIX)]
        for incomplete in ({}, {name: previous[name] for name in win64_names}):
            with self.subTest(previous=sorted(incomplete)):
                runner = FakeRunner(previous=incomplete)
                code, text, receipt = self.run_main(
                    runner, *evidence_args, "--check-remote", directory=release_set)
                self.assertEqual(code, 1)
                self.assertIn("positive release remote preflight requires authenticated "
                              "previous announcements for Win64 and Mac", text)
                self.assertEqual(receipt["outcome"], "failed")
                self.assertNotIn("release create", runner.gh_verbs())

        runner = FakeRunner()
        code, text, receipt = self.run_main(
            runner, "--allow-staging-sequence", "--release",
            f"Mac={releases['Mac']}", "--previous",
            str(self.previous_directory), directory=release_set)
        self.assertEqual(code, 0, text)
        self.assertEqual(receipt["set"]["sequence_gate"], {
            "mode": "unproven", "platforms": {},
        })

    def test_positive_sequence_plan_uploads_the_cache_lineage(self):
        release_set = self.fixture.root / "publish-lineage-sequence-one"
        self.fixture.sign(release_set, release_sequence=1, tag="v0.1.1-lineage")
        verify = publish_release.verify_release.verify_publish_set
        previous = {
            platform: result.announcement for platform, result in verify(
                self.set, trust=self.fixture.trust, compiled=self.fixture.compiled,
                releases={"Mac": self.fixture.releases["Mac"]}).platforms.items()}
        with contextlib.redirect_stderr(io.StringIO()):
            verified = verify(
                release_set, trust=self.fixture.trust, compiled=self.fixture.compiled,
                previous=previous,
                releases={"Mac": self.fixture.root / "sequence-1-Mac" / "release.json"})
        plan = publish_release.build_plan(
            verified, target=HEAD, repo=formats.REPOSITORY, title=None,
            human_version="0.1.1", notes_text="- Added one thing.")
        names = {path.name for path in plan.assets}
        for platform in formats.PLATFORMS:
            for suffix in ("", formats.SIGNATURE_SUFFIX):
                self.assertIn(formats.cache_lineage_asset(platform) + suffix, names)
        self.assertEqual(names, {path.name for path in release_set.iterdir()})

    def test_publish_refuses_replayed_sequence_receipt_and_test_twin(self):
        release_set = self.fixture.root / "publish-replay-sequence-two"
        self.fixture.sign(release_set, release_sequence=2, tag="v0.1.2")
        sequence_one = {
            platform: fixtures.write_guarded_release(
                self.fixture.root / f"publish-replay-one-{platform}", platform,
                release_sequence=1)
            for platform in formats.PLATFORMS}
        evidence = []
        for platform in formats.PLATFORMS:
            old = json.loads(sequence_one[platform].read_text(encoding="utf-8"))
            replayed = fixtures.write_guarded_release(
                self.fixture.root / f"publish-replay-two-{platform}", platform,
                release_sequence=2, test_twin=old["test_twin"])
            funnel = fixtures.write_funnel_receipt(
                self.fixture.root / f"publish-replay-one-{platform}-funnel.json",
                sequence_one[platform], include_update_cells=True)
            evidence.extend(("--release", f"{platform}={replayed}",
                             "--funnel-receipt", f"{platform}={funnel}"))
        runner = FakeRunner()
        arguments = [
            "--set", str(release_set), "--human-version", "0.1.0",
            "--notes-file", str(self.notes_file),
            "--gh", GH, "--repository-root", str(self.repository),
            *self.fixture.trust_arguments(), *evidence,
            "--previous", str(self.previous_directory),
        ]
        with contextlib.redirect_stdout(io.StringIO()) as output, \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            code = publish_release.main(arguments, runner=runner)
        text = output.getvalue() + errors.getvalue()
        self.assertEqual(code, 1)
        self.assertIn("test_twin release_sequence", text)
        self.assertEqual(runner.calls, [])


if __name__ == "__main__":
    unittest.main()
