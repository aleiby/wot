"""UE1 per-surface lightmap decoder + ground-truth composer (WoT retail 333B).

Usage: python lightmaps.py <map.wot> <t3d_json> <out.npz>

Decodes the UModel lightmap tail (FLightMapIndex / LightBits / Lights) that
ue1pkg.decode_model stops short of, then composes each surface's lightmap
exactly as the retail renderer did (the pre-radiosity, package file
version < 69 on-disk layout, which is what version-68 retail packages
contain; the runtime math is identical between branches).

Engine lighting facts the composer reimplements (retail parity):
- World light radius = 25.0 * ((int)LightRadius + 1)
  ... except Legend's SunLight: 250.0 * (LightRadius+1)
- RRadiusMult = 4093.0 / Radius^2; gate SqrtOfs < 4096
- LightSqrt[i] = (2S^3 - 3S^2 + 1)/S, S = sqrt((i+1)/4096)
- Diffuse = |((LightLoc - Origin) . vNormal)| / Radius, with vNormal AS STORED
  (not renormalized)
- HSV→RGB: Brightness = clamp(1.4*V/255 * 0.70/(0.01+sqrt(1.4*V/255))),
  piecewise hue triangle, color = (Hue + S/255*(1-Hue))*Brightness — saturation
  is "inverted" (255 = white ... actually 255 = full color: S/255 blends TOWARD
  white)
- FloatColor = hsv(Hue,Sat,255) * (LightBrightness/255) * LevelInfo.Brightness
- Per-light palette: contrib_c(i) = min((i * floor(FloatColor_c*65536)) >> 16, 127)
- Merge: per-channel add with saturation at 0x7F (RGBA7)
- Merge-time effect darkening: lights with LightEffect TorchWaver/FireWaver/
  WateryShimmer have every illumination byte scaled by a per-texel random
  factor before the palette lookup — floor(byte * (base + amp*rand)), rand
  uniform 0..1 refreshed per tick: Torch 0.95+0.05r, Fire 0.80+0.20r,
  Watery 0.60+0.40r (time-average factors 0.975 / 0.90 / 0.80)
- Shadow-mask filter: 3x3 kernel [[24,40,24],[40,64,40],[24,40,24]],
  edge-replicated, each kernel ROW's accumulation scaled by floor(acc*255/320)
  separately, the three row results summed (so a fully lit interior texel =
  70+114+70 = 254, not 255)
- Shadow bitmask: 1 bit/texel, LSB-first, rows padded to ShadowMaskU =
  (UClamp+7)>>3 bytes; light i of a surface at LightBits[DataOffset +
  i*ShadowMaskSpace], ShadowMaskSpace = ShadowMaskU*VClamp
- Radial (point) path: retail's integer forward-differencing path: the six
  quadratic coefficients are round(f * Scale) with Scale = RRadiusMult*4096,
  texel value = Hecker-rounded(Src * Diffuse * LightSqrt[Inner0>>12]) when
  Src!=0 and (DWORD)Inner0 < 4096*4096
- Non-incidence path: floor(Src * (1.02 - dist/Radius)), same distance gate,
  no Diffuse factor
- Spotlight path (LE_StaticSpot AND LE_Spotlight both dispatch here): cone
  axis = light Rotation forward vector; Sine = 1-LightCone/256; texel lit iff
  VdotV > 0 and VdotV^2 > Sine^2*|V|^2, then value =
  floor(((VdotV/|V| - Sine)/(1-Sine))^2 * Src * Diffuse * LightSqrt[SqrtOfs]),
  SqrtOfs = round(|V|^2 * RRadiusMult) gated < 4096
- Rotation forward vector = (cos(Pitch)*cos(Yaw), cos(Pitch)*sin(Yaw),
  sin(Pitch)) — roll cancels; UE1 trig quantizes angles to a 16384-entry
  table via (i>>2)&16383
- Per-light clip rect (MinU..MaxU/MinV..MaxV) from the light sphere's plane
  projection * 1.05 slack
- Static lightmap = zone ambient seed + sum of static light merges. Seed per
  channel = floor(hsv(AmbientHue, AmbientSaturation, AmbientBrightness)_c
  * 64) (ambient *0.25, packed clamp(floor(c*256))). Surface zone = majority
  vote of node iZone[1] (front) over the surface's nodes, back-side fallback;
  zone actor from the surface's zone properties, LevelInfo when none.

Deliberate simplifications (documented divergences from bit-exactness):
- Fast sqrt approximation -> exact sqrt (its mantissa-table error ~1e-3 only
  reaches the NonIncidence falloff and the clip rect, which has 2.4% slack).
- The Hecker float->byte trick rounds half-to-even and WRAPS mod 256; values
  provably stay <= ~254.2 in the static path, we round-half-even + clamp 0..255.
- TorchWaver/FireWaver/WateryShimmer merge-time flicker composes at its
  time-average factor (Torch 0.975, Fire 0.90, Watery 0.80), folded into the
  light's float_color — the merge scales the illumination magnitude uniformly
  in expectation, so scaling the palette slope is the equivalent static bake.
  This literal mean remains the frozen base. Centered-response endpoints use
  retail's exact floor(IlluminationByte*q) ordering; the per-texel random
  driver itself belongs to the runtime material path.
- The spotlight path's fast 1/sqrt approximation -> exact 1/sqrt (same class of
  divergence as the sqrt item above).
"""
import json
import os
import sys
import re
import struct
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import devtree_dir, exports_dir  # noqa: E402

try:
    import numpy as np
except ModuleNotFoundError:
    # UE's embedded Python has no numpy. import_map.py imports the pure
    # resolver helpers (resolve_light / world_light_radius / class_defaults)
    # from this module; only the lightmap composition/decoding paths need
    # numpy, and they fail loudly below if ever reached without it. A hard
    # module-level numpy import silently breaks EVERY in-editor map import.
    np = None

sys.path.insert(0, str(Path(__file__).parent))
from light_animation import (  # noqa: E402
    SHIMMER_EFFECTS,
    WAVEFORM_SHAPES,
    WAVE_TYPES,
)
from ue1pkg import Package, PayloadReader  # noqa: E402

EXPORTS = Path(os.environ.get("WOT_EXPORTS_DIR", exports_dir(required=False)))
ATLAS_ENCODE_SCALE = 2
MAX_ENDPOINT_ERROR_ATLAS_BYTES = 3

# ---------------------------------------------------------------------------
# Extended UModel decoding (keeps iLightMap + parses the lightmap tail).
# Copied from ue1pkg.decode_model rather than modifying it (bsp2obj depends
# on the shared decoder's exact output).
# ---------------------------------------------------------------------------


