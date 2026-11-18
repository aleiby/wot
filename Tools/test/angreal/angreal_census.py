#!/usr/bin/env python3
"""Which angreal inventory classes can a multiplayer match hand out?

Derived only from the shipped export (the spec of record):

* default inventory  - giMPArena.AddDefaultInventory (Dart x25),
  giMPBattle.AddDefaultInventory (Dart x25 + one of AngrealSet1 + one of
  AngrealSet2) and giWOT.AddDefaultInventory (WOTPlayer.DefaultAngrealInventory
  of the four MP player classes);
* spawn pools        - every Common/Uncommon/RareAngrealSpawnPool placed in the
  19 MP maps, class defaultproperties overlaid by the actor's T3D overrides,
  reduced to the entries AngrealSpawnPool.GetWeightedSelection can return;
* fixed pickups      - every placed actor whose class derives from
  AngrealInventory.

A pool entry is reachable only when it lies BEFORE the first None entry
(GetWeightedSelection's total loop stops there, AngrealSpawnPool.uc:24-26, so
the cumulative pick can never pass it) and has a non-zero weight (a zero
weight never raises the running total past RequiredWeight, :38-43).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import exports_dir  # noqa: E402

EXPORTS = exports_dir()
SCRIPT = EXPORTS / "script"
T3D = EXPORTS / "t3d"

MP_MAPS = ("Arena_01", "Arena_02", "Arena_03", "Arena_04", "Arena_05", "Arena_06",
           "Arena_07", "Arena_08", "Arena_09", "Arena_11", "Arena_12",
           "Battle2_01", "Battle2_02", "Battle2_03", "Battle2_04", "Battle2_05",
           "Battle3_01", "Battle3_02", "Battle4_01")

# The release checklist's p3-angreal rows (brief section A).
CHECKLIST = ("AMA", "Absorb", "AirBurst", "AirShield", "Balefire", "Champion", "Dart",
             "Decay", "Disguise", "DistantEye", "EarthShield", "EarthTremor", "ExpWard",
             "FireShield", "Fireball", "Fireworks", "Fork", "Guardian", "Heal", "Ice",
             "Illusion", "LGBlue", "LGGreen", "LGPurple", "LGRed", "LGYellow", "Levitate",
             "LightGlobe", "Lightning", "Minion", "Reflect", "RemoveCurse", "Seeker",
             "Shield", "Shift", "SoulBarb", "Special", "SpiritShield", "SwapPlaces",
             "Taint", "Target", "Tracer", "TrapDetect", "WallOfAir", "WaterShield",
             "Whirlwind")

CLASS_RE = re.compile(r"^\s*class\s+(\w+)\s+(?:expands|extends)\s+(\w+)", re.I | re.M)
POOL_RE = re.compile(r"^\s*Pool\((\d+)\)=(?:Class'\w+\.(\w+)'|None)\s*$", re.I)
WEIGHT_RE = re.compile(r"^\s*Weight\((\d+)\)=(\d+)\s*$", re.I)
ACTOR_RE = re.compile(r"^Begin Actor Class=(\w+) Name=(\w+)")
GIVE_RE = re.compile(r'GivePawnAngreal\(\s*PlayerPawn,\s*"Angreal\.AngrealInv(\w+)"')
SET_RE = re.compile(r'AngrealSet([12])\(\d\)="Angreal\.AngrealInv(\w+)"')
DEFAULT_RE = re.compile(r"DefaultAngrealInventory=Class'Angreal\.AngrealInv(\w+)'")


def rel(path: Path) -> str:
    return path.relative_to(REPO).as_posix()


def short_name(cls: str) -> str:
    return cls[len("AngrealInv"):] if cls.startswith("AngrealInv") else cls


def class_graph() -> dict[str, tuple[str, Path]]:
    graph = {}
    for uc in SCRIPT.rglob("*.uc"):
        text = uc.read_text(encoding="latin-1", errors="replace")
        match = CLASS_RE.search(text)
        if match:
            graph[match.group(1).lower()] = (match.group(2), uc)
    return graph


def derives(graph, name: str, base: str) -> bool:
    seen = set()
    name = name.lower()
    while name not in seen:
        if name == base.lower():
            return True
        if name not in graph:
            return False
        seen.add(name)
        name = graph[name][0].lower()
    return False


def default_pool(uc: Path) -> tuple[dict[int, str | None], dict[int, int]]:
    pools, weights = {}, {}
    for line in uc.read_text(encoding="latin-1").splitlines():
        pool = POOL_RE.match(line)
        weight = WEIGHT_RE.match(line)
        if pool:
            pools[int(pool.group(1))] = pool.group(2)
        elif weight:
            weights[int(weight.group(1))] = int(weight.group(2))
    return pools, weights


def reachable(pools: dict[int, str | None], weights: dict[int, int]) -> list[str]:
    """Entries AngrealSpawnPool.GetWeightedSelection can actually return."""
    out = []
    for index in range(16):
        cls = pools.get(index)
        if cls is None:
            break
        if weights.get(index, 0) > 0:
            out.append(cls)
    return out


def actors(t3d: Path):
    """Yield (class, name, first_line, [(line_no, top-level property line)])."""
    current = None
    with t3d.open(encoding="latin-1") as handle:
        for number, line in enumerate(handle, 1):
            if line.startswith("Begin Actor "):
                match = ACTOR_RE.match(line)
                current = (match.group(1), match.group(2), number, []) if match else None
            elif line.startswith("End Actor"):
                if current:
                    yield current
                current = None
            elif current is not None and line.startswith("    ") and not line.startswith("     "):
                current[3].append((number, line.rstrip("\n")))


def default_inventory() -> dict[str, list[dict]]:
    """Checklist rows granted by the MP game types' default inventory."""
    rows: dict[str, list[dict]] = {}

    def add(cls, cite, how):
        rows.setdefault(cls, []).append({"source": "default-inventory", "how": how,
                                         "cite": cite})

    for game in ("giMPArena", "giMPBattle"):
        uc = SCRIPT / "WOT" / f"{game}.uc"
        for number, line in enumerate(uc.read_text(encoding="latin-1").splitlines(), 1):
            match = GIVE_RE.search(line)
            if match:
                add(match.group(1), f"{rel(uc)}:{number}", f"{game}.AddDefaultInventory")
            match = SET_RE.search(line)
            if match:
                add(match.group(2), f"{rel(uc)}:{number}",
                    f"{game}.AddDefaultInventory AngrealSet{match.group(1)} (random 1 of 3)")
    giwot = SCRIPT / "WOT" / "giWOT.uc"
    for player in ("AesSedai", "Forsaken", "Hound", "Whitecloak"):
        uc = SCRIPT / "WoTPawns" / f"{player}.uc"
        for number, line in enumerate(uc.read_text(encoding="latin-1").splitlines(), 1):
            match = DEFAULT_RE.search(line)
            if match:
                add(match.group(1), f"{rel(uc)}:{number} via {rel(giwot)}:83-89",
                    f"giWOT.AddDefaultInventory ({player}.DefaultAngrealInventory)")
    return rows


