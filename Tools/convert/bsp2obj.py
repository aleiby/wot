"""Compiled BSP → OBJ with per-texture material groups.

Usage: python3 bsp2obj.py <map.wot> <out_dir> [tex_manifest_root] [atlas_json]
Emits <Map>.obj / <Map>.mtl / <Map>.bsp.json (summary). Geometry is what the
player saw: CSG-ed node polygons, invisible/portal surfs dropped. UE1 coords
are left as-is (agreed handling: UE5 fixes handedness at import; WorldToMeters
handles scale in VR). If tex_manifest_root (utx2png output) is
given, UVs are normalized by texture size; otherwise they stay in texels.

Lightmap UVs (TEXCOORD_1): if <out_dir>/<Map>_lmatlas.json exists (produced
by lightmap_atlas.py), or an explicit atlas_json path is passed as the 4th
arg, every GLB vertex gets a second UV set into the composed-lightmap atlas:
uv1 = ((x + u_tex + 0.5)/W, (y + v_tex + 0.5)/H), where (u_tex, v_tex) are
the fractional lightmap texel coords of the vertex's ORIGINAL world position
(lightmaps.lightmap_uv — the same Pan/UScale/VScale texel-center convention
the composer used, computed before the axis swap). Surfaces without a
lightmap (PF_Unlit, sky, iLightMap==-1) map to the atlas's reserved
fullbright texel so the material multiply leaves them untouched.

Sky-slot split: if <out_dir>/<Map>.t3d.json exists (same discovery pattern as
the atlas), sky-zone faces whose material is also used by non-sky faces get
a "__SkyZone"-suffixed slot (SKY_SUFFIX; skygeo.py applies the same rule) so
setup_childsky.py can hide the far sky-brush copy completely.

Macro-pan split: a material slot used by macro-textured surfaces with more
than one authored PanU/PanV pair is split into deterministic
"__MacroPan_<u>_<v>" slots. This keeps the canonical GLB/parity fixture in
lockstep with the native BSP v10 importer, whose section record carries one
exact macro transform.
"""
import glob
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

from gltf import write_glb
from ue1pkg import (PF_FAKEBACKDROP, PF_INVISIBLE, PF_MASKED, PF_MODULATED,
                    PF_PORTAL, PF_TRANSLUCENT, PF_TWOSIDED, PF_UNLIT,
                    Package, decode_model)

PF_AUTOUPAN, PF_AUTOVPAN = 0x200, 0x400
PF_SPECIALLIT = 0x00100000

SKIP = PF_INVISIBLE | PF_PORTAL

# Sky-zone faces whose material is ALSO used by non-sky faces go to their own
# "<mat>__SkyZone" slot so setup_childsky.py can hide the far sky-brush copy
# without stripping real world geometry (previously such world_shared slots
# stayed visible: the Arena_06/Battle2_03/Battle2_04 residual). Sky-only
# materials keep their plain slot, so maps without sharing are byte-identical.
SKY_SUFFIX = "__SkyZone"


def flag_suffix(f):
    s = ""
    if f & PF_MASKED: s += "_M"
    if f & PF_TRANSLUCENT: s += "_T"
    if f & PF_MODULATED: s += "_Mod"
    if f & PF_TWOSIDED: s += "_2S"
    if f & PF_UNLIT: s += "_U"
    if f & PF_FAKEBACKDROP: s += "_Sky"
    # Dynamic emitters are all bSpecialLit=false. Keep authored
    # PF_SpecialLit surfaces in distinct material sections so their MIC can
    # set DynLightEnable=0 without suppressing ordinary uses of the texture.
    if f & PF_SPECIALLIT: s += "_SL"
    return s


