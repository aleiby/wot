# SteamOS installer (`Install-WoT-Reborn.desktop`)

The release asset a Steam Deck owner downloads in Desktop Mode and opens. It
installs the latest WoT Reborn release into `~/Games/WoT Reborn`, places the
original game installer beside the game if it finds one on the device, adds the
game to Steam and gives it library artwork. Running it again repairs or updates
the install. It needs only the Python standard library, Python 3.9 or later
(SteamOS ships 3.14).

## Files

- `install_wot_reborn.py`: the program. Importable, with `main()`; every side
  effect goes through `Env` so the tests need no network, Steam or Deck. Its
  docstring lists the steps. Test-only flags: `--target <dir>` (install folder),
  `--installer-search-dir <dir>` (the only folder searched for the original
  installer; it replaces Downloads, Desktop and removable media) and
  `--self-test`. The desktop entry passes no arguments.
- `vdf.py`: the binary VDF reader and writer for `shortcuts.vdf` (untouched
  entries and unknown keys re-serialize byte for byte) and the text VDF reader
  for `loginusers.vdf`.
- `build_desktop_entry.py`: assembles the asset deterministically from the
  program, `vdf.py` and `art/`. The `Exec` line runs a one-line `python3 -I -c`
  bootstrap with `%k`; the payload rides in `#` comment lines.
- `art/`: the committed Steam artwork (JPEG capsules and hero, PNG logo, logo
  position JSON). These files are the asset's inputs, so building it needs no
  Pillow. `art/wordmark.png` is the generator's title lettering; it is not in
  the asset.
- `make_steam_art.py`: dev-side generator for `art/` (Pillow). It uses the
  blurred setup backdrop that ships in the game and `art/wordmark.png`.
- `test_*.py`: unit tests.

## Build the asset

```
python3 Tools/deck/installer/build_desktop_entry.py --out Install-WoT-Reborn.desktop
```

The same inputs give the same bytes on any platform (text files are normalised
to LF). The file holds nothing release-specific, so it is rebuilt and attached
to every release: `Tools/dist/publish_release.py` builds it at the release's
build commit, and `verify_release.py --published` rebuilds it there and
compares the bytes.

## Regenerate the artwork

```
python3 Tools/deck/installer/make_steam_art.py \
    --backdrop Source/WoT/UI/Resources/WoTSetupBackdropDefault.png \
    --wordmark Tools/deck/installer/art/wordmark.png \
    --out Tools/deck/installer/art
```

The output is deterministic for a given Pillow version and inputs. Every image
is pinned by its SHA-256 in `Tools/public/binary-allowlist.sha256`, so changed
art needs updated entries there.

## Tests

```
python3 -m unittest discover -s Tools/deck/installer -p "test_*.py"
```
