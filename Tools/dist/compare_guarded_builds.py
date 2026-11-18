#!/usr/bin/env python3
"""Compare two guarded builds of ONE commit, e.g. --compile-cache off vs on.

The fast pipeline (persistent build root + guarded import of only the shipping
producers) must produce a release equivalent to the clean-ISO pipeline. This
reads two finished ISOs -- nothing is rebuilt, nothing is written except the
optional report -- and classifies every difference:

* ``image``: compiled executables/symbols (WoT-<P>-<Config>.exe/.pdb and
  game symbols). MSVC /experimental:deterministic still embeds
  full source/PDB paths (UBT VCToolChain.cs warns C5049), and two ISOs never
  share a project path, so these differ between ANY two runs. Their compiled
  receipts (definitions, checks, configuration, engine identity) must match.
* ``staging-manifest``: UAT Manifest_*.txt, which record source timestamps;
  they must match once the timestamp column is removed.
* ``regenerated``: a cooked bootstrap/neutral-shell map whose GENERATED source
  (the generator's own output-identity receipt) differs between the ISOs, and
  cooked data derived from those maps (WoT/AssetRegistry.bin). Both pipelines
  regenerate these maps every run.
* ``unexpected``: anything else. Any unexpected difference fails the
  comparison.

Cooked containers are compared entry by entry from the UnrealPak -List logs
every guarded cook already writes (logs/cooked-container-inventory*.log), so a
differing .ucas/.utoc/.pak is explained -- or not -- package by package.

Stdlib only; exit 0 = equivalent, 1 = different or unreadable.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path


CONFIGURATIONS = ("Test", "Shipping")
RELEASE_DIRECTORIES = {"Test": "release", "Shipping": "release-shipping"}
SUFFIXES = {"Test": "", "Shipping": "-shipping"}
TARGETS = ("WoT",)
IMAGE_PATTERN = re.compile(
    r"(^|/)(WoT-(Win64|Mac)-(Test|Shipping)(\.exe|\.pdb)?"
    # A re-signed Mac bundle's seal covers the images above.
    r"|_CodeSignature/CodeResources)$")
MANIFEST_PATTERN = re.compile(r"(^|/)Manifest_[A-Za-z]+_[A-Za-z0-9]+\.txt$")
CONTAINER_SUFFIXES = (".pak", ".utoc", ".ucas")
ENTRY_PATTERN = re.compile(
    r'"([^"]+)"\s+offset:\s*\d+,\s*size:\s*(\d+)\s*bytes,\s*'
    r"(?:hash|sha1):\s*([0-9a-fA-F]+)")
GENERATED_MAP_PATTERN = re.compile(
    r"(WoT/Content/WoT/Maps/WoTBootstrap|"
    r"WoT/Plugins/WoTNeutralShells/Content/WoTMapShell\d\d)\.(umap|uexp)$")
# Cooked data that summarizes every package, so a regenerated map moves it.
DERIVED_FROM_GENERATED = ("WoT/AssetRegistry.bin", "<ContainerHeader>")


class CompareError(RuntimeError):
    """An input needed for the comparison is missing or malformed."""


def _json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CompareError(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise CompareError(f"{path} is not a JSON object")
    return value


def _stamp(iso: Path, number: int, step: str) -> dict:
    return _json(iso / ".stamps" / f"{number:02d}-{step}.json")


def zip_members(path: Path) -> dict[str, dict]:
    members = {}
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            digest = hashlib.sha256()
            with archive.open(info) as handle:
                for chunk in iter(lambda: handle.read(1 << 20), b""):
                    digest.update(chunk)
            if info.filename.startswith("WoT Reborn/Game/"):
                name = info.filename[len("WoT Reborn/Game/"):]
            else:
                name = info.filename.split("/", 1)[1] if "/" in info.filename \
                    else info.filename
            members[name] = {"size": info.file_size,
                             "sha256": digest.hexdigest()}
    return members


def normalized_manifest(archive: Path, member: str) -> list[str]:
    with zipfile.ZipFile(archive) as handle:
        name = next(info.filename for info in handle.infolist()
                    if info.filename.endswith("/" + member)
                    or info.filename == member)
        text = handle.read(name).decode("utf-8", errors="replace")
    return sorted(line.split("\t", 1)[0] for line in text.splitlines()
                  if line.strip())


def cooked_entries(log: Path) -> dict[tuple[str, str], tuple[int, str]]:
    rows = {}
    container = None
    try:
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise CompareError(f"cannot read cooked inventory log {log}") from exc
    for line in lines:
        if line.startswith("COMMAND "):
            tokens = line.split()
            container = tokens[-2].replace("\\", "/").rsplit("/", 1)[-1]
            continue
        match = ENTRY_PATTERN.search(line)
        if match:
            rows[(container, match.group(1))] = (
                int(match.group(2)), match.group(3).lower())
    if not rows:
        raise CompareError(f"no cooked entries in {log}")
    return rows


def generated_sources(iso: Path) -> dict[str, str]:
    """Generated map -> sha256 of its generated (pre-cook) package."""
    result = {}
    receipts = iso / "Saved" / "ImportReceipts"
    shells = receipts / "neutral-shells-output-identity.json"
    if shells.is_file():
        for row in _json(shells).get("packages", []):
            result["WoT/Plugins/WoTNeutralShells/Content/"
                   + str(row.get("relative_path", "")).rsplit(".", 1)[0]] = \
                row.get("sha256")
    bootstrap = iso / "Content" / "WoT" / "Maps" / "WoTBootstrap.umap"
    if bootstrap.is_file():
        result["WoT/Content/WoT/Maps/WoTBootstrap"] = hashlib.sha256(
            bootstrap.read_bytes()).hexdigest()
    return result


def _receipt_identity(path: Path) -> dict:
    value = _json(path)
    return {key: value.get(key) for key in (
        "format", "target", "platform", "configuration", "definitions",
        "checks", "modules_inspected", "engine", "commit", "tag",
        "toolchain")}


class Comparison:
    def __init__(self) -> None:
        self.rows: list[dict] = []

    def add(self, area: str, item: str, verdict: str, detail=None) -> None:
        self.rows.append({"area": area, "item": item, "class": verdict,
                          "detail": detail})

    @property
    def unexpected(self) -> list[dict]:
        return [row for row in self.rows if row["class"] == "unexpected"]


def compare_configuration(baseline: Path, candidate: Path, platform: str,
                          configuration: str, result: Comparison) -> None:
    area = f"{configuration}"
    releases = []
    for iso in (baseline, candidate):
        manifest_path = iso / RELEASE_DIRECTORIES[configuration] / "release.json"
        manifest = _json(manifest_path)
        releases.append((manifest_path, manifest))
    (base_path, base), (cand_path, cand) = releases
    for key in ("format", "configuration", "platform", "git_commit", "asset",
                "source_snapshot", "engine_identity",
                "neutral_support_authority"):
        if base.get(key) != cand.get(key):
            result.add(area, f"release.json:{key}", "unexpected",
                       {"baseline": base.get(key), "candidate": cand.get(key)})
    for target in TARGETS:
        compiled = []
        for manifest in (base, cand):
            pair = (manifest.get("target_receipts") or {}).get(target) or {}
            receipt = pair.get("compiled_receipt") or {}
            compiled.append({key: receipt.get(key) for key in (
                "definitions", "checks", "configuration", "target",
                "platform", "modules_inspected", "engine")})
        if compiled[0] != compiled[1]:
            result.add(area, f"compiled receipt {target}", "unexpected",
                       {"baseline": compiled[0], "candidate": compiled[1]})
    for key in ("paths", "validation"):
        values = [((manifest.get("cooked_container_inventory") or {})
                   .get("receipt") or {}).get(key) for manifest in (base, cand)]
        if values[0] != values[1]:
            result.add(area, f"cooked inventory {key}", "unexpected")

    base_members = zip_members(base_path.parent / base["asset"])
    cand_members = zip_members(cand_path.parent / cand["asset"])
    for name in sorted(set(base_members) ^ set(cand_members)):
        result.add(area, f"zip member {name}", "unexpected",
                   "present in only one release")
    base_entries = cooked_entries(
        baseline / "logs" / f"cooked-container-inventory"
        f"{SUFFIXES[configuration]}.log")
    cand_entries = cooked_entries(
        candidate / "logs" / f"cooked-container-inventory"
        f"{SUFFIXES[configuration]}.log")
    candidate_sources = generated_sources(candidate)
    regenerated = {
        path for path, digest in generated_sources(baseline).items()
        if candidate_sources.get(path) != digest}
    entry_rows = []
    for key in sorted(set(base_entries) | set(cand_entries)):
        if base_entries.get(key) == cand_entries.get(key):
            continue
        container, entry = key
        normalized = entry.replace("../../../", "")
        match = GENERATED_MAP_PATTERN.search(normalized)
        if key not in base_entries or key not in cand_entries:
            verdict = "unexpected"
        elif match and match.group(1) in regenerated:
            verdict = "regenerated"
        elif normalized in DERIVED_FROM_GENERATED and regenerated:
            verdict = "regenerated"
        else:
            verdict = "unexpected"
        entry_rows.append((container, entry, verdict))
        result.add(area, f"cooked {container}:{entry}", verdict)
    for name in sorted(set(base_members) & set(cand_members)):
        if base_members[name] == cand_members[name]:
            result.add(area, f"zip member {name}", "identical")
            continue
        if IMAGE_PATTERN.search(name):
            result.add(area, f"zip member {name}", "image", {
                "baseline_size": base_members[name]["size"],
                "candidate_size": cand_members[name]["size"]})
        elif MANIFEST_PATTERN.search(name):
            same = (normalized_manifest(base_path.parent / base["asset"], name)
                    == normalized_manifest(cand_path.parent / cand["asset"],
                                           name))
            result.add(area, f"zip member {name}",
                       "staging-manifest" if same else "unexpected")
        elif name.endswith(CONTAINER_SUFFIXES):
            container = name.rsplit("/", 1)[-1]
            stem = container.rsplit(".", 1)[0]
            related = [row for row in entry_rows
                       if str(row[0]).rsplit(".", 1)[0] == stem]
            explained = bool(related) and all(
                row[2] == "regenerated" for row in related)
            result.add(area, f"zip member {name}",
                       "regenerated" if explained else "unexpected",
                       {"differing_entries": len(related)})
        else:
            result.add(area, f"zip member {name}", "unexpected")


def compare(baseline: Path, candidate: Path, *,
            configurations=CONFIGURATIONS,
            allow_different_commits: bool = False) -> dict:
    seeds = [_stamp(iso, 1, "seed") for iso in (baseline, candidate)]
    identity = [(seed.get("release_commit"),
                 (seed.get("source_snapshot") or {}).get("sha256"))
                for seed in seeds]
    if identity[0] != identity[1] and not allow_different_commits:
        raise CompareError(
            f"the ISOs were seeded from different sources: {identity}")
    cooks = [_stamp(iso, 4, "cook") for iso in (baseline, candidate)]
    platform = cooks[0].get("platform")
    if platform != cooks[1].get("platform"):
        raise CompareError("the ISOs were built for different platforms")
    result = Comparison()
    for key in ("maps", "neutral_support_authority", "reference_closure"):
        if key == "reference_closure" and not all(key in cook for cook in cooks):
            continue
        values = [cook.get(key) for cook in cooks]
        if key == "reference_closure":
            values = [{field: (value or {}).get(field) for field in (
                "packages", "closure_sha256")} for value in values]
        if values[0] != values[1]:
            result.add("cook", key, "unexpected",
                       {"baseline": values[0], "candidate": values[1]})
    modes = [((cook.get("compile_cache") or {}).get("mode") or "clean-iso")
             for cook in cooks]
    result.add("cook", "compile_cache.mode", "mode",
               {"baseline": modes[0], "candidate": modes[1]})
    for configuration in configurations:
        present = [(iso / RELEASE_DIRECTORIES[configuration]
                    / "release.json").is_file()
                   for iso in (baseline, candidate)]
        if not any(present):
            continue
        if not all(present):
            result.add(configuration, "release.json", "unexpected",
                       "present in only one ISO")
            continue
        compare_configuration(baseline, candidate, platform, configuration,
                              result)
    counts: dict[str, int] = {}
    for row in result.rows:
        counts[row["class"]] = counts.get(row["class"], 0) + 1
    return {
        "format": "wot-guarded-build-comparison-v1",
        "baseline": str(baseline),
        "candidate": str(candidate),
        "platform": platform,
        "verdict": "equivalent" if not result.unexpected else "different",
        "counts": counts,
        "unexpected": result.unexpected,
        "rows": result.rows,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--configuration", choices=(*CONFIGURATIONS, "all"),
                        default="all")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--allow-different-commits", action="store_true")
    args = parser.parse_args(argv)
    configurations = (CONFIGURATIONS if args.configuration == "all"
                      else (args.configuration,))
    try:
        report = compare(args.baseline.resolve(), args.candidate.resolve(),
                         configurations=configurations,
                         allow_different_commits=args.allow_different_commits)
    except (CompareError, OSError, KeyError, zipfile.BadZipFile) as exc:
        print(f"COMPARE GUARDED BUILDS ERROR: {exc}", file=sys.stderr)
        return 1
    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"COMPARE {report['verdict'].upper()} {report['counts']}")
    for row in report["unexpected"][:40]:
        print(f"  UNEXPECTED [{row['area']}] {row['item']}")
    return 0 if report["verdict"] == "equivalent" else 1


if __name__ == "__main__":
    raise SystemExit(main())
