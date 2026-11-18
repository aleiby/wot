"""Stamp LevelInfo.Brightness onto existing per-map light-table assets.

The full importer now writes this field while creating LT_<Map>. This
pythonscript commandlet is the idempotent migration for already-imported MP
content:

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...patch_level_brightness.py [<Map> ...]"

Authored values come from exports/t3d, never from a hand-maintained content
list. A census assertion pins the shipped MP set: Arena_09=1.1, all others=1.
"""

import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import exports_dir  # noqa: E402
EXPORTS = Path(os.environ.get("WOT_EXPORTS_DIR", exports_dir(required=False)))
T3D_DIR = EXPORTS / "t3d"
RESULT_FILE = REPO / "Saved" / "patch_level_brightness_result.json"

MP_MAPS = [
    *(f"Arena_{index:02d}" for index in (1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12)),
    *(f"Battle2_{index:02d}" for index in range(1, 6)),
    "Battle3_01", "Battle3_02", "Battle4_01",
]


def authored_level_brightness(map_name):
    sys.path.insert(0, str(REPO / "Tools" / "convert"))
    from t3d import parse

    path = T3D_DIR / f"{map_name}.t3d"
    level = next(
        (actor for actor in parse(path)
         if actor["cls"].casefold() == "levelinfo"),
        None)
    if level is None:
        raise ValueError(f"{path}: no LevelInfo actor")
    return float(level["props"].get("Brightness", 1.0))


def plan_maps(map_names=MP_MAPS):
    plan = {
        map_name: authored_level_brightness(map_name)
        for map_name in map_names
    }
    if tuple(map_names) == tuple(MP_MAPS):
        nondefault = {
            name: value for name, value in plan.items()
            if abs(value - 1.0) > 1.0e-6
        }
        if nondefault != {"Arena_09": 1.1}:
            raise ValueError(
                "MP LevelInfo.Brightness census drifted: "
                + repr(nondefault))
    return plan


def main():
    import unreal

    maps = [value for value in sys.argv[1:] if not value.startswith("-")] \
        or MP_MAPS
    plan = plan_maps(maps)
    results = {}
    for map_name, brightness in plan.items():
        path = f"/Game/WoT/Maps/LightTables/LT_{map_name}"
        row = {"brightness": brightness, "saved": False}
        results[map_name] = row
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            row["skip"] = "no imported light table"
            unreal.log(
                f"LEVELBRIGHTNESS {map_name} skipped: no imported light table")
            continue
        table = unreal.load_asset(path)
        if not isinstance(table, unreal.WoTMapLightTable):
            row["error"] = "asset is not WoTMapLightTable"
            unreal.log_error(
                f"LEVELBRIGHTNESS {map_name}: wrong asset type at {path}")
            continue
        table.set_editor_property("level_brightness", float(brightness))
        row["saved"] = bool(unreal.EditorAssetLibrary.save_asset(path))
        unreal.log(
            f"LEVELBRIGHTNESS {map_name}: {brightness:g} "
            f"saved={row['saved']}")

    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULT_FILE.write_text(
        json.dumps(results, indent=2) + "\n", encoding="utf-8")
    errors = [
        f"{name}: {row['error']}"
        for name, row in results.items() if "error" in row
    ]
    if errors:
        raise RuntimeError("; ".join(errors))


if __name__ == "__main__":
    main()
