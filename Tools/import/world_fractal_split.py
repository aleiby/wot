"""Deterministic source-GLB splitting for render-gated world fractals.

The BSP converter emits one mesh with one primitive per material slot.  UE's
component render timestamp is therefore too coarse for a procedural texture:
one visible wall makes every fractal slot on the combined component look
rendered.  This module moves selected material primitives into their own GLBs
without rewriting any accessor or buffer data.  Interchange consequently sees
separate static meshes/components while positions, normals, UV0, lightmap UV1,
and indices still point at byte-identical source data.
"""

from copy import deepcopy
import json
from pathlib import Path
import struct


_GLB_MAGIC = 0x46546C67
_GLB_VERSION = 2
_JSON_CHUNK = 0x4E4F534A


def runtime_fractal_texture_keys(flipbook_root):
    """Return case-folded (package, asset-key) identities from runtime records.

    The manifest directory and asset key name the imported sheet, while the
    record's package-qualified object_path is the catalog/runtime authority.
    Requiring those package identities to agree makes this fail closed instead
    of splitting an unrelated same-named texture.
    """
    keys = set()
    manifest_paths = sorted(
        Path(flipbook_root).glob("*/_flipbooks.json"),
        key=lambda path: str(path).casefold())
    if not manifest_paths:
        raise FileNotFoundError(
            f"no runtime flipbook manifests under {Path(flipbook_root)}")
    for manifest_path in manifest_paths:
        package = manifest_path.parent.name
        data = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        for asset_key, record in sorted(
                data.get("runtime", {}).items(),
                key=lambda item: (item[0].casefold(), item[0])):
            object_path = record.get("object_path", "")
            if "." not in object_path:
                raise ValueError(
                    f"runtime record lacks a package-qualified object_path: "
                    f"{manifest_path}: {asset_key}")
            object_package = object_path.split(".", 1)[0]
            if object_package.casefold() != package.casefold():
                raise ValueError(
                    f"runtime record package {object_package!r} disagrees with "
                    f"manifest package {package!r}: {manifest_path}: {asset_key}")
            keys.add((package.casefold(), asset_key.casefold()))
    return keys


def record_bearing_slots(materials, runtime_keys):
    """Find material slots whose package-qualified texture has a live record."""
    slots = []
    for slot, info in sorted(
            materials.items(), key=lambda item: (item[0].casefold(), item[0])):
        identity = (str(info.get("package", "")).casefold(),
                    str(info.get("texture", "")).casefold())
        if identity in runtime_keys:
            slots.append(slot)
    return slots


def _read_glb(path):
    raw = Path(path).read_bytes()
    if len(raw) < 20:
        raise ValueError(f"truncated GLB: {path}")
    magic, version, total = struct.unpack_from("<III", raw, 0)
    if magic != _GLB_MAGIC or version != _GLB_VERSION or total != len(raw):
        raise ValueError(f"invalid GLB 2 header: {path}")
    chunks = []
    offset = 12
    while offset < len(raw):
        if offset + 8 > len(raw):
            raise ValueError(f"truncated GLB chunk header: {path}")
        length, chunk_type = struct.unpack_from("<II", raw, offset)
        offset += 8
        end = offset + length
        if end > len(raw):
            raise ValueError(f"truncated GLB chunk: {path}")
        chunks.append((chunk_type, raw[offset:end]))
        offset = end
    json_chunks = [payload for kind, payload in chunks if kind == _JSON_CHUNK]
    if len(json_chunks) != 1 or chunks[0][0] != _JSON_CHUNK:
        raise ValueError(f"GLB must contain one leading JSON chunk: {path}")
    document = json.loads(json_chunks[0].rstrip(b" \t\r\n\0"))
    return document, chunks


