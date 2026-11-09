# Onboarding aids — implementation plan

**Status:** in progress (branch `feature/onboarding-aids`) · **Date:** 2026-11-06

The multiplayer-only port drops the original game's single-player ramp, so a
new player meets ~38 ter'angreal and their counter relationships with no
teacher. This plan adds three always-available teaching surfaces — a browsable
**field guide**, a **first-pickup info card**, and a **death-recap counter
hint** ("Fireball — Fire Shield absorbs fire weaves") — as an **optional
improvement** that a parity-pure build can switch off or omit entirely.

It revises a 2026-07 design for three new realities: the repository is public,
the angreal verification corpus (`Tools/test/angreal/`) now exists, and the
project policy separates retail parity from improvements.

## Architecture

1. **All behavior lives in a game plugin, `Plugins/WoTOnboarding/`** (runtime
   module `WoTOnboarding`, enabled in `WoT.uproject`). A parity build disables
   the plugin (or ships without it); a normal build carries a master setting
   **`bEnableOnboardingAids` (default ON)** plus one toggle per in-match aid
   (both default ON). Parity-capture and golden-compare lanes run with the
   plugin off so no overlay ever touches a comparison frame.
2. **The game module exposes one tiny seam header and nothing else.**
   `Source/WoT/WoTOnboardingSeams.h` carries plain-type delegates the game
   module broadcasts whether or not anything listens:
   - `FWoTClientItemAdded(FName ClassName)` — from
     `UWoTInventoryComponent::OnRep_Inventory` (and the server-side add path,
     so listen hosts fire it too) for item types newly present;
   - `FWoTClientDeathMessage(const FString& Line)` — from the
     `'WOTDeathMessage'` client branch in the player controller;
   - `FWoTShowGuideRequested()` — from a `ShowGuide` input action;
   - a registration list for extra menu rows (label callback + invoke
     callback), consumed by the in-game menu and front-end menu;
   - a registration list for extra settings rows, consumed by the settings
     screen;
   - `FWoTResolveRetailText` — a resolver the game module installs at startup
     so the plugin can request display text by (file, section, property)
     through the same runtime mechanism the menus use (`WoTRetailText`).
     The plugin links none of the game module's classes; the seams are the
     entire boundary. This is deliberately the seed of a general extension
     surface: a future improvement module reuses the same seams.
3. **Data is facts-only in the tree; retail prose only at runtime.** Per the
   contribution rules, retail text is never compiled in or committed. The
   guide's checked-in data carries *derived facts with citations* — exactly
   the precedent `Tools/test/angreal/angreal_expectations.json` set — and the
   player's own localization files supply Title/Description/Quote text at
   runtime through the retail-text resolver.
4. **The roster authority is the existing census.** The guide covers exactly
   the multiplayer-obtainable classes `Tools/test/angreal/angreal_census.py`
   derives (default inventory + reachable spawn-pool entries + fixed pickups)
   plus placeable traps; the completeness gate cross-references
   `angreal_expectations.json` (in-tree, so the gate runs without reference
   data).

## Data files

| File | Status | Contents |
|---|---|---|
| `Tools/import/data/angreal_guide_facts.csv` | generated, **checked in** | one row per item: `Class, CppClass, Elements, Rarity, Roles, MaxInitialCharges, MaxCharges, Counters, HintKey` — facts only, no prose |
| `Tools/import/data/angreal_counter_overrides.csv` | authored | `Class, Field, Value, Supersedes, Citation` — every edge removed or added by hand carries an `exports/script/...` citation |
| `Tools/import/data/angreal_guide_hints.csv` | authored | `HintKey, Template` — our own counter-hint templates (`{title}`/`{counter}` resolved at runtime; no retail text) |

`Tools/import/import_angreal_guide.py` derives the facts CSV from the exported
script (`$WOT_REFERENCE_ROOT`), and, run under the editor's python commandlet,
builds `/Game/WoT/UI/Guide/DA_WoTAngrealGuide` from the three CSVs — the same
parse → deterministic CSV → data-asset pattern as
`Tools/import/import_menu_strings.py`, including the `Saved/…_result.json`
completion record.

**Counter derivation rules** (edges come only from these; everything else is
hint text):

