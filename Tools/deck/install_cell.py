"""Deck acceptance cell for the SteamOS installer (Tools/deck/installer).

`deck.py install-cell` builds Install-WoT-Reborn.desktop from this checkout,
copies it to a scratch folder under the Deck's home and runs it through the
asset's own bootstrap, as Dolphin would, but with the program's test-only
overrides: `--target <scratch>/WoT Reborn` and `--installer-search-dir
<scratch>/dl`. The override replaces every default search location
(Downloads, Desktop, removable media), so the program can only see
`<scratch>/dl`, which holds a HARD LINK to the owner's Downloads installer:
same filesystem, no extra disk, and the program's rename moves only that link.

Safety rules:
- Before and after every case, `~/Downloads/<installer>` must exist with the
  pinned size. If it went missing, the case FAILs and the file is linked back
  from the scratch copy with the same inode before anything is cleaned up; if
  that fails, nothing is cleaned up.
- The program runs without a display, so its console UI answers "Not now" to
  closing Steam: `fresh` and `repair` can never shut Steam down, and they need
  Steam running (otherwise the program would write shortcuts.vdf itself, which
  is the `fallback` case).
- `fallback` shuts Steam down and refuses to run without
  `--allow-steam-shutdown` (the owner's OK).
- Cleanup removes the scratch folder, the scratch shortcut's grid art and its
  compatdata prefix, never anything of the owner's own install. Steam keeps the
  shortcut until the owner removes it in the Steam UI, so the cell prints it.
"""
from __future__ import annotations

import importlib.util
import json
import re
import shlex
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import deck

INSTALLER_DIR = Path(__file__).resolve().parent / "installer"
ASSET_NAME = "Install-WoT-Reborn.desktop"
# install_wot_reborn.ORIGINAL_INSTALLER_PIN; a unit test keeps them equal.
PINNED_INSTALLER_NAME = "setup_the_wheel_of_time_333b_(54846).exe"
PINNED_INSTALLER_SIZE = 1029509080
SCRATCH_PREFIX = "wot-install-cell"
CASES = ("fresh", "repair", "fallback")
DEFAULT_CASES = ("fresh", "repair")
# The owner's own install's shortcut (plan: never touched by a cell).
PROTECTED_APP_IDS = frozenset({2524773648})
STEP3_ROUTES = ("steamos-add-to-steam", "steam-addnonsteamgame-url")
EXISTING_ROUTE = "existing entry reused"
FALLBACK_ROUTE = "shortcuts.vdf fallback"
ART_LEAVES = ("{appid}p.jpg", "{appid}.jpg", "{appid}_hero.jpg", "{appid}_logo.png",
              "{appid}.json")
DATA_MARKER = "wot-install-cell-keep"
COMPATDATA = "$HOME/.local/share/Steam/steamapps/compatdata"
_SCRATCH = re.compile(r"^" + re.escape(SCRATCH_PREFIX) + r"[A-Za-z0-9._-]*$")
_INSTALLER_NAME = re.compile(r"^setup_the_wheel_of_time[A-Za-z0-9 ._()-]*\.exe$")
_STAMP = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ ")
# The account folder install.log names; only art under it is ever removed.
_ACCOUNT = re.compile(r"^/[^\0\n]*/\.local/share/Steam/userdata/[0-9]+$")


@dataclass(frozen=True)
class Verdict:
    ok: bool
    reason: str


class Layout:
    """Remote paths, as double-quoted shell words under the Deck's $HOME."""

    def __init__(self, scratch: str = SCRATCH_PREFIX,
                 installer_name: str = PINNED_INSTALLER_NAME):
        if not _SCRATCH.match(scratch):
            raise deck.DeckError(f"--scratch must be a plain folder name starting with "
                                 f"{SCRATCH_PREFIX!r}: {scratch!r}")
        if not _INSTALLER_NAME.match(installer_name):
            raise deck.DeckError(f"not an original installer file name: {installer_name!r}")
        self.scratch = scratch
        self.installer_name = installer_name

    @staticmethod
    def home(*parts: str) -> str:
        return '"$HOME/' + "/".join(parts) + '"'

    def path(self, *parts: str) -> str:
        return self.home(self.scratch, *parts)

    def shown(self, *parts: str) -> str:
        """The same path for people to read."""
        return "~/" + "/".join((self.scratch,) + parts)

    @property
    def downloads(self) -> str:
        return self.home("Downloads", self.installer_name)

    @property
    def link(self) -> str:
        return self.path("dl", self.installer_name)

    @property
    def entry(self) -> str:
        return self.path(ASSET_NAME)

    def parts(self, case: str) -> Tuple[str, ...]:
        return ("fallback",) if case == "fallback" else ()

    def target(self, case: str, *more: str) -> str:
        return self.path(*self.parts(case), "WoT Reborn", *more)

    def log(self, case: str) -> str:
        return self.path(*self.parts(case), ".wot-reborn-install", "install.log")


