#!/usr/bin/env python3
"""Decode all pipeline meshes from retail packages and compare the James oracle.

The census comes from shipped ``#exec MESH IMPORT`` metadata, not a hard-coded
name list.  Byte differences receive a semantic comparison that is insensitive
to triangle/vertex ordering but preserves animation trajectories, corner UVs,
triangle types, texture slots, and shipped sequence metadata.
"""
import argparse
import json
import math
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import devtree_dir, exports_dir, original_dir  # noqa: E402

from mesh2glb import find_pair
from ue1pkg import Package, decode_mesh, encode_mesh_as_james
from ue3d import MTT_PLACEHOLDER, load_anim, load_data, parse_exec_metadata


ROOT = Path(__file__).resolve().parents[2]


def source_packages(retail_root):
    packages = sorted((Path(retail_root) / "System").glob("*.u"),
                      key=lambda path: path.name.lower())
    if not packages:
        raise FileNotFoundError(
            f"no retail System packages under {Path(retail_root) / 'System'}")
    return packages


def _mesh_exports(packages, names):
    wanted = set(names)
    result = defaultdict(list)
    for package_path in packages:
        package = Package(package_path)
        for export in package.exports:
            if (export["name"].casefold() in wanted and
                    package.objname(export["cls"]) in ("Mesh", "LodMesh")):
                result[export["name"].casefold()].append((package, export))
    return result


def _trajectories(frames, numverts):
    return [tuple(frame[index] for frame in frames) for index in range(numverts)]


def _semantic_parts(data, frames):
    trajectories = _trajectories(frames, data["numverts"])
    vertices = Counter(trajectories)
    geometry = Counter()
    uvs = Counter()
    types = Counter()
    textures = Counter()
    flags = Counter()
    complete = Counter()
    for triangle in data["tris"]:
        geom = tuple(trajectories[index] for index in triangle["idx"])
        triangle_type = triangle["type"]
        geometry[geom] += 1
        types[(geom, triangle_type)] += 1
        if (triangle_type & 0xF) == MTT_PLACEHOLDER:
            # LodMesh retains attachment geometry and type, but deliberately
            # discards import-only UV/color/texture bytes for invisible faces.
            complete[("attachment", geom, triangle_type)] += 1
        else:
            uv = tuple(tuple(value) for value in triangle["uvs"])
            uvs[(geom, uv)] += 1
            textures[(geom, triangle["tex"])] += 1
            flags[(geom, triangle["flags"])] += 1
            complete[("visible", geom, uv, triangle_type,
                      triangle["tex"], triangle["flags"])] += 1
    return dict(vertices=vertices, geometry=geometry, uvs=uvs, types=types,
                textures=textures, flags=flags, complete=complete)


def _counter_delta(left, right):
    return sum((left - right).values()), sum((right - left).values())


def semantic_differences(generated_data, generated_frames,
                         reference_data, reference_frames):
    differences = []
    if len(generated_frames) != len(reference_frames):
        differences.append(
            f"animation frames retail={len(generated_frames)} "
            f"devtree={len(reference_frames)}")
    if generated_data["numverts"] != reference_data["numverts"]:
        differences.append(
            f"frame vertices retail={generated_data['numverts']} "
            f"devtree={reference_data['numverts']}")
    if generated_data["numpolys"] != reference_data["numpolys"]:
        differences.append(
            f"polygons retail={generated_data['numpolys']} "
            f"devtree={reference_data['numpolys']}")
    # Component comparison still produces useful diagnostics when counts match.
    if (len(generated_frames) != len(reference_frames) or
            generated_data["numverts"] != reference_data["numverts"]):
        return differences

    generated = _semantic_parts(generated_data, generated_frames)
    reference = _semantic_parts(reference_data, reference_frames)
    labels = (
        ("vertices", "animated vertex trajectories"),
        ("geometry", "polygon geometry"),
        ("uvs", "visible-polygon UVs"),
        ("types", "triangle types"),
        ("textures", "visible-polygon texture indices"),
        ("flags", "James triangle flags"),
    )
    for key, label in labels:
        if generated[key] != reference[key]:
            retail_only, devtree_only = _counter_delta(generated[key], reference[key])
            differences.append(
                f"{label} differ (retail-only {retail_only}, "
                f"devtree-only {devtree_only})")
    return differences


