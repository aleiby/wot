"""Import the first-person hand art (Art/hands) into editor content.

UnrealEditor-Cmd WoT.uproject -run=pythonscript -script="<this file> [Art/hands dir]"

Per class (channeler, darklord, hound, zealot):
- <class>.glb through the default Interchange importer into
  /Game/WoT/FirstPerson/Hands/<class> (skeletal mesh, skeleton, material).
- The five team-colour textures as T_<class>_<colour>. The game draws them
  with the lit mesh master, so they are world textures: raw in gamma space
  (texture_sampling.py), like every other mesh skin.

Writes Saved/ImportReceipts/first-person-hands.json naming the skeletal mesh
and texture paths, which the game's hand factory loads by convention.
Reruns replace the imported assets.
"""
import json
import sys
import traceback
from pathlib import Path

import unreal

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
import texture_sampling
from texture_sampling import WORLD, import_texture

REPO = Path(__file__).resolve().parents[2]
DEST_ROOT = "/Game/WoT/FirstPerson/Hands"
CLASSES = ("channeler", "darklord", "hound", "zealot")
COLOURS = ("base", "black", "blue", "red", "green")

EAL = unreal.EditorAssetLibrary


def texture_path(cls, colour):
    return f"{DEST_ROOT}/{cls}/T_{cls}_{colour}"


def texture_sampling_claims(_context=None):
    """Every team-colour hand texture this importer acquires: world, raw."""
    return [texture_sampling.claim(texture_path(cls, colour), WORLD,
                                   "import_first_person_hands")
            for cls in CLASSES for colour in COLOURS]


def import_mesh(glb, dest):
    """The glTF skeletal mesh (mesh, skeleton, material) through Interchange."""
    task = unreal.AssetImportTask()
    task.filename = str(glb)
    task.destination_path = dest
    task.automated = True
    task.replace_existing = True
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])


def import_class(art, cls):
    dest = f"{DEST_ROOT}/{cls}"
    glb = art / cls / f"{cls}.glb"
    if not glb.exists():
        raise RuntimeError(f"missing {glb}")
    import_mesh(glb, dest)
    meshes = []
    for path in EAL.list_assets(dest, recursive=True):
        asset = EAL.load_asset(path)
        if isinstance(asset, unreal.SkeletalMesh):
            meshes.append(asset.get_path_name())
    if len(meshes) != 1:
        raise RuntimeError(f"{cls}: expected one skeletal mesh, found {meshes}")
    textures = {}
    for colour in COLOURS:
        png = art / cls / f"{cls}_{colour}.png"
        if not png.exists():
            raise RuntimeError(f"missing {png}")
        texture, _ = import_texture(unreal, png, texture_path(cls, colour), WORLD,
                                    replace_existing=True)
        textures[colour] = texture.get_path_name()
    return {"mesh": meshes[0], "textures": textures, "ok": True}


def main():
    art = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "Art" / "hands"
    receipt = {"art": str(art), "classes": {}}
    ok = True
    for cls in CLASSES:
        try:
            entry = import_class(art, cls)
        except Exception:
            ok = False
            entry = {"ok": False, "error": traceback.format_exc()}
        receipt["classes"][cls] = entry
    receipt["ok"] = ok
    out = REPO / "Saved" / "ImportReceipts" / "first-person-hands.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(receipt, indent=1))
    unreal.log(f"FIRST-PERSON-HANDS {'OK' if ok else 'FAILED'} -> {out}")
    if not ok:
        raise RuntimeError("first-person hand import failed; see the receipt")


if __name__ == "__main__":
    main()
