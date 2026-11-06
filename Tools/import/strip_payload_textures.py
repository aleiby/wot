#!/usr/bin/env python3
"""Finalize payload-bound textures as dimension-matched stripped placeholders.

This is a deliberately late, stripped-mode-only pass.  It catches canonical
retail textures imported by any producer without teaching each producer about
the stripped source policy.  The decision helpers stay free of ``unreal`` so
their safety gates can be checked with the stdlib test suite.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

# Run directly via -script the pythonscript commandlet does not put this
# directory on sys.path, so sibling imports (stripped_texture_import) fail;
# mirror the other Tools/import leaves.
_IMPORT_DIR = Path(__file__).resolve().parent
if str(_IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(_IMPORT_DIR))

from stripped_texture_import import (
    ensure_placeholder_png,
    stripped_texture_is_current,
)


TEXTURE_ROOT = "/Game/WoT"
FLIPBOOK_ROOT = "/Game/WoT/Textures/Flipbooks"

PAYLOAD_FINALIZE_STRIP = "strip"
PAYLOAD_FINALIZE_ALREADY = "already"
PAYLOAD_FINALIZE_UNBOUND = "unbound"
PAYLOAD_FINALIZE_FLIPBOOK = "flipbook"


def payload_finalize_is_flipbook_path(asset_path):
    """Return whether an asset is under the separately-owned flipbook root."""
    normalized = str(asset_path).replace("\\", "/").rstrip("/").casefold()
    root = FLIPBOOK_ROOT.casefold()
    return normalized == root or normalized.startswith(root + "/")


def payload_finalize_decision(asset_path, has_binding, placeholder_current):
    """Return the mutation decision for one Texture2D registry result."""
    if not has_binding:
        return PAYLOAD_FINALIZE_UNBOUND
    if payload_finalize_is_flipbook_path(asset_path):
        return PAYLOAD_FINALIZE_FLIPBOOK
    if placeholder_current:
        return PAYLOAD_FINALIZE_ALREADY
    return PAYLOAD_FINALIZE_STRIP


def payload_finalize_stripped_mode(environ=None):
    """Keep this destructive source replacement confined to stripped builds."""
    environ = os.environ if environ is None else environ
    return environ.get("WOT_PAYLOAD_MODE", "").strip().casefold() == "stripped"


def _payload_finalize_dimensions(texture):
    for x_name, y_name in (("blueprint_get_size_x", "blueprint_get_size_y"),
                           ("get_size_x", "get_size_y")):
        x_fn = getattr(texture, x_name, None)
        y_fn = getattr(texture, y_name, None)
        if x_fn and y_fn:
            width, height = int(x_fn()), int(y_fn())
            if width > 0 and height > 0:
                return width, height
    raise RuntimeError(
        f"{texture.get_path_name()}: Texture2D size accessors are unavailable")


def _payload_finalize_asset_path(asset_data):
    package_name = str(asset_data.package_name)
    if not package_name or package_name == "None":
        raise RuntimeError(f"Texture2D registry result has no package name: {asset_data}")
    return package_name


def _payload_finalize_log_asset(unreal, message):
    # Some UE Python builds expose log_verbose while others expose only log.
    # Keep the distinctive prefix in the compatibility fallback so the lines
    # remain filterable as verbose per-asset diagnostics.
    logger = getattr(unreal, "log_verbose", None)
    if logger:
        logger(message)
    else:
        unreal.log(f"STRIP-PAYLOAD-TEXTURES VERBOSE {message}")


def _payload_finalize_restore_binding(texture, binding_class, payload_key,
                                      destination_mip_count):
    binding = texture.get_asset_user_data_of_class(binding_class)
    if not binding:
        if not texture.add_asset_user_data_of_class(binding_class):
            raise RuntimeError(
                f"failed to restore payload binding to {texture.get_path_name()}")
        binding = texture.get_asset_user_data_of_class(binding_class)
    if not binding:
        raise RuntimeError(
            f"payload binding disappeared on {texture.get_path_name()}")
    binding.set_editor_property("payload_key", payload_key)
    binding.set_editor_property(
        "destination_mip_count", destination_mip_count)


def _payload_finalize_import_placeholder(unreal, asset_path, texture,
                                         binding_class, placeholder,
                                         width, height):
    mip_gen_settings = texture.get_editor_property("mip_gen_settings")
    binding = texture.get_asset_user_data_of_class(binding_class)
    if not binding:
        raise RuntimeError(f"payload binding disappeared before strip: {asset_path}")
    payload_key = binding.get_editor_property("payload_key")
    destination_mip_count = int(
        binding.get_editor_property("destination_mip_count"))
    if destination_mip_count <= 0:
        raise RuntimeError(
            f"{asset_path}: invalid payload destination mip count "
            f"{destination_mip_count}")

    destination, asset_name = asset_path.rsplit("/", 1)
    task = unreal.AssetImportTask()
    task.filename = str(placeholder)
    task.destination_path = destination
    task.destination_name = asset_name
    task.automated = True
    task.save = False
    task.replace_existing = True
    task.replace_existing_settings = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    texture = unreal.load_asset(asset_path)
    if not texture or not isinstance(texture, unreal.Texture2D):
        raise RuntimeError(f"failed to reimport stripped texture: {asset_path}")
    if _payload_finalize_dimensions(texture) != (width, height):
        raise RuntimeError(
            f"stripped placeholder has wrong dimensions: {asset_path}")
    if not stripped_texture_is_current(texture, placeholder, width, height):
        raise RuntimeError(
            f"stripped placeholder source was not applied: {asset_path}")

    texture.set_editor_property("mip_gen_settings", mip_gen_settings)
    _payload_finalize_restore_binding(
        texture, binding_class, payload_key, destination_mip_count)
    if unreal.EditorAssetLibrary.save_asset(asset_path) is False:
        raise RuntimeError(f"failed to save stripped texture: {asset_path}")


def finalize_payload_textures(unreal):
    """Scan all WoT Texture2Ds and strip only payload-bound destinations."""
    counts = {"total": 0, "stripped": 0, "already": 0,
              "skipped_unbound": 0}
    if not payload_finalize_stripped_mode():
        unreal.log(
            "STRIP-PAYLOAD-TEXTURES disabled: WOT_PAYLOAD_MODE is not stripped")
        return counts

    binding_class = unreal.load_class(
        None, "/Script/WoT.WoTPayloadAssetUserData")
    if not binding_class:
        raise RuntimeError(
            "UWoTPayloadAssetUserData not loaded -- build the WoT editor module first")

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    texture_class = unreal.TopLevelAssetPath("/Script/Engine", "Texture2D")
    asset_filter = unreal.ARFilter(
        package_paths=[unreal.Name(TEXTURE_ROOT)],
        class_paths=[texture_class],
        recursive_paths=True,
        recursive_classes=False,
    )
    assets = sorted(
        registry.get_assets(asset_filter) or (),
        key=lambda item: str(item.package_name).casefold())
    counts["total"] = len(assets)

    placeholder_directory = (
        Path(unreal.Paths.project_saved_dir()) / "StrippedTexturePlaceholders")
    flipbooks_excluded = 0
    for asset_data in assets:
        asset_path = _payload_finalize_asset_path(asset_data)
        texture = asset_data.get_asset()
        if not texture or not isinstance(texture, unreal.Texture2D):
            raise RuntimeError(
                f"AssetRegistry Texture2D could not be loaded: {asset_path}")
        binding = texture.get_asset_user_data_of_class(binding_class)
        if not binding:
            counts["skipped_unbound"] += 1
            reason = ("unbound flipbook (separately handled)"
                      if payload_finalize_is_flipbook_path(asset_path)
                      else "unbound")
            _payload_finalize_log_asset(unreal, f"skip {asset_path}: {reason}")
            continue

        if payload_finalize_is_flipbook_path(asset_path):
            flipbooks_excluded += 1
            unreal.log_warning(
                f"STRIP-PAYLOAD-TEXTURES skip bound flipbook {asset_path}: "
                "flipbook sheets are handled separately")
            continue

        width, height = _payload_finalize_dimensions(texture)
        placeholder = ensure_placeholder_png(
            placeholder_directory, width, height)
        current = stripped_texture_is_current(
            texture, placeholder, width, height)
        decision = payload_finalize_decision(asset_path, True, current)
        if decision == PAYLOAD_FINALIZE_ALREADY:
            counts["already"] += 1
            _payload_finalize_log_asset(unreal, f"already {asset_path}")
            continue

        _payload_finalize_import_placeholder(
            unreal, asset_path, texture, binding_class, placeholder,
            width, height)
        counts["stripped"] += 1
        _payload_finalize_log_asset(
            unreal, f"stripped {asset_path} ({width}x{height})")

    if flipbooks_excluded:
        unreal.log(
            "STRIP-PAYLOAD-TEXTURES NOTE "
            f"bound_flipbooks_excluded={flipbooks_excluded}")
    return counts


def main():
    import unreal

    counts = finalize_payload_textures(unreal)
    unreal.log(
        "STRIP-PAYLOAD-TEXTURES "
        f"total={counts['total']} stripped={counts['stripped']} "
        f"already={counts['already']} "
        f"skipped_unbound={counts['skipped_unbound']}")
    unreal.log("STRIP-PAYLOAD-TEXTURES OK")


if __name__ == "__main__":
    main()
