import argparse
import hashlib
import importlib
import json
import os
import platform
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

generator = importlib.import_module("Tools.import.generate_neutral_map_shells")


SPEC = Path(__file__).with_name("specs") / "neutral-map-shells-v1.json"


class _ActorClass:
    def __init__(self, path):
        self.path = path

    def get_path_name(self):
        return self.path


class _Actor:
    def __init__(self, fake, world, name, class_path, *, hidden=False):
        self.fake = fake
        self.world = world
        self.name = name
        self.actor_class = _ActorClass(class_path)
        self.hidden = hidden

    def get_class(self):
        return self.actor_class

    def get_path_name(self):
        return f"/Temp/Untitled.Untitled:PersistentLevel.{self.name}"

    def destroy_actor(self):
        self.fake.events.append(("destroy_actor", self.name))
        if self.fake.fail_actor_destruction:
            return
        self.world.actors.remove(self)
        self.fake.destroyed_actor_classes.append(self.actor_class.path)


class _World:
    def __init__(self, fake, *, fresh_blank=False):
        self.fake = fake
        self.package = None
        self.actors = []
        self.world_settings = _Actor(
            fake, self, "WorldSettings",
            "/Script/Engine.WorldSettings", hidden=True)
        self.actors.append(self.world_settings)
        if fresh_blank:
            # UE 5.8's blank-map path creates this hidden actor.  The editor
            # subsystem omits it, but GameplayStatics must expose it.
            self.actors.append(_Actor(
                fake, self, "ChaosDebugDrawActor",
                "/Script/Engine.Actor", hidden=True))

    def get_world_settings(self):
        return self.world_settings


class _EditorLoadingAndSavingUtils:
    def __init__(self, fake):
        self.fake = fake
        self.current = _World(fake)

    def new_blank_map(self, save_existing_map):
        if save_existing_map:
            raise AssertionError("generator must not save an existing editor map")
        self.current = _World(self.fake, fresh_blank=True)
        self.fake.events.append(("blank", None))
        return self.current

    def load_map(self, package):
        # The commandlet-safe world switch: loading any non-staged map
        # releases the current staged world exactly like the editor does.
        self.current = _World(self.fake)
        self.current.package = package
        self.fake.events.append(("load_map", package))
        return self.current

    def save_map(self, world, package):
        if world is not self.current:
            return False
        self.fake.saved_actor_censuses.append([
            actor.get_class().get_path_name() for actor in world.actors
        ])
        path = self.fake.staging_filename(package)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(("FAKE-UMAP-V2\0" + package).encode("ascii"))
        world.package = package
        self.fake.assets.add(package)
        self.fake.saved_packages.append(package)
        self.fake.events.append(("save", package))
        return True


