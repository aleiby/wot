#!/usr/bin/env python3
"""Synthetic inputs for the release-plumbing specs. Imported by tests only.

Everything here lives below a tempfile.TemporaryDirectory: the three Ed25519
keys are minted by `openssl genpkey` inside it (release_crypto.generate_test_key
refuses any other location), the trust files are synthetic public exports of
those keys, and the "guarded releases" imitate build_stripped_pak.py's package
stage byte-shape (Win64: zipfile, files only, host modes 0666/0777; Mac: Unix
modes and explicit directories). Each signing also gets a passing cache gate A
receipt per platform, produced by the real gate (cache_lineage.evaluate_gate)
over a synthetic cache manifest. No production key, path,
or artifact is touched, and the directory is deleted with the fixture.
"""
from __future__ import annotations

import copy
import hashlib
import json
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from typing import Dict, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_content_label as content_label  # noqa: E402
import cache_lineage  # noqa: E402
import generate_bootstrap_trust as generator  # noqa: E402
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import mac_dmg  # noqa: E402
import sign_release  # noqa: E402

COMMIT = "0a1b2c3d4"
RELEASE_KEY_ID = "release-v1"
GENERATED_UTC = {"Win64": "2026-09-28T01:02:03.456789+00:00",
                 "Mac": "2026-09-28T04:05:06.000001+00:00"}

# A release is a guarded SHIPPING build (sign_release refuses Test without
# --allow-test); the Test shapes remain available for the --allow-test specs.


def release_identity_bytes(release_sequence: int = 0,
                           release_commit: str = COMMIT) -> bytes:
    return formats.canonical_bytes({
        "format": "wot-release-identity-v1",
        "release_commit": release_commit,
        "release_sequence": release_sequence,
        "version": 1,
    }, "release identity")


def release_identity_relative(platform: str) -> str:
    return ("WoT/Build/WoTRelease/release-identity.json"
            if platform == "Win64"
            else "Contents/UE/WoT/Build/WoTRelease/release-identity.json")


def platform_files(platform: str, configuration: str = formats.SHIPPING_CONFIGURATION,
                   release_sequence: int = 0,
                   release_commit: str = COMMIT) -> Dict[str, tuple]:
    """The staged tree build_stripped_pak packages, per configuration."""
    if platform == "Win64":
        game = f"WoT/Binaries/Win64/WoT-Win64-{configuration}.exe"
        return {
            game: (f"MZ win64 {configuration} game ".encode() * 4000, 0o100777),
            "WoT/Content/Paks/pakchunk0-Windows.pak": (bytes(range(256)) * 64, 0o100666),
            release_identity_relative(platform): (
                release_identity_bytes(release_sequence, release_commit), 0o100666),
            "Engine/Binaries/ThirdParty/Ogg/Win64/libogg_64.dll": (b"dll" * 99, 0o100666),
            "Manifest_NonUFSFiles_Win64.txt": ((game + "\n").encode(), 0o100666),
        }
    return {
        f"Contents/MacOS/WoT-Mac-{configuration}": (
            b"\xcf\xfa\xed\xfe mac game " * 3000, 0o100755),
        "Contents/Info.plist": (b"<plist><dict/></plist>\n", 0o100644),
        release_identity_relative(platform): (
            release_identity_bytes(release_sequence, release_commit), 0o100644),
        "Contents/UE/WoT/Content/Paks/pakchunk0-Mac.pak": (bytes(range(256)) * 80, 0o100644),
        "Contents/_CodeSignature/CodeResources": (b"<plist/>\n", 0o100644),
    }


def product_paths(platform: str, configuration: str = formats.SHIPPING_CONFIGURATION
                  ) -> tuple:
    if platform == "Win64":
        return ("Windows", f"Windows/WoT/Binaries/Win64/WoT-Win64-{configuration}.exe")
    return ("Mac", f"Mac/WoT.app/Contents/MacOS/WoT-Mac-{configuration}")


WIN64_FILES = platform_files("Win64")
MAC_FILES = platform_files("Mac")
PRODUCT_PATHS = {platform: product_paths(platform) for platform in formats.PLATFORMS}


def openssl_or_skip() -> str:
    try:
        return crypto.find_openssl()
    except crypto.CryptoError as exc:
        raise unittest.SkipTest(str(exc))


def _info(name: str, mode: int, create_system: int) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=(2026, 9, 28, 12, 0, 0))
    info.create_system = create_system
    info.external_attr = mode << 16
    if name.endswith("/"):
        info.external_attr |= 0x10
    info.compress_type = zipfile.ZIP_DEFLATED
    return info


