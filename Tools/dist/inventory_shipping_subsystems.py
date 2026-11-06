#!/usr/bin/env python3
"""Inventory every subsystem class and its active registrations under Source/WoT."""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

FORMAT = "shipping-subsystem-dormancy-v1"
BASES = {
    "UWorldSubsystem",
    "UTickableWorldSubsystem",
    "UGameInstanceSubsystem",
}
REGISTRATION_PATTERNS = (
    ("world-delegate", re.compile(r"\bFWorldDelegates::\w+\s*\.\s*Add")),
    ("object-delegate", re.compile(
        r"\bFCoreUObjectDelegates::\w+[^;{}]*?\.\s*Add", re.S)),
    ("actor-spawn-handler", re.compile(r"\bAddOnActorSpawnedHandler\s*\(")),
    ("ticker", re.compile(r"\bAddTicker\s*\(")),
    ("timer", re.compile(r"\bSetTimer(?:ForNextTick)?\s*\(")),
    ("begin-play", re.compile(r"\bOnWorldBeginPlay\s*\.\s*Add")),
    ("network-delegate", re.compile(
        r"\b(?:OnNetworkFailure|OnTravelFailure)\s*\(\s*\)\s*\.\s*Add")),
    ("disconnect-delegate", re.compile(
        r"\bGetHandleDisconnectDelegate\s*\(\s*\)\s*\.\s*Add")),
    ("online-delegate", re.compile(
        r"\b(?:AddOn\w+Delegate_Handle|On\w+Delegates\s*\.\s*AddUObject)\s*\(")),
    ("tick-function", re.compile(r"\.\s*RegisterTickFunction\s*\(")),
    ("always-active-tick", re.compile(
        r"\bPrimaryActorTick\s*\.\s*bCanEverTick\s*=\s*true\b")),
)


class InventoryFailed(Exception):
    """Shipping subsystem discovery or policy closure is incomplete."""


@dataclass(frozen=True)
class Subsystem:
    class_name: str
    base: str
    header: str
    implementation: str
    bootstrap_policy: str
    activation_prerequisite: str
    comment: str


@dataclass(frozen=True)
class Registration:
    class_name: str
    kind: str
    implementation: str
    line: int
    activation_prerequisite: str
    guard_required: bool
    guard_dominated: bool


@dataclass(frozen=True)
class Inventory:
    subsystems: tuple[Subsystem, ...]
    registrations: tuple[Registration, ...]


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise InventoryFailed(f"cannot read {path}: {exc}") from exc


def _relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def _load_policy(path: Path) -> tuple[str, dict[str, dict]]:
    try:
        document = json.loads(_read(path))
    except json.JSONDecodeError as exc:
        raise InventoryFailed(f"invalid policy JSON: {exc}") from exc
    if not isinstance(document, dict) or set(document) != {
            "format", "guard_define", "policies"}:
        raise InventoryFailed(
            "policy must contain only format, guard_define, policies")
    if document["format"] != FORMAT:
        raise InventoryFailed(f"policy format must be {FORMAT}")
    if not isinstance(document["guard_define"], str):
        raise InventoryFailed("policy guard_define must be a string")
    if not isinstance(document["policies"], list):
        raise InventoryFailed("policy policies must be an array")
    expected_fields = {
        "class", "header", "implementation", "bootstrap_policy",
        "activation_prerequisite", "forbidden_pre_activation", "comment",
    }
    policies: dict[str, dict] = {}
    for index, row in enumerate(document["policies"]):
        if not isinstance(row, dict) or set(row) != expected_fields:
            raise InventoryFailed(f"policy row {index} has a non-canonical shape")
        if (not all(isinstance(row[field], str) for field in expected_fields
                    - {"forbidden_pre_activation"})
                or not isinstance(row["forbidden_pre_activation"], list)
                or not row["forbidden_pre_activation"]
                or not all(isinstance(item, str) and item
                           for item in row["forbidden_pre_activation"])):
            raise InventoryFailed(f"policy row {index} has invalid fields")
        if row["class"] in policies:
            raise InventoryFailed(f"duplicate policy for {row['class']}")
        policies[row["class"]] = row
    return document["guard_define"], policies


