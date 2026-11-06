"""Pure-Python AngrealSpawnPool class-default and effective-slot helpers.

UE1 T3D actor properties are deltas against the actor class defaults. The
exported UnrealScript defaultproperties are therefore the source of truth for
every unauthored Pool/Weight slot. Keep this module editor-independent so the
map importer, reconcile patcher, and unit tests all use the same fold.
"""
import os
import re
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import exports_dir  # noqa: E402
EXPORTS = Path(os.environ.get("WOT_EXPORTS_DIR", exports_dir(required=False)))
ANGREAL_SCRIPT = EXPORTS / "script" / "Angreal"

POOL_CLASSES = (
    "AngrealSpawnPool",
    "CommonAngrealSpawnPool",
    "UncommonAngrealSpawnPool",
    "RareAngrealSpawnPool",
)

_PORTED_ANGREAL = [
    "Fireball", "Fireworks", "Seeker", "Taint", "Dart", "Ice", "Decay",
    "SoulBarb", "Heal", "Levitate", "Whirlwind", "Shift", "EarthTremor",
    "Balefire", "SwapPlaces", "Reflect", "Fork", "Absorb", "AirBurst",
    "RemoveCurse", "AMA", "ExpWard", "Special", "Target", "TrapDetect",
    "WallOfAir", "Illusion", "DistantEye",
    "Tracer", "Lightning", "Champion", "Guardian", "Minion", "Disguise",
    "Shield", "FireShield", "AirShield", "WaterShield", "EarthShield",
    "SpiritShield",
]
ANGREAL_CLASSMAP = {
    f"AngrealInv{name}": f"WoTAngrealInv{name}" for name in _PORTED_ANGREAL
}

_DEFAULTPROPERTIES = re.compile(
    r"\bdefaultproperties\s*\{(?P<body>.*?)\}", re.IGNORECASE | re.DOTALL)
_POOL_DEFAULT = re.compile(
    r"^\s*Pool\((\d+)\)\s*=\s*(None|Class'Angreal\.[^']+')\s*$",
    re.IGNORECASE | re.MULTILINE)
_WEIGHT_DEFAULT = re.compile(
    r"^\s*Weight\((\d+)\)\s*=\s*(-?\d+)\s*$",
    re.IGNORECASE | re.MULTILINE)


def _default_overrides(path):
    """Return the Pool/Weight assignments in one .uc defaultproperties."""
    text = path.read_text(encoding="latin-1")
    match = _DEFAULTPROPERTIES.search(text)
    if match is None:
        raise ValueError(f"{path}: no defaultproperties block")
    body = match.group("body")
    pools = {int(index): value for index, value in _POOL_DEFAULT.findall(body)}
    weights = {
        int(index): int(value) for index, value in _WEIGHT_DEFAULT.findall(body)
    }
    invalid = sorted((set(pools) | set(weights)) - set(range(16)))
    if invalid:
        raise ValueError(f"{path}: out-of-range pool defaults {invalid}")
    return pools, weights


def _pool_defaults(script_dir=ANGREAL_SCRIPT):
    """Parse base defaults first, then overlay each rarity subclass."""
    base_pools, base_weights = _default_overrides(
        script_dir / "AngrealSpawnPool.uc")
    defaults = {}
    for cls in POOL_CLASSES:
        pools = dict(base_pools)
        weights = dict(base_weights)
        if cls != "AngrealSpawnPool":
            class_pools, class_weights = _default_overrides(
                script_dir / f"{cls}.uc")
            pools.update(class_pools)
            weights.update(class_weights)
        defaults[cls] = tuple({
            "pool": pools.get(index),
            "weight": weights.get(index, 0),
        } for index in range(16))
    return defaults


# Module-scope parse is intentional: the exported .uc files are the spec and a
# missing/malformed spec must fail the importer/patcher before it mutates maps.
POOL_DEFAULTS = _pool_defaults()


def _angreal_class_name(ref):
    """Class'Angreal.AngrealInvSeeker' -> AngrealInvSeeker; None -> None."""
    if not ref or str(ref).strip().casefold() in ("none", ""):
        return None
    value = str(ref).strip().strip("'\"")
    if value.lower().startswith("class'"):
        value = value[6:].rstrip("'")
    return value.split(".")[-1] if "." in value else value


def effective_pool_slots(props, cls):
    """Return ``[(index, UE1 class name or None, weight), ...]``.

    Presence, rather than truthiness, selects an authored T3D delta so an
    explicit ``Pool(i)=None`` clears the inherited class-default entry.
    """
    defaults = POOL_DEFAULTS.get(cls, ())
    slots = []
    for index in range(16):
        default = defaults[index] if index < len(defaults) else {}
        pool_key = f"Pool({index})"
        weight_key = f"Weight({index})"
        pool_ref = props[pool_key] if pool_key in props else default.get("pool")
        weight = props[weight_key] if weight_key in props \
            else default.get("weight", 0)
        slots.append((index, _angreal_class_name(pool_ref), int(weight)))
    return slots


def pool_entries(props, cls):
    """Return effective ported ``[(UE5 class name, weight), ...]`` entries."""
    entries = []
    for _index, name, weight in effective_pool_slots(props, cls):
        if not name or weight <= 0:
            continue
        ue5 = ANGREAL_CLASSMAP.get(name)
        if ue5:
            entries.append((ue5, weight))
    return entries


def pool_unported(props, cls):
    """Return effective positive-weight entries omitted by the port map."""
    return [(name, weight)
            for _index, name, weight in effective_pool_slots(props, cls)
            if name and weight > 0 and name not in ANGREAL_CLASSMAP]