def load_tex_sizes(root):
    """(pkg.lower(), name.lower()) -> (canonical_folder, canonical_name, w, h, ds).
    Package-qualified (folder == export package) AND case-insensitive: UE package
    names are case-insensitive and a map's import table can disagree in case with
    the .utx stem (WOTDecorations vs WoTDecorations, Naturet vs NatureT). Values
    carry the canonical export-folder case so the asset path is always right. `ds`
    = the texture's DrawScale (default 1.0): UE1 renders a texture at 1/DrawScale
    the tiling, so the UV normalization uses w*ds, not w."""
    sizes = {}
    for m in glob.glob(str(Path(root) / "*/_manifest.json")):
        folder = Path(m).parent.name
        for name, info in json.load(open(m))["textures"].items():
            sizes[(folder.lower(), name.lower())] = (folder, name, info["w"],
                                                     info["h"],
                                                     info.get("drawscale", 1.0))
    # Flipbook conversion runs before BSP conversion.  Its manifest is the
    # authority for the rare grouped-name collision while preserving legacy
    # leaf keys for every unique grouped procedural export.
    flipbook_root = Path(root).parent / "flipbooks"
    for manifest_path in flipbook_root.glob("*/_flipbooks.json"):
        folder = manifest_path.parent.name
        for asset_key, info in json.loads(
                manifest_path.read_text(encoding="utf-8-sig"))["fire"].items():
            object_path = info.get("object_path")
            if not object_path or "." not in object_path:
                continue
            package, relative = object_path.split(".", 1)
            sizes[(package.lower(), relative.lower())] = (
                folder, asset_key, info["w"], info["h"], 1.0)
    return sizes


def load_tex_macros(root, sizes):
    """Canonical base texture -> canonical authored MacroTexture facts."""
    records = {}
    for manifest_path in sorted(Path(root).glob("*/_manifest.json"),
                                key=lambda path: str(path).casefold()):
        folder = manifest_path.parent.name
        document = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        for name, record in document.get("textures", {}).items():
            records[(folder.casefold(), str(name).casefold())] = record
    result = {}
    for base_key, record in records.items():
        macro = record.get("macro")
        if not macro:
            continue
        if (not isinstance(macro, list) or len(macro) != 2
                or not all(isinstance(value, str) and value for value in macro)):
            raise ValueError(f"invalid MacroTexture identity for {base_key}")
        canonical = sizes.get((macro[0].casefold(), macro[1].casefold()))
        if canonical is None:
            raise ValueError(
                f"MacroTexture is absent from the canonical resolver: "
                f"{macro[0]}.{macro[1]}")
        result[base_key] = canonical
    return result


def resolve_tex(pkg, ref, tex_sizes):
    """Surf texture ref -> (canonical_package, canonical_name, w, h, ds). Normalizes
    case to the exported folder; parametric/unexported textures keep the ref's
    own (package, name) with size (1, 1) and DrawScale 1.0 (handled downstream)."""
    r = pkg.objref(ref)
    if not r:
        return ("", "None", 1, 1, 1.0)
    full = pkg.objpath(ref) or r[1]
    parts = full.split(".")
    if ref < 0 and parts and parts[0].casefold() == r[0].casefold():
        parts = parts[1:]
    relative = ".".join(parts) if parts else r[1]
    canon = tex_sizes.get((r[0].lower(), relative.lower()))
    if canon:
        return canon
    canon = tex_sizes.get((r[0].lower(), r[1].lower()))
    if canon:
        return canon
    return (r[0], r[1], 1, 1, 1.0)



