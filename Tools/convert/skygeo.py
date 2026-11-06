"""Extract the sky-zone (FakeBackdrop child-scene) geometry as its own mesh.

Retail renders the sky as a recursive child frame rooted at the SkyZoneInfo
(the renderer's CreateChildFrame): the sky-zone brushes (dome/box +
cloud/star/moon/sun layers) drawn by the NORMAL pipeline — real lightmaps,
the 2x overbright, and PF_AutoUPan/VPan texture panning
(35 * Zone.TexU/VPanSpeed). A baked panorama can't reproduce the panning, so
we import the actual geometry and render it as a camera-locked backdrop (the
UE1 child frame equivalent).

This emits, per map:
  <name>_skygeo.glb   — sky-zone surfaces (material groups, lightmap UV1),
                        vertices in the SAME UE5 convention as the world mesh
                        (x,z,y)/100, so the importer path is shared. When
                        <name>_skyrest.json exists (skybake.py --rest,
                        portal-unsealed zones), also the rest-of-map surfaces
                        the child frame sees THROUGH the sky's portal sheets:
                        "SkyRest__<mat>" groups (bound to the world MICs by
                        setup_childsky.py — live pan/lightmaps), "SkyRestFB"
                        (nested sky windows, hidden), "SkyRestBlock"
                        (flipped-winding solid-space blockers, opaque black).
  <name>_skygeo.json  — materials (package/texture/flags/pan_zone), the sky
                        origin + rotation, and per-zone TexU/VPanSpeed.

Sky-zone surfaces = any surface whose BSP nodes touch the winning SkyZoneInfo's
zone. Includes PF_Portal sky layers (Arena_04 modulated sky, B3_02) which the
world converter routes elsewhere.

Usage: python skygeo.py <map.wot> <out_dir> <tex_root> <atlas_json> <t3d_json>
"""
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bsp2obj as B
import lightmaps as L
import skybake as SB
from bsp2obj import PF_AUTOUPAN, PF_AUTOVPAN  # noqa (0x200/0x400)
from ue1pkg import PF_INVISIBLE, Package  # noqa


def sky_zone_of(pkg, bsp, actors):
    skies, links = SB.zone_sky_links(actors, pkg, bsp)
    if not skies:
        return None, None
    winner = SB.pick_winner(bsp, skies, links)
    for zi, zref in enumerate(bsp["zones"] or []):
        if zref and str(pkg.objname(zref)).lower() == winner["name"].lower():
            return zi, winner
    return None, winner


def sky_surface_ids(bsp, sky_zone):
    """Surfaces whose nodes touch the sky zone."""
    out = set()
    for node in bsp["nodes"]:
        if len(node) > 4 and node[2] >= 3:
            iz = node[4]
            if sky_zone in (tuple(iz) if isinstance(iz, (list, tuple)) else (iz,)):
                out.add(node[1])
    return out


def tri_dist(p, a, b, c):
    """Distance from point p to triangle abc (Ericson, Real-Time Collision
    Detection 5.1.5). All numpy 3-vectors."""
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = ab @ ap, ac @ ap
    if d1 <= 0 and d2 <= 0:
        return np.linalg.norm(ap)
    bp = p - b
    d3, d4 = ab @ bp, ac @ bp
    if d3 >= 0 and d4 <= d3:
        return np.linalg.norm(bp)
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        return np.linalg.norm(ap - ab * (d1 / (d1 - d3)))
    cp = p - c
    d5, d6 = ab @ cp, ac @ cp
    if d6 >= 0 and d5 <= d6:
        return np.linalg.norm(cp)
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        return np.linalg.norm(ap - ac * (d2 / (d2 - d6)))
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        return np.linalg.norm(p - (b + w * (c - b)))
    denom = 1.0 / (va + vb + vc)
    v, w = vb * denom, vc * denom
    return np.linalg.norm(p - (a + ab * v + ac * w))


