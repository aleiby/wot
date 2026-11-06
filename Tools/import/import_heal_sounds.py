"""Patch authored heal-zone sounds into already-imported Battle maps.

Reads each map's generated ``exports/zones/<Map>.zones.json`` so the live map
stamp stays driven by retail package data. The converter folds UE1 sound groups:
``Angreal.Heal.ActivateHL`` becomes ``Angreal/ActivateHL``.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_heal_sounds.py [<Map> ...]"

The commandlet writes ``Saved/import_heal_sounds_result.json`` because stdout
and unreal.log output are not reliable completion signals in this environment.
"""
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import exports_dir  # noqa: E402
ZONE_EXPORTS = exports_dir(required=False) / "zones"
RESULT_FILE = REPO / "Saved" / "import_heal_sounds_result.json"

BATTLE_MAPS = [
    "Battle2_01", "Battle2_02", "Battle2_03", "Battle2_04", "Battle2_05",
    "Battle3_01", "Battle3_02", "Battle4_01",
]


def plan_map(map_name):
    """Return ``[{label, heal_sound}]`` from generated retail zone data."""
    path = ZONE_EXPORTS / f"{map_name}.zones.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    stamps = []
    for zone in data.get("zones", []):
        if not zone.get("heal"):
            continue
        sound = zone.get("heal_sound", "")
        if not sound:
            raise ValueError(
                f"{path}: heal zone {zone.get('actor')!r} has no heal_sound; "
                "regenerate zones JSON with Tools/convert/zones.py")
        # All eight shipped Battle maps currently author this value. Keep the
        # assertion as a census alarm while stamping the extracted value below.
        assert sound == "Angreal/ActivateHL", (
            f"{path}: unexpected authored heal sound {sound!r}")
        stamps.append({
            "label": f"Zone_{zone['actor']}",
            "heal_sound": sound,
        })
    return stamps


def main():
    import unreal

    maps = [name for name in sys.argv[1:] if not name.startswith("-")] \
        or BATTLE_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    results = {}
    errors = []
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        result = {"planned": 0, "stamped": 0, "missing": 0, "saved": False}
        results[map_name] = result
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            result["skip"] = "no imported map"
            unreal.log(f"HEALSOUND {map_name} skipped: no imported map")
            continue
        if not les.load_level(asset):
            result["skip"] = "load_level failed"
            unreal.log_error(f"HEALSOUND {map_name}: load_level failed")
            continue

        try:
            plan = plan_map(map_name)
        except (AssertionError, OSError, ValueError, json.JSONDecodeError) as exc:
            result["error"] = str(exc)
            errors.append(f"{map_name}: {exc}")
            unreal.log_error(f"HEALSOUND {map_name}: {exc}")
            continue
        result["planned"] = len(plan)
        by_label = {}
        for actor in eas.get_all_level_actors():
            by_label.setdefault(actor.get_actor_label(), actor)
        for stamp in plan:
            actor = by_label.get(stamp["label"])
            if not isinstance(actor, unreal.WoTZoneVolume) \
                    or not actor.get_editor_property("heal_zone"):
                result["missing"] += 1
                unreal.log_error(
                    f"HEALSOUND {map_name}: missing heal zone {stamp['label']}")
                continue
            actor.set_editor_property("heal_sound_path", stamp["heal_sound"])
            result["stamped"] += 1

        result["saved"] = bool(les.save_current_level())
        unreal.log(
            f"HEALSOUND {map_name}: stamped={result['stamped']}/"
            f"{result['planned']} missing={result['missing']} "
            f"saved={result['saved']}")

    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULT_FILE.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    if errors:
        raise RuntimeError(
            "heal-sound patch failed; see result file: " + "; ".join(errors))


if __name__ == "__main__":
    main()
