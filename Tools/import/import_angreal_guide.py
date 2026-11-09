#!/usr/bin/env python3
"""Derive the angreal field-guide facts and build the guide data asset.

Parse, derivation and editor halves of the guide importer
(docs/plans/2026-11-06-onboarding-aids.md, Tasks 1-3). Walks the private
reference export `exports/script/<Package>/*.uc`, records each class header
(`class X expands Y`) with its defaultproperties scalars and `Name(index)=`
arrays, and resolves inherited values through the parent chain the way the
original property system did. On top of the parse, `build_guide_rows` derives
the counter matrix and facts rows the plan specifies, `render_facts_csv`
renders them deterministically, and `--facts-only` regenerates the checked-in
`Tools/import/data/angreal_guide_facts.csv` and stops (pure python).

Without the flag the editor half then builds, from the facts CSV plus the
hint templates resolved onto rows as HintText:
  /Game/WoT/UI/Guide/DA_WoTAngrealGuide

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script=".../Tools/import/import_angreal_guide.py" -unattended -nullrhi

Completion is recorded in Saved/import_angreal_guide_result.json because
commandlet stdout is not a reliable completion signal on the parent hosts.

The angreal census (Tools/test/angreal/angreal_census.py) walks the same tree
with the same conventions (latin-1 reads, `class X expands|extends Y`
headers, lowercase class keys), but its parsing is census-specific — it
resolves the reference root at import time and only extracts spawn-pool
properties — so this module carries the standalone parser the plan specifies
instead of importing it.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools"))
sys.path.insert(0, str(REPO / "Tools" / "import"))
from wot_config import (  # noqa: E402
    ConfigurationError, PathLike, exports_dir, require_path)
from import_menu_strings import write_if_changed  # noqa: E402

CLASS_RE = re.compile(
    r"^\s*class\s+(\w+)\s+(?:expands|extends)\s+(\w+)", re.I | re.M)
PROP_RE = re.compile(r"^([A-Za-z_]\w*)(?:\((\d+)\))?=(.*)$")


@dataclass
class UcClass:
    """One exported class: header identity plus its defaultproperties."""

    name: str
    package: str
    parent: str
    props: Dict[str, str] = field(default_factory=dict)
    array_props: Dict[str, List[str]] = field(default_factory=dict)


def default_script_root(explicit: Optional[PathLike] = None) -> Path:
    """The exported-script root, resolved through wot_config precedence."""
    return require_path(exports_dir(explicit) / "script",
                        "exported script reference directory", directory=True)


def reference_root_available() -> bool:
    """True when the exported script tree is readable; gated tests skip otherwise."""
    try:
        return default_script_root().is_dir()
    except ConfigurationError:
        return False


def _unquote(value: str) -> str:
    # Whitespace around the value is syntax; a quoted value keeps its interior
    # exactly. The exported defaultproperties use no escape convention, so the
    # quotes are simply stripped.
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1]
    return value


def parse_script_classes(script_root: PathLike) -> Dict[str, UcClass]:
    """Map lowercase class name -> UcClass for every .uc under the root.

    Packages are the first directory level under the root
    (`exports/script/<Package>/`). Files are read as latin-1, the exporter's
    encoding. A file without a `class X expands Y` header carries no exported
    class and is skipped. Scalar duplicates keep the last value in file order;
    sparse `Name(index)=` arrays keep empty strings in the gaps. Files are
    visited in sorted path order, so the result is deterministic.
    """
    root = Path(script_root)
    classes: Dict[str, UcClass] = {}
    for uc in sorted(root.rglob("*.uc")):
        text = uc.read_text(encoding="latin-1", errors="replace")
        header = CLASS_RE.search(text)
        if not header:
            continue
        relative = uc.relative_to(root)
        parsed = UcClass(
            name=header.group(1),
            package=relative.parts[0] if len(relative.parts) > 1 else "",
            parent=header.group(2))
        in_defaults = False
        in_block = False
        for raw in text.splitlines():
            line = raw.strip()
            if not in_defaults:
                in_defaults = line.lower() == "defaultproperties"
                continue
            if not in_block:
                in_block = line == "{"
                continue
            if line == "}":
                break
            match = PROP_RE.match(line)
            if not match:
                continue
            prop, index, value = match.groups()
            if index is None:
                parsed.props[prop] = _unquote(value)
            else:
                entries = parsed.array_props.setdefault(prop, [])
                position = int(index)
                while len(entries) <= position:
                    entries.append("")
                entries[position] = _unquote(value)
        classes[parsed.name.lower()] = parsed
    return classes


def ancestors(classes: Dict[str, UcClass], name: str) -> List[str]:
    """Lowercase inheritance chain: the class itself, then each parent.

    The walk stops at the first class missing from the map (engine packages
    ship stripped, so every chain ends where the export does), at a cycle, or
    immediately — an empty list — when the class itself is unknown.
    """
    chain: List[str] = []
    current = name.lower()
    while current in classes and current not in chain:
        chain.append(current)
        current = classes[current].parent.lower()
    return chain


def resolved_prop(classes: Dict[str, UcClass], name: str,
                  prop: str) -> Optional[str]:
    """The nearest defaultproperties value for `prop` along the parent chain.

    Property names compare case-insensitively, matching the original property
    system; None when no class in the chain sets the property.
    """
    wanted = prop.lower()
    for link in ancestors(classes, name):
        for key, value in sorted(classes[link].props.items()):
            if key.lower() == wanted:
                return value
    return None


def resolved_bool(classes: Dict[str, UcClass], name: str, prop: str) -> bool:
    """resolved_prop read as an UnrealScript bool: only a literal True is True."""
    value = resolved_prop(classes, name, prop)
    return value is not None and value.strip().lower() == "true"


def resolved_array(classes: Dict[str, UcClass], name: str,
                   prop: str) -> List[str]:
    """The parent-chain resolution of a `Name(index)=` array.

    The original property system starts a subclass from a copy of its parent's
    defaults and then applies the subclass entries per index, so the merge
    walks base-first and lets nearer non-empty entries win their slot.
    """
    wanted = prop.lower()
    merged: List[str] = []
    for link in reversed(ancestors(classes, name)):
        for key, entries in sorted(classes[link].array_props.items()):
            if key.lower() != wanted:
                continue
            while len(merged) < len(entries):
                merged.append("")
            for index, entry in enumerate(entries):
                if entry:
                    merged[index] = entry
            break
    return merged


# --- Task 2: counter matrix, roster, facts CSV -------------------------------

DATA_DIR = REPO / "Tools" / "import" / "data"
FACTS_CSV_PATH = DATA_DIR / "angreal_guide_facts.csv"
OVERRIDES_CSV_PATH = DATA_DIR / "angreal_counter_overrides.csv"
HINTS_CSV_PATH = DATA_DIR / "angreal_guide_hints.csv"
EXPECTATIONS_JSON_PATH = (
    REPO / "Tools" / "test" / "angreal" / "angreal_expectations.json")

FACTS_COLUMNS = ("Class", "CppClass", "Elements", "Rarity", "Roles",
                 "MaxInitialCharges", "MaxCharges", "Counters", "HintKey")
OVERRIDE_COLUMNS = ("Class", "Field", "Value", "Supersedes", "Citation")
HINT_COLUMNS = ("HintKey", "Template")
MULTI_VALUE_FIELDS = ("Counters", "Elements", "Roles")
SCALAR_FIELD_ATTRS = {"Rarity": "rarity",
                      "MaxInitialCharges": "max_initial_charges",
                      "MaxCharges": "max_charges",
                      "HintKey": "hint_key"}

ELEMENT_BY_FLAG = {"bElementAir": "Air", "bElementEarth": "Earth",
                   "bElementFire": "Fire", "bElementSpirit": "Spirit",
                   "bElementWater": "Water"}
RARITY_BY_FLAG = {"bCommon": "Common", "bUncommon": "Uncommon",
                  "bRare": "Rare"}
ROLE_BY_FLAG = {"bCombat": "Combat", "bDefensive": "Defensive",
                "bOffensive": "Offensive"}

# Derivation anchors (lowercase, the parser's key form). Edges come only from
# the plan's counter rules:
# - An element shield's resolved ReflectorClasses names the matching
#   ignore-element reflector (AngrealInvFireShield.uc ->
#   IgnoreFireElementReflector); it counters every offensive roster row
#   carrying that element flag.
# - ReturnToSenderReflector counters every offensive roster row descending
#   from ProjectileLauncher (ReturnToSenderReflector.uc casts the projectile
#   back at the originator). Reflector identity resolves through the
#   reflector's own self-inclusive parent chain, so ForkRTSReflector
#   (ForkRTSReflector.uc:12, expands ReturnToSenderReflector and returns the
#   projectile via its Super) carries the same edges.
# - ReflectSeekerReflector counters the Seeker (ReflectSeekerReflector.uc
#   redirects a seeker at its originator; no shipped ReflectorClasses entry
#   names it, so on retail data this rule contributes no edge).
# - Trap detection counters the rows whose placed pieces the detector keeps.
#   It keeps a deployed Trap or an armed explosive ward only when the piece's
#   trap ID is in its own TrapIconInfos table (TrapDetectLeech.uc:203-233,
#   table at :286-291). The ID is a Trap's TrapType when set, else the class
#   name, and no shipped trap sets TrapType, so a subclass the table does not
#   name (FireWallSlab under WallSlab, Alarm) goes undetected. A trap row
#   places its ResourceClass plus that actor's SlabClass (Wall.uc builds the
#   barrier from slabs); a launcher row places its ProjectileClassName.
# - Curse removal counters the Taint (AngrealInvRemoveCurse.uc stops artifact
#   effects in radius, the lift for the Taint's permanent artifact curse).
IGNORE_REFLECTOR_ELEMENT = {
    "ignore%selementreflector" % element.lower(): element
    for element in ELEMENT_BY_FLAG.values()}
RETURN_TO_SENDER_REFLECTOR = "returntosenderreflector"
REFLECT_SEEKER_REFLECTOR = "reflectseekerreflector"
PROJECTILE_LAUNCHER_CLASS = "projectilelauncher"
SEEKER_CLASS = "angrealinvseeker"
TAINT_CLASS = "angrealinvtaint"
TRAP_DETECT_CLASS = "angrealinvtrapdetect"
TRAP_DETECT_LEECH_CLASS = "trapdetectleech"
EXP_WARD_PROJECTILE_CLASS = "angrealexpwardprojectile"
REMOVE_CURSE_CLASS = "angrealinvremovecurse"

TRAPS_PACKAGE = "wottraps"
WOT_INVENTORY_CLASS = "wotinventory"
TRAP_BASE_CLASS = "trap"
ANGREAL_ROW_PREFIX = "AngrealInv"
TRAP_ROW_SUFFIX = "Inventory"
OFFENSIVE_ROLE = "Offensive"

CLASS_REF_RE = re.compile(r"Class'(?:\w+\.)?(\w+)'", re.I)
TRAP_TYPE_RE = re.compile(r"\bTrapType=(\w+)", re.I)
BUDGET_ENTRY_RE = re.compile(
    r"ResourcePackageName=(\w+).*?ResourceAdapterName=(\w+)", re.I)
HINT_PLACEHOLDER_RE = re.compile(r"\{(\w*)\}")
HINT_PLACEHOLDERS = ("counter", "title")


class GenerationError(ValueError):
    """The guide facts cannot be derived as specified from the given data."""


@dataclass(frozen=True)
class RosterEntry:
    """One guide subject: an expectations angreal or a placeable trap."""

    class_name: str
    cpp_class: str
    is_trap: bool = False


@dataclass
class GuideRow:
    """One derived facts row; list fields stay sorted and ;-join in the CSV."""

    class_name: str
    cpp_class: str
    elements: List[str]
    rarity: str
    roles: List[str]
    max_initial_charges: str
    max_charges: str
    counters: List[str]
    hint_key: str


@dataclass(frozen=True)
class Override:
    """One angreal_counter_overrides.csv row, with its source for messages."""

    class_name: str
    field: str
    value: str
    supersedes: bool
    citation: str
    source: str


def _class_ref_name(value: str) -> Optional[str]:
    """The bare class name inside a Class'Package.Name' property value."""
    match = CLASS_REF_RE.search(value)
    return match.group(1) if match else None


def load_expectations_roster(
        path: Optional[PathLike] = None) -> List[RosterEntry]:
    """Every angreal_expectations.json entry as a roster entry.

    Entries map to exports classes through their `uc` file stem, which also
    collapses cast variants (`Shift@aimed`) onto the class that carries them;
    variants must agree on `cpp_class`.
    """
    source = Path(path) if path is not None else EXPECTATIONS_JSON_PATH
    document = json.loads(source.read_text(encoding="utf-8"))
    by_stem: Dict[str, str] = {}
    for entry in document["classes"]:
        stem = Path(entry["uc"]).stem
        cpp_class = entry["cpp_class"]
        previous = by_stem.get(stem)
        if previous is not None and previous != cpp_class:
            raise GenerationError(
                "%s: expectations variants disagree on cpp_class (%s vs %s)"
                % (stem, previous, cpp_class))
        by_stem[stem] = cpp_class
    return [RosterEntry(stem, by_stem[stem])
            for stem in sorted(by_stem, key=str.lower)]


def trap_roster(classes: Dict[str, UcClass]) -> List[RosterEntry]:
    """The placeable traps: WoTTraps inventories the shipped budgets sell.

    Criterion: a class named by a `Budget(n)=(ResourcePackageName=WOTTraps,
    ResourceAdapterName=...)` entry in any exported budget (all four shipped
    team budgets list the same seven, AesSedaiBudget.uc:15-21 and siblings;
    BudgetInfo.uc:73-86 resolves entries to WOTInventory classes), expanding
    WOTInventory, whose ResourceClass names a Trap-descended placed actor.
    Abstract bases, placed actors, slabs and effects never appear in a budget,
    so nothing is excluded by hand. The port's class naming convention gives
    `CppClass` (WoTPitInventory for PitInventory).
    """
    adapters = set()
    for key in sorted(classes):
        for entry in resolved_array(classes, classes[key].name, "Budget"):
            match = BUDGET_ENTRY_RE.search(entry)
            if match and match.group(1).lower() == TRAPS_PACKAGE:
                adapters.add(match.group(2).lower())
    entries: List[RosterEntry] = []
    problems: List[str] = []
    for adapter in sorted(adapters):
        parsed = classes.get(adapter)
        if parsed is None:
            problems.append("%s: budget names a class the export lacks"
                            % adapter)
            continue
        if (parsed.package.lower() != TRAPS_PACKAGE
                or WOT_INVENTORY_CLASS not in ancestors(classes,
                                                        adapter)[1:]):
            problems.append("%s: budget trap is not a WoTTraps WOTInventory"
                            % parsed.name)
            continue
        actor = _class_ref_name(
            resolved_prop(classes, adapter, "ResourceClass") or "")
        if actor is None or TRAP_BASE_CLASS not in ancestors(classes, actor):
            problems.append(
                "%s: ResourceClass does not name a Trap-descended actor"
                % parsed.name)
            continue
        entries.append(RosterEntry(parsed.name, "WoT" + parsed.name,
                                   is_trap=True))
    if problems:
        raise GenerationError("trap roster derivation failed:\n  "
                              + "\n  ".join(problems))
    return entries


def _read_csv_rows(path: Path, columns: tuple) -> List[tuple]:
    """CSV body rows (line number, cells) after validating the exact header."""
    with path.open(encoding="utf-8", newline="") as handle:
        rows = list(csv.reader(handle))
    if not rows or tuple(rows[0]) != columns:
        raise GenerationError("%s: header must be exactly %s"
                              % (path.name, ",".join(columns)))
    body = []
    for number, cells in enumerate(rows[1:], 2):
        if len(cells) != len(columns):
            raise GenerationError("%s:%d: expected %d fields, found %d"
                                  % (path.name, number, len(columns),
                                     len(cells)))
        body.append((number, cells))
    return body


def load_overrides_csv(path: Optional[PathLike] = None) -> List[Override]:
    """angreal_counter_overrides.csv rows, each with a verified-shape citation.

    Every row must cite the exports; `Supersedes` is yes, no or empty.
    Semantic enforcement (a strip or add without Supersedes=yes, a no-op row)
    happens in build_guide_rows where the derived facts are known.
    """
    source = Path(path) if path is not None else OVERRIDES_CSV_PATH
    overrides: List[Override] = []
    for number, cells in _read_csv_rows(source, OVERRIDE_COLUMNS):
        class_name, field_name, value, supersedes, citation = cells
        where = "%s:%d" % (source.name, number)
        if (field_name not in MULTI_VALUE_FIELDS
                and field_name not in SCALAR_FIELD_ATTRS):
            raise GenerationError("%s: unknown override field %r"
                                  % (where, field_name))
        if supersedes.strip().lower() not in ("", "no", "yes"):
            raise GenerationError("%s: Supersedes must be yes, no or empty"
                                  % where)
        if not citation.strip() or "exports/" not in citation:
            raise GenerationError(
                "%s: every override needs a real exports citation" % where)
        overrides.append(Override(
            class_name=class_name, field=field_name, value=value,
            supersedes=supersedes.strip().lower() == "yes",
            citation=citation, source=where))
    return overrides


def load_hints_csv(path: Optional[PathLike] = None) -> Dict[str, str]:
    """angreal_guide_hints.csv as HintKey -> Template, placeholders checked."""
    source = Path(path) if path is not None else HINTS_CSV_PATH
    hints: Dict[str, str] = {}
    for number, cells in _read_csv_rows(source, HINT_COLUMNS):
        key, template = cells
        where = "%s:%d" % (source.name, number)
        if not re.fullmatch(r"\w+", key):
            raise GenerationError("%s: HintKey must be a bare word" % where)
        if key in hints:
            raise GenerationError("%s: duplicate HintKey %s" % (where, key))
        if not template.strip():
            raise GenerationError("%s: empty hint template" % where)
        if not template.isascii():
            raise GenerationError("%s: hint template must be ASCII" % where)
        for match in HINT_PLACEHOLDER_RE.finditer(template):
            if match.group(1) not in HINT_PLACEHOLDERS:
                raise GenerationError(
                    "%s: unknown placeholder {%s} (templates resolve only "
                    "{title} and {counter} at runtime)"
                    % (where, match.group(1)))
        hints[key] = template
    return hints


def _derived_hint_key(entry: RosterEntry) -> str:
    """The default HintKey: the short class name the hints CSV is keyed by."""
    name = entry.class_name
    if entry.is_trap and name.endswith(TRAP_ROW_SUFFIX):
        return name[:-len(TRAP_ROW_SUFFIX)]
    if name.startswith(ANGREAL_ROW_PREFIX):
        return name[len(ANGREAL_ROW_PREFIX):]
    return name


def _placed_pieces(classes: Dict[str, UcClass], name: str) -> List[str]:
    """The actors a roster row puts in the world for the trap detector to find.

    A trap row places its ResourceClass and, for a wall, that actor's
    SlabClass; a launcher row places its ProjectileClassName.
    """
    pieces: List[str] = []
    actor = _class_ref_name(resolved_prop(classes, name, "ResourceClass") or "")
    if actor is not None:
        pieces.append(actor)
        slab = _class_ref_name(resolved_prop(classes, actor, "SlabClass") or "")
        if slab is not None:
            pieces.append(slab)
    projectile = resolved_prop(classes, name, "ProjectileClassName")
    if projectile:
        pieces.append(projectile.split(".")[-1])
    return pieces


def _trap_detect_ids(classes: Dict[str, UcClass]) -> List[str]:
    """The lowercase trap IDs the detector's TrapIconInfos table names."""
    ids: List[str] = []
    for entry in resolved_array(classes, TRAP_DETECT_LEECH_CLASS,
                                "TrapIconInfos"):
        match = TRAP_TYPE_RE.search(entry)
        if match:
            ids.append(match.group(1).lower())
    return ids


