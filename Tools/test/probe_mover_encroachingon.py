#!/usr/bin/env python3
"""Probe shipped Engine.u Mover.EncroachingOn for the Seal exception.

In a later engine revision of Mover.EncroachingOn, the newer clause destroys
encroaching Fragments and ownerless Inventory, except inventory of the Seal
class. The older form did not exempt Seals.

Retail Engine.u strips ScriptText, so whether the Seal exemption shipped only
exists in the serialized bytecode.  Signature: a 'Seal' name constant among the
IsA calls of EncroachingOn (the older form has only Carcass/Decoration/
Fragment/Inventory/Pawn name constants).  The full ordered trace also verifies
the dispatch structure the G26/G27 port relies on (TakeDamage 10000 for
carcass/decoration, EncroachDamage, MakeGroupStop/MakeGroupReturn via Leader,
KilledBy for crush).

Result (2026-07-18): Seal exemption PRESENT; MakeGroupStop + MakeGroupReturn +
PlayTakeHitSound + KilledBy all called in the later revision's order — shipped
333B matches its EncroachingOn structure 1:1.  Companion one-off probes of
Mover.MakeGroupReturn ('Open'/'Close' + follower recursion), MakeGroupStop
(state-freeze + follower chain), and PostBeginPlay also matched.  See
PORT_NOTES "Mover group commands in custom movers".

Run from repo root:  python3 Tools/test/probe_mover_encroachingon.py
"""
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "convert"))
sys.path.insert(0, str(TOOLS / "test"))
sys.path.insert(0, str(TOOLS))
import ue1pkg  # noqa: E402
from ue1pkg import Package, _read_struct_header  # noqa: E402
import probe_pawn_takedamage_momentum as walker  # noqa: E402
from wot_config import original_dir  # noqa: E402

PKG = original_dir() / "System" / "Engine.u"


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
        if (export["name"] == "EncroachingOn"
                and pkg.objname(export["cls"]) == "Function"
                and outer_chain(pkg, export)[:1] == ["Mover"]):
            target = export
            break
    if target is None:
        raise SystemExit("Engine.u Mover.EncroachingOn function export not found")

    print(f"{PKG}  v{pkg.version}  Mover.EncroachingOn "
          f"off={target['off']} size={target['size']}")
    ue1pkg._skip_serialized_script = walker._recording_skip
    walker.EVENTS.clear()
    _read_struct_header(pkg, target)

    events = walker.EVENTS
    names = [val for at, why, val in events if why == "namconst"]
    calls = [val for at, why, val in events if why in ("callfunc", "finalfunc")]
    print(f"\nname constants: {names}")
    print(f"calls:          {calls}")

    print("\nOrdered trace (calls / name consts / int consts / jumps):")
    for at, why, val in events:
        if why in ("callfunc", "finalfunc", "namconst", "jumpifnot", "vecconst"):
            print(f"  @{at:4d} {why:9s} {val}")

    verdicts = []
    verdicts.append(("Seal exemption",
                     "PRESENT (devtree #if-NEW IS retail 333B)"
                     if "Seal" in names else
                     "ABSENT (older unexempted form shipped)"))
    verdicts.append(("group dispatch",
                     "MakeGroupStop+MakeGroupReturn both called"
                     if "MakeGroupStop" in calls and "MakeGroupReturn" in calls
                     else "MISSING a group verb — STRUCTURE DIFFERS"))
    verdicts.append(("crush dispatch",
                     "KilledBy called" if "KilledBy" in calls
                     else "KilledBy MISSING — STRUCTURE DIFFERS"))
    print()
    for what, verdict in verdicts:
        print(f"VERDICT [{what}]: {verdict}")


if __name__ == "__main__":
    main()
