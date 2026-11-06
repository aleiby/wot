# WoT Reborn

A multiplayer engine for *The Wheel of Time* (1999). Requires the original game.

[![WoT Reborn gameplay](https://img.youtube.com/vi/Y1bwPW3puhI/maxresdefault.jpg)](https://www.youtube.com/watch?v=Y1bwPW3puhI)

*[See it in action on YouTube](https://www.youtube.com/watch?v=Y1bwPW3puhI) (1:53).*

This unofficial, fan-made project ports the multiplayer side of Legend
Entertainment's 1999 game to Unreal Engine 5.8, for Windows and macOS.

**You need your own copy of the original game.** This repository and its
releases contain no game data at all: no maps, textures, meshes, sounds, music,
scripts or text. On first launch the game rebuilds everything it needs from the
installer you supply, on your machine, and keeps it there.

This project is not affiliated with or endorsed by the rights holders of the
game or of the books. See [NOTICE](NOTICE) and the legal section below.

**Contents:** [What it is](#what-it-is-and-is-not) · [Installing](#installing) · [Uninstalling](#uninstalling) · [How the game gets its data](#how-the-game-gets-its-data) · [Building from source](#building-from-source) · [Repository map](#repository-map) · [Contributing](#contributing) · [License and legal](#license-and-legal)

## What it is, and is not

- **It is** a faithful port of the game's multiplayer: Arena deathmatch and
  Citadel battle, the 19 shipped multiplayer maps, every artifact, the
  citadel editor, traps, seals, chat, scoreboard, the original menus and HUD.
  Movement, timing, damage and cadence are locked to the original.
- **It is** a port written in C++ from the behavior of the shipped game. The
  original UnrealScript that shipped with the game is the specification.
- **It is not** the single-player campaign. The story levels, tutorials and
  their NPC scripting are not included.
- **It is not** a source of the original game. Nothing here replaces owning it.

<a id="playing"></a>

## Installing

You need to own the original game. [Buy The Wheel of Time on
GOG](https://www.gog.com/en/game/the_wheel_of_time), then download its
**Windows offline installer**, even if you will play on a Mac. GOG pushes you
toward its GOG Galaxy app, but WoT Reborn needs the offline installer file. A
game installed through Galaxy isn't enough on its own.

To get the offline installer:

- **On gog.com:** sign in, open your games library, and click The Wheel of
  Time. Under **Download offline backup game installers**, download the game's
  installer (about 1 GB).
- **In GOG Galaxy:** select The Wheel of Time, click **More** next to Play, and
  choose **Backups & goodies**. Download the installer under **Game Backup
  Copy**.

If you already own the game, the preserved installer is also available from
[the Internet
Archive](https://archive.org/download/twot_gog_classic/The%20Wheel%20Of%20Time%20%5BGOG%5D/setup_the_wheel_of_time_333b_%2854846%29.exe).

The required file is `setup_the_wheel_of_time_333b_(54846).exe`. Leave it in
your Downloads folder and WoT Reborn finds it on first launch, or point to it
with Browse. The game reads its data but never runs it and never downloads it
for you.

- Windows: download `WoT-Win64-guarded-shipping-<commit>.zip` from the
  [latest release](https://github.com/aleiby/wot/releases/latest), extract its single
  `WoT Reborn` folder anywhere you can write to, keep the folder together, and
  run `WoT Reborn.exe`. The replaceable game is under `Game`; saves,
  preferences, converted content, and logs are under `Data` in the same folder.
- Mac: download `WoT-Mac-guarded-shipping-<commit>.dmg` from the
  [latest release](https://github.com/aleiby/wot/releases/latest), open it, drag `WoT.app`
  to Applications, then open it from Applications.

On first launch the game finds the installer in your Downloads folder by
itself; click Browse if it is somewhere else.

WoT Reborn isn't code-signed on Windows, so the first time you run
`WoT Reborn.exe` Windows may show **Windows protected your PC**. Click
**More info**, then **Run anyway**. Do this only for a copy you downloaded
from this repository's Releases page.

On the first Mac launch, allow **Local Network** access for LAN play. For
Internet play, choose **Always Allow** when macOS asks to let WoT use its
anonymous online ID in your keychain; macOS asks again after each update.

Multiplayer works over LAN and over the internet. Internet play uses Epic
Online Services for session discovery and relay with an anonymous device
identity; no account is needed.

## Uninstalling

- Windows: delete the `WoT Reborn` folder you extracted. Saves, preferences,
  converted game data, logs and any downloaded update are all inside it, so
  that removes everything.
- Mac: choose **Uninstall WoT Reborn…** from the application menu. It deletes
  the game's data and update files, then moves the app to the Trash; your
  installer is not touched. To uninstall by hand, drag `WoT.app` from
  Applications to the Trash. In Finder, choose **Go ▸ Go to Folder…**, enter
  `~/Library/Application Support/WoT Reborn`, and delete that folder. You may
  also delete `~/Library/Caches/com.aleiby.wot`.

## How the game gets its data

The releases here contain our code, materials, menus and empty map shells,
but every retail payload (level geometry, texture pixels, audio samples,
mesh vertices, script-derived data) is absent. At first launch the game:

- locates the retail installer you supplied and checks its size and SHA-256;
- reads the installer with an embedded Inno Setup reader (nothing is executed);
- decodes the Unreal 1 packages directly and writes a local, content-addressed
  cache that is verified against a signed descriptor compiled into the game;
- binds that cache to the running game at load time.

Release announcements, manifests and key rings on GitHub Releases are signed
(Ed25519) and verified. Updates download in the
background; Windows installs them when you next start `WoT Reborn.exe`, while
Mac installs them at quit and you open the app again yourself. If an update
fails before reaching the menu, the next start restores the previous version
(on Mac, open it once more after the rollback notice); saves and preferences stay intact.

## Building from source

Requirements:

- Unreal Engine 5.8 built from source with this project's engine patch, published
  on the EpicGames-organization licensee-only fork:
  [aleiby/UnrealEngine, branch `wot-reborn-5.8`](https://github.com/aleiby/UnrealEngine/tree/wot-reborn-5.8)
  (Epic's `5.8.0-release` plus `EnginePatches/UE-5.8`). The game module calls
  the patch's additions to the engine (the per-user data root and the guarded
  world hooks), so the stock engine from the Epic launcher cannot build it. The
  link works once your GitHub account is linked to an Epic Games account
  ([how](https://www.unrealengine.com/ue-on-github)).
- The prebuilt innoextract library the retail reader links:
  `python Tools/build/fetch_innoextract_libs.py fetch --platform Win64` (or
  `--platform Mac`) downloads the reviewed build from this repository's
  releases and checks it against `ThirdParty/innoextract/1.9/vendor-manifest.json`;
  `Tools/build/build_innoextract_native.py` builds it from source instead.
- Windows: Visual Studio 2022. macOS: Xcode with Apple Clang.
- Python 3.9 or later with Pillow and NumPy for the content pipeline.
- A private reference directory holding your own copy's data. Point
  `WOT_REFERENCE_ROOT` at it; see [docs/LOCAL-CONFIGURATION.md](docs/LOCAL-CONFIGURATION.md).
  `Tools/extract` regenerates the script and map exports from your installed copy.

On Mac, set `source_engine_root_mac` to the patched source checkout.
`installed_engine_root_mac` is only an optional, separate developer-baseline
install; it must not point at the source checkout. Build and run the standalone
headless suites from the repository root:

```sh
ENGINE="$(python3 Tools/wot_config.py get source_engine_root_mac)"
for target in WoTBootstrapCoreTests WoTUpdaterCoreTests WoTUpdateTransportsTests WoTRetailReaderTests; do
  Tools/build/build_guarded_ue58.sh --engine "$ENGINE" --project "$PWD/WoT.uproject" \
    --target "$target" --platform Mac --configuration Development
done
Binaries/Mac/WoTBootstrapCoreTests --suite WoT.BootstrapCore
Binaries/Mac/WoTUpdaterCoreTests --suite WoT.UpdaterCore
Binaries/Mac/WoTUpdateTransportsTests --suite WoT.Update.PlatformHop
Binaries/Mac/WoTRetailReaderTests --suite WoT.RetailReader
```

The editor automation suite requires the pipeline-generated `Content/` tree:

```sh
Tools/build/build_guarded_ue58.sh --engine "$ENGINE" --project "$PWD/WoT.uproject" \
  --target WoTEditor --platform Mac --configuration Development
"$ENGINE/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/WoT.uproject" \
  -ExecCmds="Automation RunTests WoT; Quit" \
  -unattended -nullrhi -nosplash -nopause -nocrashreports -stdout
```

Judge each run by its exit code and `TEST COMPLETE` line. Both compilers must
stay green: MSVC treats some warnings as errors that Apple Clang lets through,
and vice versa. [CONTRIBUTING.md](CONTRIBUTING.md) has the Windows commands and
the porting rules.

Generated content is never committed (`Content/` is ignored by git). There are
no hand-authored assets and no editor-GUI steps: the pipeline under `Tools/` is deterministic.

## Repository map

| Path | What |
|---|---|
| `Source/WoT/` | The game module: gameplay, networking, HUD, menus, bootstrap |
| `Source/WoTRetailReader/`, `Source/WoTBootstrapCore/`, `Source/WoTUpdaterCore/`, … | The retail reader, first-run bootstrap, signed updater and their tests |
| `Tools/extract`, `Tools/convert`, `Tools/import` | Retail data extraction and conversion, headless import |
| `Tools/dist` | Guarded packaging, release signing, verification and publishing |
| `Tools/public` | Public-tree export, leak guard and provenance scan |
| `Config/` | Engine and game configuration |
| `EnginePatches/UE-5.8/*.json` | Public source/toolchain and patched-file hash pins; the patch itself stays on the licensee-only engine fork |
| `PORT_NOTES.md` | Every intentional difference from the original, and why |

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md). The short version: the shipped
script is the spec, C++ carries the behavior, divergences are written down,
and the original's script, code or data is never pasted anywhere public.
Contributions are accepted under the Developer Certificate of Origin.

Please report security problems privately as described in
[SECURITY.md](SECURITY.md).

## License and legal

The code and documentation in this repository are released under the
[MIT License](LICENSE). That license covers only what was written for this
project. It grants no rights to the original game, its data, or the
trademarks and literary property it is based on; see [NOTICE](NOTICE).

*The Wheel of Time* (1999) was developed by Legend Entertainment and published
by GT Interactive, and is based on the novels of Robert Jordan. The game is
sold today through GOG. This project claims nothing of theirs, redistributes
none of it, accepts no donations, and will promptly honor any takedown request
from a rights holder (see [SECURITY.md](SECURITY.md)).

Unreal Engine is licensed separately by Epic Games under its own agreement.
