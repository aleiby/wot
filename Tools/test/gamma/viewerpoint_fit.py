"""Which viewer-point convention predicts golden's corona show/hide?

For every decisive sweep row (false_hide = golden shows, false_show = golden
hides, per the decomposition's corona reading), test conventions:
  P0 = manifest pawn point            (current port)
  PE = pawn + eye (camera)
  P0/PE +/- small z nudges
For each convention, prediction = leaf(point) in light's baked set (AND the
LOS trace passes — reuse csg trace). Score = how many rows it gets right.
Golden truth: false_hide rows -> golden SHOWS; false_show rows (with our
corona drawn, ours_c>8) -> golden HIDES.
"""
import csv
import json
import sys
from pathlib import Path
from wot_gamma_paths import CONVERSION, EXPORTS, SCRATCH

CONV = CONVERSION

sys.setrecursionlimit(100000)


class Tree:
    def __init__(self, m):
        bake = json.loads(
            (EXPORTS / "leaves" / f"{m}.leaves.json").read_text())
        n = bake["nodes"]
        self.pl, self.ch, self.lv = n["planes"], n["children"], n["leaves"]
        self.csg = n["csg"]
        self.root = bool(bake["root_outside"])
        self.coronas = bake["coronas"]

    def dot(self, node, p):
        pl = self.pl[node]
        return pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] - pl[3]

    def leaf(self, p):
        node, parent, side = 0, 0, 0
        while node != -1:
            side = 1 if self.dot(node, p) >= 0 else 0
            parent = node
            node = self.ch[node][side]
        return self.lv[parent][side]

    def lc(self, node, end, start, outside):
        while node != -1:
            d1, d2 = self.dot(node, start), self.dot(node, end)
            c = self.csg[node]
            if d1 > -0.001 and d2 > -0.001:
                outside = outside or c
                node = self.ch[node][1]
            elif d1 < 0.001 and d2 < 0.001:
                outside = outside and not c
                node = self.ch[node][0]
            else:
                t = d1 / (d2 - d1)
                mid = tuple(s + (s - e) * t for s, e in zip(start, end))
                ff = 1 if d1 > 0 else 0
                near = (outside or c) if ff else (outside and not c)
                if not self.lc(self.ch[node][ff], mid, start, near):
                    return False
                outside = (outside and not c) if ff else (outside or c)
                node = self.ch[node][1 - ff]
                start = mid
        return bool(outside)

    def trace(self, a, b):
        return self.lc(0, tuple(b), tuple(a), self.root)


def main():
    rows = list(csv.DictReader((SCRATCH / "gate_lostrace.csv").open()))
    trees, t3ds, mans = {}, {}, {}
    CONVS = {
        "pawn": (0, 0, 0.0),
        "eye": (0, 0, 29.4),
        "pawn+8": (0, 0, 8.0),
        "pawn+16": (0, 0, 16.0),
        "pawn-8": (0, 0, -8.0),
        "eye+8": (0, 0, 37.4),
    }
    score = {k: [0, 0] for k in CONVS}   # right, total
    detail = []
    for row in rows:
        m, f, L = row["map"], row["pose"], row["corona"]
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
        loc = tuple(map(float, t3ds[m][L]["location"]))
        golden_shows = row["verdict"] == "false_hide"
        lset = set(t.coronas.get(L, []))
        preds = {}
        for k, (dx, dy, dz) in CONVS.items():
            p = (pawn[0] + dx, pawn[1] + dy, pawn[2] + dz)
            pred = t.leaf(p) in lset and t.trace(cam, loc)
            preds[k] = pred
            score[k][1] += 1
            score[k][0] += (pred == golden_shows)
        detail.append((m, f, L, golden_shows, preds))

    for k, (r, n) in sorted(score.items(), key=lambda kv: -kv[1][0]):
        print(f"{k:8s}: {r}/{n} rows predicted correctly")
    print()
    best = max(score, key=lambda k: score[k][0])
    print(f"rows the '{best}' convention still gets wrong:")
    for m, f, L, g, preds in detail:
        if preds[best] != g:
            print(f"  {m} {f} {L}: golden={'show' if g else 'hide'} "
                  f"pred={'show' if preds[best] else 'hide'} "
                  f"(pawn={preds['pawn']}, eye={preds['eye']})")


if __name__ == "__main__":
    main()
