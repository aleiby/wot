"""Shared wire types and material constants for map light animation.

One manifest drives the centered-response images, the material instances that
consume it, and the exact MPC contract below, whose one map actor is
configured from the same manifest.
"""

from __future__ import annotations

from pathlib import PurePosixPath, PureWindowsPath
from typing import Literal, TypedDict


WAVE_TYPES = ("LT_Pulse", "LT_Blink", "LT_SubtlePulse")
# UE1 ELightType wire values (Actor).  Driver actors must retain the type;
# period/phase alone are insufficient because Blink is integer parity, not sine.
LIGHT_TYPE_WIRE_IDS = {
    "LT_Pulse": 2,
    "LT_Blink": 3,
    "LT_SubtlePulse": 7,
}
# Retail multiplier written as ``mean + amplitude * (2 * WaveN - 1)``.
# Keeping this beside the group-order wire contract prevents the response
# manifest and the mover/mesh MID multiplier from acquiring separate shapes.
WAVEFORM_SHAPES = {
    "LT_Pulse": (0.60, 0.39),
    "LT_Blink": (0.50, 0.50),
    "LT_SubtlePulse": (0.90, 0.09),
}
# UE1 ELightEffect enum order (Actor), which is also the manifest order.
SHIMMER_EFFECTS = (
    "LE_TorchWaver", "LE_FireWaver", "LE_WateryShimmer",
)

MAX_WAVEFORM_GROUPS = 4
MAX_RESPONSE_TERMS = 6

# The exact MPC wire contract.  Freeze is opt-out by default: until a
# runtime driver explicitly enables animation, every centered coefficient is
# zero and the shader returns the literal mean atlas sample.
MPC_SCALAR_DEFAULTS = (
    ("Wave0", 0.5),
    ("Wave1", 0.5),
    ("Wave2", 0.5),
    ("Wave3", 0.5),
    ("NoiseFrame", 0.0),
    ("Freeze", 1.0),
)

# Small exactly-representable IDs feed a 32-bit avalanche in the material.
# The avalanche, rather than the magnitude of these values, decorrelates the
# three effect classes.
EFFECT_NOISE_SALTS = {
    "LE_TorchWaver": 1,
    "LE_FireWaver": 2,
    "LE_WateryShimmer": 3,
}

# Plain aliases, not typing.TypeAlias (3.10+): the Mac host pipeline runs 3.9.
WaveformGroupKey = tuple[str, int, int]
# json.dumps serializes this exact three-field tuple as the required JSON row.
WaveformGroupWireKey = WaveformGroupKey
AnimationTermKind = Literal["waveform", "effect", "intersection"]


class WaveformGroupManifest(TypedDict):
    """One driver group, sorted by ``(type, period, phase)``."""

    key: WaveformGroupWireKey
    mean: float
    amplitude: float
    light_names: list[str]
    light_count: int


class _OptionalAnimationTermFields(TypedDict, total=False):
    group: WaveformGroupWireKey
    effect: str
    effect_mean: float
    effect_amplitude: float


class AnimationTermManifest(_OptionalAnimationTermFields):
    """One centered-response slot in deterministic material slot order."""

    kind: AnimationTermKind
    light_names: list[str]
    light_count: int
    response_png: str


class MapAnimationManifest(TypedDict):
    """Value of the lightmap-atlas JSON ``animation`` section."""

    groups: list[WaveformGroupManifest]
    terms: list[AnimationTermManifest]


class MaterialTermBinding(TypedDict):
    """One manifest response resolved to its material coefficient selectors."""

    response_png: str
    wave_index: int
    effect_salt: int


def light_wave_group_selectors(light_actors, light_props, light_anim):
    """Resolve per-light selectors from the checked-in zone rows.

    Group order is the response/driver order: sorted exact
    ``(LightType, LightPeriod, LightPhase)`` keys.  Only emitting waveform
    lights create groups.  Every other row receives the steady sentinel -1.
    The returned mean/amplitude vectors always have four safe components so a
    material can consume them without a map-specific graph shape.
    """
    if not (isinstance(light_actors, list)
            and isinstance(light_props, list)
            and isinstance(light_anim, list)
            and len(light_actors) == len(light_props) == len(light_anim)):
        raise ValueError("zones light actors/props/anim tables are not parallel")

    rows = []
    active_keys = set()
    for name, props, anim in zip(light_actors, light_props, light_anim):
        if not isinstance(name, str) or not isinstance(props, list) \
                or len(props) < 4 or not isinstance(anim, list) \
                or len(anim) != 4:
            raise ValueError("zones light animation row has an invalid shape")
        light_type, period, phase, _effect = anim
        key = (light_type, int(period), int(phase))
        emits_wave = bool(props[3]) and light_type in WAVE_TYPES
        rows.append((name, key, emits_wave))
        if emits_wave:
            active_keys.add(key)

    keys = sorted(active_keys)
    if len(keys) > MAX_WAVEFORM_GROUPS:
        raise ValueError(
            f"zones light table has {len(keys)} waveform groups; maximum is "
            f"{MAX_WAVEFORM_GROUPS}")
    index_by_key = {key: index for index, key in enumerate(keys)}
    selectors = {
        name: index_by_key[key] if emits_wave else -1
        for name, key, emits_wave in rows
    }
    means = [1.0] * MAX_WAVEFORM_GROUPS
    amplitudes = [0.0] * MAX_WAVEFORM_GROUPS
    for index, key in enumerate(keys):
        means[index], amplitudes[index] = WAVEFORM_SHAPES[key[0]]
    return selectors, tuple(means), tuple(amplitudes)