def _trap_detect_finds(classes: Dict[str, UcClass], name: str,
                       detectable: List[str]) -> bool:
    """True when the detector keeps one of the row's placed pieces."""
    for piece in _placed_pieces(classes, name):
        chain = ancestors(classes, piece)
        if TRAP_BASE_CLASS in chain:
            trap_id = resolved_prop(classes, piece, "TrapType") or piece
        elif EXP_WARD_PROJECTILE_CLASS in chain:
            trap_id = piece
        else:
            continue
        if trap_id.lower() in detectable:
            return True
    return False


def _derive_counters(classes: Dict[str, UcClass],
                     rows: Dict[str, GuideRow]) -> None:
    """Apply the plan's counter rules; see the anchor-constant comment."""
    offensive = [row for row in rows.values()
                 if OFFENSIVE_ROLE in row.roles]
    names_by_lower = {name.lower(): name for name in rows}
    detectable = _trap_detect_ids(classes)
    for row in rows.values():
        links = set()
        for reference in resolved_array(classes, row.class_name,
                                        "ReflectorClasses"):
            reflector = _class_ref_name(reference)
            if reflector is None:
                continue
            # Self-inclusive chain: an exported reflector matches by its own
            # name first; a stripped (unexported) one still matches by name.
            links.update(ancestors(classes, reflector)
                         or [reflector.lower()])
        counters = row.counters
        for link in sorted(links):
            element = IGNORE_REFLECTOR_ELEMENT.get(link)
            if element is not None:
                counters.extend(other.class_name for other in offensive
                                if element in other.elements)
        if RETURN_TO_SENDER_REFLECTOR in links:
            counters.extend(
                other.class_name for other in offensive
                if PROJECTILE_LAUNCHER_CLASS in ancestors(classes,
                                                          other.class_name))
        if (REFLECT_SEEKER_REFLECTOR in links
                and SEEKER_CLASS in names_by_lower):
            counters.append(names_by_lower[SEEKER_CLASS])
        if row.class_name.lower() == TRAP_DETECT_CLASS:
            counters.extend(
                other.class_name for other in rows.values()
                if _trap_detect_finds(classes, other.class_name, detectable))
        if (row.class_name.lower() == REMOVE_CURSE_CLASS
                and TAINT_CLASS in names_by_lower):
            counters.append(names_by_lower[TAINT_CLASS])
        row.counters = sorted(set(counters), key=str.lower)


