"""Why do Arena_04/07 sky rays miss? Candidate node count, sky-box bbox
vs SkyZoneInfo origin, per-zone node counts."""
import json
import sys
from pathlib import Path

import numpy as np

TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS / "convert"))
sys.path.insert(0, str(TOOLS))
from wot_config import local_path, original_dir  # noqa: E402
import lightmaps as L

S = local_path("conversion_root")

for name in ("Arena_04", "Arena_07"):
    pkg = L.Package(original_dir() / "Maps" / f"{name}.wot")
    models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
    bsp = L.decode_model_full(pkg, max(models, key=lambda e: e["size"]))
    actors = json.load(open(S / f"{name}.t3d.json"))["actors"]
    skies = [a for a in actors if a["cls"].lower() == "skyzoneinfo"]
    winner = skies[-1]
    hd = [a for a in skies
          if a.get("props", {}).get("bHighDetail", "False") == "True"]
    if hd:
        winner = hd[-1]
    zone_names = [str(pkg.objname(z)) if z else None for z in (bsp["zones"] or [])]
    sky_zone = next(zi for zi, zn in enumerate(zone_names)
                    if zn and zn.lower() == winner["name"].lower())
    from collections import Counter
    zc = Counter()
    pts = np.array(bsp["points"])
    cand_pts = []
    for node in bsp["nodes"]:
        ivp, isurf, nv = node[0], node[1], node[2]
        iz = node[4] if len(node) > 4 else (0, 0)
        if nv < 3:
            continue
        for z in tuple(iz):
            zc[z] += 1
        if sky_zone in tuple(iz):
            for i in range(nv):
                cand_pts.append(bsp["points"][bsp["verts"][ivp + i][0]])
    print(f"{name}: winner {winner['name']} zone {sky_zone} "
          f"loc {winner['location']}")
    print(f"  zone table: {list(enumerate(zone_names))}")
    print(f"  node counts per zone: {dict(sorted(zc.items()))}")
    if cand_pts:
        cp = np.array(cand_pts)
        print(f"  sky-zone node verts: {len(cp)}, bbox min {cp.min(0)} "
              f"max {cp.max(0)}")
    else:
        print("  NO candidate nodes for the sky zone")
