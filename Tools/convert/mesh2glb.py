"""Vertex-animated mesh → skinned GLB with one morph target per frame.

Usage: python3 mesh2glb.py <MeshName> <out_dir> [models_dir] [--script-root DIR]
                           [--carriers] [--curvy-levels N]
Reads _d.3d/_a.3d reconstructed from retail System/*.u by default, or from an
explicit models_dir.  ``--source`` also offers a reference-oracle escape hatch.
Metadata comes from exports/script (#exec — nothing guessed).
Emits <Mesh>.glb (canonical base = frame 0, targets F1..Fn) +
<Mesh>.meta.json. The authenticated stripped carrier for Angreal.TaintExpGeoB
alone uses frame 6 as its transient import base and rebases all other frames.
Canonical output is carrier-free. ``--carriers`` opts the isolated stripped
import path into COLOR_0 plus TEXCOORD_1 render-mapping carriers.
"""
import argparse
import json
import math
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import devtree_dir, exports_dir, original_dir  # noqa: E402

import authenticated_alternate_bind as alternate_bind
from curvy import (eval_curvy, float32_vectors, plan_curvy,
                   smooth_normals as curvy_smooth_normals)
from gltf import write_skinned_glb
from ue3d import (MTT_MASKED, MTT_MODULATE, MTT_PLACEHOLDER,
                  MTT_TRANSLUCENT, MTT_TWOSIDED, load_anim, load_data,
                  parse_exec_metadata)

ROOT = Path(__file__).resolve().parents[2]
RETAIL_SOURCE = "retail-package"
DEVTREE_SOURCE = "devtree"
RETAIL_ROOT = Path(os.environ.get("WOT_RETAIL_ROOT", original_dir(required=False)))
CONV_ROOT = Path(os.environ.get("WOT_CONV_DIR", ROOT / "Intermediate/Conv"))
RETAIL_MODEL_DIR = CONV_ROOT / "retail-meshes"
DEVTREE_MODEL_DIRS = sorted(
    (p for p in (devtree_dir(required=False) / "U2Radiosity").glob("*/Models")
     if p.is_dir()),
    key=lambda p: str(p).lower())
# find_pair() remains usable as a small James reader helper, but its implicit
# source is now the retail reconstruction.  The reference source is an oracle only.
MODEL_DIRS = [RETAIL_MODEL_DIR]

def _alternate_bind_vector_sha256(kind, vectors):
    return alternate_bind.vector_sha256(kind, vectors)


def _authenticated_alternate_bind_plan(payload_key, frames, normals, *, carriers):
    """Select the one authenticated non-collapsed transient import bind.

    Canonical conversion always retains retail frame zero.  The alternate bind
    exists only so UE Interchange can ingest the carrier-bearing stripped mesh;
    every retail frame remains a morph reconstruction target or the bind itself.
    """
    if not carriers or payload_key != alternate_bind.IDENTITY:
        return None
    if (len(frames) != alternate_bind.SOURCE_FRAME_COUNT
            or len(normals) != alternate_bind.SOURCE_FRAME_COUNT):
        raise ValueError("authenticated alternate-bind frame count changed")
    bind_frame = alternate_bind.BIND_FRAME
    actual_hashes = {
        "raw_base_positions_sha256":
            _alternate_bind_vector_sha256("positions", frames[0]),
        "raw_base_normals_sha256":
            _alternate_bind_vector_sha256("normals", normals[0]),
        "bind_positions_sha256":
            _alternate_bind_vector_sha256("positions", frames[bind_frame]),
        "bind_normals_sha256":
            _alternate_bind_vector_sha256("normals", normals[bind_frame]),
    }
    if actual_hashes != alternate_bind.HASHES:
        raise ValueError(
            "authenticated alternate-bind source vectors changed")
    target_frames = [
        frame for frame in range(len(frames)) if frame != bind_frame]
    receipt = {
        "format": alternate_bind.FORMAT,
        "source_identity": payload_key,
        "source_frame_count": len(frames),
        "bind_frame": bind_frame,
        "target_frames": target_frames,
        **actual_hashes,
    }
    return {
        "bind_frame": bind_frame,
        "target_frames": target_frames,
        "positions": frames[bind_frame],
        "normals": normals[bind_frame],
        "receipt": receipt,
    }