def decode_model_full(pkg, export, verbose=False):
    r = PayloadReader(pkg, export)
    r.properties()
    r.o += 25 + 16                    # FBox(25) + FSphere(16) — UPrimitive
    vectors = [r.fvec() for _ in range(r.cindex())]
    points = [r.fvec() for _ in range(r.cindex())]
    nodes = []
    node_planes = []                  # full FPlane (X,Y,Z,W) parallel to nodes
    node_children = []                # (iBack, iFront) parallel to nodes; -1=leaf
    node_leaves = []                  # iLeaf[0]=back, iLeaf[1]=front; -1=none
    node_flags = []                   # FBspNode.NodeFlags byte parallel to nodes
    for _ in range(r.cindex()):
        plane = struct.unpack_from("<4f", r.d, r.o)
        r.o += 16 + 8                 # Plane, ZoneMask
        node_flags.append(r.u8())     # NodeFlags
        ivp, isurf = r.cindex(), r.cindex()
        iback, ifront = r.cindex(), r.cindex()
        for _ in range(3):            # iPlane,iCollB,iRendB
            r.cindex()
        izone = (r.u8(), r.u8())      # iZone[0]=back, iZone[1]=front
        nv = r.u8()
        node_leaves.append((r.i32(), r.i32()))   # iLeaf[2] (UE1 FBspNode)
        nodes.append((ivp, isurf, nv, plane[:3], izone))
        node_planes.append(plane)
        node_children.append((iback, ifront))
    surfs = []
    for _ in range(r.cindex()):
        tex = r.cindex()
        flags = r.u32()
        pbase, vnorm, vtu, vtv = (r.cindex() for _ in range(4))
        ilightmap = r.cindex()
        r.cindex()                    # iBrushPoly
        panu = struct.unpack_from("<h", r.d, r.o)[0]
        panv = struct.unpack_from("<h", r.d, r.o + 2)[0]
        r.o += 4
        r.cindex()                    # Actor
        surfs.append(dict(tex=tex, flags=flags, pbase=pbase, vnorm=vnorm,
                          vtu=vtu, vtv=vtv, panu=panu, panv=panv,
                          ilightmap=ilightmap))
    verts = []
    for _ in range(r.cindex()):
        verts.append((r.cindex(), r.cindex()))

    # ---- tail (UModel format, package file version < 69 path) -------------
    num_shared_sides = r.i32()
    num_zones = r.i32()
    if not (0 <= num_zones <= 64):
        raise ValueError(f"NumZones={num_zones} implausible @0x{r.o - 4:x}")
    zones = []
    for _ in range(num_zones):
        zactor = r.cindex()           # object ref
        r.o += 16                     # Connectivity(8) + Visibility(8)
        zones.append(zactor)
    polys = r.cindex()                # object ref

    lm_n = r.cindex()
    lightmap = []
    for i in range(lm_n):
        data_offset = r.i32()
        pan = r.fvec()
        uclamp, vclamp = r.cindex(), r.cindex()
        uscale = struct.unpack_from("<f", r.d, r.o)[0]
        vscale = struct.unpack_from("<f", r.d, r.o + 4)[0]
        r.o += 8
        ilightactors = r.i32()
        if not (1 <= uclamp <= 1024 and 1 <= vclamp <= 1024):
            raise ValueError(f"LightMap[{i}] clamp {uclamp}x{vclamp}")
        if not (0.1 <= uscale <= 65536 and 0.1 <= vscale <= 65536):
            raise ValueError(f"LightMap[{i}] scale {uscale}x{vscale}")
        lightmap.append(dict(data_offset=data_offset, pan=pan, uclamp=uclamp,
                             vclamp=vclamp, uscale=uscale, vscale=vscale,
                             ilightactors=ilightactors))

    lb_n = r.cindex()
    lightbits = r.d[r.o:r.o + lb_n]
    r.o += lb_n
    bounds_n = r.cindex()
    r.o += 25 * bounds_n              # FBox each
    lh_n = r.cindex()
    r.o += 4 * lh_n                   # INT each
    leaves = []
    for _ in range(r.cindex()):
        izone, iperm, ivol = r.cindex(), r.cindex(), r.cindex()
        r.o += 8                      # VisibleZones
        leaves.append((izone, iperm, ivol))
    lights = [r.cindex() for _ in range(r.cindex())]
    root_outside, linked = r.i32(), r.i32()
    if r.o != r.end:
        raise ValueError(f"tail mismatch: ended @0x{r.o:x}, export ends 0x{r.end:x}")
    for i, lm in enumerate(lightmap):
        if lm["ilightactors"] >= len(lights):
            raise ValueError(f"LightMap[{i}].iLightActors {lm['ilightactors']} "
                             f">= {len(lights)} lights")
        if not (0 <= lm["data_offset"] <= len(lightbits)):
            raise ValueError(f"LightMap[{i}].DataOffset {lm['data_offset']} "
                             f"outside LightBits[{len(lightbits)}]")
    if verbose:
        print(f"  model: {len(vectors)} vectors, {len(points)} points, "
              f"{len(nodes)} nodes, {len(surfs)} surfs, {len(verts)} verts")
        print(f"  tail: NumSharedSides={num_shared_sides} zones={num_zones} "
              f"lightmaps={len(lightmap)} lightbits={len(lightbits)} "
              f"bounds={bounds_n} leafhulls={lh_n} leaves={len(leaves)} "
              f"lights={len(lights)} RootOutside={root_outside} Linked={linked}")
    return dict(vectors=vectors, points=points, nodes=nodes, surfs=surfs,
                verts=verts, zones=zones, lightmap=lightmap,
                lightbits=np.frombuffer(bytes(lightbits), dtype=np.uint8),
                leaves=leaves, lights=lights, node_planes=node_planes,
                node_children=node_children, node_leaves=node_leaves,
                node_flags=node_flags, root_outside=root_outside)


# ---------------------------------------------------------------------------
# Class-default resolution for light properties (shipped .uc = spec; Engine
# script is stripped in retail so Engine classes use UE1's defaults).
# ---------------------------------------------------------------------------

LIGHT_PROPS = ("LightType", "LightEffect", "LightBrightness", "LightHue",
               "LightSaturation", "LightRadius", "LightPeriod", "LightPhase",
               "LightCone", "bSpecialLit", "bStatic", "bNoDelete",
               "bDynamicLight")

# Animated light types composed at their waveform time-average: UE1
# recomposes lightmaps per frame with a global-lighting amplitude table;
# a static golden catches an arbitrary phase, so the mean is the faithful
# static stand-in. Runtime animation is a feel-pass follow-up (PORT_NOTES).
#   LT_Pulse:       B *= 0.6 + 0.39*sin(...)  -> 0.6
#   LT_SubtlePulse: B *= 0.9 + 0.09*sin(...)  -> 0.9
# Blink/Flicker/Strobe/TexturePalette* stay skipped (absent from MP maps).
LIGHT_TYPE_MEAN = {"LT_Steady": 1.0, **{
    light_type: shape[0] for light_type, shape in WAVEFORM_SHAPES.items()
}}
# (base, amplitude) of the sinusoids — hi/lo = base ± amp
LIGHT_TYPE_WAVE = WAVEFORM_SHAPES

# Shimmer/waver EFFECT darkening, composed at its time-average: the retail
# light merge scales each illumination byte by a per-texel random factor
# (base + amp*rand, rand uniform 0..1 per tick) before the palette lookup;
# the static-bake stand-in is the factor midpoint. Constant across the
# hi/lo wave variants — those model the LightType sinusoid, and this
# random flicker averages out identically at either endpoint.
LIGHT_EFFECT_MEAN = {"LE_TorchWaver": 0.975, "LE_FireWaver": 0.90,
                     "LE_WateryShimmer": 0.80}
# Half-ranges about those means. UE1's light manager applies
# Torch=0.95+0.05u, Fire=0.80+0.20u, Water=0.60+0.40u, so u=0/1 is
# mean-amplitude / mean+amplitude. Keeping these separate from the mean
# constants protects the literal mean bake's operation order and float values.
LIGHT_EFFECT_AMPLITUDE = {"LE_TorchWaver": 0.025, "LE_FireWaver": 0.10,
                          "LE_WateryShimmer": 0.20}


def _wave_scale(light_type, wave):
    if light_type not in LIGHT_TYPE_WAVE or wave == "mean":
        return LIGHT_TYPE_MEAN.get(light_type, 1.0)
    base, amp = LIGHT_TYPE_WAVE[light_type]
    return base + amp if wave == "hi" else base - amp


def waveform_group_key(resolved):
    """Exact per-actor waveform identity used by the response manifest."""
    return (resolved["LightType"], int(resolved["LightPeriod"]),
            int(resolved["LightPhase"]))


def canonical_light_animation(resolved):
    """Canonicalize binary enum bytes through zones.py's shipped tables.

    T3D normally carries enum names, while package-property rows may carry
    their serialized byte values. zones.light_anim_row is
    the single shipped-333B enum authority (not the later LT_Scripted
    ordering), so the baker reuses it instead of duplicating an enum table.
    """
    light_type = str(resolved["LightType"])
    light_effect = str(resolved["LightEffect"])
    if not light_type.isdigit() and not light_effect.isdigit():
        return resolved
    from zones import light_anim_row  # local import: zones imports lightmaps
    light_type, period, phase, light_effect = light_anim_row(resolved)
    out = dict(resolved)
    out.update(LightType=light_type, LightPeriod=period, LightPhase=phase,
               LightEffect=light_effect)
    return out


def _selected_wave_scale(resolved, endpoint, group):
    """Wave scale for one bake state; unrelated groups stay at their mean.

    ``group is None`` retains the legacy all-wave endpoint behavior used
    by the census tool. Response baking always supplies an exact group.
    """
    if group is not None and waveform_group_key(resolved) != tuple(group):
        endpoint = "mean"
    return _wave_scale(resolved["LightType"], endpoint)


def _selected_effect_scale(resolved, endpoint, effect_class):
    """FloatColor scale; selected shimmer endpoints run in Merge instead."""
    effect = resolved["LightEffect"]
    if effect not in LIGHT_EFFECT_MEAN:
        return 1.0
    if effect_class is not None and effect == effect_class \
            and endpoint != "mean":
        return 1.0
    return LIGHT_EFFECT_MEAN[effect]


