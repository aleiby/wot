#!/usr/bin/env python3
"""Extract gameplay zones and exact ambient-zone lookup data from a .wot BSP.

UE1 zones are IMPLICIT: a ZoneInfo point actor tags a BSP region bounded by
PF_Portal zone-partition polys, and the finished assignment is baked into the
UModel — every node carries iZone[back/front], and Zones[] maps each zone number
to its ZoneActor. We reconstruct each zone's extent as the AABB of the polygons on
its boundary (both adjacent zones), pair it with its ZoneInfo's gameplay and
CitadelEditor props
(bWaterZone / bPainZone+DamagePerSec+DamageType / bHealZone+HealPerSec /
ZoneGravity / ZoneVelocity incl. the TrigVelo2 initially-on fold /
ZoneGroundFriction / ZoneFluidFriction), and emit them under ``zones`` for
import_map.py to place as AWoTZoneVolume actors.

``ambient_zones`` contains raw AmbientBrightness/Hue/Saturation for every model
zone (plus the LevelInfo zone-zero fallback).  ``zone_bsp`` is the minimal flat
point-classification tree.  Child values >= 0 are node indices; negative values
are leaf markers encoded as ``-(zone + 1)``.  Descent starts at node zero and
chooses front when ``dot(Point, Plane.xyz) - Plane.w >= 0`` (coplanar is front),
back otherwise, matching lightmaps.point_zone and UE1's iZone[front/back].

``leaf_bsp`` is parallel to ``zone_bsp`` and carries RootOutside plus compact
``[iLeafBack, iLeafFront, isCsg]`` rows. The same descent therefore returns a
compiled zone and leaf, while isCsg supplies the only additional node state the
zero-extent UModel LineCheck needs. ``leaf_lights`` stores each leaf's shipped
NULL-terminated permeating run as indices into the deduplicated
``light_actors`` name table. Parallel ``light_props`` rows are
``[raw_brightness, world_radius, special_lit, emits]``; non-emitting rows still
participate in retail threshold/visible-count selection. Parallel ``light_anim``
rows are ``[LightType, LightPeriod, LightPhase, LightEffect]``.

``friction_nodes`` is the sparse ``[node_index, Texture.Friction]`` channel.
Only nodes whose BSP surface resolves through the package-qualified texture
manifest identity to a non-default value are present; runtime lookup defaults
to 1.0. Movers are deliberately absent because this is the compiled world
model, matching the original TraceTexture's LevelInfo-only gate.

Coords are raw UE1 world units — the same space import_map places actors in (the
GLB's /100 is undone by the ×100 mesh import), so no conversion here.

Usage: python3 zones.py <map.wot> <out.json> <texture_manifest_root>
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ue1pkg import Package, PayloadReader          # noqa: E402
from lightmaps import (class_defaults, decode_model_full, resolve_light,
                       world_light_radius)  # noqa: E402

# Gameplay-effect ZoneInfo classes to place. WaterZone `expands WOTZoneInfo` but
# its class name has no "ZoneInfo", so an explicit allow-set is needed (resolving
# the full UnrealScript hierarchy from the .wot isn't available). Deliberately
# EXCLUDES SkyZoneInfo (sky), WaysZone (lighting-only), WarpZoneInfo/WarpZoneMarker
# (teleport portals — not a water/pain/gravity/current effect).
EFFECT_ZONE_CLASSES = {"WOTZoneInfo", "ZoneInfo", "WaterZone"}

# Per-class default effects. WaterZone's defaultproperties set bWaterZone=True; an
# instance may re-serialize bWaterZone=False to turn it off (respected below). Do
# NOT add the base ZoneInfo ZoneGravity=(0,0,-950) default — that's normal gravity
# and would falsely flag every zone; gravity/velocity/pain stay instance-driven.
CLASS_DEFAULT_EFFECT = {"WaterZone": {"water": True}}

NF_NOT_CSG = 0x01
NF_IS_NEW = 0x20


def _c_atoi(value):
    """UE1 Int(string): C atoi leading whitespace/sign/digits, else zero."""
    match = re.match(r"\s*([+-]?\d+)", "" if value is None else str(value))
    return int(match.group(1)) if match else 0


def _vec(props, name):
    """A Vector StructProperty (12 raw bytes -> 3 floats), or None if absent."""
    raw = props.get("_structs", {}).get(name, {}).get(0)
    if not raw or len(raw) < 12:
        return None
    return list(struct.unpack_from("<3f", raw, 0))


def _rot_pitch(props, name, default):
    """Pitch from a UE1 Rotator StructProperty, in native 65536-turn units."""
    raw = props.get("_structs", {}).get(name, {}).get(0)
    if not raw or len(raw) < 12:
        return default
    return struct.unpack_from("<i", raw, 0)[0]


def _sound_subpath(pkg, ref, property_name):
    """ObjectProperty sound ref -> flattened '<Package>/<Name>' import key."""
    if ref in (None, 0):
        return ""
    if type(ref) is not int:
        raise ValueError(
            f"{property_name}: expected decoded ObjectProperty ref, got {ref!r}")
    try:
        identity = pkg.objref(ref)
    except (IndexError, TypeError, ValueError) as exc:
        raise ValueError(
            f"{property_name}: invalid ObjectProperty ref {ref}") from exc
    if not identity or len(identity) != 2 or not all(identity):
        raise ValueError(
            f"{property_name}: unresolved ObjectProperty ref {ref}: {identity!r}")
    package, name = identity
    return f"{package}/{name}"


def _read_zoneinfo(pkg, export, cls):
    p = PayloadReader(pkg, export).properties()
    cd = CLASS_DEFAULT_EFFECT.get(cls, {})
    # instance prop wins if present (a WaterZone can set bWaterZone=False);
    # else fall back to the class default (WaterZone → True).
    water = bool(p["bWaterZone"]) if "bWaterZone" in p else cd.get("water", False)
    # TrigVelo2 startup fold (WOTZoneInfo.uc PreBeginPlay): when bTrigZoneVelo2,
    # ZoneVelocity is OVERWRITTEN at startup with MaxZoneVelocity2 (initially on)
    # or MinZoneVelocity2 (off — usually absent = zero). Arena_11's updraft
    # (WOTZoneInfo1) is authored ONLY this way; the trigger wiring itself is a
    # follow-up, this captures the initial state.
    velocity = _vec(p, "ZoneVelocity")
    if p.get("bTrigZoneVelo2", False):
        key = ("MaxZoneVelocity2" if p.get("bTrigZoneVelo2InitiallyOn", False)
               else "MinZoneVelocity2")
        velocity = _vec(p, key)
    return dict(
        name=export["name"],
        location=_vec(p, "Location") or [0.0, 0.0, 0.0],
        water=water,
        pain=bool(p.get("bPainZone", False)),
        damage_per_sec=int(p.get("DamagePerSec", 0)),
        # ZoneInfo.DamageType (name, e.g. xxFxx fire pain); "" = class default
        # None — matters so a Fire Shield can eat fire-typed zone pain.
        damage_type=str(p.get("DamageType", "")),
        # WOTZoneInfo heal zones: +HealPerSec health on the 1s pain cadence
        # (WOTPlayer/WOTPawn.uc PainTimer) while feet are in the zone.
        heal=bool(p.get("bHealZone", False)),
        heal_per_sec=int(p.get("HealPerSec", 0)),
        # UE1 Package.Group.Name -> imported /Sounds/<Package>/<Name>.
        heal_sound=_sound_subpath(pkg, p.get("HealSound", 0), "HealSound"),
        # UE1 ZoneInfo default ZoneGroundFriction=8; water zones author 0.6.
        ground_friction=float(p.get("ZoneGroundFriction", 8.0)),
        # UE1 ZoneInfo default ZoneFluidFriction=1.2; authored water zones use
        # 0.6. physFlying consumes the CURRENT zone's value.
        fluid_friction=float(p.get("ZoneFluidFriction", 1.2)),
        # bFogZone gates UE1 volumetric lighting: fog renders only while the
        # VIEWER is in a fog zone. Emitted so the runtime camera gate has the
        # zone extents (AWoTZoneVolume.bFogZone).
        fog=bool(p.get("bFogZone", False)),
        gravity=_vec(p, "ZoneGravity"),
        velocity=velocity,
        edit_team=int(p.get("Team", 255)),
        min_edit_distance=int(p.get("MinEditDistance", 0)),
        edit_view_angle_pitch=_rot_pitch(p, "EditViewAngle", -8192),
        edit_view_distance=float(p.get("EditViewDistance", 32.0)),
        allow_editing=bool(p.get("bAllowEditing", True)),
    )


def _read_ambient(pkg, export, cls):
    """Zone lighting properties, resolving omissions through class defaults."""
    defaults = class_defaults(cls)
    props = PayloadReader(pkg, export).properties() if export else {}
    result = {
        key: int(float(props.get(key, defaults[key])))
        for key in ("AmbientBrightness", "AmbientHue", "AmbientSaturation")
    }
    result["MinLightCount"] = int(float(
        props.get("MinLightCount", defaults.get("MinLightCount", 6))))
    result["MaxLightCount"] = int(float(
        props.get("MaxLightCount", defaults.get("MaxLightCount", 6))))
    return result


def _leaf_marker(zone):
    """Encode a BSP leaf zone without colliding with non-negative node indices."""
    return -(int(zone) + 1)


# SHIPPED 333B enum orders, canonicalized from binary property bytes. A
# later engine revision inserts LT_Scripted at index 5, but the shipped
# enum does NOT have it: Arena_12's 33 SubtlePulse lights carry
# binary byte 7 against their T3D name
# LT_SubtlePulse (with LT_Scripted it would be 8). ELightEffect matches the
# usual UE1 order (witnesses: 1=TorchWaver, 2=FireWaver, 3=WateryShimmer,
# 6=FastWave, 13=NonIncidence, 17=Cylinder across 210 paired lights).
_SHIPPED_LIGHT_TYPES = (
    "LT_None", "LT_Steady", "LT_Pulse", "LT_Blink", "LT_Flicker",
    "LT_Strobe", "LT_BackdropLight", "LT_SubtlePulse",
    "LT_TexturePaletteOnce", "LT_TexturePaletteLoop")
_SHIPPED_LIGHT_EFFECTS = (
    "LE_None", "LE_TorchWaver", "LE_FireWaver", "LE_WateryShimmer",
    "LE_Searchlight", "LE_SlowWave", "LE_FastWave", "LE_CloudCast",
    "LE_StaticSpot", "LE_Shock", "LE_Disco", "LE_Warp", "LE_Spotlight",
    "LE_NonIncidence", "LE_Shell", "LE_OmniBumpMap", "LE_Interference",
    "LE_Cylinder", "LE_Rotor", "LE_Unused")


def _canonical_enum(value, table, prefix):
    text = str(value)
    if text.isdigit():
        index = int(text)
        if 0 <= index < len(table):
            return table[index]
        raise ValueError(f"{prefix} byte {index} outside the shipped enum")
    return text


def light_anim_row(resolved):
    """Return the positional animation row parallel to one light actor name."""
    return [
        _canonical_enum(resolved["LightType"], _SHIPPED_LIGHT_TYPES, "LightType"),
        int(resolved["LightPeriod"]), int(resolved["LightPhase"]),
        _canonical_enum(resolved["LightEffect"], _SHIPPED_LIGHT_EFFECTS, "LightEffect"),
    ]


def _texture_frictions(manifest_root):
    """Return friction lookups plus the manifest package census from utx2png.

    Exact Package.Group.Name is authoritative. The flat fallback matches the
    established bsp2obj resolver for old manifests, whose exported PNG layout
    is Package/Name and whose package census has no colliding grouped leaves.
    """
    exact = {}
    flat = {}
    if manifest_root is None:
        raise ValueError("texture manifest root is required")
    manifest_paths = sorted(
        Path(manifest_root).glob("*/_manifest.json"),
        key=lambda path: str(path).casefold())
    if not manifest_paths:
        raise FileNotFoundError(
            f"texture manifest root has no */_manifest.json: {manifest_root}")
    packages = set()
    for manifest_path in manifest_paths:
        package = manifest_path.parent.name
        packages.add(package.casefold())
        data = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        for name, info in data.get("textures", {}).items():
            friction = float(info.get("friction", 1.0))
            object_path = info.get("object_path", f"{package}.{name}")
            exact[str(object_path).casefold()] = friction
            flat[(package.casefold(), name.casefold())] = friction
    return exact, flat, packages


def _surface_friction(pkg, texture_ref, lookups):
    exact, flat, packages = lookups
    if not texture_ref:
        return 1.0
    path = pkg.objpath(texture_ref)
    identity = pkg.objref(texture_ref)
    if identity and identity[0].casefold() not in packages:
        raise FileNotFoundError(
            f"texture package {identity[0]!r} referenced by "
            f"{pkg.name}.{path or identity[1]} has no _manifest.json")
    if path:
        # Same-package exports have no package object in their outer chain.
        if not identity or path.split(".", 1)[0].casefold() != identity[0].casefold():
            path = f"{pkg.name}.{path}"
        value = exact.get(path.casefold())
        if value is not None:
            return value
    if identity:
        return flat.get((identity[0].casefold(), identity[1].casefold()), 1.0)
    return 1.0


def extract(wot_path, texture_manifest_root=None):
    # A sparse friction channel cannot distinguish a valid all-default map from
    # a missing pipeline input, so validate manifests before decoding the map.
    friction_lookups = _texture_frictions(texture_manifest_root)
    pkg = Package(wot_path)
    models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
    if not models:
        return {"zones": [], "ambient_zones": [], "zone_bsp": [],
                "leaf_bsp": {"root_outside": 0, "num_leaves": 0,
                             "nodes": []},
                "check_walk_surfaces": True,
                "friction_nodes": [],
                "light_actors": [], "light_props": [], "light_anim": [],
                "leaf_lights": {}}
    bsp = decode_model_full(pkg, max(models, key=lambda e: e["size"]))
    points, verts, nodes, zones = bsp["points"], bsp["verts"], bsp["nodes"], bsp["zones"]

    # AABB per zone number from the points of every node bordering that zone.
    nz = len(zones)
    lo = [[1e18, 1e18, 1e18] for _ in range(nz)]
    hi = [[-1e18, -1e18, -1e18] for _ in range(nz)]
    seen = [False] * nz
    for ivp, isurf, nv, _plane, izone in nodes:
        if nv < 1:
            continue
        for zi in izone:                      # (back, front)
            if not (0 <= zi < nz):
                continue
            seen[zi] = True
            for k in range(nv):
                px, py, pz = points[verts[ivp + k][0]]
                lo[zi][0] = min(lo[zi][0], px); hi[zi][0] = max(hi[zi][0], px)
                lo[zi][1] = min(lo[zi][1], py); hi[zi][1] = max(hi[zi][1], py)
                lo[zi][2] = min(lo[zi][2], pz); hi[zi][2] = max(hi[zi][2], pz)

    by_name = {e["name"]: e for e in pkg.exports}
    gameplay = []
    for z in range(nz):
        if not seen[z] or not zones[z]:
            continue
        aname = pkg.objname(zones[z])
        e = by_name.get(aname)
        if not e:
            continue
        cls = pkg.objname(e["cls"]) or ""
        # Effect-bearing ZoneInfo family only (incl. WaterZone); excludes Sky,
        # Ways, and Warp zones (see EFFECT_ZONE_CLASSES).
        if cls not in EFFECT_ZONE_CLASSES:
            continue
        zi = _read_zoneinfo(pkg, e, cls)
        grav = zi["gravity"]
        vel = zi["velocity"]
        has_effect = (zi["water"] or zi["pain"] or zi["heal"] or zi["fog"]
                      or zi["ground_friction"] != 8.0
                      or abs(zi["fluid_friction"] - 1.2) > 1e-6
                      or (grav and any(grav)) or (vel and any(vel)))
        # WOTZoneInfo also defines the CitadelEditor region even when it carries
        # no movement/environment effect. WaterZone derives from WOTZoneInfo.
        has_citadel_metadata = cls in {"WOTZoneInfo", "WaterZone"}
        if not has_effect and not has_citadel_metadata:
            continue
        # sanity: the ZoneInfo should sit inside its own zone's AABB.
        L = zi["location"]
        inside = all(lo[z][i] - 1.0 <= L[i] <= hi[z][i] + 1.0 for i in range(3))
        gameplay.append(dict(zone=z, actor=aname, aabb=[lo[z], hi[z]],
                             location=L, location_inside=inside,
                             water=zi["water"], pain=zi["pain"], fog=zi["fog"],
                             damage_per_sec=zi["damage_per_sec"],
                             damage_type=zi["damage_type"],
                             heal=zi["heal"], heal_per_sec=zi["heal_per_sec"],
                             heal_sound=zi["heal_sound"],
                             ground_friction=zi["ground_friction"],
                             fluid_friction=zi["fluid_friction"],
                             gravity=grav or [], velocity=vel or [],
                             edit_team=zi["edit_team"],
                             min_edit_distance=zi["min_edit_distance"],
                             edit_view_angle_pitch=zi["edit_view_angle_pitch"],
                             edit_view_distance=zi["edit_view_distance"],
                             allow_editing=zi["allow_editing"]))

    level = next((e for e in pkg.exports
                  if pkg.objname(e["cls"]) == "LevelInfo"), None)
    level_name = level["name"] if level else ""
    level_class = pkg.objname(level["cls"]) if level else "LevelInfo"
    level_props = PayloadReader(pkg, level).properties() if level else {}
    check_walk_surfaces = bool(level_props.get("bCheckWalkSurfaces", True))
    ideal_player_count = _c_atoi(level_props.get("IdealPlayerCount"))
    level_ambient = _read_ambient(pkg, level, level_class)
    ambient_zones = []
    for z in range(max(nz, 1)):
        export = None
        actor_name = ""
        actor_class = ""
        if z != 0 and z < nz and zones[z]:
            actor_name = pkg.objname(zones[z]) or ""
            export = by_name.get(actor_name)
            if export:
                actor_class = pkg.objname(export["cls"]) or ""
        if z == 0:
            export, actor_name, actor_class = level, level_name, level_class
        ambient = (_read_ambient(pkg, export, actor_class)
                   if export else level_ambient)
        ambient_zones.append(dict(
            zone=z, actor_class=actor_class, actor=actor_name,
            ambient_brightness=ambient["AmbientBrightness"],
            ambient_hue=ambient["AmbientHue"],
            ambient_saturation=ambient["AmbientSaturation"],
            min_light_count=ambient["MinLightCount"],
            max_light_count=ambient["MaxLightCount"]))

    zone_bsp = []
    for node_index, plane in enumerate(bsp["node_planes"]):
        back_child, front_child = bsp["node_children"][node_index]
        back_zone, front_zone = nodes[node_index][4]
        zone_bsp.append(dict(
            plane=list(plane),
            front=(front_child if front_child >= 0
                   else _leaf_marker(front_zone)),
            back=(back_child if back_child >= 0
                  else _leaf_marker(back_zone))))

    # Lighting calls the zero-extent model LineCheck with ExtraNodeFlags=0.
    # FBspNode::IsCsg is therefore NumVertices>0 with non-solid/new nodes
    # excluded. This single bit beside iLeaf[2] avoids a duplicate solid tree.
    leaf_nodes = []
    for node_index, node_leaves in enumerate(bsp["node_leaves"]):
        num_vertices = nodes[node_index][2]
        flags = bsp["node_flags"][node_index]
        is_csg = int(num_vertices > 0
                     and not (flags & (NF_NOT_CSG | NF_IS_NEW)))
        leaf_nodes.append([int(node_leaves[0]), int(node_leaves[1]), is_csg])

    # CheckSurfaces/TraceTexture resolves Hit.Item -> node -> iSurf -> Texture
    # only for the LevelInfo world model (the original engine gates on the
    # hit actor). Preserve that exact node identity; do not project friction onto
    # imported render triangles or movers.
    friction_nodes = []
    for node_index, (_ivp, surface_index, _nv, _plane, _zones) in enumerate(nodes):
        if not 0 <= surface_index < len(bsp["surfs"]):
            continue
        friction = _surface_friction(
            pkg, bsp["surfs"][surface_index]["tex"], friction_lookups)
        if abs(friction - 1.0) > 1e-6:
            friction_nodes.append([node_index, friction])

    light_actors = []
    light_actor_indices = {}

    def intern_light(name):
        if name not in light_actor_indices:
            light_actor_indices[name] = len(light_actors)
            light_actors.append(name)
        return light_actor_indices[name]

    leaf_lights = {}
    model_lights = bsp["lights"]
    for leaf_index, (_zone, permeating, _volumetric) in enumerate(bsp["leaves"]):
        if permeating < 0:
            continue
        if permeating >= len(model_lights):
            raise ValueError(f"Leaf[{leaf_index}].iPermeating {permeating} "
                             f"outside Lights[{len(model_lights)}]")
        run = []
        light_index = permeating
        while light_index < len(model_lights) and model_lights[light_index]:
            name = pkg.objname(model_lights[light_index])
            if not name:
                raise ValueError(f"Leaf[{leaf_index}] light ref "
                                 f"{model_lights[light_index]} has no name")
            run.append(intern_light(name))
            light_index += 1
        if light_index >= len(model_lights):
            raise ValueError(f"Leaf[{leaf_index}] permeating run is not "
                             "NULL-terminated")
        leaf_lights[str(leaf_index)] = run

    # Lightning.uc finds actors by Tag and folds only LightType to LT_None on
    # its first tick.  Those actors remain in the retail consider list (raw
    # brightness/radius intact), but AddLight rejects them at rest.
    lightning_tags = set()
    for export in pkg.exports:
        if (pkg.objname(export["cls"]) or "").lower() != "lightning":
            continue
        tag = PayloadReader(pkg, export).properties().get("LightTag", "")
        if tag:
            lightning_tags.add(str(tag).lower())

    light_props = []
    light_anim = []
    for name in light_actors:
        export = by_name.get(name)
        if export is None:
            raise ValueError(f"Permeating light {name} has no actor export")
        cls = pkg.objname(export["cls"]) or "Light"
        props = PayloadReader(pkg, export).properties()
        # resolve_light is the same class-default/.uc-chain authority used by
        # the lightmap baker and importer.  Location is irrelevant to this
        # compact property table, but the resolver's actor contract includes it.
        resolved = resolve_light(dict(
            cls=cls, name=name, props=props, location=(0.0, 0.0, 0.0),
            rotation=(0.0, 0.0, 0.0)))
        brightness = int(resolved["LightBrightness"])
        radius = world_light_radius(cls, resolved["LightRadius"])
        special = int(bool(resolved["bSpecialLit"]))
        gated = str(props.get("Tag", "")).lower() in lightning_tags
        emits = (brightness != 0 and not gated
                 and str(resolved["LightType"]).lower()
                 not in ("lt_none", "none"))
        # Positional schema: raw brightness, world radius, special-lit, emits.
        light_props.append([brightness, radius, special, emits])
        # Parallel positional animation schema. Keep light_props unchanged: it
        # is consumed by the existing mover/mesh priority path.
        light_anim.append(light_anim_row(resolved))
    return {"zones": gameplay, "ambient_zones": ambient_zones,
            "zone_bsp": zone_bsp,
            "leaf_bsp": {"root_outside": int(bool(bsp["root_outside"])),
                         "num_leaves": len(bsp["leaves"]),
                         "nodes": leaf_nodes},
            "check_walk_surfaces": check_walk_surfaces,
            "ideal_player_count": ideal_player_count,
            "friction_nodes": friction_nodes,
            "light_actors": light_actors, "light_props": light_props,
            "light_anim": light_anim,
            "leaf_lights": leaf_lights}


def _dump(data):
    """Readable zone sections; BSP/run data compact one-line-per-row
    (indent=1 on ~150k nodes across the 19 maps ballooned the exports to
    ~21MB / 1.5M lines; the tree is machine-consumed only)."""
    bsp_rows = ",\n  ".join(
        json.dumps(n, separators=(",", ":")) for n in data["zone_bsp"])
    leaf_bsp = data["leaf_bsp"]
    leaf_rows = ",\n  ".join(
        json.dumps(n, separators=(",", ":")) for n in leaf_bsp["nodes"])
    run_rows = ",\n  ".join(
        json.dumps(leaf) + ":" + json.dumps(run, separators=(",", ":"))
        for leaf, run in data["leaf_lights"].items())
    return (
        '{\n"zones": ' + json.dumps(data["zones"], indent=1)
        + ',\n"ambient_zones": ' + json.dumps(data["ambient_zones"], indent=1)
        + ',\n"zone_bsp": [\n  ' + bsp_rows + '\n ]'
        + ',\n"leaf_bsp":{"root_outside":'
        + str(int(bool(leaf_bsp["root_outside"])))
        + ',"num_leaves":' + str(int(leaf_bsp["num_leaves"]))
        + ',"nodes":[\n  ' + leaf_rows + '\n ]}'
        + ',\n"check_walk_surfaces":'
        + json.dumps(bool(data.get("check_walk_surfaces", True)))
        + ',\n"ideal_player_count":'
        + str(int(data.get("ideal_player_count", 0)))
        + ',\n"friction_nodes":'
        + json.dumps(data.get("friction_nodes", []), separators=(",", ":"))
        + ',\n"light_actors":'
        + json.dumps(data["light_actors"], separators=(",", ":"))
        + ',\n"light_props":'
        + json.dumps(data["light_props"], separators=(",", ":"))
        + ',\n"light_anim":'
        + json.dumps(data["light_anim"], separators=(",", ":"))
        + ',\n"leaf_lights":{\n  ' + run_rows + '\n }\n}'
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wot", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "texture_manifest_root", type=Path,
        help="root containing one <Package>/_manifest.json per referenced texture package")
    args = parser.parse_args(argv)

    data = extract(args.wot, args.texture_manifest_root)
    args.output.write_text(_dump(data), encoding="utf-8")
    print(f"{args.wot.stem}: {len(data['zones'])} zone volume(s), "
          f"{len(data['ambient_zones'])} ambient zone(s), "
          f"{len(data['zone_bsp'])} BSP node(s), "
          f"{len(data['friction_nodes'])} friction node(s), "
          f"{len(data['leaf_lights'])} permeating leaf run(s), "
          f"{len(data['light_actors'])} referenced light actor(s)")
    for d in data["zones"]:
        eff = []
        if d["water"]:
            eff.append("water")
        if d["pain"]:
            t = f":{d['damage_type']}" if d["damage_type"] else ""
            eff.append(f"pain{d['damage_per_sec']}{t}")
        if d["heal"]:
            eff.append(f"heal{d['heal_per_sec']}")
        if d["ground_friction"] != 8.0:
            eff.append(f"friction{d['ground_friction']}")
        if abs(d["fluid_friction"] - 1.2) > 1e-6:
            eff.append(f"fluid{d['fluid_friction']}")
        if d["gravity"] and any(d["gravity"]):
            eff.append("gravity")
        if d["velocity"] and any(d["velocity"]):
            eff.append("current")
        sz = [round(d["aabb"][1][i] - d["aabb"][0][i]) for i in range(3)]
        print(f"  zone {d['zone']:2d} {d['actor']}: {'+'.join(eff)} "
              f"size={sz} inside={d['location_inside']}")


if __name__ == "__main__":
    main()