def _apply_overrides(rows: Dict[str, GuideRow], overrides: List[Override],
                     rarity_unresolved: Dict[str, str],
                     errors: List[str]) -> None:
    """Fold the authored overrides in, enforcing the supersede rules.

    A row that changes nothing is an error (stale overrides must go); a row
    that contradicts a cleanly derived fact needs Supersedes=yes; a row that
    resolves a derivation error (the forced-override rarity case) needs only
    its citation.
    """
    for override in overrides:
        row = rows.get(override.class_name)
        if row is None:
            errors.append("%s: override names unknown roster class %s"
                          % (override.source, override.class_name))
            continue
        if override.field in MULTI_VALUE_FIELDS:
            value = override.value
            if len(value) < 2 or value[0] not in "+-":
                errors.append("%s: %s override value must be +Name or -Name"
                              % (override.source, override.field))
                continue
            member = value[1:]
            target = {"Counters": row.counters, "Elements": row.elements,
                      "Roles": row.roles}[override.field]
            if value[0] == "-":
                if member not in target:
                    errors.append(
                        "%s: strips %s that %s does not derive"
                        % (override.source, member, override.class_name))
                elif not override.supersedes:
                    errors.append(
                        "%s: contradicts derived %s without Supersedes=yes"
                        % (override.source, override.field))
                else:
                    target.remove(member)
            else:
                if member in target:
                    errors.append("%s: adds %s that is already derived"
                                  % (override.source, member))
                elif not override.supersedes:
                    errors.append(
                        "%s: adds a non-derived %s without Supersedes=yes"
                        % (override.source, override.field))
                elif override.field == "Counters" and member not in rows:
                    errors.append(
                        "%s: added counter %s is not a roster row"
                        % (override.source, member))
                else:
                    target.append(member)
                    target.sort(key=str.lower)
        else:
            attr = SCALAR_FIELD_ATTRS[override.field]
            if (override.field == "Rarity"
                    and override.class_name in rarity_unresolved):
                row.rarity = override.value
                del rarity_unresolved[override.class_name]
            elif override.value == getattr(row, attr):
                errors.append("%s: %s override changes nothing"
                              % (override.source, override.field))
            elif not override.supersedes:
                errors.append(
                    "%s: contradicts derived %s without Supersedes=yes"
                    % (override.source, override.field))
            else:
                setattr(row, attr, override.value)


