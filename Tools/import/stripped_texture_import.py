"""Shared guarded import path for payload-bound stripped textures.

This module keeps its argument, PNG, and convergence helpers stdlib-only so
they can be verified without launching Unreal.  Engine callers pass the
already-imported ``unreal`` module to the small mutation helpers at the bottom.
"""
from __future__ import annotations

import binascii
import os
import struct
import zlib
from pathlib import Path

from payload_binding import ensure_texture_payload_binding


# PNG is RGBA; Unreal's imported source/platform byte order is BGRA.  Magenta
# is deliberately symmetric in R/B, so both representations are ff 00 ff ff.
# It is opaque so masked assets display an unmistakable magenta rectangle when
# a required player cache is absent instead of failing subtly.
STRIPPED_PLACEHOLDER_RGBA = (255, 0, 255, 255)
STRIPPED_PLACEHOLDER_BGRA = (255, 0, 255, 255)


def _option_values(arguments, option):
    values = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == option:
            if index + 1 >= len(arguments) or arguments[index + 1].startswith("--"):
                raise RuntimeError(f"{option} requires a value")
            values.append(arguments[index + 1])
            index += 2
            continue
        if argument.startswith(option + "="):
            values.append(argument.split("=", 1)[1])
        index += 1
    if len(set(values)) > 1:
        raise RuntimeError(f"conflicting {option} values")
    return values


def importer_positionals(arguments):
    """Return argv positionals after removing stripped-import options."""
    result = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument in ("--dest", "--mode"):
            if index + 1 >= len(arguments):
                raise RuntimeError(f"{argument} requires a value")
            index += 2
            continue
        if argument in ("--stripped", "--allow-canonical") \
                or argument.startswith("--dest=") \
                or argument.startswith("--mode="):
            index += 1
            continue
        if not argument.startswith("--"):
            result.append(argument)
        index += 1
    return result


def stripped_mode_requested(arguments, environ=None):
    """Resolve the texture payload mode; full remains the default."""
    environ = os.environ if environ is None else environ
    requested = []
    if "--stripped" in arguments:
        requested.append("stripped")
    requested.extend(value.casefold() for value in _option_values(arguments, "--mode"))
    for variable in ("WOT_TEXTURE_IMPORT_MODE", "WOT_PAYLOAD_MODE"):
        value = environ.get(variable, "").strip().casefold()
        if value:
            requested.append(value)
    invalid = sorted(set(requested) - {"full", "stripped"})
    if invalid:
        raise RuntimeError(f"invalid texture import mode: {invalid[0]}")
    if len(set(requested)) > 1:
        raise RuntimeError("conflicting texture import modes")
    return bool(requested and requested[0] == "stripped")


def normalize_unreal_destination(destination, label="texture"):
    if not isinstance(destination, str) or not destination.strip():
        raise RuntimeError(f"stripped {label} destination must be a non-empty path")
    raw = destination.strip().replace("\\", "/")
    if not raw.startswith("/"):
        raise RuntimeError(
            f"stripped {label} destination must be an absolute /Game path: {destination}")
    parts = []
    for component in raw.split("/"):
        if not component or component == ".":
            continue
        if component == "..":
            if not parts:
                raise RuntimeError(
                    f"stripped {label} destination escapes its root: {destination}")
            parts.pop()
        else:
            parts.append(component)
    normalized = "/" + "/".join(parts)
    folded = normalized.casefold()
    if folded != "/game" and not folded.startswith("/game/"):
        raise RuntimeError(
            f"stripped {label} destination must resolve under /Game: {destination}")
    return normalized


def stripped_destination(arguments, canonical_destination, label,
                         environ=None):
    """Resolve and guard the required isolated virtual output root."""
    environ = os.environ if environ is None else environ
    cli_values = _option_values(arguments, "--dest")
    cli = cli_values[0] if cli_values else None
    env = environ.get("WOT_PAYLOAD_CONTENT_ROOT", "").strip() or None
    if cli and env and normalize_unreal_destination(cli, label).casefold() != \
            normalize_unreal_destination(env, label).casefold():
        raise RuntimeError("--dest conflicts with WOT_PAYLOAD_CONTENT_ROOT")
    destination = cli or env
    if not destination:
        raise RuntimeError(
            f"stripped {label} import requires explicit --dest or "
            "WOT_PAYLOAD_CONTENT_ROOT")
    normalized = normalize_unreal_destination(destination, label)
    canonical = normalize_unreal_destination(
        canonical_destination, label).casefold()
    folded = normalized.casefold()
    if (folded == canonical or folded.startswith(canonical + "/")) \
            and "--allow-canonical" not in arguments:
        raise RuntimeError(
            f"stripped {label} destination resolves under canonical "
            f"{canonical_destination}; choose an isolated non-canonical "
            "--dest/WOT_PAYLOAD_CONTENT_ROOT or pass --allow-canonical only "
            "in a disposable workspace")
    return normalized


