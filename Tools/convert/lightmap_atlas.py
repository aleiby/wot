"""UE1 composed-lightmap â†’ RGB8 atlas for UE5 material injection.

Usage: python lightmap_atlas.py <map.wot> <t3d_json> <out_dir>

Emits <out_dir>/<Map>_lmatlas.png + <Map>_lmatlas.json per the converter/UE
contract:
- pixel = texel_total*2 (composed total 0..127 -> 0..254; the encode never
  ceilings). Consumers decode the display-space light multiplier as
  M = 2*(pixel/255) = total/63.75 -- identity at a composed 63.75, full UE1
  overbright range to ~2.0. The x2 lives in EVERY atlas-consuming master
  (world opaque/masked/wave + the blend families; import_map.py).
- per-surface rects packed with a 1-px edge-replicated gutter (bilinear-safe).
- atlas: smallest power-of-two square 1024..4096 that fits, else 4096x8192.
- json: {"size":[W,H], "surfs":{"<surf_index>":{"x","y","w","h"}},
  "animation":{"groups":[...],"terms":[...]}} with x/y =
  interior origin (excluding gutter) â€” where texel (0,0) sits.
- one unsigned, full-RGB centered response PNG per animation term. Waveform
  and shimmer terms store high-low bytes; mixed terms store the scale-2
  four-corner response. All share the base pack and replicated gutters.
- extra key "fullbright": {"x","y"} â€” a reserved 1x1 texel at FULLBRIGHT
  (128, i.e. M = 256/255 ~ 1.0 after the consumer x2);
  bsp2obj points vertices of surfaces WITHOUT a lightmap (PF_Unlit, sky,
  iLightMap==-1) at its center so every GLB vertex has a valid TEXCOORD_1
  and unlit geometry is not blackened by the multiply. Consumers may ignore.

A full round-trip self-check runs on every surface: the composer's texel
world positions are pushed through the same lightmap_uv â†’ TEXCOORD_1 â†’
pixel-index path bsp2obj uses (float32, as stored in the GLB) and the atlas
pixel must equal the composed texel exactly.
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

import lightmaps as L

# Atlas encode scale for the merged RGBA7 total (0..127). Scale 2 spans
# the FULL retail overbright range (255 at total 127.5 — unreachable, so
# the encode never ceilings); every atlas consumer decodes with a display
# x2 (import_map.py world masters, incl. the blend family). FULLBRIGHT=128
# keeps unlit/no-lightmap surfaces at M = 256/255 ~ 1.0 after that x2.
# History: scale 4 ceilinged at a composed 63.75 — HALF the retail range;
# 2% of Battle2_02's texels (26.8% on the stained glass) clamped there,
# producing the measured bright-core deficit (A/B'd + rolled out
# before correction: WIN mean delta
# -8 -> -0.4, bright ratio 0.839 -> 0.914). Do not change one side
# without the other.
ATLAS_SCALE = 2
FULLBRIGHT = 128

SIZES = [(1024, 1024), (2048, 2048), (4096, 4096), (4096, 8192)]


def pack(rects, w_atlas, h_atlas):
    """Shelf-pack rects [(key, w, h)] (outer sizes, gutter included).
    â†’ {key: (x, y)} outer origins, or None if they don't fit."""
    order = sorted(range(len(rects)), key=lambda i: (-rects[i][2],
                                                     -rects[i][1]))
    pos = {}
    x = y = shelf = 0
    for i in order:
        key, w, h = rects[i]
        if w > w_atlas:
            return None
        if x + w > w_atlas:
            y += shelf
            x = shelf = 0
        shelf = max(shelf, h)
        if y + h > h_atlas:
            return None
        pos[key] = (x, y)
        x += w
    return pos


def build_atlas(out, fullbright=FULLBRIGHT):
    """compose_map() output â†’ (atlas uint8 (H,W,3), doc dict, stats dict)."""
    n = len(out["surf_index"])
    rects = [("__fullbright__", 3, 3)]
    for k in range(n):
        rects.append((k, int(out["surf_uclamp"][k]) + 2,
                      int(out["surf_vclamp"][k]) + 2))
    for w_atlas, h_atlas in SIZES:
        pos = pack(rects, w_atlas, h_atlas)
        if pos is not None:
            break
    if pos is None:
        raise ValueError("surfaces do not fit even in 4096x8192")

    atlas = np.zeros((h_atlas, w_atlas, 3), dtype=np.uint8)
    surfs = {}
    for k in range(n):
        gx, gy = pos[k]
        uc, vc = int(out["surf_uclamp"][k]), int(out["surf_vclamp"][k])
        ofs = int(out["surf_texel_ofs"][k])
        total = out["texel_rgb"][ofs:ofs + uc * vc].reshape(vc, uc, 3)
        # Multiply before the integer conversion: base totals are integral and
        # therefore byte-identical, while centered responses may carry exact
        # half-RGBA7 units that scale 2 is specifically meant to preserve.
        img = np.minimum(total * ATLAS_SCALE, 255).astype(np.uint8)
        atlas[gy:gy + vc + 2, gx:gx + uc + 2] = np.pad(
            img, ((1, 1), (1, 1), (0, 0)), mode="edge")
        surfs[str(int(out["surf_index"][k]))] = dict(x=gx + 1, y=gy + 1,
                                                     w=uc, h=vc)
    fx, fy = pos["__fullbright__"]
    atlas[fy:fy + 3, fx:fx + 3] = fullbright
    doc = dict(size=[w_atlas, h_atlas], surfs=surfs,
               fullbright=dict(x=fx + 1, y=fy + 1))
    texel_area = int((out["surf_uclamp"].astype(np.int64)
                      * out["surf_vclamp"]).sum())
    padded_area = sum(w * h for _, w, h in rects)
    stats = dict(size=[w_atlas, h_atlas],
                 n_rects=len(rects),
                 texel_eff=texel_area / (w_atlas * h_atlas),
                 padded_eff=padded_area / (w_atlas * h_atlas))
    return atlas, doc, stats