- An element shield (its `ReflectorClasses` names the matching
  ignore-element reflector) counters every offensive row carrying that
  element flag (`bElementFire` → Fire, and likewise Air/Earth/Water/Spirit).
- `ReturnToSenderReflector` counters every offensive row descending from
  `ProjectileLauncher`.
- `ReflectSeekerReflector` counters the Seeker.
- Trap detection counters every placeable trap row.
- Curse removal counters the Taint.
- Overrides may add or strip edges only with `Supersedes=yes` and a citation
  (the canonical example: Balefire's travel-through-everything behavior strips
  it from every shield's derived counters, cited to
  `exports/script/Angreal/AngrealInvBalefire.uc`).

Rarity comes from exactly one of `bCommon/bUncommon/bRare` resolved through
the parent chain; zero or several is a generation error that forces an
override or exclusion. Roles come from `bOffensive/bDefensive/bCombat`.

## Global constraints

- Simulation, replication and damage stay untouched. Everything here is
  client-side presentation, menus, config and generated data. The seam
  broadcasts are behavior-neutral when unbound.
- No retail-derived prose in the tree: not in CSVs, fixtures, specs, comments
  or commit messages. Citations instead (`AngrealInvFireball.uc`).
- Python is 3.9-compatible (`Optional[X]`, no `match`), stdlib-only, and
  deterministic (sorted rows, write-if-changed, LF). Tests are `unittest`,
  colocated, run as
  `PYTHONPATH=Tools/import python3 -m unittest Tools.import.test_import_angreal_guide`.
  Tests that read `$WOT_REFERENCE_ROOT` skip cleanly when it is unset.
- C++ builds on BOTH platforms before a task is done (the compilers disagree;
  see CONTRIBUTING "Building"). Only one UE build runs per machine at a time.
- Automation specs run headless (`-nullrhi -unattended`), live under leaf
  names in `WoT.Onboarding.*` (a spec whose full name prefixes another's is
  silently skipped), and any spec that creates a world must end play and shut
  the game instance down in teardown.
- Exported plugin symbols use the module's API macro (`WOTONBOARDING_API`),
  including friends of exported free functions (MSVC links fail otherwise).
- `PORT_NOTES.md` gains its entry in the same pull request that lands the
  player-visible behavior (Task 7).

---

### Task 0: Contributor-funnel dry run

Work in a fresh clone of the public repository, configured exactly as
CONTRIBUTING and `docs/LOCAL-CONFIGURATION.md` describe (reference root from
an owned copy, `Tools/local.json` for engine roots). Record every pothole —
a doc gap, a script that assumes a private path, a test that cannot skip —
and file each as an issue rather than fixing it silently. The clone, branch
and this plan file are the first produce of the run. Building the editor and
running the native suites completes this task; it can finish after Tasks 1-2
since the python layer needs no engine.

### Task 1: Facts parse layer (python, no unreal)

**Files:** create `Tools/import/import_angreal_guide.py` (parse half),
`Tools/import/test_import_angreal_guide.py`.

Read `Tools/test/angreal/angreal_census.py` first and reuse its approach (and
any reusable helper) for walking `exports/script/*/*.uc`: class header
(`class X expands Y`), `defaultproperties` scalars and `Name(index)=` arrays,
parent-chain resolution for inherited flags, and the census's reachability
output for the roster. Public interfaces for later tasks:

```python
parse_script_classes(script_root) -> Dict[str, UcClass]
  # UcClass: name, package, parent, props: Dict[str,str], array_props: Dict[str, List[str]]
resolved_prop(classes, name, prop) -> Optional[str]   # parent-chain walk
resolved_bool(classes, name, prop) -> bool
ancestors(classes, name) -> List[str]  # lowercase chain, self-inclusive (class itself first)
```

Tests: synthetic `.uc` fixtures written to a temp dir (invented classes and
text, nothing retail), covering scalar/array/inheritance/unquoting; plus
reference-gated pins (unittest.skipUnless guards on `reference_root_available()`)
asserting the real Fireball resolves element Fire, rarity Common, role
Offensive, and the real Fire Shield's `ReflectorClasses` names the
fire-element ignore reflector.

### Task 2: Counter matrix, roster, facts CSV

