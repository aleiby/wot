"""Exact UE1 WaterTexture/WetTexture/WaveTexture simulation.

This is a direct, byte-oriented reimplementation of the shipped engine
machinery in the original Fire package: 128-initialized interleaved
half-resolution fields, both ``CalculateWater`` parity passes,
``Output4Pix``, and every authored ``WaterRedrawDrops`` case.
``bake_flipbooks.py`` uses this module as the cross-platform oracle for the
C++ runtime port.
"""
import math

import numpy as np


(D_FIXED, D_PHASE, D_SHALLOW, D_HALF, D_RANDMOVE, D_FIXEDRAND,
 D_WHIRLY, D_BIGWHIRLY, D_HLINE, D_VLINE, D_DIAG1, D_DIAG2,
 D_HOSC, D_VOSC, D_DOSC1, D_DOSC2, D_RAIN, D_AREACLAMP,
 D_LEAKY, D_DRIPPY) = range(20)
D_WHIRLY_BACK = 64
D_BIGWHIRLY_BACK = 65


def _app_round(value):
    # All table inputs are non-negative; UE1 appRound is floor(x + 0.5).
    return int(math.floor(float(value) + 0.5))


def _phase_value(index):
    # InitTables passes a FLOAT angle to the DOUBLE appSin, then converts the
    # completed expression back to FLOAT at appRound's call boundary.
    angle = np.float32(
        np.float32(np.float32(index) / np.float32(256.0))
        * np.float32(6.2831853))
    value = np.float32(
        float(np.float32(127.45))
        + float(np.float32(127.5)) * math.sin(float(angle)))
    return _app_round(value)


PHASE = np.array([_phase_value(t) for t in range(256)], dtype=np.int32)

