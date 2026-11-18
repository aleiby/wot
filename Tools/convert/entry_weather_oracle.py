"""Entry-lightning illumination oracle and supplied-sample atlas composer.

The original-map expectation path derives illumination from the retail map and
actor export independently of the native product.  Native-input comparison
paths may decode caller-supplied WOTPBSP v3 metadata and compose supplied
steady/runtime atlas samples; those inputs do not define the original-map
illumination expectations.
"""

import argparse
import hashlib
import json
import math
import re
import struct
from pathlib import Path

import numpy as np

import lightmap_atlas
import lightmaps


def filtered_shadow_bytes(bits, mask_u, vclamp):
    """Expose the retail one-bit shadow filter as deterministic row bytes."""
    source = np.frombuffer(bits, dtype=np.uint8)
    expected = int(mask_u) * int(vclamp)
    if source.size != expected or mask_u <= 0 or vclamp <= 0:
        raise ValueError("shadow bytes do not match mask dimensions")
    return lightmaps.shadow_map_gen(source, int(mask_u), int(vclamp)).tobytes()


def merge_palette(steady, illumination, fixed_color):
    """Apply UE1's fixed-point palette floor and saturating RGBA7 merge."""
    base = np.asarray(steady, dtype=np.uint8)
    illum = np.asarray(illumination, dtype=np.uint8)
    fixed = np.asarray(fixed_color, dtype=np.int64)
    if base.ndim != 3 or base.shape[-1] != 3:
        raise ValueError("steady lightmap must be HxWx3")
    if illum.shape != base.shape[:2] or fixed.shape != (3,):
        raise ValueError("illumination or fixed color shape mismatch")
    contribution = np.minimum(
        (illum[..., None].astype(np.int64) * fixed) >> 16, 127)
    return np.minimum(base.astype(np.int64) + contribution, 127).astype(np.uint8)