def program_arguments(layout: Layout, case: str) -> List[str]:
    """The program's test-only overrides (shell words; $HOME expands on the Deck)."""
    return ["--target", layout.target(case), "--installer-search-dir", layout.path("dl")]


def _builder():
    spec = importlib.util.spec_from_file_location(
        "wot_deck_build_desktop_entry", INSTALLER_DIR / "build_desktop_entry.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_entry() -> bytes:
    """Install-WoT-Reborn.desktop from this checkout (the release builder, stdlib only)."""
    return _builder().build_entry(INSTALLER_DIR)


def bootstrap_stage1() -> str:
    """The `python3 -I -c` program of the asset's Exec line."""
    return _builder().BOOTSTRAP_STAGE1


# --- Scripts (each starts with a marker comment naming its step) ----------------------


def guard_script(layout: Layout) -> str:
    return ("# WOT-CELL-GUARD\n"
            f"f={layout.downloads}\n"
            'if [ -f "$f" ] && [ ! -L "$f" ]; then stat -c \'present %s %i\' -- "$f"; '
            "else echo missing; fi\n")


def steam_script() -> str:
    return ("# WOT-CELL-STEAM\n"
            "if pgrep -x steam >/dev/null; then echo running; else echo stopped; fi\n")


def link_script(layout: Layout) -> str:
    """A hard link (never -s, never a copy) of the Downloads installer in <scratch>/dl."""
    return ("# WOT-CELL-LINK\n"
            "set -e\n"
            f"mkdir -p {layout.path('dl')}\n"
            f"l={layout.link}\n"
            f'if [ ! -e "$l" ]; then ln -- {layout.downloads} "$l"; fi\n'
            '[ -f "$l" ] && [ ! -L "$l" ]\n'
            'stat -c \'linked %i %h\' -- "$l"\n')


def fresh_prep_script(layout: Layout, case: str) -> str:
    return ("# WOT-CELL-PREP\n"
            "set -e\n"
            f"[ ! -e {layout.target(case)} ] || "
            "{ echo 'the scratch target exists from an earlier run'; exit 3; }\n"
            f"rm -f {layout.log(case)}\n"
            "echo ready\n")


def repair_prep_script(layout: Layout) -> str:
    """Delete one Game/ file (recorded) and mark Data/, so the re-run must repair and keep."""
    return ("# WOT-CELL-REPAIR-PREP\n"
            "set -e\n"
            f"cd {layout.target('repair')} 2>/dev/null && [ -f 'WoT Reborn.exe' ] && "
            "[ -d Game ] || "
            "{ echo 'no scratch install to repair: run the fresh case first'; exit 3; }\n"
            "victim=$(find Game -type f | LC_ALL=C sort | head -1)\n"
            '[ -n "$victim" ]\n'
            'size=$(stat -c %s -- "$victim")\n'
            'rm -f -- "$victim"\n'
            "mkdir -p Data\n"
            f"printf 'keep\\n' > Data/{DATA_MARKER}\n"
            f"rm -f {layout.log('repair')}\n"
            'printf "deleted %s %s\\n" "$size" "$victim"\n')


def shutdown_script() -> str:
    return ("# WOT-CELL-STEAM-SHUTDOWN\n"
            "steam -shutdown >/dev/null 2>&1 || true\n"
            "for i in $(seq 60); do pgrep -x steam >/dev/null || "
            "{ echo stopped; exit 0; }; sleep 1; done\n"
            "echo running\n")


def upload_command(layout: Layout) -> str:
    return f"mkdir -p {layout.path()} && cat > {layout.entry}"


def run_script(layout: Layout, case: str, timeout: float) -> str:
    """The asset's own Exec bootstrap, run like Dolphin would, plus the overrides.

    No display: the program's console UI answers "Not now" to closing Steam.
    """
    words = " ".join(program_arguments(layout, case))
    return ("# WOT-CELL-RUN\n"
            f"cd {layout.path()} && env -u DISPLAY -u WAYLAND_DISPLAY "
            f"timeout --kill-after={deck.KILL_AFTER_SECONDS} {int(timeout)} "
            f"python3 -I -c {shlex.quote(bootstrap_stage1())} {layout.entry} {words} "
            "< /dev/null\n")


def log_script(layout: Layout, case: str) -> str:
    return f"# WOT-CELL-LOG\ncat {layout.log(case)} 2>/dev/null || true\n"


_SNAPSHOT = r"""
import json, os, stat, subprocess, sys
target, name, grid, appid, probe, marker = sys.argv[1:7]
def size(path):
    try:
        info = os.lstat(path)
    except OSError:
        return None
    return info.st_size if stat.S_ISREG(info.st_mode) else None
art = {}
if grid and appid:
    for leaf in ("%sp.jpg", "%s.jpg", "%s_hero.jpg", "%s_logo.png", "%s.json"):
        art[leaf % appid] = size(os.path.join(grid, leaf % appid)) is not None
print(json.dumps({
    "exe": size(os.path.join(target, "WoT Reborn.exe")) is not None,
    "game": os.path.isdir(os.path.join(target, "Game")),
    "installer": size(os.path.join(target, name)),
    "art": art,
    "probe": size(os.path.join(target, probe)) if probe else None,
    "marker": size(os.path.join(target, "Data", marker)) is not None,
    "steam": subprocess.call(["pgrep", "-x", "steam"], stdout=subprocess.DEVNULL) == 0,
}))
"""


def snapshot_script(layout: Layout, case: str, grid: str, appid: Optional[int],
                    probe: str) -> str:
    arguments = " ".join([layout.target(case), shlex.quote(PINNED_INSTALLER_NAME),
                          shlex.quote(grid), shlex.quote(str(appid or "")),
                          shlex.quote(probe), shlex.quote(DATA_MARKER)])
    return f"# WOT-CELL-SNAPSHOT\npython3 -I -c {shlex.quote(_SNAPSHOT)} {arguments}\n"


def restore_script(layout: Layout, inode: int) -> str:
    """Link the Downloads installer back from the scratch link with its inode."""
    return ("# WOT-CELL-RESTORE\n"
            f"f={layout.downloads}\n"
            '[ -e "$f" ] && { echo present; exit 0; }\n'
            f"src=$(find {layout.path()} -xdev -type f -inum {int(inode)} -print -quit)\n"
            '[ -n "$src" ] || { echo lost; exit 1; }\n'
            'mkdir -p "$HOME/Downloads" && ln -- "$src" "$f" && echo "restored $src"\n')


def cleanup_script(layout: Layout, art: Dict[int, str]) -> str:
    """Remove the scratch art and prefix of each scratch appid, then the scratch folder.

    ``art`` maps each scratch shortcut's appid to its grid folder ("" if
    install.log named none); only those exact files and that prefix go.
    """
    lines = ["# WOT-CELL-CLEANUP", "set -e"]
    for appid, grid in sorted(art.items()):
        if appid in PROTECTED_APP_IDS:
            raise deck.DeckError(f"refusing to clean up the owner's shortcut {appid}")
        if grid:
            leaves = " ".join(shlex.quote(grid + "/" + leaf.format(appid=int(appid)))
                              for leaf in ART_LEAVES)
            lines.append(f"rm -f -- {leaves}")
        lines.append(f'rm -rf -- "{COMPATDATA}/{int(appid)}"')
    lines.append(f"rm -rf -- {layout.path()}")
    lines.append("echo cleaned")
    return "\n".join(lines) + "\n"


# --- Judging ------------------------------------------------------------------------


def parse_guard(output: bytes) -> Tuple[bool, Optional[int], Optional[int]]:
    """(present, size, inode) from guard_script's output."""
    words = output.decode("utf-8", "replace").split()
    if len(words) == 3 and words[0] == "present" and words[1].isdigit() \
            and words[2].isdigit():
        return True, int(words[1]), int(words[2])
    return False, None, None


def parse_install_log(text: str) -> dict:
    info = {"route": None, "appid": None, "account": None, "steam_closed": False,
            "stopped": None, "done": False}
    for line in text.splitlines():
        message = _STAMP.sub("", line, count=1)
        match = re.match(r"shortcut route: (.+) \(appid (\d+)\)$", message)
        if match:
            info["route"], info["appid"] = match.group(1), int(match.group(2))
        match = re.match(r"steam account folder: (.+)$", message)
        if match:
            info["account"] = match.group(1)
        if message == "Steam closed":
            info["steam_closed"] = True
        if message.startswith("stopped: ") and info["stopped"] is None:
            info["stopped"] = message[len("stopped: "):]
        if message == "done":
            info["done"] = True
    return info


def judge(case: str, returncode: int, log: dict, snapshot: dict,
          deleted: Optional[Tuple[int, str]] = None) -> Verdict:
    if returncode != 0 or not log["done"]:
        return Verdict(False, f"the installer exited {returncode}: "
                              f"{log['stopped'] or 'no done line in install.log'}")
    route, appid = log["route"], log["appid"]
    if appid is None:
        return Verdict(False, "install.log names no shortcut")
    if appid in PROTECTED_APP_IDS:
        return Verdict(False, f"the program reported the owner's own shortcut {appid}")
    if not snapshot.get("exe") or not snapshot.get("game"):
        return Verdict(False, "the scratch target has no WoT Reborn.exe and Game/")
    if snapshot.get("installer") != PINNED_INSTALLER_SIZE:
        return Verdict(False, f"the original installer beside the game has size "
                              f"{snapshot.get('installer')}, not {PINNED_INSTALLER_SIZE}")
    art = snapshot.get("art") or {}
    if len(art) != len(ART_LEAVES) or not all(art.values()):
        return Verdict(False, f"grid art missing for appid {appid}: "
                              f"{sorted(leaf for leaf, ok in art.items() if not ok)}")
    if case in ("fresh", "repair"):
        if log["steam_closed"] or not snapshot.get("steam"):
            return Verdict(False, "Steam was shut down or is no longer running")
    if case == "fresh" and route not in STEP3_ROUTES:
        return Verdict(False, f"route {route!r}: the shortcut was not added through the "
                              "running Steam (step 3)")
    if case == "repair":
        if route != EXISTING_ROUTE:
            return Verdict(False, f"route {route!r}: the repair did not reuse the shortcut")
        if deleted is None or snapshot.get("probe") != deleted[0]:
            return Verdict(False, f"the deleted Game file {deleted and deleted[1]!r} "
                                  "was not restored")
        if not snapshot.get("marker"):
            return Verdict(False, "Data/ was not kept")
    if case == "fallback" and route != FALLBACK_ROUTE:
        return Verdict(False, f"route {route!r}: not the shortcuts.vdf fallback")
    detail = f"route {route} (appid {appid}), art written, installer placed"
    if case == "repair":
        detail += f", {deleted[1]} restored, Data/ kept"
    return Verdict(True, detail)


# --- Driver ------------------------------------------------------------------------


@dataclass
class _State:
    inode: Optional[int] = None          # the Downloads installer's, at the start
    art: Dict[int, str] = field(default_factory=dict)        # scratch appid -> grid
    shortcuts: Dict[int, str] = field(default_factory=dict)  # scratch appid -> exe
    uploaded: bool = False
    steam_stopped: bool = False          # by this cell (the fallback case)
    stop: bool = False
    started: bool = False                # a case got past its first guard


def _ssh(target, runner, script: str, timeout: float = 120):
    result = deck.ssh(target, runner, script, timeout)
    deck._require_reached(result, script.splitlines()[0].lstrip("# ").lower())
    return result


def _text(result) -> str:
    return result.stdout.decode("utf-8", "replace").strip()


def _guard(target, runner, layout: Layout) -> Tuple[bool, Optional[int], str]:
    present, size, inode = parse_guard(_ssh(target, runner, guard_script(layout)).stdout)
    if not present:
        return False, None, f"~/Downloads/{layout.installer_name} is missing"
    if size != PINNED_INSTALLER_SIZE:
        return False, inode, (f"~/Downloads/{layout.installer_name} is {size} bytes, not "
                              f"the pinned {PINNED_INSTALLER_SIZE}")
    return True, inode, "present with the pinned size"


def _restore(target, runner, layout: Layout, state: _State) -> str:
    if state.inode is None:
        return "LOST: no inode was recorded, nothing to restore from"
    result = deck.ssh(target, runner, restore_script(layout, state.inode))
    text = _text(result)
    if result.returncode == 0 and text.startswith(("restored", "present")):
        ok, _, why = _guard(target, runner, layout)
        if ok:
            return text
        return f"LOST: after restoring, {why}"
    return f"LOST: {text or result.stderr.decode(errors='replace')}"


def _run_case(target, runner, layout: Layout, case: str, timeout: float,
              entry: bytes, state: _State, out: Callable[[str], None]) -> Verdict:
    steam = _text(_ssh(target, runner, steam_script()))
    deleted = None
    if case in ("fresh", "repair") and steam != "running":
        return Verdict(False, "Steam isn't running: start it (Game Mode) first; without it "
                              "the program would write shortcuts.vdf itself (the fallback)")
    if case == "fallback" and steam == "running":
        out("install-cell: shutting Steam down for the fallback case")
        state.steam_stopped = True
        if _text(_ssh(target, runner, shutdown_script(), 120)) != "stopped":
            return Verdict(False, "Steam did not close within 60 s")
    if case == "repair":
        prep = _ssh(target, runner, repair_prep_script(layout))
        words = _text(prep).split(" ", 2)
        if prep.returncode != 0 or len(words) != 3 or words[0] != "deleted" \
                or not words[1].isdigit():
            return Verdict(False, _text(prep) or "could not prepare the repair")
        deleted = (int(words[1]), words[2])
    else:
        prep = _ssh(target, runner, fresh_prep_script(layout, case))
        if prep.returncode != 0:
            return Verdict(False, _text(prep) or "could not prepare the scratch target")
    linked = _ssh(target, runner, link_script(layout))
    words = _text(linked).split()
    if linked.returncode != 0 or len(words) != 3 or words[0] != "linked" \
            or not words[1].isdigit() or int(words[1]) != state.inode \
            or not words[2].isdigit() or int(words[2]) < 2:
        state.stop = True
        return Verdict(False, f"<scratch>/dl is not a hard link of the Downloads installer "
                              f"(inode {state.inode}): "
                              f"{_text(linked) or linked.stderr.decode(errors='replace')}")
    if not state.uploaded:
        upload = runner.run(deck._ssh_argv(target) + [upload_command(layout)], entry, 120)
        if upload.returncode != 0:
            state.stop = True
            return Verdict(False, "could not copy the desktop entry: "
                                  + upload.stderr.decode(errors="replace"))
        state.uploaded = True
    ran = _ssh(target, runner, run_script(layout, case, timeout), timeout + 120)
    log = parse_install_log(_text(_ssh(target, runner, log_script(layout, case))))
    appid = log["appid"]
    account = log["account"] or ""
    grid = account + "/config/grid" if _ACCOUNT.match(account) else ""
    if appid is not None and appid not in PROTECTED_APP_IDS:
        state.art[appid] = grid  # "" when install.log named no valid account folder
        state.shortcuts[appid] = layout.shown(*layout.parts(case), "WoT Reborn",
                                              "WoT Reborn.exe")
    snapshot_result = _ssh(target, runner, snapshot_script(
        layout, case, grid, appid, deleted[1] if deleted else ""))
    try:
        snapshot = json.loads(_text(snapshot_result))
    except ValueError:
        return Verdict(False, "could not read the scratch install back: "
                              + snapshot_result.stderr.decode(errors="replace"))
    if ran.returncode != 0:
        out(ran.stderr.decode("utf-8", "replace").strip())
    return judge(case, ran.returncode, log, snapshot, deleted)


def run_install_cells(target, runner, cases: Sequence[str] = DEFAULT_CASES,
                      layout: Optional[Layout] = None, allow_steam_shutdown: bool = False,
                      keep: bool = False, timeout: float = 1800.0,
                      entry: Optional[bytes] = None,
                      out: Callable[[str], None] = print) -> List[Tuple[str, Verdict]]:
    layout = layout or Layout()
    unknown = [case for case in cases if case not in CASES]
    if unknown:
        raise deck.DeckError(f"unknown install-cell case(s) {unknown}; choose from {CASES}")
    ordered = [case for case in CASES if case in cases]  # fallback last: it stops Steam
    verdicts: List[Tuple[str, Verdict]] = []
    state = _State()
    ok, state.inode, why = _guard(target, runner, layout)
    if not ok:
        return [("downloads-installer", Verdict(False, why + "; nothing was touched"))]
    entry = entry if entry is not None else build_entry()
    for case in ordered:
        name = f"install-{case}"
        if case == "fallback" and not allow_steam_shutdown:
            verdicts.append((name, Verdict(False, "refused: the fallback case shuts Steam "
                                                  "down; pass --allow-steam-shutdown (needs "
                                                  "the owner's OK)")))
            continue
        if state.stop:
            verdicts.append((name, Verdict(False, "not run: an earlier case stopped the cell")))
            continue
        ok, _, why = _guard(target, runner, layout)
        if not ok:
            verdicts.append((name, Verdict(False, "before the case: " + why)))
            state.stop = True
            continue
        state.started = True
        try:
            verdict = _run_case(target, runner, layout, case, timeout, entry, state, out)
        except (deck.DeckError, subprocess.TimeoutExpired) as exc:
            verdict = Verdict(False, f"the case could not finish: {exc}")
            state.stop = True
        ok, _, why = _guard(target, runner, layout)
        if not ok:
            restored = _restore(target, runner, layout, state)
            verdict = Verdict(False, f"~/Downloads/{layout.installer_name} went missing "
                                     f"during the case ({why}); {restored}")
            state.stop = True
        verdicts.append((name, verdict))
    if keep:
        out(f"install-cell: --keep: left {layout.shown()} and its shortcut in place")
    elif not state.started:
        out("install-cell: no case ran, so there is nothing to clean up")
    else:
        verdicts.append(("cleanup", _cleanup(target, runner, layout, state, out)))
    _print_owner_steps(layout, state, out)
    return verdicts


def _cleanup(target, runner, layout: Layout, state: _State,
             out: Callable[[str], None]) -> Verdict:
    ok, _, why = _guard(target, runner, layout)
    if not ok:
        restored = _restore(target, runner, layout, state)
        if restored.startswith("LOST"):
            return Verdict(False, f"NOT cleaned up: {why}; {restored}. The scratch folder "
                                  f"{layout.shown()} may hold the only copy")
        out(f"install-cell: {restored}")
    result = _ssh(target, runner, cleanup_script(layout, state.art), 300)
    if result.returncode != 0 or _text(result) != "cleaned":
        return Verdict(False, "cleanup failed: " + result.stderr.decode(errors="replace"))
    removed = ", ".join(str(appid) for appid in sorted(state.art)) or "none"
    return Verdict(True, f"removed {layout.shown()} and the grid art and compatdata prefix "
                         f"of scratch appid(s) {removed}")


def _print_owner_steps(layout: Layout, state: _State, out: Callable[[str], None]) -> None:
    for appid, exe in sorted(state.shortcuts.items()):
        out(f"OWNER: remove the scratch shortcut in Steam: Library > Non-Steam > "
            f"'WoT Reborn.exe' (appid {appid}, Exe {exe}) > Manage > Remove non-Steam "
            "game from your library. Steam keeps it until it is removed there.")
    if state.steam_stopped:
        out("OWNER: Steam was shut down for the fallback case; return to Gaming Mode (or "
            "start Steam) to bring it back. The fallback left a shortcuts.vdf.wot-backup-* "
            "copy beside shortcuts.vdf.")
