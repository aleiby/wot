"""Generate the T3D-class -> default-mesh mapping (decoration placement).

Usage: python3 class2mesh.py [out.json] [script_root]
Scans script_root (default: exports/script) for `Mesh=...Mesh'X'`
defaultproperties. Inventory-derived classes are excluded only after they gain
a functional fixed-placement importer; until then their decoration stand-ins
must remain visible. Output is
committed at exports/inventory/class2mesh.json (import_map.py reads the
conv-dir copy first, then falls back to the repo copy).
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import exports_dir  # noqa: E402

# Keep Inventory-derived mesh stand-ins until each class has a functional
# fixed-placement importer. Spawn-pool/leaf-BP support alone does not replace
# FIXED-authored T3D placements (G39).
FUNCTIONAL_PICKUP_IMPORTS = {
    "andilayroot",
    "angrealinvabsorb",
    "angrealinvbalefire",
    "angrealinvdart",
    "angrealinvdecay",
    "angrealinvearthtremor",
    "angrealinvexpward",
    "angrealinvfireball",
    "angrealinvfireshield",
    "angrealinvfork",
    "angrealinvheal",
    "angrealinvminion",
    "angrealinvreflect",
    "angrealinvshift",
    "angrealinvswapplaces",
    "angrealinvtarget",
    "angrealinvwatershield",
}


def inventory_derived_classes(script_root):
    """Return exported classes whose ancestry reaches Engine.Inventory."""
    parents = {}
    declaration = re.compile(
        r"(?im)^class\s+(\w+)(?:\s+(?:expands|extends)\s+([\w.]+))?")
    for uc in Path(script_root).glob("*/*.uc"):
        match = declaration.search(uc.read_text(encoding="latin-1"))
        if match:
            parents[match.group(1).lower()] = (
                (match.group(2) or "").split(".")[-1].lower())

    def is_inventory_derived(class_name):
        # Engine's stripped shipped source is absent from exports/script, so
        # the literal parent name Inventory is the terminal marker.
        seen = set()
        current = class_name
        while current and current not in seen:
            if current == "inventory":
                return True
            seen.add(current)
            current = parents.get(current, "")
        return False

    return {name for name in parents if is_inventory_derived(name)}


def class_to_mesh(script_root, functional_pickup_imports=None):
    """Return exported classes with meshes, applying the graduation set.

    Pass an explicit empty set to recover the pre-filter mapping.  Fixed-pickup
    reconcilers use that view to recompute the exact legacy decoration mesh
    they are allowed to remove; the committed output uses the default set.
    """
    if functional_pickup_imports is None:
        functional_pickup_imports = FUNCTIONAL_PICKUP_IMPORTS
    scripts = []
    declaration = re.compile(
        r"(?im)^class\s+(\w+)(?:\s+(?:expands|extends)\s+([\w.]+))?")
    for uc in Path(script_root).glob("*/*.uc"):
        text = uc.read_text(encoding="latin-1")
        class_match = declaration.search(text)
        if not class_match:
            continue
        class_name = class_match.group(1).lower()
        scripts.append((class_name, text))

    inventory_classes = inventory_derived_classes(script_root)

    result = {}
    for class_name, text in scripts:
        mesh_match = re.search(r"(?im)^\s*Mesh=.*?Mesh'([\w.]+)'", text)
        if (mesh_match
                and not (class_name in inventory_classes
                         and class_name in functional_pickup_imports)):
            result[class_name] = mesh_match.group(1).split(".")[-1]
    return result


def write_class_to_mesh(out, script_root):
    mapping = class_to_mesh(script_root)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(mapping, indent=0, sort_keys=True))
    print(f"{len(mapping)} classes -> {out}")
    return mapping


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1
               else exports_dir(required=False) / "inventory/class2mesh.json")
    script_root = Path(sys.argv[2] if len(sys.argv) > 2
                       else exports_dir() / "script")
    write_class_to_mesh(out, script_root)


if __name__ == "__main__":
    main()
