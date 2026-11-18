"""Pure-Python UE1 package reader and retail payload decoders.

Handles package version 68 (WoT retail 333B) and nearby versions. Used by the
inventory, texture/font/sound, BSP, and fractal conversion tools. Format
behavior is matched to the UE1 package file layout (the shipped loader's
on-disk format).
"""
import json
import math
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import exports_dir  # noqa: E402

TAG = 0x9E2A83C1


class Reader:
    def __init__(self, data):
        self.d = data
        self.o = 0
        self.end = len(data)

    def _require(self, size):
        if (size < 0 or self.o < 0 or self.o + size > self.end or
                self.o + size > len(self.d)):
            raise ValueError("read extends beyond payload")

    def u32(self):
        self._require(4)
        v = struct.unpack_from("<I", self.d, self.o)[0]; self.o += 4; return v

    def i32(self):
        self._require(4)
        v = struct.unpack_from("<i", self.d, self.o)[0]; self.o += 4; return v

    def u16(self):
        self._require(2)
        v = struct.unpack_from("<H", self.d, self.o)[0]; self.o += 2; return v

    def u8(self):
        self._require(1)
        v = self.d[self.o]; self.o += 1; return v

    def cindex(self):
        """UE1 compact signed index."""
        b = self.u8()
        neg = b & 0x80
        v = b & 0x3F
        if b & 0x40:
            shift = 6
            for byte_index in range(4):
                if self.o >= self.end or self.o >= len(self.d):
                    raise ValueError("unterminated compact index")
                b = self.u8()
                v |= (b & 0x7F) << shift
                if not (b & 0x80):
                    return -v if neg else v
                if byte_index == 3:
                    raise ValueError("compact index exceeds 5 bytes")
                shift += 7
        return -v if neg else v


class Package:
    def __init__(self, path):
        self.path = Path(path)
        self.name = self.path.stem      # this package's own name (== on-disk folder)
        with open(self.path, "rb") as f:
            r = Reader(f.read())
        self.d = r.d
        if r.u32() != TAG:
            raise ValueError(f"not a UE package: {path}")
        self.version = r.u16()
        self.licensee = r.u16()
        self.flags = r.u32()
        name_n, name_off = r.i32(), r.i32()
        exp_n, exp_off = r.i32(), r.i32()
        imp_n, imp_off = r.i32(), r.i32()

        self.names = []
        r.o = name_off
        for _ in range(name_n):
            if self.version >= 64:
                ln = r.cindex()
                s = r.d[r.o:r.o + ln - 1].decode("latin-1"); r.o += ln
            else:
                e = r.d.index(b"\0", r.o)
                s = r.d[r.o:e].decode("latin-1"); r.o = e + 1
            r.u32()  # name flags
            self.names.append(s)

        self.imports = []
        r.o = imp_off
        for _ in range(imp_n):
            cpkg = self.names[r.cindex()]
            cname = self.names[r.cindex()]
            pkg = r.i32()
            oname = self.names[r.cindex()]
            self.imports.append((cpkg, cname, pkg, oname))

        self.exports = []
        r.o = exp_off
        for _ in range(exp_n):
            cls, sup = r.cindex(), r.cindex()
            group = r.i32()
            name = self.names[r.cindex()]
            oflags = r.u32()
            size = r.cindex()
            off = r.cindex() if size > 0 else 0
            self.exports.append(dict(cls=cls, sup=sup, group=group,
                                     name=name, flags=oflags,
                                     size=size, off=off))

    def objname(self, ref):
        """Resolve an object reference to a name ('Class' semantics: 0 = UClass).

        WARNING: bare last component — drops the package. For any reference to a
        cross-package ASSET (texture, mesh, skin) use objref()/objpath() so
        same-named objects in different packages don't alias. objname stays only
        for package-local uses (class names, same-package exports)."""
        if ref > 0:
            return self.exports[ref - 1]["name"]
        if ref < 0:
            return self.imports[-ref - 1][3]
        return None

    def objref(self, ref):
        """Authoritative package-qualified identity of an object reference:
        returns (package, name) where package matches the on-disk export folder
        (this package's own name for a same-package export; the outermost import
        name for a cross-package reference) and name is the object's own name.
        Intermediate GROUPS are dropped (exporters flatten to <Package>/<Name>).
        Returns None for a null ref.

        Walks imports[i][2] (outer) / exports[i]['group'] up to the root, the
        same chain bake_flipbooks.resolve_source uses. This is THE key every
        asset lookup must use — never the bare objname()."""
        if ref == 0:
            return None
        if ref > 0:                       # export → object lives in THIS package
            return (self.name, self.exports[ref - 1]["name"])
        imp = self.imports[-ref - 1]
        name = imp[3]
        outer, pkg, guard = imp[2], None, 0
        while outer != 0 and guard < 32:  # climb to the outermost import = package
            if outer > 0:                 # outer is an export in THIS package
                pkg = self.name; break
            o = self.imports[-outer - 1]
            pkg = o[3]; outer = o[2]; guard += 1
        return (pkg or self.name, name)

    def objpath(self, ref):
        """Full dotted 'Package.Group.Name' path (debug/display). None if null."""
        if ref == 0:
            return None
        parts, cur, guard = [], ref, 0
        while cur != 0 and guard < 32:
            if cur > 0:
                e = self.exports[cur - 1]
                parts.append(e["name"]); cur = e["group"]
            else:
                imp = self.imports[-cur - 1]
                parts.append(imp[3]); cur = imp[2]
            guard += 1
        return ".".join(reversed(parts))

    @staticmethod
    def qualkey(package, name):
        """Round-trip-safe, injective material-slot / lookup key for a
        (package, name) asset identity: '<Package>__<Name>'. Verified: no UE1
        package or texture name contains '__' or '.', and Interchange preserves
        '__' verbatim in glTF material slot names (it collapses '.'->'_'), so
        distinct (package,name) pairs never alias to the same key."""
        return f"{package}__{name}"

    def export_class_histogram(self):
        h = {}
        for e in self.exports:
            c = self.objname(e["cls"]) or "Class"
            h[c] = h.get(c, 0) + 1
        return h


# --------------------------------------------------------------------------
# Payload decoding (v68). Serialization layouts follow the UE1 on-disk
# formats: FPropertyTag (tagged properties), FMipmap (texture mips),
# TArray/TLazyArray (arrays), UModel/FBspNode/FBspSurf/FVert (BSP), and
# primitive bounds.
# --------------------------------------------------------------------------

PF_INVISIBLE, PF_MASKED, PF_TRANSLUCENT = 0x1, 0x2, 0x4
PF_ENVIRONMENT = 0x10
PF_MODULATED, PF_FAKEBACKDROP, PF_TWOSIDED = 0x40, 0x80, 0x100
PF_NOSMOOTH, PF_FLAT = 0x800, 0x4000
PF_UNLIT, PF_PORTAL = 0x400000, 0x04000000

_TAG_SIZES = {0x00: 1, 0x10: 2, 0x20: 4, 0x30: 12, 0x40: 16}


RF_HAS_STACK = 0x02000000


def _format_float(value, *, signed=False, width=0):
    """UE1's legacy ``%f`` rounding (half away from zero), at six places."""
    negative = math.copysign(1.0, value) < 0.0
    scaled = math.floor(abs(value) * 1_000_000.0 + 0.5)
    whole, fraction = divmod(scaled, 1_000_000)
    body = f"{whole}.{fraction:06d}"
    if signed:
        sign = "-" if negative else "+"
        return sign + body.zfill(max(0, width - 1))
    return ("-" if negative else "") + body


class PayloadReader(Reader):
    def __init__(self, pkg, export):
        super().__init__(pkg.d)
        self.pkg = pkg
        self.export = export
        self.o = export["off"]
        self.end = export["off"] + export["size"]
        if export["flags"] & RF_HAS_STACK:
            # FStateFrame: Node, StateNode (objrefs), ProbeMask(8),
            # LatentAction(4), then bytecode offset if Node set.
            node = self.cindex()
            self.cindex()
            self.o += 12
            if node:
                self.cindex()

    def name(self):
        index = self.cindex()
        if not 0 <= index < len(self.pkg.names):
            raise ValueError(
                f"name index {index} out of range (0..{len(self.pkg.names) - 1}) "
                f"decoding {self.export['name']!r}")
        return self.pkg.names[index]

    def fvec(self):
        v = struct.unpack_from("<3f", self.d, self.o); self.o += 12; return v

    def properties(self):
        """Tagged properties until None. Returns {name: value} for scalar
        types we care about; skips the rest positionally."""
        props = {}
        while True:
            n = self.name()
            if n == "None":
                return props
            info = self.u8()
            ptype = info & 0x0F
            if ptype == 10:  # StructProperty: extra struct-name
                self.name()
            szbits = info & 0x70
            if szbits in _TAG_SIZES:
                size = _TAG_SIZES[szbits]
            elif szbits == 0x50:
                size = self.u8()
            elif szbits == 0x60:
                size = self.u16()
            else:
                size = self.u32()
            aidx = 0
            if (info & 0x80) and ptype != 3:  # array index (not bool)
                b = self.u8()
                if b & 0x80:
                    if (b & 0xC0) == 0x80:
                        aidx = ((b & 0x7F) << 8) + self.u8()
                    else:
                        aidx = int.from_bytes(self.d[self.o:self.o+3], "big")
                        self.o += 3
                else:
                    aidx = b
            animation_types = {
                "AnimNext": (5, None), "MinFrameRate": (4, 4),
                "MaxFrameRate": (4, 4), "PrimeCount": (1, 1),
            }
            if n in animation_types:
                expected_type, expected_size = animation_types[n]
                if (n in props or ptype != expected_type
                        or aidx != 0
                        or (n == "AnimNext" and not 1 <= size <= 5)
                        or (expected_size is not None and size != expected_size)):
                    raise ValueError(f"malformed or duplicate {n} property")
            if ptype == 3:  # BoolProperty: value in bit7, no payload
                props[n] = bool(info & 0x80)
                continue
            here = self.o
            if ptype == 10:  # struct: keep raw payload for known arrays
                props.setdefault("_structs", {}).setdefault(n, {})[aidx] =                     bytes(self.d[here:here + size])
            if ptype == 1:  # Byte
                props[n] = self.u8()
            elif ptype == 2:  # Int
                props[n] = self.i32()
            elif ptype == 4:  # Float (e.g. Texture.DrawScale — #30 tiling fix;
                #                 was silently dropped before, defaulting to 1.0)
                props[n] = struct.unpack_from("<f", self.d, here)[0]
                self.o += 4
            elif ptype == 5:  # Object ref
                props[n] = self.cindex()
            elif ptype == 6:  # Name (e.g. ZoneInfo.DamageType) — name-table index
                props[n] = self.name()
            elif ptype == 13:  # Str (e.g. LevelSummary.IdealPlayerCount):
                #                 compact-counted Latin-1 buffer incl. its NUL
                count = self.cindex()
                if count > 0:
                    props[n] = self.d[self.o:self.o + count - 1].decode("latin-1")
            if n in animation_types and self.o != here + size:
                raise ValueError(f"{n} property size disagrees with its value")
            self.o = here + size
        return props


def decode_palette(pkg, export):
    r = PayloadReader(pkg, export)
    r.properties()
    n = r.cindex()
    cols = [tuple(r.d[r.o + 4 * i: r.o + 4 * i + 4]) for i in range(n)]
    return cols  # [(R,G,B,A) × 256]


