# Steam Deck test harness

Runs the Windows build of WoT Reborn on a Steam Deck under Proton and judges
each run by the game's own heartbeat, so Deck-specific problems (Wine file
semantics, installer discovery, self-update) can be tested from a desktop.

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

All commands read `Tools/local.json`; pass `--local-json` to use another file.

```
python3 Tools/deck/deck.py deploy <local game folder> <remote folder>
python3 Tools/deck/deck.py launch <remote folder> --app-id <shortcut id> [--unit wotdeck] [--proton "Proton 11.0"]
python3 Tools/deck/deck.py heartbeat <remote Data folder>
python3 Tools/deck/deck.py show            # bring the game window forward
python3 Tools/deck/deck.py stop [--unit wotdeck]
```

`launch` starts the game as a user service that outlives the SSH session. In
Game Mode it runs off-screen until `show` brings its window forward, because
gamescope only shows apps that Steam launched itself.

## Tests

```
PYTHONPATH=Tools/deck python3 -m unittest Tools.deck.test_deck
```

The tests fake SSH and need no Deck.