def png_dimensions(path):
    """Read only the PNG signature/IHDR needed for structural prevalidation."""
    path = Path(path)
    with path.open("rb") as stream:
        header = stream.read(24)
    if (len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n"
            or header[12:16] != b"IHDR"):
        raise RuntimeError(f"invalid PNG header: {path}")
    width, height = struct.unpack(">II", header[16:24])
    if width <= 0 or height <= 0:
        raise RuntimeError(f"invalid PNG dimensions: {path}")
    return width, height


def validate_png(path, *, expected_bit_depth=None, allowed_color_types=None):
    """Read and structurally validate a complete PNG, returning its size.

    Import preflights use this stronger check when a later failure would occur
    after shared assets had already been rebuilt. It validates the signature,
    chunk framing/CRCs, one IHDR, at least one IDAT, and the terminal IEND.
    """
    path = Path(path)
    try:
        payload = path.read_bytes()
    except OSError as exc:
        raise RuntimeError(f"could not read PNG: {path}: {exc}") from exc
    if payload[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError(f"invalid PNG signature: {path}")
    offset = 8
    dimensions = None
    saw_idat = False
    saw_iend = False
    while offset < len(payload):
        if offset + 12 > len(payload):
            raise RuntimeError(f"truncated PNG chunk header: {path}")
        length = struct.unpack(">I", payload[offset:offset + 4])[0]
        chunk_type = payload[offset + 4:offset + 8]
        end = offset + 12 + length
        if end > len(payload):
            raise RuntimeError(f"truncated PNG chunk payload: {path}")
        data = payload[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack(">I", payload[offset + 8 + length:end])[0]
        actual_crc = binascii.crc32(chunk_type + data) & 0xffffffff
        if actual_crc != expected_crc:
            raise RuntimeError(f"PNG chunk CRC mismatch: {path}")
        if chunk_type == b"IHDR":
            if dimensions is not None or offset != 8 or length != 13:
                raise RuntimeError(f"invalid PNG IHDR: {path}")
            width, height = struct.unpack(">II", data[:8])
            if width <= 0 or height <= 0:
                raise RuntimeError(f"invalid PNG dimensions: {path}")
            bit_depth, color_type, compression, filtering, interlace = data[8:13]
            if expected_bit_depth is not None and bit_depth != expected_bit_depth:
                raise RuntimeError(
                    f"unexpected PNG bit depth {bit_depth}: {path}")
            if allowed_color_types is not None \
                    and color_type not in allowed_color_types:
                raise RuntimeError(
                    f"unexpected PNG color type {color_type}: {path}")
            if compression != 0 or filtering != 0 or interlace not in (0, 1):
                raise RuntimeError(f"invalid PNG IHDR methods: {path}")
            dimensions = (width, height)
        elif chunk_type == b"IDAT":
            saw_idat = True
        elif chunk_type == b"IEND":
            if length != 0 or end != len(payload):
                raise RuntimeError(f"invalid terminal PNG IEND: {path}")
            saw_iend = True
            offset = end
            break
        offset = end
    if dimensions is None or not saw_idat or not saw_iend:
        raise RuntimeError(f"incomplete PNG structure: {path}")
    return dimensions


def _png_chunk(chunk_type, payload):
    body = chunk_type + payload
    return (struct.pack(">I", len(payload)) + body
            + struct.pack(">I", binascii.crc32(body) & 0xffffffff))


def solid_placeholder_png(width, height):
    """Return a deterministic, streaming-compressed opaque-magenta RGBA PNG."""
    if (not isinstance(width, int) or isinstance(width, bool) or width <= 0
            or not isinstance(height, int) or isinstance(height, bool) or height <= 0):
        raise ValueError(f"invalid texture dimensions: {width!r}x{height!r}")
    pixel = bytes(STRIPPED_PLACEHOLDER_RGBA)
    row = b"\0" + pixel * width
    compressor = zlib.compressobj(level=9)
    compressed = bytearray()
    for _ in range(height):
        compressed.extend(compressor.compress(row))
    compressed.extend(compressor.flush())
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", ihdr)
            + _png_chunk(b"IDAT", bytes(compressed)) + _png_chunk(b"IEND", b""))


def ensure_placeholder_png(directory, width, height):
    """Materialize the shared placeholder file only when its bytes differ."""
    directory = Path(directory)
    path = directory / f"opaque-magenta-rgba8-{width}x{height}.png"
    expected = solid_placeholder_png(width, height)
    if not path.is_file() or path.read_bytes() != expected:
        directory.mkdir(parents=True, exist_ok=True)
        path.write_bytes(expected)
    return path


def _texture_dimensions(texture):
    for x_name, y_name in (("blueprint_get_size_x", "blueprint_get_size_y"),
                           ("get_size_x", "get_size_y")):
        x_fn = getattr(texture, x_name, None)
        y_fn = getattr(texture, y_name, None)
        if x_fn and y_fn:
            return int(x_fn()), int(y_fn())
    return None


def _import_source_filename(texture):
    try:
        import_data = texture.get_editor_property("asset_import_data")
        getter = getattr(import_data, "get_first_filename", None)
        return getter() if getter else None
    except (AttributeError, RuntimeError):
        return None


def stripped_texture_is_current(texture, placeholder_png, width, height):
    """True when an existing asset demonstrably came from this placeholder."""
    filename = _import_source_filename(texture)
    if not filename or _texture_dimensions(texture) != (width, height):
        return False
    try:
        return Path(filename).resolve() == Path(placeholder_png).resolve()
    except OSError:
        return False


def sync_texture_properties(texture, desired):
    """Converge editable texture settings without dirtying an exact match."""
    changed = set()
    for prop, value in desired:
        if texture.get_editor_property(prop) != value:
            texture.set_editor_property(prop, value)
            changed.add(prop)
    return changed


def prevalidate_stripped_textures(unreal, specs, asset_exists, load_asset):
    """Validate the whole source/target batch before the first asset write."""
    mismatches = []
    for spec in specs:
        source = Path(spec["source_png"])
        if not source.is_file():
            raise RuntimeError(f"missing texture source PNG: {source}")
        actual = png_dimensions(source)
        expected = (spec["width"], spec["height"])
        if actual != expected:
            raise RuntimeError(
                f"texture source dimensions disagree for {source}: "
                f"{actual[0]}x{actual[1]} != {expected[0]}x{expected[1]}")
        asset_path = spec["asset_path"]
        if asset_exists(asset_path):
            asset = load_asset(asset_path)
            if not asset or not isinstance(asset, unreal.Texture2D):
                mismatches.append(asset_path)
    if mismatches:
        raise RuntimeError(
            "stripped texture batch prevalidation found existing non-Texture2D "
            "class mismatches; zero assets were written: " + ", ".join(mismatches))


def import_stripped_texture(unreal, spec, desired_properties=()):
    """Import/converge one placeholder and binding, saving only on change."""
    eal = unreal.EditorAssetLibrary
    att = unreal.AssetToolsHelpers.get_asset_tools()
    asset_path = spec["asset_path"]
    destination, asset_name = asset_path.rsplit("/", 1)
    placeholder = ensure_placeholder_png(
        Path(unreal.Paths.project_saved_dir()) / "StrippedTexturePlaceholders",
        spec["width"], spec["height"])

    texture = unreal.load_asset(asset_path) if eal.does_asset_exist(asset_path) else None
    imported = not texture or not stripped_texture_is_current(
        texture, placeholder, spec["width"], spec["height"])
    if imported:
        task = unreal.AssetImportTask()
        task.filename = str(placeholder)
        task.destination_path = destination
        task.destination_name = asset_name
        task.automated = True
        task.save = False
        task.replace_existing = bool(texture)
        task.replace_existing_settings = False
        att.import_asset_tasks([task])
        texture = unreal.load_asset(asset_path)
    if not texture or not isinstance(texture, unreal.Texture2D):
        raise RuntimeError(f"failed to import stripped texture: {asset_path}")
    if _texture_dimensions(texture) != (spec["width"], spec["height"]):
        raise RuntimeError(f"stripped placeholder has wrong dimensions: {asset_path}")

    changed = sync_texture_properties(
        texture, (*desired_properties, ("never_stream", True)))
    _, binding_changed = ensure_texture_payload_binding(
        unreal, asset_path, spec["package"], spec["source_name"],
        spec["destination_mip_count"], save=False)
    if imported or changed or binding_changed:
        if eal.save_asset(asset_path) is False:
            raise RuntimeError(f"failed to save stripped texture: {asset_path}")
    return texture, imported or bool(changed) or binding_changed