class _FakeUnreal:
    class Actor:
        pass

    class EditorActorSubsystem:
        pass

    def __init__(self, content_root, events=None):
        self.content_root = Path(content_root)
        self.assets = set()
        self.saved_packages = []
        self.deleted_packages = []
        self.deleted_directories = []
        self.collect_garbage_calls = 0
        self.destroyed_actor_classes = []
        self.saved_actor_censuses = []
        self.fail_actor_destruction = False
        self.events = events if events is not None else []
        self.expected_output_root = None
        self.output_complete_at_delete = None
        self.fail_delete_directory = False
        self.EditorLoadingAndSavingUtils = _EditorLoadingAndSavingUtils(self)
        # UE 5.8 exposes ordinary FPaths helpers but no Python mount-point API.
        self.Paths = types.SimpleNamespace(
            project_content_dir=lambda: os.fspath(self.content_root),
            convert_relative_path_to_full=lambda path: os.path.abspath(path),
        )
        self.EditorAssetLibrary = types.SimpleNamespace(
            does_asset_exist=lambda package: package in self.assets,
            does_directory_exist=lambda package_root: any(
                asset.startswith(package_root + "/") for asset in self.assets),
            delete_asset=self.delete_asset,
            delete_directory=self.delete_directory,
        )
        self.SystemLibrary = types.SimpleNamespace(
            collect_garbage=self.collect_garbage,
        )
        self.GameplayStatics = types.SimpleNamespace(
            get_all_actors_of_class=self.get_all_actors_of_class,
        )

    def staging_filename(self, package):
        prefix = "/Game/"
        if not package.startswith(prefix):
            raise AssertionError(f"non-/Game staging package: {package}")
        return self.content_root / (package[len(prefix):] + ".umap")

    def delete_asset(self, package):
        self.events.append(("delete_asset", package))
        if self.expected_output_root is not None and \
                self.output_complete_at_delete is None:
            self.output_complete_at_delete = all(
                (self.expected_output_root / f"{name}.umap").is_file()
                for name in generator.SHELL_NAMES)
        if self.fail_delete_directory:
            return False
        if package not in self.assets:
            return False
        path = self.staging_filename(package)
        if path.exists():
            path.unlink()
        self.assets.remove(package)
        self.deleted_packages.append(package)
        return True

    def delete_directory(self, package_root):
        self.events.append(("delete_directory", package_root))
        self.deleted_directories.append(package_root)
        if self.expected_output_root is not None:
            self.output_complete_at_delete = all(
                (self.expected_output_root / f"{name}.umap").is_file()
                for name in generator.SHELL_NAMES)
        current_package = self.EditorLoadingAndSavingUtils.current.package
        if (self.fail_delete_directory
                or (current_package is not None
                    and current_package.startswith(package_root + "/"))):
            return False
        packages = sorted(
            package for package in self.assets
            if package.startswith(package_root + "/"))
        for package in packages:
            self.delete_asset(package)
        staging_root = self.content_root / package_root.removeprefix("/Game/")
        if staging_root.exists():
            staging_root.rmdir()
        return True

    def collect_garbage(self):
        self.collect_garbage_calls += 1
        self.events.append(("collect_garbage", None))

    def get_all_actors_of_class(self, world, actor_class):
        if actor_class is not self.Actor:
            raise AssertionError(f"unexpected actor class: {actor_class}")
        return list(world.actors)

    def get_editor_subsystem(self, kind):
        if kind is self.EditorActorSubsystem:
            return types.SimpleNamespace(get_all_level_actors=lambda: [
                actor for actor in self.EditorLoadingAndSavingUtils.current.actors
                if actor is not self.EditorLoadingAndSavingUtils.current.world_settings
                and not actor.hidden
            ])
        raise AssertionError(f"unexpected editor subsystem: {kind}")


def _arguments(root, *, output=None, denied=(), disjoint=()):
    output = output or root / "job" / "GeneratedContent" / "NeutralMapShells"
    mandated = [root / name for name in generator.MANDATED_DENIED_ROOT_NAMES]
    denied_roots = []
    for path in [*mandated, *denied]:
        if path not in denied_roots:
            denied_roots.append(path)
    return argparse.Namespace(
        spec=SPEC,
        output_root=output,
        denied_read_open_receipt=root / "evidence" / "denied.json",
        output_identity_receipt=root / "evidence" / "identity.json",
        deny_read_root=denied_roots,
        require_disjoint_from=list(disjoint),
        require_empty_output=True,
        no_replace=True,
    )