def _discover(source_root: Path) -> dict[str, tuple[str, Path]]:
    pattern = re.compile(
        r"\bclass(?:\s+\w+_API)?\s+(U\w+)\s*:\s*public\s+"
        r"(UWorldSubsystem|UTickableWorldSubsystem|UGameInstanceSubsystem)\b")
    discovered: dict[str, tuple[str, Path]] = {}
    for header in sorted(source_root.rglob("*.h")):
        text = _read(header)
        for match in pattern.finditer(text):
            class_name, base = match.groups()
            if class_name in discovered:
                raise InventoryFailed(f"duplicate subsystem definition {class_name}")
            discovered[class_name] = (base, header)
    return discovered


def _implementation_for(class_name: str, source_root: Path,
                        preferred_relative: str) -> Path:
    matches = []
    token = class_name + "::"
    for source in sorted(source_root.rglob("*.cpp")):
        if token in _read(source):
            matches.append(source)
    preferred = source_root / preferred_relative
    if not matches:
        # Marker-only subsystems such as UWoTZoneIndex deliberately have no
        # methods. Their policy maps to the actual actor/component behavior
        # owner, rather than inventing a class solely to satisfy the inventory.
        if preferred.is_file():
            return preferred
        raise InventoryFailed(f"no implementation found for {class_name}")
    if len(matches) > 1:
        # A class may split methods across files. Policy names the behavior owner
        # containing lifecycle/registration code; prefer that deterministically.
        active = [path for path in matches if re.search(
            rf"{re.escape(class_name)}::(?:Initialize|OnWorldBeginPlay|ShouldCreateSubsystem)",
            _read(path))]
        if len(active) == 1:
            return active[0]
        if preferred in matches:
            return preferred
        raise InventoryFailed(
            f"multiple lifecycle implementations found for {class_name}: "
            + ", ".join(path.name for path in matches))
    return matches[0]


def _matching_brace(masked: str, opening: int) -> int:
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise InventoryFailed("unterminated function body in subsystem source")


def _method_ranges(masked: str, class_name: str) -> list[tuple[int, int, int]]:
    result = []
    pattern = re.compile(
        rf"\b{re.escape(class_name)}\s*::\s*\w+\s*\([^;{{}}]*\)"
        rf"\s*(?:const\s*)?(?:&&\s*)?\{{", re.S)
    for match in pattern.finditer(masked):
        opening = masked.find("{", match.start(), match.end())
        result.append((match.start(), opening, _matching_brace(masked, opening)))
    return result


def _all_method_ranges(masked: str) -> list[tuple[str, int, int, int]]:
    result = []
    pattern = re.compile(
        r"\b([A-Za-z_]\w*)\s*::\s*~?\w+\s*\([^;{}]*\)"
        r"\s*(?:const\s*)?(?:&&\s*)?\{", re.S)
    for match in pattern.finditer(masked):
        opening = masked.find("{", match.start(), match.end())
        result.append((match.group(1), match.start(), opening,
                       _matching_brace(masked, opening)))
    return result


def _shipping_excluded(text: str, offset: int) -> bool:
    values = {
        "WOT_GUARDED_BOOTSTRAP": True,
        "UE_BUILD_SHIPPING": True,
        "WOT_WITH_BOOTSTRAP_TEST_HARNESSES": False,
    }
    stack: list[bool] = []
    for line in text[:offset].splitlines():
        stripped = line.strip()
        match = re.match(r"#\s*if\s+(.+)$", stripped)
        if match:
            expression = match.group(1)
            known = None
            for name, value in values.items():
                if re.fullmatch(rf"!?\s*{name}", expression.strip()):
                    known = (not value if expression.strip().startswith("!")
                             else value)
                    break
            stack.append(known is False)
        elif re.match(r"#\s*else\b", stripped) and stack:
            stack[-1] = not stack[-1]
        elif re.match(r"#\s*endif\b", stripped) and stack:
            stack.pop()
    return any(stack)


