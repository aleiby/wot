"""Rig-drift correction for retail captures (2026-07-14 discovery).

The Windows capture rig drifted bright-in-darks vs the archived goldens:
darks/mids +10..+18 luma, ~0 above
p95. This tool builds a per-channel quantile-mapping LUT from a (fresh
recapture of an archived golden pose, archived golden PNG) pair and applies
it to any capture taken on the drifted rig, mapping it into golden space so
it can be compared against our renders.

Usage:
  python drift_correct.py fit <fresh_golden_pose.bmp/png> <archived_golden.png> <lut.json>
  python drift_correct.py apply <lut.json> <capture.png> <out.png>

Fit uses position-independent luma quantiles (the skymarg lesson: never fit
on misregistered pixels), one shared curve for RGB — the drift measured
channel-uniform. Re-fit whenever the rig is revalidated.
"""
import json
import sys

import numpy as np
from PIL import Image


def _lum(img):
    return np.asarray(img.convert("RGB"), np.float32) @ np.array(
        [0.299, 0.587, 0.114], np.float32)


def fit(fresh_path, golden_path, out_path):
    fresh = _lum(Image.open(fresh_path))
    gold = _lum(Image.open(golden_path))
    qs = np.linspace(0.5, 99.5, 199)
    fq = np.percentile(fresh, qs)
    gq = np.percentile(gold, qs)
    # monotone cleanup + endpoints
    fq = np.maximum.accumulate(fq)
    gq = np.maximum.accumulate(gq)
    lut = dict(x=[0.0] + fq.tolist() + [255.0],
               y=[0.0] + gq.tolist() + [255.0])
    json.dump(lut, open(out_path, "w"))
    mid = np.interp(128.0, lut["x"], lut["y"])
    print(f"LUT written: drifted 128 -> golden {mid:.1f}; "
          f"p50 {np.median(fresh):.1f} -> {np.interp(np.median(fresh), lut['x'], lut['y']):.1f}")


def apply(lut_path, in_path, out_path):
    lut = json.load(open(lut_path))
    img = np.asarray(Image.open(in_path).convert("RGB"), np.float32)
    out = np.interp(img, lut["x"], lut["y"]).clip(0, 255).astype(np.uint8)
    Image.fromarray(out).save(out_path)
    print(f"corrected -> {out_path}")


if __name__ == "__main__":
    if sys.argv[1] == "fit":
        fit(*sys.argv[2:5])
    elif sys.argv[1] == "apply":
        apply(*sys.argv[2:5])
    else:
        print(__doc__)