**Files:** extend the two Task 1 files; author
`angreal_counter_overrides.csv` and `angreal_guide_hints.csv`; generate and
commit `angreal_guide_facts.csv`.

Implement the derivation rules above plus:

```python
build_guide_rows(classes, census_obtainable, overrides) -> List[GuideRow]
render_facts_csv(rows) -> str
generate_rows() -> List[GuideRow]        # wires real paths + census
```

The roster is the census-obtainable set plus placeable traps; any class in
the Angreal/WoTTraps packages outside the roster needs no row (the census
already encodes reachability — no separate exclusions file). Hints: one per
offensive row, our own wording, derived default ("<Title placeholder> —
countered by …" is rendered at runtime from resolved names; the hint CSV
stores sentence *templates* keyed per class so wording stays tunable).

Ungated tests: derivation on synthetic fixtures (shield edge, return-to-sender
edge, override strip with citation enforcement, rarity-conflict error);
completeness against `angreal_expectations.json` — every entry in its `order`
has a facts row and vice versa, and `cpp_class` values agree. Reference-gated
test: freshly generated CSV matches the checked-in file byte-for-byte.

### Task 3: Plugin scaffold, seams, data asset, importer

**Files:** create `Plugins/WoTOnboarding/WoTOnboarding.uplugin`,
`Plugins/WoTOnboarding/Source/WoTOnboarding/` (module build rules,
`WoTAngrealGuide.h/.cpp`); create `Source/WoT/WoTOnboardingSeams.h/.cpp`;
modify `WoT.uproject` (plugin entry), the inventory component, the player
controller's death-message branch, and `import_angreal_guide.py` (editor
half).

- `UWoTAngrealGuide : UDataAsset` lives in the plugin
  (`/Script/WoTOnboarding.WoTAngrealGuide`): `TArray<FWoTAngrealGuideRow>
  Rows` (facts + `HintText` from the hints CSV), `FindByClassName`,
  `FindByTitleIn(const FString&)` (longest contained resolved-title match,
  for recap resolution), `CounteredBy(FName)`.
- Seams header as in Architecture §2; the game module broadcasts from the two
  hook sites and installs the retail-text resolver at startup; everything
  no-ops when nothing is bound (a game-module spec asserts exactly that).
- Importer builds the asset from the three CSVs; result-file verification
  pins row count and the Fireball row.
- Both platforms build with the plugin enabled AND disabled (disable locally
  via `WoT.uproject` edit or `-DisablePlugins=WoTOnboarding` equivalent lane;
  the seams compile either way).

### Task 4: Settings, master switch, guide key

**Files:** plugin settings object + settings-row registrations; add a
`ShowGuide` action mapping to `Config/DefaultInput.ini`; game controller
binds it and broadcasts the seam.

- Plugin persists `bEnableOnboardingAids`, `bShowFirstPickupCards`,
  `bShowDeathCounterHints` in its own `GameUserSettings` section; all default
  ON. Master OFF tears down every aid (no widgets, no handlers).
- Settings rows appear in the existing settings screen through the seam
  registry under an "Onboarding" group.
- The retail help action and window stay exactly as ported; the guide gets
  its own binding (default F1 if unclaimed in `DefaultInput.ini`, else G;
  record the choice in the commit). Integrating it into the managed-rebind
  list is deferred and noted in the plan, not silently done.

### Task 5: Guide browser

**Files:** plugin Slate widget (`SWoTGuideBrowser`), menu-row registrations.

List pane grouped by rarity, detail pane: resolved Title, facts line
(elements/rarity/roles/charges), runtime-resolved Description and Quote,
Counters and Countered-by as rows that deep-link (`SelectRow(FName)`).
Opens from the front-end menu row, the in-game menu row, and the seam's
`FWoTShowGuideRequested`. Nullrhi spec: constructs over the full asset,
`SelectRow` on every row, unresolved text renders the fact rows and omits the
prose panes (never placeholder lorem).

### Task 6: First-pickup card

**Files:** plugin card widget + handler on `FWoTClientItemAdded`; seen-set in
the plugin's settings section.