def decode_texture(pkg, export):
    """Plain P8 Texture → metadata, every serialized mip, and mip-0 aliases.

    ``w``, ``h``, and ``data`` remain aliases for mip zero so the existing PNG
    converters keep their API.  ``mips`` exposes the already-decoded
    ``(width, height, indices)`` tuples to payload-cache consumers without a
    second serialization parser.
    """
    r = PayloadReader(pkg, export)
    p = r.properties()
    if p.get("bParametric"):
        raise ValueError("parametric texture — handled by flipbook baker")
    mips = []
    for _ in range(r.cindex()):
        if pkg.version > 61:
            r.i32()                   # TLazyArray seek pos (added v62+; absent
            #                           in older packages e.g. LensFlareT v61)
        cnt = r.cindex()
        data = r.d[r.o:r.o + cnt]; r.o += cnt
        w, h = r.i32(), r.i32()
        r.o += 2                      # UBits, VBits
        mips.append((w, h, data))
    w, h, data = mips[0]
    anim_next = int(p.get("AnimNext", 0))
    anim_next_identity = ""
    if anim_next:
        if anim_next > 0:
            if anim_next > len(pkg.exports):
                raise ValueError("AnimNext export reference is outside the table")
            anim_class = pkg.objname(pkg.exports[anim_next - 1]["cls"])
            anim_next_identity = f"{pkg.name}.{pkg.objpath(anim_next)}"
        else:
            index = -anim_next - 1
            if index >= len(pkg.imports):
                raise ValueError("AnimNext import reference is outside the table")
            anim_class = pkg.imports[index][1]
            anim_next_identity = pkg.objpath(anim_next) or ""
        if (anim_class or "").casefold() != "texture" or not anim_next_identity:
            raise ValueError("AnimNext is not a canonical typed Texture identity")
    min_frame_rate = float(p.get("MinFrameRate", 0.0))
    max_frame_rate = float(p.get("MaxFrameRate", 0.0))
    prime_count = int(p.get("PrimeCount", 0))
    if (not math.isfinite(min_frame_rate) or min_frame_rate < 0.0
            or not math.isfinite(max_frame_rate)
            or max_frame_rate < min_frame_rate
            or not 0 <= prime_count <= 255):
        raise ValueError("malformed texture animation properties")
    return dict(name=export["name"], w=w, h=h, data=data, mips=mips,
                masked=bool(p.get("bMasked")), palette=p.get("Palette", 0),
                format=p.get("Format", 0),
                anim_next=anim_next,
                anim_next_identity=anim_next_identity,
                min_frame_rate=min_frame_rate,
                max_frame_rate=max_frame_rate,
                prime_count=prime_count,
                detail=p.get("DetailTexture", 0),
                macro=p.get("MacroTexture", 0),
                env=bool(p.get("bEnvironment")),
                # UE1 Engine.Texture defaults to 1.0. Legend's
                # CheckSurfaces reads this authored value for both ladders and
                # slippery walk surfaces (the original walking physics).
                friction=float(p.get("Friction", 1.0)),
                drawscale=float(p.get("DrawScale", 1.0)))


def decode_sound(pkg, export):
    """UE1 USound -> ``(format_name, data)``.

    ``USound::Serialize`` writes the file-type FName followed by a
    ``TLazyArray<BYTE>``.  Package versions above 61 prefix a lazy array with
    its absolute end offset; older versions store only the compact array size.
    This split is present in the retail data: all v63/v68 sounds have the
    offset, while FootStubA.uax v61 has none.  In every v63/v68 Sound export
    the saved offset equals the export payload end.
    """
    if pkg.objname(export["cls"]) != "Sound":
        raise ValueError(f"{export['name']} is not a Sound export")

    r = PayloadReader(pkg, export)
    r.properties()
    format_name = r.name()
    if pkg.version > 61:
        data_end = r.i32()
        if data_end != r.end:
            raise ValueError(
                f"unexpected sound data end {data_end} (export ends at {r.end})")
    data_size = r.cindex()
    if data_size < 0 or r.o + data_size != r.end:
        raise ValueError(
            f"invalid sound data size {data_size} ({r.end - r.o} bytes remain)")
    return format_name, bytes(r.d[r.o:r.end])


def export_sounds(package_path, out_dir, names=None):
    """Write each Sound export as flat ``<Name>.wav`` output.

    Returns one output path per Sound export.  Duplicate export names therefore
    appear more than once in the returned list and, like UCC batchexport, the
    later export wins in the flat output directory.
    """
    pkg = Package(package_path)
    requested = set(names) if names else None
    output = Path(out_dir)
    output.mkdir(parents=True, exist_ok=True)
    exported = []
    found = set()
    for export in pkg.exports:
        if pkg.objname(export["cls"]) != "Sound":
            continue
        sound_name = export["name"]
        if requested is not None and sound_name not in requested:
            continue
        # The name comes from untrusted package data (the bootstrap runs this
        # over user-downloaded files) — confine it to a plain UE identifier so
        # it cannot traverse out of the output directory.
        if not re.fullmatch(r"[A-Za-z0-9_. \-]+", sound_name) or ".." in sound_name:
            raise ValueError(
                f"refusing suspicious sound name {sound_name!r} in {package_path}")
        format_name, data = decode_sound(pkg, export)
        if format_name.casefold() != "wav":
            raise ValueError(
                f"unsupported sound format {format_name!r} for {sound_name}")
        target = output / f"{sound_name}.wav"
        target.write_bytes(data)
        exported.append(target)
        found.add(sound_name)

    missing = sorted((requested or set()) - found)
    if missing:
        raise ValueError(f"Sound exports not found in {package_path}: {missing}")
    return exported


# --------------------------------------------------------------------------
# ScriptText and Class-default export
# --------------------------------------------------------------------------

# Serialized UnrealScript bytecode stores object/name references as compact
# package indexes, although those references occupy four bytes in the in-memory
# bytecode size recorded by UStruct.  These token groups are enough to advance
# both representations without interpreting the program.
_EXPR_NO_OPERANDS = frozenset((
    0x08, 0x0B, 0x16, 0x17, 0x25, 0x26, 0x27, 0x28, 0x2A, 0x2D, 0x30, 0x31,
))
_EXPR_OBJECT_REF = frozenset((0x00, 0x01, 0x02, 0x20, 0x29))
_EXPR_TWO_EXPRESSIONS = frozenset((0x0F, 0x10, 0x14, 0x1A))


def _skip_serialized_script(r, memory_size):
    """Advance over bytecode while accounting for compact saved references."""
    memory_offset = 0

    def raw(size):
        nonlocal memory_offset
        r._require(size)
        r.o += size
        memory_offset += size

    def object_ref():
        nonlocal memory_offset
        r.cindex()
        memory_offset += 4

    def name_ref():
        nonlocal memory_offset
        value = r.cindex()
        memory_offset += 4
        return value

    def parameters():
        while expression() != 0x16:
            pass

    def expression():
        nonlocal memory_offset
        token = r.u8()
        memory_offset += 1
        if 0x39 <= token < 0x60:          # scalar conversion
            expression()
        elif token >= 0x70:               # one-byte native function id
            parameters()
        elif token >= 0x60:               # extended native function id
            raw(1)
            parameters()
        elif token in _EXPR_TWO_EXPRESSIONS:
            expression()
            expression()
        elif token == 0x06:               # jump
            raw(2)
        elif token in _EXPR_OBJECT_REF:
            object_ref()
        elif token in _EXPR_NO_OPERANDS:
            pass
        elif token in (0x04, 0x0D, 0x0E):
            expression()
        elif token == 0x1C:               # final function
            object_ref()
            parameters()
        elif token in (0x1B, 0x38):       # virtual/global function
            name_ref()
            parameters()
        elif token in (0x12, 0x19):       # class/object context
            expression()
            raw(3)
            expression()
        elif token == 0x11:               # new
            for _ in range(4):
                expression()
        elif token in (0x1D, 0x1E):       # int/float constant
            raw(4)
        elif token == 0x1F:               # ANSI string constant
            while True:
                byte = r.u8()
                memory_offset += 1
                if not byte:
                    break
        elif token == 0x34:               # UTF-16 string constant
            while True:
                value = bytes(r.d[r.o:r.o + 2])
                raw(2)
                if value == b"\0\0":
                    break
        elif token == 0x21:               # name constant
            name_ref()
        elif token in (0x22, 0x23):       # rotator/vector constant
            raw(12)
        elif token in (0x24, 0x2C):       # byte-sized number
            raw(1)
        elif token in (0x13, 0x2E):       # meta/dynamic cast
            object_ref()
            expression()
        elif token == 0x07:               # conditional jump
            raw(2)
            expression()
        elif token == 0x2F:               # iterator
            expression()
            raw(2)
        elif token == 0x05:               # switch
            raw(1)
            expression()
        elif token == 0x09:               # assert
            raw(2)
            expression()
        elif token == 0x0A:               # case
            r._require(2)
            destination = struct.unpack_from("<H", r.d, r.o)[0]
            raw(2)
            if destination != 0xFFFF:
                expression()
        elif token == 0x0C:               # label table
            if memory_offset & 3:
                raise ValueError("unaligned UnrealScript label table")
            while True:
                label = name_ref()
                raw(4)
                if label == 0:
                    break
        elif token == 0x18:               # skip
            raw(2)
            expression()
        elif token in (0x32, 0x33):       # struct comparison
            object_ref()
            expression()
            expression()
        elif token == 0x36:               # struct member
            object_ref()
            expression()
        else:
            raise ValueError(f"unknown UnrealScript expression token 0x{token:02x}")
        return token

    while memory_offset < memory_size:
        expression()
    if memory_offset != memory_size:
        raise ValueError(
            f"UnrealScript bytecode size mismatch: {memory_offset} != {memory_size}")


def decode_script_text(pkg, export):
    """Decode one ScriptText TextBuffer, excluding its serialized trailing NUL.

    Retail v68 evidence for every TextBuffer is: tagged UObject properties,
    cursor/top int32 values, then one compact-counted ANSI buffer.  The count
    includes a NUL and consumes the rest of the export payload exactly.
    """
    if pkg.objname(export["cls"]) != "TextBuffer":
        raise ValueError(f"{export['name']} is not a TextBuffer export")
    r = PayloadReader(pkg, export)
    properties = r.properties()
    cursor = r.i32()
    top = r.i32()
    text_size = r.cindex()
    if text_size <= 0 or r.o + text_size != r.end:
        raise ValueError(
            f"invalid TextBuffer size {text_size} ({r.end - r.o} bytes remain)")
    text = bytes(r.d[r.o:r.end])
    if text[-1:] != b"\0" or b"\0" in text[:-1]:
        raise ValueError(f"TextBuffer {export['name']} is not one NUL-terminated string")
    return dict(text=text[:-1], cursor=cursor, top=top, properties=properties)


