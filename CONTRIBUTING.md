# Contributing

Thank you for helping bring a 1999 multiplayer game back to life. This page is
the contract between contributors: what the port is, how it is built, and the
lines it does not cross.

## Before you start

- **You need your own copy of the game.** Everything the tools read comes from
  a private reference directory you create from your installed copy. Set
  `WOT_REFERENCE_ROOT` to it; [docs/LOCAL-CONFIGURATION.md](docs/LOCAL-CONFIGURATION.md)
  explains the layout and the machine-local settings file (`Tools/local.json`).
- `Tools/extract` regenerates `exports/` (the shipped UnrealScript, map text
  and inventory) from your installed copy. Those exports are the specification of
  record for everything ported here. They stay in your private directory.

## Ground rules

1. **The shipped script is the spec.** When in doubt, match the exported `.uc`
   and say so in the commit.
2. **C++ carries all behavior.** Generated leaf Blueprints only bind assets.
   Tunables live in data tables or data assets backed by CSV or JSON in the
   repository.
3. **Divergences are written down.** Any intentional difference from the
   original's behavior goes into `PORT_NOTES.md` in the same pull request,
   with the reason. Gameplay ideas go to an issue, never into the port
   (see [Parity fixes and enhancements](#parity-fixes-and-enhancements)).
4. **Simulation is parity-locked; presentation is free.** Movement, timing,
   damage and cadence match the original. Rendering, audio presentation and UI
   polish may modernize within the frame budget.
5. **Nothing from the original is ever pasted.** Not UnrealScript, not engine
   code, not comments, not user-facing text, not data tables. Not in source,
   not in tests, not in issues, pull requests or commit messages. Cite instead:
   `WOTPlayer.uc:576-606`. Every contributor can regenerate the exports and
   look it up. Retail text the game shows is loaded at runtime from the
   player's own localization files; it is not compiled in.
6. **No generated content in the tree.** Every asset under `Content/` is
   produced by the pipeline from the reference data and is ignored by git.
   There are no editor-GUI steps.
7. **No machine details in the tree.** Engine locations and scratch
   directories belong in `Tools/local.json`, which is ignored.

## Parity fixes and enhancements

Every change is one of two kinds, and issues are labelled to match:

- **`parity`**: the port behaves differently from the original game, crashes,
  or fails to install. Fixes that bring the port closer to the original are
  always welcome. Use the bug report form.
- **`enhancement`**: a deliberate change from how the original plays. These
  are tracked and discussed as issues (use the idea form) but are not merged
  into the port for now. Presentation, tooling and documentation improvements
  that leave play unchanged are not enhancements in this sense; send them as
  normal pull requests.

### Optional improvements

A player-facing feature the original never had, even one that leaves play
unchanged (the onboarding aids that teach items and counters, say), ships as
an optional plugin rather than as a change to the port:

- The plugin lives under `Plugins/` and reaches the game only through a small
  seam header in the game module (`Source/WoT/WoTOnboardingSeams.h`). A seam
  does nothing when nothing listens, so the game builds and plays the same
  with the plugin disabled or absent.
- A master setting turns the plugin off while the game runs. Switched off, it
  holds no seam binding and draws nothing; a spec proves it.
- Parity checks, captures and comparisons run with the plugin off.
- Its `PORT_NOTES.md` entry says what it adds and how to turn it off.

`Plugins/WoTOnboarding` is the worked example.

## Porting conventions

| UnrealScript | UE5 C++ |
|---|---|
| `state X {}` / `GotoState` | shared enum FSM + Enter/Exit/Tick dispatch |
| latent `Sleep` / `FinishAnim` | named-step continuations (`FTimerManager` / `OnAnimEnd`) |
| `replication {}` | `UPROPERTY(Replicated…)` + `GetLifetimeReplicatedProps`, matching `COND_*` |
| `simulated` | explicit role guards |
| `defaultproperties` | CDO constructor defaults; asset refs via soft paths or leaf BPs |
| `PlayerPawn` | split: `APlayerController` + `ACharacter` |
| `GameInfo` | split: `AGameMode` + `AGameState` + `APlayerState` |
| `Inventory` chain | `UWoTInventoryComponent` (array, original selection order) |
| `PlayAnim` / `LoopAnim` / `TweenAnim` | `UVertexAnimComponent`, same names and semantics |
| `Accessed None` tolerance | `IsValid()` guards wherever the original could hit `None` |

### The port loop, per class

1. Read the exported `.uc` for the class.
2. Write the C++ under `Source/WoT/…`, in your own words. Match behavior,
   not text.
3. Build on your platform. Add or extend an automation spec and run it
   headless.
4. Open a pull request sized to review in one sitting.

Review checklist: replication conditions match; role guards match `simulated`;
constructor defaults match `defaultproperties`; no editor-GUI steps; nothing
pasted; `PORT_NOTES.md` updated for any divergence.

## Building

Both platforms must build. The compilers disagree: MSVC turns shadowed
variables and some other warnings into errors that Apple Clang accepts, and
Clang catches things MSVC does not. After pulling changes under `Source/`,
rebuild on your platform before working.

The game module needs the patched source engine (see the README); the stock
launcher engine cannot build it. Fetch the innoextract library once per
platform first (`python Tools/build/fetch_innoextract_libs.py fetch --platform
Win64` or `--platform Mac`).

```sh
# macOS
"$(python3 Tools/wot_config.py get source_engine_root_mac)/Engine/Build/BatchFiles/Mac/Build.sh" \
  WoTEditor Mac Development -project="$PWD/WoT.uproject"

# Windows (PowerShell)
& "$(python Tools/wot_config.py get source_engine_root_win64)\Engine\Build\BatchFiles\Build.bat" `
  WoTEditor Win64 Development -project="$PWD\WoT.uproject" -NoHotReload
```

On Mac, `installed_engine_root_mac` is a separate developer-baseline install;
it must differ from the source root and may be unset. Build the four standalone
test programs through the guarded wrapper, then run their complete suites:

```sh
ENGINE="$(python3 Tools/wot_config.py get source_engine_root_mac)"
for target in WoTBootstrapCoreTests WoTUpdaterCoreTests WoTUpdateTransportsTests WoTRetailReaderTests; do
  Tools/build/build_guarded_ue58.sh --engine "$ENGINE" \
    --project "$PWD/WoT.uproject" --target "$target" \
    --platform Mac --configuration Development
done

Binaries/Mac/WoTBootstrapCoreTests --suite WoT.BootstrapCore
Binaries/Mac/WoTUpdaterCoreTests --suite WoT.UpdaterCore
Binaries/Mac/WoTUpdateTransportsTests --suite WoT.Update.PlatformHop
Binaries/Mac/WoTRetailReaderTests --suite WoT.RetailReader
```

The editor suite requires an imported `Content/` tree. Build the editor target
with the same wrapper, then run:

```sh
Tools/build/build_guarded_ue58.sh --engine "$ENGINE" \
  --project "$PWD/WoT.uproject" --target WoTEditor \
  --platform Mac --configuration Development
"$ENGINE/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/WoT.uproject" \
  -ExecCmds="Automation RunTests WoT; Quit" \
  -unattended -nullrhi -nosplash -nopause -nocrashreports -stdout
```

The automation suite runs headless through the editor binary with
`-nullrhi -unattended`; judge a run by its exit code and `TEST COMPLETE`
line, never by a tail of the log. The pure-Python tools have unit tests:

```sh
# One module: name it explicitly (the Tools directories are namespace
# packages, so discovery cannot start inside them) and put its directory on
# PYTHONPATH, because some tests import their siblings by bare name.
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=Tools/public \
  python3 -m unittest Tools.public.test_leak_guard

# Every module, the way the public-guard workflow runs them.
for module in $(git ls-files 'Tools/test_*.py' 'Tools/**/test_*.py' | sed -e 's|\.py$||' -e 's|/|.|g'); do
  PYTHONDONTWRITEBYTECODE=1 \
    PYTHONPATH=$(echo "$module" | sed -e 's|\.test_[^.]*$||' -e 's|\.|/|g') \
    python3 -m unittest "$module"
done
```

Always run them by module path as above: `Tools.import` cannot be written in a
Python `import` statement (`import` is a keyword), and `unittest` loads it
through `importlib` instead.

Tests that need the private reference data skip themselves when
`WOT_REFERENCE_ROOT` has no such data, and say so. A test that runs either way
uses synthetic fixtures, so its passing does not show that the retail parsing
path was exercised. The exported script and map text (`exports/**/*.uc`,
`*.t3d`) is Windows-1252, not UTF-8: read it with `encoding="latin-1"` (the
`.int` string tables use `cp1252`).

Native (engine-side) behavior that the shipped script only declares is ported
from observed behavior and the script's contract. If your change touches a
native function, describe the intended behavior and cite the script; the
maintainer verifies it against the original game's behavior.

## Pull requests

- **Sign off every commit** (`git commit -s`). This is the
  [Developer Certificate of Origin](https://developercertificate.org/): you
  certify that you wrote the change or have the right to submit it under the
  project license. There is no CLA.
- One topic per pull request, with a description that says what the original
  does, what the change does, and how you verified it.
- A new feature is one topic: one branch and one pull request. Open it early
  (a draft is fine), push follow-up work to the same branch as new commits,
  and keep the description current. Do not open a pull request on top of
  another that has not merged. If a feature is too large to review at once,
  get its first part merged, then open the next part against `main`.
- Built on both platforms when `Source/` changes (say which you could not
  build; the maintainer covers the other).
- Tests green. New behavior comes with a spec.
- `PORT_NOTES.md` updated for every divergence; ideas filed as `enhancement`
  issues.
- No retail bytes, no exports, no generated content, no machine paths. The
  public guard workflow fails the pull request if it finds any.
- Never quote the original's script or data in the description or commits.
  Cite file and line.

## Conduct and security

Everyone here follows the [Code of Conduct](CODE_OF_CONDUCT.md). Security
problems and rights-holder requests go through [SECURITY.md](SECURITY.md).