def _decode_native_payload(payload):
    """Decode the v3 fields needed by the independent dynamic-light oracle."""
    payload = memoryview(payload)

    def require(offset, size, label):
        if offset < 0 or size < 0 or offset > len(payload) \
                or size > len(payload) - offset:
            raise ValueError(f"native payload truncates {label}")

    def u16(offset, label):
        require(offset, 2, label)
        return struct.unpack_from("<H", payload, offset)[0]

    def u32(offset, label):
        require(offset, 4, label)
        return struct.unpack_from("<I", payload, offset)[0]

    require(0, 52, "header")
    if bytes(payload[:8]) != b"WOTPBSP\0" or u16(8, "version") != 3:
        raise ValueError("native oracle comparison requires WOTPBSP version 3")
    vertex_count = u32(12, "vertex count")
    index_count = u32(16, "index count")
    section_count = u32(20, "section count")
    offset = 52
    for section_index in range(section_count):
        require(offset, 24, f"section {section_index}")
        identity_bytes = u16(offset, f"section {section_index} identity")
        if u16(offset + 2, f"section {section_index} reserved") != 0:
            raise ValueError("native section reserved field is nonzero")
        offset += 24
        require(offset, identity_bytes, f"section {section_index} identity")
        offset += identity_bytes
    geometry_bytes = vertex_count * 40 + index_count * 4
    require(offset, geometry_bytes, "geometry")
    offset += geometry_bytes
    require(offset, 12, "atlas header")
    atlas_width, atlas_height, atlas_bytes = struct.unpack_from(
        "<III", payload, offset)
    if atlas_bytes != atlas_width * atlas_height * 4:
        raise ValueError("native atlas byte count is inconsistent")
    offset += 12
    require(offset, atlas_bytes, "atlas pixels")
    atlas_bgra = bytes(payload[offset:offset + atlas_bytes])
    offset += atlas_bytes
    trailer_start = offset
    require(offset, 24, "weather trailer header")
    (trailer_bytes, triangle_count, surface_count,
     light_count, pair_count, root_outside) = struct.unpack_from(
        "<6I", payload, offset)
    if trailer_bytes != len(payload) - trailer_start:
        raise ValueError("native trailer length is inconsistent")
    if triangle_count != index_count // 3 or root_outside > 1:
        raise ValueError("native trailer header is inconsistent")
    offset += 24
    require(offset, triangle_count * 4, "triangle provenance")
    offset += triangle_count * 4
    surfaces = {}
    for surface_index in range(surface_count):
        require(offset, 28, f"surface {surface_index}")
        source_index, x, y, width, height, pan_u, pan_v = struct.unpack_from(
            "<5I2f", payload, offset)
        if source_index != surface_index:
            raise ValueError("native surface rows are not dense and ordered")
        surfaces[source_index] = dict(
            x=x, y=y, width=width, height=height, pan_u=pan_u, pan_v=pan_v)
        offset += 28
    native_lights = []
    for light_index in range(light_count):
        require(offset, 4, f"light {light_index} header")
        identity_bytes, reserved = struct.unpack_from("<HH", payload, offset)
        if identity_bytes == 0 or reserved != 0:
            raise ValueError("native light header is invalid")
        offset += 4
        require(offset, identity_bytes + 8, f"light {light_index}")
        try:
            identity = bytes(payload[offset:offset + identity_bytes]).decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValueError("native light identity is invalid UTF-8") from error
        offset += identity_bytes
        brightness, hue, saturation, color_reserved, level_brightness = \
            struct.unpack_from("<BBBBf", payload, offset)
        if color_reserved != 0:
            raise ValueError("native light color reserved field is nonzero")
        native_lights.append(dict(
            identity=identity,
            brightness=brightness,
            hue=hue,
            saturation=saturation,
            level_brightness=level_brightness,
        ))
        offset += 8
    native_pairs = []
    pair_keys = set()
    aggregate = hashlib.sha256()
    for pair_index in range(pair_count):
        require(offset, 12, f"pair {pair_index} header")
        light_index, surface_index, byte_count = struct.unpack_from(
            "<III", payload, offset)
        offset += 12
        require(offset, byte_count, f"pair {pair_index} bytes")
        key = (light_index, surface_index)
        if key in pair_keys:
            raise ValueError("native payload contains a duplicate pair")
        if light_index >= light_count or surface_index not in surfaces:
            raise ValueError("native pair references an absent record")
        surface = surfaces[surface_index]
        width, height = surface["width"], surface["height"]
        if width == 0 or height == 0 or byte_count != width * height:
            raise ValueError("native pair size disagrees with its surface")
        data = bytes(payload[offset:offset + byte_count])
        pair_keys.add(key)
        native_pairs.append(dict(
            light_index=light_index,
            surface_index=surface_index,
            illumination=data,
        ))
        aggregate.update(struct.pack("<III", light_index, surface_index, byte_count))
        aggregate.update(data)
        offset += byte_count
    if offset != len(payload):
        raise ValueError("native payload has trailing bytes")
    return dict(
        atlas_width=atlas_width,
        atlas_height=atlas_height,
        atlas_bgra=atlas_bgra,
        surfaces=surfaces,
        lights=native_lights,
        pairs=native_pairs,
        pair_stream_sha256=aggregate.hexdigest(),
    )


def _native_fget_hsv(hue, saturation):
    """UE1 FGetHSV with a float32 store at every original FLOAT boundary."""
    value = np.float32(255.0 * 1.4 / 255.0)
    value = np.float32(
        float(value) * (0.70 / (0.01 + math.sqrt(float(value)))))
    value = np.float32(min(max(float(value), 0.0), 1.0))
    if hue < 86:
        hue_rgb = np.array([
            np.float32((85 - hue) / np.float32(85.0)),
            np.float32(hue / np.float32(85.0)),
            np.float32(0.0),
        ], dtype=np.float32)
    elif hue < 171:
        hue_rgb = np.array([
            np.float32(0.0),
            np.float32((170 - hue) / np.float32(85.0)),
            np.float32((hue - 85) / np.float32(85.0)),
        ], dtype=np.float32)
    else:
        hue_rgb = np.array([
            np.float32((hue - 170) / np.float32(85.0)),
            np.float32(0.0),
            np.float32((255 - hue) / np.float32(84.0)),
        ], dtype=np.float32)
    saturation_float = np.float32(float(saturation) / 255.0)
    remainder = (np.float32(1.0) - hue_rgb).astype(np.float32)
    whitened = (remainder * saturation_float).astype(np.float32)
    mixed = (hue_rgb + whitened).astype(np.float32)
    return (mixed * value).astype(np.float32)