def build_groups(pkg, bsp, tex_sizes, stats=None, include_planes=False):
    """BSP model → {material: [(idx, uvs, normal) triangles]} (shared by map
    and mover converters)."""
    from collections import defaultdict
    groups = defaultdict(list)
    texflags = {}
    stats = stats if stats is not None else defaultdict(int)
    for ivp, isurf, nv, plane_n in bsp["nodes"]:
        if nv < 3:
            continue
        s = bsp["surfs"][isurf]
        if s["flags"] & SKIP:
            stats["skipped_faces"] += 1
            continue
        package, tex, w, h, ds = resolve_tex(pkg, s["tex"], tex_sizes)
        mat = Package.qualkey(package, tex) + flag_suffix(s["flags"])
        texflags[mat] = (package, tex, s["flags"])
        base = bsp["points"][s["pbase"]]
        tu = bsp["vectors"][s["vtu"]]
        tv = bsp["vectors"][s["vtv"]]
        surf_n = bsp["vectors"][s["vnorm"]]
        idx, uvs, pts = [], [], []
        for i in range(nv):
            pi = bsp["verts"][ivp + i][0]
            p = bsp["points"][pi]
            u = sum((a - b) * c for a, b, c in zip(p, base, tu)) + s["panu"]
            v = sum((a - b) * c for a, b, c in zip(p, base, tv)) + s["panv"]
            idx.append(pi)
            uvs.append((u / (w * ds), v / (h * ds)))
            pts.append(p)
        nx = ny = nz = 0.0
        for i in range(nv):
            (x1, y1, z1), (x2, y2, z2) = pts[i], pts[(i + 1) % nv]
            nx += (y1 - y2) * (z1 + z2)
            ny += (z1 - z2) * (x1 + x2)
            nz += (x1 - x2) * (y1 + y2)
        if nx * surf_n[0] + ny * surf_n[1] + nz * surf_n[2] < 0:
            idx.reverse(); uvs.reverse()
            stats["flipped"] += 1
        area_eps = 1e-3
        for i in range(1, nv - 1):
            tri = (idx[0], idx[i], idx[i + 1])
            if len(set(tri)) < 3:
                continue
            a, b_, c = (bsp["points"][t] for t in tri)
            ux, uy, uz = (b_[0]-a[0], b_[1]-a[1], b_[2]-a[2])
            vx, vy, vz = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
            cx, cy, cz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
            if cx*cx + cy*cy + cz*cz < area_eps:
                stats["degenerate_tris"] += 1
                continue
            row = (tri, (uvs[0], uvs[i], uvs[i + 1]), surf_n)
            # Mover-only metadata. The default tuple shape is intentionally
            # unchanged so world conversion remains byte-for-byte identical.
            if include_planes:
                row += ((tuple(base), tuple(surf_n)),)
            groups[mat].append(row)
            stats["tris"] += 1
        stats["faces"] += 1
    return groups, texflags, stats


def split_mover_groups_by_plane(groups, texflags, points):
    """Split material groups into deterministic planar mover sections.

    The world converter deliberately never calls this function: its historical
    material grouping and GLB bytes remain unchanged.  Retail dynamic movers,
    however, select their permeating lights against each drawn surface plane.
    Interchange maps one glTF primitive/material to one UE static-mesh section,
    so a mover slot must not combine the unrelated planes which happen to use
    the same texture.

    Returns ``(groups, texflags, surfaces, extras)``.  ``surfaces`` is the
    importer manifest in UE1 local coordinates; ``extras`` carries the same
    plane in glTF coordinates on the primitive itself.  Plane keys retain the
    oriented normal because opposite-facing coplanar surfaces select different
    lights.
    """
    from collections import defaultdict
    import math

    def plane_of(triangle):
        indices, _uvs, normal = triangle[:3]
        if len(triangle) > 3:
            origin, normal = triangle[3]
        else:
            origin = points[indices[0]]
        length = math.sqrt(sum(float(value) * float(value)
                               for value in normal))
        if length <= 0.0:
            raise ValueError("mover triangle has a zero surface normal")
        unit = tuple(float(value) / length for value in normal)
        origin = tuple(float(value) for value in origin)
        distance = sum(unit[axis] * origin[axis] for axis in range(3))
        # Quantization only identifies mathematically identical BSP planes;
        # the unrounded origin/normal below remain the runtime contract.
        key = tuple(round(value, 7) for value in (*unit, distance))
        return key, origin, unit

    buckets = defaultdict(list)
    plane_rows = {}
    for material, triangles in groups.items():
        for triangle in triangles:
            plane_key, origin, normal = plane_of(triangle)
            key = (material, plane_key)
            buckets[key].append(triangle)
            plane_rows.setdefault(key, (origin, normal))

    split_groups = {}
    split_texflags = {}
    surfaces = []
    extras = {}
    for section_index, key in enumerate(sorted(buckets)):
        material, _plane_key = key
        slot = f"{material}__MP{section_index:04d}"
        origin, normal = plane_rows[key]
        # Carry the authoritative retail plane into mover-only GLB emission.
        # Compiled BSP split vertices can drift a few units from their source
        # FBSPSurf plane; projecting those emitted duplicates makes the actual
        # section planar without mutating the shared model point table.
        split_groups[slot] = [triangle[:3] + ((origin, normal),)
                              for triangle in buckets[key]]
        split_texflags[slot] = texflags[material]
        surfaces.append(dict(
            slot=slot,
            source_material=material,
            origin=list(origin),
            normal=list(normal),
        ))
        extras[slot] = {
            "wotMoverPlane": {
                # groups_to_glb performs the same UE1 -> glTF conversion for
                # vertex data: (x,z,y)/100 and the y/z normal swap.
                "origin": [origin[0] / 100.0, origin[2] / 100.0,
                           origin[1] / 100.0],
                "normal": [normal[0], normal[2], normal[1]],
            }
        }
    return split_groups, split_texflags, surfaces, extras


