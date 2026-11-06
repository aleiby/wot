"""Bake the UE1 sky-zone backdrop to a lat-long panorama for UE5 injection.

Usage: python skybake.py <map.wot> <t3d_json> <conv_dir>

UE1 renders PF_FakeBackdrop surfaces as windows into a zone's
ZoneInfo.SkyZone: the view rotates in place at the SkyZoneInfo location
(the view coords divide out the sky zone's Rotation then Location) —
exactly an environment map captured at that point.

Winner selection (per zone, shipped spec): the renderer draws the sky of
GetZoneActor(iZone)->SkyZone where iZone is the FB surface's node zone on
the viewer side. Each zone's SkyZone comes from LinkToSkybox():
  - engine default (UE1 ZoneInfo): last SkyZoneInfo in actor order, then
    last with bHighDetail == bHighDetailMode (True on the golden config);
  - WOT override (exports/script/WOT/WOTZoneInfo.uc:131-141, applies to
    WOTZoneInfo/WaterZone/WaysZone zones): SkyZone = last SkyZoneInfo whose
    Tag matches the zone's Event; an EMPTY Event iterates ALL SkyZoneInfos
    (AllActors '' = no tag filter) leaving plain last-in-order.
Sky is therefore PER-ZONE; the UE5 importer binds ONE panorama per map, so
the baked winner is the sky serving the most FB nodes (counting each node's
nonzero-side zones — a viewer cannot stand in the null zone 0). Maps whose
zones link additional skies keep them recorded in <Map>_sky.json "links"
(known residual: minority zones show the majority sky).
SkyboxTrigger can relink at runtime; the bake is the level-start state.

The bake ray-casts the sky box's own BSP nodes (front/back zone == the
winner's zone index), sampling surface textures with UE1 display-space
compositing (each pass clamped at the framebuffer like retail):
  opaque      fb = tex
  translucent fb = tex + fb*(1-tex)          [ONE / ONE_MINUS_SRC_COLOR]
  modulated   fb = 2*tex*fb                  [DST_COLOR / SRC_COLOR]
Lit sky surfaces multiply the ground-truth composed lightmap
(lightmaps.compose_map) at the FULL retail range: display mult =
total/63.75, up to ~2.0 at a saturated 127 (the RGBA7-through-modulate-2x
overbright algebra, PORT_NOTES). The lightmap atlas is NOT
used here — its uint8 encode (min(total*ATLAS_SCALE, 255)) is a display
asset whose scale changed in the overbright rollout; composing from source
keeps the bake exact and atlas-independent. AutoU/VPan cloud scrolling is
baked at t=0 (PORT_NOTES).

Emits <conv_dir>/<Map>_sky.png (4096x2048 lat-long, v=0 at +Z) and
<Map>_sky.json {"file", "sky_zone", "actor", "links"}. SkyZoneInfo rotation
is baked into the panorama (sample directions pre-rotated).
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image

import lightmaps as L
from bsp2obj import load_tex_sizes

W, H = 4096, 2048
# Rays per batch. Each ray's hit stack and composite are independent, so
# batching is output-identical to a single pass; it exists to bound memory
# on maps whose sky zone has no opaque base (Battle3_02): there EVERY ray
# falls back against the whole map's nodes, and the single-pass hit
# accumulation + per-ray Python stacks (a tuple + ndarray per hit, tens of
# millions of hits) ran to tens of GB — silent OOM.
CHUNK = 262144
PF_TRANSLUCENT, PF_MODULATED, PF_MASKED = 0x4, 0x40, 0x2
PF_INVISIBLE, PF_UNLIT, PF_FAKEBACKDROP = 0x1, 0x400000, 0x80
PF_TWOSIDED = 0x100

WOT_ZONE_CLASSES = {"wotzoneinfo", "waterzone", "wayszone"}

_OBJREF = re.compile(r"'[\w\-]+\.([\w\-. ]+)'")


def _zone_number(actor):
    m = re.search(r"ZoneNumber=(\d+)", actor.get("props", {}).get("Region", ""))
    return int(m.group(1)) if m else None


def _load_tex(conv, package, name, cache, sizes):
    """Load tex/<package>/<name>.png — package-qualified (never a glob-first-wins
    pick across folders, which aliased same-named textures) AND case-canonical:
    a map's import table can disagree in case with the exported folder/stem
    (WOTDecorations vs WoTDecorations), so resolve the objref through the
    manifest lookup (bsp2obj.load_tex_sizes) before touching the path."""
    key = (package.lower(), name.lower())
    if key in cache:
        return cache[key]
    canon = sizes.get(key)
    if canon:
        package, name = canon[0], canon[1]
    p = conv / "tex" / package / f"{name}.png"
    hit = np.asarray(Image.open(p).convert("RGB"), dtype=np.float64) \
        if p.exists() else None
    cache[key] = hit
    return hit


def zone_sky_links(actors, pkg, bsp):
    """Per-zone LinkToSkybox winner (see module doc).

    -> (skies, [sky actor per zone index] aligned with bsp['zones'])."""
    skies = [a for a in actors if a["cls"].lower() == "skyzoneinfo"]
    if not skies:
        return [], []
    default = skies[-1]
    hd = [a for a in skies
          if a.get("props", {}).get("bHighDetail", "False") == "True"]
    if hd:
        default = hd[-1]
    amap = {a["name"].lower(): a for a in actors}
    links = []
    for zref in (bsp["zones"] or []):
        zname = pkg.objname(zref) if zref else None
        za = amap.get(str(zname).lower()) if zname else None
        sky = default
        if za and za["cls"].lower() in WOT_ZONE_CLASSES:
            event = za.get("props", {}).get("Event", "")
            if event:
                m = [s for s in skies
                     if s.get("props", {}).get("Tag", "").lower()
                     == event.lower()]
                if m:
                    sky = m[-1]
            else:
                sky = skies[-1]     # unfiltered AllActors: plain last
        links.append(sky)
    return skies, links


def pick_winner(bsp, skies, links):
    """Map winner = the sky serving the most FakeBackdrop nodes, each node
    counted for its nonzero-side zone(s) (zone 0 is the null zone — no
    viewer stands there; nodes with both sides null are unreachable)."""
    if not links:
        return skies[-1] if skies else None
    tally = {}
    for node in bsp["nodes"]:
        isurf, nv = node[1], node[2]
        iz = node[4] if len(node) > 4 else (0, 0)
        if nv < 3 or not (bsp["surfs"][isurf]["flags"] & PF_FAKEBACKDROP):
            continue
        for z in {z for z in iz if z != 0}:
            sky = links[z] if z < len(links) else links[0]
            tally[sky["name"]] = tally.get(sky["name"], 0) + 1
    if not tally:
        return skies[-1]
    byname = {s["name"]: s for s in skies}
    # deterministic: max count, ties broken by actor order (earliest wins)
    order = {s["name"]: i for i, s in enumerate(skies)}
    best = sorted(tally.items(), key=lambda kv: (-kv[1], order[kv[0]]))[0][0]
    return byname[best]


def _node_theta_bounds(nodes, bsp, pts, origin):
    """Conservative polar-angle range [theta_lo, theta_hi] of all directions
    from origin to each node's polygon. BSP polys are convex, so every hit
    direction lies in the conical hull of the corner directions, which fits
    inside the angular cap around the mean corner direction whenever the cap
    radius < 90 deg; the cap's theta span is theta(center) +/- radius (padded
    1e-3 rad >> fp error + the polygon test's 1e-6 tolerance sliver). Nodes
    without a valid cap get (0, pi) = never culled. Culling with these bounds
    cannot remove a hit, so chunk output is unchanged — it only skips nodes
    that cannot intersect a chunk's theta band (each CHUNK of row-major rays
    is a narrow band; without this the whole-map fallback re-sweeps every
    node for all 32 chunks)."""
    out = np.empty((len(nodes), 2))
    for i, (ivp, isurf, nv) in enumerate(nodes):
        poly = pts[[bsp["verts"][ivp + k][0] for k in range(nv)]]
        w = poly - origin[None, :]
        wl = np.linalg.norm(w, axis=1)
        if (wl < 1e-9).any():
            out[i] = (0.0, np.pi)
            continue
        u = w / wl[:, None]
        c = u.sum(axis=0)
        cl = np.linalg.norm(c)
        if cl < 1e-9:
            out[i] = (0.0, np.pi)
            continue
        c /= cl
        r = np.arccos(np.clip((u @ c).min(), -1.0, 1.0))
        if r > 1.55:                    # cap not convex approaching 90 deg
            out[i] = (0.0, np.pi)
            continue
        tc = np.arccos(np.clip(c[2], -1.0, 1.0))
        out[i] = (max(tc - r - 1e-3, 0.0), min(tc + r + 1e-3, np.pi))
    return out


def _cast_nodes(nodes, ray_ids, d, origin, bsp, pts, vecs):
    """Vectorized per-node plane+polygon test over rays d[ray_ids]; returns
    sparse hits [(isurf, gids, tvals, back)] with gids = GLOBAL ray indices
    (memory-safe for thousands of candidate nodes). back = the ray strikes
    the plane from BEHIND the UE1 surface normal (denom > 0) — the shell
    bake's solid-space terminator; the sky bake ignores it."""
    rays_d = d[ray_ids]
    hits = []
    for ivp, isurf, nv in nodes:
        s = bsp["surfs"][isurf]
        n_ = vecs[s["vnorm"]]
        base = pts[s["pbase"]]
        denom = rays_d @ n_
        with np.errstate(divide="ignore", invalid="ignore"):
            t = ((base - origin) @ n_) / denom
        ok = np.isfinite(t) & (t > 1.0)
        if not ok.any():
            continue
        sel = np.where(ok)[0]
        hitp = origin[None, :] + rays_d[sel] * t[sel, None]
        poly = pts[[bsp["verts"][ivp + k][0] for k in range(nv)]]
        ax = int(np.argmax(np.abs(n_)))
        keep = [i for i in range(3) if i != ax]
        p2 = poly[:, keep]
        h2 = hitp[:, keep]
        inside = np.ones(len(sel), dtype=bool)
        sign = None
        for i in range(nv):
            e = p2[(i + 1) % nv] - p2[i]
            w_ = h2 - p2[i][None, :]
            cr = e[0] * w_[:, 1] - e[1] * w_[:, 0]
            if sign is None:
                sign = np.where(cr >= 0, 1.0, -1.0)
            inside &= (cr * sign) >= -1e-6
        if inside.any():
            hits.append((isurf, ray_ids[sel[inside]], t[sel[inside]],
                         denom[sel[inside]] > 0))
    return hits


