"""Pure-Python pins for the G45 world-surface meshel reconstruction.

The HLSL cannot run here, so ``_reconstruct`` mirrors its arithmetic line for
line (the string pins below keep the two in step) and is checked against the
exact UE1 model: the spatial function evaluated at lightmap meshel centres
(SPATIAL_BEGIN1 / spatial_NonIncidence) and bilinearly filtered.
"""

import math
import random
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import dynamic_light_material as dlm  # noqa: E402


def _f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _f16(value):
    return struct.unpack("<e", struct.pack("<e", value))[0]


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def _mul(a, s):
    return tuple(x * s for x in a)


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _norm(a):
    return math.sqrt(_dot(a, a))


def _atten(corner, light, radius, shape, normal):
    """One corner of the HLSL light loop (NonIncidence / Incidence / Shell)."""
    lw = _sub(light, corner)
    d = _norm(lw)
    s = d / radius
    atten_n = min(max(1.02 - s, 0.0), 1.0) if s * s * 4093.0 < 4095.5 else 0.0
    cos_f = max(0.0, _dot(normal, _mul(lw, 1.0 / max(d, 0.001))))
    atten_i = (cos_f * (1.0 - s) * (1.0 - s) * (2.0 * s + 1.0)
               if s < 1.0 else 0.0)
    atten_s = 1.0 - 10.0 * abs(s - 0.9) if 0.8 < s < 1.0 else 0.0
    if shape > 1.5:
        return atten_s
    shape = min(max(shape, 0.0), 1.0)
    return atten_n + (atten_i - atten_n) * shape


def _ue1_shell(dist):
    """UE1's FLightManager::spatial_Shell, Dist = |Vertex| * RRadius."""
    if dist >= 1.0 or dist <= 0.8:
        return 0.0
    return 1.0 - 10.0 * abs(dist - 0.9)


def _reconstruct(wp, q, dqx, dqy, dwx, dwy, light, radius, shape, normal):
    """Line-for-line mirror of _DYNAMIC_MESHEL_HLSL for one light."""
    q0 = (math.floor(q[0]), math.floor(q[1]))
    f = (q[0] - q0[0], q[1] - q0[1])
    det = dqx[0] * dqy[1] - dqx[1] * dqy[0]
    qulp = 2.0 ** (math.floor(math.log2(max(abs(q[0]), abs(q[1]), 1.0))) - 23.0)
    fn = (min(f[0], 1.0 - f[0]), min(f[1], 1.0 - f[1]))
    on_meshel = fn[0] <= 16.0 * qulp and fn[1] <= 16.0 * qulp
    lattice = (not on_meshel
               and abs(det) > 1.0e-4 * (_dot(dqx, dqx) + _dot(dqy, dqy)))
    if lattice:
        ju = _mul(_sub(_mul(dwx, dqy[1]), _mul(dwy, dqx[1])), 1.0 / det)
        jv = _mul(_sub(_mul(dwy, dqx[0]), _mul(dwx, dqy[0])), 1.0 / det)
        base = _sub(_sub(wp, _mul(ju, f[0])), _mul(jv, f[1]))
        corners = (base, _add(base, ju), _add(base, jv), _add(_add(base, ju), jv))
        weights = ((1 - f[0]) * (1 - f[1]), f[0] * (1 - f[1]),
                   (1 - f[0]) * f[1], f[0] * f[1])
    else:
        corners = (wp, wp, wp, wp)
        weights = (1.0, 0.0, 0.0, 0.0)
    return sum(w * _atten(c, light, radius, shape, normal)
               for c, w in zip(corners, weights))


def _cubic_camera_position(a, b, alpha):
    """The shot director's two-key, endpoint-clamped cubic interpolation."""
    tangent = _mul(_sub(b, a), 0.5)
    a2, a3 = alpha * alpha, alpha * alpha * alpha
    return tuple((2 * a3 - 3 * a2 + 1) * p0
                 + (a3 - 2 * a2 + alpha) * t0
                 + (-2 * a3 + 3 * a2) * p1
                 + (a3 - a2) * t1
                 for p0, t0, p1, t1 in zip(a, tangent, b, tangent))


