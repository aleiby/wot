"""Patch the retail NavigationPoint list and ReachSpec graph into MP maps.

Seal.GetTouchingPathNode walks every NavigationPoint and compares its cylinder
base against the placement floor (Seal.uc:184-201). Stamp all authored
NavigationPoint-family classes as AWoTPathNode data actors with source class,
location, bDark, CollisionHeight, ExtraCost, and ordered
Paths()/upstreamPaths() edges.
The Myrddraal still filters this full list through GetDarkFlag
(Myrddraal.uc:229-306). The stamped PlayerStart/Teleporter twins are nav-list
data actors, not duplicate gameplay starts or teleporters.

The plan_map() half is stdlib-only for dry inspection. Re-runs update actors by
stable T3D label, remove duplicates, replace their edge arrays, and save each
map once. Pawn movement remains on UE5 NavMesh.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_path_nodes.py [<Map> ...]"
"""
import json
import re
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parents[1] / "Tools" / "convert"))

from import_ambience import T3D_DIR  # noqa: E402
from t3d import parse, typed  # noqa: E402


# The shipped MP maps in graph-stamping scope. Arena_10 is not a retail map.
ARENA_MAPS = [
    "Arena_01", "Arena_02", "Arena_03", "Arena_04", "Arena_05",
    "Arena_06", "Arena_07", "Arena_08", "Arena_09", "Arena_11", "Arena_12",
]

BATTLE_MAPS = [
    "Battle2_01", "Battle2_02", "Battle2_03", "Battle2_04", "Battle2_05",
    "Battle3_01", "Battle3_02", "Battle4_01",
]

MP_MAPS = ARENA_MAPS + BATTLE_MAPS
REACHSPEC_DIR = _HERE.parents[1] / "Intermediate" / "Conv" / "reachspecs"

NODE_DEFAULT_HEIGHTS = {
    "PathNode": 50.0,
    "WotPathNode": 50.0,
    "MashadarPathNode": 50.0,
    "InventorySpot": 40.0,
    "PlayerStart": 40.0,
    "Teleporter": 40.0,
    "TeleporterWithEffects": 40.0,
}


def _bool(value):
    return str(value or "").lower() == "true"


def _load_reachspecs(map_name):
    path = REACHSPEC_DIR / f"{map_name}.json"
    if not path.is_file():
        raise FileNotFoundError(
            f"{map_name}: missing ReachSpec JSON {path}; run ue1pkg.py "
            "reachspecs before stamping path nodes")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"{map_name}: cannot read ReachSpec JSON {path}: {exc}") from exc
    reachspecs = payload.get("reachspecs") if isinstance(payload, dict) else None
    if not isinstance(reachspecs, list):
        raise ValueError(
            f"{map_name}: ReachSpec JSON {path} has no reachspecs array")
    for index, spec in enumerate(reachspecs):
        if (not isinstance(spec, dict) or
                not isinstance(spec.get("start"), str) or
                not isinstance(spec.get("end"), str) or
                not isinstance(spec.get("distance"), int) or
                isinstance(spec.get("distance"), bool)):
            raise ValueError(
                f"{map_name}: invalid ReachSpec record {index} in {path}")
    return reachspecs


def _indexed_reachspecs(map_name, label, props, property_name):
    pattern = re.compile(rf"^{re.escape(property_name)}\((\d+)\)$", re.IGNORECASE)
    slots = []
    for key, value in props.items():
        match = pattern.match(key)
        if match:
            slot = int(match.group(1))
            # UE1 NavigationPoint.Paths/upstreamPaths are fixed 16-slot arrays;
            # a larger slot index cannot come from a faithful export.
            if slot > 15:
                raise ValueError(
                    f"{map_name}: {label}.{key} exceeds the retail 16-slot "
                    "Paths/upstreamPaths array")
            try:
                spec_index = int(value)
            except (TypeError, ValueError) as exc:
                raise ValueError(
                    f"{map_name}: {label}.{key} has invalid ReachSpec index "
                    f"{value!r}") from exc
            slots.append((slot, spec_index))
    slots.sort(key=lambda pair: pair[0])
    return [spec_index for _, spec_index in slots]


