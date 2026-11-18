"""Corona parity probe: per visible corona, ours-vs-golden luma at the
projected sprite core. Measures the modulate+additive pass-order question
(UE5 composites BLEND_MODULATE in a LATER translucency pass, so the corona
pair composes (FB+S)*(1-S) = FB(1-S)+S-S^2 instead of UE1's screen
FB(1-S)+S — a deficit that grows with corona brightness, total at S=1).

Camera model and BSP occlusion are mask_pickups'. Coronas = bCorona lights
with a Skin, not Lightning-gated (place_coronas' selection). For each
non-occluded projection, sample a WIN-px-radius disc at the core and report
mean luma ours (565-quantized) vs golden plus the surrounding annulus as a
background reference.

Usage: python corona_probe.py <renders_root> <Map> [Map ...]
CSV to stdout: map,pose,corona,dist_uu,px,py,luma_gold,luma_ours,d_core,d_bg
"""
import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mask_pickups import CX, CY, FX, FY, H, U2D, W, _level_tris, _occluded, basis
from scoreboard import GOLD, luma, q565
from wot_gamma_paths import CONVERSION

SKIN_RE = re.compile(r"Texture'([\w\-]+)\.(?:[\w\-]+\.)*([\w\-]+)'")
WIN = 12          # core disc radius, px
BG = (20, 32)     # background annulus radii, px
EDGE = 40         # skip projections this close to the frame edge


def corona_actors(mapname, conv):
    actors = json.loads((conv / f"{mapname}.t3d.json").read_text())["actors"]
    lightning = {a["props"].get("LightTag", "").lower()
                 for a in actors if a["cls"].lower() == "lightning"} - {""}
    out = []
    for a in actors:
        p = a.get("props", {})
        if (p.get("bCorona") == "True" and "Skin" in p
                and p.get("Tag", "").lower() not in lightning
                and SKIN_RE.match(p["Skin"])):
            out.append((a["name"], np.array(a["location"], float)))
    return out


def probe_map(root, mapname, conv):
    man = json.loads((GOLD / mapname / "manifest.json").read_text())
    eye = float(man.get("eye_height", 29.4))
    cor = corona_actors(mapname, conv)
    if not cor:
        return
    pts = np.array([p for _, p in cor])
    geo = _level_tris(mapname)
    yy, xx = np.ogrid[:H, :W]
    for shot in man["shots"]:
        ours_f = root / mapname / shot["file"]
        gold_f = GOLD / mapname / shot["file"]
        if not (ours_f.exists() and gold_f.exists()):
            continue
        ours = luma(q565(np.asarray(Image.open(ours_f).convert("RGB"))))
        gold = luma(np.asarray(Image.open(gold_f).convert("RGB")).astype(np.float64))
        if float((gold > 8).mean()) < 0.005:      # golden-dark pose
            continue
        cam = np.array([shot["pawn"]["x"], shot["pawn"]["y"],
                        shot["pawn"]["z"]], float) + np.array([0, 0, eye])
        F, R, U = basis(shot["rot"]["pitch"] * U2D, shot["rot"]["yaw"] * U2D,
                        shot["rot"]["roll"] * U2D)
        rel = pts - cam
        xc = rel @ F
        sx = CX + FX * (rel @ R) / np.where(xc == 0, 1e-9, xc)
        sy = CY - FY * (rel @ U) / np.where(xc == 0, 1e-9, xc)
        for i, (name, p) in enumerate(cor):
            if xc[i] <= 16:
                continue
            if not (EDGE < sx[i] < W - EDGE and EDGE < sy[i] < H - EDGE):
                continue
            if _occluded(cam, p, geo, slack=8.0):
                continue
            r2 = (xx - sx[i]) ** 2 + (yy - sy[i]) ** 2
            core = r2 <= WIN ** 2
            ann = (r2 > BG[0] ** 2) & (r2 <= BG[1] ** 2)
            lg, lo = float(gold[core].mean()), float(ours[core].mean())
            dbg = float(ours[ann].mean() - gold[ann].mean())
            print(f"{mapname},{shot['file']},{name},{xc[i]:.0f},"
                  f"{sx[i]:.0f},{sy[i]:.0f},{lg:.1f},{lo:.1f},"
                  f"{lo - lg:.1f},{dbg:.1f}")


def main():
    conv = CONVERSION
    root = Path(sys.argv[1])
    print("map,pose,corona,dist_uu,px,py,luma_gold,luma_ours,d_core,d_bg")
    for m in sys.argv[2:]:
        probe_map(root, m, conv)


if __name__ == "__main__":
    main()