def _shot_alpha(timeline_seconds, duration):
    raw = min(max(timeline_seconds / duration, 0.0), 1.0)
    return (4.0 * raw ** 3 if raw < 0.5
            else 1.0 - (-2.0 * raw + 2.0) ** 3 / 2.0)


def _project_triangle(positions, camera, pitch, yaw, horizontal_fov,
                      width, height):
    """Project UE coordinates with the director's true horizontal FOV."""
    pitch, yaw = math.radians(pitch), math.radians(yaw)
    forward = (math.cos(pitch) * math.cos(yaw),
               math.cos(pitch) * math.sin(yaw), math.sin(pitch))
    right = (-math.sin(yaw), math.cos(yaw), 0.0)
    up = (-math.sin(pitch) * math.cos(yaw),
          -math.sin(pitch) * math.sin(yaw), math.cos(pitch))
    tan_half = math.tan(math.radians(horizontal_fov) * 0.5)
    aspect = width / height
    result = []
    for position in positions:
        delta = _sub(position, camera)
        depth = _dot(delta, forward)
        ndc_x = _dot(delta, right) / (depth * tan_half)
        ndc_y = _dot(delta, up) / (depth * tan_half / aspect)
        result.append(((ndc_x + 1.0) * width * 0.5,
                       (1.0 - ndc_y) * height * 0.5, depth))
    return result


def _gpu_quad_inputs(positions, uv1, screen, pixel, atlas=1024.0,
                     half_uv=False):
    """Interpolate one D3D pixel quad, including same-primitive helper lanes.

    HLSL ``ddx``/``ddy`` use the quad's top row/left column coarse differences.
    Uncovered lanes still run the primitive's interpolation plane as helper
    lanes, so barycentrics deliberately extrapolate outside the triangle.
    """
    if half_uv:
        uv1 = [tuple(_f16(value) for value in item) for item in uv1]

    def sample(x, y):
        px, py = x + 0.5, y + 0.5
        x0, y0, z0 = screen[0]
        x1, y1, z1 = screen[1]
        x2, y2, z2 = screen[2]
        den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        b0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den
        b1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den
        b2 = 1.0 - b0 - b1
        weights = [b0 / z0, b1 / z1, b2 / z2]
        total = sum(weights)
        weights = [item / total for item in weights]
        uv = tuple(sum(weights[i] * uv1[i][axis] for i in range(3))
                   for axis in range(2))
        wp = tuple(_f32(sum(weights[i] * positions[i][axis]
                              for i in range(3))) for axis in range(3))
        q = tuple(_f32(_f32(uv[axis] * atlas) - 0.5)
                  for axis in range(2))
        return q, wp

    x, y = pixel
    left, top = x & ~1, y & ~1
    lanes = {(xx, yy): sample(xx, yy)
             for yy in (top, top + 1) for xx in (left, left + 1)}
    q, wp = lanes[(x, y)]
    dqx = tuple(_f32(b - a) for a, b in zip(
        lanes[(left, top)][0], lanes[(left + 1, top)][0]))
    dqy = tuple(_f32(b - a) for a, b in zip(
        lanes[(left, top)][0], lanes[(left, top + 1)][0]))
    dwx = tuple(_f32(b - a) for a, b in zip(
        lanes[(left, top)][1], lanes[(left + 1, top)][1]))
    dwy = tuple(_f32(b - a) for a, b in zip(
        lanes[(left, top)][1], lanes[(left, top + 1)][1]))
    return wp, q, dqx, dqy, dwx, dwy


def _quad_result(inputs, light, radius, normal):
    wp, q, dqx, dqy, dwx, dwy = inputs
    det = dqx[0] * dqy[1] - dqx[1] * dqy[0]
    threshold = 1.0e-4 * (_dot(dqx, dqx) + _dot(dqy, dqy))
    value = _reconstruct(wp, q, dqx, dqy, dwx, dwy,
                         light, radius, 0.0, normal)
    return SimpleNamespace(value=value, det=det, threshold=threshold,
                           lattice=abs(det) > threshold,
                           per_pixel=_atten(wp, light, radius, 0.0, normal))


