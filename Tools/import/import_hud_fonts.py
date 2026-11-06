"""Extract the shipped UE1 HUD fonts and build offline UE5 UFont assets.

The optional stripped mode substitutes opaque-magenta, correct-size one-mip
atlas pages while retaining the original WOT texture key. It requires an
explicit isolated destination and is failure-atomic for that destination.

UnrealEditor-Cmd WoT.uproject -run=pythonscript \
  -script=".../Tools/import/import_hud_fonts.py" -unattended -nullrhi
"""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import unreal

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
sys.path.insert(0, str(IMPORT_DIR.parent))
from wot_config import original_dir  # noqa: E402
from payload_binding import ensure_texture_payload_binding
from import_transaction import run_isolated_import_transaction
from stripped_texture_import import (
    import_stripped_texture,
    importer_positionals,
    prevalidate_stripped_textures,
    stripped_destination,
    stripped_mode_requested,
)


def system_python():
    """Locate the SYSTEM interpreter for the converter subprocess (needs PIL).
    Under UnrealEditor's embedded Python, sys.executable is the editor binary —
    not a Python — and stock Windows ships no `python3` shim, so probe both."""
    for name in ("python3", "python"):
        exe = shutil.which(name)
        if exe:
            return exe
    raise RuntimeError("no python3/python on PATH for the font converter")


ROOT = Path(__file__).resolve().parents[2]
RAW_ARGS = sys.argv[1:]
POSITIONALS = importer_positionals(RAW_ARGS)
RETAIL_ROOT = Path(POSITIONALS[0]) if POSITIONALS else original_dir()
CONVERTED_ROOT = Path(POSITIONALS[1] if len(POSITIONALS) > 1 else
                      ROOT / "Saved" / "HUDFontImport")
CONVERTER = ROOT / "Tools/convert/ue1pkg.py"
CANONICAL_DESTINATION = "/Game/WoT/UI/Fonts"
STRIPPED = stripped_mode_requested(RAW_ARGS)
DESTINATION = stripped_destination(
    RAW_ARGS, CANONICAL_DESTINATION, "HUD font texture") \
    if STRIPPED else CANONICAL_DESTINATION
DATA_ASSET_PATH = f"{DESTINATION}/DA_WoTHUDFonts"
# WOTCanvas.uc:13-18 is deliberately asymmetric: the two small menu faces have
# no _S exports. Keep that fact visible instead of manufacturing names.
WOT_FONT_SPECS = [
    ("F_Charge", True),
    ("F_Key", True),
    ("F_KeySelected", True),
    ("F_WOTReg14", True),
    ("F_WOTReg30", True),
    ("F_WOTReg08", False),
    ("F_WOTIta14", False),
]
WOT_FONT_NAMES = [
    variant
    for base_name, has_small in WOT_FONT_SPECS
    for variant in ([base_name, f"{base_name}_S"] if has_small else [base_name])
]
TAHOMA_FONT_NAMES = [
    "Tahoma10", "Tahoma20", "Tahoma30",
    "TahomaB10", "TahomaB20", "TahomaB30",
]
UTFONT_CENSUS_NAMES = ["UTFont12", "UTFont24", "UTFont40"]
# Package font censuses are parent-verified. F_Element is the thirteenth WOT.u
# font but remains outside this registry; the UTFont trio is likewise unused by
# UWindowRootWindow.SetupFonts (UWindowRootWindow.uc:339-355).
FONT_PACKAGES = (
    {
        "id": "WOT",
        "source": RETAIL_ROOT / "System" / "WOT.u",
        "imports": WOT_FONT_NAMES,
        "census": WOT_FONT_NAMES + ["F_Element"],
    },
    {
        "id": "UWindowFonts",
        "source": RETAIL_ROOT / "Textures" / "UWindowFonts.utx",
        "imports": TAHOMA_FONT_NAMES,
        "census": TAHOMA_FONT_NAMES + UTFONT_CENSUS_NAMES,
    },
)
NAMES = [name for package in FONT_PACKAGES for name in package["imports"]]
FONT_PACKAGE_BY_NAME = {
    name: package["id"]
    for package in FONT_PACKAGES
    for name in package["imports"]
}

ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary


