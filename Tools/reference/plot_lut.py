"""Transfer-curve report figures from lut_data.json."""
import json
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from wot_config import local_path  # noqa: E402
OUT = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else local_path("scratch_root")
data_path = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else OUT / "lut_data.json"
D = json.loads(data_path.read_text())

SURFACE = "#fcfcfb"; INK = "#0b0b0b"; INK2 = "#52514e"; MUTED = "#898781"
GRID = "#e1e0d9"; BASE = "#c3c2b7"
C_R = "#e34948"; C_G = "#008300"; C_B = "#2a78d6"   # validated slots 6/4/1

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Segoe UI", "DejaVu Sans"],
    "text.color": INK, "axes.labelcolor": INK2,
    "xtick.color": MUTED, "ytick.color": MUTED,
    "axes.edgecolor": BASE, "axes.linewidth": 0.8,
})

# ---------------- Figure 1: empirical transfer curve ----------------
fig, ax = plt.subplots(figsize=(9, 6), dpi=200)
fig.patch.set_facecolor(SURFACE); ax.set_facecolor(SURFACE)

x255 = np.linspace(0, 255, 256)
xn = x255 / 255.0

# reference/candidate curves: neutral, dashed, direct-labeled
ax.plot(x255, x255, ls=(0, (2, 3)), lw=1.2, color=BASE, zorder=1)
ax.annotate("identity (linear match)", xy=(216, 216), xytext=(150, 232),
            fontsize=8.5, color=MUTED,
            arrowprops=dict(arrowstyle="-", color=BASE, lw=0.7))

g = D["candidates"]["pow 1/2.2"]
y_cand = 255 * g["a"] * xn ** (1 / 2.2)
ax.plot(x255, y_cand, ls="--", lw=1.4, color=MUTED, zorder=2)
ax.annotate(f"pow 1/2.2 · a={g['a']:.2f}  (best stock curve)",
            xy=(140, float(np.interp(140, x255, y_cand))), xytext=(104, 205),
            fontsize=8.5, color=INK2,
            arrowprops=dict(arrowstyle="-", color=MUTED, lw=0.7))

f = D["candidates"]["free power fit"]
y_free = 255 * f["a"] * xn ** f["gamma"]
ax.plot(x255, y_free, ls="-.", lw=1.2, color=INK2, zorder=2)
ax.annotate(f"free fit γ={f['gamma']:.2f}",
            xy=(60, float(np.interp(60, x255, y_free))), xytext=(18, 150),
            fontsize=8.5, color=INK2,
            arrowprops=dict(arrowstyle="-", color=MUTED, lw=0.7))

# measured channel LUTs — display only well-supported bins (>=0.05% of pixels)
trimmed = {}
for cname, color in (("R", C_R), ("G", C_G), ("B", C_B)):
    ch = D["channels"][cname]
    lx, lm = np.array(ch["lut_x"]), np.array(ch["lut_med"])
    ln = np.array(ch["lut_n"], dtype=float)
    keep = ln >= 0.0005 * ln.sum()
    lx, lm = lx[keep], lm[keep]
    q1a, q3a = np.array(ch["lut_q1"])[keep], np.array(ch["lut_q3"])[keep]
    trimmed[cname] = (lx, lm)
    if cname == "G":
        ax.fill_between(lx, q1a, q3a, color=C_G, alpha=0.12, lw=0, zorder=3)
    ax.plot(lx, lm, lw=2.0, color=color, zorder=4)
for cname, color, dy in (("R", C_R, 8), ("G", C_G, 0), ("B", C_B, -12)):
    lx, lm = trimmed[cname]
    ax.annotate(f"{cname} measured (γ={D['channels'][cname]['gamma']:.2f})",
                xy=(float(lx[-1]) + 3, float(lm[-1]) + dy),
                fontsize=9, color=color, fontweight="bold", va="center")

ax.set_xlim(0, 255); ax.set_ylim(0, 255)
ax.set_aspect("equal")
ax.grid(color=GRID, lw=0.6); ax.set_axisbelow(True)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
ax.set_xlabel("golden pixel value (original game, Glide)", fontsize=10)
ax.set_ylabel("UE 5.8 render pixel value (same pose)", fontsize=10)
ax.set_title("Empirical transfer curve: UE render vs golden capture\n"
             "9.3M registered pixel pairs across 19 maps · median per 4-value bin · band = G-channel IQR",
             fontsize=11, loc="left", color=INK, pad=12)
fig.tight_layout()
fig.savefig(OUT / "fig1_transfer_curve.png", facecolor=SURFACE)
plt.close(fig)

# ---------------- Figure 2: per-map gamma distribution ----------------
maps = sorted(D["maps"].items(), key=lambda kv: kv[1]["gamma_median"])
names = [k for k, _ in maps]
med = np.array([v["gamma_median"] for _, v in maps])
q1 = np.array([v["gamma_iqr"][0] for _, v in maps])
q3 = np.array([v["gamma_iqr"][1] for _, v in maps])

fig, ax = plt.subplots(figsize=(9, 7), dpi=200)
fig.patch.set_facecolor(SURFACE); ax.set_facecolor(SURFACE)
ypos = np.arange(len(names))

ax.axvline(1.0, color=BASE, ls=(0, (2, 3)), lw=1.2, zorder=1)
ax.text(1.0, len(names) - 0.2, " γ=1 · linear (scale-only difference)",
        fontsize=8.5, color=MUTED, va="top")
ax.axvline(1 / 2.2, color=MUTED, ls="--", lw=1.4, zorder=1)
ax.text(1 / 2.2, -0.55, " γ=1/2.2 · gamma-encode mismatch", fontsize=8.5,
        color=INK2, va="top")

ax.hlines(ypos, q1, q3, color=C_B, lw=2, alpha=0.45, zorder=2)
ax.plot(med, ypos, "o", ms=7, color=C_B, mec=SURFACE, mew=1.2, zorder=3)

ax.set_yticks(ypos, names, fontsize=8.5)
ax.set_xlim(-0.1, 1.15)
ax.grid(axis="x", color=GRID, lw=0.6); ax.set_axisbelow(True)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
ax.set_xlabel("fitted exponent γ in  UE = a · golden^γ   (per-shot fits, dot = map median, bar = IQR)",
              fontsize=10)
ax.set_title("Per-map transfer exponent: every map sits far from linear, clustered near 1/2.2",
             fontsize=11, loc="left", color=INK, pad=12)
fig.tight_layout()
fig.savefig(OUT / "fig2_gamma_by_map.png", facecolor=SURFACE)
plt.close(fig)
print("wrote fig1_transfer_curve.png, fig2_gamma_by_map.png")
