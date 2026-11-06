#!/usr/bin/env python3
"""Export all retail ScriptText classes and compare the UCC `.uc` oracle.

Run from anywhere with no arguments for the repository defaults::

    python3 Tools/convert/verify_script.py
"""
import argparse
import filecmp
import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import exports_dir, original_dir  # noqa: E402

from ue1pkg import export_scripts


ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PACKAGES = (
    "Angreal", "Environment", "Fire", "IpServer", "Legend", "MachinShin",
    "ParticleSystems", "UBrowser", "UWindow", "WOT", "WoTBase",
    "WoTBrowser", "WoTDecorations", "WoTPawns", "WoTTraps",
)
EXPECTED_SCRIPT_FILES = 1_253


def _uc_files(root):
    root = Path(root)
    return {path.relative_to(root).as_posix(): path
            for path in root.glob("*/*.uc")}


def _default_lines(data):
    marker = b"defaultproperties\r\n{\r\n"
    if marker not in data or not data.endswith(b"}\r\n"):
        return None, None
    source, defaults = data.split(marker, 1)
    return source, defaults[:-3].splitlines()


def _file_contains(path, evidence):
    try:
        return all(item.casefold() in path.read_text(
            encoding="latin-1", errors="replace").casefold() for item in evidence)
    except OSError:
        return False


def classify_mismatch(name, generated, reference, retail_root):
    """Return a stable category and byte/config evidence for a known delta."""
    if generated.rstrip(b"\0") == reference.rstrip(b"\0"):
        return "trailing-nul", "content is identical after removing trailing NUL bytes"
    if (generated.replace(b"\r\n", b"\n") ==
            reference.replace(b"\r\n", b"\n")):
        return "line-endings", "content is identical after CRLF/LF normalization"

    generated_source, generated_defaults = _default_lines(generated)
    reference_source, reference_defaults = _default_lines(reference)
    if generated_source != reference_source:
        return "unclassified", "source bytes differ before defaultproperties"

    system = Path(retail_root) / "System"
    if name == "UBrowser/UBrowserMainClientWindow.uc" and _file_contains(
            system / "WoT.ini", ("LANTabName=WOTBrowserLAN",
                                  "ServerListNames[0]=WOTBrowserWOT")):
        return (
            "runtime-config",
            "UCC loaded globalconfig values from WoT.ini; the package tag stores "
            "LANTabName=UBrowserLAN while WoT.ini stores WOTBrowserLAN and the "
            "WOTBrowser* ServerListNames array",
        )
    if name == "WOT/menuSlot.uc" and (_file_contains(
            system / "User.ini", ("SlotNames[0]=..Empty..",)) and
            _file_contains(system / "WoT.int", ("SlotNames[0]=..Empty..",))):
        return (
            "runtime-config-localization",
            "UCC loaded the nine '..Empty..' SlotNames from User.ini/WoT.int; "
            "the package tags retain dated local save-slot strings",
        )

    generated_counts = Counter(generated_defaults or ())
    reference_counts = Counter(reference_defaults or ())
    extra = list((generated_counts - reference_counts).elements())
    missing = list((reference_counts - generated_counts).elements())
    if extra and not missing and all(line.strip().endswith(b"=None") for line in extra):
        rendered = ", ".join(line.decode("latin-1").strip() for line in extra)
        return (
            "runtime-none-elision",
            "source is byte-identical; generated-only serialized defaults are "
            f"{rendered}, which UCC omitted after live default-object comparison",
        )
    return "unclassified", "source is identical but defaultproperties differ"


