#!/usr/bin/env python3
"""Stdlib tests for the persistent guarded build root (guarded_build_root.py).

The build root is only sound if UBT's timestamp checks see exactly the content
changes (ActionGraph.cs IsIndividualActionOutdated), nothing is ever copied
INTO UBT's outputs, and products leave only as verified copies. These tests pin
each of those properties without running UBT.
"""
from __future__ import annotations

import json
import os
import stat
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import guarded_build_root as br  # noqa: E402

REPO = Path(__file__).resolve().parents[2]

IDENTITY = {
    "format": "wot-ue58-source-receipt-v1",
    "platform": "Win64",
    "commit": "7deeb413d3dc1fc034f48d1aacc0861301829d32",
    "tag": "5.8.0-release",
    "toolchain": {"msvc_cl": "19.44.35228"},
    "checks": ["commit-matches-pin"],
}


def fake_pe(payload: bytes = b"exe") -> bytes:
    """A minimal complete PE32+ image whose one section holds payload."""
    e_lfanew = 64
    optional_size = 240  # PE32+ with 16 data directories
    table = e_lfanew + 24 + optional_size
    raw_pointer = (table + 40 + 511) // 512 * 512
    raw_size = (len(payload) + 511) // 512 * 512
    image = bytearray(raw_pointer + raw_size)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, e_lfanew)
    image[e_lfanew:e_lfanew + 4] = b"PE\0\0"
    struct.pack_into("<HH", image, e_lfanew + 4, 0x8664, 1)
    struct.pack_into("<H", image, e_lfanew + 20, optional_size)
    optional = e_lfanew + 24
    struct.pack_into("<H", image, optional, 0x20B)
    struct.pack_into("<I", image, optional + 60, raw_pointer)
    struct.pack_into("<I", image, optional + 108, 16)
    struct.pack_into("<II", image, table + 16, raw_size, raw_pointer)
    image[raw_pointer:raw_pointer + len(payload)] = payload
    return bytes(image)


def fake_macho(payload: bytes = b"exe", *, signature: int = 0) -> bytes:
    """A minimal 64-bit Mach-O: one LC_SEGMENT_64 over the whole file and an
    optional LC_CODE_SIGNATURE of `signature` bytes after it."""
    header, segment, code_signature = 32, 72, 16
    commands = segment + (code_signature if signature else 0)
    body = header + commands + len(payload)
    image = bytearray(body + signature)
    image[0:4] = b"\xcf\xfa\xed\xfe"
    struct.pack_into("<II", image, 16, 2 if signature else 1, commands)
    struct.pack_into("<II", image, header, 0x19, segment)
    struct.pack_into("<QQ", image, header + 40, 0, body)
    if signature:
        struct.pack_into("<IIII", image, header + segment, 0x1D,
                         code_signature, body, signature)
    image[header + commands:body] = payload
    return bytes(image)


def fake_fat(*slices: bytes) -> bytes:
    """A universal (fat) Mach-O holding the given thin slices."""
    offset = 4096
    table = bytearray(struct.pack(">II", 0xCAFEBABE, len(slices)))
    blobs = bytearray()
    for image in slices:
        table += struct.pack(">IIIII", 0x0100000C, 0, offset + len(blobs),
                             len(image), 12)
        blobs += image
    return bytes(table.ljust(offset, b"\0") + blobs)


class FakeClock:
    def __init__(self, now: float):
        self.now = now
        self.slept = []

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.slept.append(seconds)
        self.now += seconds


class BuildRootTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.base = Path(self._tmp.name)
        self.iso = self.base / "iso"
        self.root = self.base / "roots" / "Win64-7deeb413d3dc"

    def write(self, path: Path, data: bytes | str) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, str):
            data = data.encode("utf-8")
        path.write_bytes(data)
        return path

    def seed_iso(self) -> None:
        self.write(self.iso / "WoT.uproject", "{}")
        self.write(self.iso / "Config" / "DefaultEngine.ini", "[x]\n")
        self.write(self.iso / "Source" / "WoT" / "A.cpp", "int a;\n")
        self.write(self.iso / "Source" / "WoT" / "B.h", "#pragma once\n")
        self.write(self.iso / "ThirdParty" / "lib" / "x.lib", b"\x00lib")
        self.write(self.iso / "Build" / "Mac" / "e.entitlements", "<plist/>")


