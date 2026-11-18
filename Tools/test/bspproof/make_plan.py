#!/usr/bin/env python3
"""Generate the deterministic Arena_07 packaged BSP proof input plan."""
from __future__ import annotations

import argparse
import copy
import json
import math
import sys
from pathlib import Path


FLOOR_COLUMNS = 16
FLOOR_ROWS = 8


class PlanError(ValueError):
    pass


def _finite_vector(value, dimensions, label):
    if not isinstance(value, list) or len(value) != dimensions:
        raise PlanError(f"{label} must contain {dimensions} numbers")
    result = [float(component) for component in value]
    if not all(math.isfinite(component) for component in result):
        raise PlanError(f"{label} contains a non-finite value")
    return result


def _render_positions(render):
    meshes = render.get("meshes")
    if not isinstance(meshes, list) or not meshes:
        raise PlanError("render_main.json has no meshes")
    positions = []
    for mesh in meshes:
        if not isinstance(mesh, dict) or not mesh.get("cpu_readable", False):
            raise PlanError("render_main.json contains an unreadable mesh")
        stream = mesh.get("positions")
        if not isinstance(stream, list) or not stream:
            raise PlanError("render mesh has no positions")
        positions.extend(_finite_vector(point, 3, "position") for point in stream)
    return positions


def _bounds(positions):
    return ([min(point[axis] for point in positions) for axis in range(3)],
            [max(point[axis] for point in positions) for axis in range(3)])


def _grid_point(minimum, maximum, column, row):
    x0 = minimum[0] + (maximum[0] - minimum[0]) * .05
    x1 = maximum[0] - (maximum[0] - minimum[0]) * .05
    y0 = minimum[1] + (maximum[1] - minimum[1]) * .05
    y1 = maximum[1] - (maximum[1] - minimum[1]) * .05
    x = x0 + (x1 - x0) * (column + .5) / FLOOR_COLUMNS
    y = y0 + (y1 - y0) * (row + .5) / FLOOR_ROWS
    return x, y