def _apply_selected_effect(illum, resolved, endpoint, effect_class):
    """Apply an exact UE1 shimmer endpoint to illumination bytes.

    The literal mean bake intentionally retains the landed approximation that
    folds q_mean into FloatColor. For a selected u=0/1 response endpoint,
    retail instead floors ``IlluminationByte*q`` before the fixed-point palette
    lookup (FLightManager::Merge); that ordering matters.
    """
    effect = resolved["LightEffect"]
    if effect_class is None or effect != effect_class or endpoint == "mean":
        return illum
    q = LIGHT_EFFECT_MEAN[effect]
    amplitude = LIGHT_EFFECT_AMPLITUDE[effect]
    q = q + amplitude if endpoint == "hi" else q - amplitude
    return np.floor(illum.astype(np.float32) * np.float32(q)).astype(np.uint8)


def animation_manifest(map_name, actors, zone_data=None):
    """Build the deterministic map-level centered-response manifest.

    ``actors`` is the T3D actor list. When the checked-in zones export is
    supplied, every active animation row is cross-checked against its
    ``light_anim`` row before any response is baked.
    """
    # T3D can contain duplicate object records; package/export consumers use
    # the last one, matching the zones census and schema tests.
    by_name = {actor["name"]: actor for actor in actors}
    lightning_tags = {
        str(actor["props"].get("LightTag", "")).lower()
        for actor in by_name.values() if is_a(actor["cls"], "Lightning")
    } - {""}

    active = {}
    for name, actor in by_name.items():
        rec = canonical_light_animation(resolve_light(actor))
        if int(rec["LightBrightness"]) <= 0:
            continue
        if rec["LightType"] not in LIGHT_TYPE_MEAN:
            continue
        if rec["LightEffect"] not in SUPPORTED_EFFECTS:
            continue
        if str(actor["props"].get("Tag", "")).lower() in lightning_tags:
            continue
        active[name] = rec

    relevant = {
        name: rec for name, rec in active.items()
        if rec["LightType"] in WAVE_TYPES
        or rec["LightEffect"] in SHIMMER_EFFECTS
    }
    if zone_data is not None:
        names = zone_data["light_actors"]
        props = zone_data["light_props"]
        anim = zone_data["light_anim"]
        if not (len(names) == len(props) == len(anim)):
            raise ValueError(f"{map_name}: zones light tables are not parallel")
        zone_relevant = {}
        for name, light_props, row in zip(names, props, anim):
            light_type, period, phase, effect = row
            if not light_props[3] or light_type not in LIGHT_TYPE_MEAN:
                continue
            if effect not in SUPPORTED_EFFECTS:
                continue
            if light_type in WAVE_TYPES or effect in SHIMMER_EFFECTS:
                zone_relevant[name] = [light_type, int(period), int(phase),
                                       effect]
        resolved_rows = {
            name: [rec["LightType"], int(rec["LightPeriod"]),
                   int(rec["LightPhase"]), rec["LightEffect"]]
            for name, rec in relevant.items()
        }
        if resolved_rows != zone_relevant:
            missing = sorted(set(zone_relevant) - set(resolved_rows))
            extra = sorted(set(resolved_rows) - set(zone_relevant))
            changed = sorted(
                name for name in set(resolved_rows) & set(zone_relevant)
                if resolved_rows[name] != zone_relevant[name])
            raise ValueError(
                f"{map_name}: light_anim cross-check failed "
                f"(missing={missing}, extra={extra}, changed={changed})")

    group_names = {}
    effect_names = {}
    intersection_names = {}
    for name, rec in relevant.items():
        group = (waveform_group_key(rec)
                 if rec["LightType"] in WAVE_TYPES else None)
        effect = (rec["LightEffect"]
                  if rec["LightEffect"] in SHIMMER_EFFECTS else None)
        if group is not None:
            group_names.setdefault(group, []).append(name)
        if effect is not None:
            effect_names.setdefault(effect, []).append(name)
        if group is not None and effect is not None:
            intersection_names.setdefault((group, effect), []).append(name)

    groups = []
    for key in sorted(group_names):
        mean, amplitude = LIGHT_TYPE_WAVE[key[0]]
        light_names = sorted(group_names[key])
        groups.append(dict(key=key, mean=mean, amplitude=amplitude,
                           light_names=light_names,
                           light_count=len(light_names)))

    terms = []

    def add_term(**fields):
        light_names = sorted(fields.pop("light_names"))
        slot = len(terms)
        terms.append(dict(**fields, light_names=light_names,
                          light_count=len(light_names),
                          response_png=(
                              f"{map_name}_lmatlas_response_{slot}.png")))

    for key in sorted(group_names):
        add_term(kind="waveform", group=key,
                 light_names=group_names[key])
    for effect in SHIMMER_EFFECTS:
        if effect not in effect_names:
            continue
        add_term(kind="effect", effect=effect,
                 effect_mean=LIGHT_EFFECT_MEAN[effect],
                 effect_amplitude=LIGHT_EFFECT_AMPLITUDE[effect],
                 light_names=effect_names[effect])
    effect_order = {effect: index
                    for index, effect in enumerate(SHIMMER_EFFECTS)}
    for key, effect in sorted(
            intersection_names,
            key=lambda item: (item[0], effect_order[item[1]])):
        add_term(kind="intersection", group=key, effect=effect,
                 effect_mean=LIGHT_EFFECT_MEAN[effect],
                 effect_amplitude=LIGHT_EFFECT_AMPLITUDE[effect],
                 light_names=intersection_names[(key, effect)])
    return dict(groups=groups, terms=terms)
ZONE_PROPS = ("AmbientBrightness", "AmbientHue", "AmbientSaturation")
PARSE_PROPS = LIGHT_PROPS + ZONE_PROPS
BASE_DEFAULTS = dict(LightType="LT_None", LightEffect="LE_None",
                     LightBrightness=0, LightHue=0, LightSaturation=0,
                     # LightCone: UE1 Actor sets NO default (engine
                     # zero-init = 0); Engine Light and shipped
                     # Legend/SunLight.uc both set LightCone=128 — picked up
                     # by the .uc chain walk below
                     LightRadius=0, LightPeriod=0, LightPhase=0, LightCone=0,
                     bSpecialLit=False,
                     bStatic=False, bNoDelete=False, bDynamicLight=False,
                     # UE1 ZoneInfo defaultproperties sets AmbientSaturation=255
                     # (every zone actor chains through it); Brightness/Hue are
                     # engine zero-init
                     AmbientBrightness=0, AmbientHue=0, AmbientSaturation=255)

_uc_cache = {}


def _find_uc(cls):
    roots_and_patterns = (
        (EXPORTS, f"script/*/{cls}.uc"),
        (devtree_dir(required=False), f"U2Radiosity/*/Classes/{cls}.uc"),
    )
    for root, pattern in roots_and_patterns:
        hits = sorted(root.glob(pattern))
        if hits:
            return hits[0]
    return None


def _parse_uc(cls):
    """→ (parent_class_or_None, {prop: value}) for PARSE_PROPS set in
    defaultproperties."""
    key = cls.lower()
    if key in _uc_cache:
        return _uc_cache[key]
    path = _find_uc(cls)
    parent, props = None, {}
    if path:
        text = path.read_text(errors="replace")
        m = re.search(r"^\s*class\s+(\w+)\s+(?:expands|extends)\s+(\w+)",
                      text, re.M | re.I)
        if m:
            parent = m.group(2)
        dp = re.search(r"^defaultproperties\s*\{(.*?)^\}", text,
                       re.M | re.S | re.I)
        if dp:
            for line in dp.group(1).splitlines():
                mm = re.match(r"\s*(\w+)\s*=\s*([^/\s]+)", line)
                if mm and mm.group(1) in PARSE_PROPS:
                    props[mm.group(1)] = mm.group(2).strip()
    _uc_cache[key] = (parent, props)
    return parent, props


def class_defaults(cls):
    """Accumulated defaults walking the superclass chain."""
    out = dict(BASE_DEFAULTS)
    chain, seen = [], set()
    c = cls
    while c and c.lower() not in seen and c.lower() != "object":
        seen.add(c.lower())
        chain.append(c)
        c = _parse_uc(c)[0]
    for c in reversed(chain):          # base-most first so leaves override
        for k, v in _parse_uc(c)[1].items():
            out[k] = _coerce(k, v)
    return out


def _coerce(prop, val):
    if isinstance(val, (int, float, bool)):
        return val
    val = str(val).strip().strip('"')
    if prop.startswith("b"):
        return val.lower() == "true"
    if prop in ("LightType", "LightEffect"):
        return val
    try:
        return int(float(val.replace("+", "")))
    except ValueError:
        return val