WAVETABLE = np.array([
    max(0, min(255, (t // 2 - 256) + (1 if (t - 512) < 256 else 0)))
    for t in range(1536)
], dtype=np.uint8)


class SpeedRand:
    """Retail RANDASM lagged-XOR recurrence with deterministic initialization.

    The package does not serialize Fire's process-global ``appRand`` table.
    A seed-1 instance of the retail MSVC ``rand`` used by ``appRand`` fills the
    same 512-byte table, after which calls reproduce the x86 DWORD recurrence
    exactly.  SLWater02 includes Rain drops, so the pinned fixture also
    cross-validates this deterministic initialization and call sequence.
    """

    def __init__(self, seed=1):
        state = int(seed) & 0xffffffff
        self.arr = bytearray(512)
        for i in range(512):
            state = (214013 * state + 2531011) & 0xffffffff
            self.arr[i] = (state >> 16) & 0xff
        self.index = 0

    def __call__(self):
        source = (self.index + 4 * (1 + 31)) & 0xfc
        dest = (self.index + 4) & 0xfc
        word = int.from_bytes(self.arr[source:source + 4], "little")
        dest_word = int.from_bytes(self.arr[dest:dest + 4], "little") ^ word
        self.arr[dest:dest + 4] = dest_word.to_bytes(4, "little")
        self.index = dest
        return word & 0xff


# The original engine owns this table at package/process scope, not per texture.
# Seed 1 is the deterministic substitute for the unavailable retail appRand
# history.  Callers may still inject a private stream for isolated fixtures.
GLOBAL_SPEED_RAND = SpeedRand(1)


def reset_global_random(seed=1):
    global GLOBAL_SPEED_RAND
    GLOBAL_SPEED_RAND = SpeedRand(seed)
    return GLOBAL_SPEED_RAND


def global_random():
    return GLOBAL_SPEED_RAND


def build_wave_lut(props):
    """Reimplements the original engine's UWaveTexture::SetWaveLight."""
    wave_amp = int(props.get("WaveAmp", 0))
    # PI and 255.0 are DOUBLE in the source; assignments round to FLOAT.
    lamp = np.float32(math.pi * int(props.get("BumpMapLight", 0)) / 255.0)
    viewer = np.float32(math.pi * int(props.get("BumpMapAngle", 0)) / 255.0)
    phong_range = int(props.get("PhongRange", 0))
    phong_radius = np.float32(
        np.float32(int(props.get("PhongSize", 0))) / np.float32(512.0))
    amplitude = np.float32(
        np.float32(wave_amp) / np.float32(255.0))
    table = np.zeros(1024, dtype=np.uint8)
    for i in range(1024):
        x = np.float32(amplitude * np.float32(512.0 - i)
                       / np.float32(196.0))
        # FakeAtan accepts DOUBLE, but its Abs argument is explicitly FLOAT.
        fake_atan = (float(np.float32(3.1415) * np.float32(0.5)) * float(x)
                     / (abs(float(np.float32(x))) + 1.0))
        normal = np.float32(fake_atan + math.pi * float(np.float32(0.5)))
        temp = int((256 - phong_range // 2)
                   * math.cos(float(np.float32(normal - lamp))))
        reflected = np.float32(normal * np.float32(2.0) - lamp)
        delta = np.float32(reflected - viewer)
        if delta * delta < phong_radius * phong_radius and phong_radius != 0:
            highlight = np.float32(
                np.float32(phong_range * 2)
                * np.float32(phong_radius - np.float32(abs(float(delta))))
                / phong_radius)
            temp += int(highlight)
        table[i] = max(0, min(255, temp))
    return table


class WaterSim:
    """Exact constant-time water step; ``out`` is the rendered P8 bitmap."""

    def __init__(self, fractal, seed=None, table=None, rand=None):
        w, h, data = fractal["mips"][0]
        if w < 8 or h < 8 or w & (w - 1) or h & (h - 1):
            raise ValueError(f"water dimensions must be power-of-two >= 8: {w}x{h}")
        self.w, self.h = int(w), int(h)
        self.ubits = self.w.bit_length() - 1
        self.umask, self.vmask = self.w - 1, self.h - 1
        self.xd, self.yd = self.w // 2, self.h // 2
        self.source_fields = np.full(self.w * self.h // 2, 128, dtype=np.uint8)
        initial = bytes(data[:self.w * self.h]).ljust(self.w * self.h, b"\0")
        self.out = np.frombuffer(initial, dtype=np.uint8).reshape(
            self.h, self.w).copy()
        self.wave_parity = 0
        self.global_phase = 0
        self.prime_current = 0
        self.step_count = 0
        self.accumulator = np.float32(0.0)

        props = fractal["props"]
        self.prime_count = max(0, int(props.get("PrimeCount", 0)))
        self.min_frame_rate = np.float32(props.get("MinFrameRate", 0.0))
        self.max_frame_rate = np.float32(props.get("MaxFrameRate", 0.0))
        self.wave_amp = int(props.get("WaveAmp", 0))
        if table is None:
            # UWetTexture::SetRefractionTable: signed clamp stored in BYTE.
            values = []
            for i in range(1024):
                value = int(np.float32(i - 511)
                            * np.float32(np.float32(self.wave_amp)
                                         / np.float32(512.0)))
                values.append(max(-128, min(127, value)) & 0xff)
            self.render_table = np.array(values, dtype=np.uint8)
        else:
            self.render_table = np.asarray(table, dtype=np.uint8).reshape(1024)

        raw = props.get("_structs", {}).get("Drops", {})
        count = max(0, min(int(props.get("NumDrops", 0)), 256))
        self.drops = [list(bytes(raw[i])[:8].ljust(8, b"\0"))
                      for i in sorted(raw)[:count]]
        # The default is the process-global Fire stream.  Isolated fixtures
        # inject SpeedRand(seed) explicitly when they need an independent run.
        self.rand = (rand if rand is not None else
                     SpeedRand(seed) if seed is not None else
                     GLOBAL_SPEED_RAND)

    def _field_index(self, field_b, x, y):
        return ((y & (self.yd - 1)) << self.ubits) \
            + (self.xd if field_b else 0) + (x & (self.xd - 1))

    def _output4_values(self, old_dest, source_a, source_c, source_e,
                        source_g, source_b, source_d, source_f, source_h):
        wave_index = (512 + int(source_e) + int(source_g)
                      + int(source_f) + int(source_h) - (int(old_dest) << 1))
        new_dest = int(WAVETABLE[wave_index])
        ea = int(source_e) - int(source_a)
        fb = int(source_f) - int(source_b)
        gc = int(source_g) - int(source_c)
        hd = int(source_h) - int(source_d)
        # C++ signed integer division truncates toward zero.
        average = int((fb + hd + ea + gc) / 2)
        dest1 = int(self.render_table[512 + average])
        dest2 = int(self.render_table[512 + gc + hd])
        dest3 = int(self.render_table[512 + fb + hd])
        dest4 = int(self.render_table[512 + hd + hd])
        return new_dest, dest1, dest2, dest3, dest4

    def _output4(self, dest_index, sources, pixels):
        values = self._output4_values(int(self.source_fields[dest_index]), *sources)
        self.source_fields[dest_index] = values[0]
        for (y, x), value in zip(pixels, values[1:]):
            self.out[y & self.vmask, x & self.umask] = value

    def redraw_drops(self):
        fields = self.source_fields
        xmask, ymask = self.xd - 1, self.yd - 1
        self.global_phase += 1

        def put_both(x, y, value_a, value_b=None):
            base = ((y & ymask) << self.ubits) + (x & xmask)
            fields[base] = value_a & 0xff
            fields[base + self.xd] = (value_a if value_b is None else value_b) & 0xff

        for drop in self.drops:
            kind = drop[0]
            xnow, ynow = drop[2], drop[3]
            if kind == D_FIXED:
                put_both(xnow, ynow, drop[7])
            elif kind == D_PHASE:
                drop[1] = (drop[1] + drop[7]) & 0xff
                put_both(xnow, ynow, int(PHASE[drop[1]]))
            elif kind == D_HALF:
                drop[1] = (drop[1] + drop[7]) & 0xff
                put_both(xnow, ynow, max(128, int(PHASE[drop[1]])))
            elif kind == D_SHALLOW:
                drop[1] = (drop[1] + drop[7]) & 0xff
                put_both(xnow, ynow, 64 + (int(PHASE[drop[1]]) >> 1))
            elif kind == D_RANDMOVE:
                drop[2] = (xnow - (self.rand() & 3) + (self.rand() & 3)) & xmask
                drop[3] = (ynow - (self.rand() & 3) + (self.rand() & 3)) & ymask
                # Retail computes DropDest before moving the authored record.
                put_both(xnow, ynow, 128 + 57, 128 - 57)
            elif kind == D_FIXEDRAND:
                put_both(xnow, ynow, self.rand(), self.rand())
            elif kind in (D_WHIRLY, D_WHIRLY_BACK,
                          D_BIGWHIRLY, D_BIGWHIRLY_BACK):
                phase = drop[4] | (drop[5] << 8)
                delta = drop[6] | (drop[7] << 8)
                if kind in (D_WHIRLY_BACK, D_BIGWHIRLY_BACK):
                    phase = (phase - delta) & 0xffff
                else:
                    phase = (phase + delta) & 0xffff
                drop[4], drop[5] = phase & 0xff, (phase >> 8) & 0xff
                shift = 3 if kind in (D_BIGWHIRLY, D_BIGWHIRLY_BACK) else 4
                x = xnow + (int(PHASE[drop[5]]) >> shift)
                y = ynow + (int(PHASE[(drop[5] + 64) & 0xff]) >> shift)
                put_both(x, y, drop[1])
            elif kind in (D_HLINE, D_VLINE, D_DIAG1, D_DIAG2,
                          D_HOSC, D_VOSC, D_DOSC1, D_DOSC2):
                if kind >= D_HOSC:
                    drop[1] = (drop[1] + drop[6]) & 0xff
                    depth = int(PHASE[drop[1]])
                else:
                    depth = drop[1]
                segment = drop[7] >> 1
                for t in range(segment + 1):
                    if kind in (D_HLINE, D_HOSC):
                        x, y = xnow + t, ynow
                    elif kind in (D_VLINE, D_VOSC):
                        x, y = xnow, ynow + t
                    elif kind in (D_DIAG1, D_DOSC1):
                        x, y = xnow - t, ynow + t
                    else:
                        x, y = xnow + t, ynow + t
                    put_both(x, y, depth)
            elif kind == D_AREACLAMP:
                segment = drop[7] >> 1
                for v in range(segment):
                    # Original loop writes Dest before advancing it and runs
                    # u=1..SegSize, so the covered width is exactly SegSize.
                    for u in range(segment):
                        put_both(xnow + u, ynow + v, drop[1])
            elif kind == D_RAIN:
                if (self.rand() & 15) == 0:
                    spray = drop[7]
                    y = ynow + ((self.rand() * spray) >> 8)
                    x = xnow + ((self.rand() * spray) >> 8)
                    put_both(x, y, drop[1], 255 ^ drop[1])
            elif kind == D_LEAKY:
                drop[4] = (drop[4] + drop[7]) & 0xff
                if drop[4] <= drop[7]:
                    put_both(xnow, ynow, drop[1], 255 ^ drop[1])
            elif kind == D_DRIPPY:
                drop[4] = (drop[4] + drop[7]) & 0xff
                if drop[4] <= drop[7]:
                    drop[4] = self.rand()
                    put_both(xnow, ynow, drop[1], 255 ^ drop[1])

    def calculate_water(self):
        self.wave_parity = (self.wave_parity + 1) & 0xff
        if (self.wave_parity & 1) == 0:
            # EVEN: B fields from previous/current rows update A.
            for y in range(self.yd):
                for x in range(self.xd):
                    sources = (
                        self.source_fields[self._field_index(True, x - 3, y - 1)],
                        self.source_fields[self._field_index(True, x - 2, y - 1)],
                        self.source_fields[self._field_index(True, x - 1, y - 1)],
                        self.source_fields[self._field_index(True, x, y - 1)],
                        self.source_fields[self._field_index(True, x - 3, y)],
                        self.source_fields[self._field_index(True, x - 2, y)],
                        self.source_fields[self._field_index(True, x - 1, y)],
                        self.source_fields[self._field_index(True, x, y)],
                    )
                    pixels = ((2 * y - 1, 2 * x - 1),
                              (2 * y - 1, 2 * x),
                              (2 * y, 2 * x - 1),
                              (2 * y, 2 * x))
                    self._output4(self._field_index(False, x, y), sources, pixels)
        else:
            # ODD: A fields from current/next rows update B.
            for y in range(self.yd):
                for x in range(self.xd):
                    sources = (
                        self.source_fields[self._field_index(False, x - 2, y)],
                        self.source_fields[self._field_index(False, x - 1, y)],
                        self.source_fields[self._field_index(False, x, y)],
                        self.source_fields[self._field_index(False, x + 1, y)],
                        self.source_fields[self._field_index(False, x - 2, y + 1)],
                        self.source_fields[self._field_index(False, x - 1, y + 1)],
                        self.source_fields[self._field_index(False, x, y + 1)],
                        self.source_fields[self._field_index(False, x + 1, y + 1)],
                    )
                    pixels = ((2 * y, 2 * x),
                              (2 * y, 2 * x + 1),
                              (2 * y + 1, 2 * x),
                              (2 * y + 1, 2 * x + 1))
                    self._output4(self._field_index(True, x, y), sources, pixels)

    def tick(self, before_step=None):
        """One ConstantTimeTick, with UWetTexture's source lock hook."""
        if before_step is not None:
            before_step()
        self.redraw_drops()
        self.calculate_water()
        self.step_count += 1

    def prime(self, before_step=None):
        while self.prime_current < self.prime_count:
            self.prime_current += 1
            self.tick(before_step)

    def tick_texture(self, delta_seconds, before_step=None):
        """Matches UE1's UTexture::Tick accumulator and PrimeCount semantics."""
        before = self.step_count
        self.prime(before_step)
        if self.max_frame_rate == np.float32(0.0):
            self.tick(before_step)
            return self.step_count - before
        min_time = np.float32(1.0) / np.clip(
            self.max_frame_rate, np.float32(0.01), np.float32(100.0))
        max_time = np.float32(1.0) / np.clip(
            self.min_frame_rate, np.float32(0.01), np.float32(100.0))
        self.accumulator = np.float32(
            self.accumulator + np.float32(delta_seconds))
        if self.accumulator < min_time:
            pass
        elif self.accumulator < max_time:
            self.tick(before_step)
            self.accumulator = np.float32(0.0)
        else:
            self.tick(before_step)
            self.accumulator = np.float32(self.accumulator - max_time)
            if self.accumulator > max_time:
                self.accumulator = max_time
        return self.step_count - before


def bake_wave(fractal, pal_rgba, frames=32, warmup=512, seed=None):
    """WaveTexture -> RGBA frames after exact constant-time simulation."""
    sim = WaterSim(fractal, seed, table=build_wave_lut(fractal["props"]))
    sim.prime()
    for _ in range(warmup):
        sim.tick()
    raw = []
    for _ in range(frames + 8):
        sim.tick()
        raw.append(pal_rgba[sim.out])
    out = []
    for i in range(frames):
        frame = raw[i].astype(np.float32)
        if i < 8:
            alpha = (i + 1) / 9.0
            frame = raw[frames + i].astype(np.float32) * (1 - alpha) + frame * alpha
        out.append(frame.astype(np.uint8))
    return out


def apply_wet(source_rgba, displacement):
    """Exact PostLoad upsample followed by ApplyWetTexture displacement."""
    source_rgba = np.asarray(source_rgba, dtype=np.uint8)
    displacement = np.asarray(displacement, dtype=np.uint8)
    h, w = displacement.shape
    sh, sw = source_rgba.shape[:2]
    if (sw > w or sh > h or sw < 1 or sh < 1
            or w % sw or h % sh):
        raise ValueError(f"wet source cannot upsample {sw}x{sh} to {w}x{h}")
    ys = (np.arange(h) * sh // h)[:, None]
    xs = np.arange(w)[None, :] * sw // w
    expanded_source = source_rgba[ys, xs]
    dest_x = np.arange(w, dtype=np.int32)[None, :]
    dest_y = np.arange(h, dtype=np.int32)[:, None]
    sx = (dest_x + displacement.astype(np.int32)) & (w - 1)
    return expanded_source[dest_y, sx]


def bake_wet(fractal, source_idx, source_rgba, frames=32, warmup=64, seed=None,
             source_oracle=None, rand=None):
    """WetTexture -> RGBA frames using exact ApplyWetTexture displacement."""
    sim = WaterSim(fractal, seed, rand=rand)
    sim.prime((lambda: source_oracle.update(np.float32(0.0)))
              if source_oracle is not None else None)
    for _ in range(warmup):
        if source_oracle is not None:
            source_rgba = source_oracle.tick()
        sim.tick()
    source_idx = np.asarray(source_idx, dtype=np.uint8)
    source_rgba = np.asarray(source_rgba, dtype=np.uint8)
    if source_rgba.shape[:2] != source_idx.shape:
        raise ValueError("wet source index/color dimensions differ")
    raw = []
    for _ in range(frames + 8):
        if source_oracle is not None:
            source_rgba = source_oracle.tick()
        sim.tick()
        raw.append(apply_wet(source_rgba, sim.out))
    out = []
    for i in range(frames):
        frame = raw[i].astype(np.float32)
        if i < 8:
            alpha = (i + 1) / 9.0
            frame = raw[frames + i].astype(np.float32) * (1 - alpha) + frame * alpha
        out.append(frame.astype(np.uint8))
    return out