def write_guarded_zip(path: Path, platform: str,
                      files: Optional[Dict[str, tuple]] = None,
                      extra: Optional[Dict[str, tuple]] = None,
                      configuration: str = formats.SHIPPING_CONFIGURATION,
                      release_sequence: int = 0,
                      release_commit: str = COMMIT) -> None:
    """Imitate build_stripped_pak.py _stage_package for one platform."""
    files = dict(files or platform_files(
        platform, configuration, release_sequence, release_commit))
    files.update(extra or {})
    root = formats.GUARDED_ZIP_ROOTS[platform]
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        if platform == "Mac":
            # ditto shape after its source-side metadata suppression: Unix
            # attributes and explicit directories, with no AppleDouble entries.
            directories = {root}
            for relative in files:
                parts = relative.split("/")
                for depth in range(1, len(parts)):
                    directories.add(root + "/" + "/".join(parts[:depth]))
            for directory in sorted(directories):
                archive.writestr(_info(directory + "/", 0o040755, 3), b"")
            for relative, (data, mode) in sorted(files.items()):
                archive.writestr(_info(f"{root}/{relative}", mode, 3), data)
        else:
            # zipfile on Windows: files only, create_system 0, host modes.
            archive.writestr(_info(f"{root}/WoT Reborn.exe", 0o100666, 0),
                             b"MZ synthetic stable launcher")
            for relative, (data, mode) in sorted(files.items()):
                archive.writestr(_info(f"{root}/Game/{relative}", mode, 0), data)


def synthetic_test_twin(platform: str, commit: str, snapshot_sha256: str,
                        release_sequence: int = 0) -> dict:
    """The shape build_stripped_pak._test_twin_binding writes (digests synthetic)."""
    def digest(label: str) -> str:
        return hashlib.sha256(f"{label}-{platform}-{commit}".encode()).hexdigest()
    return {
        "asset": formats.guarded_install_zip_asset(
            platform, commit, formats.TEST_CONFIGURATION),
        "asset_sha256": digest("test-zip"),
        "asset_size": 4242,
        "configuration": formats.TEST_CONFIGURATION,
        "container_bytes": {"differs": [], "identical": ["Paks/global.utoc"]},
        "cooked_container_inventory_receipt_sha256": digest("cooked-inventory"),
        "format": formats.TEST_TWIN_FORMAT,
        "git_commit": commit,
        "project_package_census_sha256": digest("census"),
        "release_json_sha256": digest("test-release-json"),
        "release_sequence": release_sequence,
        "source_snapshot_sha256": snapshot_sha256,
        "staged_tree_sha256": digest("test-staged-tree"),
        "zen_lifetime_receipt_sha256": digest("zen"),
    }


def synthetic_update_evidence(name: str, sequence: int = 10, platform: str = "Win64") -> dict:
    digest = lambda value: hashlib.sha256(value.encode("utf-8")).hexdigest()
    if name == "mac-uninstall":
        profile = "/tmp/wot-funnel/profiles/mac-uninstall"
        bundle = "/tmp/wot-funnel/cell-trees/mac-uninstall/WoT.app"
        bundle_parent = str(Path(bundle).parent)
        data_root = profile + "/Library/Application Support/WoT Reborn"
        update_dir = str(Path(bundle).parent / ".wot-update")
        installer = str(Path(bundle_parent) / formats.PINNED_INSTALLER_NAME)
        sentinel = str(Path(bundle_parent)
                       / ".wot-uninstall-funnel-sentinel")
        session_gate = data_root + "/session.lock"
        parent_identity = {
            "device": 17, "inode": 101, "path": bundle_parent,
        }
        installer_identity = {
            "device": 17, "inode": 102, "path": installer,
            "sha256": digest("protected-installer"), "size": 1024,
        }
        sentinel_identity = {
            "device": 17, "inode": 103, "path": sentinel,
            "sha256": digest("protected-sentinel"), "size": 43,
        }
        session_gate_identity = {
            "device": 17, "inode": 104, "mode": 0o600,
            "path": session_gate, "sha256": digest(""), "size": 0,
        }
        return {
            "network": {
                "announcement_url":
                    "http://127.0.0.1:28123/announcement.json",
                "inherited_endpoint_variables_scrubbed": True,
                "release_base_url":
                    "http://127.0.0.1:28123" + formats.FUNNEL_RELEASE_BASE_PATH,
            },
            "paths": {
                "bundle": bundle,
                "bundle_parent": bundle_parent,
                "data_root": data_root,
                "installer": installer,
                "profile": profile,
                "session_gate": session_gate,
                "sibling_sentinel": sentinel,
                "trash": profile + "/.Trash",
                "update_dir": update_dir,
            },
            "postconditions": {
                "bundle_absent": True,
                "bundle_parent_unchanged": True,
                "data_root_absent": True,
                "installer_unchanged": True,
                "session_gate_absent": True,
                "sibling_sentinel_unchanged": True,
                "trash_contains_bundle_or_original_absent": True,
                "update_dir_absent": True,
            },
            "protected_artifacts": {
                "bundle_parent_after": dict(parent_identity),
                "bundle_parent_before": dict(parent_identity),
                "installer_after": dict(installer_identity),
                "installer_before": dict(installer_identity),
                "sibling_sentinel_after": dict(sentinel_identity),
                "sibling_sentinel_before": dict(sentinel_identity),
            },
            "process": {
                "argv": [
                    bundle + "/Contents/MacOS/WoT-Mac-Test",
                    "-wottestpreferencesdir=" + data_root + "/Preferences",
                    "-wottestuninstall=confirm",
                ],
                "exit_status": 0,
                "pid": 8123,
            },
            "request_transcript": [],
            "result": {
                "answer": "confirm",
                "failure_reason": "",
                "format": "wot-uninstall-result-v1",
                "remaining_paths": [],
                "removed_paths": [data_root, update_dir, bundle],
            },
            "session_gate": {
                "before": session_gate_identity,
                "owner_only_before_launch": True,
            },
        }
    base_root = hashlib.sha256(b"base-tree").hexdigest()
    good_root = hashlib.sha256(b"good-tree").hexdigest()
    bad_root = hashlib.sha256(b"bad-tree").hexdigest()
    variant_root = bad_root if name == "update-bad-build-rolls-back" else good_root
    final_root = (good_root if name == "update-apply"
                  or (platform == "Mac" and name == "interrupted-swap")
                  else base_root)
    second_role = ("D" if platform == "Mac"
                   and name == "update-bad-build-rolls-back" else "B")
    second_sequence = (sequence + 1 if name == "update-apply"
                       or (platform == "Mac" and name == "interrupted-swap")
                       else sequence)
    evidence = {
        "base": {"archive_sha256": "a" * 64, "git_commit": COMMIT,
                 "release_json_sha256": "b" * 64, "release_sequence": sequence, "tree_root": base_root},
        "cleanup": [{"attempt": name, "deleted": True}],
        "derivation_receipt": {"path": "update-fixture-derivation-v1.json", "sha256": "d" * 64, "size": 10},
        "processes": [{"role": "A", "pid": 100, "ready": True, "release_identity_sequence": sequence},
                      {"role": second_role, "pid": 101, "ready": True,
                       "release_identity_sequence": second_sequence}],
        "request_transcript": [],
        "trees": {"live_before": base_root, "live_after": final_root,
                  "previous_after": None},
        "variant_tree_root": variant_root, "trial_absent": True, "previous_absent": True, "frontend_handoff": True,
    }
    if name == "update-apply":
        evidence.update(ready_marker=f"{sequence + 1}\n", explicit_launch=True)
    elif platform == "Mac" and name == "interrupted-swap":
        evidence.update(ready_marker=f"{sequence + 1}\n",
                        interrupted_layout=True, staged_absent=True)
    else:
        evidence.update(started_marker=f"{sequence + 1} started\n",
            failure_notice={"failed_sequence": sequence + 1, "restored_sequence": sequence,
                "path": "Preferences/update-failed-v1.notice", "sha256": "f" * 64, "state": "pending"})
        if name == "update-bad-build-rolls-back":
            evidence.update(ready_marker=f"{sequence + 1}\n", bad_exit_status=1,
                archive_request_count_after_staging=1, archive_request_count_after_normal_recheck=1,
                recheck={"event": "update-check-result", "result": "known-bad", "pid": 101, "status": "KnownBad",
                         "reason": "failed-release", "sequence": sequence + 1})
            if platform == "Mac":
                evidence.update(rollback_exit_status=1,
                                rolled_back_marker=f"{sequence + 1}\n")
        else:
            evidence.update(missing_live_before=True, player_data_unchanged=True, recovery_without_trial=True)
    return evidence