def convert_sky(map_path, out_dir, tex_root, atlas_json, t3d_json):
    out_dir = Path(out_dir)
    name = Path(map_path).stem
    tex_sizes = B.load_tex_sizes(tex_root) if tex_root else {}
    atlas = json.loads(Path(atlas_json).read_text()) if atlas_json else None
    pkg, bsp = L.load_level(map_path)
    actors = json.loads(Path(t3d_json).read_text())["actors"]
    by_name = {a.get("name"): a for a in actors if isinstance(a, dict)}

    sky_zone, winner = sky_zone_of(pkg, bsp, actors)
    if sky_zone is None:
        (out_dir / f"{name}_skygeo.json").write_text(json.dumps({}))
        print(f"{name}: no sky zone")
        return
    sky_ids = sky_surface_ids(bsp, sky_zone)

    # Materials used by BOTH sky and non-sky faces get the same "__SkyZone"
    # slot split the world converter applies (bsp2obj.sky_shared_split), so
    # the sky mesh's slot names keep matching the world mesh's for binding
    # AND the far sky-brush copy becomes fully hideable (world_shared is
    # empty by construction after the split).
    world_mats, sky_texmats = set(), set()
    for node in bsp["nodes"]:
        isurf, nv = node[1], node[2]
        if nv < 3:
            continue
        s = bsp["surfs"][isurf]
        if s["flags"] & PF_INVISIBLE:
            continue
        package, tex, _, _, _ = B.resolve_tex(pkg, s["tex"], tex_sizes)
        m = Package.qualkey(package, tex) + B.flag_suffix(s["flags"])
        (sky_texmats if isurf in sky_ids else world_mats).add(m)
    split = sky_texmats & world_mats

    zone_names = [pkg.objname(z) if z else None for z in (bsp.get("zones") or [])]
    groups = defaultdict(list)
    texflags = {}
    pan_zones = defaultdict(Counter)
    pts = bsp["points"]

    # Rest-of-map surfaces a PORTAL-UNSEALED sky zone shows (skybake.py
    # --rest): retail's child frame keeps traversing through the sky's
    # translucent portal sheets and draws the map behind them. Ship that
    # content as LIVE GEOMETRY in the sky mesh — "SkyRest__<mat>" slots that
    # setup_childsky.py binds to the same WORLD MICs (pan + atlas lightmaps
    # stay live), nested FakeBackdrop windows as a hidden slot (retail
    # re-enters the sky there; what sits behind them in the box is the
    # closest static equivalent), and solid-space blockers as
    # flipped-winding opaque black (retail's traversal stops at solid volume).
    rest_doc = out_dir / f"{name}_skyrest.json"
    rest = json.loads(rest_doc.read_text()) if rest_doc.exists() else {}
    rest_visible = {int(k) for k in rest.get("visible", {})}
    rest_fb = {int(k) for k in rest.get("fb", {})}
    rest_block = {int(k) for k in rest.get("blockers", {})}

    for node in bsp["nodes"]:
        ivp, isurf, nv = node[0], node[1], node[2]
        if nv < 3:
            continue
        s = bsp["surfs"][isurf]
        if s["flags"] & PF_INVISIBLE:
            continue
        package, tex, w, h, ds = B.resolve_tex(pkg, s["tex"], tex_sizes)
        mat = Package.qualkey(package, tex) + B.flag_suffix(s["flags"])
        is_sky = isurf in sky_ids
        if not is_sky:
            if not (isurf in rest_visible or isurf in rest_fb
                    or isurf in rest_block):
                continue
        else:
            if mat in split:
                mat += B.SKY_SUFFIX
            texflags[mat] = (package, tex, s["flags"])
            if s["flags"] & (PF_AUTOUPAN | PF_AUTOVPAN) and len(node) > 4:
                for z in tuple(node[4]):
                    if z and z < len(zone_names) and zone_names[z]:
                        pan_zones[mat][zone_names[z]] += 1
        base = pts[s["pbase"]]
        tu, tv = bsp["vectors"][s["vtu"]], bsp["vectors"][s["vtv"]]
        idx, uvs, poly = [], [], []
        for i in range(nv):
            pi = bsp["verts"][ivp + i][0]
            p = pts[pi]
            u = sum((a - b) * c for a, b, c in zip(p, base, tu)) + s["panu"]
            v = sum((a - b) * c for a, b, c in zip(p, base, tv)) + s["panv"]
            idx.append(pi)
            uvs.append((u / (w * ds), v / (h * ds)))
            poly.append(p)
        lmuv = None
        if atlas:
            aw, ah = atlas["size"]
            ent = atlas["surfs"].get(str(isurf)) if s.get("ilightmap", -1) >= 0 else None
            if ent:
                frac = L.lightmap_uv(bsp, isurf, poly)
                lmuv = [((ent["x"] + fu + 0.5) / aw, (ent["y"] + fv + 0.5) / ah)
                        for fu, fv in frac]
            else:
                fb = atlas["fullbright"]
                lmuv = [((fb["x"] + 0.5) / aw, (fb["y"] + 0.5) / ah)] * nv
        # Newell normal + winding agreement (same as bsp2obj)
        nx = ny = nz = 0.0
        for i in range(nv):
            (x1, y1, z1), (x2, y2, z2) = poly[i], poly[(i + 1) % nv]
            nx += (y1 - y2) * (z1 + z2); ny += (z1 - z2) * (x1 + x2); nz += (x1 - x2) * (y1 + y2)
        n = (nx, ny, nz)
        if is_sky:
            dest = mat
        elif isurf in rest_visible:
            dest = "SkyRest__" + mat
        elif isurf in rest_fb:
            dest = "SkyRestFB"
        else:
            dest = None
        if dest:
            for i in range(1, nv - 1):
                tri = (idx[0], idx[i], idx[i + 1])
                groups[dest].append((tri, (uvs[0], uvs[i], uvs[i + 1]), n,
                                     (lmuv[0], lmuv[i], lmuv[i + 1]) if lmuv else None))
        if not is_sky and isurf in rest_block:
            # solid-space blocker: same polys, flipped winding, so the face
            # rasterizes toward the sky origin and occludes opaque black.
            nneg = (-n[0], -n[1], -n[2])
            for i in range(1, nv - 1):
                tri = (idx[0], idx[i + 1], idx[i])
                groups["SkyRestBlock"].append(
                    (tri, (uvs[0], uvs[i + 1], uvs[i]), nneg,
                     (lmuv[0], lmuv[i + 1], lmuv[i]) if lmuv else None))

    # Sky origin/rotation + pan speeds from the SkyZoneInfo (+ any zone that
    # owns an auto-pan material).
    # UE1 ZoneInfo defaultproperties: TexU/VPanSpeed default to 1.0, and the T3D
    # omits default-valued props — so an ABSENT pan speed means 1.0, not 0.
    def num(a, k, d):
        v = a.get(k) if a else None
        return float(v) if v is not None else d
    sw = by_name.get(winner["name"], {})
    loc = sw.get("location") or sw.get("Location") or [0, 0, 0]
    rot = sw.get("rotation") or sw.get("Rotation") or [0, 0, 0]
    ox, oy, oz = (float(loc[0]), float(loc[1]), float(loc[2]))

    # Min distance from the sky origin to any FACE going into the mesh —
    # sky AND SkyRest copies (not vertex: a huge cloud sheet's interior
    # passes far closer to the pivot than its corners). setup_childsky.py
    # sizes SkyScale so the nearest point of ANY emitted surface lands
    # beyond the whole world's depth — else a plane slices through world
    # geometry (sky drawn in front of mountains).
    origin = np.array([ox, oy, oz], dtype=float)
    dists = [tri_dist(origin, *(np.array(pts[t[k]], dtype=float) for k in range(3)))
             for g in groups.values() for t, _, _, _ in g]

    # Emit GLB (bsp2obj convention: (x,z,y)/100, reversed winding) but RE-CENTER
    # on the sky origin so the mesh geometry hugs its own pivot. AWoTSkyBox
    # camera-locks the pivot to the view and scales up ~12x; a mesh whose verts
    # sat ~12000uu from the pivot fails to rasterize once scaled (precision/cull),
    # so the vertices are stored relative to the SkyZoneInfo origin.
    glb_groups = {}
    for mat in sorted(groups):
        pos, nrm, uv, ind, uv1 = [], [], [], [], ([] if atlas else None)
        for tri, uvs, n, lmuvs in groups[mat]:
            for k in (2, 1, 0):
                p = pts[tri[k]]
                pos += [(p[0] - ox) / 100.0, (p[2] - oz) / 100.0, (p[1] - oy) / 100.0]
                nrm += [n[0], n[2], n[1]]
                uv += [uvs[k][0], uvs[k][1]]
                if uv1 is not None:
                    uv1 += [lmuvs[k][0], lmuvs[k][1]]
                ind.append(len(ind))
        glb_groups[mat] = (pos, nrm, uv, ind) if uv1 is None else (pos, nrm, uv, ind, uv1)
    B.write_glb(out_dir / f"{name}_skygeo.glb", f"{name}_skygeo", glb_groups)
    pan_speeds = {}
    for zn in set(z for c in pan_zones.values() for z in c):
        za = by_name.get(zn, {})
        pan_speeds[zn] = dict(u=num(za, "TexUPanSpeed", 1.0),
                              v=num(za, "TexVPanSpeed", 1.0))

    def mat_entry(m, p, t, fl):
        e = dict(package=p, texture=t, flags=fl)
        if m in pan_zones:
            e["pan_zone"] = pan_zones[m].most_common(1)[0][0]
        return e

    # Empty by construction since the __SkyZone split (both converters route
    # shared-material sky faces to their own slot); kept as a safety net —
    # setup_childsky.py still refuses to hide anything listed here.
    shared = sorted(set(texflags) & world_mats)
    (out_dir / f"{name}_skygeo.json").write_text(json.dumps(dict(
        sky_zone=sky_zone, sky_actor=winner["name"],
        sky_origin=[float(x) for x in loc], sky_rotation=[float(x) for x in rot],
        min_surface_dist=float(min(dists)), max_surface_dist=float(max(dists)),
        world_shared=shared,
        sky_split=sorted(split),
        pan_speeds=pan_speeds,
        materials={m: mat_entry(m, *v) for m, v in texflags.items()},
        sky_rest=dict(visible=len(rest_visible), fb=len(rest_fb),
                      blockers=len(rest_block)) if rest else None,
    ), indent=1))
    print(f"{name}: sky zone {sky_zone} ({winner['name']}), {len(sky_ids)} surfs, "
          f"{len(groups)} materials, origin={[round(x) for x in loc]}, "
          f"pan_speeds={pan_speeds}"
          + (f", rest visible {len(rest_visible)} fb {len(rest_fb)} "
             f"block {len(rest_block)}" if rest else "")
          + (f", sky-split {sorted(split)}" if split else ""))


if __name__ == "__main__":
    convert_sky(*sys.argv[1:6])
