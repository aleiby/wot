"""Export P8 textures from UE1 packages to PNG (palette-index-0 → alpha).

Usage: python3 utx2png.py <out_dir> <pkg.utx|pkg.u|map.wot> ...
Writes <out_dir>/<PackageStem>/<TextureName>.png + _manifest.json per package.
"""
import json
from pathlib import Path

import numpy as np
from PIL import Image

from mesh_texture_variants import key0_texture_names, keyed_texture_name
from ue1pkg import Package, decode_palette, decode_texture


def _dilate_masked_rgb(img):
    """Fill transparent texels' RGB with nearest opaque color. Palette
    index 0 often stores an authoring key color (e.g. magenta on
    IronWind2378msk); UE's RGBA mip averaging bleeds it into visible edges
    as fringes. UE1 never blended across palette indices (its .utx mips
    are palette-indexed), so kill the key color at export."""
    a = np.asarray(img).copy()
    alpha = a[..., 3]
    if not (alpha == 0).any() or not (alpha > 0).any():
        return img
    opaque = alpha > 0
    rgb = a[..., :3].astype(np.float32)
    filled = opaque.copy()
    for _ in range(16):
        if filled.all():
            break
        # neighbor sums via shifted copies
        acc = np.zeros_like(rgb)
        cnt = np.zeros(filled.shape, dtype=np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            sh = np.roll(filled, (dy, dx), axis=(0, 1))
            shr = np.roll(rgb, (dy, dx), axis=(0, 1))
            acc += shr * sh[..., None]
            cnt += sh
        grow = ~filled & (cnt > 0)
        rgb[grow] = (acc[grow] / cnt[grow, None])
        filled |= grow
    if not filled.all():
        rgb[~filled] = rgb[opaque].mean(axis=0)
    a[..., :3] = np.clip(rgb + 0.5, 0, 255).astype(np.uint8)
    return Image.fromarray(a)

PLAIN_TEXTURE_CLASSES = {"Texture"}  # Fire/Water etc. are parametric — flipbook baker's job


def _texture_image(texture, palette, *, key_index_zero=False):
    """Expand mip zero while preserving palette RGB exactly.

    Alpha follows source ``bMasked`` for the ordinary PNG. A keyed sibling
    instead alpha-tests index zero regardless of that flag. RGB dilation is
    confined to transparent texels and never changes the ordinary asset.
    """
    image = Image.new("RGBA", (texture["w"], texture["h"]))
    pixels = image.load()
    data = texture["data"]
    for y in range(texture["h"]):
        row = y * texture["w"]
        for x in range(texture["w"]):
            palette_index = data[row + x]
            red, green, blue, _alpha = palette[palette_index]
            transparent = palette_index == 0 and (
                key_index_zero or texture["masked"])
            pixels[x, y] = (red, green, blue, 0 if transparent else 255)
    if key_index_zero or texture["masked"]:
        image = _dilate_masked_rgb(image)
    return image


def _refname(pkg, ref):
    """objref → bare object name (imports = (cpkg, cname, outer, objname))."""
    if ref > 0:
        return pkg.exports[ref - 1]["name"]
    if ref < 0:
        return pkg.imports[-ref - 1][3]
    return None


def export_package(path, out_root, key0_names=()):
    pkg = Package(path)
    out = out_root / Path(path).stem
    manifest, skipped = {}, []
    key0_casefold = {name.casefold() for name in key0_names}
    palettes = {}
    for i, e in enumerate(pkg.exports):
        if pkg.objname(e["cls"]) == "Palette":
            palettes[i + 1] = decode_palette(pkg, e)  # keyed by objref
    for export_index, e in enumerate(pkg.exports, 1):
        if pkg.objname(e["cls"]) not in PLAIN_TEXTURE_CLASSES:
            continue
        try:
            t = decode_texture(pkg, e)
            pal = palettes.get(t["palette"])
            if pal is None or t["format"] != 0:
                skipped.append(f"{e['name']} (format={t['format']})")
                continue
            out.mkdir(parents=True, exist_ok=True)
            img = _texture_image(t, pal)
            img.save(out / f"{e['name']}.png")
            entry = dict(w=t["w"], h=t["h"], masked=t["masked"],
                         palette0=list(pal[0][:3]),
                         # Keep the full UE1 identity so BSP surfaces can join
                         # Package.Group.Name without a bare-name alias.
                         object_path=f"{pkg.name}.{pkg.objpath(export_index)}",
                         # Stored even at the Texture default so bootstrap can
                         # distinguish pre-friction manifests and regenerate.
                         friction=t["friction"])
            if e["name"].casefold() in key0_casefold:
                key0_name = keyed_texture_name(e["name"])
                _texture_image(t, pal, key_index_zero=True).save(
                    out / f"{key0_name}.png")
                entry["key0"] = f"{key0_name}.png"
            # UE1 Texture.DrawScale renders the texture at 1/DrawScale the tiling
            # (UScale = Scale*ScaleFactor, and the surface draw uses 1/UScale)
            # — #30 root cause. Store when non-default so bsp2obj can
            # normalize UV by USize*DrawScale (most 128px world textures are 2.0).
            if abs(t.get("drawscale", 1.0) - 1.0) > 1e-6:
                entry["drawscale"] = t["drawscale"]
            if t.get("env"):
                # bEnvironment: UE1 sphere-maps the texture on meshes —
                # the glass lamp globes etc.
                entry["env"] = True
            # UE1 close-range detail / large-scale macro modulation layers —
            # cross-package refs (WOTDetail etc.);
            # store PACKAGE-QUALIFIED [package, name] so the importer binds the
            # right asset (never a bare-name lookup that aliases across packages).
            for key in ("detail", "macro"):
                ref = t.get(key)
                r = pkg.objref(ref) if ref else None
                if r:
                    entry[key] = list(r)
            manifest[e["name"]] = entry
        except Exception as ex:  # noqa: BLE001 — keep exporting the rest
            skipped.append(f"{e['name']} ({ex})")
    # Always write the manifest, even when every texture in the package is
    # parametric (flipbook-baked) and nothing rasterizes: consumers with
    # fail-closed package checks (zones.py friction) must be able to tell
    # "package converted, nothing rasterizable" (Flames06.utx) apart from
    # "package never converted" (a real pipeline hole). Both streams hit
    # this concurrently (Windows zones wave; Mac first full 19-map
    # bootstrap convert dying on Battle2_02).
    out.mkdir(parents=True, exist_ok=True)
    (out / "_manifest.json").write_text(
        json.dumps(dict(textures=manifest, skipped=skipped), indent=1))
    return len(manifest), skipped


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_root", type=Path)
    parser.add_argument("packages", nargs="+")
    parser.add_argument(
        "--mesh-meta-root", type=Path,
        help="directory of mesh *.meta.json files (default: <out_root>/../mesh)")
    arguments = parser.parse_args()
    meta_root = arguments.mesh_meta_root or arguments.out_root.parent / "mesh"
    key0_names = key0_texture_names(meta_root) if meta_root.is_dir() else set()
    for package in arguments.packages:
        n, skipped = export_package(package, arguments.out_root, key0_names)
        print(f"{Path(package).name}: {n} textures, {len(skipped)} skipped")