def sky_shared_split(pkg, bsp, tex_sizes, t3d_json):
    """(sky-zone surface ids, materials used by BOTH sky and non-sky faces).

    Mirrors skygeo.py's face classification exactly (same winner-zone
    selection, same per-SURFACE membership), so the world mesh's split slots
    line up with the sky mesh's. Needs the map's t3d.json (SkyZoneInfo winner
    selection reads actors) and zone data in the decoded bsp (the atlas path
    decodes it); returns empty sets when either is missing — no split."""
    if not (t3d_json.exists() and bsp.get("zones")):
        return set(), set()
    import skygeo as SG   # lazy: skygeo imports this module
    actors = json.loads(t3d_json.read_text())["actors"]
    sky_zone, _ = SG.sky_zone_of(pkg, bsp, actors)
    if sky_zone is None:
        return set(), set()
    sky_ids = SG.sky_surface_ids(bsp, sky_zone)
    sky_mats, world_mats = set(), set()
    for node in bsp["nodes"]:
        isurf, nv = node[1], node[2]
        if nv < 3:
            continue
        s = bsp["surfs"][isurf]
        if s["flags"] & PF_INVISIBLE:
            continue
        package, tex, _, _, _ = resolve_tex(pkg, s["tex"], tex_sizes)
        mat = Package.qualkey(package, tex) + flag_suffix(s["flags"])
        (sky_mats if isurf in sky_ids else world_mats).add(mat)
    return sky_ids, sky_mats & world_mats


def base_surface_material_slot(pkg, surface, surface_index, tex_sizes,
                               sky_ids, split_materials):
    package, texture, width, height, draw_scale = resolve_tex(
        pkg, surface["tex"], tex_sizes)
    slot = Package.qualkey(package, texture) + flag_suffix(surface["flags"])
    if surface_index in sky_ids and slot in split_materials:
        slot += SKY_SUFFIX
    return package, texture, width, height, draw_scale, slot


def macro_pan_split_slots(pkg, bsp, tex_sizes, sky_ids, split_materials,
                          macro_lookups):
    """Return (portal, base-slot) keys whose macro pan is not single-valued."""
    pans = defaultdict(set)
    for node in bsp["nodes"]:
        _vertex_pool, surface_index, vertex_count = node[:3]
        if vertex_count < 3:
            continue
        surface = bsp["surfs"][surface_index]
        if surface["flags"] & PF_INVISIBLE:
            continue
        package, texture, *_unused, slot = base_surface_material_slot(
            pkg, surface, surface_index, tex_sizes, sky_ids, split_materials)
        if (package.casefold(), texture.casefold()) not in macro_lookups:
            continue
        portal = bool(surface["flags"] & PF_PORTAL)
        pans[(portal, slot)].add((int(surface["panu"]), int(surface["panv"])))
    return {key for key, values in pans.items() if len(values) > 1}


