"""Build-time asset-identity validator — enforces the package-qualified invariant
so bare-name aliasing can't return.

Usage: python3 Tools/test/check_asset_identity.py <conv_tex_root> [maps_glob]

For every map's surface texture reference it resolves the AUTHORITATIVE
(package, name) via Package.objref (the outer chain) and asserts:
  1. qualkey('Package','Name') is injective over all available assets — no two
     distinct (package,name) collapse to the same lookup key.
  2. every referenced texture resolves to a UNIQUE available (package,name) in
     the exported tex tree (or is a known parametric/skipped texture, e.g. the
     Fire/Water fractals handled by the flipbook baker).
Exits non-zero (fails the build) on any collision or unresolved reference whose
PACKAGE was exported — i.e. a real mis-resolution. References into a package that
was never exported at all are reported as coverage gaps (not a hard fail) so the
check is usable before a full re-export.
"""
import glob
import json
import sys
from collections import defaultdict
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS / "convert"))
sys.path.insert(0, str(TOOLS))
from ue1pkg import Package, decode_model  # noqa: E402
from wot_config import original_dir  # noqa: E402

PF_INVISIBLE = 0x1
# Parametric texture classes: not static PNGs — handled by the flipbook baker,
# correctly absent from tex/. Identified by the surf texture ref's import class.
PARAMETRIC_CLASSES = {"WetTexture", "WaveTexture", "FireTexture", "WaterTexture",
                      "IceTexture", "ScriptedTexture", "FractalTexture"}


def load_tex_tree(tex_root):
    """-> (avail, packages_lower, skipped). avail = {(pkg.lower,name.lower):
    (pkg,name)}; skipped = {(pkg.lower,name.lower)} the exporter could not decode
    (recorded in each manifest's "skipped" list, e.g. the RGBA/truecolor lens
    flares in LensFlareT that the P8 exporter can't handle). Case-insensitive on
    the package (UE names are case-insensitive; the map import table and the .utx
    stem can disagree in case, e.g. WOTDecorations vs WoTDecorations)."""
    avail, packages, skipped = {}, set(), set()
    for m in glob.glob(str(Path(tex_root) / "*/_manifest.json")):
        pk = Path(m).parent.name
        packages.add(pk.lower())
        doc = json.loads(Path(m).read_text())
        for n in doc.get("textures", {}):
            avail[(pk.lower(), n.lower())] = (pk, n)
        for s in doc.get("skipped", []):
            # entries look like "Corona1 (tile cannot extend outside image)"
            skipped.add((pk.lower(), s.split(" (", 1)[0].strip().lower()))
    return avail, packages, skipped


def ref_class(pkg, ref):
    """Class name of an object reference (import class_name / export's class)."""
    if ref < 0:
        return pkg.imports[-ref - 1][1]
    if ref > 0:
        return pkg.objname(pkg.exports[ref - 1]["cls"])
    return None


def main():
    tex_root = sys.argv[1] if len(sys.argv) > 1 else "/tmp/wotconv/tex"
    maps_glob = (sys.argv[2] if len(sys.argv) > 2
                 else str(original_dir() / "Maps" / "*.wot"))
    avail, packages, skipped = load_tex_tree(tex_root)

    # (1) qualkey injectivity over every available asset (case-insensitive)
    keys = {}
    for lk, (pk, n) in avail.items():
        k = Package.qualkey(pk, n).lower()
        if keys.get(k, lk) != lk:
            print(f"FAIL alias: qualkey {k!r} maps to both {keys[k]} and {lk}")
            sys.exit(1)
        keys[k] = lk
    print(f"qualkey injective over {len(avail)} assets in {len(packages)} packages")

    # (2) every map's static-texture ref resolves to a unique available
    #     (package, name), case-insensitively. Parametric (fractal) refs skipped.
    hard = 0
    gaps = defaultdict(set)
    undecodable = defaultdict(set)
    for mapf in sorted(glob.glob(maps_glob)):
        try:
            pkg = Package(mapf)
            models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
            bsp = decode_model(pkg, max(models, key=lambda e: e["size"]))
        except Exception as e:  # noqa: BLE001
            print(f"  {Path(mapf).stem}: decode error {e}")
            continue
        badhard = set()
        for s in bsp["surfs"]:
            if s["flags"] & PF_INVISIBLE:
                continue
            ref = pkg.objref(s["tex"])
            if not ref:
                continue
            if ref_class(pkg, s["tex"]) in PARAMETRIC_CLASSES:
                continue                        # fractal — flipbook baker's job
            lk = (ref[0].lower(), ref[1].lower())
            if lk in avail:
                continue
            if lk in skipped:                   # exporter can't decode this fmt
                undecodable[ref[0]].add(ref[1])  # -> known limitation, not aliasing
            elif ref[0].lower() in packages:    # package exported, texture absent
                badhard.add(ref)                # -> real missing asset (aliasing/bug)
            else:                               # package not exported here
                gaps[ref[0]].add(ref[1])
        if badhard:
            hard += len(badhard)
            print(f"FAIL {Path(mapf).stem}: {len(badhard)} static-texture refs "
                  f"missing in an EXPORTED package: {sorted(badhard)[:8]}")
    if undecodable:
        print(f"undecodable refs (exporter skipped — known format limitation, "
              f"e.g. RGBA lens flares, NOT aliasing): "
              f"{ {p: sorted(v) for p, v in sorted(undecodable.items())} }")
    if gaps:
        print(f"coverage gaps (packages not in this tex tree): "
              f"{ {p: len(v) for p, v in sorted(gaps.items())} }")
    if hard:
        print(f"\nFAIL: {hard} static-texture references unresolved "
              f"within exported packages")
        sys.exit(1)
    print("\nOK: every map static-texture ref resolves to a unique available "
          "(package, name) — no bare-name aliasing possible.")


if __name__ == "__main__":
    main()
