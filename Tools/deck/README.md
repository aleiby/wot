# Steam Deck test harness

Runs the Windows build of WoT Reborn on a Steam Deck under Proton from a
desktop, reads the game's own bootstrap heartbeat back, and runs the Windows
path probe (`probe/`) under Proton, so Deck-specific problems (Wine file
semantics, installer discovery, self-update) can be investigated without
sitting at the Deck.

## Setup

1. On the Deck, enable Developer Mode (Settings → System) and allow SSH for the
   `deck` user, then authorize this machine's public key in
   `~/.ssh/authorized_keys` on the Deck.
2. Add the Deck's address to the gitignored `Tools/local.json`:
   `"deck_host": "<deck hostname or address>"` (optional `"deck_user"`,
   default `deck`).
3. Add the game to Steam on the Deck once as a non-Steam game, and note its
   shortcut id. The id names the Proton prefix the harness launches into.

## Commands

All commands read `Tools/local.json`. To use another file, put `--local-json`
before the command: `python3 Tools/deck/deck.py --local-json other.json heartbeat ...`.

```
python3 Tools/deck/deck.py deploy <local game folder> <remote folder>
python3 Tools/deck/deck.py launch <remote folder> --app-id <shortcut id> [--unit wotdeck] [--proton "Proton 11.0"]
python3 Tools/deck/deck.py heartbeat <remote Data folder>
python3 Tools/deck/deck.py show            # bring the game window forward
python3 Tools/deck/deck.py stop [--unit wotdeck]
python3 Tools/deck/deck.py probe <wot_path_probe.exe> --app-id <shortcut id> <probe args...>
python3 Tools/deck/deck.py cells <remote folder> --app-id <shortcut id>
python3 Tools/deck/deck.py lock-cell <remote folder> --app-id <shortcut id>
python3 Tools/deck/deck.py update-cell <remote folder> <release zip on the Deck> --app-id <shortcut id>
```

`probe` copies the probe into the shortcut's Proton prefix, runs it with the
given arguments (for example `env`, `walk <Windows path>`, `census <dir>`),
prints one JSON line per check and exits with the probe's own status.

`launch` starts the game as a user service that outlives the SSH session. In
Game Mode it runs off-screen until `show` brings its window forward, because
gamescope only shows apps that Steam launched itself.

### Cells

Each cell runs the game under Proton, judges it by the game's own heartbeat or
its update folder, prints `PASS` or `FAIL` with a reason, and always stops the
game.

- `cells` launches a fresh install and judges its first run: it reached the
  main menu, found the original installer in Downloads without Browse, and
  completed its update check.
- `lock-cell` needs an install whose update check already staged an update.
  It starts the game directly, runs the launcher a second time, and passes
  only if the staged update was left untouched.
- `update-cell` stages the game folder from a newer release zip of the same
  build configuration (already copied to the Deck), launches, and passes when
  that release runs and its trial is retired.

## Tests

```
PYTHONPATH=Tools/deck python3 -m unittest Tools.deck.test_deck Tools.deck.test_cells
```

The tests fake SSH and need no Deck.