class PrepareTests(BuildRootTestCase):
    def test_first_prepare_creates_state_without_reset(self):
        result = br.prepare(self.root, "Win64", IDENTITY)
        self.assertIsNone(result["reset"])
        state = br.read_state(self.root)
        self.assertEqual(state["engine_identity"], IDENTITY)
        self.assertEqual(state["platform"], "Win64")
        self.assertIsNone(state["last_ubt_exit_epoch"])

    def test_same_identity_keeps_ubt_outputs(self):
        br.prepare(self.root, "Win64", IDENTITY)
        sentinel = self.write(self.root / "Intermediate" / "keep.obj", b"o")
        product = self.write(self.root / "Binaries" / "Win64" / "x.exe", b"x")
        br.mark_ubt_exit(self.root, clock=lambda: 100.0)
        result = br.prepare(self.root, "Win64", dict(IDENTITY),
                            clock=lambda: 200.0)
        self.assertIsNone(result["reset"])
        self.assertTrue(sentinel.is_file() and product.is_file())
        self.assertEqual(br.read_state(self.root)["last_ubt_exit_epoch"],
                         100.0)

    def test_changed_identity_platform_or_unknown_state_resets(self):
        cases = (
            ("toolchain", dict(IDENTITY, toolchain={"msvc_cl": "19.45"}),
             "Win64", True),
            ("commit", dict(IDENTITY, commit="0" * 40), "Win64", True),
            ("platform", IDENTITY, "Mac", True),
        )
        for label, identity, platform_name, state_present in cases:
            with self.subTest(label):
                br.prepare(self.root, "Win64", IDENTITY)
                obj = self.write(self.root / "Intermediate" / "a.obj", b"o")
                exe = self.write(
                    self.root / "Binaries" / "Win64" / "a.exe", b"x")
                result = br.prepare(self.root, platform_name, identity)
                self.assertIsNotNone(result["reset"])
                self.assertFalse(obj.exists() or exe.exists())
                # Reset back for the next case.
                br.prepare(self.root, "Win64", IDENTITY)
        # Outputs without any state file: provenance unknown.
        (self.root / br.STATE_NAME).unlink()
        orphan = self.write(self.root / "Intermediate" / "orphan.obj", b"o")
        result = br.prepare(self.root, "Win64", IDENTITY)
        self.assertEqual(
            result["reset"], "no build-root state beside existing UBT outputs")
        self.assertFalse(orphan.exists())

    def test_clock_behind_last_ubt_exit_resets(self):
        br.prepare(self.root, "Win64", IDENTITY)
        obj = self.write(self.root / "Intermediate" / "a.obj", b"o")
        br.mark_ubt_exit(self.root, clock=lambda: 5_000.0)
        result = br.prepare(self.root, "Win64", IDENTITY,
                            clock=lambda: 4_000.0)
        self.assertEqual(
            result["reset"], "system clock is behind the last UBT exit")
        self.assertFalse(obj.exists())

    def test_root_name_is_platform_and_pinned_commit(self):
        root = br.root_for(self.base, "Win64", IDENTITY["commit"])
        self.assertEqual(root.name, "Win64-7deeb413d3dc")
        with self.assertRaises(br.BuildRootError):
            br.root_for(self.base, "Win64", "not-a-commit")

    def test_engine_identity_is_path_independent(self):
        receipt = dict(IDENTITY, engine="/synthetic/engine",
                       manifest="/synthetic/iso-a/EnginePatches/x.json")
        other = dict(
            receipt, manifest="/synthetic/iso-b/EnginePatches/x.json")
        self.assertEqual(br.engine_identity(receipt),
                         br.engine_identity(other))
        with self.assertRaises(br.BuildRootError):
            br.engine_identity({"format": "something-else"})

    @unittest.skipUnless((REPO / ".git").exists(),
                         "exported source tree has no Git metadata")
    def test_default_parent_is_beside_the_git_common_dir(self):
        parent = br.default_parent(REPO)
        self.assertEqual(parent.name, ".guarded-build")
        self.assertTrue((parent.parent / ".git").exists())