def _write_glb(path, document, source_chunks):
    encoded = bytearray(json.dumps(
        document, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))
    while len(encoded) % 4:
        encoded += b" "
    chunks = [(_JSON_CHUNK, bytes(encoded))]
    chunks.extend((kind, payload) for kind, payload in source_chunks
                  if kind != _JSON_CHUNK)
    total = 12 + sum(8 + len(payload) for _, payload in chunks)
    out = bytearray(struct.pack("<III", _GLB_MAGIC, _GLB_VERSION, total))
    for kind, payload in chunks:
        out += struct.pack("<II", len(payload), kind)
        out += payload
    Path(path).write_bytes(out)


def _part_document(source, primitive_indices, asset_name):
    document = deepcopy(source)
    meshes = document.get("meshes", [])
    if len(meshes) != 1:
        raise ValueError(
            f"world GLB contract requires one mesh, found {len(meshes)}")
    source_primitives = source["meshes"][0].get("primitives", [])
    primitives = [deepcopy(source_primitives[index])
                  for index in primitive_indices]
    if not primitives:
        raise ValueError("cannot emit a world GLB part with no primitives")

    used_materials = sorted({primitive["material"] for primitive in primitives})
    remap = {old: new for new, old in enumerate(used_materials)}
    materials = source.get("materials", [])
    if any(index < 0 or index >= len(materials) for index in used_materials):
        raise ValueError("world GLB primitive has an invalid material index")
    document["materials"] = [deepcopy(materials[index])
                             for index in used_materials]
    for primitive in primitives:
        primitive["material"] = remap[primitive["material"]]
    document["meshes"][0]["primitives"] = primitives
    document["meshes"][0]["name"] = asset_name
    for node in document.get("nodes", []):
        if node.get("mesh") == 0:
            node["name"] = asset_name
    return document


def split_world_glb(source_path, output_dir, split_materials, base_asset_name):
    """Emit a base GLB plus one GLB per selected material, in stable order.

    Returns dictionaries with ``path``, ``asset_name``, and ``material_slot``;
    the base (when non-empty) is first and has a null material_slot.  With an
    empty selection the original source is returned directly and output_dir is
    not touched, which preserves the historical import path byte-for-byte.
    """
    source_path = Path(source_path)
    requested = sorted(set(split_materials),
                       key=lambda value: (value.casefold(), value))
    if not requested:
        return [dict(path=source_path, asset_name=base_asset_name,
                     material_slot=None)]

    document, chunks = _read_glb(source_path)
    meshes = document.get("meshes", [])
    if len(meshes) != 1:
        raise ValueError(
            f"world GLB contract requires one mesh, found {len(meshes)}: "
            f"{source_path}")
    materials = document.get("materials", [])
    material_names = [material.get("name", "") for material in materials]
    if len(material_names) != len(set(material_names)):
        raise ValueError(f"world GLB has duplicate material names: {source_path}")
    material_to_primitives = {name: [] for name in material_names}
    primitives = meshes[0].get("primitives", [])
    for primitive_index, primitive in enumerate(primitives):
        material_index = primitive.get("material")
        if not isinstance(material_index, int) \
                or material_index < 0 or material_index >= len(material_names):
            raise ValueError(
                f"world GLB primitive has an invalid material: {source_path}")
        material_to_primitives[material_names[material_index]].append(
            primitive_index)
    missing = [name for name in requested
               if not material_to_primitives.get(name)]
    if missing:
        raise ValueError(
            f"record-bearing slots are absent from world GLB {source_path}: "
            f"{missing}")

    split_indices = {index for name in requested
                     for index in material_to_primitives[name]}
    base_indices = [index for index in range(len(primitives))
                    if index not in split_indices]
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    parts = []
    if base_indices:
        path = output_dir / f"{base_asset_name}.glb"
        _write_glb(path, _part_document(
            document, base_indices, base_asset_name), chunks)
        parts.append(dict(path=path, asset_name=base_asset_name,
                          material_slot=None))
    for index, material_name in enumerate(requested):
        asset_name = f"{base_asset_name}_Fractal_{index:03d}"
        path = output_dir / f"{asset_name}.glb"
        _write_glb(path, _part_document(
            document, material_to_primitives[material_name], asset_name), chunks)
        parts.append(dict(path=path, asset_name=asset_name,
                          material_slot=material_name))
    return parts
