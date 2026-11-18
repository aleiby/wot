#!/usr/bin/env python3
"""Offline census for the P30 runtime light-waveform design.

This consumes only checked-in T3D/zones exports and retail ``.wot`` packages;
it never imports Unreal Python or starts the editor.

Usage:
    python Tools/convert/runtime_light_waveform_census.py
    python Tools/convert/runtime_light_waveform_census.py --fast

The default run also recomposes the five pulse maps at mean/high/low to measure
whether the existing endpoint lerp is byte-identical to the static mean.  Use
``--fast`` to skip that approximately 25-second check.
"""

import argparse
import builtins
from collections import Counter, defaultdict
import io
import json
import re
import sys
from pathlib import Path
from unittest import mock

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
TOOLS_DIR = HERE.parents[0]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import bsp2obj as B  # noqa: E402
import lightmaps as L  # noqa: E402
import skygeo as SG  # noqa: E402
from bsp_linecheck import BspTree  # noqa: E402
from light_animation import SHIMMER_EFFECTS, WAVE_TYPES  # noqa: E402
from lightmap_atlas import SIZES, pack  # noqa: E402
from mesh_light_select import select_mesh_lights  # noqa: E402
from mover_light_census import (  # noqa: E402
    MODEL_RE, MOVER_CLASSES, _actor_pose,
)
from mover_light_select import (  # noqa: E402
    select_mover_surface_lights, transform_plane,
)
from t3d import parse, typed  # noqa: E402
from ue1pkg import PF_INVISIBLE, Package, decode_model  # noqa: E402
from wot_config import reference_root  # noqa: E402


MESH_RE = re.compile(r"Mesh'[\w\-]+\.([\w\-]+)'")
CLASS_RE = re.compile(
    r"^\s*class\s+\w+\s+(?:expands|extends)\s+(\w+)", re.M | re.I)
DEFAULT_RE = re.compile(r"^defaultproperties\s*\{(.*?)^\}", re.M | re.S | re.I)


def _maps(repo):
    return sorted(path.name.removesuffix(".zones.json")
                  for path in (repo / "exports/zones").glob("*.zones.json"))


def _wave_key(resolved):
    return (resolved["LightType"], int(resolved["LightPeriod"]),
            int(resolved["LightPhase"]))


_CLASS_PROP_CACHE = {}


def _class_prop(cls, name, default):
    """Resolve an arbitrary scalar default through the shipped/dev .uc chain."""
    key = (cls.lower(), name.lower())
    if key in _CLASS_PROP_CACHE:
        return _CLASS_PROP_CACHE[key]
    chain = []
    current = cls
    seen = set()
    while current and current.lower() not in seen:
        seen.add(current.lower())
        path = L._find_uc(current)  # same authority/routing as resolve_light
        parent = None
        value = None
        if path:
            text = path.read_text(errors="replace")
            match = CLASS_RE.search(text)
            parent = match.group(1) if match else None
            block = DEFAULT_RE.search(text)
            if block:
                for line in block.group(1).splitlines():
                    prop, sep, raw = line.partition("=")
                    if sep and prop.strip().lower() == name.lower():
                        value = raw.split("//", 1)[0].strip()
        chain.append(value)
        current = parent
    result = default
    for value in reversed(chain):
        if value is not None:
            result = value
    _CLASS_PROP_CACHE[key] = result
    return result


def _bool_prop(actor, name, default=False):
    value = actor["props"].get(
        name, _class_prop(actor["cls"], name, default))
    return value is True or str(value).lower() == "true"


def _active_light(actor, resolved, lightning_tags):
    return (int(resolved["LightBrightness"]) > 0
            and resolved["LightType"] in L.LIGHT_TYPE_MEAN
            and str(actor["props"].get("Tag", "")).lower()
            not in lightning_tags)