def _read_property_tag(r, *, saved_class_defaults=False):
    name = r.name()
    if name == "None":
        return None
    info = r.u8()
    property_type = info & 0x0F
    struct_name = r.name() if property_type == 10 else None
    size_bits = info & 0x70
    if size_bits in _TAG_SIZES:
        size = _TAG_SIZES[size_bits]
    elif size_bits == 0x50:
        size = r.u8()
    elif size_bits == 0x60:
        size = r.u16()
    else:
        size = r.u32()
    array_index = 0
    if (info & 0x80) and property_type != 3:
        byte = r.u8()
        if not byte & 0x80:
            array_index = byte
        elif (byte & 0xC0) == 0x80:
            array_index = ((byte & 0x7F) << 8) + r.u8()
        else:
            array_index = ((byte & 0x3F) << 24) + int.from_bytes(
                r.d[r.o:r.o + 3], "big")
            r.o += 3
    # Ordinary UObject BoolProperty tags carry their value in info bit 7 and
    # have no payload.  Saved UClass defaults are the one UE1 serialization
    # form that retains the declared byte span after the bool tag.
    if property_type == 3:
        if saved_class_defaults:
            r._require(size)
            r.o += size
        value = b""
    else:
        r._require(size)
        value = bytes(r.d[r.o:r.o + size])
        r.o += size
    return dict(name=name, type=property_type, struct=struct_name,
                array_index=array_index, info=info, value=value)


def _read_struct_header(pkg, export):
    """Return the saved UStruct header and leave no dependency on C++ sizes."""
    r = PayloadReader(pkg, export)
    if export["cls"] != 0:
        r.properties()
    super_field = r.cindex()
    next_field = r.cindex()
    script_text = r.cindex()
    children = r.cindex()
    friendly_index = r.cindex()
    if not 0 <= friendly_index < len(pkg.names):
        raise ValueError(f"bad friendly-name index in {pkg.name}.{export['name']}")
    line = r.i32()
    text_position = r.i32()
    memory_script_size = r.i32()
    if memory_script_size < 0:
        raise ValueError(f"negative script size in {pkg.name}.{export['name']}")
    _skip_serialized_script(r, memory_script_size)
    return r, dict(super=super_field, next=next_field, script_text=script_text,
                   children=children, friendly=pkg.names[friendly_index],
                   line=line, text_position=text_position,
                   script_size=memory_script_size)


def _read_class_defaults(pkg, export):
    r, header = _read_struct_header(pkg, export)
    # UClass inherits the saved UState masks/table fields before its own flags,
    # GUID, dependency lists, package imports, within-class, and config name.
    r._require(42)
    r.o += 42
    dependency_count = r.cindex()
    if dependency_count < 0:
        raise ValueError(f"negative dependency count in {pkg.name}.{export['name']}")
    for _ in range(dependency_count):
        r.cindex()
        r.i32()
        r.u32()
    import_count = r.cindex()
    if import_count < 0:
        raise ValueError(f"negative import count in {pkg.name}.{export['name']}")
    for _ in range(import_count):
        r.name()
    r.cindex()  # ClassWithin
    r.name()    # ClassConfigName
    tags = []
    while True:
        tag = _read_property_tag(r, saved_class_defaults=True)
        if tag is None:
            break
        tags.append(tag)
    if r.o != r.end:
        raise ValueError(
            f"Class defaults leave {r.end - r.o} bytes in {pkg.name}.{export['name']}")
    header["defaults"] = tags
    return header


class _ScriptResolver:
    """Small reflection view used only to render UCC-compatible defaults."""
    _PROPERTY_TYPES = {
        "ByteProperty": "byte", "IntProperty": "int",
        "BoolProperty": "bool", "FloatProperty": "float",
        "ObjectProperty": "object", "ClassProperty": "object",
        "NameProperty": "name", "StrProperty": "string",
        "StringProperty": "string", "StructProperty": "struct",
        "ArrayProperty": "array", "FixedArrayProperty": "fixed_array",
        "MapProperty": "map",
    }

    def __init__(self, package_path, *, legacy_float_rounding=False):
        package_path = Path(package_path)
        self.legacy_float_rounding = legacy_float_rounding
        self.paths = {path.stem.casefold(): path for path in package_path.parent.glob("*.u")}
        self.paths[package_path.stem.casefold()] = package_path
        self.packages = {}
        self.export_maps = {}
        self.property_export_maps = {}
        self.find_property_cache = {}
        self.class_headers = {}
        self.property_infos = {}
        self.struct_fields = {}
        self.enum_values = {}
        self.effective_defaults = {}
        self.decoded_defaults = {}

    def package(self, name):
        key = name.casefold()
        if key not in self.paths:
            return None
        if key not in self.packages:
            self.packages[key] = Package(self.paths[key])
        return self.packages[key]

    def export_map(self, pkg):
        key = pkg.name.casefold()
        if key not in self.export_maps:
            result = {}
            for index in range(1, len(pkg.exports) + 1):
                result.setdefault(pkg.objpath(index).casefold(), []).append(index)
            self.export_maps[key] = result
        return self.export_maps[key]

    def property_export_map(self, pkg):
        """(owner export, case-folded property name) -> property export.

        Level export resolves hundreds of thousands of actor tags.  Scanning a
        package's complete export table for every inherited property is
        needlessly quadratic (Engine.u alone has several thousand fields), so
        retain the same lookup semantics behind a package-local index.
        """
        key = pkg.name.casefold()
        if key not in self.property_export_maps:
            result = {}
            for index, export in enumerate(pkg.exports, 1):
                if pkg.objname(export["cls"]) in self._PROPERTY_TYPES:
                    result.setdefault(
                        (export["group"], export["name"].casefold()), index)
            self.property_export_maps[key] = result
        return self.property_export_maps[key]

    def resolve(self, pkg, ref):
        if ref == 0:
            return None
        if ref > 0:
            if ref > len(pkg.exports):
                return None
            return pkg, ref
        path = pkg.objpath(ref)
        if not path or "." not in path:
            return None
        package_name, object_path = path.split(".", 1)
        target = self.package(package_name)
        if target is None:
            return None
        candidates = self.export_map(target).get(object_path.casefold(), [])
        imported_class = pkg.imports[-ref - 1][1]
        index = next((candidate for candidate in candidates
                      if ((imported_class == "Class" and
                           target.exports[candidate - 1]["cls"] == 0)
                          or target.objname(
                              target.exports[candidate - 1]["cls"]) == imported_class)),
                     None)
        return (target, index) if index is not None else None

    @staticmethod
    def key(resolved):
        pkg, index = resolved
        return pkg.name.casefold(), index

    def class_header(self, resolved):
        key = self.key(resolved)
        if key not in self.class_headers:
            pkg, index = resolved
            export = pkg.exports[index - 1]
            if export["cls"] != 0:
                raise ValueError(f"{pkg.objpath(index)} is not a Class export")
            self.class_headers[key] = _read_class_defaults(pkg, export)
        return self.class_headers[key]

    def superclass(self, resolved):
        pkg, index = resolved
        return self.resolve(pkg, pkg.exports[index - 1]["sup"])

    def property_info(self, resolved):
        key = self.key(resolved)
        if key in self.property_infos:
            return self.property_infos[key]
        pkg, index = resolved
        export = pkg.exports[index - 1]
        class_name = pkg.objname(export["cls"])
        kind = self._PROPERTY_TYPES.get(class_name)
        if kind is None:
            raise ValueError(f"unsupported reflected field class {class_name!r}")
        r = PayloadReader(pkg, export)
        r.properties()
        super_field = r.cindex()
        next_field = r.cindex()
        array_dim = r.i32()
        flags = r.u32()
        category = r.name()
        if flags & 0x20:  # replicated property
            r.u16()
        info = dict(name=export["name"], kind=kind, array_dim=array_dim,
                    flags=flags, category=category, next=next_field,
                    super=super_field, package=pkg, index=index)
        info["owner"] = (pkg, export["group"])
        if kind == "byte":
            info["enum"] = r.cindex()
        elif class_name == "ObjectProperty":
            info["property_class"] = r.cindex()
        elif class_name == "ClassProperty":
            info["property_class"] = r.cindex()
            info["meta_class"] = r.cindex()
        elif kind == "struct":
            info["struct_ref"] = r.cindex()
        elif kind == "array":
            info["inner"] = r.cindex()
        elif kind == "fixed_array":
            info["inner"] = r.cindex()
            info["count"] = r.i32()
        elif kind == "map":
            info["key"] = r.cindex()
            info["value"] = r.cindex()
        self.property_infos[key] = info
        return info

    def find_property(self, class_ref, name):
        wanted = name.casefold()
        cache_key = self.key(class_ref), wanted
        if cache_key in self.find_property_cache:
            return self.find_property_cache[cache_key]
        current = class_ref
        while current is not None:
            pkg, index = current
            field_index = self.property_export_map(pkg).get((index, wanted))
            if field_index is not None:
                result = self.property_info((pkg, field_index))
                self.find_property_cache[cache_key] = result
                return result
            current = self.superclass(current)
        self.find_property_cache[cache_key] = None
        return None

    def fields(self, struct_ref, _ancestor_count=0):
        if _ancestor_count > 32:
            raise ValueError("superstruct chain exceeds 32 ancestors")
        key = self.key(struct_ref)
        if key in self.struct_fields:
            return self.struct_fields[key]
        pkg, index = struct_ref
        export = pkg.exports[index - 1]
        _r, header = _read_struct_header(pkg, export)
        fields = []
        field = self.resolve(pkg, header["children"])
        seen = set()
        while field is not None and self.key(field) not in seen:
            seen.add(self.key(field))
            field_pkg, field_index = field
            field_export = field_pkg.exports[field_index - 1]
            if field_pkg.objname(field_export["cls"]) in self._PROPERTY_TYPES:
                fields.append(self.property_info(field))
            field_reader = PayloadReader(field_pkg, field_export)
            field_reader.properties()
            field_reader.cindex()
            field = self.resolve(field_pkg, field_reader.cindex())
        inherited = self.resolve(pkg, header["super"])
        if inherited is not None:
            fields.extend(self.fields(inherited, _ancestor_count + 1))
        self.struct_fields[key] = fields
        return fields

    def enum_names(self, prop):
        resolved = self.resolve(prop["package"], prop.get("enum", 0))
        if resolved is None:
            return None
        key = self.key(resolved)
        if key not in self.enum_values:
            pkg, index = resolved
            r = PayloadReader(pkg, pkg.exports[index - 1])
            r.properties()
            r.cindex()
            r.cindex()
            count = r.cindex()
            self.enum_values[key] = [r.name() for _ in range(count)]
        return self.enum_values[key]

    def read_value(self, prop, r):
        kind = prop["kind"]
        if kind == "byte":
            return r.u8()
        if kind == "int":
            return r.i32()
        if kind == "bool":
            return bool(r.u8())
        if kind == "float":
            r._require(4)
            value = struct.unpack_from("<f", r.d, r.o)[0]
            r.o += 4
            return value
        if kind == "object":
            return self.object_identity(r.pkg, r.cindex())
        if kind == "name":
            return r.name()
        if kind == "string":
            count = r.cindex()
            if count == 0:
                return ""
            if count < 0:
                size = -count * 2
                r._require(size)
                raw = bytes(r.d[r.o:r.o + size])
                r.o += size
                return raw[:-2].decode("utf-16-le")
            r._require(count)
            raw = bytes(r.d[r.o:r.o + count])
            r.o += count
            return raw[:-1].decode("latin-1")
        if kind == "struct":
            struct_ref = self.resolve(prop["package"], prop["struct_ref"])
            if struct_ref is None:
                raise ValueError(f"cannot resolve struct for {prop['name']}")
            return self.read_struct(struct_ref, r)
        if kind == "array":
            inner = self.property_info(self.resolve(prop["package"], prop["inner"]))
            count = r.cindex()
            if count < 0:
                raise ValueError(f"negative array size for {prop['name']}")
            return [self.read_value(inner, r) for _ in range(count)]
        if kind == "fixed_array":
            inner = self.property_info(self.resolve(prop["package"], prop["inner"]))
            return [self.read_value(inner, r) for _ in range(prop["count"])]
        if kind == "map":
            return None
        raise ValueError(f"cannot read property kind {kind}")

    def read_struct(self, struct_ref, r):
        result = []
        for prop in self.fields(struct_ref):
            if prop["flags"] & (0x1000 | 0x2000):  # native or transient
                continue
            values = [self.read_value(prop, r) for _ in range(prop["array_dim"])]
            result.append((prop, values))
        return result

    def object_identity(self, pkg, ref):
        if ref == 0:
            return None
        if ref > 0:
            export = pkg.exports[ref - 1]
            path = pkg.objpath(ref)
            return pkg.objname(export["cls"]) or "Class", f"{pkg.name}.{path}"
        imported = pkg.imports[-ref - 1]
        return imported[1], pkg.objpath(ref)

    def decoded_tag(self, class_ref, tag, prop):
        if tag["type"] == 3:
            return bool(tag["info"] & 0x80)
        value_reader = PayloadReader.__new__(PayloadReader)
        Reader.__init__(value_reader, tag["value"])
        value_reader.pkg = class_ref[0]
        value_reader.export = None
        value = self.read_value(prop, value_reader)
        if value_reader.o != value_reader.end:
            raise ValueError(
                f"{prop['name']} leaves {value_reader.end - value_reader.o} value bytes")
        return value

    def effective(self, class_ref):
        key = self.key(class_ref)
        if key in self.effective_defaults:
            return self.effective_defaults[key]
        parent = self.superclass(class_ref)
        result = dict(self.effective(parent)) if parent is not None else {}
        for tag in self.class_header(class_ref)["defaults"]:
            result[(tag["name"].casefold(), tag["array_index"])] = (class_ref, tag)
        self.effective_defaults[key] = result
        return result

    def is_child_of(self, class_ref, possible_parent):
        current = class_ref
        parent_key = self.key(possible_parent)
        while current is not None:
            if self.key(current) == parent_key:
                return True
            current = self.superclass(current)
        return False

    def values_equal(self, left, right):
        if isinstance(left, str) and isinstance(right, str):
            return left.casefold() == right.casefold()
        if isinstance(left, list) and isinstance(right, list):
            if (left and isinstance(left[0], tuple) and len(left[0]) == 2
                    and isinstance(left[0][0], dict)):
                if len(left) != len(right):
                    return False
                return all(
                    a[0]["name"].casefold() == b[0]["name"].casefold()
                    and self.values_equal(a[1], b[1])
                    for a, b in zip(left, right))
            return len(left) == len(right) and all(
                self.values_equal(a, b) for a, b in zip(left, right))
        return left == right

    def zero_value(self, prop):
        kind = prop["kind"]
        if kind in ("byte", "int"):
            return 0
        if kind == "float":
            return 0.0
        if kind == "bool":
            return False
        if kind == "object":
            return None
        if kind in ("name", "string"):
            return "None" if kind == "name" else ""
        if kind == "array":
            return []
        if kind == "fixed_array":
            inner = self.property_info(self.resolve(prop["package"], prop["inner"]))
            return [self.zero_value(inner) for _ in range(prop["count"])]
        if kind == "struct":
            struct_ref = self.resolve(prop["package"], prop["struct_ref"])
            return [(field, [self.zero_value(field) for _ in range(field["array_dim"])])
                    for field in self.fields(struct_ref)
                    if not field["flags"] & (0x1000 | 0x2000)]
        return None

    def format_value(self, prop, value, default=None):
        kind = prop["kind"]
        if kind == "byte":
            names = self.enum_names(prop)
            return names[value] if names is not None and value < len(names) else str(value)
        if kind == "int":
            return str(value)
        if kind == "bool":
            return "True" if value else "False"
        if kind == "float":
            return (_format_float(value) if self.legacy_float_rounding
                    else f"{value:.6f}")
        if kind == "object":
            return "None" if value is None else f"{value[0]}'{value[1]}'"
        if kind == "name":
            return value
        if kind == "string":
            return f'"{value}"'
        if kind == "array":
            inner = self.property_info(self.resolve(prop["package"], prop["inner"]))
            old = default if isinstance(default, list) else []
            return "(" + ",".join(
                self.format_value(inner, item, old[index] if index < len(old) else None)
                for index, item in enumerate(value)) + ")"
        if kind == "fixed_array":
            inner = self.property_info(self.resolve(prop["package"], prop["inner"]))
            old = default if isinstance(default, list) else []
            return "(" + ",".join(
                self.format_value(inner, item, old[index] if index < len(old) else None)
                for index, item in enumerate(value)) + ")"
        if kind == "struct":
            parent = default if default is not None else self.zero_value(prop)
            parent_by_name = {field[0]["name"].casefold(): field[1] for field in parent}
            parts = []
            for field, values in value:
                old_values = parent_by_name.get(
                    field["name"].casefold(),
                    [self.zero_value(field) for _ in range(field["array_dim"])])
                for index, item in enumerate(values):
                    old = old_values[index]
                    if self.values_equal(item, old):
                        continue
                    field_name = (field["name"] if field["array_dim"] == 1
                                  else f"{field['name']}[{index}]")
                    parts.append(
                        f"{field_name}={self.format_value(field, item, old)}")
            return "(" + ",".join(parts) + ")" if parts else ""
        return ""

    def default_value(self, class_ref, prop, array_index=0):
        """Effective CDO value for one reflected property element."""
        cache_key = (self.key(class_ref), prop["name"].casefold(), array_index)
        if cache_key in self.decoded_defaults:
            return self.decoded_defaults[cache_key]
        prior = self.effective(class_ref).get(
            (prop["name"].casefold(), array_index))
        if prior is None:
            value = self.zero_value(prop)
        else:
            default_class, tag = prior
            default_prop = self.find_property(default_class, tag["name"]) or prop
            value = self.decoded_tag(default_class, tag, default_prop)
        self.decoded_defaults[cache_key] = value
        return value

    def render_defaults(self, class_ref):
        parent = self.superclass(class_ref)
        parent_defaults = self.effective(parent) if parent is not None else {}
        lines = []
        for tag in self.class_header(class_ref)["defaults"]:
            prop = self.find_property(class_ref, tag["name"])
            if prop is None:
                raise ValueError(
                    f"cannot resolve property {tag['name']} for "
                    f"{class_ref[0].objpath(class_ref[1])}")
            value = self.decoded_tag(class_ref, tag, prop)
            owner_in_parent = (parent is not None and
                               self.is_child_of(parent, prop["owner"]))
            prior = (parent_defaults.get(
                (tag["name"].casefold(), tag["array_index"]))
                if owner_in_parent else None)
            old_value = None
            has_comparison = False
            if prior is not None:
                old_class, old_tag = prior
                old_prop = self.find_property(old_class, old_tag["name"]) or prop
                old_value = self.decoded_tag(old_class, old_tag, old_prop)
                has_comparison = True
            elif not owner_in_parent:
                old_value = self.zero_value(prop)
                has_comparison = True
            if has_comparison and self.values_equal(value, old_value):
                continue
            text = self.format_value(prop, value, old_value)
            if not text and prop["kind"] == "struct":
                continue
            property_name = (prop["name"] if prop["array_dim"] == 1
                             else f"{prop['name']}({tag['array_index']})")
            lines.append(f"     {property_name}={text}\r\n")
        return "".join(lines).encode("latin-1")


