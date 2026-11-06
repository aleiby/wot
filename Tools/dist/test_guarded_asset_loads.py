#!/usr/bin/env python3
"""Static guard for packaged gameplay bypasses of authenticated WoT assets."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / "Source" / "WoT"
TARGET_ROOT = re.compile(r"/Game/WoT/(?:Textures|Sounds|Meshes)/", re.I)

# These files are the reviewed loading boundaries, not gameplay consumers.
# Keep this list narrow and give every entry an auditable reason.
ALLOWLIST = {
    "Assets/WoTRuntimeAssets.cpp":
        "authenticated texture/sound resolver and its development fallback",
    "Mesh/WoTRuntimeVertexMesh.cpp":
        "authenticated vertex-mesh resolver",
    "WoTMenuStyle.cpp":
        "front-end texture injection boundary authenticated by the menu provider",
    "WoTSound.cpp":
        "sound resolver boundary",
    "WoTVertexAnimComponent.cpp":
        "B3 vertex-mesh and skin-override boundary",
}


def guarded_source(text: str) -> str:
    """Select WOT_GUARDED_BOOTSTRAP=1 while preserving source line numbers."""
    active = True
    stack: list[tuple[bool, bool | None, bool]] = []
    output: list[str] = []
    for line in text.splitlines(keepends=True):
        match = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if not match:
            output.append(line if active else ("\n" if line.endswith("\n") else ""))
            continue
        directive, expression = match.group(1), match.group(2).strip()
        if directive in {"if", "ifdef", "ifndef"}:
            parent = active
            condition: bool | None = None
            if "WOT_GUARDED_BOOTSTRAP" in expression:
                if directive == "ifndef" or re.search(
                        r"!\s*WOT_GUARDED_BOOTSTRAP", expression):
                    condition = False
                else:
                    condition = True
            stack.append((parent, condition, False))
            active = parent and (condition is not False)
        elif directive == "else" and stack:
            parent, condition, _ = stack[-1]
            stack[-1] = (parent, condition, True)
            active = parent and (condition is not True)
        elif directive == "elif" and stack:
            parent, previous, _ = stack[-1]
            condition = None
            if "WOT_GUARDED_BOOTSTRAP" in expression:
                condition = not bool(re.search(
                    r"!\s*WOT_GUARDED_BOOTSTRAP", expression))
            stack[-1] = (parent, condition, True)
            active = parent and (condition is not False) and previous is not True
        elif directive == "endif" and stack:
            parent, _, _ = stack.pop()
            active = parent
        output.append("\n" if line.endswith("\n") else "")
    return "".join(output)


def direct_load_violations(text: str) -> list[tuple[int, str]]:
    """Return guarded direct loads of target roots as (line, explanation)."""
    text = guarded_source(text)
    target_vars: set[str] = set()
    for statement in text.split(";"):
        if not TARGET_ROOT.search(statement):
            continue
        declarations = re.findall(
            r"(?:FSoftObjectPath|TSoftObjectPtr\s*<[^>]+>)\s+(\w+)", statement)
        assignments = re.findall(r"\b(\w+)\s*=", statement)
        containers = re.findall(r"\b(\w+)\s*\.\s*Add\s*\(", statement)
        target_vars.update(declarations + assignments + containers)

    violations: list[tuple[int, str]] = []
    call_pattern = re.compile(
        r"LoadObject\s*<|LoadSynchronous\s*\(|TryLoad\s*\(|\.\s*Get\s*\(")
    for match in call_pattern.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        statement_start = text.rfind(";", 0, match.start()) + 1
        statement_end = text.find(";", match.end())
        if statement_end < 0:
            statement_end = len(text)
        statement = text[statement_start:statement_end]
        kind = match.group(0).strip()
        if kind.startswith("LoadObject"):
            if TARGET_ROOT.search(statement) or any(
                    re.search(rf"\b{re.escape(name)}\b", statement)
                    for name in target_vars):
                violations.append((line, "direct LoadObject boundary"))
            continue
        if TARGET_ROOT.search(statement):
            violations.append((line, f"direct {kind} on a WoT asset path"))
            continue
        prefix = text[max(statement_start, match.start() - 160):match.start()]
        receiver = re.search(r"(\w+)(?:\s*\[[^\]]+\])?\s*(?:->|\.)\s*$", prefix)
        if receiver and receiver.group(1) in target_vars:
            violations.append((line, f"direct {kind} through {receiver.group(1)}"))
        elif "LoadSynchronous" in kind:
            # Guarded gameplay has no legitimate synchronous soft-object load;
            # registries and development paths are removed above or allowlisted.
            violations.append((line, "guarded LoadSynchronous bypass"))
    return violations


def repository_violations() -> list[str]:
    failures: list[str] = []
    for path in sorted(SOURCE.rglob("*.cpp")):
        relative = path.relative_to(SOURCE).as_posix()
        if relative.startswith(("Tests/", "Testing/")) or relative in ALLOWLIST:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for line, reason in direct_load_violations(text):
            failures.append(f"{relative}:{line}: {reason}")
    return failures


class GuardedAssetLoadTest(unittest.TestCase):
    def test_preprocessor_selects_guarded_side(self) -> None:
        source = """#if !WOT_GUARDED_BOOTSTRAP
bad();
#else
good();
#endif
"""
        selected = guarded_source(source)
        self.assertNotIn("bad()", selected)
        self.assertIn("good()", selected)

    def test_direct_and_bound_bypasses_are_detected(self) -> None:
        source = """
void Direct() {
  FSoftObjectPath(TEXT("/Game/WoT/Textures/P/T.T")).TryLoad();
}
void Bound() {
  const FSoftObjectPath Path(TEXT("/Game/WoT/Meshes/M/M.M"));
  Path.TryLoad();
}
"""
        self.assertEqual([line for line, _ in direct_load_violations(source)], [3, 7])

    def test_development_only_bypass_is_ignored(self) -> None:
        source = """#if !WOT_GUARDED_BOOTSTRAP
FSoftObjectPath(TEXT("/Game/WoT/Sounds/P/S.S")).TryLoad();
#endif
"""
        self.assertEqual(direct_load_violations(source), [])

    def test_repository_has_no_unreviewed_bypass(self) -> None:
        failures = repository_violations()
        self.assertEqual(failures, [], "\n" + "\n".join(failures))


if __name__ == "__main__":
    unittest.main()
