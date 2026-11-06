#!/usr/bin/env python3
"""Behavioral specs for the all-tree Shipping subsystem inventory."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import inventory_shipping_subsystems as inventory


class ShippingSubsystemInventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "Source/WoT"
        self.root.mkdir(parents=True)
        self.target = Path(self.temp.name) / "Source/WoT.Target.cs"
        self.target.write_text(
            'GlobalDefinitions.Add("WOT_GUARDED_BOOTSTRAP=1");',
            encoding="utf-8")
        self.policy = Path(self.temp.name) / "policy.json"
        self.write_subsystem("WoTAlpha", "UWoTAlphaSubsystem",
                             "UGameInstanceSubsystem",
                             "IsWoTPayloadBootstrapActive")
        self.write_policy()

    def tearDown(self):
        self.temp.cleanup()

    def write_subsystem(self, stem, class_name, base, guard):
        (self.root / f"{stem}.h").write_text(
            f"class {class_name} : public {base} {{}};", encoding="utf-8")
        (self.root / f"{stem}.cpp").write_text(
            f"""#include \"{stem}.h\"
void {class_name}::Initialize(FSubsystemCollectionBase& C) {{
  if (!{guard}()) return;
  FWorldDelegates::OnWorldTickStart.AddUObject(this, &{class_name}::Tick);
}}
""", encoding="utf-8")

    def write_policy(self, mutate=None):
        document = {
            "format": inventory.FORMAT,
            "guard_define": "WOT_GUARDED_BOOTSTRAP",
            "policies": [{
                "class": "UWoTAlphaSubsystem",
                "header": "WoTAlpha.h",
                "implementation": "WoTAlpha.cpp",
                "bootstrap_policy": "payload-active",
                "activation_prerequisite": "IsWoTPayloadBootstrapActive",
                "forbidden_pre_activation": ["world delegates"],
                "comment": "synthetic behavior owner",
            }],
        }
        if mutate:
            mutate(document)
        self.policy.write_text(json.dumps(document), encoding="utf-8")

    def run_inventory(self):
        return inventory.inventory_tree(
            self.target, self.root, self.policy, require_bootstrap_guards=True)

    def test_complete_guarded_policy_passes(self):
        result = self.run_inventory()
        self.assertEqual([item.class_name for item in result.subsystems],
                         ["UWoTAlphaSubsystem"])
        self.assertEqual(len(result.registrations), 1)

    def test_missing_policy_row_fails(self):
        self.write_policy(lambda doc: doc["policies"].clear())
        with self.assertRaisesRegex(inventory.InventoryFailed,
                                    "unclassified Shipping subsystem"):
            self.run_inventory()

    def test_new_nested_subsystem_is_not_excluded(self):
        nested = self.root / "Deep/Diagnostics"
        nested.mkdir(parents=True)
        (nested / "WoTBeta.h").write_text(
            "class UWoTBetaSubsystem : public UWorldSubsystem {};",
            encoding="utf-8")
        (nested / "WoTBeta.cpp").write_text(
            "void UWoTBetaSubsystem::OnWorldBeginPlay(UWorld&) {}",
            encoding="utf-8")
        with self.assertRaisesRegex(inventory.InventoryFailed,
                                    "UWoTBetaSubsystem"):
            self.run_inventory()

    def test_guard_is_required_before_registration(self):
        implementation = self.root / "WoTAlpha.cpp"
        implementation.write_text(implementation.read_text().replace(
            "if (!IsWoTPayloadBootstrapActive()) return;\n  ", ""),
            encoding="utf-8")
        with self.assertRaisesRegex(inventory.InventoryFailed,
                                    "registration without bootstrap guard"):
            self.run_inventory()

    def test_guard_after_registration_does_not_dominate(self):
        implementation = self.root / "WoTAlpha.cpp"
        implementation.write_text(
            """void UWoTAlphaSubsystem::Initialize(FSubsystemCollectionBase&) {
  FWorldDelegates::OnWorldTickStart.AddUObject(this, &UWoTAlphaSubsystem::Tick);
  if (!IsWoTPayloadBootstrapActive()) return;
}
""", encoding="utf-8")
        with self.assertRaisesRegex(inventory.InventoryFailed,
                                    "registration without bootstrap guard"):
            self.run_inventory()

    def test_guard_hidden_in_dead_branch_does_not_dominate(self):
        implementation = self.root / "WoTAlpha.cpp"
        implementation.write_text(
            """void UWoTAlphaSubsystem::Initialize(FSubsystemCollectionBase&) {
  if (false) { if (!IsWoTPayloadBootstrapActive()) return; }
  FWorldDelegates::OnWorldTickStart.AddUObject(this, &UWoTAlphaSubsystem::Tick);
}
""", encoding="utf-8")
        with self.assertRaisesRegex(inventory.InventoryFailed,
                                    "registration without bootstrap guard"):
            self.run_inventory()

    def test_split_implementation_file_is_inventoried(self):
        (self.root / "WoTAlphaExtra.cpp").write_text(
            """void UWoTAlphaSubsystem::StartLater() {
  if (!IsWoTPayloadBootstrapActive()) return;
  GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UWoTAlphaSubsystem::Tick);
}
""", encoding="utf-8")
        result = self.run_inventory()
        self.assertEqual(len(result.registrations), 2)
        self.assertIn("WoTAlphaExtra.cpp",
                      {item.implementation for item in result.registrations})

    def test_readiness_listener_actor_is_guard_enforced(self):
        actor = self.root / "Ambient.cpp"
        actor.write_text(
            """void AWoTAmbient::BeginPlay() {
  RegisterWoTBootstrapWorldReadinessListener(Listener);
}
void AWoTAmbient::Schedule() {
  GetWorldTimerManager().SetTimer(Timer, this, &AWoTAmbient::Poll, 1.0f);
}
""", encoding="utf-8")
        with self.assertRaisesRegex(inventory.InventoryFailed,
                                    "AWoTAmbient"):
            self.run_inventory()

    def test_policy_paths_and_classes_must_match_discovery(self):
        self.write_policy(lambda doc: doc["policies"][0].update(
            implementation="NotAlpha.cpp"))
        with self.assertRaisesRegex(inventory.InventoryFailed,
                                    "implementation mismatch"):
            self.run_inventory()

    def test_guard_define_must_be_bound_by_shipping_target(self):
        self.target.write_text("// unguarded", encoding="utf-8")
        with self.assertRaisesRegex(inventory.InventoryFailed,
                                    "guarded Shipping definition"):
            self.run_inventory()


if __name__ == "__main__":
    unittest.main()
