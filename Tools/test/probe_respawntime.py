#!/usr/bin/env python3
"""Verify the shipped Engine.Inventory.RespawnTime default from retail CDO bytes.

Reads original/System/Engine.u (UE1 package v68) and looks for the
RespawnTime FloatProperty tag in a class's serialized default-properties block.
UE1's SerializeTaggedProperties omits any property whose value equals the
zero/inherited default, so an ABSENT tag means the default is 0.0.

Result (2026-07-05): Inventory has no tag -> default 0.0 (matches UE1).
Weapon carries RespawnTime=30.0 (matches UE1's Weapon default) -> validates the
method. Pickup inherits (no tag).

Run from repo root:  python3 Tools/test/probe_respawntime.py
"""
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "convert"))
sys.path.insert(0, str(TOOLS))
from ue1pkg import Package, PayloadReader, _TAG_SIZES  # noqa: E402
from wot_config import original_dir  # noqa: E402

PKG = original_dir() / "System" / "Engine.u"
TYPE_NAMES = {1: "Byte", 2: "Int", 3: "Bool", 4: "Float", 5: "Object",
              6: "Name", 10: "Struct"}


def enc_cindex(v):
    """UE1 compact index encoding of a non-negative int (inverse of
    Reader.cindex for values >= 0)."""
    out = bytearray()
    b = v & 0x3F
    v >>= 6
    if v:
        b |= 0x40
    out.append(b)
    while v:
        c = v & 0x7F
        v >>= 7
        if v:
            c |= 0x80
        out.append(c)
    return bytes(out)


def read_float_tag(pkg, pos):
    """pos is just past a property-name cindex. Decode the tag; return
    (type_name, size, arr_idx, float_value_or_None)."""
    r = PayloadReader.__new__(PayloadReader)
    r.d, r.pkg, r.o = pkg.d, pkg, pos
    info = r.u8()
    ptype = info & 0x0F
    if ptype == 10:
        r.name()  # struct sub-name
    szbits = info & 0x70
    if szbits in _TAG_SIZES:
        size = _TAG_SIZES[szbits]
    elif szbits == 0x50:
        size = r.u8()
    elif szbits == 0x60:
        size = r.u16()
    else:
        size = r.u32()
    aidx = 0
    if (info & 0x80) and ptype != 3:
        b = r.u8()
        if b & 0x80:
            if (b & 0xC0) == 0x80:
                aidx = ((b & 0x7F) << 8) + r.u8()
            else:
                aidx = int.from_bytes(r.d[r.o:r.o + 3], "big"); r.o += 3
        else:
            aidx = b
    val = struct.unpack_from("<f", r.d, r.o)[0] if ptype == 4 and size == 4 \
        else None
    return TYPE_NAMES.get(ptype, f"?{ptype}"), size, aidx, val


def main():
    p = Package(PKG)
    nidx = p.names.index("RespawnTime")
    needle = enc_cindex(nidx)
    print(f"{PKG}  v{p.version}  RespawnTime name idx={nidx} "
          f"cindex={needle.hex()}")
    for cls in ("Inventory", "Weapon", "Pickup"):
        e = next(x for x in p.exports
                 if x["name"] == cls and p.objname(x["cls"]) is None)
        lo, hi = e["off"], e["off"] + e["size"]
        pos, hits = lo, []
        while (i := p.d.find(needle, pos, hi)) >= 0:
            hits.append((i, read_float_tag(p, i + len(needle))))
            pos = i + 1
        if not hits:
            print(f"  {cls:<10} no RespawnTime tag -> default 0.0 (inherited)")
        for i, (tn, size, aidx, val) in hits:
            print(f"  {cls:<10} @{i}: {tn} size={size} arr={aidx} value={val}")


if __name__ == "__main__":
    main()