def mesh_names(metadata):
    """Canonical #exec-derived pipeline mesh census, in stable display order."""
    return sorted((item["name"] for item in metadata.values()), key=str.lower)


def reconstruct_retail_james(metadata, output_dir=RETAIL_MODEL_DIR,
                             retail_root=RETAIL_ROOT, names=None):
    """Decode canonical retail Mesh/LodMesh exports into flat James pairs."""
    packages = sorted((Path(retail_root) / "System").glob("*.u"),
                      key=lambda path: path.name.lower())
    if not packages:
        raise FileNotFoundError(
            f"no retail System packages under {Path(retail_root) / 'System'}")
    selected = list(names) if names is not None else mesh_names(metadata)
    from ue1pkg import export_meshes  # not needed for the reference source
    return export_meshes(packages, output_dir, selected, metadata)


def find_pair(meta, model_dirs=None, canonical_names=None):
    """Locate a James pair by canonical mesh name or authored #exec basename.

    Retail extraction is flat and names pairs after the imported mesh, while
    the later source tree retains authored filenames such as TrapAlarm_d.3d.
    Search recursively and resolve either convention deterministically.
    """
    search_dirs = [Path(path) for path in (model_dirs or MODEL_DIRS)]
    if canonical_names is None:
        canonical_names = model_dirs is None
        for directory in search_dirs:
            try:
                manifest = json.loads((directory / "_manifest.json").read_text())
                canonical_names = manifest.get("source") == RETAIL_SOURCE
            except (FileNotFoundError, json.JSONDecodeError, OSError):
                continue
            break
    canonical_pair = (
        f"{meta['name']}_d.3d".lower(), f"{meta['name']}_a.3d".lower())
    authored_pair = (
        Path(meta["datafile"].replace("\\", "/")).name.lower(),
        Path(meta["anivfile"].replace("\\", "/")).name.lower())
    pairs = ([canonical_pair, authored_pair] if canonical_names
             else [authored_pair, canonical_pair])
    for d in search_dirs:
        files = {f.name.lower(): f for f in sorted(d.rglob("*.3[dD]"))}
        for dname, aname in pairs:
            if dname in files and aname in files:
                return files[dname], files[aname]
    rendered = ", ".join(map(str, search_dirs)) or "<no model directories>"
    wanted = " or ".join(f"{dname}/{aname}" for dname, aname in pairs)
    raise FileNotFoundError(f"{wanted} not under {rendered}")


def rot_yaw(p, yaw_units):
    """#exec ORIGIN YAW is in 1/256 turns."""
    a = yaw_units * 2.0 * math.pi / 256.0
    c, s = math.cos(a), math.sin(a)
    return (p[0] * c - p[1] * s, p[0] * s + p[1] * c, p[2])


def rot_origin(p, rot):
    """Apply UE1's forward RotOrigin: roll, then pitch, then yaw."""
    pitch, yaw, roll = rot
    x, y, z = p
    if roll:
        a = roll * 2.0 * math.pi / 256.0
        c, s = math.cos(a), math.sin(a)
        y, z = y * c + z * s, -y * s + z * c
    if pitch:
        a = pitch * 2.0 * math.pi / 256.0
        c, s = math.cos(a), math.sin(a)
        x, z = x * c - z * s, x * s + z * c
    return rot_yaw((x, y, z), yaw) if yaw else (x, y, z)


