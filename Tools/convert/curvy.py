"""Deterministic UE1-style curved-surface subdivision.

The topology plan is frame-independent.  Position indices are welded across
UV seams, while each child triangle retains its own interpolated corner UVs.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import exports_dir, original_dir, require_path  # noqa: E402

from ue3d import MTT_FLAT, MTT_PLACEHOLDER


ROOT = Path(__file__).resolve().parents[2]
HASH_SCHEMA = "wot-curvy-frame-hashes-v1"
HASH_MESHES = (
    "AMApearl",
    "AngrealShieldPickup",
)


def reference_inputs(retail_root=None):
    """Return required retail package and exported-script inputs."""
    source_root = (require_path(retail_root, "original reference directory",
                                directory=True)
                   if retail_root is not None else original_dir())
    return source_root, exports_dir() / "script"


@dataclass(frozen=True)
class CurvyPlan:
    levels: int
    base_vertex_count: int
    triangles: tuple
    parents: tuple

    @property
    def vertex_count(self):
        return self.base_vertex_count + len(self.parents)


def _triangle_copy(triangle, indices, uvs):
    child = dict(triangle)
    child["idx"] = tuple(indices)
    child["uvs"] = tuple(tuple(uv) for uv in uvs)
    return child


def _is_fixed_triangle(triangle):
    triangle_type = int(triangle.get("type", 0))
    return ((triangle_type & 0x0f) == MTT_PLACEHOLDER
            or (triangle_type & MTT_FLAT) != 0)


def plan_curvy(tris, levels):
    """Return the stable topology/UV plan for ``levels`` subdivisions.

    Each parent pair addresses the extended position array: original vertices
    first, followed by midpoint vertices in first-seen edge order.  Midpoints
    are welded by position index, independently of per-corner UVs.
    """
    if not isinstance(levels, int) or levels < 0:
        raise ValueError("curvy subdivision levels must be a non-negative integer")
    triangles = [
        _triangle_copy(triangle, triangle["idx"], triangle["uvs"])
        for triangle in tris
    ]
    referenced = [index for triangle in triangles for index in triangle["idx"]]
    base_vertex_count = max(referenced, default=-1) + 1
    parents = []
    next_vertex = base_vertex_count

    for _level in range(levels):
        edge_vertices = {}
        children = []
        for triangle in triangles:
            if _is_fixed_triangle(triangle):
                children.append(triangle)
                continue
            indices = triangle["idx"]
            uvs = triangle["uvs"]
            midpoint_indices = []
            midpoint_uvs = []
            for first, second in ((0, 1), (1, 2), (2, 0)):
                edge = tuple(sorted((indices[first], indices[second])))
                midpoint = edge_vertices.get(edge)
                if midpoint is None:
                    midpoint = next_vertex
                    next_vertex += 1
                    edge_vertices[edge] = midpoint
                    parents.append(edge)
                midpoint_indices.append(midpoint)
                midpoint_uvs.append(tuple(
                    (float(uvs[first][axis]) + float(uvs[second][axis])) * 0.5
                    for axis in range(2)))
            a, b, c = indices
            uv_a, uv_b, uv_c = uvs
            m01, m12, m20 = midpoint_indices
            uv01, uv12, uv20 = midpoint_uvs
            children.extend((
                _triangle_copy(triangle, (a, m01, m20),
                               (uv_a, uv01, uv20)),
                _triangle_copy(triangle, (m01, b, m12),
                               (uv01, uv_b, uv12)),
                _triangle_copy(triangle, (m20, m12, c),
                               (uv20, uv12, uv_c)),
                _triangle_copy(triangle, (m01, m12, m20),
                               (uv01, uv12, uv20)),
            ))
        triangles = children

    return CurvyPlan(
        levels=levels,
        base_vertex_count=base_vertex_count,
        triangles=tuple(triangles),
        parents=tuple(parents),
    )


def _normalized(vector):
    length = math.sqrt(sum(component * component for component in vector))
    if length == 0.0:
        return (0.0, 0.0, 0.0)
    reciprocal = 1.0 / length
    return tuple(component * reciprocal for component in vector)


def smooth_normals(positions, triangles):
    """Build curvy base normals with the runtime's double operation order."""
    accumulated = [[0.0, 0.0, 0.0] for _ in positions]
    for triangle in triangles:
        a, b, c = (positions[index] for index in triangle)
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        fx = uy * vz - uz * vy
        fy = uz * vx - ux * vz
        fz = ux * vy - uy * vx
        reciprocal = 1.0 / math.sqrt(
            fx * fx + fy * fy + fz * fz + 0.001)
        fx *= reciprocal
        fy *= reciprocal
        fz *= reciprocal
        for index in triangle:
            accumulated[index][0] += fx
            accumulated[index][1] += fy
            accumulated[index][2] += fz
    return [_normalized(normal) for normal in accumulated]


