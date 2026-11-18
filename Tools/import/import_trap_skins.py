"""Create the WallSlab stage-skin material instances (Slice 6b).

Wall.uc swaps the slab texture per damage stage (Texture'WoTTraps.TrapWall2'
at stage 2, 'TrapWall3' at stage 3; stage 1 keeps the mesh's base skin).
import_mesh.py only builds MIs for skins a mesh references directly, so the
two swap-target skins get theirs here — same convention: MI_<Texture> under
/Game/WoT/Materials/Instances, parent M_WoT_Master, diffuse in "Tex".

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script=".../import_trap_skins.py <converted-texture-dir>"
"""
import os
import sys
from pathlib import Path

import unreal

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
from payload_binding import ensure_texture_payload_binding
import texture_sampling
from texture_sampling import WORLD, import_texture

REPO = Path(__file__).resolve().parents[2]
TEXROOT = Path(
    sys.argv[1] if len(sys.argv) > 1
    else (Path(os.environ.get(
        "WOT_CONV_DIR", REPO / "Intermediate" / "Conv")) / "tex")
)

ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary

MASTER = "/Game/WoT/Materials/M_WoT_Master"
INSTANCES = "/Game/WoT/Materials/Instances"
# (texture package dir, name): MI_<name> parented to the master. TrapWall2/3 are
# the WallSlab damage stages (6b); LB_A02-10 the randomized LSbolt beam-segment
# skins (6a-iv; LB_A01 rides the mesh import).
SKINS = [("WoTTraps", "TrapWall2"), ("WoTTraps", "TrapWall3")] + [
    ("Angreal", f"LB_A{i:02d}") for i in range(2, 11)]


def texture_sampling_claims(_context=None):
    """Every stage/beam skin this importer acquires: world textures, raw."""
    return [texture_sampling.claim(f"/Game/WoT/Textures/{pkg}/{skin}", WORLD,
                                   "import_trap_skins")
            for pkg, skin in SKINS]


def load_or_import_texture(pkg, skin):
    """Load (or import) one skin and converge it to the raw world sampling;
    an existing texture is no longer returned without that check."""
    tex_path = f"/Game/WoT/Textures/{pkg}/{skin}"
    existed = EAL.does_asset_exist(tex_path)
    png = TEXROOT / pkg / f"{skin}.png"
    if not existed and not png.is_file():
        raise RuntimeError(f"texture missing and converted PNG not found: {png}")
    tex, _ = import_texture(unreal, png, tex_path, WORLD)
    ensure_texture_payload_binding(unreal, tex_path, pkg, skin)
    return tex, not existed


def main():
    master = unreal.load_asset(MASTER)
    if not master:
        raise RuntimeError(f"master material missing: {MASTER}")
    made = imported = 0
    for pkg, skin in SKINS:
        tex, was_imported = load_or_import_texture(pkg, skin)
        imported += was_imported
        mi_path = f"{INSTANCES}/MI_{skin}"
        if EAL.does_asset_exist(mi_path):
            mi = unreal.load_asset(mi_path)
        else:
            mi = ATT.create_asset(f"MI_{skin}", INSTANCES,
                                  unreal.MaterialInstanceConstant,
                                  unreal.MaterialInstanceConstantFactoryNew())
            mi.set_editor_property("parent", master)
            made += 1
        # Idempotent: an existing, correctly-bound instance is left untouched so
        # reruns don't churn committed binary assets (SB-D10 lesson).
        bound = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(
            mi, "Tex")
        if bound != tex:
            unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
                mi, "Tex", tex)
            EAL.save_asset(mi_path)
    unreal.log(
        f"import_trap_skins: {imported} textures imported, {made} instances "
        f"created, {len(SKINS)} bound OK")


if __name__ == "__main__":
    main()