def transform_frames(frames, meta):
    """Raw packed ints → UE1 mesh-local uu: (raw-Origin)*Scale, RotOrigin'd."""
    org = meta.get("origin", [0, 0, 0])
    sc = meta.get("scale", [0.1, 0.1, 0.2])
    rot = meta.get("rot", [0, 0, 0])
    out = []
    for f in frames:
        pts = []
        for v in f:
            p = ((v[0] - org[0]) * sc[0],
                 (v[1] - org[1]) * sc[1],
                 (v[2] - org[2]) * sc[2])
            pts.append(rot_origin(p, rot) if any(rot) else p)
        out.append(pts)
    return out


def transform_frames_float32(frames, meta):
    """Runtime decoder transform with float32 subtract/multiply boundaries."""
    def f32(value):
        return struct.unpack("<f", struct.pack("<f", value))[0]

    def sin_cos(authored_units):
        runtime_units = int(authored_units * 256.0) & 0xffff
        cardinal = {
            0: (0.0, 1.0),
            16384: (1.0, 0.0),
            32768: (0.0, -1.0),
            49152: (-1.0, 0.0),
        }
        if runtime_units in cardinal:
            return cardinal[runtime_units]
        angle = (runtime_units
                 * (6.283185307179586476925286766559 / 65536.0))
        return math.sin(angle), math.cos(angle)

    org = [f32(value) for value in meta.get("origin", [0, 0, 0])]
    scale = [f32(value) for value in meta.get("scale", [0.1, 0.1, 0.2])]
    pitch, yaw, roll = meta.get("rot", [0, 0, 0])
    transformed = []
    for frame in frames:
        points = []
        for vertex in frame:
            x, y, z = tuple(
                f32(f32(f32(vertex[axis]) - org[axis]) * scale[axis])
                for axis in range(3))
            if roll:
                sine, cosine = sin_cos(roll)
                new_y = y * cosine + z * sine
                new_z = -y * sine + z * cosine
                y, z = new_y, new_z
            if pitch:
                sine, cosine = sin_cos(pitch)
                new_x = x * cosine - z * sine
                new_z = x * sine + z * cosine
                x, z = new_x, new_z
            if yaw:
                sine, cosine = sin_cos(yaw)
                new_x = x * cosine - y * sine
                new_y = x * sine + y * cosine
                x, y = new_x, new_y
            points.append(tuple(f32(value) for value in (x, y, z)))
        transformed.append(points)
    return transformed


def smooth_normals(pts, tris):
    """Per-vertex normals for one frame, matching the retail renderer: each
    triangle normal is normalized BEFORE accumulating (unweighted average of
    unit face normals, not area-weighted), added once per corner occurrence.
    Retail links every triangle type into the vertex connectivity, so callers
    pass the full set including placeholder tris. Measured on MordethStatue:
    area-weighting deviated 9.2deg mean / 24deg p90 from the retail normals."""
    n = [[0.0, 0.0, 0.0] for _ in pts]
    for t in tris:
        a, b, c = (pts[i] for i in t)
        ux, uy, uz = b[0]-a[0], b[1]-a[1], b[2]-a[2]
        vx, vy, vz = c[0]-a[0], c[1]-a[1], c[2]-a[2]
        fx, fy, fz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
        l = math.sqrt(fx*fx + fy*fy + fz*fz + 0.001)
        fx, fy, fz = fx/l, fy/l, fz/l
        for i in t:
            n[i][0] += fx; n[i][1] += fy; n[i][2] += fz
    out = []
    for v in n:
        l = math.sqrt(v[0]**2 + v[1]**2 + v[2]**2) or 1.0
        out.append((v[0]/l, v[1]/l, v[2]/l))
    return out


def _frame_normals(frames, triangles, curvy_levels):
    normal_builder = curvy_smooth_normals if curvy_levels else smooth_normals
    return [normal_builder(frame, triangles) for frame in frames]


def to_gltf(p):
    """UE-intended uu → glTF (importer maps back as (x,z,y)*100)."""
    return (p[0] / 100.0, p[2] / 100.0, p[1] / 100.0)