def light_inventory(actors):
    # T3D can repeat an object name; the runtime/export tables use the last row.
    by_name = {actor["name"]: actor for actor in actors}
    lightning_tags = {
        str(actor["props"].get("LightTag", "")).lower()
        for actor in actors if actor["cls"].lower() == "lightning"
    } - {""}
    groups = Counter()
    group_colors = defaultdict(Counter)
    authored_effects = Counter()
    active_effects = Counter()
    intersections = Counter()
    resolved_by_name = {}
    for actor in by_name.values():
        resolved = L.resolve_light(actor)
        resolved_by_name[actor["name"]] = resolved
        effect = resolved["LightEffect"]
        if effect in SHIMMER_EFFECTS:
            authored_effects[effect] += 1
        if not _active_light(actor, resolved, lightning_tags):
            continue
        wave = resolved["LightType"] in WAVE_TYPES
        if wave:
            key = _wave_key(resolved)
            groups[key] += 1
            group_colors[key][
                (int(resolved["LightHue"]),
                 int(resolved["LightSaturation"]))] += 1
        if effect in SHIMMER_EFFECTS:
            active_effects[effect] += 1
            if wave:
                intersections[(_wave_key(resolved), effect)] += 1
    return dict(
        groups=groups, group_colors=group_colors,
        authored_effects=authored_effects, active_effects=active_effects,
        intersections=intersections, resolved_by_name=resolved_by_name,
    )


def atlas_shape(bsp):
    rects = [("__fullbright__", 3, 3)]
    for surface_index, surface in enumerate(bsp["surfs"]):
        if surface["ilightmap"] < 0:
            continue
        lightmap = bsp["lightmap"][surface["ilightmap"]]
        rects.append((surface_index, int(lightmap["uclamp"]) + 2,
                      int(lightmap["vclamp"]) + 2))
    for width, height in SIZES:
        if pack(rects, width, height) is not None:
            return width, height, len(rects) - 1
    raise ValueError("lightmap rectangles exceed the supported atlas sizes")


def _zone_speeds(actors):
    return {
        actor["name"].lower(): (
            float(actor["props"].get("TexUPanSpeed", 1.0)),
            float(actor["props"].get("TexVPanSpeed", 1.0)),
        )
        for actor in actors
    }


def panning_inventory(package, bsp, actors):
    sky_zone, winner = SG.sky_zone_of(package, bsp, actors)
    sky_ids = (set() if sky_zone is None
               else SG.sky_surface_ids(bsp, sky_zone))
    visible = {
        int(node[1]) for node in bsp["nodes"]
        if int(node[2]) >= 3
        and not (bsp["surfs"][int(node[1])]["flags"] & PF_INVISIBLE)
    }
    counts = Counter()
    for surface_index in visible:
        bits = int(bsp["surfs"][surface_index]["flags"]) & 0x600
        if not bits:
            continue
        counts["sky" if surface_index in sky_ids else "world"] += 1
        counts[{0x200: "u", 0x400: "v", 0x600: "uv"}[bits]] += 1

    # Reproduce bsp2obj's material key and majority-zone selection, then check
    # whether one MIC is being asked to represent different authored rates.
    material_for_surface = {}
    world_materials = set()
    sky_materials = set()
    for surface_index in visible:
        surface = bsp["surfs"][surface_index]
        source_package, texture, _w, _h, _scale = B.resolve_tex(
            package, surface["tex"], {})
        material = (Package.qualkey(source_package, texture)
                    + B.flag_suffix(surface["flags"]))
        material_for_surface[surface_index] = material
        (sky_materials if surface_index in sky_ids
         else world_materials).add(material)
    split = sky_materials & world_materials
    zone_names = [package.objname(ref) if ref else None
                  for ref in (bsp.get("zones") or [])]
    votes = defaultdict(Counter)
    slot_flags = {}
    for node in bsp["nodes"]:
        surface_index = int(node[1])
        if surface_index not in visible:
            continue
        surface = bsp["surfs"][surface_index]
        bits = int(surface["flags"]) & 0x600
        if not bits:
            continue
        material = material_for_surface[surface_index]
        if surface_index in sky_ids and material in split:
            material += B.SKY_SUFFIX
        slot_flags[material] = bits
        if len(node) > 4:
            for zone in tuple(node[4]):
                if zone and zone < len(zone_names) and zone_names[zone]:
                    votes[material][zone_names[zone]] += 1
    speeds = _zone_speeds(actors)
    conflicts = 0
    conflict_details = []
    zero_direction_slots = 0
    for material, zone_votes in votes.items():
        rates = {speeds.get(name.lower(), (1.0, 1.0))
                 for name in zone_votes}
        if len(rates) > 1:
            conflicts += 1
            conflict_details.append(dict(
                material=material,
                zones=", ".join(
                    f"{name}={speeds.get(name.lower(), (1.0, 1.0))}"
                    f" x{count}"
                    for name, count in zone_votes.most_common()),
                chosen=zone_votes.most_common(1)[0][0],
            ))
        chosen = zone_votes.most_common(1)[0][0]
        rate_u, rate_v = speeds.get(chosen.lower(), (1.0, 1.0))
        flags = slot_flags[material]
        zero_direction_slots += int(
            bool(flags & 0x200) and rate_u == 0.0
            or bool(flags & 0x400) and rate_v == 0.0)

    # Mover brushes have their own UModels and material family.
    exports_by_name = {export["name"]: export for export in package.exports}
    mover_actors = mover_surfaces = 0
    for actor in actors:
        if actor["cls"] not in MOVER_CLASSES:
            continue
        match = MODEL_RE.search(actor["props"].get("Brush", ""))
        if not match or match.group(1) not in exports_by_name:
            continue
        mover_bsp = decode_model(package, exports_by_name[match.group(1)])
        affected = sum(bool(int(surface["flags"]) & 0x600)
                       for surface in mover_bsp["surfs"])
        mover_actors += int(affected > 0)
        mover_surfaces += affected
    return dict(
        world_surfaces=counts["world"], sky_surfaces=counts["sky"],
        u_surfaces=counts["u"], v_surfaces=counts["v"],
        uv_surfaces=counts["uv"], pan_slots=len(votes),
        rate_conflicts=conflicts, zero_direction_slots=zero_direction_slots,
        conflict_details=conflict_details,
        mover_actors=mover_actors, mover_surfaces=mover_surfaces,
        mesh_flag_consumers=0,
    )


