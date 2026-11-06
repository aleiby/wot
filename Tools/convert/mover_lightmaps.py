"""Reference composer for UE1 static-mover per-FPoly baked lightmaps.

This is the readable oracle for the native cache builder.  It deliberately
reuses the world composer's decoder, light resolver, shadow filter, and
spatial kernels so the only new behavior is the UE1 FPoly mover frame and
per-texel world-zone ambient lookup.
"""
import json
import re
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from lightmaps import (
    LIGHT_EFFECT_MEAN,
    LIGHT_TYPE_MEAN,
    SPOTLIGHT_EFFECTS,
    SUPPORTED_EFFECTS,
    _selected_effect_scale,
    _selected_wave_scale,
    ambient_seed,
    canonical_light_animation,
    clip_rect,
    decode_model_full,
    fget_hsv,
    point_zone,
    resolve_light,
    shadow_map_gen,
    spatial_cylinder,
    spatial_none,
    spatial_nonincidence,
    spatial_spotlight,
    surf_basis,
    world_light_radius,
)
from mover_light_select import rotation_axes
from ue1pkg import Package, decode_brush_model


_STRUCT_FIELD_RE = re.compile(
    r"([A-Za-z]+)\s*=\s*([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)")
_MODEL_RE = re.compile(r"Model'[^']*\.(\w+)'", re.IGNORECASE)


@dataclass(frozen=True)
class MoverPose:
    location: np.ndarray
    rotation: np.ndarray
    pre_pivot: np.ndarray
    main_scale: np.ndarray
    post_scale: np.ndarray


def ue1_rotation_matrix(rotation):
    """Return a column-vector matrix matching UE1/UE FRotator axes.

    ``rotation_axes`` is source-derived from UE1's 16384-entry SinTab and
    returns local X/Y/Z axes as rows.  Transposing makes ``matrix @ local``
    the same operation as UE's ``FTransform(Rotator).TransformPosition``.
    """
    return np.asarray(rotation_axes(rotation), dtype=np.float64).T


def _struct_vector(value, default):
    fields = {name.casefold(): float(number)
              for name, number in _STRUCT_FIELD_RE.findall(value or "")}
    return np.array([
        fields.get("x", default[0]), fields.get("y", default[1]),
        fields.get("z", default[2]),
    ], dtype=np.float64)


def _struct_rotator(value, default=(0.0, 0.0, 0.0)):
    fields = {name.casefold(): float(number)
              for name, number in _STRUCT_FIELD_RE.findall(value or "")}
    return np.array([
        fields.get("pitch", default[0]), fields.get("yaw", default[1]),
        fields.get("roll", default[2]),
    ], dtype=np.float64)


def _scale(value):
    fields = {name.casefold(): float(number)
              for name, number in _STRUCT_FIELD_RE.findall(value or "")}
    sheer = fields.get("sheerrate", 0.0)
    if sheer != 0.0:
        raise ValueError(f"mover brush shear is unsupported: {sheer}")
    return np.array([
        fields.get("x", 1.0), fields.get("y", 1.0), fields.get("z", 1.0),
    ], dtype=np.float64)


def _actor_pose(actor):
    props = actor["props"]
    location = (_struct_vector(props["BasePos"], (0.0, 0.0, 0.0))
                if "BasePos" in props
                else np.asarray(actor["location"], dtype=np.float64))
    return MoverPose(
        location=location,
        rotation=_struct_rotator(props.get("BaseRot", "")),
        pre_pivot=_struct_vector(props.get("PrePivot", ""),
                                 (0.0, 0.0, 0.0)),
        main_scale=_scale(props.get("MainScale", "")),
        post_scale=_scale(props.get("PostScale", "")),
    )


def transform_poly(poly, pose):
    """Apply ABrush::BuildCoords + FPoly::Transform to one decoded FPoly."""
    if (not np.isfinite(np.concatenate([
            pose.location, pose.rotation, pose.pre_pivot,
            pose.main_scale, pose.post_scale])).all()
            or np.any(pose.main_scale == 0.0)
            or np.any(pose.post_scale == 0.0)):
        raise ValueError("mover brush scale/pose is zero or non-finite")
    rotation = ue1_rotation_matrix(pose.rotation)
    point_xform = (np.diag(pose.post_scale) @ rotation
                   @ np.diag(pose.main_scale))
    vector_xform = np.linalg.inv(point_xform).T

    def point(value):
        return point_xform @ (np.asarray(value, dtype=np.float64)
                              - pose.pre_pivot) + pose.location

    def vector(value):
        return vector_xform @ np.asarray(value, dtype=np.float64)

    normal = vector(poly["normal"])
    normal_length = np.linalg.norm(normal)
    if normal_length == 0.0:
        raise ValueError("mover polygon has a zero transformed normal")
    out = dict(poly)
    out["base"] = point(poly["base"])
    out["texture_u"] = vector(poly["texture_u"])
    out["texture_v"] = vector(poly["texture_v"])
    out["normal"] = normal / normal_length
    out["vertices"] = [point(vertex) for vertex in poly.get("vertices", ())]
    if np.prod(pose.main_scale) * np.prod(pose.post_scale) < 0.0:
        out["vertices"].reverse()
    return out


