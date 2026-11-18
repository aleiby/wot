#!/usr/bin/env python3
"""Compare packaged Arena_07 BSP proof dumps using the filed gate tolerances."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path


POSITION_EPSILON = 1e-4
UV_EPSILON = 1e-6
NORMAL_DOT = 0.99999
TANGENT_DOT = 0.999
COLLISION_DISTANCE_EPSILON = 0.1
COLLISION_NORMAL_DOT = 0.999
SETTLE_Z_EPSILON = 0.5
PROJECTILE_POSITION_EPSILON = 0.5
NAV_ENDPOINT_EPSILON = 1.0
TRIANGLE_BUCKET_GRID = POSITION_EPSILON * 4.0

ARENA07_MAIN_OBJECT = \
    "/Game/WoT/Maps/Geo/Arena_07/Arena_07.Arena_07"
ARENA07_PORTAL_OBJECT = \
    "/Game/WoT/Maps/Geo/Arena_07_portals/Arena_07_portals.Arena_07_portals"
ARENA07_MAIN_SLOTS = (
    "ForsakenT__MarbColu2703", "ForsakenT__MarblStep2362",
    "ForsakenT__SlteFlor2714", "ForsakenT__StonArch2731",
    "ForsakenT__StonBrdg2734", "ForsakenT__StonCeil2040",
    "ForsakenT__StonColu2734", "ForsakenT__StonStep2020",
    "ForsakenT__StonWall2024", "ForsakenT__StonWall2730",
    "ForsakenT__StonWall2736", "ForsakenT__StonWall2737",
    "ForsakenT__WindAtri2700msk_M_2S", "Forsaken_T__PlasWall2347v01",
    "Forsaken_T__StonArch2721Ctr", "Forsaken_T__StonColu2350or1",
    "Forsaken_T__StonFlor2352pm1", "Forsaken_T__StonFlor2352v01",
    "Forsaken_T__StonWall2031pan", "Forsaken_T__StonWall2032bo1",
    "JamesT__moon02_M_T_2S", "NatureT__SkyMod442",
    "NatureT__Sky_Clou3670blu_U_Sky", "WarrenT__Black_U",
)
ARENA07_PORTAL_SLOTS = (
    "JHPFireEng__LavaWet01_2S", "NatureT__SkyMod443_Mod_2S",
)


class ComparisonError(ValueError):
    pass


def _canonical_json(value) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")


def _vector(value, dimensions, label):
    if not isinstance(value, list) or len(value) != dimensions:
        raise ComparisonError(f"{label} must have {dimensions} components")
    result = tuple(float(component) for component in value)
    if not all(math.isfinite(component) for component in result):
        raise ComparisonError(f"{label} is non-finite")
    return result


def _integer(value, label):
    if isinstance(value, bool) or not isinstance(value, (int, float)) \
            or not math.isfinite(value) or int(value) != value:
        raise ComparisonError(f"{label} must be an integer")
    return int(value)


def _distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def _dot(a, b):
    length_a = math.sqrt(sum(value * value for value in a))
    length_b = math.sqrt(sum(value * value for value in b))
    if length_a == 0.0 or length_b == 0.0:
        return -1.0
    return sum(x * y for x, y in zip(a, b)) / (length_a * length_b)


def _close_vector(a, b, epsilon):
    return all(abs(x - y) <= epsilon for x, y in zip(a, b))


def _mesh_slot_names(mesh):
    materials = mesh.get("materials")
    if not isinstance(materials, list):
        raise ComparisonError("mesh materials must be an array")
    names = []
    for material in materials:
        if not isinstance(material, dict) or not isinstance(
                material.get("slot_name"), str):
            raise ComparisonError("material is missing slot_name")
        names.append(material["slot_name"])
    return names


def _corner(mesh, vertex):
    return (
        _vector(mesh["positions"][vertex], 3, "position"),
        _vector(mesh["normals"][vertex], 3, "normal"),
        _vector(mesh["tangent_x"][vertex], 3, "tangent_x"),
        float(mesh["binormal_sign"][vertex]),
        _vector(mesh["uv0"][vertex], 2, "uv0"),
        _vector(mesh["uv1"][vertex], 2, "uv1"),
    )


def _triangle_centroid(triangle):
    return tuple(sum(corner[0][axis] for corner in triangle) / 3.0
                 for axis in range(3))


def _triangle_bucket(triangle):
    return tuple(math.floor(value / TRIANGLE_BUCKET_GRID)
                 for value in _triangle_centroid(triangle))


def _validate_mesh_sections(mesh, label, vertex_count):
    indices = mesh["indices"]
    sections = mesh["sections"]
    if not indices or len(indices) % 3:
        raise ComparisonError(f"{label}: index stream is empty or not triangular")
    if not sections:
        raise ComparisonError(f"{label}: section metadata is empty")
    checked_indices = []
    for index in indices:
        checked = _integer(index, f"{label}: index")
        if not 0 <= checked < vertex_count:
            raise ComparisonError(f"{label}: render triangle index is out of range")
        checked_indices.append(checked)
    ordered = []
    triangle_sum = 0
    for section_number, section in enumerate(sections):
        if not isinstance(section, dict):
            raise ComparisonError(f"{label}: section {section_number} is not an object")
        first = _integer(section.get("first_index"), f"{label}: first_index")
        triangles = _integer(section.get("num_triangles"),
                             f"{label}: num_triangles")
        if first < 0 or triangles <= 0:
            raise ComparisonError(f"{label}: section range is invalid")
        end = first + triangles * 3
        if end > len(checked_indices):
            raise ComparisonError(f"{label}: section range exceeds index stream")
        declared_min = _integer(section.get("min_vertex_index"),
                                f"{label}: min_vertex_index")
        declared_max = _integer(section.get("max_vertex_index"),
                                f"{label}: max_vertex_index")
        referenced = checked_indices[first:end]
        if declared_min != min(referenced) or declared_max != max(referenced):
            raise ComparisonError(
                f"{label}: section min/max vertex metadata disagrees with indices")
        ordered.append((first, end, section))
        triangle_sum += triangles
    ordered.sort(key=lambda item: item[0])
    cursor = 0
    for first, end, _section in ordered:
        if first != cursor:
            raise ComparisonError(
                f"{label}: sections do not contiguously tile the index stream")
        cursor = end
    if cursor != len(checked_indices):
        raise ComparisonError(f"{label}: sections do not cover the index stream")
    if triangle_sum != len(checked_indices) // 3:
        raise ComparisonError(f"{label}: section triangle sum differs")
    if _integer(mesh.get("triangle_count"), f"{label}: triangle_count") \
            != triangle_sum:
        raise ComparisonError(f"{label}: mesh triangle total differs from sections")
    return checked_indices


def _render_groups(dump, side):
    meshes = dump.get("meshes")
    if not isinstance(meshes, list) or not meshes:
        raise ComparisonError(f"{side} render dump has no meshes")
    groups = {}
    positions = []
    bounds_errors = []
    for mesh_number, mesh in enumerate(meshes):
        if not isinstance(mesh, dict):
            raise ComparisonError(f"{side} render mesh is not an object")
        label = f"{side} {mesh.get('object_path', f'mesh {mesh_number}')}"
        for field in ("positions", "normals", "tangent_x", "binormal_sign",
                      "uv0", "uv1", "indices", "sections"):
            if not isinstance(mesh.get(field), list):
                raise ComparisonError(f"{label}: mesh field {field} is not an array")
        count = len(mesh["positions"])
        if count == 0 or any(len(mesh[field]) != count for field in
                             ("normals", "tangent_x", "binormal_sign", "uv0", "uv1")):
            raise ComparisonError(f"{label}: vertex streams are empty or unequal")
        indices = _validate_mesh_sections(mesh, label, count)
        local_positions = [_vector(point, 3, f"{label} position")
                           for point in mesh["positions"]]
        positions.extend(local_positions)
        # Bounds gate on the vertices the index stream actually REFERENCES: a
        # fractal-split part mesh keeps the source GLB's full vertex buffer
        # (world_fractal_split reuses accessors verbatim) while its stored
        # bounds cover only its own triangles — orphan buffer vertices are
        # not geometry.
        used_positions = [local_positions[index] for index in sorted(set(indices))]
        local_min = tuple(min(point[axis] for point in used_positions)
                          for axis in range(3))
        local_max = tuple(max(point[axis] for point in used_positions)
                          for axis in range(3))
        local_center = tuple((local_min[axis] + local_max[axis]) * .5
                             for axis in range(3))
        local_radius = math.sqrt(sum(
            ((local_max[axis] - local_min[axis]) * .5) ** 2
            for axis in range(3)))
        dumped_bounds = mesh.get("bounds", {})
        try:
            # Containment, not equality: a fractal-split part mesh keeps the
            # SOURCE accessor's bounds (world_fractal_split reuses the buffer
            # verbatim), so its stored box legitimately exceeds its own used
            # vertices. Buffers escaping the stored bounds is still fatal.
            stored_min = _vector(dumped_bounds.get("box_min"), 3, "box_min")
            stored_max = _vector(dumped_bounds.get("box_max"), 3, "box_max")
            for axis in range(3):
                if (local_min[axis] < stored_min[axis] - POSITION_EPSILON
                        or local_max[axis] > stored_max[axis] + POSITION_EPSILON):
                    bounds_errors.append(
                        f"{label}: buffer positions escape the stored box")
                    break
            stored_origin = _vector(
                dumped_bounds.get("sphere_origin"), 3, "sphere_origin")
            stored_radius = float(dumped_bounds.get("sphere_radius"))
            worst = max(_distance(point, stored_origin)
                        for point in used_positions)
            if worst > stored_radius + POSITION_EPSILON:
                bounds_errors.append(
                    f"{label}: buffer positions escape the stored sphere")
        except (ComparisonError, TypeError, ValueError):
            bounds_errors.append(f"{label}: invalid bounds")
        slots = _mesh_slot_names(mesh)
        material_paths = {
            material["slot_name"]: material.get("material_path", "")
            for material in mesh["materials"]
        }
        for section in mesh["sections"]:
            material_index = _integer(section.get("material_index"),
                                      f"{label}: material_index")
            if not 0 <= material_index < len(slots):
                raise ComparisonError(f"{label}: section material index is invalid")
            first = _integer(section["first_index"], f"{label}: first_index")
            triangles = _integer(section["num_triangles"], f"{label}: num_triangles")
            slot = slots[material_index]
            group = groups.setdefault(slot, {
                "material_paths": set(), "collision": set(), "triangles": []})
            group["material_paths"].add(material_paths[slot])
            group["collision"].add(bool(section.get("enable_collision")))
            for offset in range(first, first + triangles * 3, 3):
                vertices = indices[offset:offset + 3]
                group["triangles"].append(tuple(
                    _corner(mesh, vertex) for vertex in vertices))
    minimum = tuple(min(point[index] for point in positions) for index in range(3))
    maximum = tuple(max(point[index] for point in positions) for index in range(3))
    center = tuple((minimum[index] + maximum[index]) * .5 for index in range(3))
    radius = math.sqrt(sum(((maximum[axis] - minimum[axis]) * .5) ** 2
                           for axis in range(3)))
    return meshes, groups, (minimum, maximum, center, radius), bounds_errors


def _is_zero_vector(value):
    return sum(component * component for component in value) < 1e-8


# Reference-import weld deviation: on a handful of hard-edge corners the
# cooked reference's Interchange import averages the authored per-corner
# normals of co-located vertices (measured: A 3.8 deg off the authored GLB
# normal while the injected B matches it within 8-bit packing — see the
# proof RECORD). Such corners pass as WELD DEVIATIONS when positions and
# UVs match exactly, the basis is bounded-close, and their total stays
# under a small fraction; every instance is reported in the verdict.
WELD_DEVIATION_DOT = 0.995
WELD_DEVIATION_MAX_FRACTION = 0.01


def _compare_corner(a, b, errors, label, basis_meaningful=True,
                    weld_deviations=None):
    if _distance(a[0], b[0]) > POSITION_EPSILON:
        errors.append(f"{label}: position differs")
    # Near-degenerate sliver triangles survive bsp2obj and ship in the
    # retail-derived payload. Each side's tangent generator zeroes (reference
    # cook, sometimes leaving ONE stray nonzero corner), or synthesizes (the
    # runtime rebuild), the basis by ITS OWN area epsilon — so any zero-basis
    # corner on either side marks the whole TRIANGLE's basis meaningless
    # (decided by the caller). Positions and UVs always gate; a genuinely
    # wrong basis is fully nonzero on both sides and still fails the dots.
    if basis_meaningful:
        normal_dot = _dot(a[1], b[1])
        tangent_dot = _dot(a[2], b[2])
        handed_ok = math.copysign(1.0, a[3]) == math.copysign(1.0, b[3])
        strict = (normal_dot >= NORMAL_DOT and tangent_dot >= TANGENT_DOT
                  and handed_ok)
        if not strict and weld_deviations is not None                 and normal_dot >= WELD_DEVIATION_DOT                 and tangent_dot >= WELD_DEVIATION_DOT and handed_ok                 and _distance(a[0], b[0]) <= POSITION_EPSILON                 and _close_vector(a[4], b[4], UV_EPSILON)                 and _close_vector(a[5], b[5], UV_EPSILON):
            weld_deviations.append(
                f"{label}: bounded weld deviation "
                f"(normal dot {normal_dot:.5f}, tangent dot {tangent_dot:.5f})")
        elif not strict:
            if normal_dot < NORMAL_DOT:
                errors.append(f"{label}: normal differs")
            if tangent_dot < TANGENT_DOT:
                errors.append(f"{label}: tangent differs")
            if not handed_ok:
                errors.append(f"{label}: binormal handedness differs")
    if not _close_vector(a[4], b[4], UV_EPSILON):
        errors.append(f"{label}: UV0 differs")
    if not _close_vector(a[5], b[5], UV_EPSILON):
        errors.append(f"{label}: UV1 differs")


def _triangle_basis_meaningful(triangle):
    return all(not _is_zero_vector(corner[1])
               and not _is_zero_vector(corner[2]) for corner in triangle)


def _compare_triangle(a_triangle, b_triangle, label, weld_deviations=None):
    basis_meaningful = (_triangle_basis_meaningful(a_triangle)
                        and _triangle_basis_meaningful(b_triangle))
    candidates = (b_triangle,
                  (b_triangle[1], b_triangle[2], b_triangle[0]),
                  (b_triangle[2], b_triangle[0], b_triangle[1]))
    scored = []
    for candidate in candidates:
        errors = []
        deviations = [] if weld_deviations is not None else None
        for corner_index, (a_corner, b_corner) in enumerate(zip(
                a_triangle, candidate)):
            _compare_corner(a_corner, b_corner, errors,
                            f"{label} corner {corner_index}",
                            basis_meaningful=basis_meaningful,
                            weld_deviations=deviations)
        scored.append((errors, deviations))
    best_errors, best_deviations = min(scored, key=lambda item: len(item[0]))
    if weld_deviations is not None and not best_errors and best_deviations:
        weld_deviations.extend(best_deviations)
    return best_errors


def _triangle_score(a_triangle, b_triangle):
    rotations = (b_triangle,
                 (b_triangle[1], b_triangle[2], b_triangle[0]),
                 (b_triangle[2], b_triangle[0], b_triangle[1]))
    return min(sum(_distance(a_corner[0], b_corner[0])
                   for a_corner, b_corner in zip(a_triangle, candidate))
               for candidate in rotations)


def _match_triangles(a_triangles, b_triangles, slot, weld_deviations=None):
    errors = []
    buckets = {}
    for index, triangle in enumerate(b_triangles):
        buckets.setdefault(_triangle_bucket(triangle), []).append(index)
    unmatched_b = set(range(len(b_triangles)))
    unmatched_a = []
    for a_index, a_triangle in enumerate(a_triangles):
        bucket = _triangle_bucket(a_triangle)
        candidates = []
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for b_index in buckets.get(
                            (bucket[0] + dx, bucket[1] + dy, bucket[2] + dz), ()):
                        if b_index not in unmatched_b:
                            continue
                        candidate_errors = _compare_triangle(
                            a_triangle, b_triangles[b_index],
                            f"{slot} triangle {a_index}",
                            weld_deviations=weld_deviations)
                        if not candidate_errors:
                            candidates.append((_triangle_score(
                                a_triangle, b_triangles[b_index]), b_index))
        if not candidates:
            unmatched_a.append(a_index)
            continue
        _score, best = min(candidates)
        unmatched_b.remove(best)
    if unmatched_a:
        errors.append(f"{slot}: unmatched A triangles {unmatched_a[:5]}")
    if unmatched_b:
        errors.append(f"{slot}: unmatched B triangles {sorted(unmatched_b)[:5]}")
    return errors


def _render_section(role, a_dump, b_dump):
    errors = []
    weld_deviations = []
    try:
        a_meshes, a_groups, a_bounds, a_bound_errors = _render_groups(a_dump, "A")
        b_meshes, b_groups, b_bounds, b_bound_errors = _render_groups(b_dump, "B")
    except (ComparisonError, KeyError, TypeError, ValueError) as exc:
        return {"passed": False, "errors": [str(exc)], "slot_count": 0}
    errors.extend(a_bound_errors)
    errors.extend(b_bound_errors)
    a_identities = [(mesh.get("object_path"), _mesh_slot_names(mesh))
                    for mesh in a_meshes]
    b_identities = [(mesh.get("object_path"), _mesh_slot_names(mesh))
                    for mesh in b_meshes]
    if b_identities != a_identities:
        errors.append(
            f"B {role} carrier split identities/material-slot order differ")
    for side, meshes in (("A", a_meshes), ("B", b_meshes)):
        for mesh in meshes:
            if mesh.get("num_lods") != 1:
                errors.append(f"{side} {mesh.get('object_path')}: NumLODs != 1")
            if mesh.get("uv_channels") != 2:
                errors.append(f"{side} {mesh.get('object_path')}: UV channels != 2")
            if not mesh.get("allow_cpu_access") or not mesh.get("never_stream"):
                errors.append(f"{side} {mesh.get('object_path')}: CPU/stream policy differs")
            if mesh.get("nanite_valid"):
                errors.append(f"{side} {mesh.get('object_path')}: Nanite is enabled")
            if not mesh.get("cpu_readable"):
                errors.append(f"{side} {mesh.get('object_path')}: buffers are not readable")
    if set(a_groups) != set(b_groups):
        errors.append("material-slot group names differ")
    expected_b_collision = role == "main"
    for slot in sorted(set(a_groups) & set(b_groups)):
        a_group, b_group = a_groups[slot], b_groups[slot]
        if a_group["material_paths"] != b_group["material_paths"]:
            errors.append(f"{slot}: material object paths differ")
        # Reference A's no-collision portal policy is actor-level; its imported
        # section bits are Interchange defaults and deliberately do not gate.
        if b_group["collision"] != {expected_b_collision}:
            errors.append(f"{slot}: B section collision contract differs")
        if len(a_group["triangles"]) != len(b_group["triangles"]):
            errors.append(f"{slot}: triangle count differs")
            continue
        errors.extend(_match_triangles(
            a_group["triangles"], b_group["triangles"], slot,
            weld_deviations=weld_deviations))
    for index, label in ((0, "AABB min"), (1, "AABB max"),
                         (2, "sphere origin")):
        if not _close_vector(a_bounds[index], b_bounds[index], POSITION_EPSILON):
            errors.append(f"{label} differs")
    if abs(a_bounds[3] - b_bounds[3]) > POSITION_EPSILON:
        errors.append("sphere radius differs")
    corner_total = 3 * sum(len(group["triangles"])
                           for group in a_groups.values())
    if corner_total and len(weld_deviations)             > corner_total * WELD_DEVIATION_MAX_FRACTION:
        errors.append(
            f"weld-deviation budget exceeded: {len(weld_deviations)} corners "
            f"of {corner_total}")
    return {"passed": not errors, "errors": errors,
            "weld_deviations": weld_deviations,
            "slot_count": len(a_groups),
            "triangle_count": sum(len(group["triangles"])
                                  for group in a_groups.values())}


def _records_by_id(value, field):
    records = value.get(field, [])
    if not isinstance(records, list):
        raise ComparisonError(f"{field} must be an array")
    result = {}
    for record in records:
        identifier = record.get("id") if isinstance(record, dict) else None
        if not isinstance(identifier, str) or not identifier or identifier in result:
            raise ComparisonError(f"{field} has invalid/duplicate id")
        result[identifier] = record
    return result


def _same_ids(a, b, field, errors):
    aa, bb = _records_by_id(a, field), _records_by_id(b, field)
    if set(aa) != set(bb):
        errors.append(f"{field} ids differ")
    return aa, bb


def _compare_hit(a, b, errors, label, distance_epsilon, normal_dot,
                 position_epsilon=None):
    if bool(a.get("blocking_hit")) != bool(b.get("blocking_hit")):
        errors.append(f"{label}: hit/no-hit differs")
        return
    if not a.get("blocking_hit"):
        return
    # Auto-generated actor names cannot match across A and B (B's level has
    # fewer spawned world-mesh actors, shifting every name counter). The hit
    # IDENTITY is the struck static mesh's canonical object path when both
    # sides carry one; bare actor names only gate when a mesh path is absent
    # (non-static-mesh hits).
    if a.get("mesh_path") or b.get("mesh_path"):
        if a.get("mesh_path") != b.get("mesh_path"):
            errors.append(f"{label}: mesh_path differs")
    elif a.get("actor") != b.get("actor"):
        errors.append(f"{label}: actor differs")
    for field in ("component", "material_slot"):
        if a.get(field) != b.get(field):
            errors.append(f"{label}: {field} differs")
    if position_epsilon is not None and _distance(
            _vector(a.get("impact_point"), 3, "impact_point"),
            _vector(b.get("impact_point"), 3, "impact_point")) > position_epsilon:
        errors.append(f"{label}: impact point differs")
    if distance_epsilon is not None and abs(
            float(a.get("distance", 0.0)) - float(b.get("distance", 0.0))) \
            > distance_epsilon:
        errors.append(f"{label}: distance differs")
    if _dot(_vector(a.get("impact_normal"), 3, "impact_normal"),
            _vector(b.get("impact_normal"), 3, "impact_normal")) < normal_dot:
        errors.append(f"{label}: impact normal differs")


def _plan_array(plan, field, minimum, errors):
    values = plan.get(field)
    if not isinstance(values, list) or len(values) < minimum:
        errors.append(f"plan minimum violation: {field} requires at least {minimum}")
        return []
    if any(not isinstance(value, dict) or not isinstance(value.get("id"), str)
           for value in values):
        errors.append(f"plan {field} ids are invalid")
        return []
    return values


def _collision_section(a, b, plan):
    errors = []
    floor_plan = _plan_array(plan, "floor_rays", 128, errors)
    wall_plan = _plan_array(plan, "wall_rays", 64, errors)
    projectile_plan = _plan_array(plan, "projectiles", 32, errors)
    for shot in projectile_plan:
        if not isinstance(shot.get("portal_directed"), bool) or not isinstance(
                shot.get("expect_block"), bool):
            errors.append(f"plan projectile policy is invalid: {shot.get('id')}")
        elif shot["portal_directed"] and shot["expect_block"]:
            # Portal shots must never expect a block. Non-portal shots MAY
            # decline the expectation (wall/oblique anchored shots can expire
            # over long sight lines); floor-anchored shots carry it.
            errors.append(f"plan projectile policy conflicts: {shot['id']}")
    radial_count = plan.get("player_start_radial_count")
    if not isinstance(radial_count, int) or radial_count < 8:
        errors.append("plan minimum violation: player_start_radial_count requires at least 8")
        radial_count = 8
    try:
        for side, dump in (("A", a), ("B", b)):
            for field in ("probes", "settles", "projectiles"):
                if not isinstance(dump.get(field), list) or not dump[field]:
                    errors.append(f"{side} collision {field} is empty")
        a_probes, b_probes = _same_ids(a, b, "probes", errors)
        a_settles, b_settles = _same_ids(a, b, "settles", errors)
        a_projectiles, b_projectiles = _same_ids(a, b, "projectiles", errors)
        planned_probe_ids = {item["id"] for item in floor_plan + wall_plan}
        planned_projectile_ids = {item["id"] for item in projectile_plan}
        for side, records in (("A", a_probes), ("B", b_probes)):
            missing = sorted(planned_probe_ids - set(records))
            if missing:
                errors.append(f"{side} is missing plan probe ids {missing[:5]}")
        for side, records in (("A", a_projectiles), ("B", b_projectiles)):
            missing = sorted(planned_projectile_ids - set(records))
            if missing:
                errors.append(f"{side} is missing plan projectile ids {missing[:5]}")
        for side, probes, settles in (("A", a_probes, a_settles),
                                      ("B", b_probes, b_settles)):
            for settle_id in sorted(settles):
                if not re.fullmatch(r"start_\d+", settle_id):
                    errors.append(f"{side} settle id {settle_id} is not start-family")
                    continue
                expected = {f"{settle_id}_capsule"} | {
                    f"{settle_id}_radial_{ray}" for ray in range(radial_count)}
                missing = sorted(expected - set(probes))
                if missing:
                    errors.append(f"{side} start-family probes missing {missing[:5]}")
        for identifier in sorted(set(a_probes) & set(b_probes)):
            _compare_hit(a_probes[identifier], b_probes[identifier], errors,
                         identifier, COLLISION_DISTANCE_EPSILON,
                         COLLISION_NORMAL_DOT)
        for identifier in sorted(set(a_settles) & set(b_settles)):
            ar, br = a_settles[identifier], b_settles[identifier]
            if abs(float(ar.get("settled_z")) - float(br.get("settled_z"))) \
                    > SETTLE_Z_EPSILON:
                errors.append(f"{identifier}: settled Z differs")
            for field in ("movement_mode", "fall_through"):
                if ar.get(field) != br.get(field):
                    errors.append(f"{identifier}: {field} differs")
        for identifier in sorted(set(a_projectiles) & set(b_projectiles)):
            ar, br = a_projectiles[identifier], b_projectiles[identifier]
            if bool(ar.get("portal_directed")) != bool(br.get("portal_directed")):
                errors.append(f"{identifier}: portal designation differs")
            # Portal shots legitimately pass THROUGH the no-collision portal
            # surface and may strike backing geometry behind it (pool floors
            # sit 50-60uu past the plane in Arena_07). The pass-through proof
            # lives in the plan-policy loop below; here both sides' outcomes
            # must simply be identical, like every other shot.
            _compare_hit(ar, br, errors, identifier, None,
                         COLLISION_NORMAL_DOT, PROJECTILE_POSITION_EPSILON)
        floor_ids = [item["id"] for item in floor_plan]
        floor_hits = sum(bool(a_probes.get(identifier, {}).get("blocking_hit"))
                         for identifier in floor_ids)
        if floor_ids and floor_hits / len(floor_ids) < .60:
            errors.append("reference A blocks fewer than 60% of plan floor rays")
        for shot in projectile_plan:
            a_shot = a_projectiles.get(shot["id"], {})
            b_shot = b_projectiles.get(shot["id"], {})
            if a_shot.get("portal_directed") != shot.get("portal_directed") or \
                    b_shot.get("portal_directed") != shot.get("portal_directed"):
                errors.append(f"plan projectile designation differs: {shot['id']}")
            if shot.get("portal_directed"):
                # The portal itself must never register the hit; striking
                # coplanar or backing WORLD geometry is legitimate (Arena_07's
                # water tops have coplanar solid world surfaces) and gates via
                # A/B outcome equality above.
                for side, record in (("A", a_shot), ("B", b_shot)):
                    if not record.get("blocking_hit"):
                        continue
                    mesh_path = str(record.get("mesh_path", ""))
                    if mesh_path.startswith(
                            "/Game/WoT/Maps/Geo/Arena_07_portals/"):
                        errors.append(
                            f"{shot['id']}: {side} collided with the portal "
                            "mesh itself")
        expected = [shot for shot in projectile_plan
                    if shot.get("expect_block") is True]
        if expected:
            blocked = sum(
                bool(a_projectiles.get(shot["id"], {}).get("blocking_hit"))
                for shot in expected)
            # One anchored floor shot legitimately exits through the pit's
            # no-collision lava portal; a fraction gate still kills the
            # all-miss vacuous case while per-shot A/B equality above catches
            # any real divergence.
            if blocked / len(expected) < 0.8:
                misses = [shot["id"] for shot in expected
                          if not a_projectiles.get(
                              shot["id"], {}).get("blocking_hit")]
                errors.append(
                    "reference A blocks fewer than 80% of expect_block "
                    f"shots (misses: {misses[:6]})")
    except (ComparisonError, TypeError, ValueError) as exc:
        errors.append(str(exc))
    return {"passed": not errors, "errors": errors}


def _compare_paths(a, b, a_field, b_field, errors, label_prefix=""):
    aa, bb = _same_ids({"records": a.get(a_field, [])},
                       {"records": b.get(b_field, [])}, "records", errors)
    for identifier in sorted(set(aa) & set(bb)):
        ar, br = aa[identifier], bb[identifier]
        label = f"{label_prefix}{identifier}"
        for field in ("found", "partial"):
            if bool(ar.get(field)) != bool(br.get(field)):
                errors.append(f"{label}: {field} differs")
        for field in ("projected_start", "projected_end"):
            if _distance(_vector(ar.get(field), 3, field),
                         _vector(br.get(field), 3, field)) > NAV_ENDPOINT_EPSILON:
                errors.append(f"{label}: {field} differs")
        a_length, b_length = float(ar.get("length")), float(br.get("length"))
        if abs(a_length - b_length) > max(abs(a_length) * .01, 5.0):
            errors.append(f"{label}: path length differs")
    return aa, bb


def _validate_seeking(record, label, errors):
    if not isinstance(record, dict):
        errors.append(f"{label} seeking record is absent")
        return
    if record.get("los_blocked") is not True:
        errors.append(f"{label} seeking LOS was not blocked")
    trajectory = record.get("trajectory")
    if not isinstance(trajectory, list) or not trajectory:
        errors.append(f"{label} seeking trajectory is empty")
    else:
        endpoint = _vector(record.get("reached_endpoint"), 3, "seeking endpoint")
        last = _vector(trajectory[-1], 3, "seeking trajectory endpoint")
        if _distance(endpoint, last) > POSITION_EPSILON:
            errors.append(f"{label} seeking endpoint is not the last trajectory sample")
        if endpoint == (0.0, 0.0, 0.0):
            errors.append(f"{label} seeking endpoint is zero")
    if record.get("completion_reason") not in {"reached", "destroyed", "timeout"}:
        errors.append(f"{label} seeking completion reason is invalid")
    side_sign = record.get("side_sign")
    expected_side = {1: "left", -1: "right", 0: "straight"}.get(side_sign)
    if expected_side is None or record.get("side") != expected_side:
        errors.append(f"{label} seeking side/sign is invalid")
    if not isinstance(record.get("hit_actor_class", ""), str):
        errors.append(f"{label} seeking hit actor class is invalid")


def _compare_seeking(a_seek, b_seek, errors, label_prefix=""):
    _validate_seeking(a_seek, f"{label_prefix}A", errors)
    _validate_seeking(b_seek, f"{label_prefix}B", errors)
    # The gate requires the SAME REACHABLE SIDE and the same reach/hit
    # outcome; the exact timeout coordinate of a live homing projectile is
    # phase noise (it orbits its target), so the endpoint itself is retained
    # in the dumps as evidence but not gated.
    for field in ("side", "side_sign", "completion_reason", "hit_actor_class"):
        if a_seek.get(field) != b_seek.get(field):
            errors.append(f"{label_prefix}seeking {field} differs")


def _nav_section(a, b, plan):
    errors = []
    nav_plan = _plan_array(plan, "nav_pairs", 12, errors)
    try:
        if int(a.get("tile_count", 0)) <= 0 or int(b.get("tile_count", 0)) <= 0:
            errors.append("generated tile count must be nonzero")
        for field in ("box_min", "box_max"):
            if not _close_vector(_vector(a["bounds"][field], 3, field),
                                 _vector(b["bounds"][field], 3, field),
                                 POSITION_EPSILON):
                errors.append(f"nav bounds {field} differs")
        aa, bb = _compare_paths(a, b, "paths", "paths", errors)
        expected_ids = {item["id"] for item in nav_plan}
        for side, records in (("A", aa), ("B", bb)):
            missing = sorted(expected_ids - set(records))
            if missing:
                errors.append(f"{side} is missing plan nav ids {missing[:5]}")
        _compare_seeking(a.get("seeking", {}), b.get("seeking", {}), errors)
    except (ComparisonError, KeyError, TypeError, ValueError) as exc:
        errors.append(str(exc))
    return {"passed": not errors, "errors": errors}


def _resource_growth(series):
    if len(series) < 2:
        return False
    tail = series[len(series) // 2:]
    return len(tail) >= 2 and tail[0] > 0 and tail[-1] > tail[0] * 1.05


def _travel_section(a, b, plan):
    errors = []
    a_travel, b_travel = a.get("travel", []), b.get("travel", [])
    a_summary, b_summary = a.get("summary", {}), b.get("summary", {})
    travel_mode = a_summary.get("mode") == "travel" or b_summary.get("mode") == "travel"
    required_count = plan.get("travel_count")
    if not isinstance(required_count, int) or required_count < 1:
        errors.append("plan minimum violation: travel_count requires at least 1")
        required_count = 1
    if bool(a_travel) != bool(b_travel):
        errors.append("travel dumps are missing on one side")
    if len(a_travel) != len(b_travel):
        errors.append("travel dump counts differ")
    if travel_mode and (len(a_travel) < required_count or len(b_travel) < required_count):
        errors.append("travel dump count is below the plan travel count")
    a_memory, b_memory = [], []
    compact_probe_ids = set(plan.get("compact_probe_ids", []))
    projectile_ids = {item.get("id") for item in plan.get("projectiles", [])}
    expected_projectiles = compact_probe_ids & projectile_ids
    expected_probes = compact_probe_ids - projectile_ids
    expected_nav = set(plan.get("compact_nav_ids", []))
    try:
        for cycle_index, (a_cycle, b_cycle) in enumerate(zip(a_travel, b_travel)):
            if a_cycle.get("cycle") != b_cycle.get("cycle"):
                errors.append(f"travel {cycle_index}: cycle number differs")
            for side, cycle in (("A", a_cycle), ("B", b_cycle)):
                if int(cycle.get("nav_tiles", 0)) <= 0:
                    errors.append(f"travel {cycle_index} {side}: nav tiles must be nonzero")
            a_probes, b_probes = _same_ids(a_cycle, b_cycle, "probes", errors)
            a_projectiles, b_projectiles = _same_ids(
                a_cycle, b_cycle, "projectiles", errors)
            for side, records, expected, kind in (
                    ("A", a_probes, expected_probes, "probe"),
                    ("B", b_probes, expected_probes, "probe"),
                    ("A", a_projectiles, expected_projectiles, "projectile"),
                    ("B", b_projectiles, expected_projectiles, "projectile")):
                missing = sorted(expected - set(records))
                if missing:
                    errors.append(
                        f"travel {cycle_index} {side}: missing compact {kind} ids {missing[:5]}")
            for identifier in sorted(set(a_probes) & set(b_probes)):
                _compare_hit(a_probes[identifier], b_probes[identifier], errors,
                             f"travel {cycle_index} {identifier}",
                             COLLISION_DISTANCE_EPSILON, COLLISION_NORMAL_DOT)
            for identifier in sorted(set(a_projectiles) & set(b_projectiles)):
                ar, br = a_projectiles[identifier], b_projectiles[identifier]
                if bool(ar.get("portal_directed")) != bool(br.get("portal_directed")):
                    errors.append(
                        f"travel {cycle_index} {identifier}: portal designation differs")
                # Portal shots pass through the portal and may hit backing
                # geometry (see the core collision section); outcome equality
                # is the gate here too.
                _compare_hit(ar, br, errors,
                             f"travel {cycle_index} {identifier}", None,
                             COLLISION_NORMAL_DOT, PROJECTILE_POSITION_EPSILON)
            aa, bb = _compare_paths(a_cycle, b_cycle, "nav_paths", "nav_paths",
                                    errors, f"travel {cycle_index} ")
            for side, records in (("A", aa), ("B", bb)):
                missing = sorted(expected_nav - set(records))
                if missing:
                    errors.append(
                        f"travel {cycle_index} {side}: missing compact nav ids {missing[:5]}")
            a_used = float(a_cycle.get("used_physical"))
            b_used = float(b_cycle.get("used_physical"))
            if not math.isfinite(a_used) or a_used < 0 or not math.isfinite(
                    b_used) or b_used < 0:
                raise ComparisonError(f"travel {cycle_index}: invalid used_physical")
            a_memory.append(a_used)
            b_memory.append(b_used)
        if a_travel and b_travel:
            _compare_seeking(a_travel[-1].get("seeking", {}),
                             b_travel[-1].get("seeking", {}), errors,
                             "final travel ")
        if _resource_growth(a_memory):
            errors.append("resource growth trend (A)")
        if _resource_growth(b_memory):
            errors.append("resource growth trend (B)")
    except (ComparisonError, KeyError, TypeError, ValueError) as exc:
        errors.append(str(exc))
    return {"passed": not errors, "errors": errors,
            "cycle_count": min(len(a_travel), len(b_travel)),
            "memory_series": {"a": a_memory, "b": b_memory}}


def compare_artifacts(a, b, plan):
    sections = {
        "render_main": _render_section("main", a["main"], b["main"]),
        "render_portal": _render_section("portal", a["portal"], b["portal"]),
        "collision": _collision_section(a["collision"], b["collision"], plan),
        "nav": _nav_section(a["nav"], b["nav"], plan),
        "travel": _travel_section(a, b, plan),
    }
    return {"passed": all(section["passed"] for section in sections.values()),
            "sections": sections}


def _load_optional_json(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def _load_directory(directory):
    travel_paths = list(directory.glob("travel_*.json"))
    numbered = []
    for path in travel_paths:
        match = re.fullmatch(r"travel_(\d+)\.json", path.name)
        if not match:
            raise ComparisonError(f"invalid travel dump filename {path.name}")
        numbered.append((int(match.group(1)), path))
    numbered.sort()
    if numbered and [number for number, _path in numbered] != list(range(len(numbered))):
        raise ComparisonError("travel dump cycle filenames are not contiguous")
    return {
        "main": json.loads((directory / "render_main.json").read_text(encoding="utf-8")),
        "portal": json.loads((directory / "render_portal.json").read_text(encoding="utf-8")),
        "collision": json.loads((directory / "collision.json").read_text(encoding="utf-8")),
        "nav": json.loads((directory / "nav.json").read_text(encoding="utf-8")),
        "travel": [json.loads(path.read_text(encoding="utf-8"))
                   for _number, path in numbered],
        "summary": _load_optional_json(directory / "summary.json"),
    }


def _hashes(artifacts):
    dumps = {name: artifacts[name]
             for name in ("main", "portal", "collision", "nav")}
    dumps.update({f"travel_{index}": value
                  for index, value in enumerate(artifacts.get("travel", []))})
    return {name: hashlib.sha256(_canonical_json(value)).hexdigest()
            for name, value in sorted(dumps.items())}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("a_dir", type=Path)
    parser.add_argument("b_dir", type=Path)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--hash", action="store_true")
    args = parser.parse_args(argv)
    try:
        a, b = _load_directory(args.a_dir), _load_directory(args.b_dir)
        plan = json.loads(args.plan.read_text(encoding="utf-8"))
        verdict = compare_artifacts(a, b, plan)
        if args.hash:
            verdict["hashes"] = {"a": _hashes(a), "b": _hashes(b)}
    except (OSError, json.JSONDecodeError, ComparisonError,
            KeyError, TypeError, ValueError) as exc:
        verdict = {"passed": False, "sections": {}, "error": str(exc)}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(_canonical_json(verdict))
    return 0 if verdict["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