First add of each class (and only when no card, guide, or retail item-info
window is open): a non-modal ~4s card — resolved title, first sentence of the
resolved description, "hold ⟨item info key⟩ for details, ⟨guide key⟩ for the
guide" with keys read from the live bindings. Specs: fires once per class,
never when seen/toggled off/master off; seen-set survives a settings
save/load round-trip.

### Task 7: Death recap, gates, notes

**Files:** plugin recap handler on `FWoTClientDeathMessage`; gates in plugin
specs; `PORT_NOTES.md`; a proposed CONTRIBUTING delta.

- Recap: only when the line names the local player as victim (derive the
  check from the game mode's actual message construction — read it first and
  copy real shapes into fixtures as *structure*, with invented names);
  resolve the item by `FindByTitleIn`; render one line under the death
  messages ending "press ⟨guide key⟩ for the guide"; unresolvable → show
  nothing.
- **Honesty gate** (spec): every facts row's `CppClass` resolves to a ported
  class whose CDO element/rarity flags and reflector configuration back the
  row's claims; cross-check charge envelope against
  `angreal_expectations.json`. A claim the port does not back fails the
  suite, naming the row. Not-yet-ported classes go on a printed allowlist in
  the spec so shrinkage is visible.
- **Parity-off gate** (spec): with the master setting off, no onboarding
  widget exists and every seam has zero bound handlers from the plugin; and
  the game module's seam spec already proves unbound seams are inert — so a
  build without the plugin is behaviorally identical to one with it disabled.
- `PORT_NOTES.md` entry (Original / Port / Why): the original taught items
  through its single-player ramp and a modal first-pickup info window in its
  tutorial mode; the port adds an optional, default-on onboarding plugin
  (guide, pickup card, death hint), fully disableable for parity.
- CONTRIBUTING delta (separate commit, for maintainer review): extend
  "Parity fixes and enhancements" with the optional-improvement lane —
  improvements ship as plugins behind a master setting, parity lanes run
  with them off.

## Sequencing

Tasks 1-2 are engine-free and land first (one PR). Task 3 starts the C++
wave: build-footprint changes should coordinate with any in-flight packaging
on the same machine. Tasks 4-7 land in review-sized PRs per CONTRIBUTING,
each green on both platforms, `PORT_NOTES.md` arriving with Task 7.

## Status log

- 2026-11-06 — Task 1 landed (`tools(import): angreal guide facts parse
  layer`); gated suite 10/10, ungated skips the 3 reference pins cleanly.
- Funnel notes for the Task 7 CONTRIBUTING delta: (a) the documented
  `python3 -m unittest Tools.import.test_…` form works because unittest
  imports via importlib — an `import Tools.import.x` *statement* is a syntax
  error, so modules under `Tools/import` are imported bare-name via the
  `PYTHONPATH` idiom; (b) the exported `.uc` corpus is latin-1 — CONTRIBUTING
  never states the exports' encoding; (c) `Tools/import/data/` exists only
  after a generator first runs, although the importers document writing
  there; (d) `angreal_expectations.json` consumers must collapse variant
  entries (`Shift@aimed`) via their `uc` stem — the schema string does not
  say so.
- 2026-11-06 — Task 2 landed: 44-row facts CSV (37 angreal + 7 budget-sold
  traps), 43 derived counter edges, 9 cited overrides, 19 hint templates;
  35 tests (7 reference-gated). Decisions recorded:
  - `ForkRTSReflector` descends from `ReturnToSenderReflector`
    (ForkRTSReflector.uc:12), so Fork carries the same edges as Reflect.
  - The Balefire shield strip is verified on both defense paths
    (AngrealInvBalefire.uc:321-322 invokes its damage effect directly,
    bypassing the reflector pipeline; the damage-type table at
    AngrealInventory.uc:861 matches no element for it).
  - `ReflectSeekerReflector` is referenced by no shipped `ReflectorClasses`
    entry, so its rule yields zero edges on retail data — kept so a modded
    or future row derives correctly; not a bug.
  - Citadel defender resources (`SisterInventory`/`SitterInventory`,
    WOTPawns) are budget-sold but not angreal or traps; out of the v1
    roster, a possible later guide section.
  - Open for Task 5: the `{counter}` placeholder on rows countered by many
    things needs a selection rule (most-specific-first: the dedicated
    counter, then Reflect, then element shields).
