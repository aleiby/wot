"""Import the personal player-skin color variants (P02).

Retail axis: WOTPlayer.SetSkin fills every MultiSkins slot with ONE texture
from the faction's <Class>Skins.utx package (Aes/For/Hou/Whi_{Black,Blue,Red,
Green}); the port's body meshes split that into two typed slots ("<Pre>_Base",
"<Pre>_Base_2S"), so each color variant needs one MIC per slot type,
convention-named MI_<Pre>_<Color>[_2S] — the names AWoTCharacter::
ApplyPersonalSkin rebinds at runtime.

Mirror-by-construction: each variant MIC takes its PARENT from the faction's
existing MI_<Pre>_Base[_2S] (whatever master the mesh import + gamma passes
converged on), and each variant texture takes its srgb setting from the
imported base texture. KeyColor comes from the variant's own manifest
palette[0] (the opaque mesh master restores palette-index-zero). Payload
binding matches the ordinary mesh-texture path.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_player_skins.py <conv_root>"   # conv_root holds tex/
"""
import json
import sys
from pathlib import Path

import unreal

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
CONVERT_DIR = IMPORT_DIR.parent / "convert"
if str(CONVERT_DIR) not in sys.path:
    sys.path.insert(0, str(CONVERT_DIR))
from payload_binding import ensure_texture_payload_binding
from mesh_texture_variants import palette0_rgb

ARGS = [a for a in sys.argv[1:] if not a.startswith("-")]
if not ARGS:
    raise RuntimeError("import_player_skins.py requires the conv root")
TEXROOT = Path(ARGS[0]) / "tex"

ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

# (skin .utx package, texture prefix, body-mesh base texture) per faction —
# exports/script/WoTPawns/*.uc defaultproperties + Textures/<Class>Skins.utx.
FACTIONS = [
    ("AesSedaiSkins", "Aes"),
    ("ForsakenSkins", "For"),
    ("HoundSkins", "Hou"),
    ("WhitecloakSkins", "Whi"),
]
COLORS = ["Black", "Blue", "Red", "Green"]
SLOT_SUFFIXES = ["", "_2S"]   # the faction meshes' two slot types (meta census)

n_tex = n_mic = 0
for package, prefix in FACTIONS:
    manifest_path = TEXROOT / package / "_manifest.json"
    manifest = json.loads(manifest_path.read_text()).get("textures", {})
    base_texture = None
    base_path = f"/Game/WoT/Textures/WoTPawns/{prefix}_Base"
    if EAL.does_asset_exist(base_path):
        base_texture = unreal.load_asset(base_path)
    base_parents = {}
    for suffix in SLOT_SUFFIXES:
        base_mic_path = f"/Game/WoT/Materials/Instances/MI_{prefix}_Base{suffix}"
        if not EAL.does_asset_exist(base_mic_path):
            raise RuntimeError(
                f"base skin MIC missing: {base_mic_path} — import the faction "
                "meshes before the player-skin variants")
        base_parents[suffix] = unreal.load_asset(base_mic_path).get_editor_property(
            "parent")
    for color in COLORS:
        name = f"{prefix}_{color}"
        info = manifest.get(name)
        if info is None:
            raise RuntimeError(f"{package} manifest is missing {name}")
        png = TEXROOT / package / f"{name}.png"
        if not png.is_file():
            raise RuntimeError(f"variant PNG missing: {png}")
        tex_path = f"/Game/WoT/Textures/{package}/{name}"
        if not EAL.does_asset_exist(tex_path):
            task = unreal.AssetImportTask()
            task.filename = str(png)
            task.destination_path = f"/Game/WoT/Textures/{package}"
            task.automated = True
            task.save = True
            ATT.import_asset_tasks([task])
            n_tex += 1
        ensure_texture_payload_binding(unreal, tex_path, package, name)
        texture = unreal.load_asset(tex_path)
        if base_texture is not None:
            base_srgb = base_texture.get_editor_property("srgb")
            if texture.get_editor_property("srgb") != base_srgb:
                texture.set_editor_property("srgb", base_srgb)
                EAL.save_asset(tex_path)
        for suffix in SLOT_SUFFIXES:
            mic_path = f"/Game/WoT/Materials/Instances/MI_{name}{suffix}"
            if not EAL.does_asset_exist(mic_path):
                mic = ATT.create_asset(
                    f"MI_{name}{suffix}", "/Game/WoT/Materials/Instances",
                    unreal.MaterialInstanceConstant,
                    unreal.MaterialInstanceConstantFactoryNew())
                n_mic += 1
            else:
                mic = unreal.load_asset(mic_path)
            if mic.get_editor_property("parent") != base_parents[suffix]:
                mic.set_editor_property("parent", base_parents[suffix])
            MEL.set_material_instance_texture_parameter_value(
                mic, "Tex", texture)
            red, green, blue = palette0_rgb(info)
            MEL.set_material_instance_vector_parameter_value(
                mic, "KeyColor",
                unreal.LinearColor(red / 255.0, green / 255.0, blue / 255.0, 1.0))
            EAL.save_asset(mic_path)

unreal.log(f"PLAYERSKINS: {n_tex} textures imported, {n_mic} MICs created "
           f"({len(FACTIONS)}x{len(COLORS)} variants x {len(SLOT_SUFFIXES)} slots)")
