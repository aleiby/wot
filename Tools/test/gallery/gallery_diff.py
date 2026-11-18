#!/usr/bin/env python3
"""Gate two actor-gallery directories with per-entry center-crop tolerances."""

from __future__ import annotations

import argparse
import io
import json
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image


if hasattr(sys.stdout, "buffer"):
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8",
                                  errors="replace")


@dataclass(frozen=True)
class Metrics:
    bad_fraction: float | None
    center_mad: float | None
    error: str = ""


@dataclass(frozen=True)
class Policy:
    entry_names: tuple[str, ...]
    crop_fraction: float
    default_center_mad: float
    entry_center_mad: dict[str, float]
    known_differences: dict[str, str]


def pngs(directory: Path) -> dict[str, Path]:
    return {path.name: path for path in directory.glob("*.png") if path.is_file()}


def compare(a_path: Path, b_path: Path, bad_thresh: int,
            crop_fraction: float) -> Metrics:
    with Image.open(a_path) as a_image, Image.open(b_path) as b_image:
        a = np.asarray(a_image.convert("RGB"), dtype=np.int16)
        b = np.asarray(b_image.convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        return Metrics(None, None,
                       f"size mismatch {a.shape[1]}x{a.shape[0]} vs "
                       f"{b.shape[1]}x{b.shape[0]}")
    delta = np.abs(a - b)
    bad = np.max(delta, axis=2) > bad_thresh
    height, width = a.shape[:2]
    crop_width = max(1, round(width * crop_fraction))
    crop_height = max(1, round(height * crop_fraction))
    left = (width - crop_width) // 2
    top = (height - crop_height) // 2
    center = delta[top:top + crop_height, left:left + crop_width]
    return Metrics(float(np.count_nonzero(bad) / bad.size),
                   float(np.mean(center)))


def load_policy(path: Path | None, crop_override: float | None,
                mad_override: float | None) -> Policy:
    root: dict = {}
    if path:
        root = json.loads(path.read_text(encoding="utf-8"))
    diff = root.get("diff", {})
    names = tuple(sorted(
        (entry.get("name", "") for entry in root.get("entries", [])),
        key=len, reverse=True))
    entries = diff.get("entries", {})
    known_rows = diff.get("known_differences", [])
    known = {row["entry"]: row["reason"] for row in known_rows}
    crop = crop_override if crop_override is not None else float(
        diff.get("center_crop_fraction", 0.5))
    default_mad = mad_override if mad_override is not None else float(
        diff.get("default_max_center_mad", 18.0))
    return Policy(names, crop, default_mad,
                  {str(name): float(value) for name, value in entries.items()},
                  known)


def entry_for_filename(filename: str, names: tuple[str, ...]) -> str:
    stem = Path(filename).stem
    for name in names:
        if stem == name or stem.startswith(name + "_"):
            return name
    return stem.split("_", 1)[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dir_a", type=Path)
    parser.add_argument("dir_b", type=Path)
    parser.add_argument("--config", type=Path,
                        help="gallery.json containing diff policy and entry names")
    parser.add_argument("--bad-thresh", type=int, default=8,
                        help="per-channel delta for the informational bad fraction")
    parser.add_argument("--bad-frac", type=float,
                        help="optional legacy full-frame bad-fraction gate")
    parser.add_argument("--center-crop", type=float,
                        help="override the config center crop fraction")
    parser.add_argument("--center-mad", type=float,
                        help="override all per-entry center MAD limits")
    args = parser.parse_args()

    if not args.dir_a.is_dir() or not args.dir_b.is_dir():
        parser.error("dirA and dirB must both be directories")
    if args.config and not args.config.is_file():
        parser.error("--config must name an existing JSON file")
    if not 0 <= args.bad_thresh <= 255:
        parser.error("--bad-thresh must be in [0, 255]")
    if args.bad_frac is not None and not 0.0 <= args.bad_frac <= 1.0:
        parser.error("--bad-frac must be in [0, 1]")
    policy = load_policy(args.config, args.center_crop, args.center_mad)
    if not 0.0 < policy.crop_fraction <= 1.0:
        parser.error("center crop fraction must be in (0, 1]")
    if policy.default_center_mad < 0.0:
        parser.error("center MAD limits must be non-negative")

    files_a = pngs(args.dir_a)
    files_b = pngs(args.dir_b)
    shared = sorted(files_a.keys() & files_b.keys())
    only_a = sorted(files_a.keys() - files_b.keys())
    only_b = sorted(files_b.keys() - files_a.keys())
    failed = bool(only_a or only_b)

    print(f"{'name':<48} {'bad-frac':>10} {'center-MAD':>11} {'limit':>8}  verdict")
    print(f"{'-' * 48} {'-' * 10} {'-' * 11} {'-' * 8}  {'-' * 7}")
    known_hits = 0
    for name in shared:
        entry = entry_for_filename(name, policy.entry_names)
        metrics = compare(files_a[name], files_b[name], args.bad_thresh,
                          policy.crop_fraction)
        limit = policy.entry_center_mad.get(entry, policy.default_center_mad)
        if metrics.error:
            verdict = "FAIL"
            bad_text = mad_text = "n/a"
            detail = f" ({metrics.error})"
        else:
            over = metrics.center_mad > limit
            if args.bad_frac is not None:
                over = over or metrics.bad_fraction > args.bad_frac
            if over and entry in policy.known_differences:
                verdict = "KNOWN"
                known_hits += 1
                detail = f" ({policy.known_differences[entry]})"
            else:
                verdict = "FAIL" if over else "PASS"
                detail = ""
            bad_text = f"{metrics.bad_fraction:.6f}"
            mad_text = f"{metrics.center_mad:.3f}"
        failed |= verdict == "FAIL"
        print(f"{name:<48} {bad_text:>10} {mad_text:>11} {limit:>8.3f}  "
              f"{verdict}{detail}")

    if only_a:
        print("\nOnly in dirA (FAIL):")
        for name in only_a:
            print(f"  {name}")
    if only_b:
        print("\nOnly in dirB (FAIL):")
        for name in only_b:
            print(f"  {name}")
    if not shared and not only_a and not only_b:
        print("\nNo PNG files found in either directory.")
        failed = True
    print(f"\nsummary shared={len(shared)} known={known_hits} "
          f"missing={len(only_a) + len(only_b)} gate={'FAIL' if failed else 'PASS'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
