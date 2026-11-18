"""UE1 FireTexture engine port + flipbook baker.

Reimplements the retail fire-texture simulation: the heat lookup table,
the wrap/slow fire propagation, the per-spark-type behaviors for every
spark type present in WoT content, a lagged-XOR byte RNG, and the phase
table. Sim state starts from the texture's saved mip0 + saved spark array,
so the bake continues the exact shipped effect. Known approximations
(PORT_NOTES): in-place row updates vectorized per row; the jagged
ramp-line jitter simplified.
"""
import numpy as np

# ESpark — public types 0..28, transients 32..43
S_BURN, S_SPARKLE, S_PULSE, S_SIGNAL, S_BLAZE = 0, 1, 2, 3, 4
S_OZ, S_CONE, S_BLAZERIGHT, S_BLAZELEFT = 5, 6, 7, 8
S_CYL, S_CYL3D, S_LISSAJOUS, S_JUGGLERS, S_EMIT, S_FOUNTAIN = 9, 10, 11, 12, 13, 14
S_FLOCKS, S_EELS, S_ORGANIC, S_WANDERORGANIC = 15, 16, 17, 18
S_RANDOMCLOUD, S_CUSTOMCLOUD, S_LOCALCLOUD, S_STARS = 19, 20, 21, 22
S_LINELIGHTNING, S_RAMPLIGHTNING, S_SPHERELIGHTNING = 23, 24, 25
S_WHEEL, S_GAMETES, S_SPRINKLER, S_LASTTYPE = 26, 27, 28, 29
I_TRANSIENTS = 31
I_DRIFTER, I_DRIFTSLOW, I_FALLER, I_VSHOOTER = 32, 33, 34, 35
I_DROP, I_MOVE, I_SPAWNEDEEL, I_SPAWNEDTWIRL = 36, 37, 38, 39
I_SPRINKLERTWIRL, I_CUSTOM, I_GRAVITON, I_SPAWNEDSPERM = 40, 41, 42, 43

STAR_CUTOFF = 64  # engine constant
LOWESTGLOW = 10

PHASE = np.array([round(127.45 + 127.5 * np.sin(t / 256.0 * 2 * np.pi))
                  for t in range(256)], dtype=np.int32)
LIGHTPHASE = np.clip(PHASE + 64, 0, 255)  # LIGHTPHASEBIAS


def sbyte(v):
    return v - 256 if v >= 128 else v


class SpeedRand:
    """Lagged-XOR byte RNG."""

    def __init__(self, seed=1):
        rng = np.random.default_rng(seed)
        self.arr = list(rng.integers(0, 256, 64, dtype=np.uint8))
        self.i = 0

    def __call__(self):
        self.i = (self.i + 1) & 63
        j = (self.i + 31) & 63
        self.arr[j] = int(self.arr[j]) ^ int(self.arr[self.i])
        return self.arr[j]