def _surface_basis(poly, lightmap):
    # surf_basis is shared verbatim with the world composer; this tiny adapter
    # presents transformed FPoly vectors through its FBSPSurf-shaped input.
    bsp = {
        "points": [poly["base"]],
        "vectors": [poly["normal"], poly["texture_u"], poly["texture_v"]],
    }
    surface = {"pbase": 0, "vnorm": 0, "vtu": 1, "vtv": 2}
    return surf_basis(bsp, surface, lightmap)


def _find_model_export(package, model_name):
    for export in package.exports:
        if (export["name"].casefold() == model_name.casefold()
                and package.objname(export["cls"]) == "Model"):
            return export
    raise ValueError(f"mover brush model {model_name!r} is absent")


def _level_model_export(package):
    models = [export for export in package.exports
              if package.objname(export["cls"]) == "Model"]
    if not models:
        raise ValueError("map has no UModel export")
    return max(models, key=lambda export: export.get("size", 0))


def _light_record(package, actors, ref, level_brightness, cache):
    if ref in cache:
        return cache[ref]
    light_name = package.objname(ref)
    actor = actors.get(light_name)
    record = None
    if actor is not None:
        record = canonical_light_animation(resolve_light(actor))
        radius = world_light_radius(record["cls"], record["LightRadius"])
        record["radius"] = radius
        record["rradius"] = 1.0 / max(1.0, radius)
        record["rradius_mult"] = 4093.0 * record["rradius"] ** 2
        float_color = (fget_hsv(record["LightHue"],
                                record["LightSaturation"], 255)
                       * np.float32(record["LightBrightness"] / 255.0)
                       * np.float32(level_brightness)
                       * np.float32(_selected_wave_scale(
                           record, "mean", None))
                       * np.float32(_selected_effect_scale(
                           record, "mean", None)))
        record["fixd"] = np.floor(
            float_color.astype(np.float64) * 65536.0).astype(np.int64)
        # Spotlight is not currently present on the pinned Mover6 witness but
        # shares the world oracle's exact UE1 quantized forward vector.
        axes = rotation_axes(record["rotation"])
        record["view"] = np.asarray(axes[0], dtype=np.float64)
    cache[ref] = record
    return record