def _normal_sequence_metadata(mesh):
    sequences = {}
    notifications = []
    for sequence in mesh["sequences"]:
        key = sequence["name"].casefold()
        sequences[key] = (sequence["start"], sequence["num"], sequence["rate"])
        notifications.extend(
            (key, notification["time"], notification["function"].casefold())
            for notification in sequence["notifications"])
    return sequences, sorted(notifications)


def _expected_sequence_metadata(metadata):
    sequences = {
        name.casefold(): (value["start"], value["num"], value["rate"])
        for name, value in metadata["sequences"].items()
    }
    notifications = sorted(
        ((item.get("seq") or "").casefold(), item["time"],
         (item.get("func") or "").casefold())
        for item in metadata["notifies"])
    return sequences, notifications


def _float_equal(left, right):
    return math.isclose(left, right, rel_tol=1e-6, abs_tol=1e-5)


def sequence_differences(mesh, metadata):
    actual, actual_notifications = _normal_sequence_metadata(mesh)
    expected, expected_notifications = _expected_sequence_metadata(metadata)
    differences = []
    actual_names = set(actual)
    expected_names = set(expected)
    if actual_names != expected_names:
        missing = sorted(expected_names - actual_names)
        extra = sorted(actual_names - expected_names)
        differences.append(
            f"sequences differ (missing {missing or '[]'}, extra {extra or '[]'})")
    changed = []
    for name in sorted(actual_names & expected_names):
        left, right = actual[name], expected[name]
        if (left[:2] != right[:2] or not _float_equal(left[2], right[2])):
            changed.append(
                f"{name}: retail=({left[0]},{left[1]},{left[2]:g}) "
                f"script=({right[0]},{right[1]},{right[2]:g})")
    if changed:
        differences.append("sequence ranges/rates differ: " + "; ".join(changed))
    if len(actual_notifications) != len(expected_notifications) or any(
            left[0] != right[0] or left[2] != right[2] or
            not _float_equal(left[1], right[1])
            for left, right in zip(actual_notifications, expected_notifications)):
        differences.append(
            f"animation notifications differ "
            f"(retail {len(actual_notifications)}, script {len(expected_notifications)})")
    return differences


def _decode_candidate(candidates, metadata):
    decoded = []
    errors = []
    unmirror = str(metadata.get("unmirror", "0")) != "0"
    for package, export in candidates:
        try:
            mesh = decode_mesh(package, export)
            pair = encode_mesh_as_james(mesh, unmirror=unmirror)
            decoded.append((package, mesh, pair))
        except Exception as exc:
            errors.append(f"{package.name}.{export['name']}: {exc}")
    if not decoded:
        raise ValueError("; ".join(errors) if errors else "no retail export")
    first_pair = decoded[0][2]
    conflicts = [package.name for package, _mesh, pair in decoded[1:]
                 if pair != first_pair]
    if conflicts:
        names = [decoded[0][0].name] + conflicts
        raise ValueError(f"conflicting duplicate retail exports in {names}")
    return decoded[0]


