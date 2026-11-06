"""Build /Game/WoT/Anim/DT_WoTAnimSequences from Intermediate/Conv/mesh/*.meta.json.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script=".../import_animdata.py [conv_mesh_dir]"

One row per converted mesh (row name = mesh name); Sequences = the #exec MESH SEQUENCE
table mesh2glb emitted into the mesh's meta.json. Reruns rebuild the table in place.
The row struct is C++ (FWoTAnimSequenceSetRow) — build the module before running this.
"""
import json
import sys
from pathlib import Path

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import local_path  # noqa: E402

RAW_ARGS = sys.argv[1:]
RELEASE_GATED = "--stripped" in RAW_ARGS
POSITIONALS = [argument for argument in RAW_ARGS
               if not argument.startswith("--")]
CONV = Path(POSITIONALS[0] if POSITIONALS
            else local_path("conversion_root") / "mesh")
DEST_DIR = "/Game/WoT/Anim"
NAME = "DT_WoTAnimSequences"
PATH = f"{DEST_DIR}/{NAME}"

EAL = unreal.EditorAssetLibrary
ATT = unreal.AssetToolsHelpers.get_asset_tools()

if RELEASE_GATED:
    carriers = []
    for meta_file in sorted(CONV.glob("*.meta.json")):
        meta = json.loads(meta_file.read_text())
        logical_id = meta.get("animation_logical_id")
        if (not isinstance(logical_id, str)
                or not logical_id.startswith("cache/animation/")):
            raise RuntimeError(
                f"{meta_file}: stripped animation carrier lacks an exact "
                "cache-v3 logical id")
        carriers.append(logical_id)
    if len(carriers) != len(set(carriers)):
        raise RuntimeError("stripped animation carrier logical ids collide")
    unreal.log(
        f"IMPORT-ANIMDATA-OK carriers={len(carriers)} mode=guarded")
    raise SystemExit(0)

rows = []
for meta_file in sorted(CONV.glob("*.meta.json")):
    meta = json.loads(meta_file.read_text())
    seqs = meta.get("sequences") or {}
    if not seqs:
        continue
    # #exec MESH NOTIFY rows, grouped onto their sequence (meta notify 'seq' casing
    # follows the .uc text; sequence keys are the sheet's — match case-insensitively).
    notifies = {}
    for n in meta.get("notifies") or []:
        notifies.setdefault(n["seq"].upper(), []).append(
            {"Fraction": n["time"], "Function": n["func"]})
    rows.append({
        "Name": meta_file.name[:-len(".meta.json")],
        "Sequences": [
            {"Name": n, "Group": s.get("group", "None"),
             "StartFrame": s["start"], "NumFrames": s["num"], "Rate": s["rate"],
             "Notifies": notifies.get(n.upper(), [])}
            for n, s in seqs.items()
        ],
    })

if EAL.does_asset_exist(PATH):
    dt = unreal.load_asset(PATH)
else:
    factory = unreal.DataTableFactory()
    factory.struct = unreal.load_object(None, "/Script/WoT.WoTAnimSequenceSetRow")
    dt = ATT.create_asset(NAME, DEST_DIR, unreal.DataTable, factory)

ok = unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(dt, json.dumps(rows))
n = len(unreal.DataTableFunctionLibrary.get_data_table_row_names(dt))
EAL.save_asset(PATH)
unreal.log(f"IMPORT-ANIMDATA-{'OK' if ok and n == len(rows) else 'BAD'} rows={n}/{len(rows)}")
if not ok or n != len(rows):
    raise RuntimeError(f"DataTable fill failed: ok={ok} rows={n}/{len(rows)}")
