#!/usr/bin/env python3
"""Behavioral specs for the cache-v3 lazy consumer closure."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_lazy_payload_consumers as checker


class LazyPayloadConsumerCheckerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "Source/WoT"
        self.root.mkdir(parents=True)
        self.target = Path(self.temp.name) / "Source/WoT.Target.cs"
        self.target.write_text(
            'GlobalDefinitions.Add("WOT_GUARDED_BOOTSTRAP=1");',
            encoding="utf-8")
        runtime_dir = self.root / "Payload"
        runtime_dir.mkdir()
        (runtime_dir / "WoTRuntimeRegistry.cpp").write_text(
            """void FWoTRuntimeRegistry::Read() {
  ReadVerifiedCacheBlob(Lease, Descriptor, Limits);
}
""", encoding="utf-8")
        self.registry = Path(self.temp.name) / "registry.json"
        self.rows = []
        for index, (domain, schema) in enumerate(checker.REQUIRED_DOMAINS.items()):
            source = f"{domain.title().replace('_', '')}Consumer.cpp"
            function = f"Load{domain.title().replace('_', '')}FirstUse"
            (self.root / source).write_text(
                f"""void {function}() {{
  auto Verified = ReadVerifiedCacheBlob(Lease, Descriptor, Limits);
  Parse{index}(Verified.GetValue().Bytes());
}}
""", encoding="utf-8")
            self.rows.append({
                "id": f"{domain}-first-use",
                "domain": domain,
                "source": source,
                "function": function,
                "schema": schema,
                "maximum_decoded_bytes": 1024,
                "parser": f"Parse{index}",
                "mutation_vector": f"same-size-{domain}-byte-flip",
                "status": "active",
                "blocker": "",
            })
        self.write_registry()

    def tearDown(self):
        self.temp.cleanup()

    def write_registry(self, mutate=None):
        document = {
            "format": checker.FORMAT,
            "consumers": [dict(row) for row in self.rows],
        }
        if mutate:
            mutate(document)
        self.registry.write_text(json.dumps(document), encoding="utf-8")

    def check(self):
        return checker.check_tree(
            self.root, self.target, self.registry,
            required_reader="ReadVerifiedCacheBlob",
            require_mutation_vector=True)

    def test_complete_registry_passes(self):
        self.assertEqual(self.check(), (len(checker.REQUIRED_DOMAINS),
                                        len(checker.REQUIRED_DOMAINS)))

    def test_missing_domain_row_fails(self):
        self.write_registry(lambda doc: doc["consumers"].pop())
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "missing required domain"):
            self.check()

    def test_registered_site_must_call_required_reader(self):
        path = self.root / self.rows[0]["source"]
        path.write_text(path.read_text().replace(
            "ReadVerifiedCacheBlob", "ReadUncheckedCacheBlob"),
            encoding="utf-8")
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "does not carry a live ReadVerifiedCacheBlob"):
            self.check()

    def rewrite_first_consumer(self, body):
        path = self.root / self.rows[0]["source"]
        path.write_text(
            f"void {self.rows[0]['function']}() {{\n{body}\n}}\n",
            encoding="utf-8")

    def test_parser_before_verification_fails(self):
        self.rewrite_first_consumer("""
  Parse0(Unrelated);
  auto Verified = ReadVerifiedCacheBlob(Lease, Descriptor, Limits);
""")
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "does not carry a live"):
            self.check()

    def test_reader_in_dead_branch_fails(self):
        self.rewrite_first_consumer("""
  if (false) {
    auto Verified = ReadVerifiedCacheBlob(Lease, Descriptor, Limits);
    Parse0(Verified.GetValue().Bytes());
  }
""")
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "does not carry a live"):
            self.check()

    def test_unrelated_buffer_does_not_satisfy_dataflow(self):
        self.rewrite_first_consumer("""
  auto Verified = ReadVerifiedCacheBlob(Lease, Descriptor, Limits);
  Parse0(UnrelatedBytes);
""")
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "does not carry a live"):
            self.check()

    def test_unregistered_direct_blob_open_fails(self):
        (self.root / "Backdoor.cpp").write_text(
            """void LoadTextureBackdoor() {
  auto File = Platform.OpenRead(*BlobPath);
  ParseTexture(File);
}
""", encoding="utf-8")
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "lazy-reader bypass"):
            self.check()

    def test_shipping_reachable_engine_load_sinks_fail(self):
        sinks = (
            "Asset.LoadSynchronous()",
            "Path.TryLoad()",
            "FFileHelper::LoadFileToString(Text, *Path)",
            "IFileManager::Get().CreateFileReader(*Path)",
        )
        for index, sink in enumerate(sinks):
            with self.subTest(sink=sink):
                path = self.root / f"Backdoor{index}.cpp"
                path.write_text(
                    f"void Backdoor{index}() {{ {sink}; }}\n",
                    encoding="utf-8")
                with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                            "lazy-reader bypass"):
                    self.check()
                path.unlink()

    def test_wrapper_around_sensitive_sink_is_still_inventoried(self):
        (self.root / "WrapperBackdoor.cpp").write_text(
            """static void ReadThroughWrapper() { Path.TryLoad(); }
void UseWrapper() { ReadThroughWrapper(); }
""", encoding="utf-8")
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "lazy-reader bypass"):
            self.check()

    def test_sink_compiled_out_of_guarded_shipping_is_ignored(self):
        (self.root / "DeveloperOnly.cpp").write_text(
            """void DeveloperOnly() {
#if !WOT_GUARDED_BOOTSTRAP
  Asset.LoadSynchronous();
#endif
}
""", encoding="utf-8")
        self.assertEqual(self.check(), (len(checker.REQUIRED_DOMAINS),
                                        len(checker.REQUIRED_DOMAINS)))

    def test_mutation_vector_is_mandatory(self):
        self.write_registry(lambda doc: doc["consumers"][0].update(
            mutation_vector=""))
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "mutation vector"):
            self.check()

    def test_blocked_consumer_keeps_inventory_complete_but_fails_closure(self):
        self.write_registry(lambda doc: doc["consumers"][0].update(
            status="blocked",
            blocker="the gameplay materializer is not wired"))
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "blocked consumer.*gameplay materializer"):
            self.check()

    def test_domain_schema_is_pinned(self):
        self.write_registry(lambda doc: doc["consumers"][0].update(
            schema="wrong-v1"))
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "schema mismatch"):
            self.check()

    def test_duplicate_id_or_function_fails(self):
        self.write_registry(lambda doc: doc["consumers"].append(
            dict(doc["consumers"][0])))
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "duplicate consumer"):
            self.check()

    def test_two_domains_cannot_claim_a_generic_registry_read(self):
        self.write_registry(lambda doc: doc["consumers"][1].update(
            source=doc["consumers"][0]["source"],
            function=doc["consumers"][0]["function"],
            parser=doc["consumers"][0]["parser"]))
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "same first-use site"):
            self.check()

    def test_generic_registry_boundary_is_not_an_active_consumer(self):
        self.write_registry(lambda doc: doc["consumers"][0].update(
            source="Payload/WoTRuntimeRegistry.cpp",
            function="FWoTRuntimeRegistry::Read",
            parser="ReadVerifiedCacheBlob"))
        with self.assertRaisesRegex(checker.LazyConsumerCheckFailed,
                                    "generic registry boundary"):
            self.check()


if __name__ == "__main__":
    unittest.main()