def _zone_light_cap(zone_data, zone_number):
    for row in zone_data.get("ambient_zones", []):
        if int(row.get("zone", -1)) == int(zone_number):
            return max(1, min(int(row.get("max_light_count", 6)), 16))
    return 6


def pulse_consumer_inventory(repo, map_name, package, actors, inventory,
                             class_to_mesh):
    zone_data = json.loads(
        (repo / f"exports/zones/{map_name}.zones.json").read_text())
    tree = BspTree.from_export(zone_data)
    actors_by_name = {actor["name"]: actor for actor in actors}
    lights = {}
    for index, name in enumerate(zone_data["light_actors"]):
        actor = actors_by_name[name]
        resolved = inventory["resolved_by_name"][name]
        brightness, radius, special, emits = zone_data["light_props"][index]
        lights[name] = dict(
            name=name, location=tuple(actor["location"]),
            brightness=int(brightness), radius=float(radius),
            special_lit=bool(special), emits=bool(emits),
            pulse=resolved["LightType"] in WAVE_TYPES,
            group=(_wave_key(resolved)
                   if resolved["LightType"] in WAVE_TYPES else None),
        )

    def actor_lights(actor, location):
        region = tree.point_region(location)
        run = zone_data["leaf_lights"].get(str(region.leaf), [])
        names = [zone_data["light_actors"][int(index)] for index in run]
        return select_mesh_lights(
            actor_location=location,
            actor_special_lit=_bool_prop(actor, "bSpecialLit"),
            max_lights=_zone_light_cap(zone_data, region.zone),
            permeating_names=names, lights_by_name=lights,
            line_check=tree.line_check,
        )

    mesh_consumers = []
    for actor in actors:
        if (actor["cls"].lower() == "raincyd"
                or actor["cls"] in MOVER_CLASSES
                or _bool_prop(actor, "bUnlit")):
            continue
        match = MESH_RE.search(actor["props"].get("Mesh", ""))
        mesh_name = (match.group(1) if match
                     else class_to_mesh.get(actor["cls"].lower()))
        if not mesh_name:
            continue
        pulse = {light["name"] for light in actor_lights(
            actor, tuple(actor["location"])) if light["pulse"]}
        if pulse:
            mesh_consumers.append((actor["name"], pulse))

    mover_consumers = []
    surface_links = 0
    exports_by_name = {export["name"]: export for export in package.exports}
    for actor in actors:
        if actor["cls"] not in MOVER_CLASSES or _bool_prop(actor, "bUnlit"):
            continue
        match = MODEL_RE.search(actor["props"].get("Brush", ""))
        if not match or match.group(1) not in exports_by_name:
            continue
        location, rotation = _actor_pose(actor)
        pulse = set()
        if actor["props"].get("bDynamicLightMover", "").lower() == "true":
            region = tree.point_region(location)
            run = zone_data["leaf_lights"].get(str(region.leaf), [])
            permeating = [lights[zone_data["light_actors"][int(index)]]
                          for index in run]
            mover_bsp = decode_model(
                package, exports_by_name[match.group(1)])
            groups, flags, _stats = B.build_groups(
                package, mover_bsp, {}, include_planes=True)
            _split, _flags, surfaces, _extras = B.split_mover_groups_by_plane(
                groups, flags, mover_bsp["points"])
            for surface in surfaces:
                origin, normal = transform_plane(
                    location, rotation, surface["origin"], surface["normal"])
                selected = select_mover_surface_lights(
                    permeating, origin, normal,
                    _bool_prop(actor, "bSpecialLit"), emitting_only=True)
                selected_pulse = {light["name"] for light in selected
                                  if light["pulse"]}
                surface_links += len(selected_pulse)
                pulse.update(selected_pulse)
        else:
            pulse = {light["name"] for light in actor_lights(actor, location)
                     if light["pulse"]}
        if pulse:
            mover_consumers.append((actor["name"], pulse))

    all_links = mover_consumers + mesh_consumers
    reached = set().union(*(lights for _name, lights in all_links)) \
        if all_links else set()
    return dict(
        mover_consumers=len(mover_consumers),
        mover_links=sum(len(lights) for _name, lights in mover_consumers),
        mover_surface_links=surface_links,
        mesh_consumers=len(mesh_consumers),
        mesh_links=sum(len(lights) for _name, lights in mesh_consumers),
        pulse_lights_reached=sorted(reached),
        mover_names=[name for name, _lights in mover_consumers],
        mover_links_by_name={name: sorted(linked_lights)
                             for name, linked_lights in mover_consumers},
        mesh_names=[name for name, _lights in mesh_consumers],
    )


