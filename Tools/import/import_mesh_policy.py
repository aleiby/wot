"""Pure policy helpers shared by the UE mesh importer and stdlib specs."""
import json
from pathlib import Path

from mesh_material_types import split_slot_name


# Slots whose canonical bind is the mesh's own Interchange-imported material:
# runtime code/tests key on the raw material NAME (WoT.Trap.Visuals asserts
# the slab's base skin is "TrapWall1"), so these must never be rebound to
# fallback MIs even when their texture PNG exists in the conv tree.
KEEP_INTERCHANGE = {"AlarmTex", "SpearTex", "TrapWall1", "Trapburst1"}


def should_skip_stripped_curvy(name, stripped):
    return bool(stripped) and str(name).casefold().endswith("_curvy")


def load_texture_manifests(texroot):
    """{(package, name): info} from every <texroot>/<Package>/_manifest.json."""
    texinfo = {}
    for manifest in Path(texroot).glob("*/_manifest.json"):
        package = manifest.parent.name
        for name, info in json.loads(manifest.read_text()).get(
                "textures", {}).items():
            texinfo[(package, name)] = info
    return texinfo


def resolve_mesh_texture_source(texroot, texinfo, texname, *, key0=False):
    """Find the converted PNG a mesh slot texture imports from.

    Returns ``(png, package, source_name, info)`` or None: ``png`` is the
    file to import (the ``_key0`` variant when requested), ``package`` and
    ``source_name`` the retail export it came from.
    """
    texroot = Path(texroot)
    matches = sorted(texroot.glob(f"*/{texname}.png"),
                     key=lambda path: str(path).casefold())
    if not matches:
        matches = sorted(
            (path for path in texroot.glob("*/*.png")
             if path.stem.casefold() == texname.casefold()),
            key=lambda path: str(path).casefold())
    for source_png in matches:
        info = texinfo.get((source_png.parent.name, source_png.stem))
        if info is None:
            # Windows Path.glob matches case-insensitively but returns the
            # PATTERN's casing, so a .uc-authored case (Glass1's allgrey vs
            # the WOT.AllGrey export) misses the exact-case manifest key AND
            # skips the true-case fallback glob. Resolve the manifest entry
            # case-insensitively and canonicalize the stem to the export
            # case so asset paths/bindings stay consistent.
            fold = (source_png.parent.name, source_png.stem.casefold())
            for (pkg, name), candidate in texinfo.items():
                if pkg == fold[0] and name.casefold() == fold[1]:
                    info = candidate
                    source_png = source_png.parent / f"{name}.png"
                    break
        if info is None:
            continue
        png = source_png
        if key0:
            filename = info.get("key0")
            if not filename:
                continue
            png = source_png.parent / filename
            if not png.is_file():
                raise RuntimeError(f"manifest key0 PNG is missing: {png}")
        return png, source_png.parent.name, source_png.stem, info
    return None


def mesh_texture_asset_path(png):
    """/Game path a converted mesh-skin PNG imports to."""
    png = Path(png)
    return f"/Game/WoT/Textures/{png.parent.name}/{png.stem}"


def mesh_skin_texture_assets(conv_root):
    """Every skin texture (and ``_key0`` variant) import_mesh.py acquires for
    the converted meshes: its own slot census and texture resolution."""
    conv_root = Path(conv_root)
    texroot = conv_root / "tex"
    texinfo = load_texture_manifests(texroot)
    assets = set()
    for meta_file in sorted((conv_root / "mesh").glob("*.meta.json")):
        meta = json.loads(meta_file.read_text())
        for slot in meta.get("slot_types", {}):
            base, suffix = split_slot_name(slot)
            if base in KEEP_INTERCHANGE:
                continue
            variants = (False, True) if suffix == "_M" else (False,)
            for key0 in variants:
                source = resolve_mesh_texture_source(
                    texroot, texinfo, base, key0=key0)
                if source is not None:
                    assets.add(mesh_texture_asset_path(source[0]))
    return sorted(assets, key=str.casefold)


def texture_sampling_claims(context):
    """import_mesh.py's claims: every mesh skin is a world texture (raw)."""
    from texture_sampling import WORLD, claim
    return [claim(path, WORLD, "import_mesh")
            for path in mesh_skin_texture_assets(context.conv_root)]
