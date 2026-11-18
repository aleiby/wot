"""Idempotent payload bindings for imported retail textures.

This module deliberately has no top-level ``unreal`` import, so orchestration
and ``import_all.py --dry-run`` can inspect/import its key/count helpers without
launching Unreal. Engine-side callers pass the already-loaded unreal module.
"""


def _validate_component(value, label, *, allow_separator=False):
    """Same grammar as build_payload_cache._validate_component — a binding the
    cache builder would reject must never be written."""
    if (not isinstance(value, str) or not value
            or (not allow_separator and "__" in value)
            or any(char in value for char in "/\\\0\r\n")
            or any(ord(char) < 32 or ord(char) == 127 for char in value)
            or value in (".", "..")):
        raise ValueError(f"invalid payload texture {label}: {value!r}")


def texture_payload_key(package, name):
    """Return the exact cache-v3 logical id for one retail texture export."""
    _validate_component(package, "package")
    # The separator is parsed at its first occurrence; retail names may carry
    # '__' (e.g. Stop__5) while package names may not — matching the cache.
    _validate_component(name, "name", allow_separator=True)
    return f"cache/texture/{package}__{name}"


def full_mip_count(width, height):
    """Count the max(1, base >> n) chain through its final 1x1 mip."""
    if (not isinstance(width, int) or isinstance(width, bool) or width <= 0
            or not isinstance(height, int) or isinstance(height, bool)
            or height <= 0):
        raise ValueError(f"invalid texture dimensions: {width!r}x{height!r}")
    return max(width, height).bit_length()


def _texture_dimensions(texture):
    # UE exposes the Blueprint_GetSizeX/Y UFUNCTIONs under their C++ names in
    # some Python builds and their display names in others.
    for x_name, y_name in (("blueprint_get_size_x", "blueprint_get_size_y"),
                           ("get_size_x", "get_size_y")):
        x_fn = getattr(texture, x_name, None)
        y_fn = getattr(texture, y_name, None)
        if x_fn and y_fn:
            return int(x_fn()), int(y_fn())
    raise RuntimeError(
        f"{texture.get_path_name()}: Texture2D size accessors are unavailable")


def ensure_texture_payload_binding(unreal, asset_path, package, name,
                                   destination_mip_count=None, *, save=True):
    """Attach/converge UWoTPayloadAssetUserData; save only on a real change.

    ``package`` and ``name`` identify the retail source, so renamed HUD/font
    destinations intentionally bind to their original Package__Name key.
    """
    binding_class = unreal.load_class(
        None, "/Script/WoT.WoTPayloadAssetUserData")
    if not binding_class:
        raise RuntimeError(
            "UWoTPayloadAssetUserData not loaded — build the WoT editor module first")
    texture = unreal.load_asset(asset_path)
    if not texture:
        raise RuntimeError(f"payload texture asset is missing: {asset_path}")

    if destination_mip_count is None:
        no_mips = (texture.get_editor_property("mip_gen_settings") ==
                   unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        destination_mip_count = (1 if no_mips else
                                 full_mip_count(*_texture_dimensions(texture)))
    if (not isinstance(destination_mip_count, int)
            or isinstance(destination_mip_count, bool)
            or destination_mip_count <= 0):
        raise ValueError(
            f"invalid destination mip count: {destination_mip_count!r}")

    changed = False
    binding = texture.get_asset_user_data_of_class(binding_class)
    if not binding:
        if not texture.add_asset_user_data_of_class(binding_class):
            raise RuntimeError(
                f"failed to attach payload binding to {asset_path}")
        binding = texture.get_asset_user_data_of_class(binding_class)
        if not binding:
            raise RuntimeError(
                f"payload binding disappeared after attach: {asset_path}")
        changed = True

    key = texture_payload_key(package, name)
    if binding.get_editor_property("payload_key") != key:
        binding.set_editor_property("payload_key", key)
        changed = True
    if binding.get_editor_property("destination_mip_count") != destination_mip_count:
        binding.set_editor_property(
            "destination_mip_count", destination_mip_count)
        changed = True

    if changed and save:
        unreal.EditorAssetLibrary.save_asset(asset_path)
    return texture, changed