def write_funnel_receipt(path: Path, release_json: Path, *, verdict: str = "pass",
                         cells: Optional[Dict[str, str]] = None,
                         include_update_cells: bool = False, **release_overrides
                         ) -> Path:
    """A first-run funnel receipt (run_first_run_funnel.py) for a release's twin.

    For a Shipping release it names the Test twin; for a Test release, itself.
    """
    document = json.loads(Path(release_json).read_text(encoding="utf-8"))
    twin = document.get("test_twin")
    run = {
        "asset_path": "synthetic.zip",
        "asset_sha256": twin["asset_sha256"] if twin else document["sha256"],
        "asset_size": twin["asset_size"] if twin else document["size"],
        "git_commit": document["git_commit"],
        "manifest_path": "synthetic/release.json",
        "manifest_sha256": (twin["release_json_sha256"] if twin
                            else formats.sha256_file(Path(release_json))[0]),
        "release_sequence": (twin["release_sequence"] if twin
                             else document["release_sequence"]),
        "source_snapshot_sha256": document["source_snapshot"]["sha256"],
        "development_allow_source_drift": False,
        "stapled": bool(document.get("products", {}).get("receipt", {})
                        .get("stapled", False)),
    }
    run.update(release_overrides)
    cell_names = list(formats.FUNNEL_CELL_NAMES)
    if include_update_cells:
        cell_names.extend(formats.UPDATE_FUNNEL_CELLS_BY_PLATFORM[document["platform"]])
    unknown_cells = set(cells or {}) - set(cell_names)
    if unknown_cells:
        raise ValueError(f"cell overrides are outside the selected matrix: "
                         f"{sorted(unknown_cells)}")
    verdicts = {name: (cells or {}).get(name, "pass") for name in cell_names}
    timestamp = "2026-09-28T08:00:00+00:00"
    receipt = {
        "format": formats.FUNNEL_RECEIPT_FORMAT,
        "platform": document["platform"],
        "verdict": verdict,
        "started_utc": timestamp,
        "completed_utc": timestamp,
        "duration_seconds": 1.0,
        "release": run,
        "installer": {
            "path": "synthetic/" + formats.PINNED_INSTALLER_NAME,
            "size": formats.PINNED_INSTALLER_SIZE,
            "sha256": formats.PINNED_INSTALLER_SHA256,
            "acquisition": "provided",
        },
        "cells": [{
            "format": formats.FUNNEL_CELL_RECEIPT_FORMAT,
            "index": index,
            "name": name,
            "verdict": verdicts[name],
            "started_utc": timestamp,
            "completed_utc": timestamp,
            "duration_seconds": 0.1,
            "phase_timings_seconds": {},
            "phase_order": [],
            "evidence": (synthetic_update_evidence(
                name, run["release_sequence"], document["platform"])
                if name in formats.UPDATE_EVIDENCE_KEYS_BY_PLATFORM[
                    document["platform"]] else {}),
            "decisions": [],
            "skip_reason": None,
            "error": None,
        } for index, name in enumerate(cell_names)],
        "decisions": [],
    }
    fresh_first_run = next(
        cell for cell in receipt["cells"]
        if cell["name"] == "fresh-first-run")
    if fresh_first_run["verdict"] == "pass":
        notes_count = 0 if run["release_sequence"] == 0 else 1
        notes_outcome = ("not-attempted" if run["release_sequence"] == 0
                         else "selected")
        notes_request = {
            "format": "wot-funnel-loopback-request-v1",
            "if_range": "",
            "method": "GET",
            "path": (formats.FUNNEL_RELEASE_BASE_PATH
                     + formats.RELEASE_NOTES_LATEST_PATH),
            "range": "",
            "range_end": None,
            "range_start": None,
            "response_bytes": 64,
            "status": 200,
            "timestamp_utc": timestamp,
        }
        process = {
            "pid": 4242,
            "command": [],
            "stdout": "stdout.log",
            "stderr": "stderr.log",
            "state_root": "state",
        }
        manifest = {
            "blobs": [{
                "logical_id": "cache/localization/WoT.int",
                "satisfies": ["synthetic.localization"],
                "schema": "wot-localization-v3",
                "sha256": "3" * 64,
                "size": 1,
            }],
            "cache_version": formats.CACHE_MANIFEST_VERSION,
            "contract_sha256": "1" * 64,
            "format": formats.CACHE_MANIFEST_FORMAT,
            "front_end_descriptor_root": "0" * 64,
            "hash_algorithm": "sha256",
            "installer_sha256": receipt["installer"]["sha256"],
            "production_descriptor_sha256": "2" * 64,
            "world_descriptors": [],
        }
        manifest_sha256 = hashlib.sha256(formats.canonical_bytes(
            manifest, "synthetic cache manifest")).hexdigest()
        fresh_first_run["evidence"] = {
            "adjacent_installer": dict(receipt["installer"]),
            "boot_failure_receipts": [],
            "detected_heartbeat": {
                "event": "original-game-detected",
                "phase": "CheckingOriginalGameData",
                "detail": "source=adjacent",
                "pid": process["pid"],
            },
            "frontend_handoff": {
                "epoch": 2, "event": "frontend-handoff",
                "phase": "Ready", "pid": process["pid"],
            },
            "installer_requests": [],
            "loopback_requests": ([notes_request] if notes_count else []),
            "manifests": [{
                "path": "ContentCache/current/manifest.json",
                "exists": True,
                "sha256": manifest_sha256,
                "json": manifest,
            }],
            "notes_outcome": notes_outcome,
            "notes_request_count": notes_count,
            "notes_result": {
                "commit": run["git_commit"],
                "outcome": notes_outcome,
                "pid": process["pid"],
                "sequence": run["release_sequence"],
            },
            "phase_first_seen_seconds": {},
            "process": process,
            "process_alive_after_activation": True,
            "termination": {
                "forced": False,
                "returncode": 0,
                "signals": [],
                "wait_seconds": 0.0,
            },
        }
        if document["platform"] == "Mac":
            fresh_first_run["evidence"]["mac_profile_library"] = {
                "allowed_roots": [
                    "Application Support/WoT Reborn",
                    "Caches/com.aleiby.wot",
                    "Saved Application State",
                    "HTTPStorages",
                    "Logs/DiagnosticReports",
                ],
                "allowed_files": [
                    "Preferences/com.aleiby.wot.plist",
                    "Application Support/Epic/Epic Games/KeyValueStore.ini",
                ],
                "engine_identifier_store": "absent",
                "entries": [],
                "outside_root": [],
            }
        else:
            fresh_first_run["evidence"]["win_profile_appdata"] = {
                "entries": [],
                "forbidden": [],
            }
    synthetic_final = (copy.deepcopy(fresh_first_run["evidence"])
                       if fresh_first_run["verdict"] == "pass" else None)
    if synthetic_final is not None:
        synthetic_final["front_end_snapshot"] = {
            "map": "WoTBootstrap",
            "netmode": 0,
            "session": "named=0 leave=0",
        }
        damaged = next(cell for cell in receipt["cells"]
                       if cell["name"] == "damaged-cache-rebuild")
        if damaged["verdict"] == "pass":
            repaired = copy.deepcopy(synthetic_final)
            reference = repaired["manifests"][0]
            reference["sha256"] = hashlib.sha256(formats.canonical_bytes(
                reference["json"], "synthetic repaired manifest")).hexdigest()
            damaged["evidence"] = {
                "damaged_logical_id": "cache/localization/WoT.int",
                "expected_blob_sha256": "3" * 64,
                "frontend_handoff": {
                    "epoch": 2, "event": "frontend-handoff",
                    "phase": "Ready", "pid": 4242,
                },
                "repaired_blob_sha256": "3" * 64,
                "installer_requests": [],
                "final": repaired,
            }
    process_observer = ("win32-job-get-process-times"
                        if document["platform"] == "Win64"
                        else "mac-ps-start-time")

    def acquisition_process(pid: int) -> dict:
        return {
            "pid": pid,
            "command": [],
            "stdout": "stdout.log",
            "stderr": "stderr.log",
            "state_root": "state",
        }

    def acquisition_identity(pid: int) -> dict:
        before = {
            "pid": pid, "process_start_id": f"synthetic-start-{pid}",
            "observed_monotonic_seconds": 9.0,
        }
        after = dict(before)
        after["observed_monotonic_seconds"] = 13.0
        return {"observer": process_observer, "before": before, "after": after}

    downloads_pickup = next(
        cell for cell in receipt["cells"] if cell["name"] == "downloads-pickup")
    if downloads_pickup["verdict"] == "pass":
        models = (["partial-final"] if document["platform"] == "Mac" else
                  ["chrome-edge", "firefox"])
        attempts = []
        for index, model in enumerate(models, 1):
            pid = 2000 + index
            final = copy.deepcopy(synthetic_final)
            final["process"] = acquisition_process(pid)
            attempts.append({
                "model": model,
                "entry_heartbeat": {
                    "event": "original-game-needed",
                    "phase": "OriginalGameNeeded",
                    "detail": "reason=missing translocated=0 downloads=1",
                    "pid": pid, "monotonic_seconds": 10.0,
                },
                "partial_hold": ({
                    "event": "partial-hold", "pid": pid,
                    "hold_seconds": 8.0, "monotonic_seconds": 11.0,
                } if model == "partial-final" else None),
                "detected_heartbeat": {
                    "event": "original-game-detected",
                    "phase": "OriginalGameNeeded",
                    "detail": "source=downloads",
                    "pid": pid, "monotonic_seconds": 12.0,
                },
                "downloaded_installer": {
                    "path": "synthetic/downloads/" +
                        formats.PINNED_INSTALLER_NAME,
                    "size": receipt["installer"]["size"],
                    "sha256": receipt["installer"]["sha256"],
                    "monotonic_seconds": 11.5,
                },
                "installer_requests": [],
                "process_identity": acquisition_identity(pid),
                "final": final,
            })
        downloads_pickup["evidence"] = {"attempts": attempts}

    update_cells = [cell for cell in receipt["cells"]
                    if cell["name"] in formats.UPDATE_EVIDENCE_KEYS and cell["verdict"] == "pass"]
    if update_cells:
        for cell in update_cells:
            cell["evidence"]["base"].update({
                "archive_sha256": run["asset_sha256"],
                "git_commit": run["git_commit"],
                "release_json_sha256": run["manifest_sha256"],
                "release_sequence": run["release_sequence"],
            })
        sample = update_cells[0]["evidence"]

        def component(label: str) -> dict:
            return {"name": label, "sha256": hashlib.sha256(
                label.encode("utf-8")).hexdigest(), "size": 10}

        def file_ref(label: str, signed: bool = False) -> dict:
            value = component(label)
            if signed:
                value["signature"] = component(label + ".sig")
            return value

        roots = {
            cell["name"]: cell["evidence"] for cell in update_cells
        }
        good_root = roots.get("update-apply", sample)["trees"]["live_after"]
        bad_root = roots.get("update-bad-build-rolls-back", {"variant_tree_root": hashlib.sha256(b"bad-tree").hexdigest()})["variant_tree_root"]
        base = sample["base"]
        derivation = {
            "base": {
                "archive": {"name": "base.zip",
                            "sha256": base["archive_sha256"], "size": 10},
                "commit": base["git_commit"],
                "main_executable": component(
                    "Contents/MacOS/WoT-Mac-Test"
                    if document["platform"] == "Mac"
                    else "base-game.exe"),
                "product_receipt_sha256": hashlib.sha256(
                    b"product-receipt").hexdigest(),
                "release_json_sha256": base["release_json_sha256"],
                "release_sequence": base["release_sequence"],
                "tree_root": base["tree_root"],
            },
            "format": "wot-update-funnel-derivation-v1",
            "next_release": {
                "release_commit": base["git_commit"],
                "release_sequence": base["release_sequence"] + 1,
            },
            "platform": document["platform"],
            "test_public_keys": {
                key: hashlib.sha256(key.encode("utf-8")).hexdigest()
                for key in ("announcement", "offline_root", "release")
            },
            "variants": {},
        }
        for label, tree_root in (("bad", bad_root), ("good", good_root)):
            derivation["variants"][label] = {
                "announcement": file_ref(f"{label}-announcement.json", True),
                "archive": file_ref(f"{label}.zip"),
                "directory": label,
                "key_ring": file_ref(f"{label}-ring.json", True),
                "main_executable": component(
                    "Contents/MacOS/WoT-Mac-Test"
                    if document["platform"] == "Mac"
                    else "WoT-Win64-Test.exe"),
                "manifest": file_ref(f"{label}-manifest.json", True),
                "mutations": [{"operation": "synthetic",
                               "path": f"{label}/identity.json"}],
                "tag": f"synthetic-{label}",
                "tree_root": tree_root,
            }
            if document["platform"] == "Mac":
                derivation["variants"][label]["signature_verification"] = {
                    "artifact": "extracted-post-ditto",
                    "command": "codesign --verify --deep --strict",
                    "verified": True,
                }
        derivation_path = Path(path).parent / "update-fixture-derivation-v1.json"
        derivation_path.parent.mkdir(parents=True, exist_ok=True)
        derivation_path.write_text(
            json.dumps(derivation, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        derivation_digest, derivation_size = formats.sha256_file(derivation_path)
        for cell in update_cells:
            cell["evidence"]["derivation_receipt"] = {
                "path": derivation_path.name,
                "sha256": derivation_digest,
                "size": derivation_size,
            }
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
    return Path(path)


def synthetic_cache_manifest(revision: int = 1, *,
                             blob_sha256: str = "3" * 64,
                             installer_sha256: str = formats.PINNED_INSTALLER_SHA256,
                             contract_sha256: str = "1" * 64) -> dict:
    """A one-blob cache manifest stamped with ``revision``'s content label."""
    return {
        "blobs": [{
            "logical_id": "cache/localization/WoT.int",
            "satisfies": ["synthetic.localization"],
            "schema": "wot-localization-v3",
            "sha256": blob_sha256,
            "size": 1,
        }],
        "cache_version": formats.CACHE_MANIFEST_VERSION,
        "contract_sha256": contract_sha256,
        "format": formats.CACHE_MANIFEST_FORMAT,
        "front_end_descriptor_root": "0" * 64,
        "hash_algorithm": "sha256",
        "installer_sha256": installer_sha256,
        "production_descriptor_sha256": content_label.label(revision),
        "world_descriptors": [],
    }


def manifest_digest(manifest: dict) -> str:
    return formats.sha256_hex(formats.canonical_bytes(manifest, "cache manifest"))


def synthetic_smoke_receipt(platform: str, asset_sha256: str,
                            manifest: Optional[dict], *, verdict: str = "pass",
                            release_json_sha256: str = "e" * 64) -> dict:
    """The parts of a Shipping smoke receipt the cache gate reads."""
    manifests = [] if manifest is None else [{
        "exists": True,
        "json": manifest,
        "path": "work/smoke/extracted/WoT Reborn/Data/ContentCache/current/manifest.json",
        "sha256": manifest_digest(manifest),
        "size": len(formats.canonical_bytes(manifest, "cache manifest")),
    }]
    return {
        "checks": {"boot": {"cache": {"manifests": manifests}}},
        "format": cache_lineage.SMOKE_FORMAT,
        "platform": platform,
        "release": {"asset_sha256": asset_sha256,
                    "manifest_sha256": release_json_sha256},
        "verdict": verdict,
    }


def synthetic_reference(platform: str, manifest: dict, *, release_sequence: int = 0,
                        revision: Optional[int] = None) -> dict:
    """A lineage-sourced cache reference describing ``manifest``."""
    identity = cache_lineage.manifest_identity(manifest, "reference manifest")
    return {
        "contract_sha256": identity["contract_sha256"],
        "format": cache_lineage.REFERENCE_FORMAT,
        "installer_sha256": identity["installer_sha256"],
        "label": identity["label"],
        "manifest_sha256": identity["manifest_sha256"],
        "platform": platform,
        "release_sequence": release_sequence,
        "revision": (revision if revision is not None else
                     content_label.revision_for_label(identity["label"], 64)),
        "source": "lineage",
    }


def write_cache_gate_receipt(path: Path, release_json: Path, *, revision: int = 1,
                             reference_manifest: Optional[dict] = None) -> Path:
    """A passing gate A receipt for a release.json's asset, made by the real gate."""
    document = json.loads(Path(release_json).read_text(encoding="utf-8"))
    platform = document["platform"]
    candidate = synthetic_cache_manifest(revision)
    reference = synthetic_reference(
        platform, reference_manifest or synthetic_cache_manifest(1),
        release_sequence=max(0, int(document.get("release_sequence", 0)) - 1))
    receipt = cache_lineage.evaluate_gate(
        platform=platform, reference=reference,
        smoke_receipt=synthetic_smoke_receipt(platform, document["sha256"], candidate),
        compiled_revision=revision)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
    return Path(path)


def write_guarded_release(directory: Path, platform: str, *,
                          files: Optional[Dict[str, tuple]] = None,
                          extra: Optional[Dict[str, tuple]] = None,
                          commit: str = COMMIT, dirty: bool = False,
                          release_sequence: int = 0,
                          configuration: str = formats.SHIPPING_CONFIGURATION,
                          test_twin: Optional[dict] = None,
                          notarized: bool = True) -> Path:
    """A release dir as stage_package leaves it: release.json + the zip."""
    directory.mkdir(parents=True, exist_ok=True)
    asset = formats.guarded_install_zip_asset(platform, commit, configuration)
    write_guarded_zip(directory / asset, platform, files, extra, configuration,
                      release_sequence, commit)
    digest, size = formats.sha256_file(directory / asset)
    source = dict(files or platform_files(
        platform, configuration, release_sequence, commit))
    stage_root, main_path = product_paths(platform, configuration)
    prefix = stage_root + "/" + ("WoT.app/" if platform == "Mac" else "")

    def product(path: str) -> dict:
        data = source[path[len(prefix):]][0]
        return {"compiled_receipt_sha256": "c" * 64, "engine_receipt_sha256": "e" * 64,
                "path": path, "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data)}

    snapshot = {"format": "wot-source-snapshot-v1", "git_commit": commit,
                "paths": ["Source", "Tools"],
                "tracked_diff_sha256": hashlib.sha256(b"").hexdigest(),
                "tracked_diff_size": 7 if dirty else 0, "untracked": []}
    snapshot["sha256"] = hashlib.sha256(json.dumps(
        snapshot, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    document = {
        "asset": asset,
        "audit": {"cache_sha256": "a" * 64, "container_count": 3,
                  "staged_tree_receipt_sha256": "b" * 64,
                  "staged_tree_sha256": "d" * 64, "verdict": "pass"},
        "configuration": configuration,
        "format": formats.GUARDED_RELEASE_FORMAT,
        "generated_utc": GENERATED_UTC[platform],
        "git_commit": commit,
        "platform": platform,
        "release_sequence": release_sequence,
        "products": {"receipt": {
            "configuration": configuration, "format": "wot-guarded-product-receipt-v1",
            "platform": platform, "stage_root": stage_root,
            "products": {"WoT": product(main_path)}},
            "receipt_sha256": "f" * 64},
        "sha256": digest,
        "size": size,
        "source_snapshot": snapshot,
        # The real file carries a float here (build_stripped_pak.py:169, 2183):
        # proof that release.json is an input, never a canonical artifact.
        "zen_lifetime": {"receipt": {"monitor": {"interval_seconds": 0.1}},
                         "receipt_sha256": "9" * 64},
    }
    if platform == "Mac":
        document["products"]["receipt"].update({
            "notarization_submission_id": None,
            "signing": "adhoc",
            "stapled": False,
            "team_id": None,
        })
    if configuration == formats.SHIPPING_CONFIGURATION:
        document["test_twin"] = (test_twin if test_twin is not None
                                 else synthetic_test_twin(platform, commit,
                                                          snapshot["sha256"],
                                                          release_sequence))
    if platform == "Mac":
        dmg_name = formats.guarded_install_dmg_asset(commit, configuration)
        dmg_path = directory / dmg_name
        # Cross-host release-plumbing tests need only real bound bytes.  The
        # mac_dmg integration specs build and attach a real signed image.
        dmg_path.write_bytes(("synthetic test DMG " + dmg_name).encode("utf-8"))
        dmg_digest, dmg_size = formats.sha256_file(dmg_path)
        tree_root = formats.project_guarded_archive_tree(
            directory / asset, "Mac").tree_root
        background_digest = formats.sha256_hex(b"synthetic DMG background")
        sidecar_document = {
            "background_sha256": background_digest,
            "format": mac_dmg.DMG_SIDECAR_FORMAT,
            "installed_tree_root": tree_root,
            "name": dmg_name,
            "notarized": notarized,
            "sha256": dmg_digest,
            "size": dmg_size,
            "volume_name": mac_dmg.VOLUME_NAME,
        }
        sidecar_path = directory / mac_dmg.sidecar_name(dmg_name)
        sidecar_path.write_bytes(formats.canonical_bytes(
            sidecar_document, "synthetic DMG sidecar"))
        sidecar_digest, sidecar_size = formats.sha256_file(sidecar_path)
        document["dmg"] = {
            "background_sha256": background_digest,
            "installed_tree_root": tree_root,
            "name": dmg_name,
            "notarized": notarized,
            "sha256": dmg_digest,
            "size": dmg_size,
            "verify_sidecar": {"name": sidecar_path.name,
                               "sha256": sidecar_digest,
                               "size": sidecar_size},
            "volume_name": mac_dmg.VOLUME_NAME,
        }
    path = directory / "release.json"
    path.write_bytes((json.dumps(document, indent=2, sort_keys=True) + "\n")
                     .encode("utf-8"))
    return path


class ReleaseFixture:
    """Temp keys + synthetic trust + two guarded releases + a signed ring."""

    def __init__(self) -> None:
        self.openssl = openssl_or_skip()
        self._temporary = tempfile.TemporaryDirectory(prefix="wot-release-spec-")
        self.root = Path(self._temporary.name)
        keys = self.root / "keys"
        keys.mkdir()
        self.root_key = keys / "root.pem"
        self.announcement_key = keys / "announcement.pem"
        self.release_key = keys / "release.pem"
        self.root_public = crypto.generate_test_key(self.openssl, self.root_key)
        self.announcement_public = crypto.generate_test_key(self.openssl,
                                                            self.announcement_key)
        self.release_public = crypto.generate_test_key(self.openssl, self.release_key)
        self.trust_paths = self._write_trust()
        self.trust = formats.load_trust(*self.trust_paths, None)
        self.compiled = formats.load_compiled_constants()
        self.releases = {
            "Win64": write_guarded_release(self.root / "win64-release", "Win64"),
            "Mac": write_guarded_release(self.root / "mac-release", "Mac"),
        }
        # The passing funnel run on each Shipping release's Test twin.
        self.funnel_receipts = {
            platform: write_funnel_receipt(
                self.root / "funnel" / platform / "funnel-receipt.json", release)
            for platform, release in self.releases.items()}
        self.ring_dir = self.root / "ring"
        sign_release.sign_ring(
            trust=self.trust, openssl=self.openssl, root_key_path=self.root_key,
            root_key_id="offline-root-a",
            keys=[formats.ReleaseKey(RELEASE_KEY_ID, self.release_public, 0,
                                     formats.INT64_MAX)],
            revoked=[], epoch=1, output=self.ring_dir)

    def _write_trust(self):
        trust_dir = self.root / "trust"
        trust_dir.mkdir()
        export = {"ceremony_utc": "2026-09-28T00:00:00+00:00",
                  "format": generator.ANNOUNCEMENT_EXPORT_FORMAT,
                  "key_id": "announcement-v1",
                  "public_key": self.announcement_public.hex()}
        ring = {"epoch": 1, "format": generator.OFFLINE_ROOT_RING_FORMAT,
                "keys": [{"first_sequence": 1, "key_id": "offline-root-a",
                          "last_sequence": formats.INT64_MAX,
                          "public_key": self.root_public.hex()}],
                "revoked_key_ids": []}
        export_raw = generator.canonical_bytes(export)
        ring_raw = generator.canonical_bytes(ring)
        trust = generator.build_trust_document(
            announcement_key=self.announcement_public.hex(), announcement_raw=export_raw,
            root_ring_raw=ring_raw, win64_url=generator.WIN64_ANNOUNCEMENT_URL,
            mac_url=generator.MAC_ANNOUNCEMENT_URL,
            recovery_url=generator.ROOT_KEY_RING_RECOVERY_URL)
        paths = (trust_dir / "bootstrap-trust-v1.json", trust_dir / "public-root-ring.json",
                 trust_dir / "announcement-public-key-v1.json")
        paths[0].write_bytes(generator.canonical_bytes(trust))
        paths[1].write_bytes(ring_raw)
        paths[2].write_bytes(export_raw)
        return paths

    def trust_arguments(self):
        return ["--trust", str(self.trust_paths[0]), "--root-ring", str(self.trust_paths[1]),
                "--announcement-export", str(self.trust_paths[2]), "--no-trust-header"]

    def sign(self, output: Path, **overrides) -> dict:
        release_sequence = overrides.get("release_sequence", 0)
        if release_sequence != 0 and "release_jsons" not in overrides:
            releases = {
                platform: write_guarded_release(
                    self.root / f"sequence-{release_sequence}-{platform}",
                    platform, release_sequence=release_sequence)
                for platform in formats.PLATFORMS}
            overrides["release_jsons"] = releases
            if "funnel_receipts" not in overrides:
                overrides["funnel_receipts"] = {
                    platform: write_funnel_receipt(
                        self.root / f"sequence-{release_sequence}-funnel" / platform
                        / "funnel-receipt.json", release, include_update_cells=True)
                    for platform, release in releases.items()}
        arguments = dict(
            trust=self.trust, compiled=self.compiled, openssl=self.openssl,
            release_jsons=dict(self.releases),
            key_ring=self.ring_dir / formats.KEY_RING_ASSET,
            announcement_key_path=self.announcement_key,
            release_key_path=self.release_key, release_key_id=RELEASE_KEY_ID,
            release_sequence=0, human_version="0.1.0", tag="v0.1.0", output=output,
            funnel_receipts=dict(self.funnel_receipts), inspect_mac_dmg=False)
        arguments.update(overrides)
        if "cache_gates" not in overrides:
            arguments["cache_gates"] = self.write_cache_gates(
                arguments["release_jsons"], Path(output).name)
        return sign_release.sign_release(**arguments)

    def write_cache_gates(self, release_jsons: Dict[str, Path], label: str,
                          **options) -> Dict[str, Path]:
        """A passing gate A receipt per release, beside the fixture's evidence.

        A release.json that cannot be read gets none; signing refuses that
        release for its own reason first.
        """
        gates = {}
        for platform, release_json in release_jsons.items():
            path = Path(release_json)
            path = path / "release.json" if path.is_dir() else path
            try:
                gates[platform] = write_cache_gate_receipt(
                    self.root / "cache-gates" / label / platform / "cache-gate-a.json",
                    path, **options)
            except (OSError, ValueError, KeyError, TypeError,
                    cache_lineage.GateError):
                continue
        return gates

    def cleanup(self) -> None:
        self._temporary.cleanup()


def copy_set(source: Path, destination: Path) -> Path:
    shutil.copytree(source, destination)
    return destination