def verify_meshes(retail_root, script_root, reference_root, output_root):
    metadata = parse_exec_metadata(script_root)
    model_dirs = sorted((path for path in Path(reference_root).glob("*/Models")
                         if path.is_dir()), key=lambda path: str(path).lower())
    if not model_dirs:
        raise FileNotFoundError(f"no devtree Models directories under {reference_root}")
    output_root = Path(output_root)
    output_root.mkdir(parents=True, exist_ok=True)
    packages = source_packages(retail_root)
    exports = _mesh_exports(packages, metadata)
    counts = Counter()
    drift = []
    failures = []
    encoding = []

    for key, item in sorted(metadata.items(), key=lambda pair: pair[1]["name"].lower()):
        name = item["name"]
        try:
            package, mesh, pair = _decode_candidate(exports.get(key, []), item)
            generated_data_path = output_root / f"{name}_d.3d"
            generated_anim_path = output_root / f"{name}_a.3d"
            generated_data_path.write_bytes(pair[0])
            generated_anim_path.write_bytes(pair[1])
            reference_data_path, reference_anim_path = find_pair(item, model_dirs)
            data_equal = pair[0] == reference_data_path.read_bytes()
            anim_equal = pair[1] == reference_anim_path.read_bytes()
            sequence_changes = sequence_differences(mesh, item)
            if data_equal and anim_equal and not sequence_changes:
                counts["byte_identical"] += 1
                continue

            generated_data = load_data(generated_data_path)
            reference_data = load_data(reference_data_path)
            generated_frames = load_anim(
                generated_anim_path, generated_data["numverts"])
            reference_frames = load_anim(
                reference_anim_path, reference_data["numverts"])
            differences = semantic_differences(
                generated_data, generated_frames, reference_data, reference_frames)
            differences.extend(sequence_changes)
            byte_detail = dict(data_identical=data_equal, anim_identical=anim_equal)
            if differences:
                counts["drift"] += 1
                drift.append(dict(mesh=name, package=package.name,
                                  differences=differences, **byte_detail))
            else:
                counts["encoding_diff"] += 1
                encoding.append(dict(mesh=name, package=package.name, **byte_detail))
        except Exception as exc:
            counts["failures"] += 1
            failures.append(dict(mesh=name, error=str(exc)))

    total = len(metadata)
    return dict(
        total=total, packages=len(packages),
        byte_identical=counts["byte_identical"],
        encoding_diff=counts["encoding_diff"],
        drift_count=counts["drift"], failures_count=counts["failures"],
        drift=drift, failures=failures, encoding_differences=encoding)


def print_summary(summary):
    print(f"Retail packages scanned: {summary['packages']}")
    print(f"Pipeline meshes: {summary['total']}")
    print(f"Byte-identical: {summary['byte_identical']}")
    print(f"Identical content, encoding/order differs: {summary['encoding_diff']}")
    print(f"Retail-vs-devtree drift: {summary['drift_count']}")
    for item in summary["drift"]:
        print(f"  {item['mesh']} [{item['package']}]:")
        for difference in item["differences"]:
            print(f"    {difference}")
    print(f"Decode/verification failures: {summary['failures_count']}")
    for item in summary["failures"]:
        print(f"  {item['mesh']}: {item['error']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", type=Path)
    parser.add_argument("--script-root", type=Path)
    parser.add_argument("--reference-root", type=Path)
    parser.add_argument(
        "--output-dir", type=Path,
        help="keep generated James files here (must be absent or empty)")
    parser.add_argument("--json", type=Path, help="write the full classification JSON")
    arguments = parser.parse_args()
    arguments.retail_root = arguments.retail_root or original_dir()
    arguments.script_root = arguments.script_root or exports_dir() / "script"
    arguments.reference_root = (arguments.reference_root
                                or devtree_dir() / "U2Radiosity")

    if arguments.output_dir is not None:
        if arguments.output_dir.exists() and any(arguments.output_dir.iterdir()):
            parser.error(f"--output-dir must be empty: {arguments.output_dir}")
        summary = verify_meshes(
            arguments.retail_root, arguments.script_root,
            arguments.reference_root, arguments.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="verify-meshes-") as temp_dir:
            summary = verify_meshes(
                arguments.retail_root, arguments.script_root,
                arguments.reference_root, Path(temp_dir))
    if arguments.json:
        arguments.json.parent.mkdir(parents=True, exist_ok=True)
        arguments.json.write_text(json.dumps(summary, indent=1))
    print_summary(summary)
    classified = (summary["byte_identical"] + summary["encoding_diff"] +
                  summary["drift_count"] + summary["failures_count"])
    return 0 if classified == summary["total"] and not summary["failures"] else 1


if __name__ == "__main__":
    sys.exit(main())
