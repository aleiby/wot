"""Minimal GLB writer: triangle primitives grouped per material.

write_glb(path, name, groups[, primitive_extras]) where groups =
{mat_name: (positions, normals, uvs, indices[, uvs1])} with positions/normals
as flat [x,y,z...], uvs [u,v...], indices uint32 tri list. The optional 5th
element emits a TEXCOORD_1 set (lightmap atlas UVs); 4-tuples keep the original
layout. Optional primitive_extras is keyed by material. Coordinates are
passed through verbatim — the caller owns any handedness conversion.
"""
import json
import struct


def _pad(b, align, fill):
    while len(b) % align:
        b += fill
    return b


def write_glb(path, name, groups, primitive_extras=None, asset_extras=None):
    bin_parts = []
    views, accessors, prims, mats = [], [], [], []

    def add_view(data, target):
        off = sum(len(p) for p in bin_parts)
        bin_parts.append(_pad(bytearray(data), 4, b"\0"))
        views.append(dict(buffer=0, byteOffset=off, byteLength=len(data),
                          target=target))
        return len(views) - 1

    def add_accessor(view, comp, count, atype, vmin=None, vmax=None):
        a = dict(bufferView=view, componentType=comp, count=count, type=atype)
        if vmin is not None:
            a["min"], a["max"] = vmin, vmax
        accessors.append(a)
        return len(accessors) - 1

    for mat, vals in groups.items():
        pos, nrm, uv, idx = vals[:4]
        uv1 = vals[4] if len(vals) > 4 else None
        n = len(pos) // 3
        pxyz = [pos[0::3], pos[1::3], pos[2::3]]
        vp = add_view(struct.pack(f"<{len(pos)}f", *pos), 34962)
        vn = add_view(struct.pack(f"<{len(nrm)}f", *nrm), 34962)
        vt = add_view(struct.pack(f"<{len(uv)}f", *uv), 34962)
        vi = add_view(struct.pack(f"<{len(idx)}I", *idx), 34963)
        ap = add_accessor(vp, 5126, n, "VEC3",
                          [min(c) for c in pxyz], [max(c) for c in pxyz])
        an = add_accessor(vn, 5126, n, "VEC3")
        at = add_accessor(vt, 5126, n, "VEC2")
        ai = add_accessor(vi, 5125, len(idx), "SCALAR")
        attrs = dict(POSITION=ap, NORMAL=an, TEXCOORD_0=at)
        if uv1 is not None:
            vt1 = add_view(struct.pack(f"<{len(uv1)}f", *uv1), 34962)
            attrs["TEXCOORD_1"] = add_accessor(vt1, 5126, n, "VEC2")
        mats.append(dict(name=mat, pbrMetallicRoughness=dict(
            metallicFactor=0.0, roughnessFactor=1.0)))
        primitive = dict(attributes=attrs,
                         indices=ai, material=len(mats) - 1, mode=4)
        if primitive_extras is not None and mat in primitive_extras:
            primitive["extras"] = primitive_extras[mat]
        prims.append(primitive)

    doc = dict(
        asset=(dict(version="2.0", generator="wot bsp2glb",
                    extras=asset_extras) if asset_extras
               else dict(version="2.0", generator="wot bsp2glb")),
        scene=0,
        scenes=[dict(nodes=[0])],
        nodes=[dict(mesh=0, name=name)],
        meshes=[dict(name=name, primitives=prims)],
        materials=mats,
        buffers=[dict(byteLength=sum(len(p) for p in bin_parts))],
        bufferViews=views,
        accessors=accessors,
    )
    js = _pad(bytearray(json.dumps(doc, separators=(",", ":")).encode()), 4, b" ")
    bb = bytearray(b"".join(bin_parts))
    total = 12 + 8 + len(js) + 8 + len(bb)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack("<II", len(bb), 0x004E4942)); f.write(bb)


def read_glb_document(path):
    """Read the JSON document from a GLB without third-party dependencies."""
    with open(path, "rb") as handle:
        header = handle.read(20)
        if len(header) != 20:
            raise ValueError(f"truncated GLB header: {path}")
        magic, version, _total, json_size, json_kind = struct.unpack(
            "<IIIII", header)
        if magic != 0x46546C67 or version != 2 or json_kind != 0x4E4F534A:
            raise ValueError(f"invalid GLB 2 JSON header: {path}")
        payload = handle.read(json_size)
        if len(payload) != json_size:
            raise ValueError(f"truncated GLB JSON chunk: {path}")
    return json.loads(payload.decode())


def skinned_glb_has_mapping_carriers(path):
    """Return whether every primitive has the paired mapping carriers.

    Mixed or half-present COLOR_0/TEXCOORD_1 state is invalid: the stripped
    bridge requires both attributes, while canonical import requires neither.
    """
    document = read_glb_document(path)
    states = []
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            attributes = primitive.get("attributes", {})
            color = "COLOR_0" in attributes
            mapping_uv = "TEXCOORD_1" in attributes
            if color != mapping_uv:
                raise ValueError(
                    f"GLB primitive has incomplete mapping carriers: {path}")
            states.append(color)
    if not states:
        raise ValueError(f"GLB has no mesh primitives: {path}")
    if any(state != states[0] for state in states[1:]):
        raise ValueError(f"GLB mixes carrier and carrier-free primitives: {path}")
    return states[0]