class _Surface:
    """A UE1 lightmapped polygon: meshel (u,v) at O + u*A + v*B, packed in
    the atlas at integer texel origin (ox, oy) (lightmap_atlas.py)."""

    def __init__(self, origin, axis_u, axis_v, atlas_origin):
        self.o, self.a, self.b = origin, axis_u, axis_v
        self.ox, self.oy = atlas_origin
        n = (axis_u[1] * axis_v[2] - axis_u[2] * axis_v[1],
             axis_u[2] * axis_v[0] - axis_u[0] * axis_v[2],
             axis_u[0] * axis_v[1] - axis_u[1] * axis_v[0])
        self.normal = _mul(n, 1.0 / _norm(n))

    def world(self, u, v):
        return _add(_add(self.o, _mul(self.a, u)), _mul(self.b, v))

    def q(self, u, v):
        return (self.ox + u, self.oy + v)

    def exact(self, u, v, light, radius, shape):
        """UE1: evaluate at the four meshel centres, bilinear filter."""
        u0, v0 = math.floor(u), math.floor(v)
        fu, fv = u - u0, v - v0
        total = 0.0
        for du, dv, w in ((0, 0, (1 - fu) * (1 - fv)), (1, 0, fu * (1 - fv)),
                          (0, 1, (1 - fu) * fv), (1, 1, fu * fv)):
            total += w * _atten(self.world(u0 + du, v0 + dv), light, radius,
                                shape, self.normal)
        return total


