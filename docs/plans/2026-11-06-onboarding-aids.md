# Onboarding aids: implementation plan

**Status:** in progress (branch `feature/onboarding-aids`) · **Date:** 2026-11-06

The multiplayer-only port drops the original game's single-player ramp, so a
new player meets ~38 ter'angreal and their counter relationships with no
teacher. This plan adds three always-available teaching surfaces (a
browsable **field guide**, a **first-pickup info card**, and a
**death-recap counter hint** such as "Fireball - Fire Shield absorbs fire
weaves.") as an **optional improvement** that a parity-pure build can switch
off or omit entirely.

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
   - `FWoTClientItemAdded(FName ClassName)`, from
     `UWoTInventoryComponent::OnRep_Inventory` (and the server-side add path,
     so listen hosts fire it too) for item types newly present;
   - `FWoTClientKilledBy(FName ClassName)`, from the owner-only
     `ClientKilledBy` message in which the server names the item behind the
     local player's death (Task 7; it replaced a death-line seam);
   - `FWoTShowGuideRequested()`, from a `ShowGuide` input action;
   - a registration list for extra menu rows (label callback + invoke
     callback), consumed by the in-game menu and front-end menu;
   - a registration list for extra settings rows, consumed by the settings
     screen;
   - `FWoTResolveRetailText`, a resolver the game module installs at startup
     so the plugin can request display text by (file, section, property)
     through the same runtime mechanism the menus use (`WoTRetailText`).
     The plugin links none of the game module's classes; the seams are the
     entire boundary. This is deliberately the seed of a general extension
     surface: a future improvement module reuses the same seams.
3. **Data is facts-only in the tree; retail prose only at runtime.** Per the
   contribution rules, retail text is never compiled in or committed. The
   guide's checked-in data carries *derived facts with citations*, exactly
   the precedent `Tools/test/angreal/angreal_expectations.json` set, and the
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
| `Tools/import/data/angreal_guide_facts.csv` | generated, **checked in** | one row per item: `Class, CppClass, Elements, Rarity, Roles, MaxInitialCharges, MaxCharges, ChargeCost, Counters, HintKey`; facts only, no prose |
| `Tools/import/data/angreal_counter_overrides.csv` | authored | `Class, Field, Value, Supersedes, Citation`; every edge removed or added by hand carries an `exports/script/...` citation |
| `Tools/import/data/angreal_guide_hints.csv` | authored | `HintKey, Template`: our own counter-hint templates (`{title}`/`{counter:<Class>}` resolved at runtime; no retail text) |
| `Plugins/WoTOnboarding/Source/WoTOnboarding/Private/Generated/WoTAngrealGuideRows.inl` | generated, **checked in** | the facts plus resolved hint templates, one JSON row per line, compiled into the plugin (since 2026-11-09; see the status log) |

`Tools/import/import_angreal_guide.py` derives the facts CSV from the exported
script (`$WOT_REFERENCE_ROOT`). As first planned it also built
`/Game/WoT/UI/Guide/DA_WoTAngrealGuide` under the editor's python commandlet;
since 2026-11-09 it renders the generated include above instead, because the
guarded build cooks no project data assets.

**Counter derivation rules** (edges come only from these; everything else is
hint text):

- An element shield (its `ReflectorClasses` names the matching
  ignore-element reflector) counters every offensive row carrying that
  element flag (`bElementFire` → Fire, and likewise Air/Earth/Water/Spirit).
- `ReturnToSenderReflector` counters every offensive row descending from
  `ProjectileLauncher`.
- `ReflectSeekerReflector` counters the Seeker.
- Trap detection counters the rows whose placed pieces (a trap's resource and
  a wall's slabs, or a launcher's projectile) carry an identity in the
  detector's own table. The table leaves out alarms and fire walls and
  includes the armed explosive ward (Task 7 correction, see the status log).
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
an owned copy, `Tools/local.json` for engine roots). Record every pothole
(a doc gap, a script that assumes a private path, a test that cannot skip)
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
already encodes reachability; no separate exclusions file). Hints: one per
offensive row, our own wording, derived default ("<Title placeholder> -
countered by …" is rendered at runtime from resolved names; the hint CSV
stores sentence *templates* keyed per class so wording stays tunable).