def _depth_at(body: str, offset: int) -> int:
    return body[:offset].count("{") - body[:offset].count("}")


def _matching_parenthesis(text: str, opening: int) -> int:
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    raise InventoryFailed("unterminated condition in subsystem source")


def _guard_dominates(body: str, sink_offset: int, prerequisite: str) -> bool:
    escaped = re.escape(prerequisite)
    for match in re.compile(r"\bif\s*\(").finditer(body, 0, sink_offset):
        if _depth_at(body, match.start()) != 0:
            continue
        opening = body.find("(", match.start(), match.end())
        closing = _matching_parenthesis(body, opening)
        if closing >= sink_offset:
            continue
        cursor = closing + 1
        while cursor < sink_offset and body[cursor].isspace():
            cursor += 1
        returns = False
        if cursor < sink_offset and body[cursor] == "{":
            block_end = _matching_brace(body, cursor)
            returns = (block_end < sink_offset
                       and re.search(r"\breturn\b", body[cursor:block_end]))
        else:
            returns = bool(re.match(r"return\b", body[cursor:sink_offset]))
        if not returns:
            continue
        condition = body[opening + 1:closing]
        negative = re.compile(rf"!\s*{escaped}\s*\([^)]*\)")
        # In a disjunction, any false prerequisite forces this return path.
        if any(negative.fullmatch(part.strip())
               for part in condition.split("||")):
            return True
    positive = re.compile(
        rf"\bif\s*\(\s*{escaped}\s*\([^)]*\)\s*\)\s*\{{", re.S)
    for match in positive.finditer(body, 0, sink_offset):
        if _depth_at(body, match.start()) != 0:
            continue
        opening = body.find("{", match.start(), match.end())
        if opening >= 0 and _matching_brace(body, opening) > sink_offset:
            return True
    return False


def _registrations(source: Path, source_root: Path,
                   prerequisites: dict[str, str],
                   readiness_owners: set[str],
                   compiled_inert_owners: set[str]) -> list[Registration]:
    text = _read(source)
    masked = re.sub(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|"
        r"'(?:\\.|[^'\\])*'",
        lambda match: re.sub(r"[^\n]", " ", match.group(0)), text,
        flags=re.S)
    ranges = _all_method_ranges(masked)
    result: list[Registration] = []
    for kind, pattern in REGISTRATION_PATTERNS:
        for match in pattern.finditer(masked):
            owner = next((item for item in ranges
                          if item[2] < match.start() < item[3]), None)
            if owner is None or _shipping_excluded(text, match.start()):
                continue
            class_name, _start, opening, _closing = owner
            body = masked[opening + 1:_closing]
            sink_offset = match.start() - opening - 1
            prerequisite = prerequisites.get(class_name, "")
            if not prerequisite and class_name in readiness_owners:
                prerequisite = "IsWoTBootstrapWorldReady"
            guard_required = bool(prerequisite)
            dominated = (class_name in compiled_inert_owners
                         or (guard_required and _guard_dominates(
                             body, sink_offset, prerequisite)))
            result.append(Registration(
                class_name, kind, _relative(source, source_root),
                text.count("\n", 0, match.start()) + 1,
                prerequisite, guard_required, dominated))
    return result


def _readiness_listener_owners(source_root: Path) -> set[str]:
    owners = set()
    for source in sorted(source_root.rglob("*.cpp")):
        masked = re.sub(
            r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|"
            r"'(?:\\.|[^'\\])*'",
            lambda match: re.sub(r"[^\n]", " ", match.group(0)),
            _read(source), flags=re.S)
        for class_name, _start, opening, closing in _all_method_ranges(masked):
            if ("RegisterWoTBootstrapWorldReadinessListener"
                    in masked[opening + 1:closing]):
                owners.add(class_name)
    return owners