def _occupied_cells(positions, minimum, maximum):
    width = max(maximum[0] - minimum[0], 1.0)
    height = max(maximum[1] - minimum[1], 1.0)
    cells = set()
    for point in positions:
        column = min(FLOOR_COLUMNS - 1, max(0, int(
            (point[0] - minimum[0]) / width * FLOOR_COLUMNS)))
        row = min(FLOOR_ROWS - 1, max(0, int(
            (point[1] - minimum[1]) / height * FLOOR_ROWS)))
        cells.add((column, row))
    if not cells:
        cells.add((FLOOR_COLUMNS // 2, FLOOR_ROWS // 2))
    return sorted(cells)


def _camera_poses():
    # Literal Arena_07 golden-pose locations.  Rotation is converted from the
    # retail 65536-unit yaw convention to UE degrees.
    sources = (
        ("overview", (-323.0, -71.0, -45.531), -9840),
        ("lightmap_seam", (72.0, -71.0, -45.531), -23568),
        ("grazing_tangent", (71.0, -946.0, -45.531), -43056),
        ("portal_water", (-312.0, -948.0, -45.531), -58776),
        ("backface_winding", (-1024.0, -515.0, 82.469), -65424),
    )
    return [{"id": name, "location": list(location),
             "rotation": [0.0, yaw * 360.0 / 65536.0, 0.0], "fov": 90.0}
            for name, location, yaw in sources]


def make_plan(render, collision_oracle):
    positions = _render_positions(render)
    minimum, maximum = _bounds(positions)
    vertical_pad = max(100.0, (maximum[2] - minimum[2]) * .1)
    floor_rays = []
    for row in range(FLOOR_ROWS):
        for column in range(FLOOR_COLUMNS):
            x, y = _grid_point(minimum, maximum, column, row)
            floor_rays.append({
                "id": f"floor_{row:02d}_{column:02d}", "kind": "line",
                "start": [x, y, maximum[2] + vertical_pad],
                "end": [x, y, minimum[2] - vertical_pad],
            })

    cells = _occupied_cells(positions, minimum, maximum)
    directions = ((1.0, 0.0), (0.0, 1.0), (-1.0, 0.0), (0.0, -1.0),
                  (.707106781, .707106781), (-.707106781, .707106781),
                  (-.707106781, -.707106781), (.707106781, -.707106781))
    wall_length = max(maximum[0] - minimum[0], maximum[1] - minimum[1], 100.0)
    wall_z = minimum[2] + (maximum[2] - minimum[2]) * .5
    wall_rays = []
    for index in range(max(64, len(cells))):
        column, row = cells[index % len(cells)]
        x, y = _grid_point(minimum, maximum, column, row)
        dx, dy = directions[index % len(directions)]
        half = wall_length * .55
        wall_rays.append({
            "id": f"wall_{index:03d}", "kind": "line",
            "cell": [column, row],
            "start": [x - dx * half, y - dy * half, wall_z],
            "end": [x + dx * half, y + dy * half, wall_z],
        })

    x0, x1 = minimum[0], maximum[0]
    y0, y1 = minimum[1], maximum[1]
    xm, ym = (x0 + x1) * .5, (y0 + y1) * .5
    nav_z = minimum[2] + min(50.0, (maximum[2] - minimum[2]) * .25)
    nav_points = ((x0, y0), (x1, y0), (x1, y1), (x0, y1),
                  (xm, y0), (x1, ym), (xm, y1), (x0, ym), (xm, ym))
    nav_pairs_indices = ((0, 2), (1, 3), (0, 8), (1, 8), (2, 8), (3, 8),
                         (4, 6), (5, 7), (0, 5), (1, 6), (2, 7), (3, 4))
    nav_pairs = [{
        "id": f"nav_{index:02d}",
        "start": [nav_points[a][0], nav_points[a][1], nav_z],
        "end": [nav_points[b][0], nav_points[b][1], nav_z],
    } for index, (a, b) in enumerate(nav_pairs_indices)]

    projectiles = []
    center = [xm, ym, wall_z]
    # Non-portal shots anchor to location-sorted PlayerStarts (the harness
    # resolves anchor_start_index modulo the live start count): plan-literal
    # AABB-grid origins sit in UE1 SOLID space outside the playable volume and
    # a projectile actor spawned there never contacts anything (0/32 hits in
    # the first live run). floor/wall/oblique cycle per start deterministically.
    shot_kinds = ("floor", "wall", "oblique")
    for index in range(28):
        kind = shot_kinds[index % 3]
        projectiles.append({
            "id": f"projectile_{index:02d}",
            "class_path": "/Script/WoT.WoTDartProjectile",
            "anchor_start_index": index // 3,
            "shot_kind": kind,
            "radial_index": index % 8,
            "origin": [0.0, 0.0, 0.0],
            "direction": [0.0, 0.0, -1.0],
            "lifetime_ticks": 300,
            "portal_directed": False,
            "expect_block": kind == "floor",
        })
    portal_surfaces = (
        ([671.370102, -3889.758728, -1760.381866],
         [0.184288532, 0.0, 0.982872188]),
        ([-767.755697, -3941.481038, -1744.509155],
         [-0.184288532, 0.0, 0.982872188]),
        ([-90.520040, -4618.716309, -1744.509155],
         [0.0, -0.184288651, 0.982872188]),
        ([-128.000002, -512.000027, -448.0], [0.0, 0.0, 1.0]),
    )
    for surface_index, (surface, normal) in enumerate(portal_surfaces):
        projectiles.append({
            "id": f"projectile_{28 + surface_index:02d}",
            "class_path": "/Script/WoT.WoTDartProjectile",
            "origin": [surface[axis] + normal[axis] * 10.0 for axis in range(3)],
            "direction": [-component for component in normal],
            "lifetime_ticks": 2,
            "portal_directed": True,
            "expect_block": False,
        })

    plan = {
        "version": 1,
        "map": "/Game/WoT/Maps/Geo/Arena_07/Arena_07",
        "aabb": {"min": minimum, "max": maximum},
        "pawn": {
            "class_path": "/Script/WoT.WoTAesSedai",
            "capsule_radius": 17.0, "capsule_half_height": 39.0,
            "settle_seconds": 5.0, "floor_bound_z": minimum[2] - vertical_pad,
        },
        "player_start_radial_count": 8,
        "travel_count": 10,
        "floor_rays": floor_rays,
        "wall_rays": wall_rays,
        "projectiles": projectiles,
        "nav_pairs": nav_pairs,
        "seeking": {
            "class_path": "/Script/WoT.WoTSeekerProjectile",
            "spawn": [x0, ym, nav_z], "target": [x1, ym, nav_z],
            "obstruction_center": center, "lifetime_ticks": 600,
        },
        "camera_poses": _camera_poses(),
        "compact_probe_ids": [ray["id"] for ray in floor_rays[:8]]
                             + [ray["id"] for ray in wall_rays[:8]]
                             + [shot["id"] for shot in projectiles[:8]],
        "compact_nav_ids": [pair["id"] for pair in nav_pairs[:4]],
        "collision_oracle": copy.deepcopy(collision_oracle),
    }
    return plan


def _canonical_json(value):
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("render_main", type=Path)
    parser.add_argument("collision_oracle")
    parser.add_argument("out", type=Path)
    args = parser.parse_args(argv)
    try:
        render = json.loads(args.render_main.read_text(encoding="utf-8"))
        oracle = None if args.collision_oracle == "-" else json.loads(
            Path(args.collision_oracle).read_text(encoding="utf-8"))
        plan = make_plan(render, oracle)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(_canonical_json(plan), encoding="utf-8")
    except (OSError, json.JSONDecodeError, PlanError, TypeError, ValueError) as exc:
        print(f"make_plan.py: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
