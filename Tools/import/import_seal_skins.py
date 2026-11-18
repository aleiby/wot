"""Generate the four faction seal-skin MICs (G13).

Seal.SetControllingTeam (exports/script/WOT/Seal.uc:88-110) swaps
MultiSkins[1] to S_{AesSedai,Forsaken,Hound,Whitecloak}Seal when a faction
player banks the seal; AWoTSeal::ApplyControllingFaction rebinds the imported
mesh's single slot to the convention MIC MI_S_<Faction>Seal per machine.

Mirror-by-construction (the import_player_skins.py pattern): each faction MIC
takes its PARENT from the existing MI_S_Seal (whatever master the mesh import
converged on). Each faction texture samples raw under the shared world policy
(texture_sampling.py) rather than copying the base S_Seal's setting. The
faction textures are ALREADY in Content (the legacy code-package skin pass);
KeyColor is each texture's palette[0], probed offline from
original/System/WOT.u with Tools/convert/ue1pkg (the opaque mesh master
restores palette-index-zero):
  S_AesSedaiSeal (0,0,0)  S_ForsakenSeal (4,4,4)
  S_HoundSeal (163,151,151)  S_WhitecloakSeal (252,252,252)

Reruns are idempotent.

UnrealEditor-Cmd WoT.uproject -run=pythonscript -script=".../import_seal_skins.py"
"""
import sys
from pathlib import Path

import unreal

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
import texture_sampling  # noqa: E402
from texture_sampling import WORLD, load_texture  # noqa: E402

ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

MI_DEST = "/Game/WoT/Materials/Instances"
TEXROOT = "/Game/WoT/Textures/WOT"

# faction -> palette[0] RGB (WOT.u probe; see module docstring).
FACTIONS = {
    "AesSedai": (0, 0, 0),
    "Forsaken": (4, 4, 4),
    "Hound": (163, 151, 151),
    "Whitecloak": (252, 252, 252),
}


def texture_sampling_claims(_context=None):
    """The four faction seal textures this importer loads: world, raw."""
    return [texture_sampling.claim(f"{TEXROOT}/S_{faction}Seal", WORLD,
                                   "import_seal_skins")
            for faction in FACTIONS]


def main():
    base_mic_path = f"{MI_DEST}/MI_S_Seal"
    if not EAL.does_asset_exist(base_mic_path):
        raise RuntimeError(
            f"base seal MIC missing: {base_mic_path} — import the Seal mesh first")
    base_parent = unreal.load_asset(base_mic_path).get_editor_property("parent")

    made = 0
    for faction, (red, green, blue) in FACTIONS.items():
        tex_path = f"{TEXROOT}/S_{faction}Seal"
        if not EAL.does_asset_exist(tex_path):
            raise RuntimeError(f"faction seal texture missing: {tex_path}")
        texture, _ = load_texture(unreal, tex_path, WORLD)
        mic_path = f"{MI_DEST}/MI_S_{faction}Seal"
        if not EAL.does_asset_exist(mic_path):
            mic = ATT.create_asset(
                f"MI_S_{faction}Seal", MI_DEST,
                unreal.MaterialInstanceConstant,
                unreal.MaterialInstanceConstantFactoryNew())
            made += 1
        else:
            mic = unreal.load_asset(mic_path)
        if mic.get_editor_property("parent") != base_parent:
            mic.set_editor_property("parent", base_parent)
        MEL.set_material_instance_texture_parameter_value(mic, "Tex", texture)
        MEL.set_material_instance_vector_parameter_value(
            mic, "KeyColor",
            unreal.LinearColor(red / 255.0, green / 255.0, blue / 255.0, 1.0))
        EAL.save_asset(mic_path)
    unreal.log(f"SEALSKINS: {made} MICs created, {len(FACTIONS)} stamped")


if __name__ == "__main__":
    main()
