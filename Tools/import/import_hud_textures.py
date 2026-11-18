"""Import HUD textures and build DA_WoTHUDTextures.

Source packages: original/System/WOT.u (crosshairs, seeker frame, hand notches,
taint overlay, first-new-pickup flash, turtle/perf icons, warning light,
scoreboard heads) and WoTBase.u (the shipped WOTInventory S/M selection overlays).
The already-imported T32 countdown and status-frame textures are registry-only
inputs (hud_texture_manifest.py). utx2png's manifests are checked before
anything is imported. Every HUD texture, registry-only inputs included, samples
sRGB (the UI family of texture_sampling.py), as the release HUD resolves them.

The optional stripped mode uses opaque-magenta, correct-size one-mip
placeholders with the same retail payload binding. It is selected by
--stripped, --mode=stripped, WOT_TEXTURE_IMPORT_MODE, or WOT_PAYLOAD_MODE and
requires an explicitly isolated --dest / WOT_PAYLOAD_CONTENT_ROOT.

UnrealEditor-Cmd WoT.uproject -run=pythonscript \
  -script=".../Tools/import/import_hud_textures.py" -unattended -nullrhi
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
from hud_texture_manifest import (  # noqa: F401 - re-exported manifest names
    REGISTRY_ONLY_TEXTURES,
    STATUS_ICON_FRAME_NAMES,
)
import texture_sampling
from texture_sampling import (
    UI,
    import_texture,
    load_texture,
    verify_texture_sampling,
)


def system_python():
    """Locate the SYSTEM interpreter for the converter subprocess (needs PIL).
    Under UnrealEditor's embedded Python, sys.executable is the editor binary —
    not a Python — and stock Windows ships no `python3` shim, so probe both."""
    for name in ("python3", "python"):
        exe = shutil.which(name)
        if exe:
            return exe
    raise RuntimeError("no python3/python on PATH for utx2png")


ROOT = Path(__file__).resolve().parents[2]
RAW_ARGS = sys.argv[1:]
POSITIONALS = importer_positionals(RAW_ARGS)
# Resolved without the existence check so the module (its manifest and
# texture_sampling_claims) imports anywhere; main() still requires it.
RETAIL_ROOT = Path(POSITIONALS[0]) if POSITIONALS else original_dir(required=False)
CONVERTED_ROOT = Path(POSITIONALS[1] if len(POSITIONALS) > 1 else
                      ROOT / "Saved" / "HUDTextureImport")
CONVERTER = ROOT / "Tools/convert/utx2png.py"
CANONICAL_DESTINATION = "/Game/WoT/UI"
STRIPPED = stripped_mode_requested(RAW_ARGS)
DESTINATION = stripped_destination(
    RAW_ARGS, CANONICAL_DESTINATION, "HUD texture") \
    if STRIPPED else CANONICAL_DESTINATION
DATA_ASSET_PATH = f"{DESTINATION}/DA_WoTHUDTextures"
# (registry id, source package, source texture name)
TEXTURES = ([(f"Crosshair{i}", "WOT", f"Crosshair{i}") for i in range(1, 21)] +
            [("SeekerFrame", "WOT", "SeekerFrame"),
             ("I_Notch", "WOT", "I_Notch"),
             ("D_Notch", "WOT", "D_Notch"),
             ("I_Tainted", "WOT", "I_Tainted"),
             ("I_NotifyPickup", "WOT", "I_NotifyPickup"),
             ("Turtle", "WOT", "Turtle"),
             ("TurtleDead", "WOT", "TurtleDead"),
             ("Warn", "WOT", "Warn"),
             ("HeadsBase", "WOT", "HeadsBase"),
             ("HeadsBlack", "WOT", "HeadsBlack"),
             ("HeadsBlue", "WOT", "HeadsBlue"),
             ("HeadsRed", "WOT", "HeadsRed"),
             ("HeadsGreen", "WOT", "HeadsGreen"),
             ("HeadsAesSedai", "WOT", "HeadsAesSedai"),
             ("HeadsForsaken", "WOT", "HeadsForsaken"),
             ("HeadsHound", "WOT", "HeadsHound"),
             ("HeadsWC", "WOT", "HeadsWC"),
             ("StatusIconSelected", "WoTBase", "S"),
             ("StatusIconFrame", "WoTBase", "M")])
# D13 registry-only inputs (STATUS_ICON_FRAME_NAMES, REGISTRY_ONLY_TEXTURES)
# live in hud_texture_manifest.py: import_map.py's skin pass, which produces
# them, reads the same list to give them this importer's UI sampling.
SCOREBOARD_HEAD_TEXTURES = {
    "HeadsBase", "HeadsBlack", "HeadsBlue", "HeadsRed", "HeadsGreen",
    "HeadsAesSedai", "HeadsForsaken", "HeadsHound", "HeadsWC",
}
MASKED_TEXTURES = {registry_name for registry_name, _, _ in TEXTURES
                   if registry_name != "Warn"}

ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary


def texture_sampling_claims(_context=None):
    """Every texture the HUD registry holds: UI textures, sRGB. The
    registry-only inputs sit beside world textures under /Game/WoT/Textures;
    this named claim outranks the code-package skin pass's blanket one."""
    owner = "import_hud_textures"
    claims = [texture_sampling.claim(
                  f"{CANONICAL_DESTINATION}/{registry_name}", UI, owner)
              for registry_name, _, _ in TEXTURES]
    claims.extend(texture_sampling.claim(asset_path, UI, owner)
                  for _, asset_path in REGISTRY_ONLY_TEXTURES)
    return claims