Ungated tests: derivation on synthetic fixtures (shield edge, return-to-sender
edge, override strip with citation enforcement, rarity-conflict error);
completeness against `angreal_expectations.json`: every entry in its `order`
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
  for recap resolution; removed with the Task 7 recap, which gets the item
  from the server), `CounteredBy(FName)`.
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
window is open): a non-modal ~4s card with the resolved title, first sentence of the
resolved description, "hold ⟨item info key⟩ for details, ⟨guide key⟩ for the
guide" with keys read from the live bindings. Specs: fires once per class,
never when seen/toggled off/master off; seen-set survives a settings
save/load round-trip.

### Task 7: Death recap, gates, notes

**Files:** `Source/WoT/WoTKillingItem.h/.cpp`, the game mode's send and the
player controller's `ClientKilledBy`; plugin recap handler on
`FWoTClientKilledBy`; gates in plugin specs; `PORT_NOTES.md`; a proposed
CONTRIBUTING delta.

- Recap: the shipped death line never names the item (victim, conjunction,
  killer: `giWOT.uc:147-158`), so the server names it. As the killing blow
  lands, `WoTKillingItem::Resolve` reads the item behind it from the damage
  causer: an item, a projectile's source angreal, the item that places a
  trap, the Guardian for the Mashadar, and Balefire by its damage type. A
  killer-less death soon after a Whirlwind or Swap Places names that
  angreal. The game mode sends the name, or none, to the victim alone on
  every death (`ClientKilledBy`, reliable), whatever the server's own
  settings; a client without the aids drops it. The plugin shows a card in the pickup card's
  slot: "Killed by ⟨title⟩", the row's hint line, and "Press ⟨guide key⟩ for
  the field guide". No row, or a row with no hint line, shows nothing.
- **Honesty gate** (spec): every facts row's `CppClass` resolves to a ported
  class whose CDO element/rarity flags and reflector configuration back the
  row's claims, charge envelope and charge cost included; cross-check the
  charge envelope against `angreal_expectations.json`. A claim the port does not back fails the
  suite, naming the row. Not-yet-ported classes go on a printed allowlist in
  the spec so shrinkage is visible.
- **Parity-off gate** (spec): with the master setting off, no onboarding
  widget exists and every seam has zero bound handlers from the plugin; and
  the game module's seam spec already proves unbound seams are inert, so a
  build without the plugin is behaviorally identical to one with it disabled.
- `PORT_NOTES.md` entry (Original / Port / Why): the original taught items
  through its single-player ramp, where mission and tutorial maps flag
  chosen pickups for a first-pickup notice (`AngrealInventory.uc:601-619`;
  no multiplayer map flags any, and the game type's info-hint path at
  `giWOT.uc:208-224` fires for no shipped item); the port adds an optional,
  default-on onboarding plugin (guide, pickup card, death hint), fully
  disableable for parity.
- CONTRIBUTING delta (separate commit, for maintainer review): extend
  "Parity fixes and enhancements" with the optional-improvement lane:
  improvements ship as plugins behind a master setting, parity lanes run
  with them off.

### Task 8: The original window look

**Files:** `Source/WoT/WoTOnboardingSeams.h`, `Source/WoT/WoT.cpp`,
`Source/WoT/WoTPlayerController.cpp` (seams); plugin chrome and art
(`WoTOnboardingChrome`, `SWoTRetailArt`), the three widgets, the card text.

The aids draw like the original's item-info window, from the player's own
files, at the whole-number scale the game's windows use:

- **Art.** The frame is `WOTWindow.DrawWindow` and the divider
  `WOTWindow.DrawSeparator` (`WOTWindow.uc:107-152`, regions at `:184-198`):
  `InfoBoxC1` corners, a solid black fill, `InfoBoxC2` edge tiles. The gem
  bar, the five gems and the layout order are `InventoryInfoWindow`'s
  (`InventoryInfoWindow.uc:59-169`, regions at `:205-220`); `IconMask` covers
  the detail icon as it covers the window's. Item icons are each class's
  `StatusIcon`; the guide marks its selection with the HUD's gold
  selected-slot frame. The frame pieces, the detail icon and the card icons
  draw at whole-number scales with the HUD's own filtering; the grid's 50 px
  icons, the 40 px counter icons and the selection frame are not whole
  multiples of their 64 px art, so they sample bilinearly.
