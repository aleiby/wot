"""Compare a native BGRA8 atlas with the Python PNG oracle pixel-for-pixel."""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def compare_atlas(native_path, oracle_path, metadata=None):
    native_path = Path(native_path)
    oracle_path = Path(oracle_path)
    oracle = np.asarray(Image.open(oracle_path).convert("RGBA"))
    native_bytes = native_path.read_bytes()
    if len(native_bytes) != oracle.size:
        raise ValueError(
            f"native byte length {len(native_bytes)} != expected {oracle.size}")
    native = np.frombuffer(native_bytes, dtype=np.uint8).reshape(
        oracle.shape)[:, :, [2, 1, 0, 3]]
    difference = native.astype(np.int16) - oracle.astype(np.int16)
    changed = np.any(difference != 0, axis=2)
    surface_differences = []
    metadata = metadata or {"surfs": {}}
    for key, rect in metadata.get("surfs", {}).items():
        x, y, width, height = (rect[name] for name in ("x", "y", "w", "h"))
        surface_changed = changed[y:y + height, x:x + width]
        count = int(surface_changed.sum())
        if count:
            surface_differences.append({
                "surface": int(key),
                "changed_texels": count,
                "max_delta": int(np.abs(
                    difference[y:y + height, x:x + width]).max()),
            })
    first_difference = None
    coordinates = np.argwhere(changed)
    if len(coordinates):
        y, x = (int(value) for value in coordinates[0])
        surface = None
        for key, rect in metadata.get("surfs", {}).items():
            if (rect["x"] <= x < rect["x"] + rect["w"]
                    and rect["y"] <= y < rect["y"] + rect["h"]):
                surface = int(key)
                break
        first_difference = {
            "x": x,
            "y": y,
            "surface": surface,
            "native_rgba": native[y, x].tolist(),
            "oracle_rgba": oracle[y, x].tolist(),
        }
    return {
        "size": [int(oracle.shape[1]), int(oracle.shape[0])],
        "native": str(native_path),
        "oracle": str(oracle_path),
        "changed_pixels": int(changed.sum()),
        "max_absolute_channel_delta": int(np.abs(difference).max()),
        "first_difference": first_difference,
        "surface_differences": surface_differences,
        "exact": not bool(changed.any()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    metadata_path = args.metadata or args.oracle.with_suffix(".json")
    metadata = (json.loads(metadata_path.read_text())
                if metadata_path.exists() else None)
    try:
        report = compare_atlas(args.native, args.oracle, metadata)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    text = json.dumps(report, indent=2) + "\n"
    print(text, end="")
    report_path = args.report or args.native.with_suffix(".comparison.json")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(text)
    raise SystemExit(0 if report["exact"] else 1)


if __name__ == "__main__":
    main()