def bake(map_path, t3d_path, conv_dir):
    conv = Path(conv_dir)
    name = Path(map_path).stem
    pkg = L.Package(map_path)
    models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
    bsp = L.decode_model_full(pkg, max(models, key=lambda e: e["size"]))
    actors = json.load(open(t3d_path))["actors"]

    skies, links = zone_sky_links(actors, pkg, bsp)
    if not skies:
        (conv / f"{name}_sky.json").write_text(json.dumps({}))
        print(f"{name}: no sky zones")
        return
    winner = pick_winner(bsp, skies, links)
    link_doc = sorted({s["name"] for s in links})

    # zone index: the model's zone-actor table names each zone's ZoneInfo
    sky_zone = None
    for zi, zref in enumerate(bsp["zones"] or []):
        if zref and str(pkg.objname(zref)).lower() == winner["name"].lower():
            sky_zone = zi
            break
    if sky_zone is None:
        (conv / f"{name}_sky.json").write_text(json.dumps({}))
        print(f"{name}: SkyZoneInfo {winner['name']} not in zone table")
        return
    sky_actors = {winner["name"].lower(): (winner, sky_zone)}

    # ground-truth composed lightmaps (full 0..127 range; the atlas encode
    # would ceiling/rescale — see module doc)
    comp, _ = L.compose_map(map_path, t3d_path, ctx=(pkg, bsp))
    lm_ofs = {int(i): (int(o), int(uc), int(vc)) for i, o, uc, vc in zip(
        comp["surf_index"], comp["surf_texel_ofs"],
        comp["surf_uclamp"], comp["surf_vclamp"])}
    lm_texels = comp["texel_rgb"]           # float32 (N,3), 0..127

    # lat-long ray directions (v=0 at +Z), FVector world axes
    us = (np.arange(W) + 0.5) / W
    vs = (np.arange(H) + 0.5) / H
    phi = (us - 0.5) * 2.0 * np.pi          # yaw around +Z, 0 = +X
    theta = vs * np.pi                       # 0 = up
    st, ct = np.sin(theta), np.cos(theta)
    dirs = np.empty((H, W, 3))
    dirs[..., 0] = st[:, None] * np.cos(phi)[None, :]
    dirs[..., 1] = st[:, None] * np.sin(phi)[None, :]
    dirs[..., 2] = ct[:, None]

    pts = np.array(bsp["points"])
    vecs = np.array(bsp["vectors"])
    tex_cache = {}
    tex_sizes = load_tex_sizes(conv / "tex")

    for sky_name, (sky, sky_zone) in sky_actors.items():
        origin = np.array(sky["location"], dtype=np.float64)
        rot = sky.get("rotation") or [0.0, 0.0, 0.0]
        d = dirs.reshape(-1, 3)
        if any(rot):
            # bake SkyZone rotation into sample dirs (Coords /= Rotation)
            cy, sy = np.cos(rot[1] * 2 * np.pi / 65536), np.sin(rot[1] * 2 * np.pi / 65536)
            d = d.copy()
            d[:, 0], d[:, 1] = d[:, 0] * cy - d[:, 1] * sy, d[:, 0] * sy + d[:, 1] * cy

        # gather candidate nodes: front OR back zone matches the sky zone
        cand, cand_rest = [], []
        for node in bsp["nodes"]:
            ivp, isurf, nv = node[0], node[1], node[2]
            iz = node[4] if len(node) > 4 else (0, 0)
            if nv < 3:
                continue
            s = bsp["surfs"][isurf]
            if s["flags"] & PF_INVISIBLE:
                continue
            (cand if sky_zone in tuple(iz) else cand_rest).append(
                (ivp, isurf, nv))
        if not cand:
            continue

        nray = d.shape[0]

        def cast(nodes, ray_ids):
            return _cast_nodes(nodes, ray_ids, d, origin, bsp, pts, vecs)

        fb = np.zeros((nray, 3))
        rest_bounds = _node_theta_bounds(cand_rest, bsp, pts, origin)
        # bounded ray batches (see CHUNK): rays are fully independent, so
        # the per-chunk cast/fallback/composite is byte-identical to the
        # single-pass path while keeping peak memory constant.
        for c0 in range(0, nray, CHUNK):
            c1 = min(c0 + CHUNK, nray)
            ids = np.arange(c0, c1)
            # this chunk's rows span an exact theta band (row-major rays;
            # the yaw-only SkyZone rotation leaves theta untouched)
            band_lo, band_hi = theta[c0 // W], theta[(c1 - 1) // W]
            all_hits = cast(cand, ids)

            # UE1 sky pass = a CHILD FRAME rooted at the SkyZoneInfo's zone
            # (the renderer's CreateChildFrame with SkyZone->Region.
            # ZoneNumber — NOT "the whole level"). The fallback against the
            # rest of the map exists ONLY to give MODULATED sky layers a
            # multiply background: 2*tex*fb self-zeros over black, so Arena_04/
            # 07's modulated cloud planes must composite over the level
            # interior beyond them. TRANSLUCENT layers (c + fb*(1-c/255))
            # render correctly over black and need NO fallback — and pulling
            # in distant geometry under them is WRONG: Battle3_02's sky zone is
            # unsealed, so its translucent clouds were compositing the zone-63
            # citadel/nested-FakeBackdrop bleed (measured: whole-map fallback
            # washed +31G/+41B cyan; over-black matches golden's G/B, sky-mask
            # MAE 26.4->17.6; Arena_04 modulated fallback unchanged). Gate the
            # fallback on a modulated layer being present in the sky-zone stack.
            # (fallback_probe.py; PORT_NOTES "Skybox fallback: modulated-gated".)
            has_base = np.zeros(len(ids), dtype=bool)
            has_mod = np.zeros(len(ids), dtype=bool)
            for isurf, gids, tvals, _ in all_hits:
                s = bsp["surfs"][isurf]
                if not (s["flags"] & (PF_TRANSLUCENT | PF_MODULATED)):
                    has_base[gids - c0] = True
                if s["flags"] & PF_MODULATED:
                    has_mod[gids - c0] = True
            need = ~has_base & has_mod
            if need.any() and cand_rest:
                # theta-band cull (order-preserving; see _node_theta_bounds)
                sub = [n for n, (lo, hi) in zip(cand_rest, rest_bounds)
                       if hi >= band_lo and lo <= band_hi]
                if sub:
                    all_hits += cast(sub, ids[need])

            # composite per ray: nearest opaque base, translucent/mod over it
            # collect (t, kind, color) lazily: per-hit color only for rays hit
            ray_stack = [[] for _ in range(len(ids))]
            for isurf, idx, tvals, _ in all_hits:
                s = bsp["surfs"][isurf]
                ref = pkg.objref(s["tex"])
                if not ref:
                    continue
                tex = _load_tex(conv, ref[0], ref[1], tex_cache, tex_sizes)
                if tex is None:
                    continue
                hitp = origin[None, :] + d[idx] * tvals[:, None]
                tu, tv = vecs[s["vtu"]], vecs[s["vtv"]]
                uu = (hitp - base_of(bsp, s)[None, :]) @ tu + s["panu"]
                vv = (hitp - base_of(bsp, s)[None, :]) @ tv + s["panv"]
                th, tw = tex.shape[0], tex.shape[1]
                # bilinear (wrapped) — nearest sampling turned stars into blobs
                u0 = np.floor(uu - 0.5)
                v0 = np.floor(vv - 0.5)
                fu = (uu - 0.5 - u0)[:, None]
                fv = (vv - 0.5 - v0)[:, None]
                u0 = u0.astype(np.int64) % tw
                v0 = v0.astype(np.int64) % th
                u1 = (u0 + 1) % tw
                v1 = (v0 + 1) % th
                px = (tex[v0, u0] * (1 - fu) * (1 - fv) + tex[v0, u1] * fu * (1 - fv)
                      + tex[v1, u0] * (1 - fu) * fv + tex[v1, u1] * fu * fv)
                # lit sky surfaces: multiply the composed lightmap at the full
                # retail display range (total/63.75, up to ~2.0), clamped at the
                # framebuffer like each retail pass
                if not (s["flags"] & PF_UNLIT) and isurf in lm_ofs:
                    ofs, uc, vc = lm_ofs[isurf]
                    uvf = L.lightmap_uv(bsp, isurf, hitp)
                    axp = np.clip(np.floor(uvf[:, 0] + 0.5), 0,
                                  uc - 1).astype(np.int64)
                    ayp = np.clip(np.floor(uvf[:, 1] + 0.5), 0,
                                  vc - 1).astype(np.int64)
                    total = lm_texels[ofs + ayp * uc + axp].astype(np.float64)
                    px = np.clip(px * total / 63.75, 0.0, 255.0)
                kind = ("T" if s["flags"] & PF_TRANSLUCENT
                        else "M" if s["flags"] & PF_MODULATED else "O")
                for j, ridx in enumerate(idx):
                    ray_stack[ridx - c0].append((tvals[j], kind, px[j]))

            for lidx, stack in enumerate(ray_stack):
                if not stack:
                    continue
                stack.sort(key=lambda h: -h[0])       # far -> near
                col = np.zeros(3)
                started = False
                for _, kind, c in stack:
                    if kind == "O":
                        col = c
                        started = True
                    elif kind == "T":
                        col = np.clip(c + col * (1.0 - c / 255.0), 0, 255)
                        started = True
                    elif kind == "M" and started:
                        col = np.clip(2.0 * c * col / 255.0, 0, 255)
                fb[c0 + lidx] = np.clip(col, 0, 255)
            print(f"  chunk {c0 // CHUNK + 1}/{-(-nray // CHUNK)}",
                  file=sys.stderr, flush=True)

        covered = int((fb.sum(axis=1) > 0).sum())
        if covered == 0:
            # SkyZoneInfo outside its box (Arena_04/07 rig sky via modulated
            # portals instead) — leave the painted-portal rendering in place.
            (conv / f"{name}_sky.json").write_text(json.dumps({}))
            print(f"{name} sky zone {sky_zone} ({sky['name']}): 0 rays hit — "
                  f"skipped (no panorama written)")
            return
        img = fb.reshape(H, W, 3).astype(np.uint8)
        zn_out = f"{name}_sky.png"
        Image.fromarray(img).save(conv / zn_out)
        print(f"{name} sky zone {sky_zone} ({sky['name']}): "
              f"{len(cand)} nodes, {covered}/{nray} rays hit -> {zn_out}"
              + (f" [multi-sky links: {link_doc}]" if len(link_doc) > 1 else ""))
        (conv / f"{name}_sky.json").write_text(json.dumps(
            dict(file=zn_out, sky_zone=sky_zone, actor=sky["name"],
                 links=link_doc)))


def bake_rest(map_path, t3d_path, conv_dir):
    """Discover the rest-of-map surfaces a PORTAL-UNSEALED sky zone shows.

    Retail's sky pass is a child frame rooted at the SkyZoneInfo's zone;
    zone traversal continues through PORTAL surfaces into any nonzero
    opposite zone (PF_NoOcclude = translucent/masked/modulated pass
    through). Battle3_02's sky zone is geometrically CLOSED (measured here:
    every ray hits a sky-zone surface) but portal-unsealed: its 4
    translucent cloud sheets are PF_Portal faces, so retail draws THE REST
    OF THE MAP behind them (for B3_02: the map's
    outer Sky_Clou3670blu cloud dome) — the child-scene box shows void there
    today (the p99 R -41 bright tail).

    The remainder ships as LIVE GEOMETRY, not a baked panorama:
    skygeo.py re-emits the surfaces this cast finds visible into
    the sky mesh (SkyRest__* slots -> the world lightmap MICs, so panning /
    animated content stays live and sampling is native). This bake is the
    DISCOVERY instrument: it ray-casts the full sphere from the sky origin
    and classifies, per ray with no OPAQUE sky-zone base (an opaque base
    occludes anything behind it):
      - contributing surfaces (front-face opaque winner + the translucent/
        modulated layers in front of it)                -> "visible"
      - one-sided opaque surfaces struck from BEHIND    -> "blockers"
        (retail's traversal stops at solid volume; the flipped-winding black
        faces skygeo emits for these reproduce that occlusion)
      - FakeBackdrop windows (terminate the walk; retail re-enters the sky
        child frame there)                              -> "fb"
    PF_MASKED still composites as opaque (bake()'s standing limitation).
    Sealed maps (opaque sky base on every ray) write {"covered_frac": 1.0}
    and no surface lists, so skygeo.py adds nothing for them.
    Emits <Map>_skyrest.json (+ <Map>_skyrest_preview.png, a lat-long
    composite of exactly the discovered content — diagnostic only; an
    earlier cyan-wash fallback failed because it sampled nested-FB sky textures
    and had no solid-space rule).
    """
    conv = Path(conv_dir)
    name = Path(map_path).stem
    pkg = L.Package(map_path)
    models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
    bsp = L.decode_model_full(pkg, max(models, key=lambda e: e["size"]))
    actors = json.load(open(t3d_path))["actors"]

    # No-shell maps still get an EXPLICIT empty record: consumers already
    # treat {} as "probed, nothing there" (skygeo defaults, import_map's
    # missing-file warning goes quiet), and the bootstrap's completion
    # marker/census can then require the file unconditionally (Arena_03
    # broke the first full 19-map convert without this).
    def _write_empty_rest(reason):
        (conv / f"{name}_skyrest.json").write_text(
            json.dumps({"reason": reason}, indent=1))

    skies, links = zone_sky_links(actors, pkg, bsp)
    if not skies:
        print(f"{name}: no sky zones — no shell")
        _write_empty_rest("no sky zones")
        return
    winner = pick_winner(bsp, skies, links)
    sky_zone = None
    for zi, zref in enumerate(bsp["zones"] or []):
        if zref and str(pkg.objname(zref)).lower() == winner["name"].lower():
            sky_zone = zi
            break
    if sky_zone is None:
        print(f"{name}: SkyZoneInfo {winner['name']} not in zone table — no shell")
        _write_empty_rest("sky zone not in zone table")
        return

    comp, _ = L.compose_map(map_path, t3d_path, ctx=(pkg, bsp))
    lm_ofs = {int(i): (int(o), int(uc), int(vc)) for i, o, uc, vc in zip(
        comp["surf_index"], comp["surf_texel_ofs"],
        comp["surf_uclamp"], comp["surf_vclamp"])}
    lm_texels = comp["texel_rgb"]

    us = (np.arange(W) + 0.5) / W
    vs = (np.arange(H) + 0.5) / H
    phi = (us - 0.5) * 2.0 * np.pi
    theta = vs * np.pi
    st, ct = np.sin(theta), np.cos(theta)
    dirs = np.empty((H, W, 3))
    dirs[..., 0] = st[:, None] * np.cos(phi)[None, :]
    dirs[..., 1] = st[:, None] * np.sin(phi)[None, :]
    dirs[..., 2] = ct[:, None]

    pts = np.array(bsp["points"])
    vecs = np.array(bsp["vectors"])
    tex_cache = {}
    tex_sizes = load_tex_sizes(conv / "tex")

    origin = np.array(winner["location"], dtype=np.float64)
    rot = winner.get("rotation") or [0.0, 0.0, 0.0]
    d = dirs.reshape(-1, 3)
    if any(rot):
        # same pre-rotation as bake(): the shell samples WORLD directions
        # (M_WoT_Backdrop ignores the mesh transform), so the SkyZone yaw
        # must be baked into the panorama content.
        cy, sy = np.cos(rot[1] * 2 * np.pi / 65536), np.sin(rot[1] * 2 * np.pi / 65536)
        d = d.copy()
        d[:, 0], d[:, 1] = d[:, 0] * cy - d[:, 1] * sy, d[:, 0] * sy + d[:, 1] * cy

    cand, cand_rest = [], []
    for node in bsp["nodes"]:
        ivp, isurf, nv = node[0], node[1], node[2]
        iz = node[4] if len(node) > 4 else (0, 0)
        if nv < 3:
            continue
        s = bsp["surfs"][isurf]
        if s["flags"] & PF_INVISIBLE:
            continue
        (cand if sky_zone in tuple(iz) else cand_rest).append((ivp, isurf, nv))
    if not cand:
        print(f"{name}: no sky-zone nodes — no shell")
        return

    nray = d.shape[0]

    def cast(nodes, ray_ids):
        return _cast_nodes(nodes, ray_ids, d, origin, bsp, pts, vecs)

    fb = np.zeros((nray, 3))
    covered_total = 0
    visible = Counter()      # isurf -> rays it colors (winner or layer)
    blockers = Counter()     # isurf -> rays it terminates back-face
    fb_ids = Counter()       # isurf -> rays it terminates as a sky window
    rest_bounds = _node_theta_bounds(cand_rest, bsp, pts, origin)
    for c0 in range(0, nray, CHUNK):
        c1 = min(c0 + CHUNK, nray)
        ids = np.arange(c0, c1)
        band_lo, band_hi = theta[c0 // W], theta[(c1 - 1) // W]

        covered = np.zeros(len(ids), dtype=bool)
        for isurf, gids, _, _ in cast(cand, ids):
            if not (bsp["surfs"][isurf]["flags"] & (PF_TRANSLUCENT | PF_MODULATED)):
                covered[gids - c0] = True       # opaque sky base occludes the shell
        covered_total += int(covered.sum())
        hole = ~covered
        if not hole.any() or not cand_rest:
            print(f"  chunk {c0 // CHUNK + 1}/{-(-nray // CHUNK)} (sealed)",
                  file=sys.stderr, flush=True)
            continue

        sub = [n for n, (lo, hi) in zip(cand_rest, rest_bounds)
               if hi >= band_lo and lo <= band_hi]
        hits = cast(sub, ids[hole]) if sub else []

        ray_stack = [[] for _ in range(len(ids))]
        for isurf, idx, tvals, back in hits:
            s = bsp["surfs"][isurf]
            if s["flags"] & PF_FAKEBACKDROP:
                for j, ridx in enumerate(idx):
                    ray_stack[ridx - c0].append((tvals[j], "F", back[j], isurf))
                continue
            ref = pkg.objref(s["tex"])
            if not ref:
                continue
            tex = _load_tex(conv, ref[0], ref[1], tex_cache, tex_sizes)
            if tex is None:
                continue
            hitp = origin[None, :] + d[idx] * tvals[:, None]
            tu, tv = vecs[s["vtu"]], vecs[s["vtv"]]
            uu = (hitp - base_of(bsp, s)[None, :]) @ tu + s["panu"]
            vv = (hitp - base_of(bsp, s)[None, :]) @ tv + s["panv"]
            th, tw = tex.shape[0], tex.shape[1]
            u0 = np.floor(uu - 0.5)
            v0 = np.floor(vv - 0.5)
            fu = (uu - 0.5 - u0)[:, None]
            fv = (vv - 0.5 - v0)[:, None]
            u0 = u0.astype(np.int64) % tw
            v0 = v0.astype(np.int64) % th
            u1 = (u0 + 1) % tw
            v1 = (v0 + 1) % th
            px = (tex[v0, u0] * (1 - fu) * (1 - fv) + tex[v0, u1] * fu * (1 - fv)
                  + tex[v1, u0] * (1 - fu) * fv + tex[v1, u1] * fu * fv)
            if not (s["flags"] & PF_UNLIT) and isurf in lm_ofs:
                ofs, uc, vc = lm_ofs[isurf]
                uvf = L.lightmap_uv(bsp, isurf, hitp)
                axp = np.clip(np.floor(uvf[:, 0] + 0.5), 0,
                              uc - 1).astype(np.int64)
                ayp = np.clip(np.floor(uvf[:, 1] + 0.5), 0,
                              vc - 1).astype(np.int64)
                total = lm_texels[ofs + ayp * uc + axp].astype(np.float64)
                px = np.clip(px * total / 63.75, 0.0, 255.0)
            kind = ("T" if s["flags"] & PF_TRANSLUCENT
                    else "M" if s["flags"] & PF_MODULATED else "O")
            twosided = bool(s["flags"] & PF_TWOSIDED)
            for j, ridx in enumerate(idx):
                ray_stack[ridx - c0].append(
                    (tvals[j], kind, back[j], twosided, px[j], isurf))

        # near->far walk to the terminator, then the usual far->near algebra
        for lidx, stack in enumerate(ray_stack):
            if not stack:
                continue
            stack.sort(key=lambda h: h[0])
            layers = []
            colbase = np.zeros(3)          # void stays black
            for h in stack:
                kind = h[1]
                if kind == "F":
                    fb_ids[h[3]] += 1
                    break                  # sky re-entry window
                _, _, back, twosided, c, isurf = h
                if back and not twosided:
                    if kind == "O":
                        blockers[isurf] += 1
                        break              # solid space behind: void
                    continue               # one-sided T/M seen from behind
                if kind == "O":
                    colbase = c
                    visible[isurf] += 1
                    break
                layers.append((kind, c, isurf))
            col = colbase
            for kind, c, isurf in reversed(layers):
                visible[isurf] += 1
                if kind == "T":
                    col = np.clip(c + col * (1.0 - c / 255.0), 0, 255)
                else:
                    col = np.clip(2.0 * c * col / 255.0, 0, 255)
            fb[c0 + lidx] = np.clip(col, 0, 255)
        print(f"  chunk {c0 // CHUNK + 1}/{-(-nray // CHUNK)} "
              f"({int(hole.sum())} hole rays)", file=sys.stderr, flush=True)

    covered_frac = covered_total / float(nray)
    if covered_total == nray:
        (conv / f"{name}_skyrest.json").write_text(json.dumps(
            dict(sky_zone=sky_zone, actor=winner["name"], covered_frac=1.0)))
        print(f"{name} sky zone {sky_zone} ({winner['name']}): SEALED "
              f"(opaque sky base on every ray) — no rest set")
        return
    img = fb.reshape(H, W, 3).astype(np.uint8)
    preview = f"{name}_skyrest_preview.png"
    Image.fromarray(img).save(conv / preview)
    (conv / f"{name}_skyrest.json").write_text(json.dumps(dict(
        sky_zone=sky_zone, actor=winner["name"],
        covered_frac=round(covered_frac, 6),
        visible={str(k): v for k, v in sorted(visible.items())},
        blockers={str(k): v for k, v in sorted(blockers.items())},
        fb={str(k): v for k, v in sorted(fb_ids.items())},
        preview=preview), indent=1))
    print(f"{name} sky zone {sky_zone} ({winner['name']}): covered "
          f"{covered_frac:.4f}, {nray - covered_total}/{nray} portal rays; "
          f"visible {len(visible)} surfs, blockers {len(blockers)}, "
          f"fb {len(fb_ids)} -> {name}_skyrest.json")


def base_of(bsp, s):
    return np.array(bsp["points"][s["pbase"]])


if __name__ == "__main__":
    if "--rest" in sys.argv:
        a = [x for x in sys.argv[1:] if x != "--rest"]
        bake_rest(a[0], a[1], a[2])
    else:
        bake(sys.argv[1], sys.argv[2], sys.argv[3])
