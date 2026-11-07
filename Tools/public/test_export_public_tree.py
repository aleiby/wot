import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import unittest
from unittest import mock
from uuid import uuid4

from Tools.public import export_public_tree as exporter


SCRIPT = Path(__file__).with_name("export_public_tree.py")


class PublicTreeExportTests(unittest.TestCase):
    @staticmethod
    def _remove_test_tree(root):
        def make_writable_and_retry(function, path, _exception):
            os.chmod(path, stat.S_IWRITE)
            function(path)

        shutil.rmtree(root, onerror=make_writable_and_retry)

    def setUp(self):
        temporary_root = Path(__file__).resolve().parents[2] / "Intermediate"
        temporary_root.mkdir(exist_ok=True)
        self.root = temporary_root / f"PublicExportTests-{uuid4().hex}"
        self.root.mkdir()
        self.addCleanup(self._remove_test_tree, self.root)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self._git("init", "--quiet")
        self._git("config", "user.name", "Public Export Test")
        self._git("config", "user.email", "public-export@example.invalid")

    def _git(self, *arguments, input_bytes=None):
        return subprocess.run(
            ["git", *arguments],
            cwd=self.repo,
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )

    def _write(self, relative, data):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, str):
            path.write_bytes(data.encode("utf-8"))
        else:
            path.write_bytes(data)

    def _commit(self):
        self._git("add", "--all")
        tree = self._git("write-tree").stdout.decode().strip()
        paths = exporter.propose_paths(self.repo, tree)
        self._write(exporter.MANIFEST_PATH, "".join(path + "\n" for path in paths))
        self._git("add", exporter.MANIFEST_PATH)
        self._git("commit", "--quiet", "-m", "fixture")

    def _export(self, target, environment=None, *, index=False):
        env = os.environ.copy()
        if environment:
            env.update(environment)
        arguments = [sys.executable, str(SCRIPT), "--repo", str(self.repo)]
        if index:
            arguments.append("--index")
        arguments.append(str(target))
        return subprocess.run(
            arguments,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def test_hostile_git_paths_are_denied_before_destination_join(self):
        for path in (
            "../Source/escape.cpp",
            "/Source/absolute.cpp",
            "Source\\windows-separator.cpp",
            "Source/C:drive-component.cpp",
            "Source//empty-component.cpp",
            "Source/./dot-component.cpp",
            "Source/name\nwith-control.cpp",
            "Source/CON",
            "Source/trailing. ",
            "Source/question?.cpp",
            "Source/.git/config",
        ):
            with self.subTest(path=path):
                self.assertTrue(exporter._is_denied(path))

    def test_private_migration_expectations_are_denied(self):
        for path in (
            "Tools/migration/retail_builder_expectations.py",
            "Tools/migration/test_retail_builder_expectations.py",
            "Tools/migration/retail-builder-expectations.json",
        ):
            with self.subTest(path=path):
                self.assertTrue(exporter._is_denied(path))
                self.assertFalse(exporter._is_proposal_candidate(path)
                                 and not exporter._is_denied(path))

    def test_exports_allowlisted_head_bytes_with_sha256_receipt(self):
        self._write("Source/Keep.cpp", "committed source\n")
        self._write("Tools/helper.py", "print('tracked')\n")
        self._write("Build/Mac/Resources/Info.Template.plist", "<plist/>\n")
        release_files = (
            "announcement-public-key-v1.json",
            "bootstrap-trust-v1.json",
            "cache-contract-v3-production.json",
            "cache-v3-production-descriptor.json",
            "public-root-ring.json",
            "release-identity.json",
            "release-ring/WoT-root-key-ring-v1.json",
            "release-ring/WoT-root-key-ring-v1.json.sig",
            "release-version.json",
            "schemas/announcement-v1.schema.json",
            "schemas/component-identity-v1.schema.json",
            "schemas/full-package-v1.schema.json",
            "schemas/installed-tree-v1.schema.json",
            "schemas/key-ring-v1.schema.json",
            "schemas/recovery-domain-v1.schema.json",
            "schemas/release-manifest-v2.schema.json",
        )
        for relative in release_files:
            self._write("Build/WoTRelease/" + relative, relative + "\n")
        self._write("Build/WoTRelease/KEY-CUSTODY.md", "secret handling\n")
        self._write("Build/WoTRelease/test-keys/README.md", "test keys\n")
        self._write("Build/WoTRelease/ReleaseNotes.txt", "- New release.\n")
        self._write("Build/WoTRelease/private.json", "{}\n")
        self._write("Build/WoTRelease/eos-credentials.json", "{}\n")
        self._write("Config/DefaultGame.ini", "[Game]\n")
        self._write("EnginePatches/UE-5.8/fix.patch", "patch\n")
        self._write("EnginePatches/UE-5.8/manifest.json", "patch metadata\n")
        self._write("EnginePatches/UE-5.8/source-toolchain.json", "toolchain pins\n")
        self._write("ThirdParty/innoextract/README.md", "vendor\n")
        self._write("WoT.uproject", b"project\x00bytes")
        self._write("PORT_NOTES.md", "notes\n")
        self._write("docs/LOCAL-CONFIGURATION.md", "local setup\n")
        self._write("docs/BUILD-PATHS.md", "build paths\n")
        self._write("HANDOFF-private.md", "not in the phase-five allowlist\n")
        self._commit()

        self._write("Source/Keep.cpp", "dirty working-tree source\n")
        self._write("Source/untracked.cpp", "untracked\n")
        target = self.root / "public"
        target.mkdir()

        completed = self._export(target)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            (target / "Source/Keep.cpp").read_bytes(), b"committed source\n")
        self.assertEqual(
            (target / "Tools/helper.py").read_bytes(), b"print('tracked')\n")
        self.assertEqual(
            (target / "WoT.uproject").read_bytes(), b"project\x00bytes")
        self.assertFalse((target / "HANDOFF-private.md").exists())
        self.assertFalse((target / "Source/untracked.cpp").exists())
        self.assertFalse((target / "Build/WoTRelease/private.json").exists())
        self.assertFalse((target / "Build/WoTRelease/eos-credentials.json").exists())
        self.assertFalse((target / "Build/WoTRelease/KEY-CUSTODY.md").exists())
        self.assertFalse((target / "Build/WoTRelease/test-keys").exists())
        self.assertFalse((target / "Build/WoTRelease/ReleaseNotes.txt").exists())
        self.assertFalse((target / "EnginePatches/UE-5.8/fix.patch").exists())
        self.assertEqual(
            (target / "EnginePatches/UE-5.8/manifest.json").read_text(),
            "patch metadata\n")

        receipt = json.loads(
            (target / "PUBLIC_EXPORT_RECEIPT.json").read_text(encoding="utf-8"))
        expected_paths = [
            "Build/Mac/Resources/Info.Template.plist",
            *["Build/WoTRelease/" + relative for relative in release_files],
            "Config/DefaultGame.ini",
            "EnginePatches/UE-5.8/manifest.json",
            "EnginePatches/UE-5.8/source-toolchain.json",
            "PORT_NOTES.md",
            "Source/Keep.cpp",
            "ThirdParty/innoextract/README.md",
            "Tools/helper.py",
            "Tools/public/public-files.txt",
            "WoT.uproject",
            "docs/BUILD-PATHS.md",
            "docs/LOCAL-CONFIGURATION.md",
        ]
        self.assertEqual(receipt["paths"], expected_paths)
        self.assertEqual(receipt["file_count"], len(expected_paths))
        self.assertEqual(
            receipt["files"],
            [
                {
                    "mode": "100644",
                    "path": path,
                    "sha256": hashlib.sha256((target / path).read_bytes()).hexdigest(),
                }
                for path in expected_paths
            ],
        )
        self.assertNotIn("complete", receipt)
        self.assertEqual(
            receipt["source_commit"],
            self._git("rev-parse", "HEAD").stdout.decode().strip(),
        )

    def test_governance_files_github_and_renamed_gitignore_are_exported(self):
        for relative in ("README.md", "LICENSE", "NOTICE", "CONTRIBUTING.md",
                         "CODE_OF_CONDUCT.md", "SECURITY.md"):
            self._write(relative, relative + "\n")
        self._write("CLAUDE.md", "agent conventions\n")
        self._write("CLAUDE.local.md", "private box notes\n")
        self._write(".gitignore", "private ignore rules\n")
        self._write(".github/workflows/public-guard.yml", "name: guard\n")
        self._write(".github/ISSUE_TEMPLATE/bug_report.yml", "name: bug\n")
        self._write("Tools/public/scaffold/gitattributes", "public attributes\n")
        self._write("Tools/public/scaffold/gitignore", "public ignore rules\n")
        self._commit()
        target = self.root / "export"
        completed = self._export(target)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        receipt = json.loads(
            (target / "PUBLIC_EXPORT_RECEIPT.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt["paths"], [
            ".gitattributes",
            ".github/ISSUE_TEMPLATE/bug_report.yml",
            ".github/workflows/public-guard.yml",
            ".gitignore",
            "CODE_OF_CONDUCT.md",
            "CONTRIBUTING.md",
            "LICENSE",
            "NOTICE",
            "README.md",
            "SECURITY.md",
            "Tools/public/public-files.txt",
        ])
        self.assertEqual((target / ".gitignore").read_text(encoding="utf-8"),
                         "public ignore rules\n")
        self.assertEqual((target / ".gitattributes").read_text(encoding="utf-8"),
                         "public attributes\n")
        self.assertEqual(
            sorted(path.relative_to(target).as_posix()
                   for path in (target / "Tools").rglob("*") if path.is_file()),
            ["Tools/public/public-files.txt"],
        )
        self.assertFalse((target / "CLAUDE.md").exists())
        self.assertFalse((target / "CLAUDE.local.md").exists())
        renamed = [entry for entry in receipt["files"] if entry["path"] == ".gitignore"]
        self.assertEqual(renamed[0]["source_path"], "Tools/public/scaffold/gitignore")
        attributes = [entry for entry in receipt["files"]
                      if entry["path"] == ".gitattributes"]
        self.assertEqual(attributes[0]["source_path"],
                         "Tools/public/scaffold/gitattributes")
        self.assertNotIn("source_path", [entry for entry in receipt["files"]
                                         if entry["path"] == "README.md"][0])

    def test_sensitive_release_and_engine_patch_paths_are_explicitly_denied(self):
        for relative in (
            "Build/WoTRelease/KEY-CUSTODY.md",
            "Build/WoTRelease/test-keys/announcement.pem",
            "Build/WoTRelease/private-release.json",
            "Build/WoTRelease/eos-credentials-production.json",
            "EnginePatches/UE-5.8/guarded-bootstrap.patch",
        ):
            with self.subTest(relative=relative):
                self.assertTrue(exporter._is_denied(relative))
        self.assertFalse(exporter._is_denied(
            "EnginePatches/UE-5.8/manifest.json"))
        self.assertFalse(exporter._is_denied(
            "EnginePatches/UE-5.8/source-toolchain.json"))

    def test_case_folded_local_config_names_are_denied(self):
        for relative in ("Tools/Local.json", "Tools/LOCAL.JSON",
                         "Tools/test/net/Nodes.json", "Source/nOdEs.JsOn"):
            with self.subTest(relative=relative):
                self.assertTrue(exporter._is_denied(relative))

    def test_unclassified_candidate_fails_export_before_writing(self):
        self._write("Tools/reviewed.py", "reviewed\n")
        self._commit()
        self._write("Tools/copied-script.cpp", "renamed private content\n")
        self._git("add", "Tools/copied-script.cpp")
        target = self.root / "public"

        completed = self._export(target, index=True)

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("in neither", completed.stderr)
        self.assertIn("Tools/copied-script.cpp", completed.stderr)
        self.assertFalse(target.exists())

    def _commit_with_private(self, public, private):
        self._write(exporter.MANIFEST_PATH, "".join(
            path + "\n" for path in sorted([*public, exporter.MANIFEST_PATH])))
        self._write(exporter.PRIVATE_LIST_PATH, "".join(
            path + "\n" for path in sorted([*private, exporter.PRIVATE_LIST_PATH])))
        self._git("add", "--all")
        self._git("commit", "--quiet", "-m", "classified")

    def test_privately_classified_candidate_is_not_exported(self):
        self._write("Tools/reviewed.py", "reviewed\n")
        self._write("Source/PrivateProof.cpp", "private proof\n")
        self._commit_with_private(["Tools/reviewed.py"], ["Source/PrivateProof.cpp"])
        target = self.root / "public"

        completed = self._export(target)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue((target / "Tools/reviewed.py").is_file())
        self.assertFalse((target / "Source/PrivateProof.cpp").exists())
        self.assertFalse((target / exporter.PRIVATE_LIST_PATH).exists())

    def test_private_list_errors_are_fatal(self):
        cases = (
            ("both public and private",
             ["Tools/reviewed.py"], ["Tools/reviewed.py"], "in both"),
            ("already excluded by policy",
             ["Tools/reviewed.py"], ["Tools/Local.json"], "already excludes"),
            ("not a candidate path",
             ["Tools/reviewed.py"], ["docs/notes.md"], "already excludes"),
        )
        for label, public, private, message in cases:
            with self.subTest(label):
                self._write("Tools/reviewed.py", "reviewed\n")
                self._write("Tools/Local.json", "{}\n")
                self._write("docs/notes.md", "notes\n")
                self._commit_with_private(public, private)
                target = self.root / f"public-{uuid4().hex}"

                completed = self._export(target)

                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(message, completed.stderr)
                self.assertFalse(target.exists())

    def test_private_list_format_is_strict(self):
        self._write("Tools/reviewed.py", "reviewed\n")
        self._write("Source/PrivateProof.cpp", "private proof\n")
        for label, text, message in (
            ("unsorted", "Tools/public/private-files.txt\nSource/PrivateProof.cpp\n",
             "must be sorted"),
            ("no self entry", "Source/PrivateProof.cpp\n", "must admit itself"),
            ("stale path",
             "Source/Gone.cpp\nSource/PrivateProof.cpp\nTools/public/private-files.txt\n",
             "no longer exists"),
        ):
            with self.subTest(label):
                self._write(exporter.MANIFEST_PATH,
                            "Tools/public/public-files.txt\nTools/reviewed.py\n")
                self._write(exporter.PRIVATE_LIST_PATH, text)
                self._git("add", "--all")
                self._git("commit", "--quiet", "-m", label)

                completed = self._export(self.root / f"public-{uuid4().hex}")

                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(message, completed.stderr)

    def test_propose_unclassified_lists_exactly_the_refused_paths(self):
        self._write("Tools/reviewed.py", "reviewed\n")
        self._write("Source/PrivateProof.cpp", "private proof\n")
        self._commit_with_private(["Tools/reviewed.py"], ["Source/PrivateProof.cpp"])
        self._write("Source/NewBuilder.cpp", "new\n")
        self._git("add", "Source/NewBuilder.cpp")
        self._git("commit", "--quiet", "-m", "unlisted")

        completed = subprocess.run(
            [sys.executable, str(SCRIPT), "propose", "--repo", str(self.repo),
             "--unclassified"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout, "Source/NewBuilder.cpp\n")

    def test_reviewed_rewrite_must_match_exactly_once(self):
        rewrites = {"docs/BUILD-PATHS.md": ((
            re.compile(br'/Users/[A-Za-z0-9_.-]+/projects/wot/WoT[.]uproject'),
            b'$PWD/WoT.uproject',
        ),)}
        with mock.patch.object(exporter, "PUBLIC_BYTE_REWRITES", rewrites):
            # Built at runtime so this file never holds a home path literal.
            personal = '-project="/Users/{}/projects/wot/WoT.uproject"\n'.format(
                "private-dev").encode("ascii")
            rewritten, changed = exporter._public_bytes(
                "docs/BUILD-PATHS.md", personal)
            self.assertTrue(changed)
            self.assertEqual(rewritten, b'-project="$PWD/WoT.uproject"\n')
            # A source fixed upstream must fail closed, not linger silently.
            with self.assertRaisesRegex(exporter.ExportError, "matched 0 times"):
                exporter._public_bytes("docs/BUILD-PATHS.md", b"scrubbed\n")
            self.assertEqual(exporter._public_bytes("README.md", b"x"), (b"x", False))

    def test_system_paths_export_unchanged_and_untransformed(self):
        self._write(
            "Tools/build/test_check_innoextract_link.py",
            r'TOOL = r"C:\VC\bin\dumpbin.exe"',
        )
        self._commit()
        target = self.root / "public"

        completed = self._export(target)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn(r'C:\VC\bin\dumpbin.exe',
                      (target / "Tools/build/test_check_innoextract_link.py").read_text())
        receipt = json.loads(
            (target / "PUBLIC_EXPORT_RECEIPT.json").read_text(encoding="utf-8"))
        self.assertEqual([entry["path"] for entry in receipt["files"]
                          if entry.get("transform")], [])

    def test_denied_path_cannot_be_forced_into_manifest(self):
        self._write("Tools/Local.json", "{}\n")
        self._write(exporter.MANIFEST_PATH,
                    "Tools/Local.json\nTools/public/public-files.txt\n")
        self._git("add", "--all")
        self._git("commit", "--quiet", "-m", "hostile manifest")

        completed = self._export(self.root / "public")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("denied path appears", completed.stderr)

    def test_manifest_path_that_no_longer_exists_is_fatal(self):
        self._write("Source/Keep.cpp", "source\n")
        self._write(exporter.MANIFEST_PATH,
                    "Source/Missing.cpp\nTools/public/public-files.txt\n")
        self._git("add", "--all")
        self._git("commit", "--quiet", "-m", "stale manifest")

        completed = self._export(self.root / "public")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("manifest path no longer exists", completed.stderr)

    def test_retail_named_sweep_fixtures_remain_excluded(self):
        relative = "Tools/test/sweep/fixtures/complete/Arena_01/summary.json"
        self.assertTrue(exporter._is_denied(relative))

    def test_nonempty_target_is_refused_without_writing(self):
        self._write("Source/Keep.cpp", "source\n")
        self._commit()
        target = self.root / "public"
        target.mkdir()
        marker = target / "owner-file.txt"
        marker.write_text("do not overwrite\n", encoding="utf-8")

        completed = self._export(target)

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("not empty", completed.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "do not overwrite\n")
        self.assertEqual(list(target.iterdir()), [marker])

    def test_export_runs_on_the_supported_python39_floor(self):
        self._write("Source/Keep.cpp", "source\n")
        self._commit()
        target = self.root / "public"

        with mock.patch.object(exporter.sys, "version_info", (3, 9)):
            receipt = exporter.export_public_tree(self.repo, target)

        self.assertEqual(receipt["paths"], [
            "Source/Keep.cpp", "Tools/public/public-files.txt",
        ])

    def test_staged_changes_cannot_replace_or_extend_the_head_tree(self):
        self._write("Source/Changed.cpp", "head bytes\n")
        self._commit()
        self._write("Source/Changed.cpp", "staged replacement\n")
        self._write("Source/Added.cpp", "staged addition\n")
        self._git("add", "--all")
        target = self.root / "public"

        completed = self._export(target)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            (target / "Source/Changed.cpp").read_bytes(), b"head bytes\n")
        self.assertFalse((target / "Source/Added.cpp").exists())

    def test_explicit_index_export_records_and_uses_the_staged_tree(self):
        self._write("Source/Changed.cpp", "head bytes\n")
        self._commit()
        head = self._git("rev-parse", "HEAD").stdout.decode().strip()
        self._write("Source/Changed.cpp", "staged replacement\n")
        self._write("Source/Added.cpp", "staged addition\n")
        self._git("add", "--all")
        self._write(exporter.MANIFEST_PATH,
                    "Source/Added.cpp\nSource/Changed.cpp\n"
                    "Tools/public/public-files.txt\n")
        self._git("add", exporter.MANIFEST_PATH)
        index_tree = self._git("write-tree").stdout.decode().strip()
        target = self.root / "public"

        completed = self._export(target, index=True)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual((target / "Source/Changed.cpp").read_text(),
                         "staged replacement\n")
        self.assertEqual((target / "Source/Added.cpp").read_text(),
                         "staged addition\n")
        receipt = json.loads(
            (target / "PUBLIC_EXPORT_RECEIPT.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt["source_kind"], "index")
        self.assertEqual(receipt["source_commit"], head)
        self.assertEqual(receipt["source_tree"], index_tree)

    def test_replacement_refs_cannot_replace_head_bytes(self):
        self._write("Source/Keep.cpp", "original commit\n")
        self._commit()
        original_commit = self._git("rev-parse", "HEAD").stdout.decode().strip()
        self._write("Source/Keep.cpp", "replacement commit\n")
        self._commit()
        replacement_commit = self._git("rev-parse", "HEAD").stdout.decode().strip()
        self._git("replace", original_commit, replacement_commit)
        self._git("update-ref", "HEAD", original_commit)
        target = self.root / "public"

        completed = self._export(target)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            (target / "Source/Keep.cpp").read_bytes(), b"original commit\n")

    def test_inherited_git_dir_cannot_redirect_the_source_repository(self):
        self._write("Source/Keep.cpp", "source repository\n")
        self._commit()
        target = self.root / "public"

        completed = self._export(
            target, {"GIT_DIR": str(self.root / "nonexistent.git")})

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            (target / "Source/Keep.cpp").read_bytes(), b"source repository\n")

    def test_concurrent_destination_file_is_never_overwritten(self):
        self._write("Source/Keep.cpp", "committed source\n")
        self._commit()
        target = self.root / "public"
        target.mkdir()
        original_read_blobs = exporter._read_blobs

        def insert_destination(repo, entries):
            blobs = original_read_blobs(repo, entries)
            destination = target / "Source" / "Keep.cpp"
            destination.parent.mkdir()
            destination.write_text("concurrent owner\n", encoding="utf-8")
            return blobs

        with mock.patch.object(exporter, "_read_blobs", insert_destination):
            with self.assertRaises(exporter.ExportError):
                exporter.export_public_tree(self.repo, target)

        self.assertEqual(
            (target / "Source/Keep.cpp").read_text(encoding="utf-8"),
            "concurrent owner\n",
        )
        self.assertFalse((target / "PUBLIC_EXPORT_RECEIPT.json").exists())

    def test_denylist_lfs_and_retail_dependent_tests_are_never_written(self):
        self._git("config", "filter.lfs.clean", "cat")
        self._git("config", "filter.lfs.smudge", "cat")
        self._git("config", "filter.lfs.process", "")
        self._git("config", "filter.lfs.required", "false")
        self._write(".gitattributes", "Source/*.bin filter=lfs\n")
        self._write("Source/Keep.h", "safe\n")
        self._write("Source/Retail.UC", "script text\n")
        self._write("Source/fixture.bin", b"lfs-attributed bytes")
        self._write("Tools/original/secret.txt", "retail\n")
        self._write("Tools/devtree/secret.txt", "private source\n")
        self._write("Tools/exports/secret.txt", "derived\n")
        self._write("Tools/Saved/session.txt", "session\n")
        self._write("Tools/Content/asset.txt", "asset\n")
        self._write("Tools/.superpowers/brief.md", "private\n")
        self._write("Tools/convert/test_zones.py", "retail dependent\n")
        self._write(
            "Tools/dist/fixtures/bsp-native-parity-v1.json", "retail census\n")
        self._write("Tools/import/data/angreal_hands.csv", "retail defaults\n")
        self._write("Tools/import/data/menu_strings.csv", "retail localization\n")
        self._write("Tools/test/gallery/gallery.json", "retail class defaults\n")
        self._write("Tools/test/gallery/runtime-map-t5-1.json", "retail camera\n")
        self._write(
            "Tools/test/sweep/sweep_expectations.json", "retail provenance\n")
        self._write(
            "Tools/test/sweep/fixtures/complete/Arena_01/summary.json",
            "retail title\n",
        )
        self._write(
            "Tools/test/gallery/runtime-map-t5-2-sky.json", "retail camera\n")
        self._write("Tools/import/test_pool_entries.py", "portable fixture test\n")
        self._write("ThirdParty/innoextract/1.9/README.wot", "retail extension\n")
        self._commit()
        target = self.root / "public"

        completed = self._export(target)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        receipt = json.loads(
            (target / "PUBLIC_EXPORT_RECEIPT.json").read_text(encoding="utf-8"))
        self.assertEqual(
            receipt["paths"],
            ["Source/Keep.h", "Tools/public/public-files.txt"],
        )
        self.assertEqual(
            sorted(
                path.relative_to(target).as_posix()
                for path in target.rglob("*")
                if path.is_file() and path.name != "PUBLIC_EXPORT_RECEIPT.json"
            ),
            receipt["paths"],
        )

    def test_repository_info_attributes_cannot_override_lfs_denial(self):
        self._git("config", "filter.lfs.clean", "cat")
        self._git("config", "filter.lfs.smudge", "cat")
        self._git("config", "filter.lfs.process", "")
        self._git("config", "filter.lfs.required", "false")
        self._write(".gitattributes", "Source/*.bin filter=lfs\n")
        self._write("Source/fixture.bin", b"retail bytes")
        self._commit()
        info_attributes = self.repo / ".git" / "info" / "attributes"
        info_attributes.write_text("Source/*.bin -filter\n", encoding="utf-8")
        target = self.root / "public"

        completed = self._export(target)

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("info/attributes", completed.stderr.replace("\\", "/"))
        self.assertFalse(target.exists())

    def test_executable_mode_is_recorded_in_receipt(self):
        self._write("Tools/run.sh", "#!/bin/sh\n")
        self._commit()
        self._git("update-index", "--chmod=+x", "Tools/run.sh")
        self._git("commit", "--quiet", "-m", "executable mode")
        target = self.root / "public"

        completed = self._export(target)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        receipt = json.loads(
            (target / "PUBLIC_EXPORT_RECEIPT.json").read_text(encoding="utf-8"))
        run_record = [entry for entry in receipt["files"]
                      if entry["path"] == "Tools/run.sh"][0]
        self.assertEqual(run_record["mode"], "100755")
        if os.name != "nt":
            self.assertTrue((target / "Tools/run.sh").stat().st_mode & 0o100)

    def test_symlink_tree_entry_is_rejected_before_target_creation(self):
        blob = self._git(
            "hash-object", "-w", "--stdin", input_bytes=b"../outside\n"
        ).stdout.decode("ascii").strip()
        self._git(
            "update-index", "--add", "--cacheinfo",
            f"120000,{blob},Source/link",
        )
        self._write(exporter.MANIFEST_PATH,
                    "Source/link\nTools/public/public-files.txt\n")
        self._git("add", exporter.MANIFEST_PATH)
        self._git("commit", "--quiet", "-m", "symlink fixture")
        target = self.root / "public"

        completed = self._export(target)

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("unsupported manifest entry", completed.stderr)
        self.assertFalse(target.exists())

    def test_portable_path_collisions_are_rejected(self):
        with self.assertRaises(exporter.ExportError):
            exporter._validate_portable_paths([
                "Source/Caf\N{LATIN SMALL LETTER E WITH ACUTE}.cpp",
                "Source/CAFE\N{COMBINING ACUTE ACCENT}.cpp",
            ])


if __name__ == "__main__":
    unittest.main()
