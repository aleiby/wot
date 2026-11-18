"""Census retail per-surface dynamic-mover lights across the 19 MP maps.

The script consumes only checked-in raw T3D, zones exports, and retail package
geometry. It also emits the real mover GLBs and validates that each primitive's
bytes and plane extras agree with its UE-local runtime manifest row.
"""
import json
import re
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import reference_root  # noqa: E402

from bsp2obj import build_groups, groups_to_glb, split_mover_groups_by_plane
from bsp_linecheck import BspTree
from mover_light_select import select_mover_surface_lights, transform_plane
from t3d import parse, typed
from ue1pkg import Package, decode_model


MOVER_CLASSES = {
    "Mover", "DoorMover", "LockableMover", "PortcullisMover",
    "ExplodingMover", "RotatingMover", "LoopMover", "TimedMover",
    "AssertMover", "ElevatorMover", "AttachMover", "GradualMover",
    "MixMover",
}
MODEL_RE = re.compile(r"Model'[^.]+\.(\w+)'")
TUPLE_RE = re.compile(r"([A-Za-z]+)=(-?[0-9.]+)")


def _tuple(value, keys):
    fields = {key.lower(): float(number)
              for key, number in TUPLE_RE.findall(value or "")}
    return tuple(fields.get(key, 0.0) for key in keys)


def _actor_pose(actor):
    props = actor["props"]
    location = (_tuple(props["BasePos"], "xyz")
                if "BasePos" in props else tuple(actor["location"]))
    rotation = _tuple(props.get("BaseRot", ""),
                      ("pitch", "yaw", "roll"))
    return location, rotation


def _read_glb(path):
    """Return the JSON document and BIN bytes from a GLB 2 file."""
    payload = Path(path).read_bytes()
    if len(payload) < 12:
        raise ValueError(f"truncated GLB header: {path}")
    magic, version, total = struct.unpack_from("<III", payload)
    if magic != 0x46546C67 or version != 2 or total != len(payload):
        raise ValueError(f"invalid GLB 2 header: {path}")
    chunks = {}
    offset = 12
    while offset < total:
        if offset + 8 > total:
            raise ValueError(f"truncated GLB chunk header: {path}")
        size, kind = struct.unpack_from("<II", payload, offset)
        offset += 8
        end = offset + size
        if end > total:
            raise ValueError(f"truncated GLB chunk: {path}")
        chunks[kind] = payload[offset:end]
        offset = end
    if 0x4E4F534A not in chunks or 0x004E4942 not in chunks:
        raise ValueError(f"GLB is missing JSON or BIN data: {path}")
    return json.loads(chunks[0x4E4F534A].decode()), chunks[0x004E4942]


def _glb_vec3_positions(document, binary, accessor_index):
    if accessor_index is None:
        raise ValueError("mover GLB primitive has no POSITION accessor")
    accessor = document["accessors"][int(accessor_index)]
    if accessor.get("componentType") != 5126 or accessor.get("type") != "VEC3":
        raise ValueError("mover GLB POSITION accessor is not float VEC3")
    view = document["bufferViews"][int(accessor["bufferView"])]
    stride = int(view.get("byteStride", 12))
    if stride < 12:
        raise ValueError("mover GLB POSITION stride is smaller than VEC3")
    start = int(view.get("byteOffset", 0)) + int(accessor.get("byteOffset", 0))
    positions = []
    for index in range(int(accessor["count"])):
        offset = start + index * stride
        if offset + 12 > len(binary):
            raise ValueError("mover GLB POSITION accessor exceeds BIN data")
        positions.append(struct.unpack_from("<3f", binary, offset))
    return positions


def _vectors_close(actual, expected, tolerance):
    return len(actual) == len(expected) and all(
        abs(float(actual[index]) - float(expected[index])) <= tolerance
        for index in range(len(expected)))


def validate_mover_glb_planes(path, surfaces, tolerance=1.0e-4):
    """Validate emitted GLB vertices/extras against movers.json local planes.

    Returns the number of nonplanar primitives. Structural or metadata drift is
    fatal because a primitive can no longer be paired with its runtime surface
    row in that case.
    """
    document, binary = _read_glb(path)
    by_slot = {surface["slot"]: surface for surface in surfaces}
    if len(by_slot) != len(surfaces):
        raise ValueError(f"duplicate mover manifest plane slot: {path}")
    seen = set()
    nonplanar = 0
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            material_index = int(primitive.get("material", -1))
            materials = document.get("materials", [])
            if not 0 <= material_index < len(materials):
                raise ValueError(f"mover GLB primitive has no material: {path}")
            slot = materials[material_index].get("name", "")
            if slot in seen or slot not in by_slot:
                raise ValueError(f"mover GLB/manifest slot mismatch {slot}: {path}")
            seen.add(slot)
            surface = by_slot[slot]
            plane = primitive.get("extras", {}).get("wotMoverPlane")
            if not isinstance(plane, dict):
                raise ValueError(f"mover GLB primitive has no plane {slot}: {path}")
            local_origin = surface["origin"]
            local_normal = surface["normal"]
            expected_origin = (float(local_origin[0]) / 100.0,
                               float(local_origin[2]) / 100.0,
                               float(local_origin[1]) / 100.0)
            expected_normal = (float(local_normal[0]),
                               float(local_normal[2]),
                               float(local_normal[1]))
            if (not _vectors_close(plane.get("origin", ()), expected_origin,
                                   tolerance)
                    or not _vectors_close(plane.get("normal", ()),
                                          expected_normal, tolerance)):
                raise ValueError(
                    f"mover GLB/manifest plane conversion mismatch {slot}: {path}")
            positions = _glb_vec3_positions(
                document, binary, primitive.get("attributes", {}).get("POSITION"))
            if not positions:
                raise ValueError(f"mover GLB primitive has no positions {slot}: {path}")
            origin = tuple(float(value) for value in plane["origin"])
            normal = tuple(float(value) for value in plane["normal"])
            if any(abs(sum((point[axis] - origin[axis]) * normal[axis]
                           for axis in range(3))) > tolerance
                   for point in positions):
                nonplanar += 1
    if seen != set(by_slot):
        missing = sorted(set(by_slot) - seen)
        raise ValueError(f"mover manifest planes missing from GLB {missing}: {path}")
    return nonplanar