class NeutralMapShellGeneratorTests(unittest.TestCase):
    def test_reviewed_spec_is_exactly_nineteen_empty_neutral_slots(self):
        document = json.loads(SPEC.read_text(encoding="ascii"))
        self.assertEqual(document["format"], generator.FORMAT)
        self.assertEqual(document["neutral_allow_list"], [
            "/Script/Engine.Brush",
            "/Script/Engine.DefaultPhysicsVolume",
            "/Script/Engine.WorldSettings",
            "/Script/NavigationSystem.AbstractNavData",
            "/Script/WoT.WoTGameEngine",
        ])
        self.assertEqual(
            [shell["name"] for shell in document["shells"]],
            list(generator.SHELL_NAMES))
        self.assertEqual(
            {tuple(shell["persistent_level"][field])
             for shell in document["shells"]
             for field in ("actors", "object_paths", "soft_object_paths")},
            {()})
        self.assertNotIn("WoTBootstrap", SPEC.read_text(encoding="ascii"))

    def test_fresh_no_replace_root_seals_receipts_and_all_packages(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bootstrap = root / "job" / "GeneratedContent" / "BootstrapMap"
            content = root / "Content"
            arguments = _arguments(
                root, denied=(content,), disjoint=(bootstrap,))
            fake = _FakeUnreal(content)
            fake.expected_output_root = arguments.output_root
            denied, identity = generator.generate(
                arguments, fake)
            self.assertEqual(identity["package_count"], 19)
            self.assertEqual(len(identity["packages"]), 19)
            self.assertEqual(
                [row["slot"] for row in identity["packages"]],
                list(range(1, 20)))
            self.assertTrue(identity["root_began_absent"])
            self.assertEqual(
                {Path(path).name for path in denied["denied_roots"]},
                set(generator.MANDATED_DENIED_ROOT_NAMES))
            self.assertEqual(
                denied["io_audit_scope"], "python-audit-open-events-only")
            self.assertFalse(denied["native_ue_io_audited"])
            self.assertIn("cannot observe native Unreal",
                          denied["native_ue_io_limitation"])
            self.assertEqual(denied["violations"], [])
            self.assertIn(
                os.path.realpath(SPEC),
                {row["path"] for row in denied["opened_inputs"]})
            self.assertEqual(len(denied["generator_sha256"]), 64)
            for field in (
                    "generator_identity", "generator_sha256", "platform",
                    "root_identity", "spec_sha256", "version"):
                self.assertEqual(denied[field], identity[field])
            self.assertEqual(identity["platform"], platform.system())
            self.assertEqual(
                identity["root_identity"], os.path.realpath(arguments.output_root))
            spec_raw = SPEC.read_bytes()
            canonical_spec = generator._canonical_bytes(json.loads(spec_raw))
            self.assertNotEqual(
                hashlib.sha256(spec_raw).hexdigest(),
                hashlib.sha256(canonical_spec).hexdigest())
            self.assertEqual(
                identity["spec_sha256"],
                hashlib.sha256(spec_raw).hexdigest())
            spec_open = next(
                row for row in denied["opened_inputs"]
                if row["path"] == os.path.realpath(SPEC))
            self.assertEqual(spec_open["sha256"], identity["spec_sha256"])
            stat = arguments.output_root.stat()
            self.assertEqual(identity["root_native_identity"], {
                "device": stat.st_dev, "inode": stat.st_ino,
            })
            self.assertFalse(hasattr(fake.Paths, "register_mount_point"))
            expected_staging = [
                f"{generator.STAGING_PACKAGE_ROOT}/{name}"
                for name in generator.SHELL_NAMES
            ]
            self.assertEqual(fake.saved_packages, expected_staging)
            self.assertEqual(fake.destroyed_actor_classes, [
                "/Script/Engine.Actor"
            ] * len(generator.SHELL_NAMES))
            self.assertEqual(fake.saved_actor_censuses, [[
                "/Script/Engine.WorldSettings"
            ]] * len(generator.SHELL_NAMES))
            self.assertEqual(fake.deleted_packages, expected_staging)
            self.assertEqual(fake.assets, set())
            self.assertEqual(fake.collect_garbage_calls, 1)
            self.assertTrue(fake.output_complete_at_delete)
            self.assertFalse(
                (fake.content_root / "WoTNeutralShellStaging").exists())
            expected_packages = [
                f"{generator.PACKAGE_ROOT}/{name}"
                for name in generator.SHELL_NAMES
            ]
            self.assertEqual(
                [row["package"] for row in identity["packages"]],
                expected_packages)
            census_hasher = hashlib.sha256()
            for row in identity["packages"]:
                census_hasher.update(generator._canonical_bytes(row))
                self.assertEqual(len(row["sha256"]), 64)
                self.assertTrue(
                    (arguments.output_root / row["relative_path"]).is_file())
            self.assertEqual(
                identity["package_census_sha256"], census_hasher.hexdigest())
            self.assertFalse(any(
                "WoTNeutralShellStaging" in path
                for path in denied["python_audited_opens"]))

    def test_hidden_non_allow_listed_actor_is_destroyed_before_every_save(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake = _FakeUnreal(root / "Content")
            generator.generate(_arguments(root), fake)

            destroy_indexes = [
                index for index, event in enumerate(fake.events)
                if event == ("destroy_actor", "ChaosDebugDrawActor")
            ]
            save_indexes = [
                index for index, event in enumerate(fake.events)
                if event[0] == "save"
            ]
            self.assertEqual(len(destroy_indexes), len(generator.SHELL_NAMES))
            self.assertEqual(len(save_indexes), len(generator.SHELL_NAMES))
            self.assertTrue(all(
                destroy < save
                for destroy, save in zip(destroy_indexes, save_indexes)
            ))
            self.assertEqual(fake.saved_actor_censuses, [[
                "/Script/Engine.WorldSettings"
            ]] * len(generator.SHELL_NAMES))

    def test_failed_hidden_actor_destruction_refuses_generation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = _arguments(root)
            fake = _FakeUnreal(root / "Content")
            fake.fail_actor_destruction = True

            with self.assertRaisesRegex(
                    generator.GenerationFailed,
                    "post-sweep census.*?/Script/Engine.Actor"):
                generator.generate(arguments, fake)

            self.assertEqual(fake.saved_packages, [])
            self.assertEqual(fake.saved_actor_censuses, [])
            self.assertFalse(arguments.output_root.exists())
            self.assertFalse(arguments.denied_read_open_receipt.exists())
            self.assertFalse(arguments.output_identity_receipt.exists())

    def test_all_copies_precede_staging_teardown_and_receipts_follow_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            content = root / "Content"
            arguments = _arguments(root, denied=(content,))
            events = []
            fake = _FakeUnreal(content, events)
            fake.expected_output_root = arguments.output_root
            real_copy = generator._copy_file_new
            real_write = generator._write_new_json

            def observed_copy(source, destination):
                real_copy(source, destination)
                events.append(("copy", destination.name))

            def observed_write(path, document):
                events.append(("receipt", path.name))
                real_write(path, document)

            with mock.patch.object(
                    generator, "_copy_file_new", side_effect=observed_copy), \
                    mock.patch.object(
                        generator, "_write_new_json", side_effect=observed_write):
                generator.generate(arguments, fake)

            copies = [index for index, event in enumerate(events)
                      if event[0] == "copy"]
            delete = next(index for index, event in enumerate(events)
                          if event[0] in ("delete_asset", "delete_directory"))
            garbage = next(index for index, event in enumerate(events)
                           if event[0] == "collect_garbage")
            receipts = [index for index, event in enumerate(events)
                        if event[0] == "receipt"]
            self.assertEqual(len(copies), 19)
            self.assertEqual(len(receipts), 2)
            self.assertLess(max(copies), delete)
            self.assertLess(delete, garbage)
            self.assertLess(garbage, min(receipts))
            self.assertTrue(fake.output_complete_at_delete)

    def test_cleanup_failure_is_strict_and_removes_partial_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            content = root / "Content"
            arguments = _arguments(root, denied=(content,))
            fake = _FakeUnreal(content)
            fake.expected_output_root = arguments.output_root
            # In-editor release fails (as measured on the real commandlet)...
            fake.fail_delete_directory = True

            # ...and disk removal is the enforced census; simulate the
            # staging file refusing to leave the disk.
            with mock.patch.object(
                    generator, "_remove_staged_file",
                    side_effect=OSError("simulated immovable staging file")), \
                    self.assertRaisesRegex(
                        generator.GenerationFailed,
                        "could not remove staged file"):
                generator.generate(arguments, fake)

            self.assertTrue(fake.output_complete_at_delete)
            self.assertEqual(fake.collect_garbage_calls, 1)
            self.assertFalse(arguments.output_root.exists())
            self.assertFalse(arguments.denied_read_open_receipt.exists())
            self.assertFalse(arguments.output_identity_receipt.exists())

    def test_existing_staging_shell_is_never_replaced_or_cleaned_up(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            content = root / "Content"
            staging = (content / "WoTNeutralShellStaging"
                       / "WoTMapShell01.umap")
            staging.parent.mkdir(parents=True)
            staging.write_bytes(b"caller-owned-staging")
            arguments = _arguments(root, denied=(content,))
            with self.assertRaisesRegex(
                    generator.GenerationFailed,
                    "no-replace refused existing staging shell"):
                generator.generate(arguments, _FakeUnreal(content))
            self.assertEqual(staging.read_bytes(), b"caller-owned-staging")
            self.assertFalse(arguments.output_root.exists())

    def test_failed_save_cleans_only_the_new_staging_and_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            content = root / "Content"
            arguments = _arguments(root, denied=(content,))
            fake = _FakeUnreal(content)
            real_save = fake.EditorLoadingAndSavingUtils.save_map

            def fail_after_writing(world, package):
                real_save(world, package)
                return False

            fake.EditorLoadingAndSavingUtils.save_map = fail_after_writing
            with self.assertRaisesRegex(
                    generator.GenerationFailed, "failed to save empty shell"):
                generator.generate(arguments, fake)
            self.assertFalse(arguments.output_root.exists())
            self.assertFalse(
                (content / "WoTNeutralShellStaging").exists())
            self.assertFalse(arguments.denied_read_open_receipt.exists())
            self.assertFalse(arguments.output_identity_receipt.exists())

    def test_preexisting_stale_and_receipted_outputs_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = _arguments(root)
            arguments.output_root.mkdir(parents=True)
            (arguments.output_root / "stale.umap").write_bytes(b"stale")
            with self.assertRaisesRegex(generator.GenerationFailed, "must be absent"):
                generator.generate(arguments, _FakeUnreal(root / "Content"))

            arguments.output_root.joinpath("stale.umap").unlink()
            arguments.denied_read_open_receipt.parent.mkdir(parents=True)
            arguments.denied_read_open_receipt.write_text("stale")
            with self.assertRaisesRegex(generator.GenerationFailed, "receipt no-replace"):
                generator.generate(arguments, _FakeUnreal(root / "Content"))

    def test_output_and_bootstrap_roots_must_be_fully_disjoint(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "job" / "GeneratedContent" / "NeutralMapShells"
            cases = (output, output / "BootstrapMap", output.parent)
            for index, reserved in enumerate(cases):
                arguments = _arguments(
                    root / str(index), output=output,
                    disjoint=(reserved,))
                with self.assertRaisesRegex(generator.GenerationFailed, "overlaps"):
                    generator.generate(
                        arguments, _FakeUnreal(root / "Content"))

    def test_denied_content_retail_cache_and_world_object_reads_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            names = ("Content", "original", "devtree", "exports", "cache-v3")
            denied = [root / name for name in names]
            for path in denied:
                path.mkdir()
            output = root / "job" / "GeneratedContent" / "NeutralMapShells"
            arguments = _arguments(root, output=output, denied=denied)
            # A denied spec models retail/cache/world-v3 input substitution.
            arguments.spec = denied[-1] / "cache_world_fixture.wot-world-v3"
            arguments.spec.write_bytes(SPEC.read_bytes())
            with self.assertRaisesRegex(generator.GenerationFailed, "denied input open"):
                generator.generate(arguments, _FakeUnreal(root / "Content"))

    def test_output_cannot_live_in_a_denied_or_worktree_generated_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            denied = root / "Content"
            denied.mkdir()
            output = denied / "GeneratedContent" / "NeutralMapShells"
            with self.assertRaisesRegex(generator.GenerationFailed, "overlaps denied"):
                generator.generate(
                    _arguments(root, output=output, denied=(denied,)),
                    _FakeUnreal(root / "ProjectContent"))

            project_content = root / "ProjectContent"
            output = project_content / "NeutralMapShells"
            with self.assertRaisesRegex(
                    generator.GenerationFailed,
                    "overlaps persistent project Content"):
                generator.generate(
                    _arguments(root, output=output),
                    _FakeUnreal(project_content))
            self.assertFalse(output.exists())

    def test_required_freshness_flags_cannot_be_omitted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = _arguments(root)
            arguments.output_root.mkdir(parents=True)
            arguments.require_empty_output = False
            with self.assertRaisesRegex(
                    generator.GenerationFailed, "requires --require-empty-output"):
                generator.generate(
                    arguments, _FakeUnreal(root / "ProjectContent"))

            arguments.require_empty_output = True
            arguments.no_replace = False
            with self.assertRaisesRegex(
                    generator.GenerationFailed, "requires --no-replace"):
                generator.generate(
                    arguments, _FakeUnreal(root / "ProjectContent"))

    def test_second_generation_never_replaces_first_product(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = _arguments(root)
            generator.generate(arguments, _FakeUnreal(root / "Content"))
            arguments.denied_read_open_receipt = root / "evidence" / "second-denied.json"
            arguments.output_identity_receipt = root / "evidence" / "second-identity.json"
            with self.assertRaisesRegex(generator.GenerationFailed, "must be absent"):
                generator.generate(arguments, _FakeUnreal(root / "Content"))

    def test_missing_mandated_explicit_denied_root_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = _arguments(root)
            arguments.deny_read_root = arguments.deny_read_root[:-1]
            with self.assertRaisesRegex(
                    generator.GenerationFailed,
                    "missing mandated explicit denied roots.*exports"):
                generator.generate(arguments, _FakeUnreal(root / "Content"))


if __name__ == "__main__":
    unittest.main()