- **Type.** `F_WOTReg30` titles, `F_WOTReg14` body, `F_WOTIta14` quotes,
  `F_Element` gem labels, each drawn as an exact bitmap strike at a whole
  step from 1x to 8x, so every glyph lands on whole screen pixels. The card
  and the recap use the 1x or 2x strike for their scale. The guide draws its
  title, labels and gem labels at the step nearest its scale, so they keep
  about their original size, and the detail pane's reading text (rarity,
  charges, description, quote, hint and the counter-row labels) at about
  1.5x: the largest step at which every entry fits the pane without
  scrolling, and at least one step above the labels. On a screen too small
  for a 1x guide the text keeps its 1x size (1.5x for reading text) without
  whole pixels. Labels draw at the menus' unselected grey.
- **Scale.** The card and the recap draw at the largest whole number (1 or
  2) at which a 960x540 window fits the screen; below 960x540 they shrink to
  fit. The guide fills 95% of the largest 960x540 window that fits, at any
  fraction, and keeps its glyphs whole through the type steps above. At
  1280x800 (the Deck) the cards draw at 1 and the guide at about 1.27, with
  its labels at 1x and its reading text at 2x; the smallest face is 14 px
  tall, above the 12 px floor.
- **Guide** (960x540, centred): "FIELD GUIDE" and "Esc Close" across the
  top; on the left an icon grid in the four groups, 8 columns of 50 px icons
  at a 56 px pitch; on the right the item-info window's content for the
  selection (title, masked icon, rarity and charges, gem bar, description,
  divider, quote), then the rows that counter it, the hint line and the rows
  it counters, each other row as an icon that opens it. Roles are not shown:
  the item-info window has no such line. Charges read as the starting
  envelope over the cap ("20 / 35"); an angreal whose ChargeCost is 0 shows
  F_Charge's ':' glyph, the window's infinity mark, instead. Both panes scroll when their content
  overflows (mouse wheel; page keys and the right stick for the detail).
  Arrow keys, the d-pad and the left stick move the selection through the
  grid; a click selects. Escape, the controller's B and View buttons and the
  guide key close it.
- **Opening selection.** While the local player is dead (until they respawn),
  the item that killed them when the guide carries it; else the card's row
  while a card is up; else the item in the local player's selected hand when
  the guide carries it; else the first entry of the first group.
- **Card** (430 wide, left edge, mid-height): a dark band between two
  dividers, the item's icon, its title, the first sentence of its
  description, and the guide key as "F3  Field Guide".
- **Recap** (450 wide, same slot): "KILLED BY" over the killer's icon and
  title, "COUNTER" over the counter's icon and the hint line, then the
  guide key.
- **Fallback.** Without the art or the strikes (an editor without a payload,
  or a failed resolve) the same layout draws with flat panels, engine faces
  at or above the Deck floor, and empty icon slots.
- **Seams** (plain types, every one inert unbound): `ResolveRetailTexture`
  (the four named UI textures) and `ResolveItemIcon` (a class's
  `StatusIcon`), installed at startup; `SelectedItemClass`, installed by the
  local gameplay controller; and `FWoTOnboardingMenuChrome::PixelFont`,
  resolved with the chrome through a presentation authority for the local
  player, whose revocation also collapses the widget. The chrome now carries
  only the faces and that binder: the menu brushes and colours it carried
  before are gone, since the art arrives through `ResolveRetailTexture`. The
  two texture seams take a `bSmooth` flag for the bilinear case above.

## Sequencing

Tasks 1-2 are engine-free and land first (one PR). Task 3 starts the C++
wave: build-footprint changes should coordinate with any in-flight packaging
on the same machine. Tasks 4-7 land in review-sized PRs per CONTRIBUTING,
each green on both platforms, `PORT_NOTES.md` arriving with Task 7.

## Status log

- 2026-11-06: Task 1 landed (`tools(import): angreal guide facts parse
  layer`); gated suite 10/10, ungated skips the 3 reference pins cleanly.
- Funnel notes for the Task 7 CONTRIBUTING delta: (a) the documented
  `python3 -m unittest Tools.import.test_…` form works because unittest
  imports via importlib; an `import Tools.import.x` *statement* is a syntax
  error, so modules under `Tools/import` are imported bare-name via the
  `PYTHONPATH` idiom; (b) the exported `.uc` corpus is latin-1, and CONTRIBUTING
  never states the exports' encoding; (c) `Tools/import/data/` exists only
  after a generator first runs, although the importers document writing
  there; (d) `angreal_expectations.json` consumers must collapse variant
  entries (`Shift@aimed`) via their `uc` stem; the schema string does not
  say so.