def export_scripts(package_path, out_dir, names=None):
    """Write each ScriptText-bearing class as flat ``<Class>.uc`` output."""
    resolver = _ScriptResolver(package_path)
    pkg = resolver.package(Path(package_path).stem)
    requested = {name.casefold(): name for name in names} if names else None
    output = Path(out_dir)
    output.mkdir(parents=True, exist_ok=True)
    exported = []
    found = set()
    for export in pkg.exports:
        if pkg.objname(export["cls"]) != "TextBuffer":
            continue
        if export["group"] <= 0 or export["group"] > len(pkg.exports):
            raise ValueError(f"orphan ScriptText in {package_path}")
        class_export = pkg.exports[export["group"] - 1]
        if class_export["cls"] != 0:
            raise ValueError(f"ScriptText outer {class_export['name']} is not a Class")
        class_name = class_export["name"]
        key = class_name.casefold()
        if requested is not None and key not in requested:
            continue
        if not re.fullmatch(r"[A-Za-z0-9_]+", class_name):
            raise ValueError(
                f"refusing suspicious class name {class_name!r} in {package_path}")
        decoded = decode_script_text(pkg, export)
        class_ref = (pkg, export["group"])
        defaults = resolver.render_defaults(class_ref)
        # Retail UCC presents exactly one blank line at the join even where the
        # saved source has one or three trailing CRLFs.
        source = decoded["text"].rstrip(b" \r\n") + b"\r\n\r\n"
        data = (source + b"defaultproperties\r\n{\r\n" +
                defaults + b"}\r\n")
        target = output / f"{class_name}.uc"
        target.write_bytes(data)
        exported.append(target)
        found.add(key)
    missing = sorted(original for key, original in (requested or {}).items()
                     if key not in found)
    if missing:
        raise ValueError(f"ScriptText classes not found in {package_path}: {missing}")
    return exported


# --------------------------------------------------------------------------
# Level/T3D export
# --------------------------------------------------------------------------

def read_property_tags(r, *, saved_class_defaults=False):
    """Read one raw FPropertyTag stream through the shared UE1 tag reader."""
    tags = []
    while True:
        tag = _read_property_tag(
            r, saved_class_defaults=saved_class_defaults)
        if tag is None:
            return tags
        tags.append(tag)


# Private compatibility name for the conversion tools that predate the shared
# Task-23/24 typed-property representation.
_property_tags = read_property_tags


def _level_reader_after_actor_refs(pkg):
    levels = [export for export in pkg.exports
              if pkg.objname(export["cls"]) == "Level"]
    if len(levels) != 1:
        raise ValueError(
            f"{pkg.path}: expected one Level export, found {len(levels)}")
    r = PayloadReader(pkg, levels[0])
    _property_tags(r)
    count = r.i32()
    maximum = r.i32()
    if count < 0 or maximum < count or count > len(pkg.exports) * 2:
        raise ValueError(
            f"{pkg.path}: invalid Level actor database {count}/{maximum}")
    refs = [r.cindex() for _ in range(count)]
    if any(ref < 0 or ref > len(pkg.exports) for ref in refs):
        raise ValueError(f"{pkg.path}: Level actor list contains an invalid export ref")
    return r, refs


def _level_actor_refs(pkg):
    _, refs = _level_reader_after_actor_refs(pkg)
    return refs


def _fstring(r):
    """Read a compact-counted UE1 FString, including its saved terminator."""
    length = r.cindex()
    if length == 0:
        return ""
    char_count = abs(length)
    if char_count > 1_000_000:
        raise ValueError(f"invalid FString length {length}")
    byte_count = char_count if length > 0 else char_count * 2
    r._require(byte_count)
    data = bytes(r.d[r.o:r.o + byte_count])
    r.o += byte_count
    terminator = b"\0" if length > 0 else b"\0\0"
    if not data.endswith(terminator):
        raise ValueError("FString is not NUL-terminated")
    encoding = "latin-1" if length > 0 else "utf-16-le"
    return data[:-len(terminator)].decode(encoding)