def dynamic_fixed_color(brightness, hue, saturation, level_brightness, sample):
    """Return original-runtime RGB palette steps for one supplied sample."""
    level = np.float32(level_brightness)
    sample_float = np.float32(sample)
    modulated = np.float32(
        np.float32(brightness) / np.float32(255.0))
    modulated = np.float32(modulated * sample_float)
    modulated = np.float32(min(max(float(modulated), 0.0), 1.0))
    scale = np.float32(modulated * level)
    scaled = (_native_fget_hsv(hue, saturation) * scale).astype(np.float32)
    return np.floor(scaled.astype(np.float64) * 65536.0).astype(np.int64)


def compose_native_samples(payload, samples):
    """Compose one complete BGRA atlas for supplied per-light [0,1] samples."""
    decoded = _decode_native_payload(payload)
    sample_values = tuple(samples)
    if len(sample_values) != len(decoded["lights"]):
        raise ValueError("native sample count does not match light count")
    samples32 = np.asarray(sample_values, dtype=np.float32)
    if not np.all(np.isfinite(samples32)):
        raise ValueError("native samples must be finite")
    if np.any(samples32 < 0.0) or np.any(samples32 > 1.0):
        raise ValueError("native samples must be in [0,1]")
    for light in decoded["lights"]:
        level = np.float32(light["level_brightness"])
        if not np.isfinite(level) or level < 0.0 or level > 1024.0:
            raise ValueError("native level brightness is invalid")

    width = decoded["atlas_width"]
    height = decoded["atlas_height"]
    atlas = np.frombuffer(decoded["atlas_bgra"], dtype=np.uint8) \
        .reshape(height, width, 4).copy()
    active_surfaces = {
        pair["surface_index"] for pair in decoded["pairs"]
        if samples32[pair["light_index"]] > 0.0
    }
    paired_surfaces = {
        pair["surface_index"] for pair in decoded["pairs"]
    }
    for surface_index in paired_surfaces:
        surface = decoded["surfaces"][surface_index]
        x, y = surface["x"], surface["y"]
        rect_width, rect_height = surface["width"], surface["height"]
        if (x == 0 or y == 0 or x + rect_width >= width
                or y + rect_height >= height):
            raise ValueError("native paired surface has no in-bounds gutter")
        padded = atlas[
            y - 1:y + rect_height + 1,
            x - 1:x + rect_width + 1,
            :3,
        ]
        if np.any(padded > 254) or np.any((padded & 1) != 0):
            raise ValueError("native paired surface has a non-RGBA7 base channel")
    if not active_surfaces:
        return decoded["atlas_bgra"]

    totals = {}
    for surface_index in active_surfaces:
        surface = decoded["surfaces"][surface_index]
        x, y = surface["x"], surface["y"]
        rect_width, rect_height = surface["width"], surface["height"]
        totals[surface_index] = (
            atlas[y:y + rect_height, x:x + rect_width, :3] // 2
        ).astype(np.int64)

    for light_index, light in enumerate(decoded["lights"]):
        sample = samples32[light_index]
        if sample <= 0.0:
            continue
        fixed_rgb = dynamic_fixed_color(
            light["brightness"], light["hue"], light["saturation"],
            light["level_brightness"], sample)
        fixed_bgr = fixed_rgb[::-1]
        for pair in decoded["pairs"]:
            if pair["light_index"] != light_index:
                continue
            surface_index = pair["surface_index"]
            surface = decoded["surfaces"][surface_index]
            illumination = np.frombuffer(
                pair["illumination"], dtype=np.uint8).reshape(
                    surface["height"], surface["width"], 1)
            contribution = (
                illumination.astype(np.int64) * fixed_bgr.reshape(1, 1, 3)
            ) >> 16
            totals[surface_index] = np.minimum(
                totals[surface_index] + contribution, 127)

    for surface_index, total in totals.items():
        surface = decoded["surfaces"][surface_index]
        x, y = surface["x"], surface["y"]
        rect_width, rect_height = surface["width"], surface["height"]
        interior = atlas[y:y + rect_height, x:x + rect_width]
        interior[..., :3] = (total * 2).astype(np.uint8)
        interior[..., 3] = 255
        atlas[y:y + rect_height, x - 1] = atlas[y:y + rect_height, x]
        atlas[y:y + rect_height, x + rect_width] = \
            atlas[y:y + rect_height, x + rect_width - 1]
        atlas[y - 1, x - 1:x + rect_width + 1] = \
            atlas[y, x - 1:x + rect_width + 1]
        atlas[y + rect_height, x - 1:x + rect_width + 1] = \
            atlas[y + rect_height - 1, x - 1:x + rect_width + 1]
    return atlas.tobytes()