- 2026-11-06: Task 2 landed: 44-row facts CSV (37 angreal + 7 budget-sold
  traps), 43 derived counter edges, 9 cited overrides, 19 hint templates;
  35 tests (7 reference-gated). Decisions recorded:
  - `ForkRTSReflector` descends from `ReturnToSenderReflector`
    (ForkRTSReflector.uc:12), so Fork carries the same edges as Reflect.
  - The Balefire shield strip is verified on both defense paths
    (AngrealInvBalefire.uc:321-322 invokes its damage effect directly,
    bypassing the reflector pipeline; the damage-type table at
    AngrealInventory.uc:861 matches no element for it).
  - `ReflectSeekerReflector` is referenced by no shipped `ReflectorClasses`
    entry, so its rule yields zero edges on retail data; kept so a modded
    or future row derives correctly; not a bug.
  - Citadel defender resources (`SisterInventory`/`SitterInventory`,
    WOTPawns) are budget-sold but not angreal or traps; out of the v1
    roster, a possible later guide section.
  - Open for Task 5: the `{counter}` placeholder on rows countered by many
    things needs a selection rule (most-specific-first: the dedicated
    counter, then Reflect, then element shields).
- 2026-11-06: Task 0 build proof: a fresh clone of the public repository
  built `WoTEditor` (Mac, Development) through the guarded wrapper on the
  first try: vendor innoextract fetch resolved and matched its pin, build
  `Result: Succeeded`. Funnel notes: (e) the suites and the 3.9/3.12 CI
  matrix caught a real `Path.write_text(newline=)` 3.10-only call, so the
  documented Python floor is load-bearing; (f) a first build leaves three
  untracked files under `Build/Mac/` (two `Sandbox.*.entitlements`, one
  `*.PackageVersionCounter`) that a contributor's clean-tree expectations
  trip over; candidates for the public `.gitignore`.
- 2026-11-06: the funnel notes above (doc gaps, data-dir visibility, the
  expectations variant rule, the build-output ignores) are handed to the
  repository hygiene follow-up and are no longer carried by this plan;
  Task 7's CONTRIBUTING delta now covers only the optional-improvements
  (enhancements) policy lane.
- 2026-11-07: Task 4 implemented (verification pending: both-platform builds
  with the plugin enabled and disabled, plus the new specs). Decisions:
  - Guide key is **F3**: the plan's F1 and G are retail-bound (ShowHelp,
    PlayTaunt1), and F2 carries ShowInventoryInfo; F3 is the first function
    key unbound in `DefaultInput.ini`. Managed-rebind integration for
    ShowGuide stays deferred, as above.
  - `FWoTOnboardingRow` gains an `Owner` tag so a module can remove exactly
    its rows on shutdown; registration is restart-safe (re-register never
    doubles rows).
  - The plugin's three settings rows (master + two aids, own
    `GameUserSettings` section, all default on) register unconditionally:
    the rows are the only in-game way to turn the aids back on, so the
    master switch gates aid behavior only, never the rows.
  - The seam's settings rows render as an appended ONBOARDING group on the
    Controls page (the gameplay-options page, reachable from the front-end
    and the in-game menu); an empty registry renders today's screen exactly.
  - `ShowGuideRequested` stays unbound until Task 5; the F3 press is a
    seam broadcast with no other behavior.
- 2026-11-08: Task 5 landed (guide browser). Decisions:
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
- 2026-11-09: Task 6 implemented (first-pickup card). Decisions:
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
- 2026-11-09: Task 7 gates and notes landed; the death recap stays open
  (above) pending a decision on where the killing item comes from.
  Decisions:
  - Every seam binding follows the master switch live, as the menu row
    already did: switched off, the module holds no binding on any seam, so a
    switched-off build and one without the plugin behave the same. Only the
    settings rows, the way back on, stay registered.
  - The honesty gate's first run caught a wrong counter rule. Trap
    detection keeps a piece only when its identity is in the detector's own
    table (`TrapDetectLeech.uc:203-233`, table at `:286-291`): wall slabs,
    pits, portcullises, spears, staircases and the armed explosive ward. The
    identity is the class name, because no shipped trap sets `TrapType`, so
    alarms and fire walls (`FireWallSlab` is not `WallSlab`) go undetected.
    The generator now derives the edge from that table: Trap Detect no
    longer claims the alarm and the fire wall and now lists the explosive
    ward. The two trap hints that promised a counter were reworded, and the
    port already matched the original here.
  - The PORT_NOTES entry describes what ships now (guide and card); the
    recap extends it when it lands.
