#!/usr/bin/env python3
"""Remove module intermediates the target's CURRENT link does not consume.

An incremental guarded build (build_guarded_ue58 -Incremental/--incremental,
the persistent build root) keeps UBT's intermediates between runs.
record_build_receipt.py gates a release on the WOT_* macros it reads from
every Intermediate/Build/<Platform>/<arch>/<Target>/<Config>/*/Definitions.h.
A module that an earlier build compiled but this build no longer contains
would leave its Definitions.h behind, and its stale values could then satisfy
(or contradict) the gate. A clean build never has that problem, because the
wrapper's -Clean deleted the whole target directory first.

The link response file UBT writes for THIS build is the authority for which
module directories belong to it: every object the product is linked from is
named there (VCToolChain/MacToolChain link actions; the name is
UEToolChain.GetResponseFileName = <IntermediateDirectory>/<output name>.rsp,
rewritten on content change via WriteFileIfChanged). In a clean build the
module directory set and the set the link names are EQUAL (measured:
393/393 for WoT Win64 Shipping), so deleting every module directory the link
does not name restores exactly the evidence set a clean build would leave.
Anything this cannot prove -- no link response file, more than one, a link
that names no module, or a named module with no directory -- fails closed.

Stdlib only.
"""
from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path


OBJECT_SUFFIXES = (".obj", ".o", ".lib", ".a", ".res")


class PruneRejected(Exception):
    """The current module set cannot be proven; do not record a receipt."""


def _config_directories(project_dir: Path, target: str, platform: str,
                        configuration: str) -> list[Path]:
    base = project_dir / "Intermediate" / "Build" / platform
    candidates = [path for path in base.glob(f"*/{target}/{configuration}")
                  if path.is_dir()]
    archless = base / target / configuration
    if archless.is_dir():
        candidates.append(archless)
    return sorted(set(candidates))


def link_response_file(config_dir: Path, target: str, platform: str,
                       configuration: str) -> Path:
    stem = (target if configuration == "Development"
            else f"{target}-{platform}-{configuration}")
    names = {f"{stem}.exe.rsp", f"{stem}.rsp", f"{stem}.dll.rsp"}
    found = sorted(path for path in config_dir.iterdir()
                   if path.is_file() and path.name in names)
    if len(found) != 1:
        raise PruneRejected(
            f"expected exactly one link response file for {stem} in "
            f"{config_dir}, found {[path.name for path in found]}")
    return found[0]


_TOKEN = re.compile(r'"([^"]*)"|(\S+)')


def _tokens(text: str) -> list[str]:
    """Quoted or bare arguments; backslashes are kept (Windows paths)."""
    return [match.group(1) if match.group(1) is not None
            else match.group(2)
            for match in _TOKEN.finditer(text)]


def consumed_modules(response_text: str, config_dir: Path,
                     project_dir: Path) -> set[str]:
    """Module directory names the link names under this config directory."""
    tail = config_dir.relative_to(project_dir).as_posix().casefold() + "/"
    modules = set()
    for token in _tokens(response_text):
        normalized = token.replace("\\", "/")
        if not normalized.casefold().endswith(OBJECT_SUFFIXES):
            continue
        index = normalized.casefold().find(tail)
        if index < 0:
            continue
        remainder = normalized[index + len(tail):]
        if "/" not in remainder:
            continue
        module = remainder.split("/", 1)[0]
        if module and module not in (".", ".."):
            modules.add(module)
    return modules


def prune(project_dir: Path, target: str, platform: str,
          configuration: str) -> dict:
    project_dir = project_dir.resolve()
    config_dirs = _config_directories(
        project_dir, target, platform, configuration)
    if not config_dirs:
        raise PruneRejected(
            f"no intermediates for {target}/{platform}/{configuration} under "
            f"{project_dir}; build first")
    summary = {}
    for config_dir in config_dirs:
        response = link_response_file(
            config_dir, target, platform, configuration)
        consumed = consumed_modules(
            response.read_text(encoding="utf-8", errors="replace"),
            config_dir, project_dir)
        if not consumed:
            raise PruneRejected(
                f"{response} names no module objects under {config_dir}")
        present = {path.name: path for path in config_dir.iterdir()
                   if path.is_dir()}
        present_folded = {name.casefold() for name in present}
        consumed_folded = {name.casefold() for name in consumed}
        missing = sorted(name for name in consumed
                         if name.casefold() not in present_folded)
        if missing:
            raise PruneRejected(
                f"{response} links modules with no intermediate directory: "
                + ", ".join(missing))
        stale = sorted(name for name in present
                       if name.casefold() not in consumed_folded)
        for name in stale:
            shutil.rmtree(present[name])
        summary[config_dir.relative_to(project_dir).as_posix()] = {
            "link_response_file": response.name,
            "linked_modules": len(consumed),
            "pruned": stale,
        }
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--target", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--configuration", required=True)
    args = parser.parse_args(argv)
    project_dir = args.project.parent if args.project.suffix else args.project
    try:
        summary = prune(project_dir, args.target, args.platform,
                        args.configuration)
    except (PruneRejected, OSError) as exc:
        print(f"PRUNE STALE MODULES REJECTED: {exc}", file=sys.stderr)
        return 1
    for relative, row in summary.items():
        print(f"PRUNE STALE MODULES OK {relative}: "
              f"{row['linked_modules']} linked, "
              f"{len(row['pruned'])} pruned {row['pruned']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
