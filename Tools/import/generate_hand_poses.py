#!/usr/bin/env python3
"""Compile the first-person hand poses (JSON) into C++ rows; Python 3.9, stdlib only.

Each pose names per-bone rotations as [flex, spread, twist] degrees in the bone's local frame
(the hand skeleton contract: flex about +Y turns +X toward -Z, spread about +Z, twist about
+X), composed as Local = Ref * Rx(twist) * Rz(spread) * Ry(flex). They compile to
quaternions, so the game applies them as Ref.Rotation * Q. A grip pose also carries the
hand's camera-space placement (offset cm, rotator, scale multiplier) and where the held item
sits in grip_r's frame: the line it lies on (axis), the side turned toward the viewer (front),
its centre (point cm), a roll about the line (degrees), its default half-size (cm) and its lead:
how far its centre moves along the line per cm of half-size, so a larger item grows away from
the hand (a third for a rod held at its lower third, one for an orb resting in the palm).

Each held artifact class names its grip and how it sits in it: the local axis that leads along
the grip line (a rod's tip, a figure's head) and the one turned toward the viewer, each a
signed axis name (+X ... -Z) or a vector, its half-size in the hand (cm), a slide along the line
(cm, positive holds it lower) and an offset (item-local cm) from its bounds centre to the point
the grip places, for items whose handle sits off centre.
"""
import argparse
import json
import math
import pathlib
import re
import sys

PLUGIN = pathlib.Path(__file__).resolve().parents[2] / "Plugins" / "WoTFirstPerson"
# The hand skeleton contract's 28 bones (the thumb has no intermediate joint).
BONES = frozenset(["forearm_r", "wrist_r", "palm_r", "grip_r"]
                  + ["thumb_%s_r" % j for j in ("metacarpal", "proximal", "distal", "tip")]
                  + ["%s_%s_r" % (f, j) for f in ("index", "middle", "ring", "little")
                     for j in ("metacarpal", "proximal", "intermediate", "distal", "tip")])
NAME = re.compile(r"[a-z][a-z0-9_]*")
CLASS = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
AXIS = re.compile(r"([+-])([XYZ])")
REQUIRED = ("relaxed",)
# Unlisted items fall back to these grips in the game.
REQUIRED_GRIPS = ("rod", "orb")
# Emitted numbers are C++ floats in cm or degrees; anything larger is a typo.
LIMIT = 1e5