def _skip_level_url(r):
    # FURL: Protocol, Host, Map, Portal, Op, Port, Valid.
    for _ in range(4):
        _fstring(r)
    option_count = r.cindex()
    if option_count < 0 or option_count > 65_536:
        raise ValueError(f"invalid FURL option count {option_count}")
    for _ in range(option_count):
        _fstring(r)
    r.i32()  # Port
    r.i32()  # Valid (serialized UBOOL)


def _level_count(r, maximum, label):
    count = r.cindex()
    if count < 0 or count > maximum:
        raise ValueError(f"invalid {label} count {count}")
    return count


def _level_object_ref(pkg, r, label):
    ref = r.cindex()
    imports = getattr(pkg, "imports", ())
    if ref < -len(imports) or ref > len(pkg.exports):
        raise ValueError(f"{pkg.path}: {label} has invalid object ref {ref}")
    return ref


def _skip_level_travel_map(r):
    count = _level_count(r, 65_536, "TravelInfo")
    for _ in range(count):
        _fstring(r)
        _fstring(r)


def _skip_level_string_array(r, label):
    count = _level_count(r, 65_536, label)
    for _ in range(count):
        _fstring(r)
    return count


def _skip_destroy_observer_map(pkg, r):
    count = _level_count(r, 4_000_000, "destroy-observer map")
    reference_count = count
    for _ in range(count):
        _level_object_ref(pkg, r, "destroy-observer subject")
        observer_count = _level_count(r, 4_000_000, "destroy-observer array")
        reference_count += observer_count
        if reference_count > 4_000_000:
            raise ValueError("destroy-observer references exceed their bound")
        for _ in range(observer_count):
            _level_object_ref(pkg, r, "destroy observer")


def _decode_level_reachspecs(pkg):
    """Decode and fully consume a ULevel, returning ReachSpecs and layout facts."""
    r, _ = _level_reader_after_actor_refs(pkg)
    _skip_level_url(r)
    r.cindex()  # ULevel.Model
    count = _level_count(r, 10_000_000, "ReachSpec")

    def endpoint_name(ref):
        if ref <= 0:
            return ""
        if ref > len(pkg.exports):
            raise ValueError(
                f"{pkg.path}: ReachSpec endpoint has invalid export ref {ref}")
        return pkg.objname(ref) or ""

    reachspecs = []
    for _ in range(count):
        distance = r.i32()
        start = r.cindex()
        end = r.cindex()
        r.i32()  # CollisionRadius
        r.i32()  # CollisionHeight
        reach_flags = r.i32()
        pruned = r.u8()
        if pruned not in (0, 1):
            raise ValueError(f"{pkg.path}: invalid ReachSpec bPruned value {pruned}")
        reachspecs.append({
            "start": endpoint_name(start),
            "end": endpoint_name(end),
            "distance": distance,
            "reachFlags": reach_flags,
            "bPruned": pruned,
        })
    tail_start = r.o

    r._require(4)
    approx_time = struct.unpack_from("<f", r.d, r.o)[0]
    r.o += 4
    if not math.isfinite(approx_time):
        raise ValueError(f"{pkg.path}: Level ApproxTime is not finite")
    _level_object_ref(pkg, r, "Level FirstDeleted")
    for index in range(16):
        _level_object_ref(pkg, r, f"Level TextBlocks[{index}]")

    if pkg.version > 62:
        travel_info_layout = "map"
        _skip_level_travel_map(r)
    elif pkg.version >= 61:
        travel_info_layout = "parallel-arrays"
        name_count = _skip_level_string_array(r, "TravelInfo Names")
        item_count = _skip_level_string_array(r, "TravelInfo Items")
        if name_count != item_count:
            raise ValueError(
                f"{pkg.path}: TravelInfo Names/Items counts differ "
                f"({name_count}/{item_count})")
    else:
        raise ValueError(f"{pkg.path}: unsupported Level version {pkg.version}")

    destroy_observer_layout = "none"
    if pkg.version == 64:
        destroy_observer_layout = "v1"
        marker = r.u32()
        if marker:
            _skip_destroy_observer_map(pkg, r)
    elif pkg.version > 67:
        destroy_observer_layout = "v2"
        _skip_destroy_observer_map(pkg, r)

    trailing_bytes = r.end - r.o
    if trailing_bytes != 0:
        raise ValueError(
            f"{pkg.path}: Level export has {trailing_bytes} trailing bytes")
    return reachspecs, {
        "version": pkg.version,
        "tail_start": tail_start,
        "tail_end": r.o,
        "export_end": r.end,
        "trailing_bytes": trailing_bytes,
        "travel_info_layout": travel_info_layout,
        "destroy_observer_layout": destroy_observer_layout,
    }


def decode_reachspecs(pkg):
    """Decode ReachSpecs and validate the complete containing retail ULevel."""
    reachspecs, _ = _decode_level_reachspecs(pkg)
    return reachspecs


def _model_polys_ref(pkg, export):
    """Read UModel through its Polys object reference (package v62+ layout)."""
    if pkg.objname(export["cls"]) != "Model":
        raise ValueError(f"{pkg.name}.{export['name']} is not a Model")
    r = PayloadReader(pkg, export)
    _property_tags(r)
    _skip(r, 25 + 16, "model primitive bounds")
    _array(r, r.fvec, label="model vectors")
    _array(r, r.fvec, label="model points")

    def node():
        _skip(r, 16 + 8, "BSP node plane and zone mask")
        r.u8()
        for _ in range(7):
            r.cindex()
        _skip(r, 2 + 1 + 8, "BSP node zones, vertex count, and leaves")

    _array(r, node, label="model nodes", maximum=10_000_000)

    def surf():
        r.cindex()
        r.u32()
        for _ in range(6):
            r.cindex()
        _skip(r, 4, "BSP surface pan")
        r.cindex()

    _array(r, surf, label="model surfaces", maximum=10_000_000)

    def vert():
        r.cindex()
        r.cindex()

    _array(r, vert, label="model vertices", maximum=30_000_000)
    r.i32()  # NumSharedSides
    zone_count = r.i32()
    if not 0 <= zone_count <= 64:
        raise ValueError(
            f"{pkg.name}.{export['name']}: invalid model zone count {zone_count}")
    for _ in range(zone_count):
        r.cindex()
        _skip(r, 16, "model zone connectivity and visibility")
    polys_ref = r.cindex()
    if polys_ref <= 0 or polys_ref > len(pkg.exports):
        raise ValueError(
            f"{pkg.name}.{export['name']}: invalid Polys ref {polys_ref}")
    if pkg.objname(pkg.exports[polys_ref - 1]["cls"]) != "Polys":
        raise ValueError(
            f"{pkg.name}.{export['name']}: ref {polys_ref} is not Polys")
    return polys_ref


def decode_polys(pkg, export):
    """Decode the editor FPoly list used by brush-carrying actors."""
    if pkg.objname(export["cls"]) != "Polys":
        raise ValueError(f"{pkg.name}.{export['name']} is not Polys")
    r = PayloadReader(pkg, export)
    properties = _property_tags(r)
    if properties:
        raise ValueError(
            f"{pkg.name}.{export['name']}: unexpected tagged Polys properties")
    count = r.i32()
    maximum = r.i32()
    if count < 0 or maximum < count or count > 10_000_000:
        raise ValueError(
            f"{pkg.name}.{export['name']}: invalid polygon database "
            f"{count}/{maximum}")
    polygons = []
    for index in range(count):
        vertex_count = r.cindex()
        if not 0 <= vertex_count <= 16:
            raise ValueError(
                f"{pkg.name}.{export['name']} polygon {index}: invalid vertex "
                f"count {vertex_count}")
        base, normal, texture_u, texture_v = (r.fvec() for _ in range(4))
        vertices = [r.fvec() for _ in range(vertex_count)]
        poly_flags = r.u32()
        actor_ref = r.cindex()
        texture_ref = r.cindex()
        item_name = r.name()
        link = r.cindex()
        brush_poly = r.cindex()
        pan_u = struct.unpack_from("<h", r.d, r.o)[0]
        pan_v = struct.unpack_from("<h", r.d, r.o + 2)[0]
        r.o += 4
        polygons.append(dict(
            base=base, normal=normal, texture_u=texture_u,
            texture_v=texture_v, vertices=vertices, flags=poly_flags,
            actor=actor_ref, texture=texture_ref, item=item_name,
            link=link, brush_poly=brush_poly, pan_u=pan_u, pan_v=pan_v))
    if r.o != r.end:
        raise ValueError(
            f"{pkg.name}.{export['name']}: {r.end - r.o} trailing Polys bytes")
    return polygons


def decode_brush_model(pkg, export):
    """Decode the UPolys owned by one brush UModel export."""
    polys_ref = _model_polys_ref(pkg, export)
    return decode_polys(pkg, pkg.exports[polys_ref - 1])


def _t3d_vector(value):
    return ",".join(
        _format_float(component, signed=True, width=13) for component in value)


class _RetailObjectIndex:
    """Resolve imported objects the way a complete unpacked retail tree does."""
    def __init__(self, retail_root):
        retail_root = Path(retail_root)
        self.paths = {}
        for folder in ("System", "Textures", "Sounds", "Maps"):
            for path in (retail_root / folder).glob("*"):
                if path.is_file() and path.suffix.casefold() in {
                        ".u", ".utx", ".uax", ".wot"}:
                    self.paths.setdefault(path.stem.casefold(), path)
        self.packages = {}
        self.export_paths = {}

    def contains(self, source, ref):
        if ref >= 0:
            return bool(ref)
        path = source.objpath(ref)
        if not path or "." not in path:
            return False
        package_name, object_path = path.split(".", 1)
        key = package_name.casefold()
        package_path = self.paths.get(key)
        if package_path is None:
            return False
        if key not in self.packages:
            self.packages[key] = Package(package_path)
        if key not in self.export_paths:
            package = self.packages[key]
            self.export_paths[key] = {
                package.objpath(index).casefold()
                for index in range(1, len(package.exports) + 1)
            }
        return object_path.casefold() in self.export_paths[key]


def _class_is_named(resolver, class_ref, name):
    wanted = name.casefold()
    current = class_ref
    while current is not None:
        pkg, index = current
        if pkg.exports[index - 1]["name"].casefold() == wanted:
            return True
        current = resolver.superclass(current)
    return False


def _render_brush_t3d(pkg, model_ref, indent=4, object_index=None,
                      relink=False):
    if model_ref <= 0 or model_ref > len(pkg.exports):
        return []
    model = pkg.exports[model_ref - 1]
    if pkg.objname(model["cls"]) != "Model":
        return []
    polygons = decode_brush_model(pkg, model)
    base = " " * indent
    poly_base = " " * (indent + 3)
    polygon_base = " " * (indent + 6)
    coordinate_base = " " * (indent + 9)
    lines = [f"{base}Begin Brush Name={model['name']}",
             f"{poly_base}Begin PolyList"]
    for polygon_index, polygon in enumerate(polygons):
        header = f"{polygon_base}Begin Polygon"
        if polygon["item"] != "None":
            header += f" Item={polygon['item']}"
        if (polygon["texture"] and
                (object_index is None or
                 object_index.contains(pkg, polygon["texture"]))):
            header += f" Texture={pkg.objname(polygon['texture'])}"
        if polygon["flags"]:
            signed_flags = (polygon["flags"] if polygon["flags"] < 0x80000000
                            else polygon["flags"] - 0x100000000)
            header += f" Flags={signed_flags}"
        link = polygon_index if relink else polygon["link"]
        if link != -1:
            header += f" Link={link}"
        lines.append(header)
        lines.append(f"{coordinate_base}Origin   {_t3d_vector(polygon['base'])}")
        lines.append(f"{coordinate_base}Normal   {_t3d_vector(polygon['normal'])}")
        if polygon["pan_u"] or polygon["pan_v"]:
            lines.append(
                f"{coordinate_base}Pan      U={polygon['pan_u']} V={polygon['pan_v']}")
        lines.append(
            f"{coordinate_base}TextureU {_t3d_vector(polygon['texture_u'])}")
        lines.append(
            f"{coordinate_base}TextureV {_t3d_vector(polygon['texture_v'])}")
        lines.extend(
            f"{coordinate_base}Vertex   {_t3d_vector(vertex)}"
            for vertex in polygon["vertices"])
        lines.append(f"{polygon_base}End Polygon")
    lines.extend((f"{poly_base}End PolyList", f"{base}End Brush"))
    return lines