def write_skinned_glb(path, name, prims, target_names, carriers=False):
    """GLB with a 1-joint skin and per-frame morph targets.

    prims: [{mat, pos[], nrm[], uv[], idx[],
             color[required with carriers=True RGBA8],
             mapping_uv[required with carriers=True float VEC2],
             targets:[(dpos[], dnrm[]), ...]}]
    — every primitive must carry len(target_names) targets. UE Interchange
    imports this as a SkeletalMesh with named morph targets (targetNames).
    Mapping carriers are opt-in so canonical GLBs retain their historical
    TEXCOORD_0-only, color-free layout.
    """
    bin_parts = []
    views, accessors, mats, gprims = [], [], [], []

    def add_view(data, target=None):
        off = sum(len(p) for p in bin_parts)
        bin_parts.append(_pad(bytearray(data), 4, b"\0"))
        v = dict(buffer=0, byteOffset=off, byteLength=len(data))
        if target:
            v["target"] = target
        views.append(v)
        return len(views) - 1

    def acc(view, comp, count, atype, vmin=None, vmax=None, normalized=False):
        a = dict(bufferView=view, componentType=comp, count=count, type=atype)
        if vmin is not None:
            a["min"], a["max"] = vmin, vmax
        if normalized:
            a["normalized"] = True
        accessors.append(a)
        return len(accessors) - 1

    def vec3(data, minmax=False):
        v = add_view(struct.pack(f"<{len(data)}f", *data), 34962)
        n = len(data) // 3
        if minmax:
            cols = [data[0::3], data[1::3], data[2::3]]
            return acc(v, 5126, n, "VEC3",
                       [min(c) for c in cols], [max(c) for c in cols])
        return acc(v, 5126, n, "VEC3")

    for p in prims:
        n = len(p["pos"]) // 3
        ap = vec3(p["pos"], minmax=True)
        an = vec3(p["nrm"])
        at = acc(add_view(struct.pack(f"<{len(p['uv'])}f", *p["uv"]), 34962),
                 5126, n, "VEC2")
        at_mapping = None
        if carriers:
            if p.get("mapping_uv") is None:
                raise ValueError("carrier GLB primitive is missing TEXCOORD_1 data")
            if len(p["mapping_uv"]) != n * 2:
                raise ValueError(
                    "skinned GLB TEXCOORD_1 mapping count disagrees with vertices")
            at_mapping = acc(add_view(struct.pack(
                f"<{len(p['mapping_uv'])}f", *p["mapping_uv"]), 34962),
                5126, n, "VEC2")
        ac = None
        if carriers:
            if p.get("color") is None:
                raise ValueError("carrier GLB primitive is missing COLOR_0 data")
            if len(p["color"]) != n * 4:
                raise ValueError("skinned GLB COLOR_0 count disagrees with vertices")
            ac = acc(add_view(bytes(p["color"]), 34962),
                     5121, n, "VEC4", normalized=True)
        aj = acc(add_view(struct.pack(f"<{n*4}H", *([0] * n * 4)), 34962),
                 5123, n, "VEC4")
        aw = acc(add_view(struct.pack(f"<{n*4}f", *([1.0, 0, 0, 0] * n)), 34962),
                 5126, n, "VEC4")
        ai = acc(add_view(struct.pack(f"<{len(p['idx'])}I", *p["idx"]), 34963),
                 5125, len(p["idx"]), "SCALAR")
        targets = []
        for dpos, dnrm in p["targets"]:
            targets.append(dict(POSITION=vec3(dpos, minmax=True),
                                NORMAL=vec3(dnrm)))
        mats.append(dict(name=p["mat"], pbrMetallicRoughness=dict(
            metallicFactor=0.0, roughnessFactor=1.0)))
        attributes = dict(POSITION=ap, NORMAL=an, TEXCOORD_0=at,
                          JOINTS_0=aj, WEIGHTS_0=aw)
        if at_mapping is not None:
            attributes["TEXCOORD_1"] = at_mapping
        if ac is not None:
            attributes["COLOR_0"] = ac
        gprims.append(dict(
            attributes=attributes,
            indices=ai, material=len(mats) - 1, mode=4, targets=targets))

    ibm = acc(add_view(struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0,
                                   0, 0, 1, 0, 0, 0, 0, 1)), 5126, 1, "MAT4")
    doc = dict(
        asset=dict(version="2.0", generator="wot mesh2glb"),
        scene=0,
        scenes=[dict(nodes=[0, 1])],
        nodes=[dict(mesh=0, skin=0, name=name),
               dict(name="root")],
        skins=[dict(joints=[1], inverseBindMatrices=ibm, skeleton=1)],
        meshes=[dict(name=name, primitives=gprims,
                     weights=[0.0] * len(target_names),
                     extras=dict(targetNames=list(target_names)))],
        materials=mats,
        buffers=[dict(byteLength=sum(len(p) for p in bin_parts))],
        bufferViews=views,
        accessors=accessors,
    )
    js = _pad(bytearray(json.dumps(doc, separators=(",", ":")).encode()), 4, b" ")
    bb = bytearray(b"".join(bin_parts))
    total = 12 + 8 + len(js) + 8 + len(bb)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack("<II", len(bb), 0x004E4942)); f.write(bb)