def _mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def _axis(axis, degrees):
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    if axis == "x":
        return [[1, 0, 0], [0, c, -s], [0, s, c]]
    if axis == "y":
        return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def bone_quat(flex, spread=0.0, twist=0.0):
    """(x, y, z, w) of Rx(twist) * Rz(spread) * Ry(flex), w >= 0."""
    m = _mul(_mul(_axis("x", twist), _axis("z", spread)), _axis("y", flex))
    t = m[0][0] + m[1][1] + m[2][2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        w, x, y, z = 0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        w, x, y, z = (m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        w, x, y, z = (m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        w, x, y, z = (m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s
    n = math.sqrt(x * x + y * y + z * z + w * w)
    q = [x / n, y / n, z / n, w / n]
    return q if q[3] >= 0 else [-v for v in q]


def _num(value):
    value = round(float(value), 6)
    return repr(value if value != 0 else 0.0)


def _vector(values, label, length=3):
    if not isinstance(values, list) or len(values) != length:
        raise ValueError("%s must be %d finite numbers" % (label, length))
    return [_finite(v, label) for v in values]


def _finite(value, label, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or abs(value) > LIMIT:
        raise ValueError("%s must be a finite number within %g" % (label, LIMIT))
    if positive and round(float(value), 6) <= 0:
        raise ValueError("%s must be positive" % label)
    return float(value)


def _unit(values, label):
    length = math.sqrt(sum(v * v for v in values))
    if length < 1e-6:
        raise ValueError("%s must be non-zero" % label)
    return [v / length for v in values]


def _local_axis(value, label):
    if isinstance(value, list):
        return _unit(_vector(value, label), label)
    match = AXIS.fullmatch(value) if isinstance(value, str) else None
    if not match:
        raise ValueError("%s must be one of +X -X +Y -Y +Z -Z or a vector" % label)
    sign = 1.0 if match.group(1) == "+" else -1.0
    return [sign if axis == match.group(2) else 0.0 for axis in "XYZ"]


def render(source):
    data = json.loads(source)
    poses = data.get("poses")
    if not isinstance(poses, dict) or not poses:
        raise ValueError("expected a non-empty \"poses\" object")
    for name in REQUIRED:
        if name not in poses:
            raise ValueError("the %s pose is required" % name)
    for name in REQUIRED_GRIPS:
        if not isinstance(poses.get(name), dict) or not isinstance(poses[name].get("hand"), dict):
            raise ValueError("the %s grip pose is required" % name)
    out = ["// Generated by Tools/import/generate_hand_poses.py; edit Plugins/WoTFirstPerson/Data/hand_poses.json."]
    rows = []
    grips = {}
    for name in sorted(poses):
        pose = poses[name]
        if not NAME.fullmatch(name):
            raise ValueError("invalid pose name %r" % name)
        if not isinstance(pose, dict):
            raise ValueError("%s: a pose must be an object" % name)
        bones = pose.get("bones", {})
        if not isinstance(bones, dict):
            raise ValueError("%s: bones must be an object" % name)
        entries = []
        for bone in sorted(bones):
            if bone not in BONES:
                raise ValueError("%s: unknown bone %r" % (name, bone))
            angles = bones[bone]
            if not isinstance(angles, list) or not 1 <= len(angles) <= 3:
                raise ValueError("%s/%s: expected [flex, spread, twist]" % (name, bone))
            q = bone_quat(*_vector(list(angles) + [0.0] * (3 - len(angles)), "%s/%s" % (name, bone)))
            entries.append('{TEXT("%s"), FQuat(%s)}' % (bone, ", ".join(_num(v) for v in q)))
        symbol = "HandPoseBones_" + name
        out.append("const FWoTHandBoneRotation %s[] = {%s};" % (symbol, ", ".join(entries) or "{nullptr, FQuat::Identity}"))
        hand, item = pose.get("hand"), pose.get("item")
        for part, value in (("hand", hand), ("item", item)):
            if value is not None and not isinstance(value, dict):
                raise ValueError("%s: %s must be an object" % (name, part))
        has_grip = hand is not None
        if has_grip:
            offset = _vector(hand.get("offset"), name + " hand offset")
            rotator = _vector(hand.get("rotator"), name + " hand rotator")
            scale = _finite(hand.get("scale", 1.0), name + " hand scale", positive=True)
            _finite(offset[0], name + " hand offset X", positive=True)
            if item is None:
                raise ValueError("%s: a grip pose needs an item placement" % name)
            axis = _unit(_vector(item.get("axis"), name + " item axis"), name + " item axis")
            front = _vector(item.get("front"), name + " item front")
            along = sum(a * b for a, b in zip(axis, front))
            front = _unit([b - a * along for a, b in zip(axis, front)], name + " item front (across the axis)")
            point = _vector(item.get("point"), name + " item point")
            roll = _finite(item.get("roll", 0.0), name + " item roll")
            extent = _finite(item.get("half_length", 11.0), name + " item half_length", positive=True)
            lead = _finite(item.get("lead", 0.0), name + " item lead")
        else:
            offset, rotator, scale = [0.0] * 3, [0.0] * 3, 0.0
            axis, front, point, roll, extent, lead = [1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0] * 3, 0.0, 0.0, 0.0
        grips[name] = has_grip
        rows.append('{TEXT("%s"), %s, UE_ARRAY_COUNT(%s), %s, FVector(%s), FRotator(%s), %sf, FVector(%s), FVector(%s), FVector(%s), %sf, %sf, %sf}' % (
            name, symbol, symbol, "true" if has_grip else "false", ", ".join(_num(v) for v in offset),
            ", ".join(_num(v) for v in rotator), _num(scale), ", ".join(_num(v) for v in axis),
            ", ".join(_num(v) for v in front), ", ".join(_num(v) for v in point), _num(roll), _num(extent), _num(lead)))
    out.append("const FWoTHandPoseRow HandPoseRows[] = {")
    out.extend("\t" + row + "," for row in rows)
    out.append("};")
    items = data.get("items", {})
    if not isinstance(items, dict):
        raise ValueError("items must be an object")
    seen = {}
    for name in items:
        if not CLASS.fullmatch(name):
            raise ValueError("invalid item class %r" % name)
        if name.casefold() in seen:
            raise ValueError("duplicate item class %r" % name)
        seen[name.casefold()] = name
    out.append("const FWoTHeldItemGrip HeldItemGrips[] = {")
    for key in sorted(seen):
        name = seen[key]
        entry = items[name]
        grip = entry.get("grip") if isinstance(entry, dict) else None
        if not grips.get(grip):
            raise ValueError("%s: grip must name a grip pose" % name)
        axis = _local_axis(entry.get("axis"), name + " axis")
        front = _local_axis(entry.get("front"), name + " front")
        along = sum(a * b for a, b in zip(axis, front))
        if abs(along) > 0.95:
            raise ValueError("%s: front must be across the axis" % name)
        front = _unit([b - a * along for a, b in zip(axis, front)], name + " front")
        size = _finite(entry.get("size"), name + " size", positive=True)
        slide = _finite(entry.get("slide", 0.0), name + " slide")
        offset = _vector(entry.get("offset", [0.0, 0.0, 0.0]), name + " offset")
        out.append('\t{TEXT("%s"), TEXT("%s"), FVector(%s), FVector(%s), %sf, %sf, FVector(%s)},' % (
            name, grip, ", ".join(_num(v) for v in axis), ", ".join(_num(v) for v in front), _num(size), _num(slide),
            ", ".join(_num(v) for v in offset)))
    if not seen:
        out.append("\t{nullptr, nullptr, FVector::ZeroVector, FVector::ZeroVector, 0.0f, 0.0f, FVector::ZeroVector},")
    out.append("};")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--input", type=pathlib.Path, default=PLUGIN / "Data/hand_poses.json")
    parser.add_argument("--output", type=pathlib.Path,
                        default=PLUGIN / "Source/WoTFirstPerson/Private/Generated/WoTHandPoses.inl")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        expected = render(args.input.read_text(encoding="utf-8-sig")).encode("utf-8")
        current = args.output.read_bytes() if args.output.exists() else None
        if args.check:
            if current != expected:
                raise ValueError("generated hand poses are stale")
        elif current != expected:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(expected)
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