def _decode_instance_tag(resolver, pkg, export, class_ref, tag, prop):
    if tag["type"] == 3:
        return bool(tag["info"] & 0x80)
    value_reader = PayloadReader.__new__(PayloadReader)
    Reader.__init__(value_reader, tag["value"])
    value_reader.pkg = pkg
    value_reader.export = export
    value = resolver.read_value(prop, value_reader)
    if value_reader.o != value_reader.end:
        raise ValueError(
            f"{pkg.name}.{export['name']}.{tag['name']} leaves "
            f"{value_reader.end - value_reader.o} value bytes")
    return value


def level_to_t3d(package_path, system_dir=None):
    """Return a UCC-compatible Level T3D export as Latin-1 text."""
    pkg = Package(package_path)
    if system_dir is None:
        system_dir = pkg.path.parent.parent / "System"
    system_dir = Path(system_dir)
    core = system_dir / "Core.u"
    if not core.is_file():
        raise ValueError(
            f"Level export needs the retail System packages beside the map; "
            f"missing {core}")
    resolver = _ScriptResolver(core, legacy_float_rounding=True)
    object_index = _RetailObjectIndex(system_dir.parent)
    class_refs = {}
    emitted_models = set()
    lines = ["Begin Map"]
    for actor_ref in _level_actor_refs(pkg):
        if not actor_ref:
            continue
        export = pkg.exports[actor_ref - 1]
        class_name = pkg.objname(export["cls"])
        if class_name == "Camera":
            continue
        if not class_name:
            raise ValueError(
                f"{pkg.name}.{export['name']}: actor has no serialized class")
        if export["cls"] not in class_refs:
            class_refs[export["cls"]] = resolver.resolve(pkg, export["cls"])
        class_ref = class_refs[export["cls"]]
        if class_ref is None:
            raise ValueError(
                f"{pkg.name}.{export['name']}: cannot resolve class {class_name} "
                f"under {system_dir}")
        lines.append(f"Begin Actor Class={class_name} Name={export['name']}")
        r = PayloadReader(pkg, export)
        tags = _property_tags(r)
        is_mover = _class_is_named(resolver, class_ref, "Mover")
        saved_tags = {tag["name"].casefold() for tag in tags}
        synthesized = set()

        def synthesize_mover_property(name):
            key = name.casefold()
            if not is_mover or key in saved_tags or key in synthesized:
                return
            if key == "savedpos":
                lines.append(
                    "    SavedPos=(X=-12345.000000,Y=-12345.000000,"
                    "Z=-12345.000000)")
            else:
                lines.append("    SavedRot=(Pitch=123,Yaw=456,Roll=789)")
            synthesized.add(key)

        for tag in tags:
            key = tag["name"].casefold()
            # AMover::PostLoad assigns these refresh sentinels even when the
            # saved actor omitted them.  Their reflected positions are between
            # BasePos/BaseRot and the inherited Brush scale fields.
            if key in {"baserot", "mainscale"}:
                synthesize_mover_property("SavedPos")
            if key == "mainscale":
                synthesize_mover_property("SavedRot")
            prop = resolver.find_property(class_ref, tag["name"])
            if prop is None:
                raise ValueError(
                    f"{pkg.name}.{export['name']}: cannot resolve property "
                    f"{tag['name']} on {class_name}")
            value = _decode_instance_tag(
                resolver, pkg, export, class_ref, tag, prop)
            default = resolver.default_value(class_ref, prop, tag["array_index"])
            if (tag["name"].casefold() == "brush" and value is not None):
                brush_reader = Reader(tag["value"])
                model_ref = brush_reader.cindex()
                if model_ref not in emitted_models:
                    lines.extend(_render_brush_t3d(
                        pkg, model_ref, object_index=object_index,
                        relink=is_mover))
                    emitted_models.add(model_ref)
            text = resolver.format_value(prop, value, default)
            property_name = (prop["name"] if prop["array_dim"] == 1 else
                             f"{prop['name']}({tag['array_index']})")
            lines.append(f"    {property_name}={text}")
        synthesize_mover_property("SavedPos")
        synthesize_mover_property("SavedRot")
        # UObject.Name is native and therefore lives in the package export
        # table rather than the tagged-property stream.  It is nevertheless a
        # ported/editconst property and UCC emits it last for every actor.
        lines.append(f"    Name={export['name']}")
        lines.append("End Actor")
    lines.append("End Map")
    return "\r\n".join(lines) + "\r\n"