def resolve_light(actor):
    """t3d actor dict → resolved light property dict."""
    p = class_defaults(actor["cls"])
    for k in LIGHT_PROPS:
        if k in actor["props"]:
            p[k] = _coerce(k, actor["props"][k])
    # Plain-tuple fallback keeps this resolver usable in UE's numpy-less
    # embedded Python (import_map.py); numpy callers get the ndarray they
    # always did.
    p["location"] = (np.array(actor["location"], dtype=np.float64)
                     if np is not None
                     else tuple(float(x) for x in actor["location"]))
    # t3d.py emits rotation=[pitch,yaw,roll] in UE1 units, zeros when the T3D
    # has no Rotation= line (engine zero-init)
    p["rotation"] = actor.get("rotation", [0.0, 0.0, 0.0])
    p["name"] = actor["name"]
    p["cls"] = actor["cls"]
    return p


def is_a(cls, ancestor):
    """True if cls == ancestor or inherits from it (via .uc chain)."""
    c, seen = cls, set()
    while c and c.lower() not in seen:
        if c.lower() == ancestor.lower():
            return True
        seen.add(c.lower())
        c = _parse_uc(c)[0]
    return False


def world_light_radius(cls, light_radius):
    """World light radius = 25.0*(LightRadius+1) — EXCEPT Legend's SunLight,
    which overrides the multiplier to 250.0. Only override in the codebase."""
    mult = 250.0 if is_a(cls, "SunLight") else 25.0
    return mult * (int(light_radius) + 1)


def ambient_seed(actor):
    """Zone-ambient static map seed, RGBA7 units (0..64 per channel).

    Zone ambient = HSV→RGB of the zone's ambient HSB, scaled by 0.25, then
    packed clamp(floor(c*256),0,255) → floor(hsv_c*64).
    actor: the zone's t3d actor (ZoneInfo subclass or LevelInfo)."""
    p = class_defaults(actor["cls"])
    for k in ZONE_PROPS:
        if k in actor["props"]:
            p[k] = _coerce(k, actor["props"][k])
    col = fget_hsv(int(p["AmbientHue"]), int(p["AmbientSaturation"]),
                   int(p["AmbientBrightness"]))
    return np.clip(np.floor(col.astype(np.float64) * 64.0), 0,
                   255).astype(np.int32)


# ---------------------------------------------------------------------------
# UE1 runtime lighting math.
# ---------------------------------------------------------------------------

def build_lightsqrt():
    """LightSqrt[i] = (2S^3-3S^2+1)/S, S = sqrt((i+1)/4096)."""
    i = np.arange(4096, dtype=np.float64)
    s = np.sqrt((i + 1) * (1.0 / 4096.0))
    return (((2 * s - 3) * s * s + 1) / s).astype(np.float32)


# None under UE's numpy-less embedded Python (resolver-only consumers);
# composition paths that need it fail loudly on the None.
LIGHTSQRT = build_lightsqrt() if np is not None else None


def rot_vector(rotation):
    """Forward unit vector of a rotator =
    (cos(Pitch)*cos(Yaw), cos(Pitch)*sin(Yaw), sin(Pitch)) — roll cancels.
    UE1 quantizes angles to a 16384-entry sine table: sin(i) =
    sin(((i>>2)&16383)*2pi/16384), cos(i) = sin(i+16384).
    rotation: [pitch, yaw, roll] in UE1 rotator units (65536 = 360 deg)."""
    def sintab(i):
        return np.float32(np.sin(((int(i) >> 2) & 16383)
                                 * (2.0 * np.pi / 16384.0)))
    pitch, yaw, _roll = rotation
    cp, sp = sintab(int(pitch) + 16384), sintab(pitch)
    cy, sy = sintab(int(yaw) + 16384), sintab(yaw)
    return np.array([np.float32(cp * cy), np.float32(cp * sy), sp],
                    dtype=np.float64)


def fget_hsv(h, s, v):
    """HSV→RGB lighting conversion → float32[3] RGB."""
    b = v * 1.4 / 255.0
    b *= 0.70 / (0.01 + np.sqrt(b))
    b = min(max(b, 0.0), 1.0)
    if h < 86:
        hue = np.array([(85 - h) / 85.0, h / 85.0, 0.0])
    elif h < 171:
        hue = np.array([0.0, (170 - h) / 85.0, (h - 85) / 85.0])
    else:
        hue = np.array([(h - 170) / 85.0, 0.0, (255 - h) / 84.0])
    return ((hue + s / 255.0 * (1.0 - hue)) * b).astype(np.float32)


def shadow_map_gen(bits, mask_u, vclamp):
    """Shadow-mask filter: 1-bit mask → filtered byte map,
    shape (vclamp, mask_u*8), max value 254.

    bits: uint8[mask_u*vclamp] (row-major, rows of mask_u bytes, LSB-first).
    """
    w = mask_u * 8
    src = np.unpackbits(bits.reshape(vclamp, mask_u), axis=1,
                        bitorder="little").astype(np.int32)
    # horizontal pass with edge replication, per kernel row, floored per row
    left = np.concatenate([src[:, :1], src[:, :-1]], axis=1)
    right = np.concatenate([src[:, 1:], src[:, -1:]], axis=1)
    ha = (24 * left + 40 * src + 24 * right) * 255 // 320   # kernel rows 0 & 2
    hb = (40 * left + 64 * src + 40 * right) * 255 // 320   # kernel row 1
    # vertical pass: simulates the engine's three-row destination walk
    dest = np.zeros((vclamp, w), dtype=np.int32)
    d0 = d1 = 0
    d2 = 1
    for v in range(vclamp):
        if d0 < vclamp:
            dest[d0] += ha[v]
        if d1 < vclamp:
            dest[d1] += hb[v]
        if d2 < vclamp:
            dest[d2] += ha[v]
        d0 += 1; d1 += 1; d2 += 1
        if v == 0:
            d0 -= 1
        if v == vclamp - 2:
            d2 -= 1
    return dest.astype(np.uint8)


def surf_basis(bsp, s, lm):
    """Surface texel→world basis → (VertexBase, VertexDU, VertexDV, origin,
    tu, tv, n)."""
    origin = np.array(bsp["points"][s["pbase"]], dtype=np.float64)
    tu = np.array(bsp["vectors"][s["vtu"]], dtype=np.float64)
    tv = np.array(bsp["vectors"][s["vtv"]], dtype=np.float64)
    n = np.array(bsp["vectors"][s["vnorm"]], dtype=np.float64)
    m = np.array([tu, tv, n])          # rows = XAxis, YAxis, ZAxis
    inv = np.linalg.inv(m)
    ux, uy = inv[:, 0], inv[:, 1]      # inverse basis: U / V axes
    pan = lm["pan"]
    vbase = origin + ux * pan[0] + uy * pan[1]
    return (vbase, ux * lm["uscale"], uy * lm["vscale"], origin, tu, tv, n)


def clip_rect(loc, origin, tu, tv, n, radius, lm):
    """Per-light clip rect from the light sphere's plane projection."""
    rel = loc - origin
    plane_dot = rel @ n
    plane_r = np.sqrt(max(radius * radius * 1.05 - plane_dot * plane_dot, 0.0))
    center = loc - n * plane_dot
    cu = (center - origin) @ tu - lm["pan"][0]
    cv = (center - origin) @ tv - lm["pan"][1]
    ru = plane_r * np.sqrt(tu @ tu)
    rv = plane_r * np.sqrt(tv @ tv)
    minu = max(int(np.rint((cu - ru) / lm["uscale"])), 0)
    minv = max(int(np.rint((cv - rv) / lm["vscale"])), 0)
    maxu = min(int(np.rint((cu + ru) / lm["uscale"])), lm["uclamp"])
    maxv = min(int(np.rint((cv + rv) / lm["vscale"])), lm["vclamp"])
    return minu, minv, maxu, maxv


def spatial_none(shadow, vbase, du, dv, loc, diffuse, rradius_mult, rect):
    """Retail integer forward-differencing path (radial point-light falloff).
    shadow: uint8 (vclamp, uclamp) filtered shadow map. → uint8 illum map."""
    minu, minv, maxu, maxv = rect
    illum = np.zeros(shadow.shape, dtype=np.uint8)
    if maxu <= minu or maxv <= minv:
        return illum
    v0 = vbase - loc + du * minu + dv * minv
    scale = np.float64(np.float32(rradius_mult)) * 4096.0
    rnd = lambda x: np.int64(np.rint(x * scale))
    dist = rnd(v0 @ v0)
    dist_u, dist_v = rnd(v0 @ du), rnd(v0 @ dv)
    dist_uu, dist_vv, dist_uv = rnd(du @ du), rnd(dv @ dv), rnd(du @ dv)
    u = np.arange(maxu - minu, dtype=np.int64)[None, :]
    v = np.arange(maxv - minv, dtype=np.int64)[:, None]
    inner = (dist + 2 * u * dist_u + 2 * v * dist_v
             + u * u * dist_uu + v * v * dist_vv + 2 * u * v * dist_uv)
    inner_dw = (inner & 0xFFFFFFFF).astype(np.uint32)   # engine DWORD wrap
    sub = shadow[minv:maxv, minu:maxu]
    ok = (sub != 0) & (inner_dw < 4096 * 4096)
    idx = np.where(ok, inner_dw >> 12, 0)
    val = sub.astype(np.float64) * np.float64(np.float32(diffuse)) \
        * LIGHTSQRT[idx].astype(np.float64)
    # Hecker float->int trick = round half to even; clamp (see module doc)
    out = np.clip(np.rint(val), 0, 255).astype(np.uint8)
    illum[minv:maxv, minu:maxu] = np.where(ok, out, 0)
    return illum