def macro_pan_material_slot(base_slot, pan_u, pan_v):
    suffix = f"__MacroPan_{int(pan_u)}_{int(pan_v)}"
    if base_slot.endswith(SKY_SUFFIX):
        return base_slot[:-len(SKY_SUFFIX)] + suffix + SKY_SUFFIX
    return base_slot + suffix


def groups_to_glb(path, name, groups, points, primitive_extras=None):
    """Shared GLB emission (axis swap + winding reversal, /100 scale)."""
    glb_groups = {}
    for mat in sorted(groups):
        pos, nrm, uv, ind = [], [], [], []
        for triangle in groups[mat]:
            idx, uvs, n = triangle[:3]
            plane = triangle[3] if len(triangle) > 3 else None
            for pi, (tu, tv) in zip(reversed(idx), reversed(uvs)):
                p = points[pi]
                if plane is not None:
                    origin, plane_normal = plane
                    distance = sum((p[axis] - origin[axis])
                                   * plane_normal[axis] for axis in range(3))
                    p = tuple(p[axis] - distance * plane_normal[axis]
                              for axis in range(3))
                pos += [p[0] / 100.0, p[2] / 100.0, p[1] / 100.0]
                nrm += [n[0], n[2], n[1]]
                uv += [tu, tv]
                ind.append(len(ind))
        glb_groups[mat] = (pos, nrm, uv, ind)
    write_glb(path, name, glb_groups, primitive_extras=primitive_extras)