def verify_script(retail_root, reference_root, output_root):
    """Export the 15 game packages and return exact/classified results."""
    retail_root = Path(retail_root)
    reference_root = Path(reference_root)
    output_root = Path(output_root)
    reference = reference_root.resolve()
    output = output_root.resolve()
    if reference == output or reference in output.parents or output in reference.parents:
        raise ValueError(
            f"reference root {reference} and output root {output} must be disjoint")
    output_root.mkdir(parents=True, exist_ok=True)

    exports_decoded = 0
    package_counts = {}
    for package in SCRIPT_PACKAGES:
        package_path = retail_root / "System" / f"{package}.u"
        if not package_path.is_file():
            raise FileNotFoundError(f"missing retail script package: {package_path}")
        targets = export_scripts(package_path, output_root / package)
        package_counts[package] = len(targets)
        exports_decoded += len(targets)

    generated = _uc_files(output_root)
    oracle = _uc_files(reference_root)
    generated_names = set(generated)
    oracle_names = set(oracle)
    common = sorted(generated_names & oracle_names)
    exact = [name for name in common
             if filecmp.cmp(generated[name], oracle[name], shallow=False)]
    exact_names = set(exact)
    mismatches = []
    classifications = Counter()
    for name in common:
        if name in exact_names:
            continue
        category, evidence = classify_mismatch(
            name, generated[name].read_bytes(), oracle[name].read_bytes(), retail_root)
        classifications[category] += 1
        mismatches.append(dict(name=name, category=category, evidence=evidence,
                               generated_bytes=generated[name].stat().st_size,
                               reference_bytes=oracle[name].stat().st_size))
    return {
        "packages": len(SCRIPT_PACKAGES),
        "package_counts": package_counts,
        "exports_decoded": exports_decoded,
        "generated_files": len(generated),
        "reference_files": len(oracle),
        "expected_files": EXPECTED_SCRIPT_FILES,
        "byte_identical": len(exact),
        "mismatches": mismatches,
        "classifications": dict(sorted(classifications.items())),
        "only_generated": sorted(generated_names - oracle_names),
        "only_reference": sorted(oracle_names - generated_names),
    }


def print_summary(summary):
    print(f"Packages exported: {summary['packages']}")
    print(f"ScriptText exports decoded: {summary['exports_decoded']}")
    print(f"Generated .uc files: {summary['generated_files']}")
    print(f"Reference .uc files: {summary['reference_files']}")
    print(f"Expected census: {summary['expected_files']}")
    print(f"Byte-identical: {summary['byte_identical']}")
    print(f"Classified mismatches: {len(summary['mismatches'])}")
    for item in summary["mismatches"]:
        print(f"  {item['name']} [{item['category']}] "
              f"generated={item['generated_bytes']} reference={item['reference_bytes']}")
        print(f"    {item['evidence']}")
    print(f"Only in generated tree: {len(summary['only_generated'])}")
    for name in summary["only_generated"]:
        print(f"  {name}")
    print(f"Only in reference tree: {len(summary['only_reference'])}")
    for name in summary["only_reference"]:
        print(f"  {name}")


def _passed(summary):
    classified = summary["byte_identical"] + len(summary["mismatches"])
    return (summary["exports_decoded"] == EXPECTED_SCRIPT_FILES and
            summary["generated_files"] == EXPECTED_SCRIPT_FILES and
            summary["reference_files"] == EXPECTED_SCRIPT_FILES and
            classified == EXPECTED_SCRIPT_FILES and
            not summary["only_generated"] and
            not summary["only_reference"] and
            not any(item["category"] == "unclassified"
                    for item in summary["mismatches"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", type=Path)
    parser.add_argument("--reference-root", type=Path)
    parser.add_argument(
        "--output-dir", type=Path,
        help="keep generated .uc files here (must be absent or empty; default: temporary)")
    arguments = parser.parse_args()
    arguments.retail_root = arguments.retail_root or original_dir()
    arguments.reference_root = arguments.reference_root or exports_dir() / "script"

    if arguments.output_dir is not None:
        if arguments.output_dir.exists() and any(arguments.output_dir.iterdir()):
            parser.error(f"--output-dir must be empty: {arguments.output_dir}")
        summary = verify_script(
            arguments.retail_root, arguments.reference_root, arguments.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="verify-script-") as temp_dir:
            summary = verify_script(
                arguments.retail_root, arguments.reference_root, Path(temp_dir))
    print_summary(summary)
    return 0 if _passed(summary) else 1


if __name__ == "__main__":
    sys.exit(main())