def endpoint_check(repo, rows):
    results = []
    # compose_map currently accepts its actor semantics through a JSON path.
    # Intercept only that one open with an in-memory JSON stream: no scratch
    # conversion tree or temporary file is needed.
    original_open = builtins.open
    for row in rows:
        if not row["lights"]["groups"]:
            continue
        map_name = row["map"]
        actor_json = repo / f"__in_memory_{map_name}.t3d.json"
        payload = json.dumps({"actors": row["actors"]})

        def census_open(path, *open_args, **open_kwargs):
            try:
                matches = Path(path) == actor_json
            except TypeError:
                matches = False
            if matches:
                return io.StringIO(payload)
            return original_open(path, *open_args, **open_kwargs)

        map_path = repo / f"original/Maps/{map_name}.wot"
        context = L.load_level(map_path)
        with mock.patch("builtins.open", side_effect=census_open):
            mean, _stats = L.compose_map(
                map_path, actor_json, ctx=context, wave="mean")
            high, _stats = L.compose_map(
                map_path, actor_json, ctx=context, wave="hi")
            low, _stats = L.compose_map(
                map_path, actor_json, ctx=context, wave="lo")
        base = mean["texel_rgb"].astype(np.int16)
        hi = high["texel_rgb"].astype(np.int16)
        lo = low["texel_rgb"].astype(np.int16)
        changed = np.any(hi != lo, axis=1)
        # Each total byte is encoded as 2*total.  A 0.5 endpoint lerp is
        # therefore (hi+lo) in atlas-byte units; base is 2*mean.
        atlas_byte_error = np.max(
            np.abs(hi + lo - 2 * base), axis=1)
        mismatch = atlas_byte_error != 0
        results.append(dict(
            map=map_name, texels=len(base),
            affected=int(changed.sum()),
            freeze_mismatch=int(mismatch.sum()),
            affected_freeze_mismatch=int((mismatch & changed).sum()),
            max_atlas_byte_error=int(atlas_byte_error.max()),
        ))
    return results


