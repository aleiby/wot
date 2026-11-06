"""UE1 IceTexture scalar blitters + flipbook baker.

Ports ``UIceTexture::MoveIcePosition``, ``BlitTexIce`` and ``BlitIceTex``
from Fire/Src/UnFractal.cpp. Ice is a pure indexed displacement: each glass
byte is added directly to the source U lookup, so the output never invents or
blends palette colours.

Frame zero is rendered at the phase serialized in the package. Later frames
honour IceTexture.TimeMethod: frame-rate-sync advances one virtual 1/120 s
tick per rendered frame, while real-time-scroll advances by the supplied
wall-clock delta.
"""
import math

import numpy as np


(SLIDE_LINEAR, SLIDE_CIRCULAR, SLIDE_GESTATION,
 SLIDE_WAVY_X, SLIDE_WAVY_Y) = range(5)
TIME_FRAME_RATE_SYNC, TIME_REAL_TIME_SCROLL = range(2)


def sbyte(value):
    """UE1 SBYTE conversion used by the pan-speed controls."""
    value = int(value) & 0xFF
    return value - 256 if value >= 128 else value


def app_round(value):
    """Retail Win32 appRound: x87 FISTP round-to-nearest-even."""
    return int(round(float(value)))


class IceSim:
    """Non-iterative IceTexture position and scalar displacement renderer."""

    def __init__(self, fractal, source_idx, glass_idx):
        if not fractal.get("mips"):
            raise ValueError("IceTexture has no mip dimensions")
        self.h, self.w = np.asarray(source_idx).shape
        target_w, target_h, _ = fractal["mips"][0]
        self.source = np.asarray(source_idx, dtype=np.uint8)
        self.glass = np.asarray(glass_idx, dtype=np.uint8)
        if self.glass.shape != (self.h, self.w):
            raise ValueError("IceTexture source and glass dimensions differ")
        if (target_w, target_h) != (self.w, self.h):
            raise ValueError("IceTexture source/glass dimensions differ from output")
        if self.w & (self.w - 1) or self.h & (self.h - 1):
            raise ValueError("IceTexture dimensions must be powers of two")
        self.umask, self.vmask = self.w - 1, self.h - 1

        props = fractal["props"]
        self.panning_style = int(props.get("PanningStyle", SLIDE_LINEAR))
        self.time_method = int(props.get("TimeMethod", TIME_FRAME_RATE_SYNC))
        self.horiz_pan_speed = int(props.get("HorizPanSpeed", 0))
        self.vert_pan_speed = int(props.get("VertPanSpeed", 0))
        self.frequency = int(props.get("Frequency", 0))
        self.amplitude = int(props.get("Amplitude", 0))
        self.move_ice = bool(props.get("MoveIce", False))

        # These persistent floats are authored state, not transient warmup.
        self.master_count = np.float32(props.get("MasterCount", 0.0))
        self.u_displace = np.float32(props.get("UDisplace", 0.0))
        self.v_displace = np.float32(props.get("VDisplace", 0.0))
        self.u_position = np.float32(props.get("UPosition", 0.0))
        self.v_position = np.float32(props.get("VPosition", 0.0))

        self._u = np.arange(self.w, dtype=np.int32)[None, :]
        self._v = np.arange(self.h, dtype=np.int32)[:, None]

    @property
    def u_offset(self):
        return app_round(self.u_position) & self.umask

    @property
    def v_offset(self):
        return app_round(self.v_position) & self.vmask

    def move_position(self, delta_seconds):
        """Port of UIceTexture::MoveIcePosition for one effective time step."""
        # All persistent members and DeltaTime are C++ FLOATs. Preserve the
        # float32 assignment points; trig itself takes UE1's DOUBLE argument.
        delta = np.float32(delta_seconds)
        self.master_count = np.float32(
            self.master_count + np.float32(delta * np.float32(120.0)))
        self.u_displace = np.float32(
            self.u_displace + np.float32(
                np.float32(2.0) * delta * -sbyte(self.horiz_pan_speed - 128)))
        self.v_displace = np.float32(
            self.v_displace + np.float32(
                np.float32(2.0) * delta * sbyte(self.vert_pan_speed - 128)))

        phase = np.float32(
            np.float32(self.master_count * np.float32(self.frequency + 1))
            * np.float32(0.0012))
        amplitude = float(self.amplitude + 1)
        if self.panning_style == SLIDE_LINEAR:
            self.u_position = self.u_displace
            self.v_position = self.v_displace
        elif self.panning_style == SLIDE_CIRCULAR:
            self.u_position = np.float32(self.u_displace + app_round(np.float32(
                amplitude * math.sin(float(phase)))))
            self.v_position = np.float32(self.v_displace + app_round(np.float32(
                amplitude * math.cos(float(phase)))))
        elif self.panning_style == SLIDE_GESTATION:
            self.u_position = np.float32(self.u_displace + app_round(np.float32(
                amplitude * math.sin(float(phase)))))
            gestation_phase = np.float32(
                np.float32(self.master_count * np.float32(self.frequency + 1))
                * np.float32(0.0011))
            self.v_position = np.float32(self.v_displace + app_round(np.float32(
                amplitude * math.cos(float(gestation_phase)))))
        elif self.panning_style == SLIDE_WAVY_X:
            self.u_position = np.float32(self.u_displace + app_round(np.float32(
                0.5 * amplitude * math.sin(float(phase)))))
            self.v_position = self.v_displace
        elif self.panning_style == SLIDE_WAVY_Y:
            self.u_position = self.u_displace
            self.v_position = np.float32(self.v_displace + app_round(np.float32(
                0.5 * amplitude * math.cos(float(phase)))))
        else:
            raise ValueError(f"unknown IceTexture PanningStyle {self.panning_style}")

    def advance(self, delta_seconds):
        """Advance according to IceTexture.TimeMethod."""
        if self.time_method == TIME_FRAME_RATE_SYNC:
            self.move_position(1.0 / 120.0)
        elif self.time_method == TIME_REAL_TIME_SCROLL:
            self.move_position(delta_seconds)
        else:
            raise ValueError(f"unknown IceTexture TimeMethod {self.time_method}")

    def render_indices(self):
        """Render the selected scalar C blit path to source-palette indices."""
        udisp, vdisp = self.u_offset, self.v_offset
        if self.move_ice:
            # BlitTexIce: source row is static; the panned glass byte displaces U.
            glass = self.glass[(self._v + vdisp) & self.vmask,
                               (self._u + udisp) & self.umask]
            return self.source[self._v, (self._u + glass) & self.umask]

        # BlitIceTex: glass is static; the source moves in U and V beneath it.
        return self.source[(self._v + vdisp) & self.vmask,
                           (self._u + udisp + self.glass) & self.umask]


def bake(fractal, source_idx, glass_idx, source_palette, frames=32, fps=32):
    """Bake IceTexture to RGBA frames, starting at its serialized authored phase."""
    if frames < 1 or fps <= 0:
        raise ValueError("IceTexture bake requires positive frames and fps")
    sim = IceSim(fractal, source_idx, glass_idx)
    palette = np.asarray([c[:3] + (255,) for c in source_palette], dtype=np.uint8)
    if bool(fractal["props"].get("bMasked", False)):
        palette[0, 3] = 0

    out = []
    for frame in range(frames):
        if frame:
            sim.advance(1.0 / fps)
        out.append(palette[sim.render_indices()])
    return out
