"""Corona show/hide mismatch sweep: ours-vs-golden per corona projection.

For every corona light x golden pose where the projection is on-screen,
BSP-unoccluded, and uncrowded (no other corona within 2.4*HALO px — an
overlapping halo makes the measurement unattributable), measure the net halo
delta = mean(golden - ours) over a HALO-px disc at the projected light.
When both renders agree (both draw or both hide the corona) the delta is
parity noise near 0; a corona present in exactly one render shifts it by the
halo brightness (~+-18 and up, measured): delta >= +MISS_T means golden
shows and ours hides (false_hide), <= -MISS_T ours shows and golden hides
(false_show). This is the standing gate metric for AWoTCorona visibility
(the leaf-permeation bake); history: the Reach distance gate scored 26
mismatches across the 19 maps.

DECOMPOSITION: the binary disc delta conflates gating
errors with co-located emitter brightness deltas and sprite-width deltas
(a hand-checked truth table: 48/50 flagged rows were NOT gate errors). Each
emitted row now also carries the nocorona-referenced amplitude-fit
decomposition, proven on that truth table and refined on night-map fits:
  - invert UE1's screen blend against the corona-free reference render
    (S = (img-noc)/(1-noc/255)), annulus-corrected ring-median radial
    profiles, then fit amplitude A and width-stretch k against the skin
    texture's own radial profile x hue-triangle color x RZ
    (RZ=min(1,800/viewZ)). A=1, k=1 means "drawn at retail design
    brightness and width".
  - columns appended (old columns unchanged, first): A_gold, A_ours
    (amplitude at that side's best-fit k), k_gold, k_ours, family, note.
  - family: gate_mismatch  = one render draws the corona and the other
                             GENUINELY does not (shown side A>=A_SHOW with
                             profile corr>=CORR_SHOW and an ADDITIVE
                             signature; hidden side A<A_HIDE);
            width_delta    = both draw, disc delta explained by the fitted
                             corona pair, width term dominates;
            amplitude_delta= both draw, explained, amplitude term dominates;
            colocated_emitter = the disc delta is NOT the corona: fitted
                             amplitude outside the plausible corona band,
                             or the width fit railed the k grid (K_RAIL:
                             legit fits span 0.88-1.15, the pre-fix
                             billboard bug topped at ~1.35 — a railed k
                             is a flame skirt absorbed by the fit),
                             or the fitted pair fails to explain the disc
                             delta, or a multiplicative (surface-light)
                             signature, or no corona on either side;
            agree          = both sides consistent and nothing to explain.
Thresholds, justified from the truth table (59 rows) + the night-map
diagnostic fits:
  A_SHOW=0.25/CORR_SHOW=0.5/A_HIDE=0.12 — truth-table presence gates: every
    photometrically-absent side fit 0.00; drawn-clean rows 0.74-1.20; the
    true gate mismatch (Light80) fit 0.36/0.63 at corr 0.78/0.87.
  A_PLAUS=(0.45,1.6) — clean drawn rows span 0.74-1.20 (26 truth rows + 12
    diag night rows); emitter-contaminated fits read 0.20-0.40
    (flame-entangled-dim) or 1.92-6.34 (lantern-composite).
  EXPL=0.5 — the fitted corona pair must predict >= half the measured disc
    delta WITH ITS SIGN to blame the corona (screen-blend suppression
    biases the prediction high by <=1.14x at the flagged backgrounds).
  "genuinely does not draw" additionally requires the hidden side's excess
    profile to be FLAT inside the sprite (max ring |x| < max(4, 4*noise)):
    a co-located flame delta biasing the fit through zero (the truth
    table's flame-entangled-dim mechanism) must not read as a gate hide.
  VEIL slope>=+0.03 & intercept<=3.0 — veil-probe discriminator:
    citadel surface veils regress (gold-noc) vs noc at slope +0.06..+0.11,
    intercept ~0 (multiplicative); true coronas at intercept +5..+29,
    negative slope (additive).
Width attribution is by disc-delta decomposition (|delta_k| vs |delta_A|),
not a raw k threshold: matched-pair |k_gold-k_ours| noise runs to 0.13
(night-map fits) while real width flags ride the tail the 40px disc sees.

Usage: python corona_gate_sweep.py <renders_root> [--noc <root>] [--all] [Map ...]
  --noc  corona-free reference renders (default nocorona under scratch_root);
         if a reference image is missing the fits fall back to raw-luma
         excess (note "no-noc") — annulus correction still applies.
  --all  emit every scored projection (verdict ok included) with the
         decomposition, not only the flagged rows.
CSV to stdout: map,pose,corona,dist_uu,delta,verdict,A_gold,A_ours,k_gold,k_ours,family,note
Summary (ok/false_hide/false_show + family census) to stderr.
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mask_pickups import CX, CY, FX, FY, H, U2D, W, _level_tris, _occluded, basis
from scoreboard import GOLD, luma, q565
from wot_gamma_paths import CONVERSION, SCRATCH

SKIN_RE = re.compile(r"Texture'([\w\-]+)\.(?:[\w\-]+\.)*([\w\-]+)'")
HALO = 40         # net-halo disc radius, px
EDGE = 48         # skip projections this close to the frame edge
MISS_T = 12.0     # |delta| above => the two renders disagree on the corona

NOC_DEFAULT = SCRATCH / "nocorona"
PROJ_Z = 800.0    # Frame->Proj.Z at 1600w / FOV 90 (RZ = min(1, PROJ_Z/viewZ))
A_SHOW, CORR_SHOW = 0.25, 0.5   # side draws (truth-table presence gate)
A_HIDE = 0.12                   # side genuinely does not draw
A_PLAUS = (0.45, 1.6)           # plausible corona amplitude band
EXPL = 0.5                      # fitted pair must explain this delta fraction
VEIL_SLOPE, VEIL_ICPT = 0.03, 3.0
K_GRID = np.arange(0.70, 1.65, 0.025)
K_RAIL = 1.55     # both-drawn fit railing the k grid = contamination:
                  # legit fits span 0.88-1.15 (night fits), pre-fix bug <=1.35
EMIT_CLS = ("particlesprayer", "tracersprayer", "pixiesprayerblue",
            "lampflame", "flame", "smoke", "skulltorchwith", "candle",
            "boxylamp", "gothiclamp", "wirelamp", "hanginglight",
            "halftorchholder")
EMIT_R = 24.0                   # uu; co-located emitter annotation radius

MAPS = ["Arena_01", "Arena_02", "Arena_03", "Arena_04", "Arena_05",
        "Arena_06", "Arena_07", "Arena_08", "Arena_09", "Arena_11",
        "Arena_12", "Battle2_01", "Battle2_02", "Battle2_03",
        "Battle2_04", "Battle2_05", "Battle3_01", "Battle3_02", "Battle4_01"]

_tex_cache = {}


def corona_actors(mapname, conv):
    actors = json.loads((conv / f"{mapname}.t3d.json").read_text())["actors"]
    lightning = {a["props"].get("LightTag", "").lower()
                 for a in actors if a["cls"].lower() == "lightning"} - {""}
    out, emitters = [], []
    for a in actors:
        p = a.get("props", {})
        cls = a["cls"].lower()
        if any(cls.startswith(e) for e in EMIT_CLS):
            emitters.append((a["cls"], np.array(a["location"], float)))
        if (p.get("bCorona") == "True" and "Skin" in p
                and p.get("Tag", "").lower() not in lightning
                and SKIN_RE.match(p["Skin"])):
            m = SKIN_RE.match(p["Skin"])
            out.append((a["name"], np.array(a["location"], float), {
                "skin_pkg": m.group(1), "skin_name": m.group(2),
                "ds": float(p.get("DrawScale", 1.0)),
                "hue": int(p.get("LightHue", 0)),
                "sat": int(p.get("LightSaturation", 255))}))
    return out, emitters


# ---- decomposition helpers --------------------------------------------------

def hue_color(h, s):
    """UE1 renderer hue triangle + saturation lerp to white."""
    h, s = int(h), int(s)
    if h < 86:
        hue = np.array([(85 - h) / 85.0, h / 85.0, 0.0])
    elif h < 171:
        hue = np.array([0.0, (170 - h) / 85.0, (h - 85) / 85.0])
    else:
        hue = np.array([(h - 170) / 85.0, 0.0, (255 - h) / 84.0])
    return hue + (s / 255.0) * (1.0 - hue)


def screen_excess(img, base):
    """Invert UE1's screen blend: img = S + base*(1-S/255) =>
    S = (img-base)/(1-base/255). Per-channel, base clipped at 250."""
    b = np.minimum(base, 250.0)
    return (img - base) / (1.0 - b / 255.0)


def tex_radial_profile(pkg, name, hue, sat, conv):
    key = (pkg, name, hue, sat)
    if key not in _tex_cache:
        f = conv / "tex" / pkg / f"{name}.png"
        if not f.exists():
            _tex_cache[key] = None
        else:
            tex = np.asarray(Image.open(f).convert("RGB")).astype(np.float64)
            tl = luma(tex * hue_color(hue, sat)[None, None, :])
            th, tw = tl.shape
            yy, xx = np.mgrid[:th, :tw]
            rn = np.sqrt(((xx - (tw - 1) / 2.0) / (tw / 2.0)) ** 2
                         + ((yy - (th - 1) / 2.0) / (th / 2.0)) ** 2)
            edges = np.linspace(0, 1.45, 65)
            prof = np.array([np.median(tl[(rn >= a) & (rn < b)])
                             if ((rn >= a) & (rn < b)).any() else 0.0
                             for a, b in zip(edges[:-1], edges[1:])])
            _tex_cache[key] = (0.5 * (edges[:-1] + edges[1:]), prof)
    return _tex_cache[key]


def ring_medians(X, r, redges, min_px=40):
    out = np.full(len(redges) - 1, np.nan)
    for i in range(len(redges) - 1):
        m = (r >= redges[i]) & (r < redges[i + 1])
        if m.sum() >= min_px:
            out[i] = np.median(X[m])
    return out


def fit_A(rc, x, P, half_k):
    """LSQ amplitude of x against P over the sprite fit band; also corr."""
    m = ((rc >= max(14.0, 0.10 * half_k)) & (rc <= half_k) & ~np.isnan(x)
         & (P > 0.02 * (np.nanmax(P) if np.nanmax(P) > 0 else 1.0)))
    if m.sum() < 4:
        return float("nan"), float("nan"), float("nan")
    A = float((x[m] * P[m]).sum() / (P[m] ** 2).sum())
    rms = float(np.sqrt(((x[m] - A * P[m]) ** 2).mean()))
    c = float(np.corrcoef(x[m], P[m])[0, 1]) if np.nanstd(x[m]) > 0 else 0.0
    return A, rms, c


def fit_Ak(rc, x, tc, tp, half, rz):
    """Grid over width-stretch k: x(r) ~= A * P(r/(k*half)). Best (k,A,corr)."""
    best = (float("nan"), float("nan"), float("nan"), np.inf)
    for k in K_GRID:
        P = np.interp(rc / (k * half), tc, tp, right=0.0) * rz
        A, rms, c = fit_A(rc, x, P, half * k)
        if not np.isnan(A) and rms < best[3]:
            best = (float(k), A, c, rms)
    return best[:3]


def _presence(A, corr, x, rc, half, noise):
    """yes = drawn; no = GENUINELY not drawn (fit absent AND the excess
    profile is flat inside the sprite — a co-located flame/emitter delta
    carving structure through the crop makes the hide untrustworthy);
    maybe = ambiguous/contaminated."""
    if not np.isnan(A) and A >= A_SHOW and (np.isnan(corr) or corr >= CORR_SHOW):
        return "yes"
    if np.isnan(A) or A < A_HIDE:
        band = (rc <= half) & ~np.isnan(x)
        flat = (not band.any()) or \
            float(np.nanmax(np.abs(x[band]))) < max(4.0, 4.0 * noise)
        return "no" if flat else "maybe"
    return "maybe"


def decompose(gold, ours, noc, sx, sy, xc, lp, delta, emit_note, conv):
    """Amplitude/width/gate decomposition of one flagged projection.
    Returns (A_gold, A_ours, k_gold, k_ours, family, note)."""
    half = 640.0 * lp["ds"]
    rz = min(1.0, PROJ_Z / xc)
    prof = tex_radial_profile(lp["skin_pkg"], lp["skin_name"],
                              lp["hue"], lp["sat"], conv)
    nan = float("nan")
    if prof is None:
        return nan, nan, nan, nan, "unclassified", "no-skin-texture"
    tc, tp = prof

    rad = int(np.clip(1.55 * half, HALO + 24, 1000))
    x0, x1 = max(0, int(sx) - rad), min(W, int(sx) + rad)
    y0, y1 = max(0, int(sy) - rad), min(H, int(sy) + rad)
    g_c, o_c = gold[y0:y1, x0:x1], ours[y0:y1, x0:x1]
    yy, xx = np.ogrid[y0:y1, x0:x1]
    r = np.sqrt((xx - sx) ** 2 + (yy - sy) ** 2)

    notes = [emit_note] if emit_note else []
    if noc is not None:
        n_c = noc[y0:y1, x0:x1]
        Xg = luma(screen_excess(g_c, n_c))
        Xo = luma(screen_excess(o_c, n_c))
    else:
        notes.append("no-noc")
        Xg, Xo = luma(g_c), luma(o_c)

    binw = max(3.0, half / 24.0)
    redges = np.arange(0.0, min(1.5 * half, rad) + binw, binw)
    rc = 0.5 * (redges[:-1] + redges[1:])
    mg = ring_medians(Xg, r, redges)
    mo = ring_medians(Xo, r, redges)
    ann = (rc >= 1.08 * half) & (rc <= 1.45 * half) & ~np.isnan(mg)
    if ann.sum() >= 2:
        base_g, base_o = float(np.nanmean(mg[ann])), float(np.nanmean(mo[ann]))
        noise = max(float(np.nanstd(mg[ann])), 0.8)   # 565-lattice floor
    else:
        far = (rc > half) & ~np.isnan(mg)
        base_g = float(np.nanmedian(mg[far])) if far.any() else 0.0
        base_o = float(np.nanmedian(mo[far])) if far.any() else 0.0
        noise = 1.5
    xg, xo = mg - base_g, mo - base_o

    kg, Ag, cg = fit_Ak(rc, xg, tc, tp, half, rz)
    ko, Ao, co = fit_Ak(rc, xo, tc, tp, half, rz)
    pg = _presence(Ag, cg, xg, rc, half, noise)
    po = _presence(Ao, co, xo, rc, half, noise)

    def out(family):
        return Ag, Ao, kg, ko, family, "; ".join(notes)

    if np.isnan(Ag) and np.isnan(Ao):
        return out("unclassified")

    # -- one render draws, the other genuinely does not => gate --------------
    if pg == "yes" and po == "no":
        if noc is not None:            # surface-veil imposter check
            n_l, g_l = luma(n_c), luma(g_c)
            m = (r <= half) & (n_l < 200)
            nl, dl = n_l[m], (g_l - n_l)[m]
            if len(nl) > 200:
                slope, icpt = np.polyfit(nl, dl, 1)
                notes.append(f"veil slope={slope:+.3f} icpt={icpt:+.1f}")
                if slope >= VEIL_SLOPE and icpt <= VEIL_ICPT:
                    notes.append("multiplicative: surface-light veil")
                    return out("colocated_emitter")
        notes.append("golden draws; ours hides")
        return out("gate_mismatch")
    if po == "yes" and pg == "no":
        notes.append("ours draws; golden hides")
        return out("gate_mismatch")

    # -- neither draws: the disc delta is not a corona at all ----------------
    if pg in ("no", "maybe") and po in ("no", "maybe") \
            and (np.isnan(Ag) or Ag < A_SHOW) and (np.isnan(Ao) or Ao < A_SHOW):
        if abs(delta) >= MISS_T:
            notes.append("no corona either side: scene/emitter delta")
            return out("colocated_emitter")
        return out("agree")

    # -- both draw (or one side ambiguous with a real fit) -------------------
    if not (A_PLAUS[0] <= Ag <= A_PLAUS[1]) or not (A_PLAUS[0] <= Ao <= A_PLAUS[1]):
        notes.append(f"implausible corona amplitude ({Ag:.2f}/{Ao:.2f})")
        return out("colocated_emitter")
    if kg >= K_RAIL or ko >= K_RAIL:
        notes.append(f"k railed ({kg:.2f}/{ko:.2f}): contaminated fit")
        return out("colocated_emitter")
    disc = r <= HALO
    Pg = np.interp(r[disc] / (kg * half), tc, tp, right=0.0) * rz
    Po = np.interp(r[disc] / (ko * half), tc, tp, right=0.0) * rz
    d_pred = float((Ag * Pg - Ao * Po).mean())
    kbar, abar = 0.5 * (kg + ko), 0.5 * (Ag + Ao)
    Pbar = np.interp(r[disc] / (kbar * half), tc, tp, right=0.0) * rz
    d_amp = float(((Ag - Ao) * Pbar).mean())
    d_wid = float((abar * (Pg - Po)).mean())
    notes.append(f"pred={d_pred:+.1f} (amp {d_amp:+.1f} wid {d_wid:+.1f})")
    if abs(delta) < MISS_T and abs(d_pred) < MISS_T:
        return out("agree")
    if d_pred / delta < EXPL:      # signed: pred must carry the delta's sign
        notes.append("corona pair does not explain disc delta")
        return out("colocated_emitter")
    return out("width_delta" if abs(d_wid) >= abs(d_amp) else "amplitude_delta")


# ---- sweep ------------------------------------------------------------------

def _fmt(v, nd=2):
    return "" if (isinstance(v, float) and np.isnan(v)) else f"{v:.{nd}f}"


def sweep_map(root, mapname, conv, noc_root, counters, families, emit_all):
    cor, emitters = corona_actors(mapname, conv)
    if not cor:
        return
    man = json.loads((GOLD / mapname / "manifest.json").read_text())
    eye = float(man.get("eye_height", 29.4))
    geo = _level_tris(mapname)
    pts = np.array([p for _, p, _ in cor])
    emit_note = []
    for _, p, _ in cor:
        near = [(np.linalg.norm(ep - p), ec) for ec, ep in emitters
                if np.linalg.norm(ep - p) <= EMIT_R]
        emit_note.append(f"emitter {min(near)[1]}@{min(near)[0]:.0f}uu"
                         if near else "")
    yy, xx = np.ogrid[:H, :W]
    for shot in man["shots"]:
        ours_f = root / mapname / shot["file"]
        gold_f = GOLD / mapname / shot["file"]
        if not (ours_f.exists() and gold_f.exists()):
            continue
        gold_rgb = np.asarray(Image.open(gold_f).convert("RGB")).astype(np.float64)
        gold = luma(gold_rgb)
        if float((gold > 8).mean()) < 0.005:      # golden-dark pose
            continue
        ours_rgb = q565(np.asarray(Image.open(ours_f).convert("RGB")))
        ours = luma(ours_rgb)
        noc_f = noc_root / mapname / shot["file"]
        noc_rgb = None
        cam = np.array([shot["pawn"]["x"], shot["pawn"]["y"],
                        shot["pawn"]["z"]], float) + np.array([0, 0, eye])
        F, R, U = basis(shot["rot"]["pitch"] * U2D, shot["rot"]["yaw"] * U2D,
                        shot["rot"]["roll"] * U2D)
        rel = pts - cam
        xc = rel @ F
        sx = CX + FX * (rel @ R) / np.where(xc == 0, 1e-9, xc)
        sy = CY - FY * (rel @ U) / np.where(xc == 0, 1e-9, xc)
        for i, (name, p, lp) in enumerate(cor):
            if xc[i] <= 16:
                continue
            if not (EDGE < sx[i] < W - EDGE and EDGE < sy[i] < H - EDGE):
                continue
            crowded = False
            for j in range(len(cor)):
                if j != i and xc[j] > 16 and \
                        (sx[j] - sx[i]) ** 2 + (sy[j] - sy[i]) ** 2 < (2.4 * HALO) ** 2:
                    crowded = True
                    break
            if crowded:
                counters["crowded"] += 1
                continue
            if _occluded(cam, p, geo, slack=8.0):
                continue
            disc = ((xx - sx[i]) ** 2 + (yy - sy[i]) ** 2) <= HALO ** 2
            delta = float(gold[disc].mean() - ours[disc].mean())
            if delta >= MISS_T:
                verdict = "false_hide"
            elif delta <= -MISS_T:
                verdict = "false_show"
            else:
                verdict = "ok"
            counters[verdict] += 1
            if verdict == "ok" and not emit_all:
                continue
            if noc_rgb is None and noc_f.exists():
                noc_rgb = q565(np.asarray(Image.open(noc_f).convert("RGB")))
            Ag, Ao, kg, ko, family, note = decompose(
                gold_rgb, ours_rgb, noc_rgb, float(sx[i]), float(sy[i]),
                float(xc[i]), lp, delta, emit_note[i], conv)
            families[family] += 1
            print(f"{mapname},{shot['file']},{name},"
                  f"{np.linalg.norm(rel[i]):.0f},{delta:+.1f},{verdict},"
                  f"{_fmt(Ag)},{_fmt(Ao)},{_fmt(kg)},{_fmt(ko)},{family},{note}")


def main():
    conv = CONVERSION
    args = sys.argv[1:]
    noc_root, emit_all = NOC_DEFAULT, False
    if "--noc" in args:
        i = args.index("--noc")
        noc_root = Path(args[i + 1])
        del args[i:i + 2]
    if "--all" in args:
        emit_all = True
        args.remove("--all")
    root = Path(args[0])
    counters, families = Counter(), Counter()
    print("map,pose,corona,dist_uu,delta,verdict,"
          "A_gold,A_ours,k_gold,k_ours,family,note")
    for m in (args[1:] or MAPS):
        sweep_map(root, m, conv, noc_root, counters, families, emit_all)
    print(f"ok={counters['ok']} false_hide={counters['false_hide']} "
          f"false_show={counters['false_show']} "
          f"(crowded skipped={counters['crowded']})", file=sys.stderr)
    print("families: " + " ".join(f"{k}={v}" for k, v in sorted(
        families.items(), key=lambda kv: -kv[1])), file=sys.stderr)


if __name__ == "__main__":
    main()