def spatial_nonincidence(shadow, vbase, du, dv, loc, rradius, rradius_mult,
                         rect):
    """Non-incidence falloff path. → uint8 illum map."""
    minu, minv, maxu, maxv = rect
    illum = np.zeros(shadow.shape, dtype=np.uint8)
    if maxu <= minu or maxv <= minv:
        return illum
    u = np.arange(minu, maxu, dtype=np.float64)[None, :, None]
    v = np.arange(minv, maxv, dtype=np.float64)[:, None, None]
    pos = vbase - loc + du * u + dv * v
    s2 = np.einsum("vuk,vuk->vu", pos, pos)
    sqrt_ofs = np.rint(s2 * np.float64(np.float32(rradius_mult)))
    sub = shadow[minv:maxv, minu:maxu]
    ok = (sub != 0) & (sqrt_ofs < 4096) & (sqrt_ofs >= 0)
    val = np.floor(sub.astype(np.float64)
                   * (1.02 - np.sqrt(s2) * np.float64(np.float32(rradius))))
    out = np.clip(val, 0, 255).astype(np.uint8)
    illum[minv:maxv, minu:maxu] = np.where(ok, out, 0)
    return illum


def spatial_spotlight(shadow, vbase, du, dv, loc, view, light_cone, diffuse,
                      rradius_mult, rect):
    """Spotlight falloff path; both LE_StaticSpot and LE_Spotlight dispatch
    here. → uint8 illum map.

    View      = light's Rotation forward vector (== view rotation for
                non-Pawn actors)
    Sine      = 1 - LightCone/256, RSine = 1/(1-Sine)
    lit iff *Src, SqrtOfs = round(|V|^2*RRadiusMult) < 4096,
    VdotV > 0 and VdotV^2 > Sine^2*|V|^2; then
    Dot   = (VdotV*RSine/|V| - Sine*RSine)^2
    *Dest = floor(Dot * Src * Diffuse * LightSqrt[SqrtOfs])
    (1/sqrt approximation -> exact 1/sqrt, see module doc)."""
    minu, minv, maxu, maxv = rect
    illum = np.zeros(shadow.shape, dtype=np.uint8)
    if maxu <= minu or maxv <= minv:
        return illum
    sine = np.float64(np.float32(1.0 - light_cone / 256.0))
    rsine = np.float64(np.float32(1.0 / (1.0 - sine))) if sine != 1.0 \
        else np.inf
    sine_rsine = np.float64(np.float32(sine * rsine))
    sine_sq = np.float64(np.float32(sine * sine))
    u = np.arange(minu, maxu, dtype=np.float64)[None, :, None]
    v = np.arange(minv, maxv, dtype=np.float64)[:, None, None]
    pos = vbase - loc + du * u + dv * v
    size_sq = np.einsum("vuk,vuk->vu", pos, pos)
    sqrt_ofs = np.rint(size_sq * np.float64(np.float32(rradius_mult)))
    vdotv = pos @ view
    sub = shadow[minv:maxv, minu:maxu]
    ok = ((sub != 0) & (sqrt_ofs < 4096) & (sqrt_ofs >= 0)
          & (vdotv > 0.0) & (vdotv * vdotv > sine_sq * size_sq))
    idx = np.where(ok, sqrt_ofs, 0).astype(np.int64)
    with np.errstate(divide="ignore", invalid="ignore"):
        dot = np.square(vdotv * rsine / np.sqrt(size_sq) - sine_rsine)
    val = np.floor(np.where(ok, dot, 0.0) * sub.astype(np.float64)
                   * np.float64(np.float32(diffuse))
                   * LIGHTSQRT[idx].astype(np.float64))
    out = np.clip(val, 0, 255).astype(np.uint8)
    illum[minv:maxv, minu:maxu] = np.where(ok, out, 0)
    return illum


def spatial_cylinder(shadow, vbase, du, dv, loc, rradius, rect):
    """Cylinder falloff path: XY-only quadratic falloff,
    no incidence factor, no radius-sphere texel gate (the falloff self-clamps
    at 0; the clip rect still bounds iteration, as UE1's does):
    Dest = max(0, floor(Src * (1 - (dx^2 + dy^2) * RRadius^2)))."""
    minu, minv, maxu, maxv = rect
    illum = np.zeros(shadow.shape, dtype=np.uint8)
    if maxu <= minu or maxv <= minv:
        return illum
    u = np.arange(minu, maxu, dtype=np.float64)[None, :, None]
    v = np.arange(minv, maxv, dtype=np.float64)[:, None, None]
    pos = vbase - loc + du * u + dv * v
    r2 = np.square(pos[..., 0]) + np.square(pos[..., 1])
    sub = shadow[minv:maxv, minu:maxu]
    val = np.floor(sub.astype(np.float64)
                   * (1.0 - r2 * np.square(np.float64(np.float32(rradius)))))
    illum[minv:maxv, minu:maxu] = np.clip(val, 0, 255).astype(np.uint8)
    return illum


SUPPORTED_EFFECTS = {"LE_None", "LE_TorchWaver", "LE_FireWaver",
                     "LE_WateryShimmer", "LE_NonIncidence",
                     "LE_StaticSpot", "LE_Spotlight", "LE_Cylinder"}
SPOTLIGHT_EFFECTS = {"LE_StaticSpot", "LE_Spotlight"}


def load_level(map_path, verbose=False):
    """→ (pkg, bsp) for the map's level model (largest Model export)."""
    pkg = Package(map_path)
    models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
    bsp = decode_model_full(pkg, max(models, key=lambda e: e["size"]),
                            verbose=verbose)
    return pkg, bsp


def point_zone(bsp, point):
    """UE1 BSP point-to-zone lookup: descend the BSP from the root, taking
    the front child when the point is on the plane's front side (back
    otherwise), until a child is INDEX_NONE (leaf); the zone is that side's
    iZone at the leaf's parent. Matches the runtime zone assignment — a
    runtime mover gets its zone this way (its T3D Region is the stale editor
    value). Returns the zone number (index into bsp['zones']); 0 when
    NumZones==0."""
    nodes, planes, children = (bsp["nodes"], bsp["node_planes"],
                               bsp["node_children"])
    if not nodes or not bsp["zones"]:
        return 0
    px, py, pz = float(point[0]), float(point[1]), float(point[2])
    inode, zone = 0, 0
    while inode != -1:
        pl = planes[inode]
        is_front = 1 if (px * pl[0] + py * pl[1] + pz * pl[2] - pl[3]) >= 0.0 else 0
        zone = nodes[inode][4][is_front]      # iZone[IsFront]
        inode = children[inode][is_front]
    return zone


def zone_actor_name(pkg, bsp, zone_number):
    """Zone actor name for a zone number (the zone's ZoneActor). None →
    caller falls back to LevelInfo."""
    zones = bsp["zones"]
    if not (0 <= zone_number < len(zones)) or not zones[zone_number]:
        return None
    return pkg.objname(zones[zone_number])


def lightmap_uv(bsp, isurf, pts):
    """Fractional lightmap texel coordinates of world positions on a surface.

    Exact inverse of the surf_basis texel placement: texel (i,j) sits at
    VertexBase + i*VertexDU + j*VertexDV, and by the dual-basis identity
    (UncoordsX|TU)=1, (UncoordsY|TU)=0 that position maps back to exactly
    (i, j) here. pts: (N,3) world positions (must lie on the surface plane).
    → (N,2) float64 (u_tex, v_tex)."""
    s = bsp["surfs"][isurf]
    lm = bsp["lightmap"][s["ilightmap"]]
    origin = np.array(bsp["points"][s["pbase"]], dtype=np.float64)
    tu = np.array(bsp["vectors"][s["vtu"]], dtype=np.float64)
    tv = np.array(bsp["vectors"][s["vtv"]], dtype=np.float64)
    rel = np.asarray(pts, dtype=np.float64) - origin
    return np.stack([(rel @ tu - lm["pan"][0]) / lm["uscale"],
                     (rel @ tv - lm["pan"][1]) / lm["vscale"]], axis=1)


