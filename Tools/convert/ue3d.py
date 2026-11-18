"""James-format vertex-mesh decoder (_d.3d / _a.3d pairs) + #exec metadata.

Struct layouts follow the James-format mesh files (FJSDataHeader,
FJSMeshTri, FJSAnivHeader) and the animation vertex format (FMeshVert
bitfields X:11 Y:11 Z:10, little-endian). Positions are raw fixed-point
ints; world scale comes from the .uc `#exec MESHMAP SCALE` metadata,
origin/rotation from `#exec MESH ORIGIN` — nothing is guessed.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import exports_dir  # noqa: E402

# EJSMeshTriType — mesh-triangle type flags
MTT_MASK = 0x7
MTT_NORMAL, MTT_TWOSIDED, MTT_TRANSLUCENT, MTT_MASKED, MTT_MODULATE = 0, 1, 2, 3, 4
MTT_PLACEHOLDER = 8          # weapon-triangle: invisible, positions held item
MTT_UNLIT, MTT_FLAT, MTT_ENVIRONMENT, MTT_NOSMOOTH = 16, 32, 64, 128


def _sx(v, bits):
    return v - (1 << bits) if v >= (1 << (bits - 1)) else v


def load_data(path):
    """_d.3d → dict(numpolys, numverts, tris).

    WoT's importer reads a 36-byte James header and then skips twelve reserved
    bytes, so triangles always begin at byte 48.  Some older authoring-tool
    outputs retain an opaque tail after the polygon records; it is reported but
    does not change the imported mesh.
    """
    d = Path(path).read_bytes()
    if len(d) < 48:
        raise ValueError(f"{path}: truncated James data header")
    numpolys, numverts = struct.unpack_from("<2H", d, 0)
    hdr = 48
    records_end = hdr + numpolys * 16
    if records_end > len(d):
        raise ValueError(f"{path}: {numpolys} polygons extend beyond the file")
    tris = []
    o = hdr
    for triangle_index in range(numpolys):
        i0, i1, i2, ttype, color = struct.unpack_from("<3H2B", d, o)
        if max(i0, i1, i2) >= numverts:
            raise ValueError(
                f"{path}: triangle {triangle_index} vertex is outside 0..{numverts - 1}")
        uvs = [(d[o + 8 + k * 2], d[o + 9 + k * 2]) for k in range(3)]
        texnum, flags = d[o + 14], d[o + 15]
        tris.append(dict(idx=(i0, i1, i2), type=ttype, uvs=uvs,
                         tex=texnum, flags=flags, color=color))
        o += 16
    return dict(numpolys=numpolys, numverts=numverts, tris=tris,
                header_size=hdr, trailing_bytes=len(d) - records_end)


def load_anim(path, numverts):
    """_a.3d → list of frames; each frame = [(x,y,z) raw ints] * numverts."""
    d = Path(path).read_bytes()
    if len(d) < 4:
        raise ValueError(f"{path}: truncated James animation header")
    numframes, framesize = struct.unpack_from("<2H", d, 0)
    if framesize != numverts * 4:
        raise ValueError(f"{path}: FrameSize {framesize} != NumVerts*4 "
                         f"({numverts * 4}) — 16-bit variant? (plan §11)")
    required = 4 + numframes * framesize
    if required > len(d):
        raise ValueError(f"{path}: animation frames extend beyond the file")
    frames = []
    o = 4
    for _ in range(numframes):
        frame = []
        for _ in range(numverts):
            v, = struct.unpack_from("<I", d, o)
            frame.append((_sx(v & 0x7FF, 11),
                          _sx((v >> 11) & 0x7FF, 11),
                          _sx((v >> 22) & 0x3FF, 10)))
            o += 4
        frames.append(frame)
    return frames


_EXEC = re.compile(r"^#exec\s+(MESH|MESHMAP)\s+(\w+)\s+(.*)$", re.I | re.M)
_KV = re.compile(r"(\w+)=(\"[^\"]*\"|\S+)")
_NUM = re.compile(r"-?\d+\.?\d*")


def _f(s, default=0.0):
    m = _NUM.match(str(s))
    return float(m.group()) if m else default


def parse_exec_metadata(script_root, strip_line_comments=False):
    """Scan exported .uc for mesh metadata → {mesh: {import, origin, rotation,
    scale, sequences, notifies, textures}} keyed lowercase. Comment filtering is
    opt-in so established flat conversions retain their historical bytes."""
    meshes = {}

    def m(name):
        return meshes.setdefault(name.lower(), dict(
            name=name, sequences={}, notifies=[], textures={}))

    for uc in Path(script_root).rglob("*.uc"):
        source = uc.read_text(encoding="latin-1")
        if strip_line_comments:
            source = "\n".join(line.split("//", 1)[0]
                               for line in source.splitlines())
        for cmd, sub, rest in _EXEC.findall(source):
            kv = {k.upper(): v.strip('"') for k, v in _KV.findall(rest)}
            sub = sub.upper()
            if cmd.upper() == "MESH":
                if sub == "IMPORT" and "MESH" in kv:
                    mm = m(kv["MESH"])
                    mm["anivfile"] = kv.get("ANIVFILE", "")
                    mm["datafile"] = kv.get("DATAFILE", "")
                    mm["unmirror"] = kv.get("UNMIRROR", "0")
                elif sub == "ORIGIN" and "MESH" in kv:
                    mm = m(kv["MESH"])
                    mm["origin"] = [_f(kv.get(a, 0)) for a in "XYZ"]
                    mm["rot"] = [_f(kv.get(a, 0)) for a in ("PITCH", "YAW", "ROLL")]
                elif sub == "SEQUENCE" and "MESH" in kv:
                    m(kv["MESH"])["sequences"][kv["SEQ"]] = dict(
                        start=int(_f(kv.get("STARTFRAME", 0))),
                        num=int(_f(kv.get("NUMFRAMES", 1))),
                        rate=_f(kv.get("RATE", 30), 30.0))
                elif sub == "NOTIFY" and "MESH" in kv:
                    m(kv["MESH"])["notifies"].append(dict(
                        seq=kv.get("SEQ"), time=_f(kv.get("TIME", 0)),
                        func=kv.get("FUNCTION")))
            elif cmd.upper() == "MESHMAP":
                if sub == "SCALE" and "MESHMAP" in kv:
                    m(kv["MESHMAP"])["scale"] = [_f(kv.get(a, 0.1), 0.1) for a in "XYZ"]
                elif sub == "SETTEXTURE" and "MESHMAP" in kv:
                    m(kv["MESHMAP"])["textures"][int(_f(kv.get("NUM", 0)))] = \
                        kv.get("TEXTURE", "")
    return meshes


if __name__ == "__main__":
    import json
    import sys
    meta = parse_exec_metadata(sys.argv[1] if len(sys.argv) > 1
                               else exports_dir() / "script")
    print(f"meshes with metadata: {len(meta)}")
    if len(sys.argv) > 3:
        data = load_data(sys.argv[2])
        frames = load_anim(sys.argv[3], data["numverts"])
        print(f"verts={data['numverts']} polys={data['numpolys']} "
              f"frames={len(frames)}")
        json.dump(dict(sample_tri=data["tris"][0]), sys.stdout)