- 2026-11-06 — Task 0 build proof: a fresh clone of the public repository
  built `WoTEditor` (Mac, Development) through the guarded wrapper on the
  first try — vendor innoextract fetch resolved and matched its pin, build
  `Result: Succeeded`. Funnel notes: (e) the suites and the 3.9/3.12 CI
  matrix caught a real `Path.write_text(newline=)` 3.10-only call — the
  documented Python floor is load-bearing; (f) a first build leaves three
  untracked files under `Build/Mac/` (two `Sandbox.*.entitlements`, one
  `*.PackageVersionCounter`) that a contributor's clean-tree expectations
  trip over — candidates for the public `.gitignore`.
- 2026-11-06 — the funnel notes above (doc gaps, data-dir visibility, the
  expectations variant rule, the build-output ignores) are handed to the
  repository hygiene follow-up and are no longer carried by this plan;
  Task 7's CONTRIBUTING delta now covers only the optional-improvements
  (enhancements) policy lane.
- 2026-11-07 — Task 4 implemented (verification pending: both-platform builds
  with the plugin enabled and disabled, plus the new specs). Decisions:
  - Guide key is **F3**: the plan's F1 and G are retail-bound (ShowHelp,
    PlayTaunt1), and F2 carries ShowInventoryInfo; F3 is the first function
    key unbound in `DefaultInput.ini`. Managed-rebind integration for
    ShowGuide stays deferred, as above.
  - `FWoTOnboardingRow` gains an `Owner` tag so a module can remove exactly
    its rows on shutdown; registration is restart-safe (re-register never
    doubles rows).
  - The plugin's three settings rows (master + two aids, own
    `GameUserSettings` section, all default on) register unconditionally —
    the rows are the only in-game way to turn the aids back on, so the
    master switch gates aid behavior only, never the rows.
  - The seam's settings rows render as an appended ONBOARDING group on the
    Controls page (the gameplay-options page, reachable from the front-end
    and the in-game menu); an empty registry renders today's screen exactly.
  - `ShowGuideRequested` stays unbound until Task 5; the F3 press is a
    seam broadcast with no other behavior.
- 2026-11-08 — Task 5 landed (guide browser). Decisions:
  - The `{counter}` rule is structural: a counterer with exactly one edge
    is dedicated; a counterer whose countered rows share no element is the
    Reflect family; one whose countered rows share an element is an element
    shield. Dedicated beats Reflect beats shields; ties keep `Rows` order.
  - One client window at a time, as the original kept it: a
    `CloseClientWindows(Keeper)` seam lets the guide and the retail windows
    close each other. The guide draws the retail menu chrome through a
    revocation-aware `ResolveMenuChrome` seam (flat fallback without it),
    and takes in-match UI input through `SetOverlayInput`.
  - The Field Guide menu row follows the master switch live; the settings
    rows stay unconditional (Task 4).
- 2026-11-09 — Task 6 implemented (first-pickup card). Decisions:
  - The card names only the guide key. A pickup selects the new item only
    when nothing was selected (`WOTPlayer.uc` `AddInventory`), so the
    item-info key would usually describe a different item. Instead, the
    guide key pressed while a card is up opens the guide at the card's row.
  - A busy screen waits rather than queues: a first add while a card, the
    guide, or a retail window holds the screen shows nothing and leaves the
    class unseen, so its next first add (the next life's inventory, say)
    shows it. Retail windows (item info, help, the in-game menu) reach the
    plugin through a new `IsClientWindowOpen` seam query.
  - Key names come from the local player's live mappings, where the game
    applies its managed rebinds, falling back to the project input settings.
  - Placement: left edge, mid-height. The right-hand message column starts
    at y=80 of 480 (`BaseHUD.uc:1125`), progress lines are centred from a
    quarter height down (`BaseHUD.uc:684`), and the hand fan is
    bottom-centre.
  - Under automation the item-added handler is inert unless a spec supplies
    a guide, so no other suite's inventory traffic writes the persisted
    seen list.
  - Open for Task 7: the shipped death line is "victim, conjunction,
    killer" (`giWOT.uc:147-158`) and never names the item, so the recap
    cannot resolve the item from the line by title. Task 7 needs another
    source for the killing item.