def build_guide_rows(classes: Dict[str, UcClass],
                     roster: List[RosterEntry],
                     overrides: List[Override],
                     hints: Dict[str, str]) -> List[GuideRow]:
    """Derive one GuideRow per roster entry, or fail listing every problem.

    Hard failures: a rarity that resolves to zero or several flags with no
    override supplying the value; an override that is a no-op, contradicts a
    derived fact without Supersedes=yes, or names unknown classes; an
    offensive or trap row without a resolving hint; a hint row nothing uses;
    a {counter} template on a row that no other row counters.
    """
    errors: List[str] = []
    rows: Dict[str, GuideRow] = {}
    rarity_unresolved: Dict[str, str] = {}
    trap_set = {entry.class_name for entry in roster if entry.is_trap}

    for entry in sorted(roster, key=lambda item: item.class_name.lower()):
        name = entry.class_name
        if name in rows:
            errors.append("%s: duplicate roster entry" % name)
            continue
        if name.lower() not in classes:
            errors.append("%s: not in the exported script tree" % name)
            continue
        rarities = sorted(
            rarity for flag, rarity in RARITY_BY_FLAG.items()
            if resolved_bool(classes, name, flag))
        if len(rarities) == 1:
            rarity = rarities[0]
        else:
            rarity = ""
            rarity_unresolved[name] = (
                "/".join(rarities) if rarities else "none")
        rows[name] = GuideRow(
            class_name=name,
            cpp_class=entry.cpp_class,
            elements=sorted(
                element for flag, element in ELEMENT_BY_FLAG.items()
                if resolved_bool(classes, name, flag)),
            rarity=rarity,
            roles=sorted(role for flag, role in ROLE_BY_FLAG.items()
                         if resolved_bool(classes, name, flag)),
            max_initial_charges=resolved_prop(
                classes, name, "MaxInitialCharges") or "",
            max_charges=resolved_prop(classes, name, "MaxCharges") or "",
            counters=[],
            hint_key="")
    if errors:
        raise GenerationError("guide generation failed:\n  "
                              + "\n  ".join(errors))

    _derive_counters(classes, rows)
    for entry in roster:
        if entry.is_trap or OFFENSIVE_ROLE in rows[entry.class_name].roles:
            rows[entry.class_name].hint_key = _derived_hint_key(entry)
    _apply_overrides(rows, overrides, rarity_unresolved, errors)

    for name in sorted(rarity_unresolved, key=str.lower):
        errors.append(
            "%s: rarity flags resolve to %s; exactly one of "
            "bCommon/bUncommon/bRare must hold, so an override must supply "
            "Rarity with a citation" % (name, rarity_unresolved[name]))

    # {counter} resolves at runtime to a counter OF the row (the countered-by
    # relation the loader later derives by inverting the Counters column), so
    # a template may use it only when some other row counters this one.
    countered = set()
    for row in rows.values():
        countered.update(row.counters)
    used_keys: Dict[str, List[str]] = {}
    for name in sorted(rows, key=str.lower):
        row = rows[name]
        if row.hint_key:
            used_keys.setdefault(row.hint_key, []).append(name)
            if row.hint_key not in hints:
                errors.append("%s: HintKey %s is not in the hints CSV"
                              % (name, row.hint_key))
            elif ("{counter}" in hints[row.hint_key]
                    and name not in countered):
                errors.append(
                    "%s: hint %s uses {counter} but nothing counters the row"
                    % (name, row.hint_key))
        elif name in trap_set or OFFENSIVE_ROLE in row.roles:
            errors.append("%s: offensive and trap rows need a HintKey"
                          % name)
    for key, users in sorted(used_keys.items()):
        if len(users) > 1:
            errors.append("hint %s is shared by %s; hints are one per row"
                          % (key, "/".join(users)))
    for key in sorted(set(hints) - set(used_keys)):
        errors.append("hint %s is used by no facts row" % key)

    if errors:
        raise GenerationError("guide generation failed:\n  "
                              + "\n  ".join(errors))
    return [rows[name] for name in sorted(rows, key=str.lower)]


