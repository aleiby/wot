"""Shared James mesh material-slot naming and master routing.

Pure Python by design: import_map.py runs inside Unreal's embedded Python.
"""

TYPE_SUFFIXES = ("_Mod", "_2S", "_M", "_T")

# import_mesh leaves ordinary opaque skins on the legacy parent so existing
# runtime behavior remains unchanged. Typed slots need real masters because a
# MID cannot override BlendMode or TwoSided.
IMPORT_SLOT_MASTERS = {
    "": "M_WoT_Master",
    "_2S": "M_WoT_MeshLit2S",
    "_M": "M_WoT_MeshLitMasked",
    "_T": "M_WoT_MeshLitT",
    "_Mod": "M_WoT_MeshModulate",
}

GAMMA_SLOT_MASTERS = {
    "": "M_WoT_MeshLit",
    "_2S": "M_WoT_MeshLit2S",
    "_M": "M_WoT_MeshLitMasked",
    "_T": "M_WoT_MeshLitT",
    "_Mod": "M_WoT_MeshModulate",
}

_ROOT_SUFFIXES = {
    "M_WoT_Masked": "_M",
    "M_WoT_MeshLitMasked": "_M",
    "M_WoT_MeshLit2S": "_2S",
    "M_WoT_MeshLitT": "_T",
    "M_WoT_MeshTranslucent": "_T",
    "M_WoT_MeshModulate": "_Mod",
}

_STYLE_SUFFIXES = {
    2: "_M",
    3: "_T",
    4: "_Mod",
}

_BLEND_PRIORITY = {
    "": 0,
    "_2S": 0,
    "_M": 1,
    "_Mod": 2,
    "_T": 3,
}


def split_slot_name(slot):
    """Return ``(texture_name, type_suffix)`` for a generated mesh slot."""
    for suffix in TYPE_SUFFIXES:
        if slot.endswith(suffix):
            return slot[:-len(suffix)], suffix
    return slot, ""


def material_type_suffix(root_name):
    """Recover the James base-type suffix from a material parent root."""
    return _ROOT_SUFFIXES.get(root_name, "")


def actor_style_suffix(style):
    """Return the mesh blend suffix contributed by a retail Actor.Style."""
    if isinstance(style, int):
        return _STYLE_SUFFIXES.get(style, "")
    text = str(style or "").strip().lower()
    if text.isdigit():
        return _STYLE_SUFFIXES.get(int(text), "")
    if "translucent" in text:
        return "_T"
    if "modulated" in text:
        return "_Mod"
    if "masked" in text:
        return "_M"
    return ""


def effective_material_type_suffix(slot_suffix, actor_style):
    """Apply retail Translucent > Modulated > Masked union precedence."""
    actor_suffix = actor_style_suffix(actor_style)
    if _BLEND_PRIORITY[actor_suffix] > _BLEND_PRIORITY[slot_suffix]:
        return actor_suffix
    return slot_suffix


def slot_zero_skin_reference(props):
    """Return UE1 slot-zero Skin/MultiSkins override, if one is authored."""
    return props.get("Skin") or props.get("MultiSkins(0)") or None