class MirrorTests(BuildRootTestCase):
    def prepared(self, clock: FakeClock | None = None) -> FakeClock:
        clock = clock or FakeClock(time.time())
        br.prepare(self.root, "Win64", IDENTITY, clock=clock)
        return clock

    def test_first_mirror_copies_inputs_and_digests_match(self):
        self.seed_iso()
        clock = self.prepared()
        result = br.mirror(self.iso, self.root, clock=clock,
                           sleep=clock.sleep)
        self.assertEqual(result["copied"], 6)
        self.assertEqual(result["unchanged"], 0)
        self.assertEqual(result["input_tree"],
                         br.tree_digest(self.root))
        self.assertEqual(
            (self.root / "Source" / "WoT" / "A.cpp").read_text(), "int a;\n")
        # Tools/EnginePatches/Content are not UBT inputs and never mirrored.
        self.write(self.iso / "Tools" / "build" / "x.py", "pass\n")
        self.write(self.iso / "Content" / "WoT" / "x.uasset", b"asset")
        br.mirror(self.iso, self.root, clock=clock, sleep=clock.sleep)
        self.assertFalse((self.root / "Tools").exists())
        self.assertFalse((self.root / "Content").exists())

    def test_unchanged_inputs_keep_their_timestamps(self):
        self.seed_iso()
        clock = self.prepared()
        br.mirror(self.iso, self.root, clock=clock, sleep=clock.sleep)
        target = self.root / "Source" / "WoT" / "A.cpp"
        old = time.time() - 3600
        os.utime(target, (old, old))
        result = br.mirror(self.iso, self.root, clock=clock,
                           sleep=clock.sleep)
        self.assertEqual(result["copied"] + result["replaced"], 0)
        self.assertEqual(result["unchanged"], 6)
        self.assertAlmostEqual(target.stat().st_mtime, old, places=3)

    def test_changed_input_with_older_mtime_is_written_fresh(self):
        # The copy2 hazard: new content carrying an OLD timestamp must still
        # land in the build root with a timestamp newer than any output.
        self.seed_iso()
        clock = self.prepared()
        br.mirror(self.iso, self.root, clock=clock, sleep=clock.sleep)
        stale_output = self.write(
            self.root / "Intermediate" / "A.cpp.obj", b"old object")
        source = self.write(self.iso / "Source" / "WoT" / "A.cpp",
                            "int a = 2;\n")
        ancient = time.time() - 86_400
        os.utime(source, (ancient, ancient))
        result = br.mirror(self.iso, self.root, clock=clock,
                           sleep=clock.sleep)
        mirrored = self.root / "Source" / "WoT" / "A.cpp"
        self.assertEqual(result["replaced"], 1)
        self.assertEqual(mirrored.read_text(), "int a = 2;\n")
        self.assertGreater(mirrored.stat().st_mtime,
                           stale_output.stat().st_mtime - 1.0)
        self.assertGreater(mirrored.stat().st_mtime, ancient + 3600)

    def test_deletions_type_swaps_and_case_only_renames(self):
        self.seed_iso()
        clock = self.prepared()
        br.mirror(self.iso, self.root, clock=clock, sleep=clock.sleep)
        (self.iso / "Source" / "WoT" / "B.h").unlink()
        (self.iso / "Source" / "WoT" / "A.cpp").rename(
            self.iso / "Source" / "WoT" / "a.cpp")
        (self.iso / "ThirdParty" / "lib" / "x.lib").unlink()
        (self.iso / "ThirdParty" / "lib").rmdir()
        self.write(self.iso / "ThirdParty" / "lib", "now a file")
        result = br.mirror(self.iso, self.root, clock=clock,
                           sleep=clock.sleep)
        names = sorted(path.name for path in
                       (self.root / "Source" / "WoT").iterdir())
        self.assertEqual(names, ["a.cpp"])
        self.assertTrue((self.root / "ThirdParty" / "lib").is_file())
        self.assertGreaterEqual(result["deleted"], 2)
        self.assertEqual(result["input_tree"], br.tree_digest(self.root))

    def test_read_only_inputs_can_be_replaced_and_deleted(self):
        self.seed_iso()
        source = self.iso / "Source" / "WoT" / "A.cpp"
        os.chmod(source, stat.S_IREAD)
        self.addCleanup(lambda: source.exists() and os.chmod(
            source, stat.S_IREAD | stat.S_IWRITE))
        clock = self.prepared()
        br.mirror(self.iso, self.root, clock=clock, sleep=clock.sleep)
        os.chmod(source, stat.S_IREAD | stat.S_IWRITE)
        source.write_text("int a = 3;\n")
        os.chmod(source, stat.S_IREAD)
        br.mirror(self.iso, self.root, clock=clock, sleep=clock.sleep)
        mirrored = self.root / "Source" / "WoT" / "A.cpp"
        self.assertEqual(mirrored.read_text(), "int a = 3;\n")
        os.chmod(source, stat.S_IREAD | stat.S_IWRITE)
        source.unlink()
        result = br.mirror(self.iso, self.root, clock=clock,
                           sleep=clock.sleep)
        self.assertFalse(mirrored.exists())
        self.assertEqual(result["deleted"], 1)

    def test_mirror_waits_out_the_ubt_timestamp_slop(self):
        self.seed_iso()
        clock = self.prepared(FakeClock(1_000.0))
        br.mark_ubt_exit(self.root, clock=lambda: 999.5)
        br.mirror(self.iso, self.root, clock=clock, sleep=clock.sleep)
        self.assertEqual(len(clock.slept), 1)
        self.assertAlmostEqual(clock.slept[0], 1.5)

        stuck = FakeClock(1_000.0)
        stuck.sleep = lambda _seconds: None  # clock never advances
        br.mark_ubt_exit(self.root, clock=lambda: 999.9)
        with self.assertRaisesRegex(br.BuildRootError, "clock did not"):
            br.mirror(self.iso, self.root, clock=stuck, sleep=stuck.sleep)

    def test_unprepared_root_is_refused(self):
        self.seed_iso()
        with self.assertRaisesRegex(br.BuildRootError, "not prepared"):
            br.mirror(self.iso, self.root)