def plan_map(map_name):
    """Plan NavigationPoint-family stamps and their authored edge arrays."""
    reachspecs = _load_reachspecs(map_name)
    nodes = []
    for actor in typed(parse(T3D_DIR / f"{map_name}.t3d")):
        actor_class = actor["cls"]
        if actor_class not in NODE_DEFAULT_HEIGHTS:
            continue
        props = actor.get("props", {})
        nodes.append({
            "label": actor["name"],
            "source_class": actor_class,
            "location": actor["location"],
            "dark": _bool(props.get("bDark")),
            "snipe": _bool(props.get("bSnipe")),
            "collision_height": float(props.get(
                "CollisionHeight", NODE_DEFAULT_HEIGHTS[actor_class])),
            "extra_cost": int(props.get("ExtraCost", 0)),
            "_out_spec_indices": _indexed_reachspecs(
                map_name, actor["name"], props, "Paths"),
            "_in_spec_indices": _indexed_reachspecs(
                map_name, actor["name"], props, "upstreamPaths"),
        })

    labels = {node["label"] for node in nodes}
    dropped = {"out": 0, "in": 0}

    def resolve(node, index_key, endpoint_key, direction):
        edges = []
        for spec_index in node.pop(index_key):
            if not 0 <= spec_index < len(reachspecs):
                raise ValueError(
                    f"{map_name}: {node['label']} references ReachSpec "
                    f"{spec_index}, but JSON contains {len(reachspecs)} records")
            spec = reachspecs[spec_index]
            endpoint = spec[endpoint_key]
            if endpoint not in labels:
                dropped[direction] += 1
                continue
            edges.append((endpoint, spec["distance"]))
        return edges

    for node in nodes:
        node["out_edges"] = resolve(
            node, "_out_spec_indices", "end", "out")
        node["in_edges"] = resolve(
            node, "_in_spec_indices", "start", "in")
    return {"nodes": nodes, "dropped_edges": dropped}


def _single_actor(eas, actors, label, actor_class, location,
                  legacy_label=None):
    # G34 stamps nav-list twins for imported gameplay actors (notably
    # Teleporter). Manage only AWoTPathNode actors: a same-label actor of another
    # class is the gameplay twin and must never be destroyed. legacy_label
    # migrates G23's retail-label dark-node stamps to the namespaced label.
    labels = {label, legacy_label} - {None}
    matches = [actor for actor in actors
               if isinstance(actor, actor_class)
               and actor.get_actor_label() in labels]
    keep = next(iter(matches), None)
    for actor in matches:
        if actor is not keep:
            eas.destroy_actor(actor)
    if keep is None:
        keep = eas.spawn_actor_from_class(actor_class, location)
        actors.append(keep)
    keep.set_actor_label(label)
    keep.set_actor_location(location, False, False)
    return keep


def main():
    import unreal

    maps = [name for name in sys.argv[1:] if not name.startswith("-")] or MP_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"PATHNODE {map_name} skipped: no imported map")
            continue
        if not les.load_level(asset):
            unreal.log_error(f"PATHNODE {map_name}: load_level failed")
            continue
        plan = plan_map(map_name)
        actors = list(eas.get_all_level_actors())
        stamped = {}
        for stamp in plan["nodes"]:
            node = _single_actor(eas, actors,
                                 f"NavList_{stamp['label']}",
                                 unreal.WoTPathNode,
                                 unreal.Vector(*stamp["location"]),
                                 legacy_label=stamp["label"])
            node.set_editor_property("bDark", stamp["dark"])
            node.set_editor_property("bSnipe", stamp["snipe"])
            node.set_editor_property(
                "SourceClass", unreal.Name(stamp["source_class"]))
            node.set_editor_property(
                "NodeCollisionHeight", stamp["collision_height"])
            node.set_editor_property("ExtraCost", stamp["extra_cost"])
            stamped[stamp["label"]] = node

        def make_edges(edge_specs):
            edges = []
            for other_label, distance in edge_specs:
                edge = unreal.WoTPathEdge()
                edge.set_editor_property("other", stamped[other_label])
                edge.set_editor_property("distance", float(distance))
                edges.append(edge)
            return edges

        # Exact UPROPERTY names: UE's python name mangling does NOT map
        # InEdges to "in_edges" (the In prefix trips it; probed 2026-07-24),
        # while exact-case names always resolve.
        for stamp in plan["nodes"]:
            node = stamped[stamp["label"]]
            node.set_editor_property(
                "OutEdges", make_edges(stamp["out_edges"]))
            node.set_editor_property(
                "InEdges", make_edges(stamp["in_edges"]))
        les.save_current_level()
        dropped = plan["dropped_edges"]
        unreal.log(
            f"PATHNODE {map_name}: nodes={len(plan['nodes'])} "
            f"dropped_out={dropped['out']} dropped_in={dropped['in']}")


if __name__ == "__main__":
    main()
