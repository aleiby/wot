#!/usr/bin/env python3
"""Probe shipped Engine.u Pawn.TakeDamage for the momentum guard.

A later engine revision of Pawn.TakeDamage guards the knockback: after
momentum = momentum/Mass it calls AddVelocity(momentum) only when momentum
is not vect(0,0,0), where the older form calls AddVelocity unconditionally.

The guard matters because UE1's Pawn.AddVelocity knocks a
walking pawn into PHYS_Falling even for a zero vector, so an UNGUARDED retail
TakeDamage would bump walking pawns into falling physics on every
zero-momentum hit (DoT leeches, zone pain).  Retail Engine.u strips
ScriptText, so the answer only exists in the serialized bytecode.

Method (the ref-recording walker pattern from the G06 drop-physics probe):
mirror ue1pkg._skip_serialized_script but record an ordered event trace of
name refs, object refs, vector constants, and conditional jumps, then walk the
Pawn.TakeDamage Function export.  The guard's signature is a conditional jump
whose test contains a (0,0,0) vector constant BEFORE the AddVelocity call;
the later HitLocation == vect(0,0,0) compare is AFTER AddVelocity either way,
so ordering is decisive.

Result: the retail build includes the zero-momentum guard.

Run from repo root:  python3 Tools/test/probe_pawn_takedamage_momentum.py
"""
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "convert"))
sys.path.insert(0, str(TOOLS))
import ue1pkg  # noqa: E402
from ue1pkg import (Package, _read_struct_header,  # noqa: E402
                    _EXPR_NO_OPERANDS, _EXPR_OBJECT_REF, _EXPR_TWO_EXPRESSIONS)
from wot_config import original_dir  # noqa: E402

PKG = original_dir() / "System" / "Engine.u"

EVENTS = []


def _recording_skip(r, memory_size):
    """ue1pkg._skip_serialized_script with an event trace (structure identical)."""
    pkg = r.pkg
    memory_offset = 0

    def raw(size):
        nonlocal memory_offset
        r._require(size)
        r.o += size
        memory_offset += size

    def object_ref(why):
        nonlocal memory_offset
        at = memory_offset
        value = r.cindex()
        memory_offset += 4
        EVENTS.append((at, why, pkg.objname(value) if value else None))

    def name_ref(why):
        nonlocal memory_offset
        at = memory_offset
        value = r.cindex()
        memory_offset += 4
        EVENTS.append((at, why, pkg.names[value]))
        return value

    def parameters():
        while expression() != 0x16:
            pass

    def expression():
        nonlocal memory_offset
        at = memory_offset
        token = r.u8()
        memory_offset += 1
        if 0x39 <= token < 0x60:          # scalar conversion
            expression()
        elif token >= 0x70:               # one-byte native function id
            EVENTS.append((at, "native", token))
            parameters()
        elif token >= 0x60:               # extended native function id
            hi = token
            lo = r.d[r.o]
            raw(1)
            EVENTS.append((at, "native", ((hi - 0x60) << 8) + lo))
            parameters()
        elif token in _EXPR_TWO_EXPRESSIONS:
            expression()
            expression()
        elif token == 0x06:               # jump
            raw(2)
        elif token in _EXPR_OBJECT_REF:
            object_ref(f"objref{token:02x}")
        elif token in _EXPR_NO_OPERANDS:
            pass
        elif token in (0x04, 0x0D, 0x0E):
            expression()
        elif token == 0x1C:               # final function
            object_ref("finalfunc")
            parameters()
        elif token in (0x1B, 0x38):       # virtual/global function
            name_ref("callfunc")
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
            name_ref("namconst")
        elif token in (0x22, 0x23):       # rotator/vector constant
            vals = struct.unpack_from("<3f", r.d, r.o)
            EVENTS.append((at, "vecconst" if token == 0x23 else "rotconst",
                           vals))
            raw(12)
        elif token in (0x24, 0x2C):       # byte-sized number
            raw(1)
        elif token in (0x13, 0x2E):       # meta/dynamic cast
            object_ref("cast")
            expression()
        elif token == 0x07:               # conditional jump
            dest = struct.unpack_from("<H", r.d, r.o)[0]
            EVENTS.append((at, "jumpifnot", dest))
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
                label = name_ref("label")
                raw(4)
                if label == 0:
                    break
        elif token == 0x18:               # skip
            raw(2)
            expression()
        elif token in (0x32, 0x33):       # struct comparison
            object_ref("structcmp")
            expression()
            expression()
        elif token == 0x36:               # struct member
            object_ref("structmember")
            expression()
        else:
            raise ValueError(
                f"unknown UnrealScript expression token 0x{token:02x}")
        return token

    while memory_offset < memory_size:
        expression()
    if memory_offset != memory_size:
        raise ValueError(
            f"bytecode size mismatch: {memory_offset} != {memory_size}")


def outer_chain(pkg, export):
    chain = []
    group = export["group"]
    while group > 0:
        outer = pkg.exports[group - 1]
        chain.append(outer["name"])
        group = outer["group"]
    return chain


def main():
    pkg = Package(PKG)
    target = None
    for export in pkg.exports:
        if (export["name"] == "TakeDamage"
                and pkg.objname(export["cls"]) == "Function"
                and outer_chain(pkg, export)[:1] == ["Pawn"]):
            target = export
            break
    if target is None:
        raise SystemExit("Engine.u Pawn.TakeDamage function export not found")

    print(f"{PKG}  v{pkg.version}  Pawn.TakeDamage "
          f"off={target['off']} size={target['size']}")
    ue1pkg._skip_serialized_script = _recording_skip
    _read_struct_header(pkg, target)

    addvel = [at for at, why, val in EVENTS
              if why == "callfunc" and val == "AddVelocity"]
    zerovecs = [at for at, why, val in EVENTS
                if why == "vecconst" and val == (0.0, 0.0, 0.0)]
    print(f"AddVelocity call sites: {addvel}")
    print(f"vect(0,0,0) constants:  {zerovecs}")

    print("\nOrdered trace (calls / vector consts / conditional jumps):")
    for at, why, val in EVENTS:
        if why in ("callfunc", "finalfunc", "vecconst", "jumpifnot"):
            print(f"  @{at:4d} {why:9s} {val}")

    if not addvel:
        print("\nVERDICT: no AddVelocity call found (unexpected)")
        return
    guard = [at for at in zerovecs if at < addvel[0]]
    if guard:
        print("\nVERDICT: GUARDED — a vect(0,0,0) compare precedes AddVelocity"
              " (the zero-momentum guard is in retail 333B)")
    else:
        print("\nVERDICT: UNGUARDED — AddVelocity is unconditional in shipped"
              " Engine.u (the zero-momentum guard postdates retail)")


if __name__ == "__main__":
    main()