def extract_and_validate():
    CONVERTED_ROOT.mkdir(parents=True, exist_ok=True)
    manifests = {}
    for package in {package for _, package, _ in TEXTURES}:
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
        wanted = [source for _, source_package_name, source in TEXTURES
                  if source_package_name == package]
        missing = [name for name in wanted if name not in manifest]
        if missing:
            raise RuntimeError(
                f"{package}.u HUD textures missing from manifest: {missing}")
        wrong_head_size = [
            source for registry_name, source_package_name, source in TEXTURES
            if source_package_name == package
            and registry_name in SCOREBOARD_HEAD_TEXTURES
            and (manifest[source].get("w"), manifest[source].get("h")) != (16, 16)
        ]
        if wrong_head_size:
            raise RuntimeError(
                f"{package}.u scoreboard heads are not 16x16: {wrong_head_size}")
        # Every selected texture except Warn ships FLAGS=2. Palette-index zero
        # must become alpha or masked draw commands render opaque boxes.
        unmasked = [source for registry_name, source_package_name, source in TEXTURES
                    if source_package_name == package
                    and registry_name in MASKED_TEXTURES
                    and not manifest[source].get("masked")]
        if unmasked:
            raise RuntimeError(
                f"{package}.u HUD textures not masked (FLAGS=2 expected): {unmasked}")
        manifests[package] = manifest
    return manifests


def _stripped_specs(manifests):
    specs = []
    for registry_name, package, source_name in TEXTURES:
        info = manifests[package][source_name]
        specs.append({
            "source_png": CONVERTED_ROOT / package / f"{source_name}.png",
            "asset_path": f"{DESTINATION}/{registry_name}",
            "package": package,
            "source_name": source_name,
            "width": int(info["w"]),
            "height": int(info["h"]),
            "destination_mip_count": 1,
        })
    return specs


def import_textures(manifests):
    result = {}
    stripped_specs = {
        spec["asset_path"]: spec for spec in _stripped_specs(manifests)
    } if STRIPPED else {}
    for registry_name, package, source_name in TEXTURES:
        png = CONVERTED_ROOT / package / f"{source_name}.png"
        if not png.exists():
            raise RuntimeError(f"missing converted HUD texture: {png}")
        asset_path = f"{DESTINATION}/{registry_name}"
        if STRIPPED:
            texture, _ = import_stripped_texture(
                unreal, stripped_specs[asset_path],
                (("mip_gen_settings",
                  unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS),),
                family=UI)
            result[unreal.Name(registry_name)] = texture
            continue
        texture, changed = import_texture(
            unreal, png, asset_path, UI, save=False)
        if texture.get_editor_property("mip_gen_settings") != \
                unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS:
            texture.set_editor_property(
                "mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
            changed = True
        _, binding_changed = ensure_texture_payload_binding(
            unreal, asset_path, package, source_name, 1, save=False)
        if changed or binding_changed:
            EAL.save_asset(asset_path)
        result[unreal.Name(registry_name)] = texture
    return result


def add_registry_only_textures(textures):
    """Add existing assets to the map without importing them.

    Their only change is the UI sampling policy (sRGB, saved only when it
    differs). A stripped run writes nothing outside its isolated destination,
    so there the canonical inputs are verified instead (the skin pass that
    produces them already applies the UI policy)."""
    for registry_name, asset_path in REGISTRY_ONLY_TEXTURES:
        if not EAL.does_asset_exist(asset_path):
            raise RuntimeError(
                f"existing HUD registry texture is missing: {asset_path}")
        if STRIPPED:
            texture = verify_texture_sampling(unreal, asset_path, UI)
        else:
            texture, _ = load_texture(unreal, asset_path, UI)
        textures[unreal.Name(registry_name)] = texture
    return textures


def create_registry(textures):
    # Resolve the UCLASS by path rather than the unreal.WoTHUDTextures attribute:
    # a bare UCLASS() DataAsset is not always surfaced into the python stub namespace,
    # but load_class finds any class registered in the loaded WoT module.
    hud_tex_class = unreal.load_class(None, "/Script/WoT.WoTHUDTextures")
    if not hud_tex_class:
        raise RuntimeError(
            "UWoTHUDTextures not loaded — build the WoT editor module first")
    created = not EAL.does_asset_exist(DATA_ASSET_PATH)
    if not created:
        registry = unreal.load_asset(DATA_ASSET_PATH)
    else:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", hud_tex_class)
        registry = ATT.create_asset(
            "DA_WoTHUDTextures", DESTINATION, hud_tex_class, factory
        )
    if not registry:
        raise RuntimeError(f"failed to create {DATA_ASSET_PATH}")
    changed = created or registry.get_editor_property("textures") != textures
    if changed:
        registry.set_editor_property("textures", textures)
        EAL.save_asset(DATA_ASSET_PATH)


def main():
    if not POSITIONALS:
        original_dir()  # require the configured reference root up front
    manifests = extract_and_validate()
    if STRIPPED:
        specs = _stripped_specs(manifests)
        prevalidate_stripped_textures(
            unreal, specs, EAL.does_asset_exist, unreal.load_asset)

        def import_batch():
            textures = add_registry_only_textures(import_textures(manifests))
            create_registry(textures)
            return textures

        textures = run_isolated_import_transaction(
            import_batch, DESTINATION,
            EAL.does_directory_exist, EAL.delete_directory)
    else:
        textures = add_registry_only_textures(import_textures(manifests))
        create_registry(textures)
    unreal.log(
        f"import_hud_textures: imported {len(textures)} textures and populated "
        f"{DATA_ASSET_PATH} mode={'stripped' if STRIPPED else 'full'}"
    )


if __name__ == "__main__":
    main()
