#!/usr/bin/env python3
"""Export every retail Sound and byte-compare it with UCC batchexport output.

Run from anywhere with no arguments for the repository defaults::

    python3 Tools/convert/verify_sounds.py
"""
import argparse
import filecmp
import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import exports_dir, original_dir  # noqa: E402

from ue1pkg import export_sounds


ROOT = Path(__file__).resolve().parents[2]
SYSTEM_PACKAGES = (
    "Angreal", "Environment", "Fire", "IpServer", "Legend", "MachinShin",
    "ParticleSystems", "UBrowser", "UWindow", "WOT", "WoTBase",
    "WoTBrowser", "WoTDecorations", "WoTPawns", "WoTTraps",
)


def source_packages(retail_root):
    """Return the same standalone and embedded-sound packages as extract_all.sh."""
    retail_root = Path(retail_root)
    standalone = sorted((retail_root / "Sounds").glob("*.uax"))
    embedded = [retail_root / "System" / f"{name}.u"
                for name in SYSTEM_PACKAGES]
    missing = [path for path in embedded if not path.is_file()]
    if not standalone:
        missing.append(retail_root / "Sounds" / "*.uax")
    if missing:
        raise FileNotFoundError(
            "missing retail sound packages: " + ", ".join(map(str, missing)))
    return standalone + embedded


def _wav_files(root):
    root = Path(root)
    return {path.relative_to(root).as_posix(): path
            for path in root.glob("*/*.wav")}


def verify_sounds(retail_root, reference_root, output_root):
    """Export all scoped packages and return an exact-comparison summary."""
    output_root = Path(output_root)
    reference_root = Path(reference_root)
    # Same/overlapping roots would compare freshly generated files against
    # themselves and report a vacuous pass.
    ref = reference_root.resolve()
    out = output_root.resolve()
    if ref == out or ref in out.parents or out in ref.parents:
        raise ValueError(
            f"reference root {ref} and output root {out} must be disjoint")
    output_root.mkdir(parents=True, exist_ok=True)
    sound_exports = 0
    collisions = []
    packages = source_packages(retail_root)
    for package_path in packages:
        targets = export_sounds(package_path, output_root / package_path.stem)
        sound_exports += len(targets)
        counts = Counter(path.name for path in targets)
        collisions.extend(
            f"{package_path.stem}/{name} ({count} exports)"
            for name, count in sorted(counts.items()) if count > 1)

    generated = _wav_files(output_root)
    reference = _wav_files(reference_root)
    generated_names = set(generated)
    reference_names = set(reference)
    common = sorted(generated_names & reference_names)
    identical = [name for name in common
                 if filecmp.cmp(generated[name], reference[name], shallow=False)]
    identical_names = set(identical)
    mismatches = [name for name in common if name not in identical_names]
    only_generated = sorted(generated_names - reference_names)
    only_reference = sorted(reference_names - generated_names)
    total = len(generated_names | reference_names)
    return {
        "packages": len(packages),
        "sound_exports": sound_exports,
        "generated_files": len(generated),
        "reference_files": len(reference),
        "total_sounds": total,
        "byte_identical": len(identical),
        "mismatches": mismatches,
        "only_generated": only_generated,
        "only_reference": only_reference,
        "flat_name_collisions": collisions,
    }


def _print_list(label, values):
    print(f"{label}: {len(values)}")
    for value in values:
        print(f"  {value}")


def print_summary(summary):
    print(f"Packages exported: {summary['packages']}")
    print(f"Sound exports decoded: {summary['sound_exports']}")
    print(f"Generated WAV files: {summary['generated_files']}")
    print(f"Reference WAV files: {summary['reference_files']}")
    print(f"Total sounds: {summary['total_sounds']}")
    print(f"Byte-identical: {summary['byte_identical']}")
    _print_list("Mismatches", summary["mismatches"])
    _print_list("Only in generated tree", summary["only_generated"])
    _print_list("Only in reference tree", summary["only_reference"])
    _print_list("Flat-name collisions", summary["flat_name_collisions"])


def _passed(summary):
    return (summary["byte_identical"] == summary["total_sounds"] and
            not summary["mismatches"] and
            not summary["only_generated"] and
            not summary["only_reference"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", type=Path)
    parser.add_argument("--reference-root", type=Path)
    parser.add_argument(
        "--output-dir", type=Path,
        help="keep generated WAVs here (must be absent or empty; default: temporary)")
    args = parser.parse_args()
    args.retail_root = args.retail_root or original_dir()
    args.reference_root = args.reference_root or exports_dir() / "sounds"

    if args.output_dir is not None:
        if args.output_dir.exists() and any(args.output_dir.iterdir()):
            parser.error(f"--output-dir must be empty: {args.output_dir}")
        summary = verify_sounds(
            args.retail_root, args.reference_root, args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="verify-sounds-") as temp_dir:
            summary = verify_sounds(
                args.retail_root, args.reference_root, Path(temp_dir))

    print_summary(summary)
    return 0 if _passed(summary) else 1


if __name__ == "__main__":
    sys.exit(main())
