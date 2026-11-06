"""Exact shipped-content UE1 FireTexture simulation oracle.

This is the byte-sequential C-path port requested by the runtime catalog.  Its
authority is ``Fire/Src/UnFractal.cpp``: ``RedrawSparks`` (1330),
``CalcWrapFire``/``CalcSlowFire`` (238/354), and ``PostDrawSparks`` (2058).
It intentionally does not replace ``fractalfire.py``'s legacy sheet baker.
"""
import numpy as np

from fractalwater import PHASE, SpeedRand, global_random


(S_BURN, S_SPARKLE, S_PULSE, S_SIGNAL, S_BLAZE, S_OZ, S_CONE,
 S_BLAZE_RIGHT, S_BLAZE_LEFT, S_CYLINDER, S_CYLINDER_3D, S_LISSAJOUS,
 S_JUGGLERS, S_EMIT, S_FOUNTAIN, S_FLOCKS, S_EELS, S_ORGANIC,
 S_WANDER_ORGANIC, S_RANDOM_CLOUD, S_CUSTOM_CLOUD, S_LOCAL_CLOUD,
 S_STARS, S_LINE_LIGHTNING, S_RAMP_LIGHTNING, S_SPHERE_LIGHTNING,
 S_WHEEL, S_GAMETES, S_SPRINKLER) = range(29)

I_DRIFTER, I_DRIFT_SLOW = 32, 33
I_SPAWNED_EEL = 38

SUPPORTED_AUTHORED_TYPES = frozenset({
    S_BURN, S_PULSE, S_SIGNAL, S_BLAZE, S_OZ, S_CYLINDER_3D, S_EELS,
    S_STARS, S_LINE_LIGHTNING, S_RAMP_LIGHTNING, S_SPHERE_LIGHTNING,
})

STAR_CUTOFF = 38


def _sbyte(value):
    value = int(value) & 0xff
    return value - 256 if value >= 128 else value


