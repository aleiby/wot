#!/usr/bin/env python3
"""Remove retail-source seed arrays from the guarded fractal catalog.

This file is a direct Unreal PythonScript commandlet entry point and therefore
executes on load, matching the repository's importer leaf scripts. Pure logic
lives in ``stripped_fractal_catalog`` so normal unit-test imports stay inert.

The guarded flipbook importer deliberately preserves the seeded runtime
catalog. That catalog predates the release gate and can therefore still hold
retail p8 arrays even though the composed import sets
``WOT_STRIP_FRACTAL_SEED=1``. Run this in a fresh Editor process after the
composed import so the guarded artifact contains only records the runtime can
rehydrate from cache-v3.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from stripped_fractal_catalog import (  # noqa: E402
    CATALOG_PATH,
    strip_retail_source_seeds,
    verify_retail_source_seeds_empty,
)

RECEIPT_RELATIVE = Path(
    "ImportReceipts/finalize-stripped-fractal-catalog.json")


def main() -> None:
    import unreal

    catalog = unreal.load_asset(CATALOG_PATH)
    if not catalog:
        raise RuntimeError(f"guarded fractal catalog is missing: {CATALOG_PATH}")
    records, stripped_records, stripped_bytes = strip_retail_source_seeds(
        catalog.get_editor_property("textures"))
    if not stripped_records:
        raise RuntimeError(
            "guarded fractal catalog contains no retail-source records")
    catalog.set_editor_property("textures", records)
    if unreal.EditorAssetLibrary.save_asset(CATALOG_PATH) is False:
        raise RuntimeError(f"failed to save guarded fractal catalog: {CATALOG_PATH}")

    reloaded = unreal.load_asset(CATALOG_PATH)
    if not reloaded:
        raise RuntimeError(
            f"failed to reload guarded fractal catalog: {CATALOG_PATH}")
    verify_retail_source_seeds_empty(reloaded.get_editor_property("textures"))
    marker = (
        "FINALIZE-STRIPPED-FRACTAL-CATALOG OK "
        f"records={stripped_records} bytes={stripped_bytes}")
    unreal.log(marker)
    receipt = Path(unreal.Paths.project_saved_dir()) / RECEIPT_RELATIVE
    receipt.parent.mkdir(parents=True, exist_ok=True)
    temporary = receipt.with_name(receipt.name + ".tmp")
    temporary.write_text(json.dumps({
        "format": "wot-stripped-fractal-finalize-v1",
        "catalog": CATALOG_PATH,
        "retail_source_records": stripped_records,
        "seed_bytes_removed": stripped_bytes,
        "verdict": "pass",
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(receipt)


main()
