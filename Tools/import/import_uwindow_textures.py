"""Import the shipped UWindow/UBrowser server-browser chrome atlases.

Sources are the retail packages, never the development-tree BMPs:
  original/System/UWindow.u: Icons.Misc, Icons.ActiveFrame, Icons.ActiveFrameS
  original/System/UBrowser.u: Icons.Highlight
The four full atlases are retained beside stitched/cropped per-brush products.

UnrealEditor-Cmd WoT.uproject -run=pythonscript \
  -script=".../Tools/import/import_uwindow_textures.py" -unattended -nullrhi
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
from import_transaction import run_isolated_import_transaction
from payload_binding import ensure_texture_payload_binding
from stripped_texture_import import (
    import_stripped_texture,
    importer_positionals,
    prevalidate_stripped_textures,
    stripped_destination,
    stripped_mode_requested,
)


def system_python():
    """Locate the system interpreter used by the retail texture converter."""
    for name in ("python3", "python"):
        executable = shutil.which(name)
        if executable:
            return executable
    raise RuntimeError("no python3/python on PATH for utx2png")


ROOT = Path(__file__).resolve().parents[2]
RAW_ARGS = sys.argv[1:]
POSITIONALS = importer_positionals(RAW_ARGS)
RETAIL_ROOT = Path(POSITIONALS[0]) if POSITIONALS else original_dir()
CONVERTED_ROOT = Path(POSITIONALS[1] if len(POSITIONALS) > 1 else
                      ROOT / "Saved" / "UWindowTextureImport")
CONVERTER = ROOT / "Tools/convert/utx2png.py"
REGION_COMPOSITOR = ROOT / "Tools/convert/stitch_uwindow_textures.py"
CANONICAL_DESTINATION = "/Game/WoT/UI/UWindow"
STRIPPED = stripped_mode_requested(RAW_ARGS)
DESTINATION = stripped_destination(
    RAW_ARGS, CANONICAL_DESTINATION, "UWindow texture") \
    if STRIPPED else CANONICAL_DESTINATION

# (asset name, shipped package, shipped texture export)
TEXTURES = (
    ("Misc", "UWindow", "Misc"),
    ("ActiveFrame", "UWindow", "ActiveFrame"),
    ("ActiveFrameS", "UWindow", "ActiveFrameS"),
    ("Highlight", "UBrowser", "Highlight"),
)

# Per-brush products. Box-brush sources are either stitched from the exact
# defaultproperty regions or cropped from a parent-adjudicated contiguous
# region; image brushes use direct authored sub-rects.
# (asset name, source package, source texture, width, height)
BRUSH_TEXTURES = (
    ("RaisedBevel", "UWindow", "Misc", 5, 5),
    ("RecessedBevel", "UWindow", "Misc", 122, 17),
    ("SunkenFrame", "UWindow", "Misc", 9, 9),
    ("WindowFrame", "UWindow", "ActiveFrameS", 19, 33),
    ("ScrollUp", "UWindow", "Misc", 12, 10),
    ("ScrollDown", "UWindow", "Misc", 12, 10),
    ("ScrollLeft", "UWindow", "Misc", 10, 12),
    ("ScrollRight", "UWindow", "Misc", 10, 12),
    ("ScrollBackground", "UWindow", "Misc", 1, 1),
)
COMPOSITE_ROOT = CONVERTED_ROOT / "Composites"

ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary


def extract_and_validate():
    CONVERTED_ROOT.mkdir(parents=True, exist_ok=True)
    manifests = {}
    for package in sorted({item[1] for item in TEXTURES}):
        source_package = RETAIL_ROOT / "System" / f"{package}.u"
        manifest_path = CONVERTED_ROOT / package / "_manifest.json"
        if not manifest_path.exists():
            subprocess.run(
                [system_python(), str(CONVERTER), str(CONVERTED_ROOT),
                 str(source_package)],
                check=True,
            )
        if not manifest_path.exists():
            raise RuntimeError(f"utx2png did not write {manifest_path}")
        manifest = json.loads(manifest_path.read_text())["textures"]
        wanted = [
            source_name
            for _, source_package_name, source_name in TEXTURES
            if source_package_name == package
        ]
        missing = [name for name in wanted if name not in manifest]
        if missing:
            raise RuntimeError(
                f"{package}.u UWindow chrome missing from manifest: {missing}")
        manifests[package] = manifest
    subprocess.run(
        [system_python(), str(REGION_COMPOSITOR),
         str(CONVERTED_ROOT / "UWindow"), str(COMPOSITE_ROOT)],
        check=True,
    )
    missing_composites = [
        name for name, _, _, _, _ in BRUSH_TEXTURES
        if not (COMPOSITE_ROOT / f"{name}.png").is_file()
    ]
    if missing_composites:
        raise RuntimeError(
            f"UWindow brush compositor omitted: {missing_composites}")
    return manifests


def stripped_specs(manifests):
    specs = []
    for asset_name, package, source_name in TEXTURES:
        info = manifests[package][source_name]
        specs.append({
            "source_png": CONVERTED_ROOT / package / f"{source_name}.png",
            "asset_path": f"{DESTINATION}/{asset_name}",
            "package": package,
            "source_name": source_name,
            "width": int(info["w"]),
            "height": int(info["h"]),
            "destination_mip_count": 1,
        })
    for asset_name, package, source_name, width, height in BRUSH_TEXTURES:
        specs.append({
            "source_png": COMPOSITE_ROOT / f"{asset_name}.png",
            "asset_path": f"{DESTINATION}/{asset_name}",
            "package": package,
            "source_name": source_name,
            "width": width,
            "height": height,
            "destination_mip_count": 1,
        })
    return specs


def import_textures(manifests):
    result = {}
    stripped_by_path = {
        spec["asset_path"]: spec for spec in stripped_specs(manifests)
    } if STRIPPED else {}
    properties = (
        ("filter", unreal.TextureFilter.TF_NEAREST),
        ("compression_settings",
         unreal.TextureCompressionSettings.TC_EDITOR_ICON),
        ("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI),
        ("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS),
    )
    import_specs = [
        (asset_name, package, source_name,
         CONVERTED_ROOT / package / f"{source_name}.png")
        for asset_name, package, source_name in TEXTURES
    ]
    import_specs.extend(
        (asset_name, package, source_name,
         COMPOSITE_ROOT / f"{asset_name}.png")
        for asset_name, package, source_name, _, _ in BRUSH_TEXTURES
    )
    for asset_name, package, source_name, png in import_specs:
        if not png.exists():
            raise RuntimeError(f"missing converted UWindow texture: {png}")
        asset_path = f"{DESTINATION}/{asset_name}"
        if STRIPPED:
            texture, _ = import_stripped_texture(
                unreal, stripped_by_path[asset_path], properties)
            result[asset_name] = texture
            continue

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
        for prop, value in properties:
            if texture.get_editor_property(prop) != value:
                texture.set_editor_property(prop, value)
                changed = True
        _, binding_changed = ensure_texture_payload_binding(
            unreal, asset_path, package, source_name, 1, save=False)
        if changed or binding_changed:
            EAL.save_asset(asset_path)
        result[asset_name] = texture
    return result


def main():
    manifests = extract_and_validate()
    if STRIPPED:
        specs = stripped_specs(manifests)
        prevalidate_stripped_textures(
            unreal, specs, EAL.does_asset_exist, unreal.load_asset)

        def import_batch():
            return import_textures(manifests)

        textures = run_isolated_import_transaction(
            import_batch, DESTINATION,
            EAL.does_directory_exist, EAL.delete_directory)
    else:
        textures = import_textures(manifests)
    unreal.log(
        f"import_uwindow_textures: imported {len(textures)} shipped chrome "
        f"atlases/brush textures under {DESTINATION} "
        f"mode={'stripped' if STRIPPED else 'full'}")


main()
