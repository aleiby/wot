"""Offline presentation-parity census over the retail MP maps (no Unreal).

Two retail renderer facts the guarded runtime has to reproduce, read straight
from the shipped .wot packages (original/Maps) with ue1pkg/lightmaps:

1. Mirror surfaces.  A BSP surface whose PolyFlags carry PF_Mirrored
   (0x08000000, UE1 EPolyFlags) makes OccludeBsp create a mirror child
   frame: the scene re-rendered through the surface plane
   (Frame->Coords.MirrorByPlane, near clip = the plane) under the surface,
   which is then drawn over it with its own blend (the original renderer;
   DrawFrame draws children first).  A PF_Translucent mirror needs
   RenDev->ShinySurfaces, True for every hardware device in the shipped
   WoT.ini ([GlideDrv/D3DDrv/OpenGLDrv/MetalDrv] ShinySurfaces=True).  The
   texture's own PolyFlags are OR-ed in but no shipped texture sets bit 27
   (bX3 in the UE1 Texture bool layout): the census uses the surface flags.

   The guarded runtime sees a slot's LAST-WRITER flags only (the native BSP
   builder's MaterialFamilyMask does not include PF_Mirrored), so the census
   also proves every mirrored slot is pure: all of its surfaces are mirrored.

2. Volumetric fog zoning.  Retail builds a surface's fog map only when the
   viewer's zone is a fog zone (IsVolumetric) AND the surface's draw zone
   is one (the light manager's Zone=Draw->Zone) and the surface is not
   PF_Translucent.  fog_zone_facts() reports the zones a fog light's
   sphere reaches.

Usage: python presentation_census.py            # 19-map mirror table
       python presentation_census.py --fog Arena_07 Light10
"""
import collections
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lightmaps as lmz  # noqa: E402
from ue1pkg import Package, PayloadReader  # noqa: E402
from wot_config import original_dir  # noqa: E402

ORIGINAL = original_dir()

PF_INVISIBLE = 0x00000001
PF_MASKED = 0x00000002
PF_TRANSLUCENT = 0x00000004
PF_MODULATED = 0x00000040
PF_FAKEBACKDROP = 0x00000080
PF_TWOSIDED = 0x00000100
PF_SPECIALLIT = 0x00100000
PF_UNLIT = 0x00400000
PF_PORTAL = 0x04000000
PF_MIRRORED = 0x08000000

MP_MAPS = (
    "Arena_01", "Arena_02", "Arena_03", "Arena_04", "Arena_05", "Arena_06",
    "Arena_07", "Arena_08", "Arena_09", "Arena_11", "Arena_12",
    "Battle2_01", "Battle2_02", "Battle2_03", "Battle2_04", "Battle2_05",
    "Battle3_01", "Battle3_02", "Battle4_01",
)


def flag_suffix(flags):
    """The native builder's slot suffix (BSPWorld.cpp WoTWorldMaterialSlot)."""
    suffix = ""
    if flags & PF_MASKED:
        suffix += "_M"
    if flags & PF_TRANSLUCENT:
        suffix += "_T"
    if flags & PF_MODULATED:
        suffix += "_Mod"
    if flags & PF_TWOSIDED:
        suffix += "_2S"
    if flags & PF_UNLIT:
        suffix += "_U"
    if flags & PF_FAKEBACKDROP:
        suffix += "_Sky"
    if flags & PF_SPECIALLIT:
        suffix += "_SL"
    return suffix


def mirror_eligible(flags):
    """The composite the runtime reproduces: a translucent one-sided mirror
    (every census surface).  Opaque (pure reflection), masked, modulated and
    two-sided mirrors would each need their own derivation; none ships."""
    return (bool(flags & PF_MIRRORED) and bool(flags & PF_TRANSLUCENT)
            and not flags & (PF_TWOSIDED | PF_INVISIBLE | PF_FAKEBACKDROP))


def _plane_key(normal, distance):
    return (tuple(round(c, 4) + 0.0 for c in normal), round(distance, 2) + 0.0)


def load_main_model(map_path):
    pkg = Package(str(map_path))
    models = [e for e in pkg.exports if pkg.objname(e["cls"]) == "Model"]
    bsp = lmz.decode_model_full(pkg, max(models, key=lambda e: e["size"]))
    return pkg, bsp