- 2026-11-09: the death recap landed (Task 7). Decisions:
  - The server names the item. `WoTKillingItem::Resolve` reads it from the
    killing blow's damage causer as the pawn dies, and `PlayerKilled` sends
    it to the victim alone through `AWoTPlayerController::ClientKilledBy`,
    after scoring and never at a match's end. The message lives in the game
    module, so every build carries it: the server sends it whatever its own
    settings, and a client with the aids off, or without the plugin, has
    nothing bound to the seam and drops it. `FWoTClientKilledBy` replaced
    the death-line seam, and `FindByTitleIn` went with it.
  - Deaths no item dealt name nothing: falls, drowning, zones, map movers,
    pits, staircases and summoned creatures. A killer-less death inside the
    scoring window of a Whirlwind or Swap Places names the displacing
    angreal, on the same terms the scoring credits its caster.
  - The recap takes the pickup card's slot and widget for 6 s (a card holds
    4 s) and replaces a card that is up; the guide or a retail window
    holding the screen shows nothing. The guide key opens the guide at the
    killing item, as it does for a card. A row with no hint line (Guardian,
    Swap Places) shows no recap.
  - The PORT_NOTES entry now covers the recap.
- 2026-11-09: review fixes. Decisions:
  - Each hint template names the counter its sentence was written for, by
    class (`{counter:AngrealInvFireShield}`), and the line shows only while
    that row counters the hinted row: the generator fails a template whose
    named counter does not, and the runtime drops the line if the data stops
    backing it. A bare `{counter}` is refused. This replaces the 2026-11-08
    structural rule, which ranked Fork in the Reflect family and so put Fork
    into six lines written for a shield or for Reflect ("Fork absorbs fire
    weaves"); Fork's own text says the attacks that get through its shield
    still hit you.
  - Hint lines no longer repeat the item's name: the card, the recap and the
    guide all show the name right above the line. `{title}` still resolves.
  - The guide rows ship compiled into the plugin. The guarded build cooks
    only the neutral maps and the pinned support assets, so
    `/Game/WoT/UI/Guide/DA_WoTAngrealGuide` never reached a release and every
    aid would have stayed silent there. `import_angreal_guide.py` now renders
    the facts and hints CSVs into
    `Plugins/WoTOnboarding/Source/WoTOnboarding/Private/Generated/WoTAngrealGuideRows.inl`
    (the controller layout's pattern), `UWoTAngrealGuide::Load()` parses it
    into a transient guide, and the data asset, its editor importer half and
    the result file are gone. `--rows-only` re-renders from the checked-in
    CSVs without the reference export, and `--check` (run by the python
    tests) fails a stale include. Dev and shipped builds read the same rows.
  - The guarded release lane packages the plugin. Three parts of it knew
    nothing of project code plugins, so any guarded package since the plugin
    was enabled would have failed:
    - The ISO seed never copied `Plugins/`, and UBT refuses a project whose
      enabled plugin it cannot find. The seed now copies each
      `SEED_CODE_PLUGINS` entry without its `Intermediate`, and the source
      snapshot binds the plugin's source.
    - The Win64 build root mirrored all of `Plugins/` but transferred only
      products under `Binaries/Win64`. A plugin's `Binaries` and
      `Intermediate` are now UBT-owned like the project's, and its editor
      modules transfer into the ISO.
    - UBT stages the `.uplugin` of every plugin that links a module, and
      both `build_stripped_pak.py` and `run_first_run_funnel.py` refused any
      project descriptor but the neutral shells'. The exact path is allowed
      at most once and never required, so a build with the plugin off still
      passes.
- 2026-11-11: the guide opens at the killing item while the player is dead.
  Decisions:
  - From death until respawn (the controller's Dying state), every way of
    opening the guide starts at the item that killed the player, when the
    server named one and the guide carries it. It does not depend on the
    recap setting, and it wins over a card still on screen. After the
    respawn the guide opens as before.
  - The server now sends `ClientKilledBy` on every death, with none when no
    item dealt it. The client forgets the item when it leaves Dying, but it
    can miss the brief Playing state between two quick deaths; the second
    death's message then replaces the first death's item.