def _trunc_div(numerator, denominator):
    sign = -1 if numerator < 0 else 1
    return sign * (abs(int(numerator)) // int(denominator))


def _i32(value):
    value = int(value) & 0xffffffff
    return value - 0x100000000 if value & 0x80000000 else value


class FireSim:
    """Exact constant-time FireTexture; ``output`` is the P8 mip-zero field."""

    def __init__(self, fractal, seed=None, rand=None):
        width, height, initial = fractal["mips"][0]
        if (width < 8 or height < 8 or width & (width - 1)
                or height & (height - 1)):
            raise ValueError(
                f"fire dimensions must be power-of-two >= 8: {width}x{height}")
        self.w, self.h = int(width), int(height)
        self.umask, self.vmask = self.w - 1, self.h - 1
        self.output = bytearray(bytes(initial[:self.w * self.h]).ljust(
            self.w * self.h, b"\0"))
        self._initial = bytes(self.output)
        self._authored_sparks = [list(bytes(spark)[:8].ljust(8, b"\0"))
                                 for spark in fractal.get("sparks", [])]
        unsupported = sorted({spark[0] for spark in self._authored_sparks}
                             - SUPPORTED_AUTHORED_TYPES)
        if unsupported:
            raise ValueError(f"unsupported authored fire spark types {unsupported}")

        props = fractal["props"]
        self.rising = bool(props.get("bRising", False))
        self.render_heat = int(props.get("RenderHeat", 0))
        self.sparks_limit = int(props.get("SparksLimit", 0))
        if not 0 <= self.sparks_limit <= 8192:
            raise ValueError(f"invalid SparksLimit {self.sparks_limit}")
        if len(self._authored_sparks) > self.sparks_limit:
            raise ValueError("authored spark array exceeds SparksLimit")
        self.fx_frequency = int(props.get("FX_Frequency", 0)) & 0xff
        self.prime_count = max(0, int(props.get("PrimeCount", 0)))
        self.min_frame_rate = np.float32(props.get("MinFrameRate", 0.0))
        self.max_frame_rate = np.float32(props.get("MaxFrameRate", 0.0))
        self.render_table = bytearray(
            max(0, min(255, int(index / 4.0 + 1.0
                                - (255 - self.render_heat) / 16.0)))
            for index in range(1024))
        self.rand = (rand if rand is not None else
                     SpeedRand(seed) if seed is not None else global_random())
        self.reset()

    def reset(self):
        self.output[:] = self._initial
        self.sparks = [spark.copy() for spark in self._authored_sparks]
        self.aux_phase = 0
        self.global_phase = 0
        self.prime_current = 0
        self.step_count = 0
        self.accumulator = np.float32(0.0)
        self.star_status = True

    def _put(self, x, y, value):
        self.output[(int(y) & self.vmask) * self.w
                    + (int(x) & self.umask)] = int(value) & 0xff

    def _move_spark(self, spark):
        xspeed = _sbyte(spark[4])
        if (self.rand() & 127) < abs(xspeed):
            spark[2] = (spark[2] + (-1 if xspeed < 0 else 1)) & self.umask
        yspeed = _sbyte(spark[5])
        if (self.rand() & 127) < abs(yspeed):
            spark[3] = (spark[3] + (-1 if yspeed < 0 else 1)) & self.vmask

    def _draw_flash_ramp(self, x, y, xlen, ylen, color1, color2):
        x, y, xlen, ylen = (int(v) & 0xff for v in (x, y, xlen, ylen))
        color1, color2 = int(color1) & 0xff, int(color2) & 0xff
        if (((ylen & 1) and ylen * 2 >= xlen)
                or ((xlen & 1) and ylen * 2 < xlen)):
            x = (x + (-xlen if xlen & 1 else xlen)) & 0xff
            y = (y + (-ylen if ylen & 1 else ylen)) & 0xff
            xlen ^= 1
            ylen ^= 1
            color1, color2 = color2, color1

        major = 1 | max(xlen, ylen)
        flash_array = [self.rand() for _ in range(major)]
        flash_position = sum(flash_array)
        ystep, real_ylen = (-1, -ylen) if ylen & 1 else (1, ylen)
        xstep, real_xlen = (-1, -xlen) if xlen & 1 else (1, xlen)
        ramp_color = _i32(color1 << 23)
        color_slope = _trunc_div((color2 - color1) << 23, major)

        def advance_color():
            nonlocal ramp_color
            ramp_color = _i32(ramp_color + color_slope)
            return (ramp_color >> 23) & 0xff

        if xlen >= ylen:
            yposition = y << 6
            bias = _trunc_div(real_ylen * 64 - flash_position, major)
            for flash in range(xlen):
                yposition += flash_array[flash] + bias
                self._put(x, yposition >> 6, advance_color())
                x = (x + xstep) & 0xff
        else:
            xposition = x << 6
            bias = _trunc_div(real_xlen * 64 - flash_position, major)
            for flash in range(ylen):
                xposition += flash_array[flash] + bias
                self._put(xposition >> 6, y, advance_color())
                y = (y + ystep) & 0xff

    def redraw_sparks(self):
        self.aux_phase = (self.aux_phase + self.fx_frequency) & 0xff
        self.global_phase += 1
        index = 0
        while index < len(self.sparks):
            spark = self.sparks[index]
            kind = spark[0]
            kill = False
            if kind == S_BURN:
                self._put(spark[2], spark[3], self.rand())
            elif kind == S_PULSE:
                self._put(spark[2], spark[3], spark[1])
                spark[1] = (spark[1] + spark[7]) & 0xff
            elif kind == S_SIGNAL:
                if spark[1] > spark[6]:
                    self._put(spark[2], spark[3], spark[1])
                spark[1] = (spark[1] + spark[7]) & 0xff
                if spark[1] < spark[7]:
                    spark[1] = self.rand()
            elif kind == S_BLAZE:
                if len(self.sparks) < self.sparks_limit and self.rand() < 128:
                    self.sparks.append([
                        I_DRIFTER, spark[1], spark[2], spark[3],
                        self.rand(), self.rand(), spark[6], spark[7]])
            elif kind == S_OZ:
                if len(self.sparks) < self.sparks_limit and self.rand() < 128:
                    self.sparks.append([
                        I_DRIFT_SLOW, spark[1], spark[2], spark[3],
                        ((self.rand() & 127) - 63) & 0xff, (-127) & 0xff,
                        0, 2])
            elif kind == S_CYLINDER_3D:
                if ((spark[4] + 64) & 0xff) < 128:
                    heat = min(int(PHASE[(spark[4] + 64) & 0xff])
                               + spark[1], 255)
                    x = (spark[2]
                         + (int(PHASE[spark[4]]) * spark[5] >> 8)) & 0xff
                    self._put(x, spark[3], heat)
                spark[4] = (spark[4] + spark[7]) & 0xff
            elif kind == S_EELS:
                if self.rand() < 20 and len(self.sparks) < self.sparks_limit:
                    self.sparks.append([
                        I_SPAWNED_EEL, spark[1],
                        (spark[2] + (self.rand() & 31)) & self.umask,
                        (spark[3] + (self.rand() & 31)) & self.vmask,
                        self.rand(), self.rand(), spark[6], 0])
                # `SpeedRand() & 15 == 15` parses as SpeedRand() & 1 in C.
                if self.rand() & 1:
                    spark[2] = (spark[2] + (self.rand() & 15) - 7) & self.umask
                if self.rand() & 1:
                    spark[3] = (spark[3] + (self.rand() & 15) - 7) & self.vmask
            elif kind == S_STARS:
                self._put(spark[2], spark[3], spark[5])
            elif kind in (S_LINE_LIGHTNING, S_RAMP_LIGHTNING):
                if spark[1] != 0:
                    if spark[6] > 0:
                        spark[6] -= 1
                        self._draw_flash_ramp(
                            spark[2], spark[3], spark[4], spark[5], spark[1],
                            spark[1] if kind == S_LINE_LIGHTNING
                            else spark[1] >> 3)
                    elif self.rand() >= spark[7]:
                        spark[6] = (1 + self.rand()) & 5
            elif kind == S_SPHERE_LIGHTNING:
                if self.rand() >= spark[7]:
                    angle = self.rand()
                    radius = spark[6]
                    xdisp = radius * int(PHASE[angle]) >> 8
                    ydisp = radius * int(PHASE[(angle + 64) & 0xff]) >> 8
                    xlen = xdisp - radius // 2
                    ylen = ydisp - radius // 2
                    xlen = ((-xlen) | 1) if xlen <= 0 else xlen & ~1
                    ylen = ((-ylen) | 1) if ylen <= 0 else ylen & ~1
                    self._draw_flash_ramp(
                        spark[2], spark[3], xlen, ylen,
                        spark[1], spark[1] >> 2)
            elif kind == I_DRIFTER:
                spark[1] = (spark[1] - 5) & 0xff
                if spark[1] < 251:
                    self._put(spark[2], spark[3], spark[1])
                    self._move_spark(spark)
                else:
                    kill = True
            elif kind == I_DRIFT_SLOW:
                spark[1] = (spark[1] - spark[7]) & 0xff
                if spark[1] > spark[7]:
                    self._put(spark[2], spark[3], spark[1])
                    self._move_spark(spark)
                else:
                    kill = True
            elif kind == I_SPAWNED_EEL:
                spark[6] = (spark[6] - 1) & 0xff
                if spark[6] < 255:
                    self._put(spark[2], spark[3], spark[1])
                    self._move_spark(spark)
                else:
                    kill = True
            else:
                raise AssertionError(f"unexpected runtime spark type {kind}")

            if kill:
                self.sparks[index] = self.sparks[-1]
                self.sparks.pop()
            index += 1

    def calculate_wrap_fire(self):
        for y in range(self.h):
            below = (y + 1) & self.vmask
            lower = (y + 2) & self.vmask
            for x in range(self.w):
                total = (self.output[below * self.w + x]
                         + self.output[below * self.w + ((x - 1) & self.umask)]
                         + self.output[below * self.w + ((x + 1) & self.umask)]
                         + self.output[lower * self.w + x])
                self.output[y * self.w + x] = self.render_table[total]

    def calculate_slow_fire(self):
        first_line = bytes(self.output[:self.w])
        for y in range(self.h):
            row = y * self.w
            lower = first_line if y == self.h - 1 else None
            for x in range(self.w):
                lower_value = (lower[x] if lower is not None
                               else self.output[row + self.w + x])
                total = (self.output[row + x]
                         + self.output[row + ((x - 1) & self.umask)]
                         + self.output[row + ((x + 1) & self.umask)]
                         + lower_value)
                self.output[row + x] = self.render_table[total]

    def post_draw_sparks(self):
        if not self.star_status:
            return
        found = False
        for spark in self.sparks:
            if spark[0] != S_STARS:
                continue
            found = True
            pixel = spark[3] * self.w + spark[2]
            spark[5] = self.output[pixel]
            if spark[5] < STAR_CUTOFF:
                self.output[pixel] = spark[4]
        self.star_status = found

    def tick(self, before_step=None):
        if before_step is not None:
            before_step()
        self.redraw_sparks()
        if self.rising:
            self.calculate_wrap_fire()
        else:
            self.calculate_slow_fire()
        self.post_draw_sparks()
        self.step_count += 1

    def prime(self, before_step=None):
        while self.prime_current < self.prime_count:
            self.prime_current += 1
            self.tick(before_step)

    def tick_texture(self, delta_seconds, before_step=None):
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
