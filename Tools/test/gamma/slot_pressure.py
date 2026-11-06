"""How many corona lights qualify (leaf-set + trace) at each mismatch pose?
>32 means retail's MAX_CORONA_LIGHTS cache CANNOT hold them all -> eviction/
starvation explains golden hiding qualifying coronas."""
import csv
import json
import sys
from pathlib import Path
from wot_gamma_paths import CONVERSION, EXPORTS, SCRATCH

CONV = CONVERSION
sys.setrecursionlimit(100000)

from viewerpoint_fit import Tree  # noqa: E402  (reuse the class)

rows = list(csv.DictReader((SCRATCH / "gate_lostrace.csv").open()))
seen = set()
trees, t3ds, mans = {}, {}, {}
print(f"{'map':11s} {'pose':26s} {'qualifying':>10} {'verdicts_at_pose'}")
for row in rows:
    m, f = row["map"], row["pose"]
    if (m, f) in seen:
        continue
    seen.add((m, f))
    if m not in trees:
        trees[m] = Tree(m)
        t3ds[m] = {a["name"]: a for a in json.loads(
            (CONV / f"{m}.t3d.json").read_text())["actors"]}
        mans[m] = json.loads(
            (EXPORTS / "reference" / "shots" / m / "manifest.json")
            .read_text())
    t = trees[m]
    shot = next(s for s in mans[m]["shots"] if s["file"] == f)
    pawn = (shot["pawn"]["x"], shot["pawn"]["y"], shot["pawn"]["z"])
    eye = float(mans[m].get("eye_height", 29.4))
    cam = (pawn[0], pawn[1], pawn[2] + eye)
    leaf = t.leaf(pawn)
    n = 0
    for name, lset in t.coronas.items():
        if leaf in lset and name in t3ds[m]:
            loc = tuple(map(float, t3ds[m][name]["location"]))
            if t.trace(cam, loc):
                n += 1
    verd = [f"{r['corona']}:{r['verdict'][6]}" for r in rows
            if r["map"] == m and r["pose"] == f]
    print(f"{m:11s} {f[len(m)+1:]:26s} {n:>10} {verd}")