def compare_native_samples(payload, samples, actual_bgra):
    """Compare all atlas pixels against the supplied-sample oracle result."""
    decoded = _decode_native_payload(payload)
    expected = compose_native_samples(payload, samples)
    actual = bytes(actual_bgra)
    if len(actual) != len(expected):
        raise ValueError("supplied composed atlas byte count is inconsistent")
    expected_pixels = np.frombuffer(expected, dtype=np.uint8).reshape(-1, 4)
    actual_pixels = np.frombuffer(actual, dtype=np.uint8).reshape(-1, 4)
    differing = np.flatnonzero(np.any(expected_pixels != actual_pixels, axis=1))
    coordinates = [
        [int(index % decoded["atlas_width"]),
         int(index // decoded["atlas_width"])]
        for index in differing[:64]
    ]
    return {
        "samples": [float(np.float32(value)) for value in samples],
        "all_atlas_bytes_equal": differing.size == 0,
        "differing_pixel_count": int(differing.size),
        "first_differing_pixels": coordinates,
        "expected_atlas_sha256": hashlib.sha256(expected).hexdigest(),
        "actual_atlas_sha256": hashlib.sha256(actual).hexdigest(),
    }


def parse_sample_case(value):
    """Parse LABEL=S0,S1,... for CLI sample generation/comparison."""
    label, separator, csv = str(value).partition("=")
    if not separator or not re.fullmatch(r"[A-Za-z0-9_.-]+", label) or not csv:
        raise ValueError("sample case must use LABEL=S0,S1,... syntax")
    try:
        samples = [float(item) for item in csv.split(",")]
    except ValueError as error:
        raise ValueError("sample case contains a non-numeric value") from error
    if not samples or not all(math.isfinite(item) for item in samples):
        raise ValueError("sample case values must be finite")
    if any(item < 0.0 or item > 1.0 for item in samples):
        raise ValueError("sample case values must be in [0,1]")
    return label, samples


def compare_native_payload(payload, manifest, oracle_root):
    """Compare a native v3 payload with an already-built independent oracle."""
    decoded = _decode_native_payload(payload)
    oracle_root = Path(oracle_root)

    expected_lights = [row["identity"] for row in manifest["lights"]]
    expected_pairs = {}
    for row in manifest["pairs"]:
        key = (int(row["light_index"]), int(row["surface_index"]))
        if key in expected_pairs:
            raise ValueError("oracle manifest contains a duplicate pair")
        data = (oracle_root / row["file"]).read_bytes()
        if len(data) != int(row["byte_count"]):
            raise ValueError("oracle pair file length disagrees with its manifest")
        expected_pairs[key] = data
    native_pairs = {
        (row["light_index"], row["surface_index"]): row["illumination"]
        for row in decoded["pairs"]
    }
    missing = sorted(set(expected_pairs) - set(native_pairs))
    unexpected = sorted(set(native_pairs) - set(expected_pairs))
    differing = sorted(
        key for key in set(expected_pairs) & set(native_pairs)
        if expected_pairs[key] != native_pairs[key])
    native_lights = [row["identity"] for row in decoded["lights"]]
    lights_equal = native_lights == expected_lights
    return {
        "native_light_count": len(native_lights),
        "oracle_light_count": len(expected_lights),
        "light_identities_equal": lights_equal,
        "native_pair_count": len(native_pairs),
        "oracle_pair_count": len(expected_pairs),
        "native_pair_stream_sha256": decoded["pair_stream_sha256"],
        "missing_pairs": [list(key) for key in missing],
        "unexpected_pairs": [list(key) for key in unexpected],
        "differing_pairs": [list(key) for key in differing],
        "all_pair_bytes_equal": (
            lights_equal and not missing and not unexpected and not differing),
    }


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def _atlas_bgra_bytes(rgb):
    rgb = np.asarray(rgb, dtype=np.uint8)
    bgra = np.empty((*rgb.shape[:2], 4), dtype=np.uint8)
    bgra[..., 0] = rgb[..., 2]
    bgra[..., 1] = rgb[..., 1]
    bgra[..., 2] = rgb[..., 0]
    bgra[..., 3] = 255
    return bgra.tobytes()


def _resolved_light(actor, level_brightness):
    record = lightmaps.canonical_light_animation(lightmaps.resolve_light(actor))
    radius = lightmaps.world_light_radius(record["cls"], record["LightRadius"])
    record["radius"] = radius
    record["rradius"] = 1.0 / max(1.0, radius)
    record["rradius_mult"] = 4093.0 * record["rradius"] ** 2
    record["view"] = lightmaps.rot_vector(record["rotation"])
    color = lightmaps.fget_hsv(
        record["LightHue"], record["LightSaturation"], 255)
    color *= np.float32(record["LightBrightness"] / 255.0)
    color *= np.float32(level_brightness)
    record["fixed_color"] = np.floor(
        color.astype(np.float64) * 65536.0).astype(np.int64)
    return record


def _illumination(light, bsp, surface, lightmap, light_index):
    mask_u = (lightmap["uclamp"] + 7) >> 3
    mask_space = mask_u * lightmap["vclamp"]
    offset = lightmap["data_offset"] + light_index * mask_space
    end = offset + mask_space
    if offset < 0 or end > len(bsp["lightbits"]):
        raise ValueError("light shadow mask is outside the model lightbits")
    shadow = lightmaps.shadow_map_gen(
        bsp["lightbits"][offset:end], mask_u, lightmap["vclamp"])
    shadow = shadow[:, :lightmap["uclamp"]]
    vbase, du, dv, origin, tu, tv, normal = lightmaps.surf_basis(
        bsp, surface, lightmap)
    location = light["location"]
    rect = lightmaps.clip_rect(
        location, origin, tu, tv, normal, light["radius"], lightmap)
    effect = light["LightEffect"]
    if effect == "LE_NonIncidence":
        return lightmaps.spatial_nonincidence(
            shadow, vbase, du, dv, location, light["rradius"],
            light["rradius_mult"], rect)
    if effect == "LE_Cylinder":
        return lightmaps.spatial_cylinder(
            shadow, vbase, du, dv, location, light["rradius"], rect)
    diffuse = abs((location - origin) @ normal) * light["rradius"]
    if effect in lightmaps.SPOTLIGHT_EFFECTS:
        return lightmaps.spatial_spotlight(
            shadow, vbase, du, dv, location, light["view"],
            int(light["LightCone"]), diffuse, light["rradius_mult"], rect)
    return lightmaps.spatial_none(
        shadow, vbase, du, dv, location, diffuse,
        light["rradius_mult"], rect)


def _safe_component(value):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


def build_oracle(map_path, actors_path, output_path):
    map_path = Path(map_path)
    actors_path = Path(actors_path)
    output_path = Path(output_path)
    output_path.mkdir(parents=True, exist_ok=True)
    illumination_path = output_path / "illumination"
    illumination_path.mkdir(parents=True, exist_ok=True)

    package, bsp = lightmaps.load_level(map_path)
    actor_rows = json.loads(actors_path.read_text(encoding="utf-8"))["actors"]
    actors = {row["name"]: row for row in actor_rows}
    level = next(row for row in actors.values() if row["cls"] == "LevelInfo")
    level_brightness = float(level["props"].get("Brightness", 1.0))
    lightning_tags = {
        str(row["props"].get("LightTag", "")).casefold()
        for row in actors.values() if lightmaps.is_a(row["cls"], "Lightning")
    } - {""}
    selected = [
        row for row in actors.values()
        if str(row["props"].get("Tag", "")).casefold() in lightning_tags
    ]
    map_identity = map_path.stem
    selected.sort(key=lambda row: f"{map_identity}.{row['name']}".encode("utf-8"))
    if len(selected) != 5:
        raise ValueError(f"expected five entry lightning lights, found {len(selected)}")

    lights = []
    light_by_name = {}
    for index, actor in enumerate(selected):
        resolved = _resolved_light(actor, level_brightness)
        identity = f"{map_identity}.{actor['name']}"
        resolved["identity"] = identity
        resolved["ordered_index"] = index
        lights.append(resolved)
        light_by_name[actor["name"]] = resolved

    steady, steady_stats = lightmaps.compose_map(
        map_path, actors_path, ctx=(package, bsp))
    surface_rows = {}
    composed_texels = steady["texel_rgb"].astype(np.uint8).copy()
    for row, surface_id in enumerate(steady["surf_index"]):
        width = int(steady["surf_uclamp"][row])
        height = int(steady["surf_vclamp"][row])
        offset = int(steady["surf_texel_ofs"][row])
        surface_rows[int(surface_id)] = (row, width, height, offset)

    pairs = []
    for surface_id, surface in enumerate(bsp["surfs"]):
        lightmap_index = surface["ilightmap"]
        if lightmap_index < 0:
            continue
        lightmap = bsp["lightmap"][lightmap_index]
        actor_offset = lightmap["ilightactors"]
        if actor_offset < 0:
            continue
        local_index = 0
        while (actor_offset + local_index < len(bsp["lights"])
               and bsp["lights"][actor_offset + local_index] != 0):
            reference = bsp["lights"][actor_offset + local_index]
            actor_name = package.objname(reference)
            light = light_by_name.get(actor_name)
            if light is not None:
                illumination = _illumination(
                    light, bsp, surface, lightmap, local_index)
                if surface_id not in surface_rows:
                    raise ValueError(
                        f"lightning surface {surface_id} has no steady texel row")
                _row, width, height, offset = surface_rows[surface_id]
                if illumination.shape != (height, width):
                    raise ValueError("illumination dimensions disagree with surface")
                pairs.append(dict(
                    light_index=light["ordered_index"],
                    surface_index=surface_id,
                    illumination=illumination.copy(),
                    width=width,
                    height=height,
                    offset=offset,
                ))
            local_index += 1
    pairs.sort(key=lambda row: (row["light_index"], row["surface_index"]))
    keys = [(row["light_index"], row["surface_index"]) for row in pairs]
    if len(keys) != len(set(keys)):
        raise ValueError("duplicate lightning light/surface pair in retail source")

    manifest_pairs = []
    aggregate = hashlib.sha256()
    for pair in pairs:
        light = lights[pair["light_index"]]
        illumination = pair["illumination"]
        data = illumination.tobytes()
        filename = (
            f"surface-{pair['surface_index']:05d}__"
            f"{_safe_component(light['identity'])}.bin")
        relative = Path("illumination") / filename
        (output_path / relative).write_bytes(data)
        aggregate.update(pair["light_index"].to_bytes(4, "little"))
        aggregate.update(pair["surface_index"].to_bytes(4, "little"))
        aggregate.update(len(data).to_bytes(4, "little"))
        aggregate.update(data)
        begin = pair["offset"]
        end = begin + pair["width"] * pair["height"]
        surface_base = composed_texels[begin:end].reshape(
            pair["height"], pair["width"], 3)
        composed_texels[begin:end] = merge_palette(
            surface_base, illumination, light["fixed_color"]).reshape(-1, 3)
        manifest_pairs.append(dict(
            light_index=pair["light_index"],
            surface_index=pair["surface_index"],
            byte_count=len(data),
            nonzero_bytes=int(np.count_nonzero(illumination)),
            sha256=_sha256(data),
            file=relative.as_posix(),
        ))

    steady_atlas, atlas_document, atlas_stats = lightmap_atlas.build_atlas(steady)
    composed = dict(steady)
    composed["texel_rgb"] = composed_texels
    composed_atlas, composed_document, _ = lightmap_atlas.build_atlas(composed)
    if composed_document != atlas_document:
        raise ValueError("steady and composed atlas packing differs")
    steady_bytes = _atlas_bgra_bytes(steady_atlas)
    composed_bytes = _atlas_bgra_bytes(composed_atlas)
    (output_path / "steady-atlas.bgra").write_bytes(steady_bytes)
    (output_path / "composed-full-atlas.bgra").write_bytes(composed_bytes)

    manifest_lights = [dict(
        identity=row["identity"],
        brightness=int(row["LightBrightness"]),
        hue=int(row["LightHue"]),
        saturation=int(row["LightSaturation"]),
        level_brightness=level_brightness,
    ) for row in lights]
    manifest = dict(
        map=map_identity,
        root_outside=bool(bsp["root_outside"]),
        atlas=atlas_document,
        lights=manifest_lights,
        pairs=manifest_pairs,
    )
    (output_path / "manifest.json").write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        encoding="utf-8")

    changed = np.frombuffer(steady_bytes, dtype=np.uint8) \
        != np.frombuffer(composed_bytes, dtype=np.uint8)
    report = dict(
        map=map_identity,
        light_count=len(lights),
        pair_count=len(pairs),
        pair_bytes=sum(row["byte_count"] for row in manifest_pairs),
        pair_stream_sha256=aggregate.hexdigest(),
        steady_atlas_sha256=_sha256(steady_bytes),
        composed_atlas_sha256=_sha256(composed_bytes),
        changed_channels=int(changed.sum()),
        atlas_size=atlas_document["size"],
        atlas_padded_efficiency=atlas_stats["padded_eff"],
        steady_surface_count=int(steady_stats["surfaces_with_lightmaps"]),
    )
    (output_path / "report.json").write_text(
        json.dumps(report, sort_keys=True, separators=(",", ":")),
        encoding="utf-8")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map", required=True)
    parser.add_argument("--actors", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--native-payload",
        help="optional WOTPBSP v3 evidence to compare byte-for-byte")
    parser.add_argument(
        "--sample-case", action="append", default=[], metavar="LABEL=CSV",
        help="compose a labeled native atlas for comma-separated light samples")
    parser.add_argument(
        "--sample-atlas", action="append", default=[], metavar="LABEL=PATH",
        help="compare a labeled sample case with a supplied BGRA atlas")
    args = parser.parse_args(argv)
    if (args.sample_case or args.sample_atlas) and not args.native_payload:
        parser.error("--sample-case/--sample-atlas require --native-payload")
    try:
        sample_cases = [parse_sample_case(value) for value in args.sample_case]
    except ValueError as error:
        parser.error(str(error))
    if len({label for label, _samples in sample_cases}) != len(sample_cases):
        parser.error("sample case labels must be unique")
    sample_atlases = {}
    for value in args.sample_atlas:
        label, separator, path = value.partition("=")
        if (not separator or not re.fullmatch(r"[A-Za-z0-9_.-]+", label)
                or not path or label in sample_atlases):
            parser.error("sample atlases must use unique LABEL=PATH syntax")
        sample_atlases[label] = Path(path)
    case_labels = {label for label, _samples in sample_cases}
    if set(sample_atlases) - case_labels:
        parser.error("every --sample-atlas label needs a --sample-case")

    report = build_oracle(args.map, args.actors, args.output)
    failure = None
    if args.native_payload:
        output_path = Path(args.output)
        native_payload = Path(args.native_payload).read_bytes()
        manifest = json.loads(
            (output_path / "manifest.json").read_text(encoding="utf-8"))
        comparison = compare_native_payload(
            native_payload, manifest, output_path)
        report["native_comparison"] = comparison
        if not comparison["all_pair_bytes_equal"]:
            failure = "native illumination differs from the retail oracle"
        if sample_cases:
            decoded = _decode_native_payload(native_payload)
            base_pixels = np.frombuffer(
                decoded["atlas_bgra"], dtype=np.uint8).reshape(-1, 4)
            sample_reports = []
            for label, samples in sample_cases:
                expected = compose_native_samples(native_payload, samples)
                expected_path = output_path / f"composed-{label}-atlas.bgra"
                expected_path.write_bytes(expected)
                expected_pixels = np.frombuffer(
                    expected, dtype=np.uint8).reshape(-1, 4)
                changed_pixels = int(np.count_nonzero(
                    np.any(expected_pixels != base_pixels, axis=1)))
                sample_report = {
                    "label": label,
                    "samples": samples,
                    "expected_file": expected_path.name,
                    "expected_atlas_sha256": hashlib.sha256(expected).hexdigest(),
                    "changed_pixel_count": changed_pixels,
                    "zero_is_exact_base": (
                        expected == decoded["atlas_bgra"]
                        if not any(samples) else None),
                }
                if label in sample_atlases:
                    sample_report["comparison"] = compare_native_samples(
                        native_payload, samples,
                        sample_atlases[label].read_bytes())
                    if not sample_report["comparison"]["all_atlas_bytes_equal"]:
                        failure = "supplied sample atlas differs from the oracle"
                sample_reports.append(sample_report)
            report["sample_compositions"] = sample_reports
        (output_path / "report.json").write_text(
            json.dumps(report, sort_keys=True, separators=(",", ":")),
            encoding="utf-8")
    if failure:
        raise SystemExit(failure)
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))


if __name__ == "__main__":
    main()