def _csv_field(value: str) -> str:
    # Identical quoting to import_menu_strings.render_csv, so the two
    # generated registries stay convention-identical.
    value = str(value)
    quoted = (value[:1].isspace() or value[-1:].isspace()
              or any(char in value for char in ',\"\r\n'))
    return '"%s"' % value.replace('"', '""') if quoted else value


def render_facts_csv(rows: List[GuideRow]) -> str:
    """The deterministic facts CSV: sorted rows, sorted ;-joins, LF, ASCII."""
    lines = [",".join(FACTS_COLUMNS)]
    for row in sorted(rows, key=lambda item: item.class_name.lower()):
        lines.append(",".join(_csv_field(value) for value in (
            row.class_name, row.cpp_class, ";".join(row.elements),
            row.rarity, ";".join(row.roles), row.max_initial_charges,
            row.max_charges, ";".join(row.counters), row.hint_key)))
    text = "\n".join(lines) + "\n"
    if not text.isascii():
        raise GenerationError("facts CSV must be ASCII")
    return text


def generate_rows() -> List[GuideRow]:
    """The real facts rows: exported script + expectations roster + data CSVs."""
    classes = parse_script_classes(default_script_root())
    roster = load_expectations_roster() + trap_roster(classes)
    return build_guide_rows(classes, roster, load_overrides_csv(),
                            load_hints_csv())