def census(repo_root=None):
    repo = reference_root(repo_root)
    zone_root = repo / "exports/zones"
    map_names = sorted(path.name[:-len(".zones.json")]
                       for path in zone_root.glob("*.zones.json"))
    rows = []
    witnesses = []
    for map_name in map_names:
        zone_data = json.loads(
            (zone_root / f"{map_name}.zones.json").read_text())
        tree = BspTree.from_export(zone_data)
        actors = typed(parse(repo / f"exports/t3d/{map_name}.t3d"))
        actors_by_name = {actor["name"]: actor for actor in actors}
        dynamic_movers = [
            actor for actor in actors
            if actor["cls"] in MOVER_CLASSES
            and actor["props"].get("bDynamicLightMover", "").lower()
            == "true"
        ]
        missing = [name for name in zone_data["light_actors"]
                   if name not in actors_by_name]
        if missing:
            raise ValueError(f"{map_name}: missing light positions: {missing}")
        light_rows = []
        for index, light_name in enumerate(zone_data["light_actors"]):
            brightness, radius, special, emits = zone_data["light_props"][index]
            light_rows.append(dict(
                name=light_name,
                location=tuple(actors_by_name[light_name]["location"]),
                brightness=int(brightness), radius=float(radius),
                special_lit=bool(special), emits=bool(emits),
            ))

        package = Package(repo / f"original/Maps/{map_name}.wot")
        exports_by_name = {export["name"]: export
                           for export in package.exports}
        section_count = 0
        nonplanar_count = 0
        maximum_run = 0
        maximum_filtered = 0
        maximum_emitting = 0
        map_witness = None
        for actor in dynamic_movers:
            match = MODEL_RE.search(actor["props"].get("Brush", ""))
            if not match or match.group(1) not in exports_by_name:
                raise ValueError(f"{map_name}.{actor['name']}: missing brush model")
            model_name = match.group(1)
            bsp = decode_model(package, exports_by_name[model_name])
            groups, texflags, _stats = build_groups(
                package, bsp, {}, include_planes=True)
            split, _split_flags, surfaces, extras = \
                split_mover_groups_by_plane(groups, texflags, bsp["points"])
            section_count += len(surfaces)
            with tempfile.TemporaryDirectory(prefix="wot_mover_census_") as temp:
                glb_path = Path(temp) / f"{map_name}_{actor['name']}.glb"
                groups_to_glb(glb_path, model_name, split, bsp["points"],
                              primitive_extras=extras)
                nonplanar_count += validate_mover_glb_planes(glb_path, surfaces)

            location, rotation = _actor_pose(actor)
            region = tree.point_region(location)
            run_indices = zone_data["leaf_lights"].get(str(region.leaf), [])
            maximum_run = max(maximum_run, len(run_indices))
            permeating = [light_rows[int(index)] for index in run_indices]
            mover_special = actor["props"].get(
                "bSpecialLit", "").lower() == "true"
            for surface_index, surface in enumerate(surfaces):
                origin, normal = transform_plane(
                    location, rotation, surface["origin"], surface["normal"])
                filtered = select_mover_surface_lights(
                    permeating, origin, normal, mover_special,
                    emitting_only=False)
                emitting = select_mover_surface_lights(
                    permeating, origin, normal, mover_special,
                    emitting_only=True)
                maximum_emitting = max(maximum_emitting, len(emitting))
                if len(filtered) > maximum_filtered:
                    maximum_filtered = len(filtered)
                    map_witness = dict(
                        map=map_name, mover=actor["name"], cls=actor["cls"],
                        model=model_name, leaf=region.leaf,
                        surface=surface_index, slot=surface["slot"],
                        origin=origin, normal=normal,
                        lights=[light["name"] for light in filtered],
                        emitting=[light["name"] for light in emitting],
                    )
        row = dict(
            map=map_name, dynamic_movers=len(dynamic_movers),
            planar_sections=section_count, nonplanar_sections=nonplanar_count,
            max_leaf_run=maximum_run, max_filtered=maximum_filtered,
            max_emitting=maximum_emitting,
        )
        rows.append(row)
        if map_witness:
            witnesses.append(map_witness)
    return rows, witnesses


def main():
    rows, witnesses = census(sys.argv[1] if len(sys.argv) > 1 else None)
    print("map,dynamic_movers,planar_sections,nonplanar_sections,"
          "max_leaf_run,max_filtered,max_emitting")
    for row in rows:
        print(",".join(str(row[key]) for key in (
            "map", "dynamic_movers", "planar_sections", "nonplanar_sections",
            "max_leaf_run", "max_filtered", "max_emitting")))
    maximum = max(row["max_filtered"] for row in rows)
    print(f"maximum_filtered={maximum}")
    for witness in witnesses:
        if len(witness["lights"]) == maximum:
            print(json.dumps(witness, separators=(",", ":")))


if __name__ == "__main__":
    main()