def census() -> dict:
    graph = class_graph()
    pool_classes = {name: default_pool(graph[name.lower()][1])
                    for name in ("CommonAngrealSpawnPool", "UncommonAngrealSpawnPool",
                                 "RareAngrealSpawnPool")}
    rows: dict[str, list[dict]] = default_inventory()
    extras: dict[str, list[dict]] = {}
    per_map = {}
    for map_name in MP_MAPS:
        t3d = T3D / f"{map_name}.t3d"
        map_pools, map_fixed = 0, 0
        for cls, name, first, props in actors(t3d):
            if derives(graph, cls, "AngrealSpawnPool"):
                base = pool_classes.get(cls)
                if base is None:
                    raise SystemExit(f"unexpected spawn-pool class {cls} in {map_name}")
                pools, weights = dict(base[0]), dict(base[1])
                for _, text in props:
                    pool = POOL_RE.match(text)
                    weight = WEIGHT_RE.match(text)
                    if pool:
                        pools[int(pool.group(1))] = pool.group(2)
                    elif weight:
                        weights[int(weight.group(1))] = int(weight.group(2))
                map_pools += 1
                for entry in reachable(pools, weights):
                    short = short_name(entry)
                    target = rows if short in CHECKLIST else extras
                    target.setdefault(short, []).append({
                        "source": "spawn-pool", "map": map_name, "actor": name,
                        "cite": f"exports/t3d/{map_name}.t3d:{first}"})
            elif derives(graph, cls, "AngrealInventory"):
                map_fixed += 1
                short = short_name(cls)
                target = rows if short in CHECKLIST else extras
                target.setdefault(short, []).append({
                    "source": "fixed-pickup", "map": map_name, "actor": name,
                    "cite": f"exports/t3d/{map_name}.t3d:{first}"})
        per_map[map_name] = {"spawn_pools": map_pools, "fixed_angreal": map_fixed}
    result = {"schema": "wot-angreal-census-v1", "maps": per_map, "classes": {},
              "extras": {}}
    for short in CHECKLIST:
        evidence = rows.get(short, [])
        result["classes"][short] = {
            "obtainable": bool(evidence),
            "sources": sorted({item["source"] for item in evidence}),
            "maps": sorted({item["map"] for item in evidence if "map" in item}),
            "count": len(evidence), "evidence": evidence[:6]}
    for short, evidence in sorted(extras.items()):
        result["extras"][short] = {"sources": sorted({i["source"] for i in evidence}),
                                   "maps": sorted({i["map"] for i in evidence}),
                                   "evidence": evidence[:3]}
    return result


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", help="write the census JSON here")
    args = parser.parse_args(argv)
    result = census()
    if args.out:
        Path(args.out).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    obtainable = [k for k, v in result["classes"].items() if v["obtainable"]]
    missing = [k for k, v in result["classes"].items() if not v["obtainable"]]
    print(f"obtainable {len(obtainable)}: {', '.join(obtainable)}")
    print(f"not obtainable {len(missing)}: {', '.join(missing)}")
    print(f"extras: {', '.join(result['extras'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