def _float32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def float32_vectors(vectors):
    """Round a vector sequence to the runtime's float32 source-pose boundary."""
    return [tuple(_float32(value) for value in vector) for vector in vectors]


def eval_curvy(plan, positions, normals):
    """Evaluate one planned frame in float64, then round it to float32."""
    if len(positions) != plan.base_vertex_count:
        raise ValueError("curvy position count disagrees with the topology plan")
    if len(normals) != plan.base_vertex_count:
        raise ValueError("curvy normal count disagrees with the topology plan")
    extended_positions = [tuple(float(value) for value in point)
                          for point in positions]
    extended_normals = [tuple(float(value) for value in normal)
                        for normal in normals]
    for first, second in plan.parents:
        pa, pb = extended_positions[first], extended_positions[second]
        na, nb = extended_normals[first], extended_normals[second]
        midpoint_normal = _normalized(tuple(na[i] + nb[i] for i in range(3)))
        edge = tuple(pa[i] - pb[i] for i in range(3))
        normal_cross = (
            na[1] * nb[2] - na[2] * nb[1],
            na[2] * nb[0] - na[0] * nb[2],
            na[0] * nb[1] - na[1] * nb[0],
        )
        push = (0.15
                * math.sqrt(sum(component * component for component in edge))
                * math.sqrt(sum(component * component
                                for component in normal_cross)))
        extended_positions.append(tuple(
            (pa[i] + pb[i]) * 0.5 + midpoint_normal[i] * push
            for i in range(3)))
        extended_normals.append(midpoint_normal)
    rounded_positions = float32_vectors(extended_positions)
    rounded_normals = float32_vectors(extended_normals)
    return rounded_positions, rounded_normals


def frame_sha256(positions, normals):
    """Hash extended float32 positions then normals in position-index order."""
    digest = hashlib.sha256()
    for vectors in (positions, normals):
        for vector in vectors:
            digest.update(struct.pack("<3f", *vector))
    return digest.hexdigest()


def build_hash_fixture(retail_root=None):
    """Decode retail curvy meshes and hash every evaluated float32 frame."""
    from mesh2glb import (find_pair, reconstruct_retail_james,
                          transform_frames_float32)
    from ue3d import load_anim, load_data, parse_exec_metadata

    source_root, script_root = reference_inputs(retail_root)
    metadata = parse_exec_metadata(
        script_root, strip_line_comments=True)
    intermediate = ROOT / "Intermediate"
    intermediate.mkdir(parents=True, exist_ok=True)
    hashes = {}
    with tempfile.TemporaryDirectory(
            prefix="CurvyHashes-", dir=str(intermediate)) as temporary:
        models_dir = Path(temporary)
        reconstruct_retail_james(
            metadata, models_dir, source_root, names=HASH_MESHES)
        for mesh_name in HASH_MESHES:
            mesh_metadata = metadata[mesh_name.casefold()]
            data_file, animation_file = find_pair(
                mesh_metadata, [models_dir], canonical_names=True)
            data = load_data(data_file)
            raw_frames = load_anim(animation_file, data["numverts"])
            frames = [
                float32_vectors(frame) for frame in
                transform_frames_float32(raw_frames, mesh_metadata)
            ]
            all_triangles = [triangle["idx"] for triangle in data["tris"]]
            render_triangles = [
                triangle for triangle in data["tris"]
                if (triangle["type"] & 0x0f) != MTT_PLACEHOLDER
            ]
            plan = plan_curvy(render_triangles, 2)
            frame_hashes = []
            for frame in frames:
                positions, normals = eval_curvy(
                    plan, frame, smooth_normals(frame, all_triangles))
                frame_hashes.append(frame_sha256(positions, normals))
            hashes[mesh_name] = frame_hashes
    return {
        "generated_by": "Tools/convert/curvy.py:frame_sha256",
        "meshes": hashes,
        "schema": HASH_SCHEMA,
    }


def write_hashes(path, retail_root=None):
    """Write the canonical, newline-free retail curvy hash fixture."""
    payload = json.dumps(
        build_hash_fixture(retail_root=retail_root),
        sort_keys=True, separators=(",", ":"), ensure_ascii=True,
    ).encode("ascii")
    Path(path).write_bytes(payload)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate deterministic retail curvy frame hashes")
    parser.add_argument("--write-hashes", type=Path, required=True,
                        help="canonical JSON fixture output path")
    arguments = parser.parse_args(argv)
    write_hashes(arguments.write_hashes)


if __name__ == "__main__":
    main()
