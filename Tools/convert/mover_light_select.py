"""Retail dynamic-mover per-surface light selection helpers.

UE1's SetupMoverSurface selects from the mover-origin leaf's
permeating run in shipped order.  The only consider gates are oriented surface
plane facing and bSpecialLit equality.  There is deliberately no radius test,
LightingTag sort, BSP LineCheck, or Min/MaxLightCount cap.
"""
import math


def _sintab(value):
    """UE1's 16384-entry sine-table quantization."""
    return math.sin(((int(value) >> 2) & 16383)
                    * (2.0 * math.pi / 16384.0))


def rotation_axes(rotation):
    """UE1 rotator [pitch,yaw,roll] local X/Y/Z axes in world space."""
    pitch, yaw, roll = (int(value) for value in rotation)
    cp, sp = _sintab(pitch + 16384), _sintab(pitch)
    cy, sy = _sintab(yaw + 16384), _sintab(yaw)
    cr, sr = _sintab(roll + 16384), _sintab(roll)
    return (
        (cp * cy, cp * sy, sp),
        (sr * sp * cy - cr * sy,
         sr * sp * sy + cr * cy, -sr * cp),
        (-(cr * sp * cy + sr * sy),
         cy * sr - cr * sp * sy, cr * cp),
    )


def _transform_vector(vector, axes):
    return tuple(sum(axes[local][world] * float(vector[local])
                     for local in range(3)) for world in range(3))


def transform_plane(actor_location, actor_rotation, local_origin, local_normal):
    """Return a manifest mover plane transformed to its current world pose."""
    axes = rotation_axes(actor_rotation)
    rotated_origin = _transform_vector(local_origin, axes)
    origin = tuple(float(actor_location[axis]) + rotated_origin[axis]
                   for axis in range(3))
    normal = _transform_vector(local_normal, axes)
    length = math.sqrt(sum(value * value for value in normal))
    if length <= 0.0:
        raise ValueError("mover surface has a zero normal")
    return origin, tuple(value / length for value in normal)


def select_mover_surface_lights(permeating_lights, surface_origin,
                                surface_normal, mover_special_lit,
                                emitting_only=True):
    """Return retail-selected rows in the shipped permeating order."""
    selected = []
    for light in permeating_lights:
        if bool(light.get("special_lit", False)) != bool(mover_special_lit):
            continue
        delta = tuple(float(light["location"][axis])
                      - float(surface_origin[axis]) for axis in range(3))
        if sum(delta[axis] * float(surface_normal[axis])
               for axis in range(3)) <= 0.0:
            continue
        # AddLight rejects zero-brightness and resting LT_None/Lightning-gated
        # actors after the two SetupMoverSurface consider gates. Keep a switch
        # for the census, whose contract sizes the full facing/special-lit run.
        if emitting_only and (not bool(light.get("emits", True))
                              or int(light.get("brightness", 0)) == 0):
            continue
        selected.append(light)
    return selected