def _compiled_inert_owners(source_root: Path) -> set[str]:
    owners = set()
    pattern = re.compile(
        r"\b(U\w+)\s*::\s*ShouldCreateSubsystem\s*\([^;{}]*\)"
        r"\s*const\s*\{(.*?)\}", re.S)
    for source in sorted(source_root.rglob("*.cpp")):
        for match in pattern.finditer(_read(source)):
            body = match.group(2)
            if ("WOT_GUARDED_BOOTSTRAP && UE_BUILD_SHIPPING" in body
                    and re.search(r"\breturn\s+false\s*;", body)):
                owners.add(match.group(1))
    return owners


def inventory_tree(target: Path, source_root: Path, policy_path: Path,
                   *, require_bootstrap_guards: bool) -> Inventory:
    guard_define, policies = _load_policy(policy_path)
    target_text = _read(target)
    if (require_bootstrap_guards
            and re.search(rf"\b{re.escape(guard_define)}\s*=\s*1\b",
                          target_text) is None):
        raise InventoryFailed(
            f"target does not bind guarded Shipping definition {guard_define}=1")

    discovered = _discover(source_root)
    missing = sorted(set(discovered) - set(policies))
    extra = sorted(set(policies) - set(discovered))
    if missing:
        raise InventoryFailed(
            "unclassified Shipping subsystem(s): " + ", ".join(missing))
    if extra:
        raise InventoryFailed(
            "policy names nonexistent subsystem(s): " + ", ".join(extra))

    subsystems: list[Subsystem] = []
    registrations: list[Registration] = []
    for class_name in sorted(discovered):
        base, header = discovered[class_name]
        row = policies[class_name]
        implementation = _implementation_for(
            class_name, source_root, row["implementation"])
        actual_header = _relative(header, source_root)
        actual_implementation = _relative(implementation, source_root)
        if row["header"] != actual_header:
            raise InventoryFailed(
                f"header mismatch for {class_name}: policy {row['header']}, "
                f"actual {actual_header}")
        if row["implementation"] != actual_implementation:
            raise InventoryFailed(
                f"implementation mismatch for {class_name}: "
                f"policy {row['implementation']}, actual {actual_implementation}")

        subsystems.append(Subsystem(
            class_name, base, actual_header, actual_implementation,
            row["bootstrap_policy"], row["activation_prerequisite"],
            row["comment"]))

    prerequisites = {
        class_name: row["activation_prerequisite"]
        for class_name, row in policies.items()
    }
    readiness_owners = _readiness_listener_owners(source_root)
    compiled_inert_owners = _compiled_inert_owners(source_root)
    for source in sorted(source_root.rglob("*.cpp")):
        registrations.extend(_registrations(
            source, source_root, prerequisites, readiness_owners,
            compiled_inert_owners))
    undominated = [item for item in registrations
                   if item.guard_required and not item.guard_dominated]
    if require_bootstrap_guards and undominated:
        first = undominated[0]
        raise InventoryFailed(
            f"registration without bootstrap guard in {first.class_name}: "
            f"{first.implementation}:{first.line} is not dominated by "
            f"{first.activation_prerequisite}")

    registrations.sort(
        key=lambda item: (item.implementation, item.line, item.kind))
    return Inventory(tuple(subsystems), tuple(registrations))


def _write_output(path: Path, result: Inventory, target: Path,
                  source_root: Path) -> None:
    document = {
        "format": FORMAT,
        "target": target.as_posix(),
        "source_root": source_root.as_posix(),
        "subsystems": [asdict(item) for item in result.subsystems],
        "registrations": [asdict(item) for item in result.registrations],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--require-bootstrap-guards", action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = inventory_tree(
            args.target, args.source_root, args.policy,
            require_bootstrap_guards=args.require_bootstrap_guards)
        _write_output(args.output, result, args.target, args.source_root)
    except InventoryFailed as exc:
        print(f"Shipping subsystem inventory failed: {exc}", file=sys.stderr)
        return 1
    print(
        f"Shipping subsystem inventory passed: {len(result.subsystems)} "
        f"subsystems, {len(result.registrations)} active registrations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