def roundtrip_check(out, bsp, atlas, doc):
    """Composer texel world pos â†’ bsp2obj UV1 path (float32 as in the GLB)
    â†’ atlas pixel; must match the composed value exactly on every texel."""
    w_atlas, h_atlas = doc["size"]
    checked = bad = 0
    for k in range(len(out["surf_index"])):
        isurf = int(out["surf_index"][k])
        ent = doc["surfs"][str(isurf)]
        uc, vc = ent["w"], ent["h"]
        ofs = int(out["surf_texel_ofs"][k])
        pts = out["texel_pos"][ofs:ofs + uc * vc]
        uv = L.lightmap_uv(bsp, isurf, pts)                    # fractional
        u01 = ((ent["x"] + uv[:, 0] + 0.5) / w_atlas).astype(np.float32)
        v01 = ((ent["y"] + uv[:, 1] + 0.5) / h_atlas).astype(np.float32)
        px = np.floor(u01.astype(np.float64) * w_atlas).astype(np.int64)
        py = np.floor(v01.astype(np.float64) * h_atlas).astype(np.int64)
        expect = np.minimum(
            out["texel_rgb"][ofs:ofs + uc * vc] * ATLAS_SCALE,
            255).astype(np.uint8)
        inb = (px >= 0) & (px < w_atlas) & (py >= 0) & (py < h_atlas)
        got = atlas[py[inb], px[inb]]
        bad += int((~inb).sum()) + int((got != expect[inb]).any(axis=1).sum())
        checked += len(px)
    return checked, bad


def convert(map_path, t3d_path, out_dir):
    name = Path(map_path).stem
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    ctx = L.load_level(map_path)
    actors = json.load(open(t3d_path))["actors"]
    zones_path = L.EXPORTS / "zones" / f"{name}.zones.json"
    zone_data = (json.loads(zones_path.read_text())
                 if zones_path.exists() else None)
    animation = L.animation_manifest(name, actors, zone_data=zone_data)
    out, stats = L.compose_map(map_path, t3d_path, ctx=ctx)
    atlas, doc, astats = build_atlas(out)
    checked, bad = roundtrip_check(out, ctx[1], atlas, doc)
    out, response_rows = L.bake_animation_responses(
        map_path, t3d_path, animation, ctx=ctx, base=out)
    response_images = []
    for row in response_rows:
        response_atlas, response_doc, _response_stats = build_atlas(
            row["out"], fullbright=0)
        if response_doc != doc:
            raise ValueError(
                f"{name}: {row['term']['response_png']} packing drifted")
        response_checked, response_bad = roundtrip_check(
            row["out"], ctx[1], response_atlas, response_doc)
        if response_bad:
            raise SystemExit(
                f"{name}: {response_bad}/{response_checked} response "
                "round-trip mismatches")
        response_images.append((row["term"]["response_png"], response_atlas))

    doc["animation"] = animation
    Image.fromarray(atlas).save(out_dir / f"{name}_lmatlas.png")
    for response_name, response_atlas in response_images:
        Image.fromarray(response_atlas).save(out_dir / response_name)
    (out_dir / f"{name}_lmatlas.json").write_text(
        json.dumps(doc, separators=(",", ":")))
    response_text = (f", {len(response_rows)} responses emitted"
                     if response_rows else "")
    print(f"{name}: atlas {astats['size'][0]}x{astats['size'][1]}, "
          f"{astats['n_rects']} rects, texel eff {astats['texel_eff']:.1%}, "
          f"padded eff {astats['padded_eff']:.1%}, "
          f"round-trip {checked - bad}/{checked} exact{response_text}"
          + ("" if not bad else "  <-- MISMATCHES"))
    if bad:
        raise SystemExit(f"{name}: {bad} round-trip mismatches")
    return astats


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__.split("\n")[2])
        sys.exit(1)
    convert(*sys.argv[1:4])

