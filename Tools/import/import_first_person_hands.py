"""Import the first-person hand art (Art/hands) into editor content.

UnrealEditor-Cmd WoT.uproject -run=pythonscript -script="<this file> [Art/hands dir]"

Per class (channeler, darklord, hound, zealot):
- <class>.glb through the default Interchange importer into
  /Game/WoT/FirstPerson/Hands/<class> (skeletal mesh, skeleton, material).
- The five team-colour textures as T_<class>_<colour> with srgb off: the
  gamma-space pipeline samples every texture raw (import_map.py).

Writes Saved/ImportReceipts/first-person-hands.json naming the skeletal mesh
and texture paths, which the game's hand factory loads by convention.
Reruns replace the imported assets.
"""
import json
import sys
import traceback
from pathlib import Path

import unreal

REPO = Path(__file__).resolve().parents[2]
ART = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "Art" / "hands"
DEST_ROOT = "/Game/WoT/FirstPerson/Hands"
CLASSES = ("channeler", "darklord", "hound", "zealot")
COLOURS = ("base", "black", "blue", "red", "green")

EAL = unreal.EditorAssetLibrary
ATT = unreal.AssetToolsHelpers.get_asset_tools()


def import_file(path, dest, name=None):
    task = unreal.AssetImportTask()
    task.filename = str(path)
    task.destination_path = dest
    if name:
        task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = True
    ATT.import_asset_tasks([task])
    return list(task.imported_object_paths)


receipt = {"art": str(ART), "classes": {}}
ok = True
for cls in CLASSES:
    dest = f"{DEST_ROOT}/{cls}"
    entry = {}
    try:
        glb = ART / cls / f"{cls}.glb"
        if not glb.exists():
            raise RuntimeError(f"missing {glb}")
        import_file(glb, dest)
        meshes = []
        for path in EAL.list_assets(dest, recursive=True):
            asset = EAL.load_asset(path)
            if isinstance(asset, unreal.SkeletalMesh):
                meshes.append(asset.get_path_name())
        if len(meshes) != 1:
            raise RuntimeError(f"{cls}: expected one skeletal mesh, found {meshes}")
        entry["mesh"] = meshes[0]
        textures = {}
        for colour in COLOURS:
            png = ART / cls / f"{cls}_{colour}.png"
            if not png.exists():
                raise RuntimeError(f"missing {png}")
            name = f"T_{cls}_{colour}"
            import_file(png, dest, name)
            tex_path = f"{dest}/{name}"
            tex = unreal.load_asset(tex_path)
            if tex.get_editor_property("srgb"):
                tex.set_editor_property("srgb", False)
            EAL.save_asset(tex_path)
            textures[colour] = tex.get_path_name()
        entry["textures"] = textures
        entry["ok"] = True
    except Exception:
        ok = False
        entry["ok"] = False
        entry["error"] = traceback.format_exc()
    receipt["classes"][cls] = entry

receipt["ok"] = ok
out = REPO / "Saved" / "ImportReceipts" / "first-person-hands.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(receipt, indent=1))
unreal.log(f"FIRST-PERSON-HANDS {'OK' if ok else 'FAILED'} -> {out}")
if not ok:
    raise RuntimeError("first-person hand import failed; see the receipt")
