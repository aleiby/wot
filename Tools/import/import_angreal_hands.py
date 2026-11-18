"""Build /Game/WoT/Data/DT_WoTAngrealHands from the shipped hand-order CSV.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script=".../import_angreal_hands.py [angreal_hands.csv]"

The FWoTAngrealHandRow C++ row struct must be compiled before this is run.
Reruns refill and save the table in place.
"""
import sys
from pathlib import Path

import unreal

SOURCE = Path(sys.argv[1] if len(sys.argv) > 1
              else Path(__file__).parent / "data" / "angreal_hands.csv")
DEST_DIR = "/Game/WoT/Data"
NAME = "DT_WoTAngrealHands"
PATH = f"{DEST_DIR}/{NAME}"

EAL = unreal.EditorAssetLibrary
ATT = unreal.AssetToolsHelpers.get_asset_tools()

if not EAL.does_directory_exist(DEST_DIR):
    EAL.make_directory(DEST_DIR)

if EAL.does_asset_exist(PATH):
    dt = unreal.load_asset(PATH)
else:
    factory = unreal.DataTableFactory()
    factory.struct = unreal.load_object(None, "/Script/WoT.WoTAngrealHandRow")
    dt = ATT.create_asset(NAME, DEST_DIR, unreal.DataTable, factory)

csv_text = SOURCE.read_text()
ok = unreal.DataTableFunctionLibrary.fill_data_table_from_csv_string(dt, csv_text)
row_count = len(unreal.DataTableFunctionLibrary.get_data_table_row_names(dt))
EAL.save_asset(PATH)
unreal.log(f"IMPORT-ANGREAL-HANDS-{'OK' if ok and row_count == 42 else 'BAD'} "
           f"rows={row_count}/42")
if not ok or row_count != 42:
    raise RuntimeError(f"DataTable fill failed: ok={ok} rows={row_count}/42")