class TransferTests(BuildRootTestCase):
    def ubt_receipt(self, target="WoT", configuration="Test",
                    products=None, runtime=None) -> Path:
        name = br.ubt_receipt_relative("Win64", target, configuration)
        document = {
            "TargetName": target,
            "Platform": "Win64",
            "Configuration": configuration,
            "Version": {"BuildId": "build-id-1"},
            "BuildProducts": products if products is not None else [
                {"Path": "$(ProjectDir)/Binaries/Win64/WoT-Win64-Test.exe",
                 "Type": "Executable"},
                {"Path": "$(ProjectDir)/Binaries/Win64/WoT-Win64-Test.pdb",
                 "Type": "SymbolFile"},
                {"Path": "$(ProjectDir)/Binaries/Win64/D3D12/x64/D3D12Core.dll",
                 "Type": "DynamicLibrary"},
                {"Path": "$(EngineDir)/Binaries/Win64/Engine.dll",
                 "Type": "DynamicLibrary"},
            ],
            "RuntimeDependencies": runtime if runtime is not None else [
                {"Path": "$(ProjectDir)/WoT.uproject", "Type": "UFS"},
                {"Path": "$(ProjectDir)/Binaries/Win64/D3D12/x64/D3D12Core.dll",
                 "Type": "NonUFS"},
                {"Path": "$(EngineDir)/Binaries/ThirdParty/x.dll",
                 "Type": "NonUFS"},
            ],
        }
        return self.write(self.root / name, json.dumps(document))

    def build_products(self) -> None:
        self.write(self.root / "Binaries/Win64/WoT-Win64-Test.exe",
                   fake_pe(b"exe"))
        self.write(self.root / "Binaries/Win64/WoT-Win64-Test.pdb", b"pdb")
        self.write(self.root / "Binaries/Win64/D3D12/x64/D3D12Core.dll",
                   fake_pe(b"d3d"))
        self.write(self.root / "WoT.uproject", "{}")
        self.write(self.iso / "WoT.uproject", "{}")

    def transfer(self, **overrides):
        values = dict(
            platform_name="Win64", target="WoT", configuration="Test",
            receipt_path=self.iso / "receipts" / "transfer.json",
            input_tree={"sha256": "a" * 64})
        values.update(overrides)
        return br.transfer_products(self.root, self.iso, **values)

    def test_receipt_path_follows_target_receipt_default_path(self):
        self.assertEqual(
            br.ubt_receipt_relative("Win64", "WoTEditor", "Development"),
            "Binaries/Win64/WoTEditor.target")
        self.assertEqual(
            br.ubt_receipt_relative("Mac", "WoTUpdater", "Shipping"),
            "Binaries/Mac/WoTUpdater-Mac-Shipping.target")

    def test_products_and_receipt_are_copied_and_verified(self):
        self.build_products()
        self.ubt_receipt()
        result = self.transfer()
        receipt = result["receipt"]
        self.assertEqual(receipt["format"], br.TRANSFER_FORMAT)
        self.assertEqual(
            sorted(row["path"] for row in receipt["products"]),
            ["Binaries/Win64/D3D12/x64/D3D12Core.dll",
             "Binaries/Win64/WoT-Win64-Test.exe",
             "Binaries/Win64/WoT-Win64-Test.pdb"])
        self.assertEqual(receipt["engine_owned_products"], 1)
        self.assertEqual(receipt["ubt_receipt"]["build_id"], "build-id-1")
        self.assertEqual(
            (self.iso / "Binaries/Win64/WoT-Win64-Test.exe").read_bytes(),
            fake_pe(b"exe"))
        self.assertTrue(
            (self.iso / "Binaries/Win64/WoT-Win64-Test.target").is_file())
        self.assertEqual(
            [row["path"] for row in receipt["runtime_dependencies_verified"]],
            ["WoT.uproject"])
        self.assertEqual(
            br.product_digest(receipt, "Binaries/Win64/WoT-Win64-Test.exe"),
            br.sha256_file(self.iso / "Binaries/Win64/WoT-Win64-Test.exe"))
        on_disk = json.loads(Path(result["path"]).read_text())
        self.assertEqual(on_disk, receipt)

    def test_transfer_fails_closed(self):
        self.build_products()
        self.ubt_receipt(configuration="Shipping")
        with self.subTest("receipt for a different configuration"):
            (self.root / br.ubt_receipt_relative(
                "Win64", "WoT", "Shipping")).rename(
                self.root / br.ubt_receipt_relative("Win64", "WoT", "Test"))
            with self.assertRaisesRegex(br.BuildRootError, "identity"):
                self.transfer()
        with self.subTest("product outside Binaries"):
            self.ubt_receipt(products=[
                {"Path": "$(ProjectDir)/Content/x.uasset", "Type": "x"}])
            with self.assertRaisesRegex(br.BuildRootError,
                                        "outside Binaries"):
                self.transfer()
        with self.subTest("escaping product path"):
            self.ubt_receipt(products=[
                {"Path": "$(ProjectDir)/Binaries/Win64/../../x", "Type": "x"}])
            with self.assertRaisesRegex(br.BuildRootError, "unsafe"):
                self.transfer()
        with self.subTest("missing product"):
            self.ubt_receipt(products=[
                {"Path": "$(ProjectDir)/Binaries/Win64/gone.exe",
                 "Type": "Executable"}])
            with self.assertRaisesRegex(br.BuildRootError, "missing"):
                self.transfer()
        with self.subTest("truncated image"):
            self.ubt_receipt()
            exe = self.root / "Binaries/Win64/WoT-Win64-Test.exe"
            exe.write_bytes(fake_pe(b"exe")[:-1])
            with self.assertRaisesRegex(br.BuildRootError, "truncated"):
                self.transfer()
            exe.write_bytes(fake_pe(b"exe"))
        with self.subTest("runtime dependency differs"):
            self.ubt_receipt()
            (self.iso / "WoT.uproject").write_text('{"changed": 1}')
            with self.assertRaisesRegex(br.BuildRootError, "differs"):
                self.transfer()