# --- Task 3: guide data asset (editor half) ----------------------------------

RESULT_FILE = REPO / "Saved" / "import_angreal_guide_result.json"
GUIDE_DESTINATION = "/Game/WoT/UI/Guide"
GUIDE_ASSET_NAME = "DA_WoTAngrealGuide"
GUIDE_ASSET_PATH = "%s/%s" % (GUIDE_DESTINATION, GUIDE_ASSET_NAME)
# The asset class lives in the WoTOnboarding plugin, not the game module.
GUIDE_CLASS_PATH = "/Script/WoTOnboarding.WoTAngrealGuide"

# Result-file pins: the roster can only grow, and the Fireball facts are the
# plan's representative row (identifiers and derived numbers, no retail
# prose). The hint text is our own wording, pinned by its placeholders rather
# than its sentence so the template stays tunable.
MIN_ASSET_ROWS = 25
FIREBALL_CLASS = "AngrealInvFireball"
FIREBALL_FACTS = {
    "cpp_class": "WoTAngrealInvFireball",
    "elements": ["Fire"],
    "rarity": "Common",
    "roles": ["Combat", "Offensive"],
    "max_initial_charges": 20,
    "max_charges": 35,
    "counters": [],
}


def _split_multi(value: str) -> List[str]:
    """A `;`-joined CSV cell back to its list; the empty cell is no entries."""
    return value.split(";") if value else []