class FireSim:
    def __init__(self, fractal, seed=1):
        w, h, data = fractal["mips"][0]
        self.w, self.h = w, h
        self.ubits = w.bit_length() - 1
        self.umask, self.vmask = w - 1, h - 1
        self.buf = np.frombuffer(bytes(data[:w * h]), dtype=np.uint8
                                 ).reshape(h, w).astype(np.int32).copy()
        p = fractal["props"]
        self.rising = bool(p.get("bRising"))
        self.render_heat = int(p.get("RenderHeat", 0))
        self.sparks_limit = int(p.get("SparksLimit", 0))
        rh = self.render_heat
        self.rt = np.clip(np.arange(1024) / 4.0 + 1.0 - (255 - rh) / 16.0,
                          0, 255).astype(np.uint8)
        # saved sparks (transients were compacted out on save; drop junk)
        self.sparks = [list(s) for s in fractal.get("sparks", [])
                       if s[0] < I_SPAWNEDSPERM + 1]
        self.rand = SpeedRand(seed)
        self.aux_phase = 0
        self.global_phase = 0
        self.fx_freq = int(p.get("FX_Frequency", 0))
        self.star_status = any(s[0] == S_STARS for s in self.sparks)

    # -- spark helpers ------------------------------------------------------
    def put(self, x, y, heat):
        self.buf[y & self.vmask, x & self.umask] = heat & 0xFF

    def move_spark(self, s):
        a, b = sbyte(s[4]), sbyte(s[5])
        if (self.rand() & 127) < abs(a):
            s[2] = (s[2] + (1 if a >= 0 else -1)) & self.umask
        if (self.rand() & 127) < abs(b):
            s[3] = (s[3] + (1 if b >= 0 else -1)) & self.vmask
    # NB: sign picks direction, magnitude is the per-tick probability
    # out of 128

    def move_spark_two(self, s):
        a = sbyte(s[4])
        if (self.rand() & 127) < abs(a):
            s[2] = (s[2] + (1 if a >= 0 else -1)) & self.umask
        s[3] = (s[3] - 2) & self.vmask

    def spawn(self, stype, heat, x, y, a=0, b=0, c=0, d=0):
        if len(self.sparks) < self.sparks_limit:
            self.sparks.append([stype, heat & 0xFF, x & self.umask,
                                y & self.vmask, a & 0xFF, b & 0xFF,
                                c & 0xFF, d & 0xFF])

    def flash_ramp(self, x, y, xlen, ylen, c1, c2):
        """Jagged ramp line (simplified jitter)."""
        xs = -1 if (xlen & 1) else 1
        ys = -1 if (ylen & 1) else 1
        steps = max(xlen, ylen, 1)
        for i in range(steps):
            t = i / steps
            px = int(x + xs * xlen * t) + ((self.rand() & 3) - 1)
            py = int(y + ys * ylen * t)
            self.put(px, py, int(c1 + (c2 - c1) * t))

    # -- one engine tick ----------------------------------------------------
    def redraw_sparks(self):
        self.aux_phase = (self.aux_phase + self.fx_freq) & 0xFF
        self.global_phase += 1
        rnd = self.rand
        i = 0
        while i < len(self.sparks):
            s = self.sparks[i]
            t = s[0]
            kill = False
            if t == S_BURN:
                self.put(s[2], s[3], rnd())
            elif t == S_SPARKLE:
                nx = s[2] + ((rnd() * s[4]) >> 8)
                ny = s[3] + ((rnd() * s[5]) >> 8)
                self.put(nx, ny, s[1])
            elif t == S_PULSE:
                self.put(s[2], s[3], s[1])
                s[1] = (s[1] + s[7]) & 0xFF
            elif t == S_SIGNAL:
                if s[1] > s[6]:
                    self.put(s[2], s[3], s[1])
                nh = (s[1] + s[7]) & 0xFF
                s[1] = rnd() if nh < s[7] else nh
            elif t in (S_CYL, S_CYL3D):
                if t == S_CYL or ((s[4] + 64) & 255) < 128:
                    h = min(int(PHASE[(s[4] + 64) & 255]) + s[1], 255)
                    x = (s[2] + ((int(PHASE[s[4]]) * s[5]) >> 8))
                    self.put(x, s[3], h)
                s[4] = (s[4] + s[7]) & 0xFF
            elif t == S_JUGGLERS:
                h = min(int(PHASE[(s[4] + 64) & 255]) + s[1], 255)
                y = (s[3] + ((int(PHASE[s[4]]) * s[5]) >> 8))
                self.put(s[2], y, h)
                s[4] = (s[4] + s[7]) & 0xFF
            elif t == S_LISSAJOUS:
                h = int(LIGHTPHASE[(s[4] + 64) & 255])
                x = s[2] + ((int(PHASE[s[4]]) * s[1]) >> 8)
                y = s[3] + ((int(PHASE[s[5]]) * s[1]) >> 8)
                self.put(x, y, h)
                s[4] = (s[4] + s[6]) & 0xFF
                s[5] = (s[5] + s[7]) & 0xFF
            elif t == S_BLAZE:
                if rnd() < 128:
                    self.spawn(I_DRIFTER, s[1], s[2], s[3],
                               rnd() - 127, rnd() - 127, 0, 5)
            elif t in (S_BLAZERIGHT, S_BLAZELEFT):
                if rnd() < 128:
                    a = (rnd() >> 1) if t == S_BLAZERIGHT else -(rnd() >> 1)
                    self.spawn(I_DRIFTER, s[1], s[2], s[3], a, rnd() - 127, 0, 5)
            elif t == S_OZ:
                if rnd() < 128:
                    v = rnd() >> 1
                    self.spawn(I_DRIFTER, s[1], s[2], s[3],
                               v if (self.global_phase & 1) else -v,
                               -(rnd() >> 1), 0, 5)
            elif t == S_EMIT:
                if rnd() < 64:
                    self.spawn(I_DRIFTSLOW, s[1], s[2], s[3], s[4], s[5], 0, s[7])
            elif t == S_FOUNTAIN:
                if rnd() < 64:
                    self.spawn(I_GRAVITON, s[1], s[2], s[3], s[4], s[5], 0, s[7])
            elif t == S_ORGANIC:
                if rnd() < 128:
                    self.spawn(I_VSHOOTER, 0,
                               s[2] + ((rnd() * s[6]) >> 8),
                               s[3] + ((rnd() * s[6]) >> 8),
                               rnd() - 127, 256 - 127, 255, 0)
            elif t == S_STARS:
                self.put(s[2], s[3], s[5])  # restore saved background
            elif t in (S_LINELIGHTNING, S_RAMPLIGHTNING):
                if s[1]:
                    if s[6] > 0:
                        s[6] -= 1
                        self.flash_ramp(s[2], s[3], s[4], s[5], s[1],
                                        s[1] if t == S_LINELIGHTNING else 0)
                    elif rnd() >= s[7]:
                        s[6] = 1 + (rnd() & 3)
            # ---- transients ----
            elif t == I_DRIFTER:
                s[1] = (s[1] - 5) & 0xFF
                if s[1] < 251:
                    self.put(s[2], s[3], s[1])
                    self.move_spark(s)
                else:
                    kill = True
            elif t == I_DRIFTSLOW:
                s[1] = (s[1] - s[7]) & 0xFF
                if s[1] > s[7]:
                    self.put(s[2], s[3], s[1])
                    self.move_spark(s)
                else:
                    kill = True
            elif t == I_GRAVITON:
                s[1] = (s[1] - s[7]) & 0xFF
                if s[1] > LOWESTGLOW:
                    self.put(s[2], s[3], s[1])
                    self.move_spark(s)
                    if self.global_phase & 1 and sbyte(s[5]) < 124:
                        s[5] = (s[5] + 3) & 0xFF
                else:
                    kill = True
            elif t == I_VSHOOTER:
                s[6] = (s[6] - 3) & 0xFF
                if s[6] > 190:
                    self.put(s[2], s[3], s[6])
                    self.move_spark_two(s)
                else:
                    kill = True
            # unimplemented exotic types tick as no-ops (logged by baker)
            if kill:
                self.sparks[i] = self.sparks[-1]
                self.sparks.pop()
            else:
                i += 1

    def post_draw_sparks(self):
        if not self.star_status:
            return
        found = False
        for s in self.sparks:
            if s[0] == S_STARS:
                found = True
                bg = int(self.buf[s[3], s[2]])
                s[5] = bg
                if bg < STAR_CUTOFF:
                    self.buf[s[3], s[2]] = s[4]
        self.star_status = found

    def propagate(self):
        b, rt, w, h = self.buf, self.rt, self.w, self.h
        if self.rising:  # wrap fire: vertical wrap
            src = b.copy()
            for y in range(h):
                below = src[(y + 1) % h]
                lower = src[(y + 2) % h] if y < h - 2 else b[(y + 2) % h]
                s = below + np.roll(below, 1) + np.roll(below, -1) + lower
                b[y] = rt[s]
        else:  # slow fire: reads own row + one below, top line feeds last
            top = b[0].copy()
            src = b.copy()
            for y in range(h - 1):
                cur = src[y]
                s = cur + np.roll(cur, 1) + np.roll(cur, -1) + src[y + 1]
                b[y] = rt[s]
            cur = src[h - 1]
            b[h - 1] = rt[cur + np.roll(cur, 1) + np.roll(cur, -1) + top]

    def tick(self):
        self.redraw_sparks()
        self.propagate()
        self.post_draw_sparks()

    def unsupported_types(self):
        impl = {S_BURN, S_SPARKLE, S_PULSE, S_SIGNAL, S_CYL, S_CYL3D,
                S_JUGGLERS, S_LISSAJOUS, S_BLAZE, S_BLAZERIGHT, S_BLAZELEFT,
                S_OZ, S_EMIT, S_FOUNTAIN, S_ORGANIC, S_STARS,
                S_LINELIGHTNING, S_RAMPLIGHTNING,
                I_DRIFTER, I_DRIFTSLOW, I_GRAVITON, I_VSHOOTER}
        return sorted({s[0] for s in self.sparks} - impl)


def bake(fractal, palette, frames=32, warmup=128, seed=1):
    """→ (list of RGBA frames as np arrays, unsupported spark types)."""
    sim = FireSim(fractal, seed)
    for _ in range(warmup):
        sim.tick()
    pal = np.array([c[:3] for c in palette], dtype=np.uint8)
    raw = []
    for _ in range(frames + 8):     # extra tail for loop crossfade
        sim.tick()
        raw.append(sim.buf.astype(np.uint8).copy())
    # crossfade last 8 into first 8 for a seamless loop
    out = []
    for i in range(frames):
        f = raw[i].astype(np.float32)
        if i < 8:
            t = (i + 1) / 9.0
            f = raw[frames + i].astype(np.float32) * (1 - t) + f * t
        idx = f.astype(np.uint8)
        rgb = pal[idx]
        a = idx[..., None]  # heat as alpha (index 0 = transparent/black)
        out.append(np.concatenate([rgb, a], axis=-1))
    return out, sim.unsupported_types()