def convert(mesh_name, out_dir, meta_all=None, model_dirs=None,
            source=RETAIL_SOURCE,
            script_root=exports_dir(required=False) / "script",
            retail_root=RETAIL_ROOT, retail_model_dir=None, carriers=False,
            curvy_levels=0):
    if curvy_levels and carriers:
        raise ValueError("curved mesh variants cannot carry stripped-mesh mapping data")
    meta_all = meta_all or parse_exec_metadata(script_root)
    meta = meta_all[mesh_name.lower()]
    if model_dirs is None:
        if source == DEVTREE_SOURCE:
            model_dirs = DEVTREE_MODEL_DIRS
        elif source == RETAIL_SOURCE:
            retail_model_dir = Path(retail_model_dir or RETAIL_MODEL_DIR)
            # Keep the James manifest a complete 243-mesh source census even
            # when a developer asks this CLI to rebuild only one GLB.
            reconstruct_retail_james(meta_all, retail_model_dir, retail_root)
            model_dirs = [retail_model_dir]
        else:
            raise ValueError(f"unknown mesh source: {source}")
    dfile, afile = find_pair(
        meta, model_dirs, canonical_names=(source == RETAIL_SOURCE))
    data = load_data(dfile)
    raw_frames = load_anim(afile, data["numverts"])
    # Keep legacy flat conversion byte-stable, but ignore commented-out #exec
    # alternatives when matching the runtime decoder's curvy position transform.
    transform_meta = (parse_exec_metadata(
        script_root, strip_line_comments=True)[mesh_name.lower()]
        if curvy_levels else meta)
    frames = (transform_frames_float32(raw_frames, transform_meta) if curvy_levels
              else transform_frames(raw_frames, meta))

    payload_key = None
    animation_logical_id = None
    if source == RETAIL_SOURCE:
        try:
            retail_manifest = json.loads((dfile.parent / "_manifest.json").read_text())
            manifest_meshes = retail_manifest.get("meshes", {})
            manifest_entry = next(
                (entry for name, entry in manifest_meshes.items()
                 if name.casefold() == meta["name"].casefold()), None)
            if manifest_entry:
                semantic_identity = (
                    f"{manifest_entry['package']}__{meta['name']}")
                payload_key = f"mesh:{semantic_identity}"
                animation_logical_id = (
                    f"cache/animation/{semantic_identity}")
        except (FileNotFoundError, KeyError, TypeError, ValueError,
                json.JSONDecodeError, OSError):
            payload_key = None
            animation_logical_id = None

    render = [t for t in data["tris"] if (t["type"] & 0xF) != MTT_PLACEHOLDER]
    placeholders = [t for t in data["tris"] if (t["type"] & 0xF) == MTT_PLACEHOLDER]
    # placeholder tris are invisible but DO contribute to retail's vertex
    # normals (the connectivity linkup spans every triangle type)
    smooth_tris = [t["idx"] for t in data["tris"]]
    if curvy_levels:
        # Guarded runtime decoding publishes the original pose as float32 before
        # accumulating smooth normals or deriving midpoints. Pin the dev bake to
        # that same boundary so every derived frame is bit-identical.
        frames = [float32_vectors(frame) for frame in frames]
    normals = _frame_normals(frames, smooth_tris, curvy_levels)
    curvy_plan = None
    if curvy_levels:
        curvy_plan = plan_curvy(render, curvy_levels)
        render = list(curvy_plan.triangles)
        evaluated = [eval_curvy(curvy_plan, frame, frame_normals)
                     for frame, frame_normals in zip(frames, normals)]
        frames = [item[0] for item in evaluated]
        normals = [item[1] for item in evaluated]
    alternate_bind = _authenticated_alternate_bind_plan(
        payload_key, frames, normals, carriers=carriers)
    import_frame = (alternate_bind["bind_frame"] if alternate_bind else 0)
    target_frames = (alternate_bind["target_frames"] if alternate_bind
                     else list(range(1, len(frames))))

    # Per-(texnum, James base type) primitives; the same texture slot may mix
    # opaque, two-sided, masked, or translucent triangles. Each corner gets a
    # unique vertex (UVs are per-corner).
    by_tex_type = {}
    for t in render:
        key = (t["tex"], t["type"] & 0xF)
        by_tex_type.setdefault(key, []).append(t)
    prims = []
    slot_types = {}
    for (tex, base_type), tris in sorted(by_tex_type.items()):
        texname = meta["textures"].get(tex, f"tex{tex}")
        suffix = ""
        if base_type == MTT_TWOSIDED: suffix = "_2S"
        elif base_type == MTT_MASKED: suffix = "_M"
        elif base_type == MTT_TRANSLUCENT: suffix = "_T"
        elif base_type == MTT_MODULATE: suffix = "_Mod"
        slot = texname + suffix
        type_flags = 0
        for t in tris:
            type_flags |= t["type"] & 0xF0
        current = slot_types.setdefault(
            slot, {"base_type": base_type, "flags": 0})
        current["flags"] |= type_flags
        corners = []          # (vert_index, u, v)
        idx = []
        for t in tris:
            # reversed corner order: axis swap in to_gltf is a reflection
            for k in (2, 1, 0):
                corners.append((t["idx"][k],
                                t["uvs"][k][0] / 255.0, t["uvs"][k][1] / 255.0))
                idx.append(len(idx))
        pos, nrm, uv = [], [], []
        color = [] if carriers else None
        mapping_uv = [] if carriers else None
        for vi, u, v in corners:
            pos += to_gltf(frames[import_frame][vi])
            nrm += to_gltf(normals[import_frame][vi])
            uv += [u, v]
            if carriers:
                # Lossless post-import mapping carriers for the isolated
                # stripped path. UE1 mesh vertex counts are far below 24 bits;
                # +1 keeps vertex zero from becoming default black. Canonical
                # conversion never authors either attribute.
                encoded = vi + 1
                color += [encoded & 0xff, (encoded >> 8) & 0xff,
                          (encoded >> 16) & 0xff, 0xff]
                # Keep the same +1 index in full-precision TEXCOORD_1 so the
                # stripped editor bridge can cross-check the actual final
                # render order independently before removing both carriers.
                mapping_uv += [float(encoded), 0.0]
        targets = []
        for fi in target_frames:
            dpos, dnrm = [], []
            for vi, _u, _v in corners:
                p0, p1 = (to_gltf(frames[import_frame][vi]),
                          to_gltf(frames[fi][vi]))
                n0, n1 = (to_gltf(normals[import_frame][vi]),
                          to_gltf(normals[fi][vi]))
                dpos += [p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2]]
                dnrm += [n1[0]-n0[0], n1[1]-n0[1], n1[2]-n0[2]]
            targets.append((dpos, dnrm))
        primitive = dict(mat=slot, pos=pos, nrm=nrm, uv=uv,
                         idx=idx, targets=targets)
        if carriers:
            primitive.update(color=color, mapping_uv=mapping_uv)
        prims.append(primitive)

    # frames whose morph delta is (near-)zero everywhere: the engine importer
    # dedupes such targets away, so record them for the import-side count
    # check (e.g. TaintExpGeoB repeats its base pose for frames 1-5)
    degenerate = []
    for fi in target_frames:
        dp = max((abs(a - b) for p, q in zip(
                  frames[fi], frames[import_frame])
                  for a, b in zip(p, q)), default=0.0)
        dn = max((abs(a - b) for p, q in zip(
                  normals[fi], normals[import_frame])
                  for a, b in zip(p, q)), default=0.0)
        if dp < 0.15 and dn < 0.01:      # uu / unit-normal thresholds
            degenerate.append(fi)

    names = [f"F{i}" for i in target_frames]
    output_name = meta["name"] + ("_Curvy" if curvy_levels else "")
    out_dir = Path(out_dir); out_dir.mkdir(parents=True, exist_ok=True)
    write_skinned_glb(out_dir / f"{output_name}.glb", output_name, prims, names,
                      carriers=carriers)

    sockets = {}
    for si, t in enumerate(placeholders):
        sockets[f"socket{si}"] = [
            [round(sum(f[i][k] for i in t["idx"]) / 3.0, 3) for k in range(3)]
            for f in frames]
    alternate_bind_metadata = {}
    if alternate_bind:
        alternate_bind_metadata = {
            "authenticated_alternate_bind": {
                **alternate_bind["receipt"],
                "bind_positions": alternate_bind["positions"],
                "bind_normals": alternate_bind["normals"],
            },
        }
    payload_layout = (None if curvy_plan else dict(
        version=1,
        frame_verts=data["numverts"],
        anim_frames=len(frames),
        smooth_triangles=[index for tri in smooth_tris for index in tri],
        render_triangles=[index for tri in render for index in tri["idx"]],
        base_positions=frames[0],
        base_normals=normals[0],
        mapping_carrier=(
            "COLOR_0-RGB24-PLUS1+TEXCOORD_1-FLOAT-PLUS1"),
    ))
    curvy_metadata = (dict(
        levels=curvy_plan.levels,
        base_mesh=meta["name"],
        parents=[list(pair) for pair in curvy_plan.parents],
    ) if curvy_plan else None)
    (out_dir / f"{output_name}.meta.json").write_text(json.dumps(dict(
        mesh=output_name, source=source, frames=len(frames),
        mapping_carriers=bool(carriers),
        degenerate_frames=degenerate,
        origin=meta.get("origin", [0, 0, 0]),
        rot=meta.get("rot", [0, 0, 0]),
        scale=meta.get("scale", [0.1, 0.1, 0.2]),
        unmirror=str(meta.get("unmirror", "0")) != "0",
        payload_key=payload_key,
        animation_logical_id=animation_logical_id,
        **alternate_bind_metadata,
        **({"curvy": curvy_metadata} if curvy_metadata else {}),
        payload_layout=payload_layout,
        sequences=meta["sequences"], notifies=meta["notifies"],
        textures={str(k): v for k, v in meta["textures"].items()},
        slot_types=slot_types,
        sockets=sockets), indent=1))
    print(f"{output_name}: prims={len(prims)} frames={len(frames)} "
          f"tris={sum(len(p['idx'])//3 for p in prims)} "
          f"placeholders={len(placeholders)} seqs={len(meta['sequences'])} "
          f"carriers={'on' if carriers else 'off'}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mesh_name")
    parser.add_argument("out_dir", nargs="?", default=ROOT / "Intermediate/Conv/mesh")
    parser.add_argument("models_dir", nargs="?", type=Path,
                        help="explicit, already-reconstructed James source tree")
    parser.add_argument("--source", choices=(RETAIL_SOURCE,),
                        default=RETAIL_SOURCE,
                        help="mesh provenance/input selector (default: retail-package)")
    parser.add_argument("--retail-root", type=Path,
                        help="retail game root containing System/*.u")
    parser.add_argument("--retail-model-dir", type=Path,
                        help="James reconstruction cache (default: <conv>/retail-meshes)")
    parser.add_argument("--script-root", type=Path,
                        default=None,
                        help="root containing package subdirectories of exported .uc files")
    parser.add_argument("--carriers", action="store_true",
                        help="emit stripped-import COLOR_0/TEXCOORD_1 mapping carriers")
    parser.add_argument("--curvy-levels", type=int, default=0,
                        help="bake a _Curvy variant with this many subdivisions")
    arguments = parser.parse_args()
    arguments.retail_root = (arguments.retail_root.resolve()
                             if arguments.retail_root else original_dir())
    arguments.script_root = (arguments.script_root.resolve()
                             if arguments.script_root else exports_dir() / "script")
    convert(arguments.mesh_name, arguments.out_dir,
            model_dirs=[arguments.models_dir] if arguments.models_dir else None,
            source=arguments.source, script_root=arguments.script_root,
            retail_root=arguments.retail_root,
            retail_model_dir=arguments.retail_model_dir,
            carriers=arguments.carriers,
            curvy_levels=arguments.curvy_levels)