def _compose_poly(package, mover_bsp, level_bsp, actors, level_info,
                  transformed_poly, lightmap, lightning_off,
                  level_brightness, light_cache):
    uclamp, vclamp = lightmap["uclamp"], lightmap["vclamp"]
    mask_u = (uclamp + 7) >> 3
    mask_space = mask_u * vclamp
    ilightactors = lightmap["ilightactors"]
    light_refs = mover_bsp["lights"]
    has_light_run = (0 <= ilightactors < len(light_refs)
                     and light_refs[ilightactors] != 0)
    try:
        vbase, du, dv, origin, tu, tv, normal = _surface_basis(
            transformed_poly, lightmap)
    except np.linalg.LinAlgError:
        if has_light_run:
            raise
        # Battle3_01 DoorMover2 poly 2 is the retail witness: TextureV is
        # parallel to Normal, but the saved light run is empty. No spatial
        # light kernel needs an inverse basis, so seed its base zone uniformly.
        origin = np.asarray(transformed_poly["base"], dtype=np.float64)
        normal = np.asarray(transformed_poly["normal"], dtype=np.float64)
        vbase = du = dv = None

    zone_seeds = []
    for zone_ref in (level_bsp.get("zones") or [0]):
        zone_name = package.objname(zone_ref) if zone_ref else None
        zone_actor = actors.get(zone_name) if zone_name else None
        zone_seeds.append(ambient_seed(zone_actor or level_info))
    total = np.empty((vclamp, uclamp, 3), dtype=np.int32)
    if vbase is None:
        zone = point_zone(level_bsp, origin + normal * 2.0)
        if not 0 < zone < len(zone_seeds):
            zone = 0
        total[:] = zone_seeds[zone]
    else:
        for v in range(vclamp):
            row = vbase + dv * v + normal * 2.0
            for u in range(uclamp):
                zone = point_zone(level_bsp, row + du * u)
                if not 0 < zone < len(zone_seeds):
                    zone = 0
                total[v, u] = zone_seeds[zone]

    if ilightactors < 0:
        return total.astype(np.uint8)
    light_index = 0
    while (ilightactors + light_index < len(light_refs)
           and light_refs[ilightactors + light_index] != 0):
        ref = light_refs[ilightactors + light_index]
        record = _light_record(package, actors, ref, level_brightness,
                               light_cache)
        if (record is None or record["name"] in lightning_off
                or record["LightType"] not in LIGHT_TYPE_MEAN
                or int(record["LightBrightness"]) == 0
                or record["LightEffect"] not in SUPPORTED_EFFECTS):
            light_index += 1
            continue
        offset = lightmap["data_offset"] + light_index * mask_space
        if offset < 0 or offset + mask_space > len(mover_bsp["lightbits"]):
            raise ValueError("mover lightmap shadow mask is out of range")
        shadow = shadow_map_gen(
            mover_bsp["lightbits"][offset:offset + mask_space],
            mask_u, vclamp)[:, :uclamp]
        location = record["location"]
        rect = clip_rect(location, origin, tu, tv, normal,
                         record["radius"], lightmap)
        effect = record["LightEffect"]
        if effect == "LE_NonIncidence":
            illumination = spatial_nonincidence(
                shadow, vbase, du, dv, location, record["rradius"],
                record["rradius_mult"], rect)
        elif effect == "LE_Cylinder":
            illumination = spatial_cylinder(
                shadow, vbase, du, dv, location, record["rradius"], rect)
        elif effect in SPOTLIGHT_EFFECTS:
            diffuse = abs((location - origin) @ normal) * record["rradius"]
            illumination = spatial_spotlight(
                shadow, vbase, du, dv, location, record["view"],
                int(record["LightCone"]), diffuse,
                record["rradius_mult"], rect)
        else:
            diffuse = abs((location - origin) @ normal) * record["rradius"]
            illumination = spatial_none(
                shadow, vbase, du, dv, location, diffuse,
                record["rradius_mult"], rect)
        contribution = np.minimum(
            (illumination[..., None].astype(np.int64) * record["fixd"])
            >> 16, 127).astype(np.int32)
        total = np.minimum(total + contribution, 127)
        light_index += 1
    return total.astype(np.uint8)


def compose_mover(map_path, t3d_path, mover_name):
    """Compose one static mover's FPoly lightmaps in polygon-list order.

    Entries without a valid ``iBrushPoly`` are ``None``.  Lit entries are
    uint8 RGB7 arrays shaped ``(VClamp, UClamp, 3)``.
    """
    package = Package(map_path)
    actors = {actor["name"]: actor
              for actor in json.loads(Path(t3d_path).read_text())["actors"]}
    mover = actors.get(mover_name)
    if mover is None:
        raise ValueError(f"mover actor {mover_name!r} is absent")
    if mover["props"].get("bDynamicLightMover", "").casefold() == "true":
        raise ValueError(f"{mover_name} is a dynamic-light mover")
    match = _MODEL_RE.search(mover["props"].get("Brush", ""))
    if not match:
        raise ValueError(f"{mover_name} has no brush model")
    model_export = _find_model_export(package, match.group(1))
    level_bsp = decode_model_full(package, _level_model_export(package))
    mover_bsp = decode_model_full(package, model_export)
    polygons = decode_brush_model(package, model_export)
    transformed = [transform_poly(poly, _actor_pose(mover))
                   for poly in polygons]
    level_info = next((actor for actor in actors.values()
                       if actor["cls"] == "LevelInfo"), None)
    if level_info is None:
        raise ValueError("map actor export has no LevelInfo")
    level_brightness = float(level_info["props"].get("Brightness", 1.0))
    lightning_tags = {
        actor["props"].get("LightTag", "").casefold()
        for actor in actors.values() if actor["cls"].casefold() == "lightning"
    } - {""}
    lightning_off = {
        actor["name"] for actor in actors.values()
        if actor["props"].get("Tag", "").casefold() in lightning_tags
    }
    light_cache = {}
    result = []
    for poly in transformed:
        lightmap_index = int(poly.get("brush_poly", -1))
        if not 0 <= lightmap_index < len(mover_bsp["lightmap"]):
            result.append(None)
            continue
        result.append(_compose_poly(
            package, mover_bsp, level_bsp, actors, level_info, poly,
            mover_bsp["lightmap"][lightmap_index], lightning_off,
            level_brightness, light_cache))
    return result
