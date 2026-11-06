"""Pure-Python helpers for mesh-only palette-index-zero texture variants.

This module intentionally imports only the standard library.  Both conversion
tools and Unreal's embedded Python import it, so importing it must not pull in
numpy, Pillow, or unreal.
"""

import json
from pathlib import Path


KEY0_SUFFIX = "_key0"


def keyed_texture_name(texture_name):
    """Return the derived asset/PNG name for an index-zero-alpha variant."""
    if not isinstance(texture_name, str) or not texture_name:
        raise ValueError(f"invalid texture name: {texture_name!r}")
    return texture_name + KEY0_SUFFIX


def is_keyed_texture_name(texture_name):
    return isinstance(texture_name, str) and texture_name.endswith(KEY0_SUFFIX)


def key0_texture_names(meta_root):
    """Return authored texture names consumed by at least one ``_M`` slot.

    Unmapped ``texN`` placeholders are retained in the returned census so the
    caller can report them, but naturally cannot match a package export.
    Matching against UE1 exports must be case-insensitive.
    """
    names = set()
    for path in Path(meta_root).glob("*.meta.json"):
        data = json.loads(path.read_text())
        for slot in data.get("slot_types", {}):
            if slot.endswith("_M"):
                names.add(slot[:-2])
    return names


def palette0_rgb(texture_info):
    """Validate and return a manifest texture's palette-zero RGB bytes."""
    value = texture_info.get("palette0") if isinstance(texture_info, dict) else None
    if (not isinstance(value, list) or len(value) != 3
            or any(not isinstance(channel, int) or isinstance(channel, bool)
                   or not 0 <= channel <= 255 for channel in value)):
        raise ValueError(f"invalid palette0 RGB: {value!r}")
    return tuple(value)