class DynamicMeshelContractTests(unittest.TestCase):
    def test_world_graph_version_bump_forces_master_regeneration(self):
        self.assertEqual(dlm.DYNAMIC_LIGHT_GRAPH_VERSION, "g45-v3-shell")

    def test_hlsl_pins_the_lattice_and_ue1_table_bound(self):
        hlsl = dlm._DYNAMIC_MESHEL_HLSL
        for line in (
                "float2 q = UV1 * AtlasSize - 0.5;",
                "float2 dqx = ddx(q);",
                "float3 dwx = ddx(WPRel);",
                "float det = dqx.x * dqy.y - dqx.y * dqy.x;",
                "float3 Ju = (dwx * dqy.y - dwy * dqx.y) / det;",
                "float3 Jv = (dwy * dqx.x - dwx * dqy.x) / det;",
                "float3 base = WP - Ju * f.x - Jv * f.y;",
                "weight[3] = f.x * f.y;",
                "(s * s * 4093.0 < 4095.5) ? saturate(1.02 - s) : 0.0",
                "bool onMeshel = fn.x <= 16.0 * qulp && fn.y <= 16.0 * qulp;",
                "&& abs(det) > 1.0e-4 * (dot(dqx, dqx) + dot(dqy, dqy));",
                "float attenS = (s > 0.8 && s < 1.0) ? 1.0 - 10.0 * abs(s - 0.9) : 0.0;",
                "atten += weight[k] * ((shape > 1.5) ? attenS",
                ": lerp(attenN, attenI, saturate(shape)));",
                "return sum * DynEnable;"):
            self.assertIn(line, hlsl)
        # Movers/meshes keep the per-pixel graph (now with the shell branch).
        surface = dlm._DYNAMIC_SURFACE_HLSL
        for line in (
                "float3 Lw = pr[i].xyz - WP;",
                "if (s >= 1.0) continue;",
                "float attenS = (s > 0.8) ? 1.0 - 10.0 * abs(s - 0.9) : 0.0;",
                "float atten = (cl[i].a > 1.5) ? attenS"
                " : lerp(attenN, attenI, saturate(cl[i].a));"):
            self.assertIn(line, surface)

    def test_shell_lights_only_the_outer_band_like_ue1(self):
        # AMAPearl's LE_Shell: the port drew it as NonIncidence, lighting the
        # whole radius at up to full brightness (the washed-out pink room).
        normal, light, radius = (0.0, 0.0, 1.0), (0.0, 0.0, 0.0), 625.0
        for i in range(0, 1101):
            s = i / 1000.0
            corner = (s * radius, 0.0, 0.0)
            self.assertAlmostEqual(
                _atten(corner, light, radius, 2.0, normal), _ue1_shell(s),
                places=9, msg=f"s={s}")
        inside = (0.5 * radius, 0.0, 0.0)
        self.assertEqual(_atten(inside, light, radius, 2.0, normal), 0.0)
        self.assertGreater(_atten(inside, light, radius, 0.0, normal), 0.5)
        self.assertAlmostEqual(
            _atten((0.9 * radius, 0.0, 0.0), light, radius, 2.0, normal), 1.0)

    def test_reconstruction_equals_ue1_meshel_filter_exactly(self):
        rng = random.Random(7)
        surface = _Surface(
            origin=(120.0, -40.0, 16.0),
            axis_u=(30.0, 8.0, 0.0), axis_v=(-8.0, 30.0, 0.0),
            atlas_origin=(711, 402))
        light = (400.0, 320.0, 60.0)
        for shape in (0.0, 1.0, 2.0):
            for _ in range(400):
                u, v = rng.uniform(0, 14), rng.uniform(0, 14)
                # Screen steps = arbitrary affine directions on the plane.
                du1, dv1 = rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3)
                du2, dv2 = rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3)
                if abs(du1 * dv2 - dv1 * du2) < 1e-3:
                    continue
                wp = surface.world(u, v)
                got = _reconstruct(
                    wp, surface.q(u, v), (du1, dv1), (du2, dv2),
                    _sub(surface.world(u + du1, v + dv1), wp),
                    _sub(surface.world(u + du2, v + dv2), wp),
                    light, 300.0, shape, surface.normal)
                want = surface.exact(u, v, light, 300.0, shape)
                self.assertAlmostEqual(got, want, places=9)

    def test_arena_v3_real_skinny_edge_half_uvs_drop_the_explosion_light(self):
        # Arena_02 part 0 / section 5 / surface 1790 / source triangle 807,
        # sampled at the independently measured frame-155 ribbon pixel. The
        # GreenExplode is spawned at the Seeker HitLocation near p0's shot
        # anchor (-950,100,-280); its authored radius 12 becomes 25*(12+1).
        positions = [(-911.5, 192.0, -323.804),
                     (-908.0, 192.0, -324.261),
                     (-911.5, -135.529, -312.5)]
        uv1 = [(0.289611757, 0.164587036),
               (0.289546162, 0.164558545),
               (0.284671724, 0.175289348)]
        video_seconds = 155.0 / 60.0
        alpha = _shot_alpha(video_seconds + 1.5, 5.8)
        camera = _cubic_camera_position(
            (-680.0, 0.0, -245.0), (-700.0, -10.0, -247.0), alpha)
        screen = _project_triangle(
            positions, camera, -3.0 + alpha, 180.0, 80.0, 960, 540)
        light, radius = (-950.0, 100.0, -280.0), 325.0
        normal = (0.129, 0.034, 0.991)
        full = _quad_result(_gpu_quad_inputs(
            positions, uv1, screen, (400, 438)), light, radius, normal)
        half = _quad_result(_gpu_quad_inputs(
            positions, uv1, screen, (400, 438), half_uv=True),
            light, radius, normal)

        # The first two vertices are different in the payload but identical in
        # the old render buffer. The bogus inverse passes the existing relative
        # determinant guard and places all four samples tens of thousands of
        # units away, so this nonnegative light term becomes exactly zero.
        self.assertNotEqual(uv1[0], uv1[1])
        self.assertEqual(tuple(_f16(v) for v in uv1[0]),
                         tuple(_f16(v) for v in uv1[1]))
        self.assertTrue(full.lattice)
        self.assertGreater(full.det, 2.0e-4)
        self.assertGreater(full.value, 0.70)
        self.assertTrue(half.lattice)
        self.assertGreater(abs(half.det), half.threshold)
        self.assertLess(abs(half.det), 5.0e-8)
        self.assertGreater(half.per_pixel, 0.70)
        self.assertEqual(half.value, 0.0)

    def test_a2_pre_recut_fans_have_the_same_half_uv_jacobian_failure(self):
        # The real source triangles hit by the five A2 fan witnesses before
        # 891254ff2. FireballExplode3 is NonIncidence, brightness 255, radius
        # 12 -> 325uu and is spawned at the impact near p1's setup position.
        alpha = _shot_alpha(2.20 + 0.75, 5.6)
        camera = _cubic_camera_position(
            (322.0, -652.0, 82.0), (328.0, -620.0, 80.0), alpha)
        pitch, fov = 6.0 + 0.5 * alpha, 66.0 - 5.0 * alpha
        light, radius = (352.83, -158.82, 100.0), 325.0
        normal = (0.0, -1.0, 0.0)
        failing = (
            (3045, (1198, 838),
             [(435.106, -272.0, 152.0), (441.336, -272.0, 124.706),
              (429.553, -272.0, 149.174)],
             [(0.449124396, 0.228966877), (0.448905468, 0.229862973),
              (0.449319541, 0.229059651)]),
            (3005, (2194, 687),
             [(315.440, -272.0, 166.828), (315.152, -272.0, 165.865),
              (256.0, -272.0, 158.129)],
             [(0.453329533, 0.228480071), (0.453339636, 0.228511691),
              (0.455418259, 0.228765666)]),
            (3255, (2727, 460),
             [(251.890, -272.0, 190.908), (252.934, -272.0, 193.074),
              (256.0, -272.0, 152.0)],
             [(0.455562681, 0.227689475), (0.455526024, 0.227618352),
              (0.455418259, 0.228966877)]),
            (2992, (2520, 1274),
             [(277.156, -272.0, 97.876), (279.108, -272.0, 97.876),
              (275.0, -272.0, 32.0)],
             [(0.454674840, 0.230743855), (0.454606235, 0.230743855),
              (0.454750597, 0.232906669)]),
        )
        for triangle, pixel, positions, uv1 in failing:
            with self.subTest(triangle=triangle):
                screen = _project_triangle(
                    positions, camera, pitch, 86.5, fov, 3840, 2160)
                full = _quad_result(_gpu_quad_inputs(
                    positions, uv1, screen, pixel), light, radius, normal)
                half = _quad_result(_gpu_quad_inputs(
                    positions, uv1, screen, pixel, half_uv=True),
                    light, radius, normal)
                self.assertTrue(full.lattice)
                self.assertGreater(full.value, 0.45)
                self.assertGreater(half.per_pixel, 0.45)
                self.assertTrue(half.lattice)
                self.assertEqual(half.value, 0.0)

        # Triangle 3037 lands on the other possible outcome of the same lost
        # information: its rounded UV triangle is exactly rank-one in this
        # quad and takes the singular per-pixel fallback. Full precision keeps
        # the intended, well-conditioned meshel lattice on all five fans.
        positions = [(381.273, -272.0, 180.265),
                     (378.515, -272.0, 186.092),
                     (389.961, -272.0, 176.081)]
        uv1 = [(0.451016098, 0.228038907),
               (0.451113015, 0.227847591),
               (0.450710803, 0.228176266)]
        screen = _project_triangle(
            positions, camera, pitch, 86.5, fov, 3840, 2160)
        full = _quad_result(_gpu_quad_inputs(
            positions, uv1, screen, (1614, 580)), light, radius, normal)
        half = _quad_result(_gpu_quad_inputs(
            positions, uv1, screen, (1614, 580), half_uv=True),
            light, radius, normal)
        self.assertTrue(full.lattice)
        self.assertGreater(full.value, 0.5)
        self.assertFalse(half.lattice)
        self.assertAlmostEqual(half.value, half.per_pixel)

    def test_cutoff_edge_is_a_one_meshel_ramp_not_a_step(self):
        # Walk across the NonIncidence cutoff along u: per pixel the 0.02
        # step is a discontinuity; per meshel + bilinear it is continuous.
        surface = _Surface((0.0, 0.0, 0.0), (32.0, 0.0, 0.0),
                           (0.0, 32.0, 0.0), (100, 100))
        light, radius = (0.0, 0.0, 0.0), 200.0
        values = []
        u = 5.0
        while u < 7.5:
            wp = surface.world(u, 0.3)
            values.append(_reconstruct(
                wp, surface.q(u, 0.3), (0.01, 0.0), (0.0, 0.01),
                _sub(surface.world(u + 0.01, 0.3), wp),
                _sub(surface.world(u, 0.31), wp),
                light, radius, 0.0, surface.normal))
            u += 0.01
        steps = [abs(b - a) for a, b in zip(values, values[1:])]
        self.assertLess(max(steps), 0.01 * 0.25)   # no 0.02 jump anywhere
        self.assertGreater(values[0], 0.0)
        self.assertEqual(values[-1], 0.0)

    def test_fp32_lattice_derivatives_residual_is_bounded(self):
        # Realistic magnitudes: q near the top of a 4096 atlas, UV1 rounded to
        # float32, a wall 263.9uu away (1920 px, 90 deg FOV), samples in the
        # +-40uu band around the cutoff (the worst region). The derivative
        # Jacobian inherits UV1's float32 rounding (PORT_NOTES residual), so
        # pin the measured bound and that it beats per-pixel evaluation by an
        # order of magnitude; a wrong Jacobian blows straight through both.
        surface = _Surface((0.0, 0.0, 0.0), (32.0, 0.0, 0.0),
                           (0.0, 32.0, 0.0), (3900, 3000))
        light, radius, width = (130.0, 90.0, 40.0), 200.0, 4096.0
        footprint = 2.0 * 263.9 / 1920.0
        levels = 254.0 / 63.75 * 0.5 * 255.0   # Tex 0.5, FloatColor 1
        rng = random.Random(3)
        recon, per_pixel = [], []

        def q32(x, y):
            uv = (_f32((3900 + x / 32.0 + 0.5) / width),
                  _f32((3000 + y / 32.0 + 0.5) / width))
            return (_f32(uv[0] * width) - 0.5, _f32(uv[1] * width) - 0.5)

        for _ in range(1000):
            angle = rng.uniform(0.0, 2.0 * math.pi)
            rr = math.sqrt(radius * radius - 40.0 * 40.0) + rng.uniform(-40, 40)
            px = light[0] + rr * math.cos(angle)
            py = light[1] + rr * math.sin(angle)
            q = q32(px, py)
            wp = (px, py, 0.0)
            got = _reconstruct(
                wp, q, _sub(q32(px + footprint, py), q),
                _sub(q32(px, py + footprint), q), (footprint, 0.0, 0.0),
                (0.0, footprint, 0.0), light, radius, 0.0, surface.normal)
            want = surface.exact(px / 32.0, py / 32.0, light, radius, 0.0)
            recon.append(abs(got - want) * levels)
            per_pixel.append(abs(_atten(wp, light, radius, 0.0,
                                        surface.normal) - want) * levels)
        recon.sort()
        per_pixel.sort()
        # Measured over seeds 0-5: p90 0.09-0.11, p99 0.9-3.0, max 5.7-7.3
        # levels. The max is one corner's 0.02 NonIncidence step flipping
        # under Jacobian rounding, bounded by 0.02 * levels (~10.2).
        self.assertLess(recon[900], 0.5)
        self.assertLess(recon[989], 4.0)
        self.assertLessEqual(max(recon), 0.02 * levels + 0.5)
        self.assertGreater(per_pixel[989], 15.0)  # the old crisp ring

    def test_constant_uv1_fullbright_slot_keeps_per_pixel_evaluation(self):
        # Fullbright slots put every vertex on one atlas texel centre
        # (bsp2obj: UV1 = (fb.x + 0.5) / W), so q is an integer lattice point.
        normal = (0.0, 0.0, 1.0)
        wp, light = (10.0, 20.0, 0.0), (60.0, 20.0, 30.0)
        want = _atten(wp, light, 150.0, 0.0, normal)
        got = _reconstruct(wp, (2049.0, 17.0), (0.0, 0.0), (0.0, 0.0),
                           (0.1, 0.0, 0.0), (0.0, 0.1, 0.0), light, 150.0,
                           0.0, normal)
        self.assertAlmostEqual(got, want)
        # Interpolation rounding leaves a few ulps of noise on q and its
        # derivatives; a noise Jacobian must never be inverted.
        ulp = 2.0 ** -12   # float32 spacing at 2048..4095
        got = _reconstruct(wp, (2049.0 + 3 * ulp, 17.0 - 2 * ulp / 128),
                           (ulp, 0.0), (0.0, -ulp), (0.1, 0.0, 0.0),
                           (0.0, 0.1, 0.0), light, 150.0, 0.0, normal)
        self.assertAlmostEqual(got, want)