def compose_map(map_path, t3d_path, verbose=False, ctx=None, wave="mean",
                wave_group=None, effect="mean", effect_class=None,
                track_light_names=None):
    """ctx: optional pre-loaded (pkg, bsp) from load_level().
    wave: animated-light (LT_Pulse/SubtlePulse) amplitude — "mean" (static
    baseline), "hi" (base+amp) or "lo" (base−amp). Endpoints feed centered
    response baking and are complete compositions with clamps included.
    ``wave_group`` restricts that endpoint to one exact
    (LightType, LightPeriod, LightPhase) key; None retains the legacy all-wave
    census endpoint. ``effect``/``effect_class`` similarly select one shimmer
    endpoint. Unselected animation terms remain at their exact means.
    ``track_light_names`` emits a boolean ``tracked_texels`` reach mask."""
    if wave not in ("mean", "hi", "lo"):
        raise ValueError(f"invalid waveform endpoint {wave!r}")
    if effect not in ("mean", "hi", "lo"):
        raise ValueError(f"invalid shimmer endpoint {effect!r}")
    if wave_group is not None and len(tuple(wave_group)) != 3:
        raise ValueError(f"invalid waveform group {wave_group!r}")
    if effect_class is not None and effect_class not in SHIMMER_EFFECTS:
        raise ValueError(f"invalid shimmer class {effect_class!r}")
    tracked_names = set(track_light_names or ())
    pkg, bsp = ctx if ctx else load_level(map_path, verbose=verbose)
    name = Path(map_path).stem

    actors = {a["name"]: a for a in json.load(open(t3d_path))["actors"]}
    li = next(a for a in actors.values() if a["cls"] == "LevelInfo")
    level_brightness = float(li["props"].get("Brightness", 1.0))

    # Lights OFF at level start: Lightning flash lights. Lightning.uc:156-161
    # finds every actor matching its LightTag on the first Tick and sets
    # LightType=LT_None; they only flash during strikes. The editor still
    # bakes them into iLightActors (the runtime sorts bStatic=False
    # into the dynamic light class) so the static composer must
    # drop them — e.g. Arena_01 has 11 (b=128-192, r=128 -> 3225uu) that
    # otherwise flood the under-arena rooms. TriggerLights are NOT gated:
    # neither shipped WOT/TriggerLight.uc nor UE1's Engine TriggerLight
    # zeroes brightness before the first trigger, so untriggered ones light
    # dynamically at their editor brightness.
    # RE-CONFIRMED against goldens 2026-07-14 after a false alarm: fresh
    # retail captures that day showed gated-light areas brighter, but that
    # was the CAPTURE RIG drifted bright-in-darks vs the archived goldens
    # (PORT_NOTES "Retail capture rig drift"), not the gate failing —
    # composing gated lights IN regressed Arena_01 pose bad% 2.06 -> 12.29
    # (PS20_rev 0.26 -> 93) against the goldens, which are the spec of
    # record. Do not re-litigate this from fresh retail captures without
    # first re-validating the rig against an archived golden pose.
    lightning_tags = {a["props"].get("LightTag", "").lower()
                      for a in actors.values() if is_a(a["cls"], "Lightning")}
    lightning_tags.discard("")
    lightning_off = {a["name"] for a in actors.values()
                     if a["props"].get("Tag", "").lower() in lightning_tags}

    # resolve every light referenced by the model's Lights array
    light_cache = {}

    def get_light(ref):
        if ref in light_cache:
            return light_cache[ref]
        lname = pkg.objname(ref)
        rec = None
        if lname is not None and lname in actors:
            rec = canonical_light_animation(resolve_light(actors[lname]))
            radius = world_light_radius(rec["cls"], rec["LightRadius"])
            rradius = 1.0 / max(1.0, radius)
            rec["radius"] = radius
            rec["rradius"] = rradius
            rec["rradius_mult"] = 4093.0 * rradius * rradius
            fc = fget_hsv(rec["LightHue"], rec["LightSaturation"], 255) \
                * np.float32(rec["LightBrightness"] / 255.0) \
                * np.float32(level_brightness) \
                * np.float32(_selected_wave_scale(rec, wave, wave_group)) \
                * np.float32(_selected_effect_scale(
                    rec, effect, effect_class))
            rec["float_color"] = fc
            rec["fixd"] = np.floor(fc.astype(np.float64) * 65536.0).astype(np.int64)
            rec["view"] = rot_vector(rec["rotation"])   # spotlight cone axis
        light_cache[ref] = rec
        return rec

    lightbits = bsp["lightbits"]
    nlights_arr = len(bsp["lights"])

    # ---- zone ambient ----------------------------------------------------
    # Zone actor per the zone's properties ref; no actor (or zone 0) →
    # LevelInfo (the zone-actor lookup falls back to it; LevelInfo IS a
    # ZoneInfo).
    zone_seeds = []
    for zref in (bsp["zones"] or [0]):
        zname = pkg.objname(zref) if zref else None
        za = actors.get(zname) if zname else None
        zone_seeds.append(ambient_seed(za if za is not None else li))
    zone_seeds = np.array(zone_seeds, dtype=np.int32)
    # Surface → zone: vote across the nodes that render the surface;
    # iZone[1] is the front (normal-side) zone, iZone[0] the back — prefer
    # front, fall back to back, else zone 0.
    zvotes_f, zvotes_b = {}, {}
    for _ivp, isurf_n, _nv, _pl, iz in bsp["nodes"]:
        if iz[1]:
            zvotes_f.setdefault(isurf_n, Counter())[iz[1]] += 1
        if iz[0]:
            zvotes_b.setdefault(isurf_n, Counter())[iz[0]] += 1

    def surf_zone(isurf):
        for votes in (zvotes_f, zvotes_b):
            c = votes.get(isurf)
            if c:
                return c.most_common(1)[0][0]
        return 0

    skipped = Counter()
    used_lights = {}
    tex_rgb, tex_pos, tex_sid, tracked_texels = [], [], [], []
    smeta = dict(index=[], ilm=[], uclamp=[], vclamp=[], tex=[], flags=[],
                 normal=[], ofs=[])
    pairs = dict(surf=[], light=[], maxc=[], clipped=[])
    sanity = dict(checked=0, passed=0, worst=0.0)
    ambient_stats = Counter()
    ofs = 0
    n_lm_surfs = 0

    for isurf, s in enumerate(bsp["surfs"]):
        ilm = s["ilightmap"]
        if ilm < 0:
            continue
        lm = bsp["lightmap"][ilm]
        uclamp, vclamp = lm["uclamp"], lm["vclamp"]
        mask_u = (uclamp + 7) >> 3
        mask_space = mask_u * vclamp
        n_lm_surfs += 1

        vbase, du, dv, origin, tu, tv, n = surf_basis(bsp, s, lm)

        zi = surf_zone(isurf)
        seed = zone_seeds[zi] if zi < len(zone_seeds) else zone_seeds[0]
        if seed.any():
            ambient_stats[f"zone{zi}:{tuple(seed)}"] += 1
        total = np.empty((vclamp, uclamp, 3), dtype=np.int32)
        # The runtime seeds ambient from the zone of the DRAW (Zone=Draw->Zone),
        # so a surf straddling a zone boundary composes with a DIFFERENT
        # ambient per side (Arena_07's pit ring: dim room ambient above the
        # lip, AmbientBrightness=255 red inside the pit — the audit's measured
        # x2 red deficit). One static map per surf, so straddlers classify per
        # TEXEL: the zone of the space just in front (point_zone at
        # texel + 2uu*normal), matching what each drawn side shows.
        cand = set()
        for votes in (zvotes_f.get(isurf), zvotes_b.get(isurf)):
            if votes:
                cand.update(z for z in votes if 0 <= z < len(zone_seeds))
        if len({tuple(zone_seeds[z]) for z in cand}) > 1:
            for ti in range(vclamp):
                base_i = vbase + dv * ti + n * 2.0
                for tj in range(uclamp):
                    tz = point_zone(bsp, base_i + du * tj)
                    total[ti, tj] = (zone_seeds[tz]
                                     if 0 < tz < len(zone_seeds) else seed)
            ambient_stats["per_texel_straddlers"] += 1
        else:
            total[:] = seed                   # static map seeded with ambient
        tracked = np.zeros((vclamp, uclamp), dtype=bool)
        surf_lights = []
        ila = lm["ilightactors"]
        if ila >= 0:
            i = 0
            while ila + i < nlights_arr and bsp["lights"][ila + i] != 0:
                surf_lights.append((i, bsp["lights"][ila + i]))
                i += 1

        for li_idx, ref in surf_lights:
            rec = get_light(ref)
            if rec is None:
                skipped["missing_actor"] += 1
                continue
            if rec["name"] in lightning_off:
                skipped["lightning_off"] += 1
                continue
            if rec["LightType"] not in LIGHT_TYPE_MEAN:
                skipped[f"type:{rec['LightType']}"] += 1
                continue
            if rec["LightBrightness"] == 0:
                skipped["brightness_0"] += 1
                continue
            if rec["LightEffect"] not in SUPPORTED_EFFECTS:
                skipped[f"effect:{rec['LightEffect']}"] += 1
                continue

            o = lm["data_offset"] + li_idx * mask_space
            if o + mask_space > len(lightbits):
                skipped["mask_out_of_range"] += 1
                continue
            shadow = shadow_map_gen(lightbits[o:o + mask_space], mask_u,
                                    vclamp)[:, :uclamp]
            loc = rec["location"]
            rect = clip_rect(loc, origin, tu, tv, n, rec["radius"], lm)
            if rec["LightEffect"] == "LE_NonIncidence":
                illum = spatial_nonincidence(shadow, vbase, du, dv, loc,
                                             rec["rradius"],
                                             rec["rradius_mult"], rect)
            elif rec["LightEffect"] == "LE_Cylinder":
                illum = spatial_cylinder(shadow, vbase, du, dv, loc,
                                         rec["rradius"], rect)
            elif rec["LightEffect"] in SPOTLIGHT_EFFECTS:
                diffuse = abs((loc - origin) @ n) * rec["rradius"]
                illum = spatial_spotlight(shadow, vbase, du, dv, loc,
                                          rec["view"], int(rec["LightCone"]),
                                          diffuse, rec["rradius_mult"], rect)
            else:
                diffuse = abs((loc - origin) @ n) * rec["rradius"]
                illum = spatial_none(shadow, vbase, du, dv, loc, diffuse,
                                     rec["rradius_mult"], rect)

            if rec["name"] in tracked_names:
                tracked |= illum > 0
            illum = _apply_selected_effect(
                illum, rec, effect, effect_class)

            contrib = np.minimum(
                (illum[..., None].astype(np.int64) * rec["fixd"]) >> 16,
                127).astype(np.int32)
            total = np.minimum(total + contrib, 127)

            key = rec["name"]
            if key not in used_lights:
                used_lights[key] = (len(used_lights), rec)
            pairs["surf"].append(isurf)
            pairs["light"].append(used_lights[key][0])
            pairs["maxc"].append(int(contrib.max()))
            pairs["clipped"].append(bool((contrib == 127).any()))

            # sanity: single-light surfaces, unshadowed near-point (radial
            # falloff only — a spotlight's peak is on the cone axis, not at
            # the nearest texel)
            if len(surf_lights) == 1 and illum.max() > 0 \
                    and rec["LightEffect"] not in SPOTLIGHT_EFFECTS:
                uu, vv = np.meshgrid(np.arange(uclamp), np.arange(vclamp))
                p = vbase[None, None] + uu[..., None] * du[None, None] \
                    + vv[..., None] * dv[None, None]
                d2 = ((p - loc) ** 2).sum(-1)
                near = np.unravel_index(np.argmin(d2), d2.shape)
                if shadow[near] > 200:
                    # brightest texel must be the nearest one (or share its
                    # byte value — quantization plateaus tie frequently)
                    bright = np.unravel_index(np.argmax(illum), illum.shape)
                    dist_t = np.hypot(bright[0] - near[0], bright[1] - near[1])
                    sanity["checked"] += 1
                    if dist_t <= 1.5 or illum[near] == illum.max():
                        sanity["passed"] += 1
                    else:
                        sanity["worst"] = max(sanity["worst"], float(dist_t))

        uu, vv = np.meshgrid(np.arange(uclamp), np.arange(vclamp))
        pos = vbase[None, None] + uu[..., None] * du[None, None] \
            + vv[..., None] * dv[None, None]
        tex_rgb.append(total.reshape(-1, 3).astype(np.float32))
        tex_pos.append(pos.reshape(-1, 3).astype(np.float32))
        tex_sid.append(np.full(uclamp * vclamp, isurf, dtype=np.int32))
        tracked_texels.append(tracked.reshape(-1))
        smeta["index"].append(isurf)
        smeta["ilm"].append(ilm)
        smeta["uclamp"].append(uclamp)
        smeta["vclamp"].append(vclamp)
        smeta["tex"].append(pkg.objname(s["tex"]) or "None")
        smeta["flags"].append(s["flags"])
        smeta["normal"].append(n.astype(np.float32))
        smeta["ofs"].append(ofs)
        ofs += uclamp * vclamp

    out = dict(
        texel_rgb=np.concatenate(tex_rgb) if tex_rgb else np.zeros((0, 3), np.float32),
        texel_pos=np.concatenate(tex_pos) if tex_pos else np.zeros((0, 3), np.float32),
        surf_id=np.concatenate(tex_sid) if tex_sid else np.zeros(0, np.int32),
        tracked_texels=(np.concatenate(tracked_texels) if tracked_texels
                        else np.zeros(0, bool)),
        surf_index=np.array(smeta["index"], np.int32),
        surf_ilightmap=np.array(smeta["ilm"], np.int32),
        surf_uclamp=np.array(smeta["uclamp"], np.int32),
        surf_vclamp=np.array(smeta["vclamp"], np.int32),
        surf_texture=np.array(smeta["tex"]),
        surf_flags=np.array(smeta["flags"], np.uint32),
        surf_normal=np.array(smeta["normal"], np.float32).reshape(-1, 3),
        surf_texel_ofs=np.array(smeta["ofs"], np.int64),
        pair_surf=np.array(pairs["surf"], np.int32),
        pair_light=np.array(pairs["light"], np.int32),
        pair_max_contrib=np.array(pairs["maxc"], np.int32),
        pair_clipped=np.array(pairs["clipped"], bool),
        light_names=np.array([k for k in used_lights]),
        light_pos=np.array([r["location"] for _, r in used_lights.values()],
                           np.float32).reshape(-1, 3),
    )

    # ---- stats -----------------------------------------------------------
    rgb = out["texel_rgb"]
    lit = rgb.max(axis=1) > 0
    mult = rgb.max(axis=1) * 2.0 / 255.0
    stats = dict(
        map=name, pkg_version=pkg.version,
        surfaces_with_lightmaps=n_lm_surfs,
        total_texels=int(rgb.shape[0]),
        pct_texels_saturated=float(100.0 * (rgb == 127).any(axis=1).mean())
        if len(rgb) else 0.0,
        pct_light_pairs_clipped=float(100.0 * out["pair_clipped"].mean())
        if len(out["pair_clipped"]) else 0.0,
        skipped=dict(skipped),
        lit_texel_pct=float(100.0 * lit.mean()) if len(rgb) else 0.0,
        mean_display_mult=float(mult[lit].mean()) if lit.any() else 0.0,
        median_display_mult=float(np.median(mult[lit])) if lit.any() else 0.0,
        n_lights_used=len(used_lights),
        ambient=dict(ambient_stats),      # "zoneN:(r,g,b)" -> surface count
        sanity=sanity,
    )
    out["meta_json"] = np.array(json.dumps(stats))
    return out, stats