def light_wave_multiplier(wave, group, means, amplitudes):
    """Reference algebra for the mover/mesh material's per-light ratio."""
    if group < 0:
        return 1.0
    if group >= MAX_WAVEFORM_GROUPS:
        raise ValueError(f"invalid waveform group selector {group}")
    mean = float(means[group])
    if mean <= 0.0:
        return 1.0
    # This centered form makes WaveN == 0.5 return the literal float 1.0:
    # the animated addend is exactly zero instead of recomputing mean/mean.
    return 1.0 + float(amplitudes[group]) \
        * (2.0 * float(wave[group]) - 1.0) / mean


def material_term_bindings(animation: MapAnimationManifest) \
        -> list[MaterialTermBinding]:
    """Validate the wire manifest and resolve its six material slots.

    A waveform selector of -1 and an effect salt of 0 are identities.  Thus
    the same coefficient graph evaluates waveform, effect, and intersection
    records without a per-kind shader permutation.
    """
    if not isinstance(animation, dict):
        raise ValueError("light animation manifest must be an object")
    groups = animation.get("groups")
    terms = animation.get("terms")
    if not isinstance(groups, list) or not isinstance(terms, list):
        raise ValueError("light animation manifest requires groups and terms")
    if len(groups) > MAX_WAVEFORM_GROUPS:
        raise ValueError(
            f"light animation has {len(groups)} groups; maximum is "
            f"{MAX_WAVEFORM_GROUPS}")
    if len(terms) > MAX_RESPONSE_TERMS:
        raise ValueError(
            f"light animation has {len(terms)} responses; maximum is "
            f"{MAX_RESPONSE_TERMS}")

    group_keys = []
    for group in groups:
        try:
            key = tuple(group["key"])
        except (KeyError, TypeError):
            raise ValueError("light animation group is missing its key") from None
        if len(key) != 3 or key[0] not in WAVE_TYPES:
            raise ValueError(f"invalid waveform group key {key!r}")
        key = (key[0], int(key[1]), int(key[2]))
        group_keys.append(key)
    if group_keys != sorted(set(group_keys)):
        raise ValueError("waveform groups are not unique and sorted")
    group_indices = {key: index for index, key in enumerate(group_keys)}
    effect_order = {effect: index for index, effect in enumerate(SHIMMER_EFFECTS)}

    bindings = []
    order_keys = []
    seen_pngs = set()
    for term in terms:
        if not isinstance(term, dict):
            raise ValueError("light animation term must be an object")
        kind = term.get("kind")
        response_png = term.get("response_png")
        if not isinstance(response_png, str) or not response_png.endswith(".png") \
                or PurePosixPath(response_png).name != response_png \
                or PureWindowsPath(response_png).name != response_png:
            raise ValueError(f"unsafe response PNG name {response_png!r}")
        if response_png in seen_pngs:
            raise ValueError(f"duplicate response PNG {response_png!r}")
        seen_pngs.add(response_png)

        wave_index = -1
        effect_salt = 0
        if kind in ("waveform", "intersection"):
            try:
                key = tuple(term["group"])
                key = (key[0], int(key[1]), int(key[2]))
                wave_index = group_indices[key]
            except (KeyError, TypeError, ValueError, IndexError):
                raise ValueError(
                    f"{kind} response has an unknown waveform group") from None
        if kind in ("effect", "intersection"):
            effect = term.get("effect")
            if effect not in EFFECT_NOISE_SALTS:
                raise ValueError(f"{kind} response has unknown effect {effect!r}")
            effect_salt = EFFECT_NOISE_SALTS[effect]

        if kind == "waveform":
            order_key = (0, wave_index)
        elif kind == "effect":
            order_key = (1, effect_order[term["effect"]])
        elif kind == "intersection":
            order_key = (2, wave_index, effect_order[term["effect"]])
        else:
            raise ValueError(f"unknown light animation term kind {kind!r}")
        order_keys.append(order_key)
        bindings.append(dict(
            response_png=response_png,
            wave_index=wave_index,
            effect_salt=effect_salt,
        ))

    if len(order_keys) != len(set(order_keys)):
        raise ValueError("light animation responses contain a duplicate term")
    if order_keys != sorted(order_keys):
        raise ValueError("light animation responses are not in material slot order")
    waveform_indices = [binding["wave_index"]
                        for term, binding in zip(terms, bindings)
                        if term["kind"] == "waveform"]
    if waveform_indices != list(range(len(group_keys))):
        raise ValueError("waveform response slots do not match the group table")
    return bindings


def light_wave_driver_groups(animation: MapAnimationManifest) \
        -> list[tuple[int, int, int]] | None:
    """Return ordered ``(wire_type, period, phase)`` rows, or no actor.

    An empty list is distinct from ``None``: it is the shimmer-only case, which
    still needs the one driver to advance ``NoiseFrame``. Validation is shared
    with material binding so spawn and shader slot order cannot diverge.
    """
    bindings = material_term_bindings(animation)
    if not bindings:
        return None
    return [(LIGHT_TYPE_WIRE_IDS[group["key"][0]],
             int(group["key"][1]), int(group["key"][2]))
            for group in animation["groups"]]