class _Node:
    def __init__(self, cls, x, y):
        self.cls, self.x, self.y = cls, x, y
        self.props = {}

    def set_editor_property(self, name, value):
        self.props[name] = value

    def get_editor_property(self, name):
        return self.props.get(name)


def _fake_unreal(existing_collection_names):
    edges = []
    classes = {}

    def cls(name):
        return classes.setdefault(name, type(name, (), {}))

    class _Row:
        def __init__(self, name):
            self.name = name

        def get_editor_property(self, prop):
            return self.name

    collection = SimpleNamespace(get_editor_property=lambda prop: (
        [_Row(n) for n in existing_collection_names]
        if prop == "vector_parameters" else []))

    def connect(src, src_pin, dst, dst_pin):
        edges.append((src, src_pin, dst, dst_pin))
        return True

    unreal = SimpleNamespace(
        EditorAssetLibrary=SimpleNamespace(
            does_asset_exist=lambda path: True,
            save_asset=lambda path: True),
        load_asset=lambda path: collection,
        MaterialEditingLibrary=SimpleNamespace(
            connect_material_expressions=connect),
        CustomInput=lambda: _Node("CustomInput", 0, 0),
        CustomMaterialOutputType=SimpleNamespace(CMOT_FLOAT3="CMOT_FLOAT3"),
        WorldPositionIncludedOffsets=SimpleNamespace(
            WPT_CAMERA_RELATIVE="WPT_CAMERA_RELATIVE"),
        MaterialExposedTextureProperty=SimpleNamespace(
            TMTM_TEXTURE_SIZE="TMTM_TEXTURE_SIZE"),
    )
    for name in ("MaterialExpressionCustom", "MaterialExpressionWorldPosition",
                 "MaterialExpressionVertexNormalWS",
                 "MaterialExpressionTextureCoordinate",
                 "MaterialExpressionTextureObjectParameter",
                 "MaterialExpressionTextureProperty",
                 "MaterialExpressionScalarParameter",
                 "MaterialExpressionCollectionParameter",
                 "MaterialExpressionMultiply", "MaterialExpressionAdd",
                 "MaterialExpressionConstant", "MaterialExpressionMin"):
        setattr(unreal, name, cls(name))
    return unreal, edges