def _charges_int(value: str, where: str) -> int:
    """A charge cell as the asset's int32: empty (no charge envelope) is 0."""
    if not value:
        return 0
    try:
        return int(value)
    except ValueError:
        raise GenerationError("%s: charge value %r is not an integer"
                              % (where, value))


def load_facts_csv(path: Optional[PathLike] = None) -> List[GuideRow]:
    """The checked-in facts CSV back as GuideRows, shape-validated."""
    source = Path(path) if path is not None else FACTS_CSV_PATH
    rows: List[GuideRow] = []
    seen = set()
    for number, cells in _read_csv_rows(source, FACTS_COLUMNS):
        (class_name, cpp_class, elements, rarity, roles,
         max_initial_charges, max_charges, counters, hint_key) = cells
        where = "%s:%d" % (source.name, number)
        if not class_name:
            raise GenerationError("%s: empty Class" % where)
        if class_name in seen:
            raise GenerationError("%s: duplicate Class %s"
                                  % (where, class_name))
        seen.add(class_name)
        rows.append(GuideRow(
            class_name=class_name, cpp_class=cpp_class,
            elements=_split_multi(elements), rarity=rarity,
            roles=_split_multi(roles),
            max_initial_charges=max_initial_charges,
            max_charges=max_charges, counters=_split_multi(counters),
            hint_key=hint_key))
    return rows


def asset_rows(rows: List[GuideRow], hints: Dict[str, str]) -> List[dict]:
    """The plain asset-row payload: facts plus HintText resolved per HintKey.

    One dict per facts row, keyed by the asset's property names; `{title}` and
    `{counter}` stay literal in HintText (they resolve at runtime). The
    payload is the single shape the importer sets, extracts and verifies, so
    the independent re-verification compares like with like.
    """
    payload = []
    for row in rows:
        if row.hint_key and row.hint_key not in hints:
            raise GenerationError("%s: HintKey %s is not in the hints CSV"
                                  % (row.class_name, row.hint_key))
        payload.append({
            "class_name": row.class_name,
            "cpp_class": row.cpp_class,
            "elements": list(row.elements),
            "rarity": row.rarity,
            "roles": list(row.roles),
            "max_initial_charges": _charges_int(
                row.max_initial_charges, row.class_name),
            "max_charges": _charges_int(row.max_charges, row.class_name),
            "counters": list(row.counters),
            "hint_text": hints[row.hint_key] if row.hint_key else "",
        })
    return payload


def verify_asset_payload(actual: List[dict], desired: List[dict]):
    """(ok, pins, counts) for the re-loaded asset against the desired payload."""
    by_class = {row["class_name"]: row for row in actual}
    fireball = by_class.get(FIREBALL_CLASS, {})
    pins = {
        "row_count_at_least_%d" % MIN_ASSET_ROWS:
            len(actual) >= MIN_ASSET_ROWS,
        "fireball_present": FIREBALL_CLASS in by_class,
    }
    for key, expected in sorted(FIREBALL_FACTS.items()):
        pins["fireball_%s" % key] = fireball.get(key) == expected
    hint_text = fireball.get("hint_text") or ""
    pins["fireball_hint_has_placeholders"] = (
        "{title}" in hint_text and "{counter}" in hint_text)
    counts = {"rows": len(actual), "unique_classes": len(by_class)}
    ok = actual == desired and all(pins.values())
    return ok, pins, counts


