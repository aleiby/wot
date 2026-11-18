"""Retail-derived stationary actor-light selection.

The candidate ordering, visibility threshold, and visible-count cap mirror
``FLightManager::SetupForActor``.  A static bake has no viewer, so its caller
supplies the actor-zone MaxLightCount as the documented max-quality proxy.
"""
import math
import struct


_F32 = struct.Struct("<f")
_U32 = struct.Struct("<I")


def _float32(value):
    return _F32.unpack(_F32.pack(float(value)))[0]


def _float_bits(value):
    return _U32.unpack(_F32.pack(_float32(value)))[0]


def _bits_float(value):
    return _F32.unpack(_U32.pack(int(value) & 0xffffffff))[0]


def _build_sqrt_mantissa_table():
    """UE1 FLightManager::Init 10-bit SqrtManTbl."""
    table = [0.0] * 2048
    sqrt_two = _float32(math.sqrt(2.0))
    for mantissa in range(1024):
        sample = _bits_float(0x3f800000 + (mantissa << 13))
        root = _float32(math.sqrt(sample))
        root = _bits_float(_float_bits(root) - (64 << 23))
        # The retail table stores both exponent parities.  Every assignment is
        # to FLOAT, so round each result back to binary32 here.
        table[mantissa] = _float32(root * sqrt_two)
        table[mantissa + 1024] = _float32(root * 2.0)
    return tuple(table)


_SQRT_MANTISSA = _build_sqrt_mantissa_table()


def sqrt_approx(value):
    """Retail renderer SqrtApprox for a positive binary32.

    The high ten mantissa bits plus the exponent low bit select SqrtManTbl;
    the remaining exponent bits are halved and added directly to the lookup's
    IEEE-754 representation.  Actor-light distance inputs are non-negative.
    """
    bits = _float_bits(value)
    table_index = (bits >> 13) & 0x7ff
    exponent_half = (bits & 0x7f000000) >> 1
    return _bits_float(_float_bits(_SQRT_MANTISSA[table_index])
                       + exponent_half)


def _distance_squared(a, b):
    # UE1 FVector and FDistSquared operate on FLOAT.  Explicit binary32
    # round-trips keep the Python selector aligned with the retail priority
    # input before the mantissa lookup.
    total = _float32(0.0)
    for axis in range(3):
        delta = _float32(_float32(a[axis]) - _float32(b[axis]))
        total = _float32(total + _float32(delta * delta))
    return total


def lighting_tag(actor_location, light, distance_squared=None):
    """Retail AddConsider LightingTag using raw LightBrightness."""
    radius = _float32(light["radius"])
    if radius <= 0.0:
        raise ValueError("light radius must be positive")
    if distance_squared is None:
        distance_squared = _distance_squared(
            actor_location, light["location"])
    falloff = _float32(_float32(1.0) - _float32(
        sqrt_approx(distance_squared) / radius))
    value = _float32(_float32(
        falloff * _float32(int(light["brightness"]))) * _float32(1024.0))
    # appRound is appFloor(F + 0.5).
    return int(math.floor(_float32(value + _float32(0.5))))


def select_mesh_lights(*, actor_location, actor_special_lit, max_lights,
                       permeating_names, lights_by_name, line_check):
    """Return emitting light dictionaries in retail steady-state order."""
    cap = max(1, min(int(max_lights), 16))
    candidates = []
    for name in permeating_names:
        light = lights_by_name.get(name)
        if light is None:
            continue
        if bool(light.get("special_lit", False)) != bool(actor_special_lit):
            continue
        radius = float(light["radius"])
        if radius <= 0.0:
            continue
        distance_squared = _distance_squared(
            actor_location, light["location"])
        if distance_squared >= radius * radius:
            continue
        candidates.append((
            lighting_tag(actor_location, light, distance_squared), light))

    # Preserve shipped permeating-run order for equal integer tags. Retail's
    # generic sort is unstable for larger arrays; no pinned selection depends
    # on a tie, so deterministic source order is the least lossy tie policy.
    candidates.sort(key=lambda item: -item[0])
    selected = []
    visible_count = 0
    threshold = -1
    for tag, light in candidates:
        if tag < threshold or visible_count >= cap:
            break
        if not line_check(actor_location, light["location"]):
            continue
        if threshold == -1:
            # C++ integer division truncates toward zero (Python // floors).
            threshold = tag // 8 if tag >= 0 else -((-tag) // 8)
        # SetupForActor establishes Threshold and increments NumRealVisible
        # before AddLight, whose LT_None branch rejects the light.  Lightning
        # controllers fold their targets to LT_None at rest, so such candidates
        # consume the visible cap and raise the threshold without reaching the
        # material arrays.
        visible_count += 1
        if bool(light.get("emits", True)):
            selected.append(light)
    return selected