def list_package_fonts(source_package):
    """Read one source export table without decoding or writing any content."""
    code = (
        "import json,sys;"
        f"sys.path.insert(0,{str(CONVERTER.parent)!r});"
        "from ue1pkg import Package;"
        f"p=Package({str(source_package)!r});"
        "print(json.dumps(sorted(e['name'] for e in p.exports "
        "if p.objname(e['cls'])=='Font')))"
    )
    result = subprocess.run(
        [system_python(), "-c", code],
        check=True,
        capture_output=True,
        text=True,
    )
    return set(json.loads(result.stdout))


def extract_and_validate():
    decoded_fonts = {}
    for package in FONT_PACKAGES:
        package_fonts = list_package_fonts(package["source"])
        expected_fonts = set(package["census"])
        missing_exports = sorted(expected_fonts - package_fonts)
        unexpected_exports = sorted(package_fonts - expected_fonts)
        if missing_exports or unexpected_exports:
            raise RuntimeError(
                f"{package['source'].name} font census disagrees with the "
                "parent-verified retail census: "
                f"missing={missing_exports}, unexpected={unexpected_exports}")

        package_root = CONVERTED_ROOT / package["id"]
        package_root.mkdir(parents=True, exist_ok=True)
        manifest_path = package_root / "_manifest.json"
        manifest_fonts = {}
        if manifest_path.exists():
            manifest_fonts = json.loads(
                manifest_path.read_text()).get("fonts", {})
        if not set(package["imports"]).issubset(manifest_fonts):
            subprocess.run(
                [system_python(), str(CONVERTER), "fonts",
                 str(package["source"]), str(package_root),
                 *package["imports"]],
                check=True,
            )
        if not manifest_path.exists():
            raise RuntimeError(f"font decoder did not write {manifest_path}")
        manifest = json.loads(manifest_path.read_text())
        missing = [
            name for name in package["imports"]
            if name not in manifest.get("fonts", {})
        ]
        if missing:
            raise RuntimeError(
                f"{package['source'].name} fonts missing from manifest: "
                f"{missing}")
        decoded_fonts.update({
            name: manifest["fonts"][name] for name in package["imports"]
        })
    return decoded_fonts