class DynamicMeshelGraphWiringTests(unittest.TestCase):
    def _build(self, texture="T_WoT_Neutral"):
        unreal, edges = _fake_unreal(dlm.dynamic_vector_names())
        nodes = []

        def expr(cls, x, y):
            node = _Node(cls.__name__, x, y)
            nodes.append(node)
            return node

        lightmap = _Node("MaterialExpressionTextureSampleParameter2D", 0, 0)
        lightmap.set_editor_property("parameter_name", "Lightmap")
        lightmap.set_editor_property("texture", texture)
        lightmap.set_editor_property("sampler_type", "LINEAR_COLOR")
        decoded = _Node("MaterialExpressionMultiply", 0, 0)
        out = dlm.build_dynamic_world_factor(
            unreal, object(), expr, decoded, "", -180, 300,
            lightmap_parameter=lightmap)
        return nodes, edges, out

    def test_meshel_inputs_are_wired_from_the_atlas_lattice(self):
        nodes, edges, out = self._build()
        custom = next(n for n in nodes if n.cls == "MaterialExpressionCustom")
        self.assertEqual(custom.props["code"], dlm._DYNAMIC_MESHEL_HLSL)
        names = [item.props["input_name"] for item in custom.props["inputs"]]
        self.assertEqual(names[:6], ["WP", "WPRel", "Nrm", "UV1", "AtlasSize",
                                     "DynEnable"])
        self.assertEqual(len(names), 6 + 2 * dlm.DYNAMIC_LIGHT_SLOTS)
        wired = {dst_pin: src for src, _sp, dst, dst_pin in edges
                 if dst is custom}
        self.assertEqual(set(wired), set(names))
        self.assertEqual(wired["WPRel"].props["world_position_shader_offset"],
                         "WPT_CAMERA_RELATIVE")
        self.assertNotIn("world_position_shader_offset", wired["WP"].props)
        self.assertEqual(wired["UV1"].props["coordinate_index"], 1)
        size = wired["AtlasSize"]
        self.assertEqual(size.cls, "MaterialExpressionTextureProperty")
        self.assertEqual(size.props["property"], "TMTM_TEXTURE_SIZE")
        atlas = next(src for src, _sp, dst, _dp in edges if dst is size)
        self.assertEqual(atlas.cls, "MaterialExpressionTextureObjectParameter")
        # Same name AND same default as the sampler: one shared parameter.
        self.assertEqual(atlas.props["parameter_name"], "Lightmap")
        self.assertEqual(atlas.props["texture"], "T_WoT_Neutral")
        self.assertEqual(atlas.props["sampler_type"], "LINEAR_COLOR")
        self.assertEqual(out.cls, "MaterialExpressionMin")

    def test_missing_linear_default_fails_loudly(self):
        with self.assertRaisesRegex(RuntimeError, "linear default"):
            self._build(texture=None)