def _effect_text(counter):
    short = {
        "LE_TorchWaver": "Torch", "LE_FireWaver": "Fire",
        "LE_WateryShimmer": "Water",
    }
    return ", ".join(f"{short[key]}:{counter[key]}"
                     for key in SHIMMER_EFFECTS if counter[key]) or "-"


def _group_text(counter):
    return "; ".join(
        f"{key[0].removeprefix('LT_')}/{key[1]}/{key[2]}:{count}"
        for key, count in sorted(counter.items())) or "-"


def _print_table(headers, records):
    print("| " + " | ".join(headers) + " |")
    print("|" + "|".join("---" for _ in headers) + "|")
    for record in records:
        print("| " + " | ".join(str(record.get(header, ""))
                                 for header in headers) + " |")


def collect_census(repo=None):
    """Collect the reusable 19-map census without printing or recomposition."""
    repo = reference_root(repo)
    class_to_mesh = json.loads(
        (repo / "exports/inventory/class2mesh.json").read_text())

    rows = []
    for map_name in _maps(repo):
        actors = typed(parse(repo / f"exports/t3d/{map_name}.t3d"))
        lights = light_inventory(actors)
        package, bsp = L.load_level(repo / f"original/Maps/{map_name}.wot")
        width, height, lightmapped_surfaces = atlas_shape(bsp)
        panning = panning_inventory(package, bsp, actors)
        consumers = pulse_consumer_inventory(
            repo, map_name, package, actors, lights, class_to_mesh)
        delta_count = (len(lights["groups"])
                       + len(lights["active_effects"])
                       + len(lights["intersections"]))
        atlas_mib = width * height * 4 / (1024 * 1024)
        rows.append(dict(
            map=map_name, actors=actors, lights=lights, panning=panning,
            consumers=consumers, width=width, height=height,
            lightmapped_surfaces=lightmapped_surfaces,
            delta_count=delta_count, atlas_mib=atlas_mib,
        ))
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", "--reference-root", dest="repo", type=Path,
                        help="private root (overrides WOT_REFERENCE_ROOT)")
    parser.add_argument("--fast", action="store_true",
                        help="skip mean/high/low recomposition")
    args = parser.parse_args(argv)
    repo = reference_root(args.repo)
    rows = collect_census(repo)

    print("\nWAVEFORM AND SHIMMER CENSUS")
    waveform_records = []
    for row in rows:
        lights = row["lights"]
        waveform_records.append({
            "map": row["map"], "wave groups": _group_text(lights["groups"]),
            "shimmer authored/active": (
                f"{_effect_text(lights['authored_effects'])} / "
                f"{_effect_text(lights['active_effects'])}"),
            "cross": sum(lights["intersections"].values()),
            "delta atlases": row["delta_count"],
            "delta MiB": f"{row['delta_count'] * row['atlas_mib']:.1f}",
            "opaque samplers": 4 + row["delta_count"],
        })
    _print_table([
        "map", "wave groups", "shimmer authored/active", "cross",
        "delta atlases", "delta MiB", "opaque samplers",
    ], waveform_records)

    print("\nGROUP COLOR SIGNATURES (HUE/SATURATION)")
    color_records = []
    global_colors = defaultdict(Counter)
    global_counts = Counter()
    for row in rows:
        for group, colors in row["lights"]["group_colors"].items():
            global_counts[group] += row["lights"]["groups"][group]
            global_colors[group].update(colors)
    for group in sorted(global_counts):
        colors = ", ".join(
            f"H{h}/S{s}:{count}" for (h, s), count
            in sorted(global_colors[group].items()))
        color_records.append({
            "group": f"{group[0]}/{group[1]}/{group[2]}",
            "lights": global_counts[group], "H/S signatures": colors,
            "uniform H/S": "yes" if len(global_colors[group]) == 1 else "no",
        })
    _print_table(
        ["group", "lights", "H/S signatures", "uniform H/S"],
        color_records)

    print("\nPANNING AUDIT")
    panning_records = []
    for row in rows:
        pan = row["panning"]
        panning_records.append({
            "map": row["map"], "world": pan["world_surfaces"],
            "sky": pan["sky_surfaces"],
            "U/V/UV": f"{pan['u_surfaces']}/{pan['v_surfaces']}/{pan['uv_surfaces']}",
            "slots": pan["pan_slots"],
            "rate conflicts": pan["rate_conflicts"],
            "zero-rate slots": pan["zero_direction_slots"],
            "mover actors/surfs": f"{pan['mover_actors']}/{pan['mover_surfaces']}",
            "mesh flag consumers": pan["mesh_flag_consumers"],
        })
    _print_table([
        "map", "world", "sky", "U/V/UV", "slots", "rate conflicts",
        "zero-rate slots", "mover actors/surfs", "mesh flag consumers",
    ], panning_records)
    conflict_records = []
    for row in rows:
        for detail in row["panning"]["conflict_details"]:
            conflict_records.append({
                "map": row["map"], "material": detail["material"],
                "zone rates and votes": detail["zones"],
                "importer choice": detail["chosen"],
            })
    if conflict_records:
        print("\nPANNING SLOT RATE CONFLICTS")
        _print_table(
            ["map", "material", "zone rates and votes", "importer choice"],
            conflict_records)

    print("\nPULSE REACH INTO MOVER/MESH MID ARRAYS")
    consumer_records = []
    for row in rows:
        consumer = row["consumers"]
        consumer_records.append({
            "map": row["map"], "mover consumers": consumer["mover_consumers"],
            "mover links": consumer["mover_links"],
            "per-surface links": consumer["mover_surface_links"],
            "mesh consumers": consumer["mesh_consumers"],
            "mesh links": consumer["mesh_links"],
            "pulse lights": ",".join(consumer["pulse_lights_reached"]) or "-",
        })
    _print_table([
        "map", "mover consumers", "mover links", "per-surface links",
        "mesh consumers", "mesh links", "pulse lights",
    ], consumer_records)

    endpoint_rows = [] if args.fast else endpoint_check(repo, rows)
    if endpoint_rows:
        print("\nEXISTING ENDPOINT-LERP FREEZE CHECK")
        _print_table([
            "map", "texels", "affected", "freeze_mismatch",
            "affected_freeze_mismatch", "max_atlas_byte_error",
        ], endpoint_rows)

    type_totals = Counter()
    authored_totals = Counter()
    active_totals = Counter()
    total_deltas = 0
    wave_maps = 0
    rebuild_maps = 0
    for row in rows:
        for group, count in row["lights"]["groups"].items():
            type_totals[group[0]] += count
        authored_totals.update(row["lights"]["authored_effects"])
        active_totals.update(row["lights"]["active_effects"])
        total_deltas += row["delta_count"]
        wave_maps += int(bool(row["lights"]["groups"]))
        rebuild_maps += int(row["delta_count"] > 0)
    atlas_mib = rows[0]["atlas_mib"]
    existing_endpoint_mib = wave_maps * 2 * atlas_mib
    delta_mib = total_deltas * atlas_mib
    pan_totals = Counter()
    for row in rows:
        for key in (
                "world_surfaces", "sky_surfaces", "u_surfaces", "v_surfaces",
                "uv_surfaces", "pan_slots", "rate_conflicts",
                "zero_direction_slots", "mover_actors", "mover_surfaces",
                "mesh_flag_consumers"):
            pan_totals[key] += row["panning"][key]
    print("\nSUMMARY")
    print(f"maps={len(rows)} distinct_wave_groups={len(global_counts)} "
          f"pulse={type_totals['LT_Pulse']} "
          f"subtle={type_totals['LT_SubtlePulse']}")
    print(f"shimmer_authored={dict(authored_totals)}")
    print(f"shimmer_active={dict(active_totals)}")
    print(f"rebuild_maps={rebuild_maps} delta_atlases={total_deltas} "
          f"delta_mib={delta_mib:.1f} existing_endpoint_mib="
          f"{existing_endpoint_mib:.1f} net_growth_mib="
          f"{delta_mib - existing_endpoint_mib:.1f}")
    print(f"panning_world={pan_totals['world_surfaces']} "
          f"panning_sky={pan_totals['sky_surfaces']} "
          f"panning_movers={pan_totals['mover_surfaces']} "
          f"panning_rate_conflicts={pan_totals['rate_conflicts']}")
    print("pulse_array_consumers="
          f"{sum(row['consumers']['mover_consumers'] for row in rows)} movers/"
          f"{sum(row['consumers']['mesh_consumers'] for row in rows)} meshes")


if __name__ == "__main__":
    main()