def import_atlas(font_name, page, package):
    png = CONVERTED_ROOT / package / page["png"]
    if not png.exists():
        raise RuntimeError(f"missing decoded font atlas: {png}")
    asset_name = f"T_{font_name}_{page['page']}"
    asset_path = f"{DESTINATION}/{asset_name}"
    if STRIPPED:
        texture, _ = import_stripped_texture(
            unreal, {
                "source_png": png,
                "asset_path": asset_path,
                "package": package,
                "source_name": page["source_texture"],
                "width": int(page["w"]),
                "height": int(page["h"]),
                "destination_mip_count": 1,
            },
            (("filter", unreal.TextureFilter.TF_NEAREST),
             ("compression_settings",
              unreal.TextureCompressionSettings.TC_EDITOR_ICON),
             ("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI),
             ("mip_gen_settings",
              unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)))
        return texture
    changed = False
    if not EAL.does_asset_exist(asset_path):
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = DESTINATION
        task.destination_name = asset_name
        task.automated = True
        task.save = False
        ATT.import_asset_tasks([task])
        changed = True
    texture = unreal.load_asset(asset_path)
    if not texture:
        raise RuntimeError(f"failed to import {asset_path}")
    for prop, value in (
            ("filter", unreal.TextureFilter.TF_NEAREST),
            ("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON),
            ("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI),
            ("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)):
        if texture.get_editor_property(prop) != value:
            texture.set_editor_property(prop, value)
            changed = True
    _, binding_changed = ensure_texture_payload_binding(
        unreal, asset_path, package, page["source_texture"], 1, save=False)
    if changed or binding_changed:
        EAL.save_asset(asset_path)
    return texture


def create_font_asset(font_name):
    asset_path = f"{DESTINATION}/{font_name}"
    if EAL.does_asset_exist(asset_path):
        font = unreal.load_asset(asset_path)
    else:
        factory = unreal.FontFactory()
        font = ATT.create_asset(font_name, DESTINATION, unreal.Font, factory)
    if not font:
        raise RuntimeError(f"failed to create {asset_path}")
    return font, asset_path


def populate_font(font_name, decoded):
    pages = sorted(decoded["pages"], key=lambda item: item["page"])
    if [page["page"] for page in pages] != list(range(len(pages))):
        raise RuntimeError(f"{font_name}: atlas pages are not contiguous")
    package = FONT_PACKAGE_BY_NAME[font_name]
    textures = [import_atlas(font_name, page, package) for page in pages]

    characters = decoded["characters"]
    if not characters:
        raise RuntimeError(f"{font_name}: empty character table")
    character_count = max(item["index"] for item in characters) + 1
    start_us = [0] * character_count
    start_vs = [0] * character_count
    u_sizes = [0] * character_count
    v_sizes = [0] * character_count
    texture_indices = [0] * character_count
    seen = set()
    for character in characters:
        index = character["index"]
        if index in seen:
            raise RuntimeError(f"{font_name}: duplicate character index {index}")
        seen.add(index)
        start_us[index] = character["x"]
        start_vs[index] = character["y"]
        u_sizes[index] = character["w"]
        v_sizes[index] = character["h"]
        texture_indices[index] = character["page"]

    font, asset_path = create_font_asset(font_name)
    if not unreal.WoTHUDFontImportLibrary.offline_font_matches(
            font, textures, start_us, start_vs, u_sizes, v_sizes, texture_indices):
        if not unreal.WoTHUDFontImportLibrary.populate_offline_font(
                font, textures, start_us, start_vs, u_sizes, v_sizes,
                texture_indices):
            raise RuntimeError(f"C++ helper rejected metrics for {font_name}")
        EAL.save_asset(asset_path)
    return font


def create_registry(fonts):
    hud_fonts_class = unreal.load_class(None, "/Script/WoT.WoTHUDFonts")
    if not hud_fonts_class:
        raise RuntimeError("UWoTHUDFonts not loaded — build the WoT editor module first")
    created = not EAL.does_asset_exist(DATA_ASSET_PATH)
    if not created:
        registry = unreal.load_asset(DATA_ASSET_PATH)
    else:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", hud_fonts_class)
        registry = ATT.create_asset(
            "DA_WoTHUDFonts", DESTINATION, hud_fonts_class, factory)
    if not registry:
        raise RuntimeError(f"failed to create {DATA_ASSET_PATH}")
    desired = {unreal.Name(name): font for name, font in fonts.items()}
    changed = created or registry.get_editor_property("fonts") != desired
    if changed:
        registry.set_editor_property("fonts", desired)
        EAL.save_asset(DATA_ASSET_PATH)


def _stripped_specs(decoded_fonts):
    specs = []
    for font_name in NAMES:
        package = FONT_PACKAGE_BY_NAME[font_name]
        for page in decoded_fonts[font_name]["pages"]:
            specs.append({
                "source_png": CONVERTED_ROOT / package / page["png"],
                "asset_path": (
                    f"{DESTINATION}/T_{font_name}_{page['page']}"),
                "package": package,
                "source_name": page["source_texture"],
                "width": int(page["w"]),
                "height": int(page["h"]),
                "destination_mip_count": 1,
            })
    return specs


def main():
    if STRIPPED:
        unreal.log(
            "IMPORT-HUD-FONTS-SKIP guarded release uses cache-v3 font records")
        return
    decoded_fonts = extract_and_validate()
    fonts = {name: populate_font(name, decoded_fonts[name]) for name in NAMES}
    create_registry(fonts)
    unreal.log(
        f"import_hud_fonts: imported {len(fonts)} offline fonts and populated "
        f"{DATA_ASSET_PATH} mode={'stripped' if STRIPPED else 'full'}")


main()