class ClearMaterialGraphTests(unittest.TestCase):
    """The rebuild clear must not inherit the engine's skip-every-other bug."""

    def _fake(self, count):
        material = SimpleNamespace(
            expressions=[f"E{i}" for i in range(count)],
            get_path_name=lambda: "/Game/WoT/Materials/M_Test")

        class EngineLikeLibrary:
            @staticmethod
            def get_material_expressions(m):
                return list(m.expressions)   # the Python binding copies

            @staticmethod
            def delete_material_expression(m, expression):
                m.expressions.remove(expression)

            @staticmethod
            def delete_all_material_expressions(m):
                # MaterialEditingLibrary.cpp:559-567: a range-for over the
                # live collection that each delete shrinks.
                index = 0
                while index < len(m.expressions):
                    EngineLikeLibrary.delete_material_expression(
                        m, m.expressions[index])
                    index += 1

        return material, SimpleNamespace(
            MaterialEditingLibrary=EngineLikeLibrary)

    def test_engine_bulk_delete_leaves_orphans(self):
        material, unreal = self._fake(9)
        unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
        self.assertEqual(material.expressions, ["E1", "E3", "E5", "E7"])

    def test_clear_removes_every_expression(self):
        material, unreal = self._fake(9)
        dlm.clear_material_graph(unreal, material)
        self.assertEqual(material.expressions, [])

    def test_clear_fails_loudly_when_an_expression_survives(self):
        material, unreal = self._fake(3)
        unreal.MaterialEditingLibrary.delete_material_expression = (
            lambda m, e: None)
        with self.assertRaisesRegex(RuntimeError, "survived"):
            dlm.clear_material_graph(unreal, material)


if __name__ == "__main__":
    unittest.main()