def convert(map_path, out_dir, tex_root=None, atlas_json=None):
    tex_sizes = load_tex_sizes(tex_root) if tex_root else {}
    macro_lookups = load_tex_macros(tex_root, tex_sizes) if tex_root else {}
    pkg = Package(map_path)
    models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
    name = Path(map_path).stem
    out_dir = Path(out_dir); out_dir.mkdir(parents=True, exist_ok=True)

    # optional composed-lightmap atlas → TEXCOORD_1 (see module docstring)
    if atlas_json is None:
        cand = out_dir / f"{name}_lmatlas.json"
        atlas_json = str(cand) if cand.exists() else None
    atlas = None
    if atlas_json:
        import lightmaps as lmz            # numpy-only when atlas requested
        atlas = json.load(open(atlas_json))
        bsp = lmz.decode_model_full(pkg, max(models, key=lambda e: e["size"]))
    else:
        bsp = decode_model(pkg, max(models, key=lambda e: e["size"]))

    # group faces by (texture, flag-family) → material slots. Portal surfaces
    # render in UE1 unless PF_Invisible (water/waterfall zone boundaries are
    # textured translucent/modulated portals — the renderer draws them like
    # any surface) but they must not collide, so they go to a separate
    # <name>_portals.glb the importer spawns with collision disabled.
    groups = defaultdict(list)   # mat -> [(pt_indices, uvs, normal, lm_uvs)]
    pgroups = defaultdict(list)  # same, for PF_Portal & !PF_Invisible
    texflags = {}
    ptexflags = {}
    pan_zones = defaultdict(Counter)   # mat -> zone-actor-name votes
    stats = defaultdict(int)
    lm_out_of_gutter = 0
    zone_names = [pkg.objname(z) if z else None
                  for z in (bsp.get("zones") or [])]
    sky_ids, split_mats = sky_shared_split(
        pkg, bsp, tex_sizes, out_dir / f"{name}.t3d.json")
    macro_split_slots = macro_pan_split_slots(
        pkg, bsp, tex_sizes, sky_ids, split_mats, macro_lookups)
    shared_slots = {}
    for node in bsp["nodes"]:
        ivp, isurf, nv = node[0], node[1], node[2]
        if nv < 3:
            continue
        s = bsp["surfs"][isurf]
        if s["flags"] & PF_INVISIBLE:
            stats["skipped_faces"] += 1
            continue
        portal = bool(s["flags"] & PF_PORTAL)
        package, tex, w, h, ds, mat = base_surface_material_slot(
            pkg, s, isurf, tex_sizes, sky_ids, split_mats)
        shared_mat = mat
        if (portal, shared_mat) in macro_split_slots:
            mat = macro_pan_material_slot(shared_mat, s["panu"], s["panv"])
        shared_slots[mat] = shared_mat
        if shared_mat.endswith(SKY_SUFFIX):
            stats["sky_split_faces"] += 1
        (ptexflags if portal else texflags)[mat] = (package, tex, s["flags"])
        # AutoU/VPan surfaces pan at 35*Zone.TexU/VPanSpeed texels/sec —
        # record the slot's majority zone actor so the
        # importer can look up the pan speeds in the t3d.
        if s["flags"] & (PF_AUTOUPAN | PF_AUTOVPAN) and len(node) > 4:
            iz = node[4]
            for z in tuple(iz):
                if z and z < len(zone_names) and zone_names[z]:
                    pan_zones[shared_mat][zone_names[z]] += 1
        base = bsp["points"][s["pbase"]]
        tu = bsp["vectors"][s["vtu"]]
        tv = bsp["vectors"][s["vtv"]]
        surf_n = bsp["vectors"][s["vnorm"]]
        idx, uvs, pts = [], [], []
        for i in range(nv):
            pi = bsp["verts"][ivp + i][0]
            p = bsp["points"][pi]
            u = sum((a - b) * c for a, b, c in zip(p, base, tu)) + s["panu"]
            v = sum((a - b) * c for a, b, c in zip(p, base, tv)) + s["panv"]
            idx.append(pi)
            uvs.append((u / (w * ds), v / (h * ds)))
            pts.append(p)
        # Lightmap UVs from the ORIGINAL world positions (before axis swap).
        lmuv = None
        if atlas:
            aw, ah = atlas["size"]
            ent = atlas["surfs"].get(str(isurf)) \
                if s.get("ilightmap", -1) >= 0 else None
            if ent:
                frac = lmz.lightmap_uv(bsp, isurf, pts)
                lmuv = [((ent["x"] + fu + 0.5) / aw, (ent["y"] + fv + 0.5) / ah)
                        for fu, fv in frac]
                lm_out_of_gutter += int(
                    ((frac[:, 0] < -1) | (frac[:, 0] > ent["w"]) |
                     (frac[:, 1] < -1) | (frac[:, 1] > ent["h"])).sum())
            else:
                fb = atlas["fullbright"]
                lmuv = [((fb["x"] + 0.5) / aw, (fb["y"] + 0.5) / ah)] * nv
        # Backface culling follows winding, so winding must agree with the
        # surf normal: measure the polygon's actual winding (Newell) and flip
        # disagreements (source of the backface-black holes).
        nx = ny = nz = 0.0
        for i in range(nv):
            (x1, y1, z1), (x2, y2, z2) = pts[i], pts[(i + 1) % nv]
            nx += (y1 - y2) * (z1 + z2)
            ny += (z1 - z2) * (x1 + x2)
            nz += (x1 - x2) * (y1 + y2)
        flipped = nx * surf_n[0] + ny * surf_n[1] + nz * surf_n[2] < 0
        if flipped:
            idx.reverse(); uvs.reverse()
            if lmuv:
                lmuv.reverse()
        n = surf_n
        # Pre-triangulate (fan) — Interchange's OBJ n-gon path silently drops
        # faces. Skip degenerate slivers (colinear verts from BSP splits).
        area_eps = 1e-3
        for i in range(1, nv - 1):
            tri = (idx[0], idx[i], idx[i + 1])
            if tri[0] == tri[1] or tri[1] == tri[2] or tri[0] == tri[2]:
                continue
            a, b_, c = (bsp["points"][t] for t in tri)
            ux, uy, uz = (b_[0]-a[0], b_[1]-a[1], b_[2]-a[2])
            vx, vy, vz = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
            cx, cy, cz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
            if cx*cx + cy*cy + cz*cz < area_eps:
                stats["degenerate_tris"] += 1
                continue
            (pgroups if portal else groups)[mat].append(
                ((tri[0], tri[1], tri[2]),
                 (uvs[0], uvs[i], uvs[i + 1]), n,
                 (lmuv[0], lmuv[i], lmuv[i + 1]) if lmuv
                 else None))
            stats["portal_tris" if portal else "tris"] += 1
        stats["portal_faces" if portal else "faces"] += 1
        stats["flipped"] += flipped

    # GLB: UE's OBJ translator silently drops most faces; glTF imports cleanly.
    # Importer mapping (probed): UE = (gltf.x, gltf.z, gltf.y) * 100 — so we
    # emit gltf = (ue.x, ue.z, ue.y) / 100 to land on original coordinates.
    pts = bsp["points"]

    def emit(gdict):
        glb_groups = {}
        for mat in sorted(gdict):
            pos, nrm, uv, ind = [], [], [], []
            uv1 = [] if atlas else None
            for idx, uvs, n, lmuvs in gdict[mat]:
                # the y/z axis swap below is a reflection — reverse winding to
                # keep faces fronting the room interiors (UE1 convention)
                for k in (2, 1, 0):
                    pi, (tu, tv) = idx[k], uvs[k]
                    p = pts[pi]
                    pos += [p[0] / 100.0, p[2] / 100.0, p[1] / 100.0]
                    nrm += [n[0], n[2], n[1]]
                    uv += [tu, tv]
                    if uv1 is not None:
                        uv1 += [lmuvs[k][0], lmuvs[k][1]]
                    ind.append(len(ind))
            glb_groups[mat] = (pos, nrm, uv, ind) if uv1 is None \
                else (pos, nrm, uv, ind, uv1)
        return glb_groups

    # Provenance stamp: how many texture-size records this conversion actually
    # loaded. A conversion run WITHOUT the tex root silently falls back to
    # DrawScale 1.0 for every texture and produced UVs exactly 2x wrong for
    # DrawScale-2 textures, and nothing recorded that the GLBs were built
    # blind. Downstream
    # consumers that need correct UVs (the parity fixture generator) refuse a
    # GLB whose stamp is missing or zero.
    provenance = {"wot_tex_sizes": len(tex_sizes)}
    write_glb(out_dir / f"{name}.glb", name, emit(groups),
              asset_extras=provenance)
    if pgroups:
        write_glb(out_dir / f"{name}_portals.glb", f"{name}_portals",
                  emit(pgroups), asset_extras=provenance)
    def _mat_entry(m, pkgname, t, fl):
        # package = authoritative source package (objref); the importer binds
        # /Game/WoT/Textures/<package>/<texture> — never a bare-name lookup.
        e = dict(package=pkgname, texture=t, flags=fl)
        shared = shared_slots.get(m, m)
        if shared in pan_zones:
            e["pan_zone"] = pan_zones[shared].most_common(1)[0][0]
        return e

    (out_dir / f"{name}.bsp.json").write_text(json.dumps(dict(
        stats=dict(stats), materials={m: _mat_entry(m, p, t, fl)
                                      for m, (p, t, fl) in texflags.items()},
        portal_materials={m: _mat_entry(m, p, t, fl)
                          for m, (p, t, fl) in ptexflags.items()},
        points=len(pts)), indent=1))
    print(f"{name}: {stats['faces']} faces, {len(groups)} materials, "
          f"{stats['portal_faces']} portal faces ({len(pgroups)} materials), "
          f"{stats['skipped_faces']} skipped, {len(pts)} points"
          + (f", lightmap UV1 on (verts beyond gutter: {lm_out_of_gutter})"
             if atlas else "")
          + (f", sky-split {sorted(split_mats)} "
             f"({stats['sky_split_faces']} faces)" if split_mats else ""))


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2],
            sys.argv[3] if len(sys.argv) > 3 else None,
            sys.argv[4] if len(sys.argv) > 4 else None)