def export_level(package_path, out_path, system_dir=None):
    """Write one retail .wot Level export as T3D."""
    target = Path(out_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    data = level_to_t3d(package_path, system_dir)
    target.write_bytes(data.encode("latin-1"))
    return target


def export_reachspecs(package_path, out_path):
    """Write one retail .wot Level's ReachSpec array as JSON."""
    target = Path(out_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    payload = {"reachspecs": decode_reachspecs(Package(package_path))}
    target.write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8")
    return target


def _array(r, item, *, lazy=False, label="array", maximum=100_000_000):
    """Read a UE1 TArray, or a seek-prefixed TLazyArray for package v62+."""
    array_end = r.i32() if lazy and r.pkg.version > 61 else None
    count = r.cindex()
    if count < 0 or count > maximum:
        raise ValueError(f"invalid {label} count {count}")
    values = [item() for _ in range(count)]
    if array_end is not None and r.o != array_end:
        raise ValueError(
            f"{label} ended at package offset {r.o}, expected {array_end}")
    return values


def _f32(r):
    r._require(4)
    value = struct.unpack_from("<f", r.d, r.o)[0]
    r.o += 4
    return value


def _skip(r, size, label):
    r._require(size)
    r.o += size


def _mesh_triangle(r):
    indices = (r.u16(), r.u16(), r.u16())
    uvs = tuple((r.u8(), r.u8()) for _ in range(3))
    return dict(indices=indices, uvs=uvs, poly_flags=r.u32(),
                texture_index=r.i32())


def _mesh_sequence(r):
    name = r.name()
    group = r.name()
    start_frame = r.i32()
    num_frames = r.i32()
    notifications = _array(
        r, lambda: dict(time=_f32(r), function=r.name()),
        label=f"animation notifications for {name}", maximum=65536)
    rate = _f32(r)
    return dict(name=name, group=group, start=start_frame, num=num_frames,
                rate=rate, notifications=notifications)


def _poly_flags_to_james_type(poly_flags):
    supported = (PF_INVISIBLE | PF_MASKED | PF_TRANSLUCENT | PF_ENVIRONMENT |
                 PF_MODULATED | PF_TWOSIDED | PF_NOSMOOTH | PF_FLAT | PF_UNLIT)
    unknown = poly_flags & ~supported
    if unknown:
        raise ValueError(f"unsupported mesh polygon flags 0x{poly_flags:08x}")
    if poly_flags & PF_INVISIBLE:
        triangle_type = 8
    elif poly_flags & PF_MODULATED:
        triangle_type = 4
    elif poly_flags & PF_TRANSLUCENT:
        triangle_type = 2
    elif poly_flags & PF_MASKED:
        triangle_type = 3
    elif poly_flags & PF_TWOSIDED:
        triangle_type = 1
    else:
        triangle_type = 0
    if poly_flags & PF_UNLIT:
        triangle_type |= 16
    if poly_flags & PF_FLAT:
        triangle_type |= 32
    if poly_flags & PF_ENVIRONMENT:
        triangle_type |= 64
    if poly_flags & PF_NOSMOOTH:
        triangle_type |= 128
    return triangle_type


def decode_mesh(pkg, export, *, max_triangles=1520):
    """Decode a serialized UE1 ``Mesh`` or ``LodMesh`` export.

    The returned packed vertices are in runtime frame order.  For LodMesh,
    full-detail faces are expanded through the wedge/material tables; their
    visible vertex indices are offset past any leading attachment vertices.
    Import-time-only color and UV fields on invisible attachment triangles do
    not survive in a LodMesh and are explicitly marked as unavailable.

    ``max_triangles`` is checked before Python materializes any topology list.
    Its default is the cache-v3 worst-case canonical header/payload/working-set
    preflight; the largest shipped retail mesh has 1,503 triangles.
    """
    mesh_class = pkg.objname(export["cls"])
    if mesh_class not in ("Mesh", "LodMesh"):
        raise ValueError(
            f"{pkg.name}.{export['name']} is not a Mesh/LodMesh export")
    context = f"{pkg.name}.{export['name']} ({mesh_class})"
    try:
        r = PayloadReader(pkg, export)
        properties = r.properties()

        # UPrimitive's aggregate bound: FBox (two vectors + validity byte),
        # then the v62+ four-float FSphere.
        _skip(r, 25 + 16, "primitive bounds")
        saved_vertices = _array(
            r, r.u32, lazy=True, label="packed vertices")
        serialized_triangles = _array(
            r, lambda: _mesh_triangle(r), lazy=True, label="mesh triangles",
            maximum=max_triangles)
        sequences = _array(
            r, lambda: _mesh_sequence(r), label="animation sequences",
            maximum=65536)
        connects = _array(
            r, lambda: (r.i32(), r.i32()), lazy=True,
            label="vertex connections", maximum=10_000_000)

        # UMesh redundantly saves its aggregate bounds after Connects.
        _skip(r, 25 + 16, "mesh bounds")
        vert_links = _array(
            r, r.i32, lazy=True, label="vertex triangle links",
            maximum=30_000_000)
        textures = _array(
            r, r.cindex, label="mesh textures", maximum=65536)
        _array(r, lambda: _skip(r, 25, "frame bounding box"),
               label="frame bounding boxes", maximum=1_000_000)
        _array(r, lambda: _skip(r, 16, "frame bounding sphere"),
               label="frame bounding spheres", maximum=1_000_000)

        frame_verts = r.i32()
        anim_frames = r.i32()
        and_flags = r.u32()
        or_flags = r.u32()
        scale = tuple(_f32(r) for _ in range(3))
        origin = tuple(_f32(r) for _ in range(3))
        rotation_origin = (r.i32(), r.i32(), r.i32())
        current_poly = r.i32()
        current_vertex = r.i32()
        if pkg.version == 65:
            _f32(r)
        texture_lod = (_array(r, lambda: _f32(r), label="texture LOD",
                              maximum=65536)
                       if pkg.version >= 66 else [])

        lod = None
        triangles = serialized_triangles
        if mesh_class == "LodMesh":
            collapse_points = _array(
                r, r.u16, label="LOD collapse points", maximum=1_000_000)
            face_levels = _array(
                r, r.u16, label="LOD face levels", maximum=max_triangles)
            faces = _array(
                r, lambda: (r.u16(), r.u16(), r.u16(), r.u16()),
                label="LOD faces", maximum=max_triangles)
            collapse_wedges = _array(
                r, r.u16, label="LOD collapse wedges",
                maximum=max_triangles * 3)
            wedges = _array(
                r, lambda: (r.u16(), r.u8(), r.u8()),
                label="LOD wedges", maximum=max_triangles * 3)
            materials = _array(
                r, lambda: (r.u32(), r.i32()), label="LOD materials",
                maximum=65536)
            special_faces = _array(
                r, lambda: (r.u16(), r.u16(), r.u16(), r.u16()),
                label="LOD special faces", maximum=max_triangles)
            if len(faces) + len(special_faces) > max_triangles:
                raise ValueError(
                    "combined LOD face count exceeds the topology preflight")
            model_verts = r.i32()
            special_verts = r.i32()
            mesh_scale_max = _f32(r)
            lod_hysteresis = _f32(r)
            lod_strength = _f32(r)
            lod_min_verts = r.i32()
            lod_morph = _f32(r)
            lod_z_displace = _f32(r)
            remap_anim_verts = _array(
                r, r.u16, label="LOD animation remap", maximum=1_000_000)
            old_frame_verts = r.i32()

            triangles = []
            for face_index, face in enumerate(faces):
                material_index = face[3]
                if material_index >= len(materials):
                    raise ValueError(
                        f"LOD face {face_index} material {material_index} is out of range")
                face_wedges = []
                for wedge_index in face[:3]:
                    if wedge_index >= len(wedges):
                        raise ValueError(
                            f"LOD face {face_index} wedge {wedge_index} is out of range")
                    face_wedges.append(wedges[wedge_index])
                poly_flags, texture_index = materials[material_index]
                triangles.append(dict(
                    indices=tuple(wedge[0] + special_verts
                                  for wedge in face_wedges),
                    uvs=tuple((wedge[1], wedge[2]) for wedge in face_wedges),
                    poly_flags=poly_flags, texture_index=texture_index,
                    lod_special=False))
            for face in special_faces:
                triangles.append(dict(
                    indices=tuple(face[:3]), uvs=((0, 0), (0, 0), (0, 0)),
                    poly_flags=PF_INVISIBLE | PF_TWOSIDED, texture_index=0,
                    lod_special=True, discarded_import_fields=True))
            lod = dict(
                collapse_points=collapse_points, face_levels=face_levels,
                faces=faces, collapse_wedges=collapse_wedges, wedges=wedges,
                materials=materials, special_faces=special_faces,
                model_verts=model_verts, special_verts=special_verts,
                mesh_scale_max=mesh_scale_max, hysteresis=lod_hysteresis,
                strength=lod_strength, min_verts=lod_min_verts,
                morph=lod_morph, z_displace=lod_z_displace,
                remap_anim_verts=remap_anim_verts,
                old_frame_verts=old_frame_verts)

        if frame_verts < 0 or anim_frames < 0:
            raise ValueError(
                f"negative mesh dimensions {frame_verts} verts x {anim_frames} frames")
        if lod and lod["remap_anim_verts"]:
            remap = lod["remap_anim_verts"]
            old_frame_verts = lod["old_frame_verts"]
            if len(remap) != frame_verts:
                raise ValueError(
                    f"LOD animation remap has {len(remap)} entries, expected {frame_verts}")
            if len(saved_vertices) != old_frame_verts * anim_frames:
                raise ValueError(
                    "saved LOD animation size does not match OldFrameVerts")
            if any(index >= old_frame_verts for index in remap):
                raise ValueError("LOD animation remap index is out of range")
            vertices = [
                saved_vertices[frame * old_frame_verts + remap[index]]
                for frame in range(anim_frames)
                for index in range(frame_verts)
            ]
        else:
            vertices = saved_vertices
        if len(vertices) != frame_verts * anim_frames:
            raise ValueError(
                f"packed vertex count {len(vertices)} != "
                f"FrameVerts*AnimFrames ({frame_verts * anim_frames})")
        for triangle_index, triangle in enumerate(triangles):
            if any(index >= frame_verts for index in triangle["indices"]):
                raise ValueError(
                    f"triangle {triangle_index} has vertex outside 0..{frame_verts - 1}")
        if r.o != r.end:
            raise ValueError(f"unexpected mesh payload tail ({r.end - r.o} bytes)")

        return dict(
            name=export["name"], class_name=mesh_class,
            package=pkg.name, properties=properties,
            packed_vertices=vertices, frame_verts=frame_verts,
            anim_frames=anim_frames, triangles=triangles,
            sequences=sequences, connects=connects, vert_links=vert_links,
            textures=textures, texture_lod=texture_lod,
            and_flags=and_flags, or_flags=or_flags, scale=scale,
            origin=origin, rotation_origin=rotation_origin,
            current_poly=current_poly, current_vertex=current_vertex, lod=lod)
    except (IndexError, KeyError, struct.error, ValueError) as exc:
        if str(exc).startswith(context + ":"):
            raise
        raise ValueError(f"{context}: {exc}") from exc


def _negate_packed_x(value):
    x = value & 0x7FF
    if x >= 0x400:
        x -= 0x800
    return (value & ~0x7FF) | ((-x) & 0x7FF)


def encode_mesh_as_james(mesh, *, unmirror=False):
    """Return canonical ``(_d.3d bytes, _a.3d bytes)`` for a decoded mesh."""
    frame_verts = mesh["frame_verts"]
    anim_frames = mesh["anim_frames"]
    triangles = mesh["triangles"]
    if not 0 <= len(triangles) <= 0xFFFF:
        raise ValueError(f"{mesh['name']}: too many James polygons")
    if not 0 <= frame_verts <= 0xFFFF or not 0 <= anim_frames <= 0xFFFF:
        raise ValueError(f"{mesh['name']}: dimensions exceed James headers")
    frame_size = frame_verts * 4
    if frame_size > 0xFFFF:
        raise ValueError(f"{mesh['name']}: James frame is larger than 65535 bytes")

    # The importer consumes a 36-byte header followed by twelve reserved bytes.
    data = bytearray(struct.pack("<2H", len(triangles), frame_verts) + bytes(44))
    for triangle in triangles:
        indices = list(triangle["indices"])
        uvs = list(triangle["uvs"])
        if unmirror:
            indices[1], indices[2] = indices[2], indices[1]
            uvs[1], uvs[2] = uvs[2], uvs[1]
        if any(not 0 <= index <= 0xFFFF for index in indices):
            raise ValueError(f"{mesh['name']}: triangle vertex is out of range")
        if any(not 0 <= component <= 0xFF for uv in uvs for component in uv):
            raise ValueError(f"{mesh['name']}: triangle UV is out of range")
        texture_index = triangle["texture_index"]
        if not 0 <= texture_index <= 0xFF:
            raise ValueError(
                f"{mesh['name']}: texture index {texture_index} exceeds James byte")
        triangle_type = _poly_flags_to_james_type(triangle["poly_flags"])
        data += struct.pack(
            "<3HBB6B2B", *indices, triangle_type, 0,
            *(component for uv in uvs for component in uv), texture_index, 0)

    vertices = mesh["packed_vertices"]
    if unmirror:
        vertices = [_negate_packed_x(value) for value in vertices]
    animation = bytearray(struct.pack("<2H", anim_frames, frame_size))
    if vertices:
        animation += struct.pack(f"<{len(vertices)}I", *vertices)
    return bytes(data), bytes(animation)


def export_meshes(package_paths, out_dir, names=None, metadata=None):
    """Decode Mesh/LodMesh exports and write flat James file pairs.

    Duplicate package exports are accepted only when their reconstructed bytes
    agree.  Returns a manifest recording the retail package chosen per mesh.
    """
    requested = {name.casefold(): name for name in names} if names else None
    output = Path(out_dir)
    output.mkdir(parents=True, exist_ok=True)
    emitted = {}
    packages_by_mesh = {}
    for package_path in map(Path, package_paths):
        pkg = Package(package_path)
        for export in pkg.exports:
            if pkg.objname(export["cls"]) not in ("Mesh", "LodMesh"):
                continue
            key = export["name"].casefold()
            if requested is not None and key not in requested:
                continue
            if not re.fullmatch(r"[A-Za-z0-9_]+", export["name"]):
                raise ValueError(
                    f"refusing suspicious mesh name {export['name']!r} in {package_path}")
            mesh = decode_mesh(pkg, export)
            mesh_metadata = (metadata or {}).get(key, {})
            mesh["output_name"] = mesh_metadata.get("name", mesh["name"])
            unmirror = str(mesh_metadata.get("unmirror", "0")) != "0"
            pair = encode_mesh_as_james(mesh, unmirror=unmirror)
            if key in emitted and emitted[key][1] != pair:
                prior_package = emitted[key][0]
                raise ValueError(
                    f"conflicting retail mesh {export['name']} in "
                    f"{prior_package} and {package_path}")
            emitted[key] = (str(package_path), pair, mesh)
            packages_by_mesh.setdefault(key, []).append(pkg.name)

    missing = sorted(original for key, original in (requested or {}).items()
                     if key not in emitted)
    if missing:
        raise ValueError(f"Mesh exports not found: {missing}")
    entries = {}
    outputs = []
    for key, (package_path, pair, mesh) in sorted(emitted.items()):
        output_name = mesh["output_name"]
        data_path = output / f"{output_name}_d.3d"
        anim_path = output / f"{output_name}_a.3d"
        data_path.write_bytes(pair[0])
        anim_path.write_bytes(pair[1])
        outputs.extend((data_path, anim_path))
        entries[output_name] = dict(
            package=Path(package_path).stem,
            duplicate_packages=packages_by_mesh[key],
            class_name=mesh["class_name"], frame_verts=mesh["frame_verts"],
            anim_frames=mesh["anim_frames"], polygons=len(mesh["triangles"]),
            data_file=data_path.name, anim_file=anim_path.name)
    manifest = dict(source="retail-package", meshes=entries)
    manifest_path = output / "_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=1))
    manifest["outputs"] = outputs + [manifest_path]
    return manifest


def decode_font(pkg, export):
    """UE1 UFont -> pages and the direct character-index metric table.

    WoT's v68 UFont serialization is a TArray<FFontPage>, followed by
    CharactersPerPage.  Each page is an object reference to a Texture and a
    TArray of four-int32 FFontCharacter records (StartU, StartV, USize, VSize).
    The referenced P8 texture and palette are decoded here as well so callers
    do not need to rediscover the object graph.
    """
    if pkg.objname(export["cls"]) != "Font":
        raise ValueError(f"{export['name']} is not a Font export")

    r = PayloadReader(pkg, export)
    r.properties()
    page_count = r.cindex()
    if page_count < 0 or page_count > 256:
        raise ValueError(f"invalid font page count {page_count}")

    pages = []
    for page_index in range(page_count):
        texture_ref = r.cindex()
        if texture_ref <= 0 or texture_ref > len(pkg.exports):
            raise ValueError(f"font page {page_index} has invalid texture ref {texture_ref}")
        texture_export = pkg.exports[texture_ref - 1]
        if pkg.objname(texture_export["cls"]) != "Texture":
            raise ValueError(
                f"font page {page_index} references non-Texture {texture_export['name']}")

        character_count = r.cindex()
        if character_count < 0 or character_count > 65536:
            raise ValueError(f"invalid font character count {character_count}")
        if r.o + character_count * 16 > r.end:
            raise ValueError("font character array extends beyond export payload")

        characters = []
        for local_index in range(character_count):
            x, y, w, h = struct.unpack_from("<4i", r.d, r.o)
            r.o += 16
            if min(x, y, w, h) < 0:
                raise ValueError(
                    f"font page {page_index} character {local_index} has negative metrics")
            characters.append(dict(local_index=local_index, x=x, y=y, w=w, h=h))

        texture = decode_texture(pkg, texture_export)
        if texture["format"] != 0:
            raise ValueError(
                f"font page {page_index} texture is not P8 (format={texture['format']})")
        palette_ref = texture["palette"]
        if palette_ref <= 0 or palette_ref > len(pkg.exports):
            raise ValueError(f"font page {page_index} has invalid palette ref {palette_ref}")
        palette_export = pkg.exports[palette_ref - 1]
        if pkg.objname(palette_export["cls"]) != "Palette":
            raise ValueError(f"font page {page_index} palette ref is not a Palette")
        palette = decode_palette(pkg, palette_export)

        for character in characters:
            if (character["x"] + character["w"] > texture["w"] or
                    character["y"] + character["h"] > texture["h"]):
                raise ValueError(
                    f"font page {page_index} character {character['local_index']} "
                    "extends beyond its texture")
        pages.append(dict(page=page_index, texture_ref=texture_ref,
                          texture_name=texture_export["name"], texture=texture,
                          palette=palette, characters=characters))

    if r.o + 4 != r.end:
        raise ValueError(f"unexpected font payload tail ({r.end - r.o} bytes)")
    characters_per_page = r.i32()
    if characters_per_page <= 0 or characters_per_page & (characters_per_page - 1):
        raise ValueError(f"invalid CharactersPerPage {characters_per_page}")

    characters = []
    for page in pages:
        for character in page["characters"]:
            item = dict(character)
            item["page"] = page["page"]
            item["index"] = page["page"] * characters_per_page + item.pop("local_index")
            characters.append(item)
    return dict(name=export["name"], characters_per_page=characters_per_page,
                pages=pages, characters=characters)


def export_fonts(package_path, out_dir, names=None):
    """Write decoded font atlas PNGs and ``_manifest.json``.

    Palette index zero is transparent for the masked font textures.  PIL is
    imported lazily so inventory users of this module keep its stdlib-only
    dependency footprint.
    """
    from PIL import Image

    pkg = Package(package_path)
    requested = set(names) if names else None
    output = Path(out_dir)
    manifest = {"package": pkg.name, "fonts": {}}
    found = set()
    for export in pkg.exports:
        if pkg.objname(export["cls"]) != "Font":
            continue
        font_name = export["name"]
        if requested is not None and font_name not in requested:
            continue
        font = decode_font(pkg, export)
        found.add(font_name)
        font_dir = output / font_name
        font_dir.mkdir(parents=True, exist_ok=True)
        page_entries = []
        for page in font["pages"]:
            texture = page["texture"]
            palette = page["palette"]
            rgba = bytearray(texture["w"] * texture["h"] * 4)
            for pixel_index, palette_index in enumerate(texture["data"]):
                red, green, blue, _ = palette[palette_index]
                alpha = 0 if texture["masked"] and palette_index == 0 else 255
                rgba[pixel_index * 4:pixel_index * 4 + 4] = bytes(
                    (red, green, blue, alpha))
            filename = f"page_{page['page']}.png"
            Image.frombytes("RGBA", (texture["w"], texture["h"]), bytes(rgba)).save(
                font_dir / filename)
            page_entries.append(dict(page=page["page"], png=f"{font_name}/{filename}",
                                     w=texture["w"], h=texture["h"],
                                     source_texture=page["texture_name"]))
        manifest["fonts"][font_name] = dict(
            characters_per_page=font["characters_per_page"], pages=page_entries,
            characters=font["characters"])

    missing = sorted((requested or set()) - found)
    if missing:
        raise ValueError(f"Font exports not found in {package_path}: {missing}")
    output.mkdir(parents=True, exist_ok=True)
    (output / "_manifest.json").write_text(json.dumps(manifest, indent=1))
    return manifest


def decode_model(pkg, export):
    """UModel → dict(points, vectors, nodes, surfs, verts)."""
    r = PayloadReader(pkg, export)
    r.properties()
    r.o += 25 + 16                    # FBox(25) + FSphere(16) — UPrimitive
    vectors = [r.fvec() for _ in range(r.cindex())]
    points = [r.fvec() for _ in range(r.cindex())]
    nodes = []
    for _ in range(r.cindex()):
        plane = struct.unpack_from("<4f", r.d, r.o)  # X,Y,Z,W
        r.o += 16 + 8                 # Plane, ZoneMask
        r.u8()                        # NodeFlags
        ivp, isurf = r.cindex(), r.cindex()
        for _ in range(5):            # iBack,iFront,iPlane,iCollB,iRendB
            r.cindex()
        r.o += 2                      # iZone[2]
        nv = r.u8()
        r.o += 8                      # iLeaf[2]
        nodes.append((ivp, isurf, nv, plane[:3]))
    surfs = []
    for _ in range(r.cindex()):
        tex = r.cindex()
        flags = r.u32()
        pbase, vnorm, vtu, vtv = (r.cindex() for _ in range(4))
        r.cindex(); r.cindex()        # iLightMap, iBrushPoly
        panu = struct.unpack_from("<h", r.d, r.o)[0]
        panv = struct.unpack_from("<h", r.d, r.o + 2)[0]
        r.o += 4
        r.cindex()                    # Actor
        surfs.append(dict(tex=tex, flags=flags, pbase=pbase, vnorm=vnorm,
                          vtu=vtu, vtv=vtv, panu=panu, panv=panv))
    verts = []
    for _ in range(r.cindex()):
        verts.append((r.cindex(), r.cindex()))
    return dict(vectors=vectors, points=points, nodes=nodes,
                surfs=surfs, verts=verts)


FRACTAL_CLASSES = {"FireTexture", "WaterTexture", "WaveTexture",
                   "WetTexture", "IceTexture"}


def decode_fractal(pkg, export):
    """Fractal texture object → props + palette ref + sim buffer + sparks.

    Layout: the texture payload (tagged props + mips), then, for FireTexture
    only, a TArray of 8-byte FSpark (the saved sparks)."""
    r = PayloadReader(pkg, export)
    props = r.properties()
    mips = []
    n_mips = r.cindex()
    for _ in range(n_mips):
        # fractal textures serialize mips WITHOUT the TLazyArray seek prefix
        # (empirically, WoT 333B); autodetect by validating sizes.
        save = r.o
        valid_unprefixed = False
        try:
            cnt = r.cindex()
            data = r.d[r.o:r.o + cnt]; r.o += cnt
            w, h = r.i32(), r.i32()
            ub, vb = r.u8(), r.u8()
            valid_unprefixed = (
                w == 1 << ub and h == 1 << vb and cnt in (0, w * h))
        except ValueError:
            # A seek-prefix can look like a large compact array count and run
            # beyond a short, empty parametric mip before validation (RedIce).
            pass
        if not valid_unprefixed:
            r.o = save
            r.i32()                   # seek-prefixed variant
            cnt = r.cindex()
            data = r.d[r.o:r.o + cnt]; r.o += cnt
            w, h = r.i32(), r.i32()
            r.o += 2
        # parametric textures save empty data; sim buffer = zeros
        if cnt == 0:
            data = bytes(w * h)
        mips.append((w, h, data))
    out = dict(name=export["name"], cls=pkg.objname(export["cls"]),
               props=props, mips=mips)
    cls = pkg.objname(export["cls"])
    if cls == "FireTexture" and r.o < r.end:
        sparks = []
        for _ in range(r.cindex()):
            sparks.append(tuple(r.d[r.o:r.o + 8])); r.o += 8
        out["sparks"] = sparks
    elif cls in ("WaterTexture", "WetTexture", "WaveTexture") and r.o < r.end:
        drops = []
        for _ in range(r.cindex()):
            drops.append(tuple(r.d[r.o:r.o + 8])); r.o += 8
        out["drops"] = drops
    return out


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Inspect or extract UE1 package payloads")
    subparsers = parser.add_subparsers(dest="command", required=True)
    fonts_parser = subparsers.add_parser("fonts", help="extract UFont atlases and metrics")
    fonts_parser.add_argument("package")
    fonts_parser.add_argument("out_dir")
    fonts_parser.add_argument("names", nargs="*")
    sounds_parser = subparsers.add_parser("sounds", help="extract USound WAV files")
    sounds_parser.add_argument("package")
    sounds_parser.add_argument("out_dir")
    sounds_parser.add_argument("names", nargs="*")
    script_parser = subparsers.add_parser(
        "script", help="extract ScriptText classes as UnrealScript source")
    script_parser.add_argument("package")
    script_parser.add_argument("out_dir")
    script_parser.add_argument("names", nargs="*")
    level_parser = subparsers.add_parser(
        "level", help="export a .wot Level as pure-Python T3D")
    level_parser.add_argument("package")
    level_parser.add_argument("out_file")
    level_parser.add_argument(
        "--system-dir", type=Path,
        help="retail System directory (default: sibling of the map's Maps directory)")
    reachspecs_parser = subparsers.add_parser(
        "reachspecs", help="export a .wot Level ReachSpec array as JSON")
    reachspecs_parser.add_argument("package")
    reachspecs_parser.add_argument("--out", required=True)
    meshes_parser = subparsers.add_parser(
        "meshes", help="extract UMesh/ULodMesh exports as James file pairs")
    meshes_parser.add_argument("packages", nargs="+")
    meshes_parser.add_argument("out_dir")
    meshes_parser.add_argument(
        "--script-root", type=Path,
        default=None,
        help="shipped #exec metadata root (needed to reverse UNMIRROR imports)")
    meshes_parser.add_argument("--name", dest="names", action="append")
    arguments = parser.parse_args()
    if arguments.command == "meshes" and arguments.script_root is None:
        arguments.script_root = exports_dir() / "script"
    if arguments.command == "fonts":
        exported = export_fonts(arguments.package, arguments.out_dir,
                                arguments.names or None)
        print(f"{arguments.package}: {len(exported['fonts'])} fonts")
    elif arguments.command == "sounds":
        exported = export_sounds(arguments.package, arguments.out_dir,
                                 arguments.names or None)
        print(f"{arguments.package}: {len(exported)} sound exports, "
              f"{len(set(exported))} WAV files")
    elif arguments.command == "script":
        exported = export_scripts(arguments.package, arguments.out_dir,
                                  arguments.names or None)
        print(f"{arguments.package}: {len(exported)} UnrealScript classes")
    elif arguments.command == "level":
        exported = export_level(arguments.package, arguments.out_file,
                                arguments.system_dir)
        print(f"{arguments.package}: Level T3D -> {exported}")
    elif arguments.command == "reachspecs":
        exported = export_reachspecs(arguments.package, arguments.out)
        print(f"{arguments.package}: ReachSpecs -> {exported}")
    elif arguments.command == "meshes":
        from ue3d import parse_exec_metadata
        metadata = parse_exec_metadata(arguments.script_root)
        exported = export_meshes(arguments.packages, arguments.out_dir,
                                 arguments.names or None, metadata)
        print(f"{len(arguments.packages)} packages: "
              f"{len(exported['meshes'])} James mesh pairs")