def compose_centered(base_rgb, response_rgbs, coefficients):
    """Evaluate literal-base + centered responses in RGBA7 total units.

    Zero coefficients are skipped deliberately: waveform s=0.5 gives z=0,
    shimmer u=mean gives n=0, and freeze therefore returns a byte-identical
    copy of the literal base without sampling/rounding response data.
    """
    if len(response_rgbs) != len(coefficients):
        raise ValueError("response/coefficient count mismatch")
    total = np.array(base_rgb, dtype=np.float32, copy=True)
    for response, coefficient in zip(response_rgbs, coefficients):
        coefficient = float(coefficient)
        if coefficient != 0.0:
            total += np.asarray(response, dtype=np.float32) * coefficient
    return np.clip(total, 0.0, 127.0)


def encoded_atlas_rgba7(rgb):
    """Round-trip RGBA7 totals through the actual scale-2 RGB8 encoding."""
    encoded = np.minimum(
        np.asarray(rgb, dtype=np.float64) * ATLAS_ENCODE_SCALE, 255.0)
    return encoded.astype(np.uint8).astype(np.float32) / ATLAS_ENCODE_SCALE


def _encoded_reconstruction_error(reference, reconstructed):
    delta = np.abs(np.asarray(reference, dtype=np.float64)
                   - np.asarray(reconstructed, dtype=np.float64))
    mismatched = int(np.any(delta != 0.0, axis=1).sum())
    rgba7 = float(delta.max()) if delta.size else 0.0
    atlas_bytes = int(round(rgba7 * ATLAS_ENCODE_SCALE))
    return mismatched, rgba7, atlas_bytes


