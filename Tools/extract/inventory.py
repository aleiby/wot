"""Inventory: INVENTORY.md + exports/SCRIPT_DIFF.md + MP-scope draft.

Reads original/ packages (via Tools/convert/ue1pkg.py), exports/script (shipped
333B source of record), exports/t3d (map semantics), devtree (dev-branch source)
and writes:
  INVENTORY.md                     — repo-root summary (the port checklist seed)
  exports/SCRIPT_DIFF.md           — shipped vs devtree cross-diff summary
  exports/inventory/*.json         — machine-readable detail
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools"))
sys.path.insert(0, str(ROOT / "Tools/convert"))
from ue1pkg import Package  # noqa: E402
from wot_config import devtree_dir, exports_dir, original_dir  # noqa: E402

ORIGINAL = original_dir()
DEVTREE = devtree_dir()
EXPORTS = exports_dir(required=False)
OUT = EXPORTS / "inventory"
OUT.mkdir(parents=True, exist_ok=True)

# shipped script package -> devtree source dir
DEVMAP = {
    "Angreal": "Angreal", "Environment": "Environment", "Fire": "Fire",
    "IpServer": "IpServer", "Legend": "Legend", "MachinShin": "MachinShin",
    "ParticleSystems": "ParticleSystems", "UBrowser": "UBrowser",
    "UWindow": "UWindow", "WOT": "WOT", "WoTBase": "WOTBase",
    "WoTBrowser": "WOTBrowser", "WoTDecorations": "WOTDecorations",
    "WoTPawns": "WOTPawns", "WoTTraps": "WOTTraps",
}
MP_MAP_PAT = re.compile(r"^(Arena_|Battle)", re.I)
GAME_SEEDS = {"giMPArena", "giMPBattle", "giWOT"}


def normalize_body(text):
    """Code body for drift comparison: strip CR/comments/blank lines and the
    defaultproperties block (exported defaults are canonical, dev are hand-written)."""
    text = text.replace("\r", "")
    text = re.sub(r"(?im)^\s*defaultproperties\b.*", "", text, flags=re.S)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    lines = [ln.rstrip() for ln in text.split("\n") if ln.strip()]
    return "\n".join(lines).lower()


def script_stats():
    pkgs = {}
    classes = {}  # class name (lower) -> (pkg, path)
    for pdir in sorted((EXPORTS / "script").iterdir()):
        ucs = sorted(pdir.glob("*.uc"))
        pkgs[pdir.name] = dict(classes=len(ucs),
                               lines=sum(len(u.read_text(encoding="latin-1").splitlines()) for u in ucs))
        for u in ucs:
            classes[u.stem.lower()] = (pdir.name, u)
    return pkgs, classes


def parse_class_graph(classes):
    parent, refs = {}, {}
    decl = re.compile(r"(?im)^\s*class\s+([A-Za-z0-9_]+)\s+(?:expands|extends)\s+([A-Za-z0-9_.]+)")
    cref = re.compile(r"(?i)class'([A-Za-z0-9_.]+)'")
    for name, (_pkg, path) in classes.items():
        t = path.read_text(encoding="latin-1")
        m = decl.search(t)
        if m:
            parent[name] = m.group(2).split(".")[-1].lower()
        refs[name] = {r.split(".")[-1].lower() for r in cref.findall(t)}
    return parent, refs


def t3d_histograms():
    hists = {}
    actor = re.compile(rb"Begin Actor Class=([A-Za-z0-9_]+)")
    for t in sorted((EXPORTS / "t3d").glob("*.t3d")):
        h = {}
        for m in actor.finditer(t.read_bytes()):
            c = m.group(1).decode()
            h[c] = h.get(c, 0) + 1
        hists[t.stem] = h
    return hists


def mp_scope(parent, refs, hists, classes):
    seeds = {s.lower() for s in GAME_SEEDS}
    for mapname, h in hists.items():
        if MP_MAP_PAT.match(mapname):
            seeds |= {c.lower() for c in h if c.lower() in classes}
    scope = set()
    frontier = set(seeds)
    while frontier:
        cur = frontier.pop()
        if cur in scope or cur not in classes:
            continue
        scope.add(cur)
        nxt = set()
        if cur in parent:
            nxt.add(parent[cur])
        nxt |= refs.get(cur, set())
        frontier |= {n for n in nxt if n in classes and n not in scope}
    return sorted(scope), sorted(s for s in seeds if s in classes)


def devtree_diff(pkgs):
    rows = []
    for spkg, dpkg in sorted(DEVMAP.items()):
        sdir = EXPORTS / "script" / spkg
        ddir = DEVTREE / "U2Radiosity" / dpkg / "Classes"
        s = {p.stem.lower(): p for p in sdir.glob("*.uc")}
        d = {p.stem.lower(): p for p in ddir.glob("*.uc")}
        both = set(s) & set(d)
        differ = sum(1 for c in both
                     if normalize_body(s[c].read_text(encoding="latin-1"))
                     != normalize_body(d[c].read_text(encoding="latin-1")))
        rows.append(dict(pkg=spkg, shipped=len(s), devtree=len(d),
                         only_shipped=sorted(s.keys() - d.keys()),
                         only_devtree=sorted(d.keys() - s.keys()),
                         differ=differ, identical=len(both) - differ))
    return rows


def pkg_summaries():
    out = {}
    for sub, pat in [("System", "*.u"), ("Textures", "*.utx"),
                     ("Sounds", "*.uax"), ("Maps", "*.wot")]:
        for f in sorted((ORIGINAL / sub).glob(pat)):
            try:
                p = Package(f)
                out[f"{sub}/{f.name}"] = dict(version=p.version,
                                              exports=len(p.exports),
                                              names=len(p.names),
                                              classes=p.export_class_histogram())
            except Exception as e:  # noqa: BLE001 — inventory must not die on one file
                out[f"{sub}/{f.name}"] = dict(error=str(e))
    return out


def main():
    pkgs, classes = script_stats()
    parent, refs = parse_class_graph(classes)
    hists = t3d_histograms()
    scope, seeds = mp_scope(parent, refs, hists, classes)
    diff = devtree_diff(pkgs)
    summ = pkg_summaries()

    (OUT / "script_stats.json").write_text(json.dumps(pkgs, indent=1))
    (OUT / "map_actor_histograms.json").write_text(json.dumps(hists, indent=1))
    (OUT / "mp_scope_draft.json").write_text(json.dumps(
        dict(seeds=seeds, scope=scope), indent=1))
    (OUT / "script_diff.json").write_text(json.dumps(diff, indent=1))
    (OUT / "package_summaries.json").write_text(json.dumps(summ, indent=1))

    mp_maps = sorted(m for m in hists if MP_MAP_PAT.match(m))
    total_uc = sum(p["classes"] for p in pkgs.values())
    total_ln = sum(p["lines"] for p in pkgs.values())

    md = ["# INVENTORY (M0)", "",
          "Generated by `Tools/extract/inventory.py` from `exports/` + `original/` + `devtree/`.",
          "Machine-readable detail: `exports/inventory/*.json`.", "",
          "## Shipped script (spec of record — 333B batchexport)", "",
          "| Package | Classes | Lines |", "|---|---|---|"]
    md += [f"| {k} | {v['classes']} | {v['lines']} |" for k, v in sorted(pkgs.items())]
    md += [f"| **total** | **{total_uc}** | **{total_ln}** |", "",
           "Engine-level packages (Core, Editor, Engine, IpDrv, WinDrv) shipped with "
           "stripped ScriptText — dev-branch source only, in `devtree/`.", "",
           "## Maps", "",
           f"45 retail maps, {len(mp_maps)} in MP scope (Arena_*, Battle*): "
           + ", ".join(mp_maps), "",
           "Per-map actor histograms: `exports/inventory/map_actor_histograms.json`.", "",
           "## MP scope (first cut)", "",
           f"Seeds: game types {sorted(GAME_SEEDS)} + every class placed in an MP map.",
           f"Transitive closure over `expands` + `class'X'` refs: **{len(scope)} classes** "
           f"(of {total_uc} shipped) — `exports/inventory/mp_scope_draft.json`.", "",
           "## Shipped vs devtree source drift", "",
           "| Package | shipped | devtree | only-shipped | only-devtree | differ | identical |",
           "|---|---|---|---|---|---|---|"]
    for r in diff:
        md += [f"| {r['pkg']} | {r['shipped']} | {r['devtree']} | "
               f"{len(r['only_shipped'])} | {len(r['only_devtree'])} | "
               f"{r['differ']} | {r['identical']} |"]
    md += ["", "Class-level lists in `exports/inventory/script_diff.json`. "
           "Comparison normalizes comments/whitespace and ignores defaultproperties "
           "formatting.", "",
           "## Binary package summary (original/)", "",
           "| Package | Ver | Exports |", "|---|---|---|"]
    for k, v in sorted(summ.items()):
        md += [f"| {k} | {v.get('version','?')} | {v.get('exports', v.get('error'))} |"]
    (ROOT / "INVENTORY.md").write_text("\n".join(md) + "\n")

    dmd = ["# Shipped (333B) vs devtree (Aug 2003) script cross-diff", "",
           "Normalized comparison (no comments/whitespace/defaultproperties).",
           "Where they differ, **shipped wins** for the port; devtree explains history.", ""]
    for r in diff:
        dmd += [f"## {r['pkg']} — {r['differ']} differ, {r['identical']} identical",
                f"- only in shipped: {', '.join(r['only_shipped']) or '—'}",
                f"- only in devtree: {', '.join(r['only_devtree']) or '—'}", ""]
    (EXPORTS / "SCRIPT_DIFF.md").write_text("\n".join(dmd) + "\n")
    print(f"classes={total_uc} lines={total_ln} scope={len(scope)} maps={len(hists)}")


if __name__ == "__main__":
    main()