def mirror_census(map_path):
    """Every PF_Mirrored surface of the map's level model."""
    pkg, bsp = load_main_model(map_path)
    zone_names = [pkg.objname(z) if z else None for z in bsp["zones"]]
    slot_nodes = collections.defaultdict(list)     # (part, slot) -> flags
    surfaces = {}
    for node in bsp["nodes"]:
        _ivp, isurf, vertex_count, _normal, izone = node
        if vertex_count < 3:
            continue
        surf = bsp["surfs"][isurf]
        flags = surf["flags"]
        ref = pkg.objref(surf["tex"])
        base = f"{ref[0]}__{ref[1]}" if ref else "__None"
        part = "portal" if flags & PF_PORTAL else "main"
        slot = base + flag_suffix(flags)
        if not flags & PF_INVISIBLE:
            slot_nodes[(part, slot)].append(flags)
        if not flags & PF_MIRRORED:
            continue
        normal = bsp["vectors"][surf["vnorm"]]
        point = bsp["points"][surf["pbase"]]
        distance = sum(n * p for n, p in zip(normal, point))
        row = surfaces.setdefault(isurf, dict(
            surface=isurf, flags=flags, texture=base, slot=slot, part=part,
            eligible=mirror_eligible(flags), nodes=0, zones=set(),
            plane=_plane_key(normal, distance)))
        row["nodes"] += 1
        row["zones"].update(zone_names[z] for z in izone
                            if 0 < z < len(zone_names) and zone_names[z])
    rows = []
    for row in sorted(surfaces.values(), key=lambda r: (r["part"], r["slot"],
                                                         r["surface"])):
        members = slot_nodes.get((row["part"], row["slot"]), [])
        row["slot_pure"] = bool(members) and all(
            f & PF_MIRRORED for f in members)
        row["slot_last_writer_mirrored"] = bool(members) and bool(
            members[-1] & PF_MIRRORED)
        row["zones"] = sorted(row["zones"])
        rows.append(row)
    planes = sorted({r["plane"] for r in rows if r["eligible"]})
    return dict(surfaces=rows, planes=planes)


def census_all(maps=MP_MAPS):
    return {m: mirror_census(ORIGINAL / "Maps" / f"{m}.wot")
            for m in maps}


def _zone_props(pkg, zone_ref):
    if not zone_ref or zone_ref <= 0:
        return {}
    export = pkg.exports[zone_ref - 1]
    return PayloadReader(pkg, export).properties()


def _actor_location(pkg, name):
    for export in pkg.exports:
        if export["name"] != name or export["size"] <= 0:
            continue
        props = PayloadReader(pkg, export).properties()
        raw = props.get("_structs", {}).get("Location", {}).get(0)
        if raw is not None and len(raw) >= 12:
            return struct.unpack_from("<3f", raw, 0), props
    return None, None


def fog_zone_facts(map_path, light_name, viewer=None):
    """Zones around one volumetric light: which zones are bFogZone, the
    light's own zone, the viewer's zone, and per draw-zone the rendered
    (non-invisible) node polygons the fog sphere reaches."""
    pkg, bsp = load_main_model(map_path)
    zone_names = [pkg.objname(z) if z else None for z in bsp["zones"]]
    fog_zones = sorted(
        zone_names[i] for i, ref in enumerate(bsp["zones"])
        if ref and zone_names[i]
        and bool(_zone_props(pkg, ref).get("bFogZone", False)))
    location, props = _actor_location(pkg, light_name)
    if location is None:
        raise ValueError(f"{light_name} has no serialized Location")
    volume_radius = int(props.get("VolumeRadius", 0))
    radius = 25.0 * (volume_radius + 1)   # AActor::WorldVolumetricRadius

    def zone_of(point):
        return zone_names[lmz.point_zone(bsp, point)]

    reached = collections.Counter()
    portals = []
    for node in bsp["nodes"]:
        ivp, isurf, vertex_count, _normal, izone = node
        if vertex_count < 3:
            continue
        flags = bsp["surfs"][isurf]["flags"]
        points = [bsp["points"][bsp["verts"][ivp + i][0]]
                  for i in range(vertex_count)]
        nearest = min(sum((p[k] - location[k]) ** 2 for k in range(3))
                      for p in points) ** 0.5
        centroid = [sum(p[k] for p in points) / vertex_count for k in range(3)]
        center = sum((centroid[k] - location[k]) ** 2 for k in range(3)) ** 0.5
        if min(nearest, center) > radius:
            continue
        if flags & PF_PORTAL:
            portals.append(dict(
                flags=flags, zones=sorted(zone_names[z] or "" for z in izone),
                z=sorted({round(p[2], 1) for p in points})))
        if flags & PF_INVISIBLE:
            continue
        # The draw zone is the zone on the polygon's visible (front) side.
        reached[zone_names[izone[1]]] += 1
    return dict(
        fog_zones=fog_zones, light_zone=zone_of(location),
        viewer_zone=zone_of(viewer) if viewer is not None else None,
        radius=radius, surfaces_by_draw_zone=dict(reached),
        portals_in_reach=portals)


def main(argv):
    if len(argv) >= 3 and argv[0] == "--fog":
        facts = fog_zone_facts(ORIGINAL / "Maps" / f"{argv[1]}.wot",
                               argv[2])
        print(json.dumps(facts, indent=1))
        return 0
    for name, census in census_all().items():
        rows = census["surfaces"]
        print(f"{name}: {len(rows)} mirrored surfaces, "
              f"{sum(r['nodes'] for r in rows)} nodes, "
              f"{len(census['planes'])} planes")
        for row in rows:
            print(f"   {row['part']:6} {row['slot']:32} flags=0x{row['flags']:08x}"
                  f" nodes={row['nodes']:2} plane={row['plane']}"
                  f" pure={row['slot_pure']} zones={row['zones']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
