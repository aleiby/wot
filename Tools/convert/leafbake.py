"""Extract the retail corona visibility bake -> exports/leaves/<Map>.leaves.json.

UE1 draws a light's corona only when the VIEWER'S BSP leaf carries that light
in its iPermeating run (renderer DrawFrame: Leaves(Region.iLeaf).iPermeating
-> null-terminated run in Model->Lights -> GAddCorona; the LeafLights[] path
holds only dynamic lights at runtime). iPermeating is shadow-aware and
leaf-granular — no distance rule reproduces it (19-map sweep: 26 residual
mismatches both ways). This bakes the exact per-light permeated-leaf sets
plus the BSP point->leaf walk data so the runtime can do the same lookup.

Emitted per map:
  nodes.planes    [[x,y,z,w] ...]      FBspNode.Plane (PlaneDot = P.xyz|pt - w)
  nodes.children  [[iBack,iFront] ...] -1 = none (walk ends)
  nodes.leaves    [[iLeaf0,iLeaf1] ...] leaf index on back/front side; -1 = none
  nodes.csg       [0|1 ...]            FBspNode.IsCsg(0) = NumVertices>0 &&
                                       !(NodeFlags & (NF_NotCsg|NF_IsNew)).
                                       Only csg nodes flip the LineCheck
                                       solidity state — non-solid/translucent/
                                       masked sheets and portals never block
                                       (the corona TRACE_VisBlocking trace).
  root_outside    UModel.RootOutside   initial Outside state for LineCheck
  num_leaves      len(Model->Leaves)
  coronas         {light name: sorted leaf list} for every STATIC corona light
                  (place_coronas' selection: bCorona + Skin, not Lightning).
                  A static light absent from every run has [] — retail NEVER
                  draws its corona (embedded/shadowed; the false-show class).
                  Non-static lights are omitted (runtime dynamic path ~ the
                  distance gate; none exist in the 19 MP maps).

Usage: python leafbake.py <Map.wot> <t3d.json> <out.json>
       python leafbake.py --all <conv_dir>   # 19 MP maps -> exports/leaves/
"""
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lightmaps import load_level, resolve_light  # noqa: E402
from wot_config import exports_dir, original_dir  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SKIN_RE = re.compile(r"Texture'([\w\-]+)\.(?:[\w\-]+\.)*([\w\-]+)'")


def permeating_sets(pkg, bsp):
    """{light name: set(leaf indices whose iPermeating run holds the light)}"""
    lights = bsp["lights"]
    out = {}
    for ileaf, (_izone, iperm, _ivol) in enumerate(bsp["leaves"]):
        if iperm is None or iperm < 0:
            continue
        i = iperm
        while i < len(lights) and lights[i] != 0:
            n = pkg.objname(lights[i])
            if n:
                out.setdefault(n, set()).add(ileaf)
            i += 1
    return out


def extract(map_path, t3d_path):
    pkg, bsp = load_level(map_path)
    sets = permeating_sets(pkg, bsp)
    actors = json.loads(Path(t3d_path).read_text())["actors"]
    lightning = {a["props"].get("LightTag", "").lower()
                 for a in actors if a["cls"].lower() == "lightning"} - {""}
    coronas = {}
    for a in actors:
        p = a.get("props", {})
        if (p.get("bCorona") != "True" or "Skin" not in p
                or p.get("Tag", "").lower() in lightning
                or not SKIN_RE.match(p["Skin"])):
            continue
        if not resolve_light(a).get("bStatic", True):
            continue                      # dynamic light: runtime leaf insert
        coronas[a["name"]] = sorted(sets.get(a["name"], set()))
    NF_NOTCSG, NF_ISNEW = 0x01, 0x20  # UE1 EBspNodeFlags
    return dict(
        nodes=dict(
            planes=[[round(float(c), 4) for c in pl]
                    for pl in bsp["node_planes"]],
            children=[list(c) for c in bsp["node_children"]],
            leaves=[list(l) for l in bsp["node_leaves"]],
            csg=[1 if n[2] > 0 and not (fl & (NF_NOTCSG | NF_ISNEW)) else 0
                 for n, fl in zip(bsp["nodes"], bsp["node_flags"])],
        ),
        num_leaves=len(bsp["leaves"]),
        root_outside=int(bool(bsp["root_outside"])),
        coronas=coronas,
    )


def main():
    if sys.argv[1:2] == ["--all"]:
        maps = ["Arena_01", "Arena_02", "Arena_03", "Arena_04", "Arena_05",
                "Arena_06", "Arena_07", "Arena_08", "Arena_09", "Arena_11",
                "Arena_12", "Battle2_01", "Battle2_02", "Battle2_03",
                "Battle2_04", "Battle2_05", "Battle3_01", "Battle3_02",
                "Battle4_01"]
        conv = Path(sys.argv[2])
        outdir = exports_dir() / "leaves"
        outdir.mkdir(parents=True, exist_ok=True)
        for m in maps:
            out = extract(str(original_dir() / "Maps" / f"{m}.wot"),
                          str(conv / f"{m}.t3d.json"))
            (outdir / f"{m}.leaves.json").write_text(
                json.dumps(out, separators=(",", ":")))
            ncor = len(out["coronas"])
            nempty = sum(1 for v in out["coronas"].values() if not v)
            print(f"{m}: {len(out['nodes']['planes'])} nodes, "
                  f"{out['num_leaves']} leaves, {ncor} corona light(s), "
                  f"{nempty} never-show")
    else:
        out = extract(sys.argv[1], sys.argv[2])
        Path(sys.argv[3]).write_text(json.dumps(out, separators=(",", ":")))
        print(f"{len(out['nodes']['planes'])} nodes, "
              f"{len(out['coronas'])} corona light(s)")


if __name__ == "__main__":
    main()