class InFlightTests(BuildRootTestCase):
    """An interrupted UBT run must not leave half-written products behind."""

    def setUp(self):
        super().setUp()
        br.prepare(self.root, "Win64", IDENTITY)
        self.now = time.time()

    def written_at(self, relative: str, when: float,
                   data: bytes = b"x") -> Path:
        path = self.write(self.root / relative, data)
        os.utime(path, (when, when))
        return path

    def test_clean_exit_clears_the_marker_and_keeps_outputs(self):
        self.assertIsNone(br.begin_ubt(self.root, "WoT-Test",
                                       clock=lambda: self.now))
        self.assertEqual(br.read_state(self.root)[br.IN_FLIGHT_KEY]["label"],
                         "WoT-Test")
        product = self.written_at("Binaries/Win64/WoT-Win64-Test.exe",
                                  self.now + 5)
        br.end_ubt(self.root, succeeded=True, clock=lambda: self.now + 6)
        state = br.read_state(self.root)
        self.assertNotIn(br.IN_FLIGHT_KEY, state)
        self.assertEqual(state["last_ubt_exit_epoch"], self.now + 6)
        self.assertIsNone(br.begin_ubt(self.root, "WoT-Test",
                                       clock=lambda: self.now + 7))
        self.assertTrue(product.is_file())

    def test_killed_run_loses_exactly_what_it_wrote(self):
        older = self.write(self.root / "Intermediate/Build/old.obj", b"o")
        older_exe = self.write(
            self.root / "Binaries/Win64/WoTUpdater-Win64-Test.exe", b"u")
        start = self.now + 100
        br.begin_ubt(self.root, "WoT-Test", clock=lambda: start)
        # The dead run's writes: a partial link and a partial object.
        partial_exe = self.written_at(
            "Binaries/Win64/WoT-Win64-Test.exe", start + 30)
        partial_obj = self.written_at("Intermediate/Build/new.obj",
                                      start + 1)
        history = self.written_at(
            "Intermediate/Build/Win64/WoT/ActionHistory.dat", start + 31)
        # No end_ubt: the process died. A new pipeline prepares the root...
        result = br.prepare(self.root, "Win64", dict(IDENTITY),
                            clock=lambda: start + 200)
        self.assertIsNone(result["reset"])
        self.assertIn(br.IN_FLIGHT_KEY, br.read_state(self.root))
        # ...and its next UBT run recovers before building.
        recovered = br.begin_ubt(self.root, "WoT-Test",
                                 clock=lambda: start + 300)
        self.assertEqual(recovered["label"], "WoT-Test")
        self.assertEqual(recovered["deleted"], 2)
        self.assertFalse(recovered["wiped"])
        self.assertFalse(partial_exe.exists() or partial_obj.exists())
        self.assertTrue(older.is_file() and older_exe.is_file()
                        and history.is_file())
        self.assertEqual(
            br.read_state(self.root)[br.IN_FLIGHT_KEY]["started_epoch"],
            start + 300)

    def test_failed_exit_keeps_the_marker(self):
        br.begin_ubt(self.root, "WoT-Test", clock=lambda: self.now + 100)
        partial = self.written_at("Binaries/Win64/WoT-Win64-Test.exe",
                                  self.now + 110)
        br.end_ubt(self.root, succeeded=False, clock=lambda: self.now + 120)
        self.assertIn(br.IN_FLIGHT_KEY, br.read_state(self.root))
        recovered = br.begin_ubt(self.root, "WoT-Test",
                                 clock=lambda: self.now + 130)
        self.assertEqual(recovered["deleted"], 1)
        self.assertFalse(partial.exists())

    def test_reset_drops_the_marker(self):
        br.begin_ubt(self.root, "WoT-Test", clock=lambda: self.now)
        result = br.prepare(self.root, "Win64",
                            dict(IDENTITY, commit="0" * 40))
        self.assertIsNotNone(result["reset"])
        self.assertNotIn(br.IN_FLIGHT_KEY, br.read_state(self.root))

    def test_marker_without_a_start_time_wipes_ubt_outputs(self):
        obj = self.write(self.root / "Intermediate/a.obj", b"o")
        state = br.read_state(self.root)
        state[br.IN_FLIGHT_KEY] = {"label": "WoT-Test"}
        br.write_json(self.root / br.STATE_NAME, state)
        recovered = br.begin_ubt(self.root, "WoT-Test")
        self.assertTrue(recovered["wiped"])
        self.assertFalse(obj.exists())