def _response_output(base, response_rgb, affected):
    out = dict(base)
    out["texel_rgb"] = np.asarray(response_rgb, dtype=np.float32)
    out["tracked_texels"] = np.asarray(affected, dtype=bool)
    return out


def _validate_response(map_name, term, response_rgb, affected):
    """Enforce the unsigned, affected-mask, scale-2 texture contract."""
    response = np.asarray(response_rgb)
    if response.shape[-1:] != (3,):
        raise ValueError(f"{map_name}: malformed response shape {response.shape}")
    if np.any(response < 0.0):
        minimum = float(response.min())
        raise ValueError(
            f"{map_name}: {term['kind']} response is signed ({minimum})")
    encoded = np.floor(response.astype(np.float64) * 2.0).astype(np.int64)
    if encoded.size and int(encoded.max()) > 255:
        raise ValueError(
            f"{map_name}: {term['kind']} response overflows RGB8")
    if np.any(response[~np.asarray(affected, dtype=bool)] != 0.0):
        raise ValueError(
            f"{map_name}: {term['kind']} response escaped its light mask")


def bake_animation_responses(map_path, t3d_path, manifest, ctx=None,
                             base=None):
    """Compose every pure and mixed centered response in manifest order.

    Returns ``(base, rows)``. Each row contains the manifest ``term``, a
    compose_map-compatible response ``out``, its ``affected_texels`` mask, and
    ``endpoint_error`` in encoded atlas-byte units. The gate reconstructs from
    the literal base after its real RGB8 encode plus each independently encoded
    response slot; it never derives a surrogate base/slot set from the corners.
    """
    map_name = Path(map_path).stem
    if base is None:
        base, _stats = compose_map(map_path, t3d_path, ctx=ctx)
    base_rgb = base["texel_rgb"].astype(np.float32)
    encoded_base = encoded_atlas_rgba7(base_rgb)
    rows = []

    def compose_endpoint(term, wave_endpoint="mean", effect_endpoint="mean"):
        return compose_map(
            map_path, t3d_path, ctx=ctx,
            wave=wave_endpoint, wave_group=term.get("group"),
            effect=effect_endpoint, effect_class=term.get("effect"),
            track_light_names=term["light_names"])[0]

    for term in manifest["terms"]:
        kind = term["kind"]
        diagnostics = {}
        if kind == "waveform":
            high = compose_endpoint(term, wave_endpoint="hi")
            low = compose_endpoint(term, wave_endpoint="lo")
            response = (high["texel_rgb"] - low["texel_rgb"]) * np.float32(0.5)
            affected = high["tracked_texels"] | low["tracked_texels"]
            encoded = np.floor(response.astype(np.float64) * 2.0).astype(np.int16)
            expected = (high["texel_rgb"] - low["texel_rgb"]).astype(np.int16)
            if not np.array_equal(encoded, expected):
                raise ValueError(f"{map_name}: waveform high-low encode drift")
        elif kind == "effect":
            high = compose_endpoint(term, effect_endpoint="hi")
            low = compose_endpoint(term, effect_endpoint="lo")
            response = (high["texel_rgb"] - low["texel_rgb"]) * np.float32(0.5)
            affected = high["tracked_texels"] | low["tracked_texels"]
            encoded = np.floor(response.astype(np.float64) * 2.0).astype(np.int16)
            expected = (high["texel_rgb"] - low["texel_rgb"]).astype(np.int16)
            if not np.array_equal(encoded, expected):
                raise ValueError(f"{map_name}: shimmer high-low encode drift")
        elif kind == "intersection":
            high_high = compose_endpoint(
                term, wave_endpoint="hi", effect_endpoint="hi")
            high_low = compose_endpoint(
                term, wave_endpoint="hi", effect_endpoint="lo")
            low_high = compose_endpoint(
                term, wave_endpoint="lo", effect_endpoint="hi")
            low_low = compose_endpoint(
                term, wave_endpoint="lo", effect_endpoint="lo")
            response = (high_high["texel_rgb"] - high_low["texel_rgb"]
                        - low_high["texel_rgb"] + low_low["texel_rgb"]) \
                * np.float32(0.25)
            affected = (high_high["tracked_texels"]
                        | high_low["tracked_texels"]
                        | low_high["tracked_texels"]
                        | low_low["tracked_texels"])
            diagnostics = dict(
                mixed_required_texels=int(
                    np.any(response != 0.0, axis=1).sum()),
            )
        else:
            raise ValueError(f"{map_name}: unknown animation term {kind!r}")

        _validate_response(map_name, term, response, affected)
        encoded_response = encoded_atlas_rgba7(response)
        references = []
        reconstructions = []
        if kind in ("waveform", "effect"):
            references = [high["texel_rgb"], low["texel_rgb"]]
            reconstructions = [
                np.clip(encoded_base + encoded_response, 0.0, 127.0),
                np.clip(encoded_base - encoded_response, 0.0, 127.0),
            ]
        else:
            wave_row = next(
                row for row in rows
                if row["term"]["kind"] == "waveform"
                and tuple(row["term"]["group"]) == tuple(term["group"]))
            effect_row = next(
                row for row in rows
                if row["term"]["kind"] == "effect"
                and row["term"]["effect"] == term["effect"])
            encoded_wave = encoded_atlas_rgba7(
                wave_row["out"]["texel_rgb"])
            encoded_effect = encoded_atlas_rgba7(
                effect_row["out"]["texel_rgb"])
            corners = (
                (1.0, 1.0, high_high), (1.0, -1.0, high_low),
                (-1.0, 1.0, low_high), (-1.0, -1.0, low_low),
            )
            for z, n, corner in corners:
                references.append(corner["texel_rgb"])
                reconstructions.append(np.clip(
                    encoded_base + z * encoded_wave + n * encoded_effect
                    + z * n * encoded_response, 0.0, 127.0))

        reconstruction = tuple(
            _encoded_reconstruction_error(reference, reconstructed)
            for reference, reconstructed in zip(references, reconstructions))
        mismatch_counts = tuple(row[0] for row in reconstruction)
        errors_rgba7 = tuple(row[1] for row in reconstruction)
        errors_atlas_bytes = tuple(row[2] for row in reconstruction)
        endpoint_error = max(errors_atlas_bytes, default=0)
        diagnostics.update(
            encoded_endpoint_mismatched_texels=mismatch_counts,
            encoded_endpoint_errors_rgba7=errors_rgba7,
            encoded_endpoint_errors_atlas_bytes=errors_atlas_bytes,
        )
        if endpoint_error > MAX_ENDPOINT_ERROR_ATLAS_BYTES:
            raise ValueError(
                f"{map_name}: {kind} encoded endpoint error "
                f"{endpoint_error} exceeds {MAX_ENDPOINT_ERROR_ATLAS_BYTES} "
                "atlas bytes")
        rows.append(dict(
            term=term,
            out=_response_output(base, response, affected),
            affected_texels=affected,
            endpoint_error=endpoint_error,
            **diagnostics,
        ))
    return base, rows


def print_stats(st):
    print(f"\n=== {st['map']} (pkg v{st['pkg_version']}) ===")
    print(f"  surfaces with lightmaps : {st['surfaces_with_lightmaps']}")
    print(f"  total texels            : {st['total_texels']}")
    print(f"  lit texels              : {st['lit_texel_pct']:.1f}%")
    print(f"  texels saturated (==127): {st['pct_texels_saturated']:.2f}%")
    print(f"  light pairs clipped     : {st['pct_light_pairs_clipped']:.2f}%")
    print(f"  lights used             : {st['n_lights_used']}")
    print(f"  display mult (lit)      : mean {st['mean_display_mult']:.3f} "
          f"median {st['median_display_mult']:.3f}")
    print(f"  skipped                 : {st['skipped'] or '(none)'}")
    print(f"  ambient (surf counts)   : {st['ambient'] or '(none)'}")
    sa = st["sanity"]
    print(f"  sanity nearest-vs-brightest: {sa['passed']}/{sa['checked']} "
          f"within 1.5 texels (worst {sa['worst']:.1f})")


def main():
    if len(sys.argv) != 4:
        print(__doc__.split("\n")[2])
        sys.exit(1)
    map_path, t3d_path, out_path = sys.argv[1:4]
    out, stats = compose_map(map_path, t3d_path, verbose=True)
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(out_path, **out)
    print_stats(stats)


if __name__ == "__main__":
    main()