def write_result(result: dict) -> None:
    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULT_FILE.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def payload_json(payload: List[dict]) -> str:
    """The {"rows": [...]} document UWoTAngrealGuide.ImportRowsFromJson eats."""
    return json.dumps({"rows": payload}, sort_keys=True)


def payload_from_json(text: str) -> List[dict]:
    """ExportRowsAsJson back to the payload shape (row order preserved)."""
    document = json.loads(text)
    if not isinstance(document, dict) or not isinstance(
            document.get("rows"), list):
        raise GenerationError("asset export carries no rows array")
    return document["rows"]


def import_asset(payload: List[dict]):
    import unreal

    eal = unreal.EditorAssetLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    asset_class = unreal.load_class(None, GUIDE_CLASS_PATH)
    if not asset_class:
        raise RuntimeError(
            "UWoTAngrealGuide is not loaded; build the editor with the "
            "WoTOnboarding plugin enabled first")

    if not eal.does_directory_exist(GUIDE_DESTINATION):
        eal.make_directory(GUIDE_DESTINATION)
    created = not eal.does_asset_exist(GUIDE_ASSET_PATH)
    if created:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", asset_class)
        asset = tools.create_asset(
            GUIDE_ASSET_NAME, GUIDE_DESTINATION, asset_class, factory)
    else:
        asset = unreal.load_asset(GUIDE_ASSET_PATH)
    if not asset:
        raise RuntimeError("failed to create or load %s" % GUIDE_ASSET_PATH)

    # Rows cross the seam as JSON through reflection-callable methods on the
    # asset itself: the editor's python glue generates no wrapper types for a
    # plugin module loaded after it (unreal.WoTAngrealGuideRow does not
    # exist), while call_method needs only the reflection registry.
    changed = created
    if payload_from_json(str(asset.call_method("ExportRowsAsJson"))) != payload:
        error = str(asset.call_method(
            "ImportRowsFromJson", args=(payload_json(payload),)))
        if error:
            raise RuntimeError("row import failed: %s" % error)
        changed = True
    if not eal.save_asset(GUIDE_ASSET_PATH):
        raise RuntimeError("save failed for %s" % GUIDE_ASSET_PATH)

    package = asset.get_outermost()
    reloaded, reload_error = unreal.EditorLoadingAndSavingUtils.reload_packages(
        [package], unreal.ReloadPackagesInteractionMode.ASSUME_NEGATIVE)
    if not reloaded:
        raise RuntimeError("post-save reload failed for %s: %s"
                           % (GUIDE_ASSET_PATH, reload_error))

    # Result-file verification reads the reloaded package, including the
    # roster and Fireball pins, rather than trusting the live edited object.
    verified = unreal.load_asset(GUIDE_ASSET_PATH)
    if not verified:
        raise RuntimeError("post-save load failed for %s" % GUIDE_ASSET_PATH)
    ok, pins, counts = verify_asset_payload(
        payload_from_json(str(verified.call_method("ExportRowsAsJson"))),
        payload)
    if not ok:
        raise RuntimeError(
            "post-save verification failed for %s: pins=%s, counts=%s"
            % (GUIDE_ASSET_PATH, pins, counts))
    return changed, pins, counts


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="derive the guide facts CSV and build the guide data "
                    "asset (--facts-only stops after the CSV, no Unreal)")
    parser.add_argument("--facts-only", action="store_true",
                        help="regenerate %s from the exported script and the "
                             "authored data CSVs, then stop"
                             % FACTS_CSV_PATH.relative_to(REPO))
    args = parser.parse_args(argv)
    rows = generate_rows()
    csv_changed = write_if_changed(FACTS_CSV_PATH, render_facts_csv(rows))
    print("facts CSV %s: %d rows, %s"
          % (FACTS_CSV_PATH.relative_to(REPO), len(rows),
             "updated" if csv_changed else "no change"))
    if args.facts_only:
        return 0
    result = {
        "asset": GUIDE_ASSET_PATH,
        "csv": str(FACTS_CSV_PATH),
        "csv_changed": csv_changed,
        "status": "running",
    }
    write_result(result)
    try:
        payload = asset_rows(load_facts_csv(), load_hints_csv())
        result["rows"] = len(payload)
        changed, pins, imported_counts = import_asset(payload)
        result.update({
            "asset_changed": changed,
            "imported_counts": imported_counts,
            "pins": pins,
            "status": "ok",
        })
    except Exception as exc:
        result["status"] = "failed"
        result["error"] = str(exc)
        write_result(result)
        raise
    write_result(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