class ImageCompletenessTests(BuildRootTestCase):
    def image(self, data: bytes, name: str = "x.exe") -> Path:
        return self.write(self.base / name, data)

    def test_complete_images_pass(self):
        br.verify_image_complete(self.image(fake_pe()), "Win64")
        br.verify_image_complete(self.image(fake_macho()), "Mac")
        br.verify_image_complete(
            self.image(fake_macho(signature=64)), "Mac")
        br.verify_image_complete(
            self.image(fake_fat(fake_macho(b"a"), fake_macho(b"bb"))), "Mac")
        # Unchecked platforms accept anything.
        br.verify_image_complete(self.image(b"elf?"), "Linux")

    def test_truncated_or_foreign_images_fail(self):
        cases = (
            ("PE one byte short", fake_pe()[:-1], "Win64", "truncated"),
            ("PE cut to its headers", fake_pe()[:512], "Win64", "truncated"),
            ("not PE on Win64", b"built-exe", "Win64", "not a PE"),
            ("PE section table cut", fake_pe()[:300], "Win64",
             "section table"),
            ("Mach-O one byte short", fake_macho()[:-1], "Mac", "truncated"),
            ("Mach-O signature cut", fake_macho(signature=64)[:-10], "Mac",
             "truncated"),
            ("not Mach-O on Mac", fake_pe(), "Mac", "not a Mach-O"),
            ("fat slice cut", fake_fat(fake_macho(b"a"))[:-1], "Mac",
             "truncated"),
        )
        for label, data, platform_name, message in cases:
            with self.subTest(label):
                with self.assertRaisesRegex(br.BuildRootError, message):
                    br.verify_image_complete(self.image(data), platform_name)

    def test_pe_certificate_table_counts(self):
        data = bytearray(fake_pe())
        optional = 64 + 24
        struct.pack_into("<II", data, optional + 112 + 32, len(data), 16)
        with self.assertRaisesRegex(br.BuildRootError, "truncated"):
            br.verify_image_complete(self.image(bytes(data)), "Win64")
        br.verify_image_complete(self.image(bytes(data) + b"c" * 16),
                                 "Win64")


class LockTests(BuildRootTestCase):
    def test_second_holder_is_refused_and_release_allows_reuse(self):
        with br.BuildRootLock(self.root):
            with self.assertRaisesRegex(br.BuildRootError, "in use"):
                with br.BuildRootLock(self.root):
                    pass
        with br.BuildRootLock(self.root) as lock:
            self.assertEqual(lock.path.parent, self.root.parent)


class ModulesBuildIdTests(BuildRootTestCase):
    def test_reads_build_id_or_none(self):
        path = self.write(self.base / "UnrealEditor.modules",
                          json.dumps({"BuildId": "abc", "Modules": {}}))
        self.assertEqual(br.modules_build_id(path), "abc")
        self.assertIsNone(br.modules_build_id(self.base / "missing"))
        path.write_text("{}")
        self.assertIsNone(br.modules_build_id(path))


if __name__ == "__main__":
    unittest.main()
