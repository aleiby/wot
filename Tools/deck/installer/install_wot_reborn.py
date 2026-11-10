#!/usr/bin/env python3
"""Install WoT Reborn on SteamOS (Steam Deck) and add it to Steam.

This is the program inside the ``Install-WoT-Reborn.desktop`` release asset
(see ``build_desktop_entry.py``). It needs only the Python standard library,
Python 3.9 or later.

What it does, in order (each step is idempotent; running it again repairs or
updates the install):

1. Preflight: Python version, a writable home, Steam present, the target is
   absent or an existing WoT Reborn install, an interrupted earlier swap is
   finished, the game is not running from the target, and there is enough free
   space (``REQUIRED_FREE_BYTES``).
2. Resolve the latest release through the GitHub API and take the
   ``WoT-Reborn-Windows.zip`` asset with its published SHA-256 digest (a
   missing or malformed digest fails closed).
3. Download with HTTP Range resume; a sidecar beside the ``.part`` file holds
   the digest it belongs to, so a stale partial download is never resumed.
4. Extract into a staging folder (rejecting absolute, ``..``, symlink and
   out-of-tree entries), then install: fresh, or repair-swap ``Game/`` and the
   launcher (with ``Game.old`` rollback and ``.wot-update/`` removal). A newer
   installed release is kept.
5. Find the original game installer (pinned size and SHA-256) in Downloads,
   Desktop or removable media and place it beside ``WoT Reborn.exe`` under its
   pinned name, never replacing an existing file.
6. Add the Steam shortcut (SteamOS's own ``steamos-add-to-steam``, or its
   steps done by hand, then a ``shortcuts.vdf`` fallback with Steam closed)
   and write the library artwork for its appid.

Test-only overrides (the desktop entry passes no arguments):
    --target <dir>                 install here instead of ~/Games/WoT Reborn
    --installer-search-dir <dir>   look for the original installer only here
    --self-test                    print a marker and exit (bootstrap check)
"""
from __future__ import annotations

import argparse
import contextlib
import datetime
import errno
import getpass
import hashlib
import http.client
import json
import os
import re
import secrets
import shutil
import stat
import subprocess
import sys
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Optional, Sequence, Tuple

import vdf

# --- Constants ---------------------------------------------------------------

TITLE = "Install WoT Reborn"
SELF_TEST_MARKER = "WOT-REBORN-INSTALLER-SELF-TEST-OK"
USER_AGENT = "WoT-Reborn-SteamOS-installer"
MIN_PYTHON = (3, 9)

RELEASE_API_URL = "https://api.github.com/repos/aleiby/wot/releases/latest"
PLAYER_ZIP_ASSET = "WoT-Reborn-Windows.zip"
MAX_API_BYTES = 4 * 1024 * 1024

INSTALL_DIR_NAME = "WoT Reborn"
TOP_FOLDER = "WoT Reborn"          # the single top folder inside the player zip
LAUNCHER_EXE = "WoT Reborn.exe"
GAME_DIR = "Game"
GAME_OLD_DIR = "Game.old"
UPDATE_WORKSPACE = ".wot-update"
UPDATE_WORKSPACE_DISCARDED = ".wot-update.discarded"
WORK_DIR_NAME = ".wot-reborn-install"
RELEASE_VERSION_PARTS = ("Game", "WoT", "Build", "WoTRelease", "release-version.json")
RELEASE_VERSION_FORMAT = "wot-release-version-v1"
GAME_PROCESS_NAMES = ("WoT Reborn.exe", "WoT-Win64-Shipping.exe", "WoT-Win64-Test.exe")

# The peak covers the zip, the extracted game, the content the game converts on
# first launch and a received 1 GB original installer in Data/OriginalGame.
REQUIRED_FREE_BYTES = 5 * 1024 ** 3
REQUIRED_FREE_TEXT = "5 GB"

SHORTCUT_NAME = "WoT Reborn"
ADD_POLL_SECONDS = 30.0
STEAM_SHUTDOWN_SECONDS = 60.0
POLL_INTERVAL = 1.0
STEAMOS_ADD_TO_STEAM = "/usr/bin/steamos-add-to-steam"
ADD_SENTINEL = "/tmp/addnonsteamgamefile"

INSTALLER_NAME_PREFIX = "setup_the_wheel_of_time"
CHUNK = 1024 * 1024
DOWNLOAD_ATTEMPTS = 5

ART_FILES = (
    ("capsule_portrait.jpg", "{appid}p.jpg"),
    ("capsule_wide.jpg", "{appid}.jpg"),
    ("hero.jpg", "{appid}_hero.jpg"),
    ("logo.png", "{appid}_logo.png"),
    ("logo.json", "{appid}.json"),
)
ART_DIR = Path(__file__).resolve().parent / "art"


@dataclass(frozen=True)
class InstallerPin:
    name: str
    size: int
    sha256: str


# Source/WoTBootstrapCore/Public/WoTBootstrapCore/ArtifactTypes.h
ORIGINAL_INSTALLER_PIN = InstallerPin(
    "setup_the_wheel_of_time_333b_(54846).exe",
    1029509080,
    "0ad6474ebc4b5d367b27197fd856733beb45179dc6d6c094641521b16b63f8bb",
)


class InstallError(Exception):
    """An expected failure. The message is plain player text that says what to do."""


class Cancelled(Exception):
    """The player closed the progress window."""


@dataclass(frozen=True)
class ProcessInfo:
    pid: int
    comm: str
    argv: Tuple[str, ...]


@dataclass(frozen=True)
class Release:
    tag: str
    url: str
    size: int
    sha256: str


@dataclass(frozen=True)
class Account:
    steamid64: int
    account_id: int


@dataclass
class InstallerOutcome:
    status: str  # already | moved | copied | not-found | blocked | copy-failed
    dest: Path
    source: Optional[Path] = None
    rejected: List[Path] = field(default_factory=list)
    detail: str = ""


@dataclass
class ShortcutOutcome:
    appid: int
    route: str
    steam_closed: bool = False


@dataclass
class Options:
    target: Optional[Path]
    installer_search_dir: Optional[Path]
    self_test: bool


# --- Seams -------------------------------------------------------------------


class Env:
    """Every side effect outside plain file I/O under home/target goes through here."""

    def __init__(
        self,
        home: Path,
        root: Path = Path("/"),
        run: Optional[Callable[[Sequence[str], Optional[float]], "subprocess.CompletedProcess"]] = None,
        spawn: Optional[Callable[[Sequence[str]], None]] = None,
        which: Optional[Callable[[str], Optional[str]]] = None,
        urlopen: Optional[Callable[[urllib.request.Request, float], object]] = None,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
        processes: Optional[Callable[[], List[ProcessInfo]]] = None,
        disk_free: Optional[Callable[[Path], int]] = None,
        now: Optional[Callable[[], datetime.datetime]] = None,
        user: Optional[str] = None,
        environ: Optional[Dict[str, str]] = None,
        pin: InstallerPin = ORIGINAL_INSTALLER_PIN,
        pid: Optional[int] = None,
        lock: Optional[Callable[[Path], bool]] = None,
    ) -> None:
        self.home = Path(home)
        self.root = Path(root)
        self._children: List[subprocess.Popen] = []
        self.run = run or _default_run
        self.spawn = spawn or self._default_spawn
        self.which = which or shutil.which
        self.urlopen = urlopen or _default_urlopen
        self.clock = clock
        self.sleep = sleep
        self._processes = processes
        self.disk_free = disk_free or (lambda path: shutil.disk_usage(str(path)).free)
        self.now = now or (lambda: datetime.datetime.now(datetime.timezone.utc))
        self.user = user if user is not None else _current_user()
        self.environ = environ if environ is not None else dict(os.environ)
        self.pin = pin
        self.pid = os.getpid() if pid is None else pid
        self.lock = lock or self._default_lock
        self._lock_handle = None

    def system_path(self, absolute: str) -> Path:
        """An absolute system path (``/tmp/...``) under this environment's root."""
        return self.root.joinpath(absolute.lstrip("/"))

    def processes(self) -> List[ProcessInfo]:
        for child in list(self._children):
            if child.poll() is not None:  # reap, so no zombie looks like a live process
                self._children.remove(child)
        if self._processes is not None:
            return self._processes()
        return _list_processes(self.system_path("/proc"))

    def _default_lock(self, path: Path) -> bool:
        """Hold an exclusive advisory lock for the rest of this run; False if taken."""
        try:
            import fcntl
        except ImportError:  # not POSIX (the unit tests on Windows): nothing to lock
            return True
        handle = open(str(path), "a+b")
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            handle.close()
            return False
        self._lock_handle = handle  # released when the process exits
        return True

    def _default_spawn(self, argv: Sequence[str]) -> None:
        self._children.append(subprocess.Popen(
            list(argv), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, close_fds=True, start_new_session=True))


def _default_run(argv: Sequence[str], timeout: Optional[float]) -> "subprocess.CompletedProcess":
    try:
        return subprocess.run(list(argv), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout)
    except FileNotFoundError:
        return subprocess.CompletedProcess(list(argv), 127, b"", b"not found")
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(list(argv), 124, b"", b"timed out")


def _default_urlopen(request: urllib.request.Request, timeout: float):
    return urllib.request.urlopen(request, timeout=timeout)


def _current_user() -> str:
    try:
        return getpass.getuser()
    except Exception:  # no login name available: removable media are skipped
        return ""


def _list_processes(proc: Path) -> List[ProcessInfo]:
    result: List[ProcessInfo] = []
    try:
        names = os.listdir(str(proc))
    except OSError:
        return result
    for name in names:
        if not name.isdigit():
            continue
        base = proc / name
        try:
            status = (base / "stat").read_text(errors="replace")
            state = status[status.rfind(")") + 2:].split()[:1]
            if state == ["Z"]:
                continue
            comm = (base / "comm").read_text(errors="replace").strip()
            raw = (base / "cmdline").read_bytes()
        except OSError:
            continue
        argv = tuple(part.decode("utf-8", "surrogateescape") for part in raw.split(b"\0") if part)
        result.append(ProcessInfo(int(name), comm, argv))
    return result


# --- Log ---------------------------------------------------------------------


class Log:
    """``install.log``: timestamped lines, paths only. Buffered until the folder exists."""

    def __init__(self, now: Callable[[], datetime.datetime]) -> None:
        self._now = now
        self.path: Optional[Path] = None
        self._pending: List[str] = []

    def attach(self, path: Path) -> None:
        self.path = path
        pending, self._pending = self._pending, []
        for line in pending:
            self._append(line)

    def write(self, message: str) -> None:
        stamp = self._now().strftime("%Y-%m-%dT%H:%M:%SZ")
        for line in message.splitlines() or [""]:
            text = "%s %s" % (stamp, line)
            if self.path is None:
                self._pending.append(text)
            else:
                self._append(text)

    def _append(self, line: str) -> None:
        try:
            with open(str(self.path), "a", encoding="utf-8") as handle:
                handle.write(line + "\n")
        except OSError:
            sys.stderr.write(line + "\n")


# --- UI ----------------------------------------------------------------------


def _pango_escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class UI:
    """Progress, a question, and the final message. The base class only logs to stderr."""

    def start(self) -> None:
        pass

    def stage(self, text: str) -> None:
        sys.stderr.write(text + "\n")

    def fraction(self, value: float, detail: str = "") -> None:
        pass

    def finish(self) -> None:
        pass

    def question(self, text: str, yes: str, no: str) -> bool:
        sys.stderr.write(text + "\n(no answer possible here: treated as '%s')\n" % no)
        return False

    def info(self, text: str) -> None:
        sys.stderr.write(text + "\n")

    def error(self, text: str) -> None:
        sys.stderr.write(text + "\n")


class ConsoleUI(UI):
    """No display (for example over SSH): messages go to stderr."""


class ZenityUI(UI):
    def __init__(self, env: Env, zenity: str) -> None:
        self.env = env
        self.zenity = zenity
        self._proc: Optional[subprocess.Popen] = None
        self._text = ""
        self._last = 0.0

    def start(self) -> None:
        try:
            self._proc = subprocess.Popen(
                [self.zenity, "--progress", "--title=" + TITLE, "--text=Starting...",
                 "--percentage=0", "--auto-close", "--width=480"],
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            self._proc = None

    def _send(self, *lines: str) -> None:
        proc = self._proc
        if proc is None:
            return
        if proc.poll() is not None:
            self._proc = None
            raise Cancelled()
        try:
            proc.stdin.write("".join(line + "\n" for line in lines).encode("utf-8"))
            proc.stdin.flush()
        except OSError:
            self._proc = None
            raise Cancelled()

    def stage(self, text: str) -> None:
        self._text = text
        self._send("0", "# " + _pango_escape(text))

    def fraction(self, value: float, detail: str = "") -> None:
        now = self.env.clock()
        if now - self._last < 0.25 and value < 1.0:
            return
        self._last = now
        percent = max(0, min(99, int(value * 100)))  # 100 would auto-close the window
        text = self._text + (" " + detail if detail else "")
        self._send(str(percent), "# " + _pango_escape(text))

    def finish(self) -> None:
        proc, self._proc = self._proc, None
        if proc is None:
            return
        try:
            proc.stdin.write(b"100\n")
            proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()

    def _dialog(self, kind: str, text: str, extra: Sequence[str] = ()) -> int:
        argv = [self.zenity, kind, "--title=" + TITLE, "--width=480",
                "--text=" + _pango_escape(text)] + list(extra)
        try:
            return self.env.run(argv, None).returncode
        except OSError:
            return 255

    def question(self, text: str, yes: str, no: str) -> bool:
        return self._dialog("--question", text, ["--ok-label=" + yes, "--cancel-label=" + no]) == 0

    def info(self, text: str) -> None:
        if self._dialog("--info", text) not in (0, 1):
            UI.info(self, text)

    def error(self, text: str) -> None:
        if self._dialog("--error", text) not in (0, 1):
            UI.error(self, text)


class KDialogUI(UI):
    def __init__(self, env: Env, kdialog: str) -> None:
        self.env = env
        self.kdialog = kdialog

    def stage(self, text: str) -> None:
        try:
            self.env.spawn([self.kdialog, "--title", TITLE, "--passivepopup", text, "4"])
        except OSError:
            UI.stage(self, text)

    def _dialog(self, argv: Sequence[str]) -> int:
        try:
            return self.env.run([self.kdialog, "--title", TITLE] + list(argv), None).returncode
        except OSError:
            return 255

    def question(self, text: str, yes: str, no: str) -> bool:
        return self._dialog(["--yes-label", yes, "--no-label", no, "--yesno", text]) == 0

    def info(self, text: str) -> None:
        if self._dialog(["--msgbox", text]) not in (0, 1):
            UI.info(self, text)

    def error(self, text: str) -> None:
        if self._dialog(["--error", text]) not in (0, 1):
            UI.error(self, text)


class NotifyUI(UI):
    """No dialog tool: desktop notifications plus the log; questions are answered no."""

    def __init__(self, env: Env, notify: Optional[str]) -> None:
        self.env = env
        self.notify = notify

    def _send(self, text: str) -> None:
        sys.stderr.write(text + "\n")
        if self.notify:
            try:
                self.env.run([self.notify, "--app-name=" + TITLE, TITLE, text], 10)
            except OSError:
                pass

    def start(self) -> None:
        self._send("Installing WoT Reborn. This takes a few minutes; a message appears when it is done.")

    def info(self, text: str) -> None:
        self._send(text)

    def error(self, text: str) -> None:
        self._send(text)


def choose_ui(env: Env) -> UI:
    if not (env.environ.get("DISPLAY") or env.environ.get("WAYLAND_DISPLAY")):
        return ConsoleUI()
    zenity = env.which("zenity")
    if zenity:
        return ZenityUI(env, zenity)
    kdialog = env.which("kdialog")
    if kdialog:
        return KDialogUI(env, kdialog)
    return NotifyUI(env, env.which("notify-send"))


# --- Small helpers -------------------------------------------------------------


def _rename(src: Path, dst: Path) -> None:
    """Rename; the swap goes through here so tests can inject a failure."""
    os.rename(str(src), str(dst))


def _replace(src: Path, dst: Path) -> None:
    os.replace(str(src), str(dst))


_scandir = os.scandir


def _remove_path(path: Path) -> None:
    if path.is_symlink() or (os.path.lexists(str(path)) and not path.is_dir()):
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(str(path))


def _write_atomic(path: Path, data: bytes) -> None:
    temp = path.with_name(".%s.%s.tmp" % (path.name, secrets.token_hex(4)))
    try:
        with open(str(temp), "xb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(str(temp), str(path))
    except BaseException:
        with contextlib.suppress(OSError):
            temp.unlink()
        raise


def _move_noreplace(src: Path, dst: Path) -> None:
    """Rename src to dst on the same filesystem without ever replacing dst.

    Raises FileExistsError when dst exists, and another OSError (EXDEV) when
    the two are on different filesystems.
    """
    try:
        os.link(str(src), str(dst))
    except FileExistsError:
        raise
    except OSError as exc:
        if exc.errno == errno.EXDEV:
            raise
        # No hard links here (for example exFAT): check, then rename.
        if os.path.lexists(str(dst)):
            raise FileExistsError(errno.EEXIST, "file exists", str(dst))
        os.rename(str(src), str(dst))
        return
    with contextlib.suppress(OSError):
        os.unlink(str(src))


def _sha256_file(path: Path, progress: Optional[Callable[[int], None]] = None) -> str:
    digest = hashlib.sha256()
    done = 0
    with open(str(path), "rb") as handle:
        while True:
            block = handle.read(CHUNK)
            if not block:
                break
            digest.update(block)
            done += len(block)
            if progress:
                progress(done)
    return digest.hexdigest()


def _gb(value: int) -> str:
    return "%.1f GB" % (value / 1024 ** 3)


def _mb(value: int) -> str:
    return "%d MB" % (value // (1024 * 1024))


# --- Layout --------------------------------------------------------------------


class Layout:
    def __init__(self, target: Path) -> None:
        self.target = target
        self.work = target.parent / WORK_DIR_NAME
        self.part = self.work / (PLAYER_ZIP_ASSET + ".part")
        self.sidecar = self.work / (PLAYER_ZIP_ASSET + ".part.sha256")
        self.zip = self.work / PLAYER_ZIP_ASSET
        self.stage = self.work / "stage"
        self.log = self.work / "install.log"

    @property
    def exe(self) -> Path:
        return self.target / LAUNCHER_EXE


# --- Preflight -------------------------------------------------------------------


def target_kind(target: Path) -> str:
    """'fresh' (absent) or 'install'; raises InstallError for anything else."""
    if not os.path.lexists(str(target)):
        return "fresh"
    if not target.is_dir():
        raise InstallError(
            "%s already exists but isn't a folder.\n\nRename or remove it, then run the "
            "installer again." % target)
    if not (target / LAUNCHER_EXE).is_file():
        raise InstallError(
            "The folder %s already exists, but it doesn't hold WoT Reborn.\n\nRename or "
            "remove that folder, then run the installer again." % target)
    return "install"


def recover_interrupted_swap(target: Path, log: Log) -> None:
    game, old = target / GAME_DIR, target / GAME_OLD_DIR
    if not os.path.lexists(str(game)) and old.is_dir():
        _rename(old, game)
        log.write("recovered an interrupted swap: renamed %s back to %s" % (old, game))
    elif game.is_dir() and os.path.lexists(str(old)):
        _remove_path(old)
        log.write("finished an interrupted swap: removed %s" % old)
    discarded = target / UPDATE_WORKSPACE_DISCARDED
    if os.path.lexists(str(discarded)):
        with contextlib.suppress(OSError):
            _remove_path(discarded)


def running_game(processes: Sequence[ProcessInfo], target_posix: str, own_pid: int) -> Optional[ProcessInfo]:
    """A WoT Reborn process whose command line names the target, in POSIX or Proton form."""
    posix = target_posix.rstrip("/") + "/"
    wine = ("Z:" + posix.replace("/", "\\")).casefold()
    names = [name.casefold() for name in GAME_PROCESS_NAMES]
    for process in processes:
        if process.pid == own_pid:
            continue
        joined = " ".join(process.argv + (process.comm,))
        folded = joined.casefold()
        if not any(name in folded for name in names):
            continue
        if posix in joined or wine in folded:
            return process
    return None


def check_free_space(env: Env, target: Path) -> None:
    probe = target
    while not probe.exists() and probe.parent != probe:
        probe = probe.parent
    try:
        free = env.disk_free(probe)
    except OSError:
        return
    if free < REQUIRED_FREE_BYTES:
        raise InstallError(
            "WoT Reborn needs %s of free space in %s (the download, the game, and the data it "
            "builds on first launch). Only %s is free.\n\nFree up some space, then run the "
            "installer again." % (REQUIRED_FREE_TEXT, target.parent, _gb(free)))


def steam_root(env: Env) -> Path:
    return env.home / ".local" / "share" / "Steam"


def choose_account(text: str) -> Account:
    """The MostRecent login in loginusers.vdf (newest Timestamp breaks ties)."""
    document = vdf.parse_text(text)
    users = vdf.get_ci(document, "users")
    best: Optional[Tuple[Tuple[bool, int], Account]] = None
    if isinstance(users, dict):
        for steamid, fields in users.items():
            if not isinstance(fields, dict) or not steamid.isdigit():
                continue
            account_id = int(steamid) & 0xFFFFFFFF
            if account_id == 0:
                continue
            most_recent = str(vdf.get_ci(fields, "MostRecent") or "") == "1"
            stamp = str(vdf.get_ci(fields, "Timestamp") or "0")
            rank = (most_recent, int(stamp) if stamp.isdigit() else 0)
            if best is None or rank > best[0]:
                best = (rank, Account(int(steamid), account_id))
    if best is None:
        raise InstallError("Sign in to Steam once, then run the installer again.")
    return best[1]


def read_account(env: Env) -> Account:
    path = steam_root(env) / "config" / "loginusers.vdf"
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        raise InstallError("Sign in to Steam once, then run the installer again.")
    try:
        return choose_account(text)
    except vdf.VdfError:
        raise InstallError("Steam's list of accounts couldn't be read. Open Steam, sign in, "
                           "then run the installer again.")


def preflight(env: Env, layout: Layout, log: Log) -> Tuple[str, Account]:
    if sys.version_info < MIN_PYTHON:
        raise InstallError("This installer needs Python %d.%d or later." % MIN_PYTHON)
    if not env.home.is_dir() or not os.access(str(env.home), os.W_OK):
        raise InstallError("The home folder %s isn't writable, so WoT Reborn can't be "
                           "installed." % env.home)
    if not steam_root(env).is_dir():
        raise InstallError("Steam isn't set up for this user (%s is missing). Open Steam "
                           "once, then run the installer again." % steam_root(env))
    try:
        layout.work.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise InstallError("Couldn't create %s (%s)." % (layout.work, exc.strerror or exc))
    if not env.lock(layout.work / "installer.lock"):
        raise InstallError("The WoT Reborn installer is already running. Wait for it to "
                           "finish, then run it again if you need to.")
    log.attach(layout.log)
    log.write("installer started; target=%s" % layout.target)
    kind = target_kind(layout.target)
    if kind == "install":
        recover_interrupted_swap(layout.target, log)
    process = running_game(env.processes(), layout.target.as_posix(), env.pid)
    if process is not None:
        log.write("refused: game process %d is running from the target" % process.pid)
        raise InstallError("WoT Reborn is running from %s.\n\nQuit the game, then run the "
                           "installer again." % layout.target)
    check_free_space(env, layout.target)
    account = read_account(env)
    log.write("steam account folder: %s" % (steam_root(env) / "userdata" / str(account.account_id)))
    return kind, account


# --- Release, download --------------------------------------------------------------

_DIGEST_RE = re.compile(r"sha256:([0-9a-fA-F]{64})")


def parse_digest(value: object) -> str:
    """GitHub's ``sha256:<64 hex>`` asset digest as lowercase hex; anything else fails closed."""
    if isinstance(value, str):
        match = _DIGEST_RE.fullmatch(value.strip())
        if match:
            return match.group(1).lower()
    raise InstallError("The latest release has no valid checksum for its download, so it "
                       "can't be checked. Nothing was installed. Try again later.")


def parse_release(document: object) -> Release:
    if not isinstance(document, dict):
        raise InstallError("GitHub returned an unexpected answer. Try again later.")
    assets = document.get("assets")
    asset = None
    if isinstance(assets, list):
        for candidate in assets:
            if isinstance(candidate, dict) and candidate.get("name") == PLAYER_ZIP_ASSET:
                asset = candidate
                break
    if asset is None:
        raise InstallError("The latest WoT Reborn release has no Windows download yet. Try "
                           "again later.")
    url = asset.get("browser_download_url")
    size = asset.get("size")
    if (not isinstance(url, str) or urllib.parse.urlsplit(url).scheme != "https"
            or not isinstance(size, int) or isinstance(size, bool) or size <= 0):
        raise InstallError("GitHub returned an unexpected answer. Try again later.")
    digest = parse_digest(asset.get("digest"))
    tag = document.get("tag_name")
    return Release(tag if isinstance(tag, str) else "", url, size, digest)


def resolve_release(env: Env, log: Log) -> Release:
    request = urllib.request.Request(RELEASE_API_URL, headers={
        "Accept": "application/vnd.github+json", "User-Agent": USER_AGENT})
    try:
        response = env.urlopen(request, 30)
        with contextlib.closing(response):
            raw = response.read(MAX_API_BYTES + 1)
    except urllib.error.HTTPError as exc:
        log.write("release lookup failed: HTTP %s" % exc.code)
        if exc.code in (403, 429):
            raise InstallError("GitHub is limiting requests from this network right now. Try "
                               "again in an hour.")
        raise InstallError("Couldn't find the latest WoT Reborn release (GitHub answered %s). "
                           "Try again later." % exc.code)
    except (OSError, http.client.HTTPException) as exc:
        log.write("release lookup failed: %r" % exc)
        raise InstallError("Couldn't reach GitHub. Check the internet connection, then run the "
                           "installer again.")
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise InstallError("GitHub returned an unexpected answer. Try again later.")
    release = parse_release(document)
    log.write("latest release %s: %s (%d bytes, sha256:%s)" % (
        release.tag or "?", release.url, release.size, release.sha256))
    return release


_DAMAGED = "The download was damaged. Run the installer again."
_STOPPED = ("The download stopped. Check the internet connection, then run the installer "
            "again; it continues where it stopped.")


def download(env: Env, layout: Layout, release: Release, ui: UI, log: Log) -> Path:
    expected = "sha256:" + release.sha256
    offset = 0
    if layout.part.exists():
        try:
            recorded = layout.sidecar.read_text(encoding="utf-8").strip()
        except OSError:
            recorded = ""
        size = layout.part.stat().st_size
        if recorded == expected and size <= release.size:
            offset = size
            log.write("resuming the download at %d bytes" % offset)
        else:
            layout.part.unlink()
            log.write("discarded a partial download for another release")
    if not layout.part.exists():
        offset = 0
    _write_atomic(layout.sidecar, (expected + "\n").encode("ascii"))

    ui.stage("Downloading WoT Reborn...")
    attempts = 0
    while offset < release.size:
        if attempts >= DOWNLOAD_ATTEMPTS:
            raise InstallError(_STOPPED)
        attempts += 1
        try:
            offset = _fetch(env, layout, release, offset, ui)
        except (OSError, http.client.HTTPException) as exc:
            log.write("download attempt failed at %d bytes: %r" % (offset, exc))
            offset = layout.part.stat().st_size if layout.part.exists() else 0
        if offset < release.size:
            env.sleep(min(30.0, 2.0 * (2 ** (attempts - 1))))

    ui.stage("Checking the download...")
    actual = _sha256_file(layout.part, lambda done: ui.fraction(done / release.size))
    if layout.part.stat().st_size != release.size or actual != release.sha256:
        log.write("download damaged: sha256 %s" % actual)
        with contextlib.suppress(OSError):
            layout.part.unlink()
        with contextlib.suppress(OSError):
            layout.sidecar.unlink()
        raise InstallError(_DAMAGED)
    _replace(layout.part, layout.zip)
    with contextlib.suppress(OSError):
        layout.sidecar.unlink()
    log.write("download verified")
    return layout.zip


def _fetch(env: Env, layout: Layout, release: Release, offset: int, ui: UI) -> int:
    """One request. Returns the new size of the part file."""
    request = urllib.request.Request(release.url, headers={
        "User-Agent": USER_AGENT, "Accept": "application/octet-stream"})
    if offset:
        request.add_header("Range", "bytes=%d-" % offset)
    try:
        response = env.urlopen(request, 60)
    except urllib.error.HTTPError as exc:
        if exc.code == 416 and offset:
            layout.part.unlink()  # the server can't continue this file: start over
            return 0
        raise
    with contextlib.closing(response):
        status = getattr(response, "status", None) or response.getcode()
        mode = "wb"
        if offset and status == 206:
            content_range = response.headers.get("Content-Range", "") or ""
            if not content_range.startswith("bytes %d-" % offset):
                raise OSError("unexpected Content-Range %r" % content_range)
            mode = "ab"
        elif status == 200:
            offset = 0
        else:
            raise OSError("unexpected HTTP status %s" % status)
        with open(str(layout.part), mode) as handle:
            while True:
                block = response.read(CHUNK)
                if not block:
                    break
                if offset + len(block) > release.size:
                    handle.close()
                    layout.part.unlink()
                    raise InstallError(_DAMAGED)
                handle.write(block)
                offset += len(block)
                ui.fraction(offset / release.size, "%s of %s" % (_mb(offset), _mb(release.size)))
    return offset


# --- Extract, install ----------------------------------------------------------------


def zip_entry_problem(info: zipfile.ZipInfo) -> Optional[str]:
    """Why an entry may not be extracted, or None when it is fine."""
    name = info.filename
    if not name or "\0" in name or "\\" in name:
        return "bad name"
    if name.startswith("/") or re.match(r"[A-Za-z]:", name):
        return "absolute path"
    mode = (info.external_attr >> 16) & 0xFFFF
    kind = stat.S_IFMT(mode)
    if kind == stat.S_IFLNK:
        return "symlink"
    if kind not in (0, stat.S_IFREG, stat.S_IFDIR):
        return "special file"
    parts = name[:-1].split("/") if name.endswith("/") else name.split("/")
    if any(part == ".." for part in parts):
        return "parent reference"
    if any(part in ("", ".") for part in parts):
        return "bad name"
    if parts[0] != TOP_FOLDER:
        return "outside %s/" % TOP_FOLDER
    if len(parts) == 1 and not info.is_dir():
        return "outside %s/" % TOP_FOLDER
    return None


def extract(zip_path: Path, stage: Path, ui: UI, log: Log) -> Path:
    if os.path.lexists(str(stage)):
        _remove_path(stage)
    stage.mkdir(parents=True)
    ui.stage("Unpacking WoT Reborn...")
    try:
        with zipfile.ZipFile(str(zip_path)) as archive:
            infos = archive.infolist()
            for info in infos:
                problem = zip_entry_problem(info)
                if problem:
                    log.write("rejected zip entry %r: %s" % (info.filename, problem))
                    raise InstallError(
                        "The download contains an unexpected file (%s), so nothing was "
                        "installed. Please report this." % info.filename)
            total = sum(info.file_size for info in infos) or 1
            done = 0
            for info in infos:
                parts = info.filename.rstrip("/").split("/")
                dest = stage.joinpath(*parts)
                if info.is_dir():
                    dest.mkdir(parents=True, exist_ok=True)
                    continue
                dest.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as source, open(str(dest), "xb") as sink:
                    shutil.copyfileobj(source, sink, CHUNK)
                done += info.file_size
                ui.fraction(done / total)
    except (zipfile.BadZipFile, zipfile.LargeZipFile, FileExistsError, EOFError) as exc:
        log.write("bad zip: %r" % exc)
        with contextlib.suppress(OSError):
            zip_path.unlink()
        raise InstallError(_DAMAGED)
    except OSError as exc:
        log.write("unpacking failed: %r" % exc)
        raise InstallError("Couldn't unpack WoT Reborn into %s (%s). Check the free space, then "
                           "run the installer again." % (stage, exc.strerror or exc))
    root = stage / TOP_FOLDER
    if not (root / LAUNCHER_EXE).is_file() or not (root / GAME_DIR).is_dir():
        raise InstallError("The download doesn't contain the game, so nothing was installed. "
                           "Please report this.")
    return root


def read_release_version(install_root: Path) -> Optional[str]:
    path = install_root.joinpath(*RELEASE_VERSION_PARTS)
    try:
        if path.stat().st_size > 4096:
            return None
        document = json.loads(path.read_bytes().decode("utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(document, dict) or document.get("format") != RELEASE_VERSION_FORMAT:
        return None
    version = document.get("human_version")
    return version if isinstance(version, str) and version else None


_VERSION_RE = re.compile(r"v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?")


def _version_key(text: Optional[str]):
    if not text:
        return None
    match = _VERSION_RE.fullmatch(text.strip())
    if not match:
        return None
    core = tuple(int(part) for part in match.group(1, 2, 3))
    pre = match.group(4)
    if pre is None:
        return core, (1,)
    identifiers = []
    for item in pre.split("."):
        identifiers.append((0, int(item), "") if item.isdigit() else (1, 0, item))
    return core, (0, tuple(identifiers))


def compare_versions(a: Optional[str], b: Optional[str]) -> Optional[int]:
    """Semantic-version order of a and b (-1, 0, 1), or None if either doesn't parse."""
    ka, kb = _version_key(a), _version_key(b)
    if ka is None or kb is None:
        return None
    return (ka > kb) - (ka < kb)


def installed_is_newer(installed: Optional[str], candidate: Optional[str]) -> bool:
    return compare_versions(installed, candidate) == 1


def retire_update_workspace(target: Path, log: Log) -> None:
    """Remove ``.wot-update/`` (rename first, so a partial delete can never look like one)."""
    workspace = target / UPDATE_WORKSPACE
    if not os.path.lexists(str(workspace)):
        return
    discarded = target / UPDATE_WORKSPACE_DISCARDED
    if os.path.lexists(str(discarded)):
        _remove_path(discarded)
    _rename(workspace, discarded)
    log.write("removed the updater's staging folder %s" % workspace)
    try:
        _remove_path(discarded)
    except OSError as exc:
        log.write("could not delete %s yet: %r" % (discarded, exc))


def install_tree(staged_root: Path, target: Path, log: Log) -> str:
    """Fresh install, or repair-swap Game/ and the launcher. Returns 'fresh' or 'repaired'."""
    if target_kind(target) == "fresh":
        target.parent.mkdir(parents=True, exist_ok=True)
        _rename(staged_root, target)
        log.write("fresh install into %s" % target)
        return "fresh"
    game, old = target / GAME_DIR, target / GAME_OLD_DIR
    if os.path.lexists(str(old)):
        _remove_path(old)
    moved_old = moved_new = False
    try:
        if os.path.lexists(str(game)):
            _rename(game, old)
            moved_old = True
        _rename(staged_root / GAME_DIR, game)
        moved_new = True
        retire_update_workspace(target, log)
        _replace(staged_root / LAUNCHER_EXE, target / LAUNCHER_EXE)
    except OSError as exc:
        log.write("swap failed: %r; rolling back" % exc)
        try:
            if moved_new:
                _rename(game, staged_root / GAME_DIR)
            if moved_old:
                _rename(old, game)
        except OSError as rollback:
            log.write("rollback failed: %r" % rollback)
            raise InstallError(
                "Updating the game in %s failed, and the previous files couldn't be put back. "
                "Run the installer again; it finishes the repair." % target)
        raise InstallError("Couldn't update the game in %s, so the previous version was kept "
                           "(%s). Run the installer again." % (target, exc.strerror or exc))
    try:
        _remove_path(old)
    except OSError as exc:
        log.write("could not delete %s yet (the next run does): %r" % (old, exc))
    log.write("repaired %s (Data/ and other files kept)" % target)
    return "repaired"


def clean_work(layout: Layout) -> None:
    for path in (layout.stage, layout.zip, layout.part, layout.sidecar):
        with contextlib.suppress(OSError):
            if os.path.lexists(str(path)):
                _remove_path(path)


# --- Original installer -------------------------------------------------------------


def installer_search_dirs(env: Env, override: Optional[Path]) -> List[Tuple[Path, int]]:
    """(folder, extra depth) pairs. An override replaces every default location."""
    if override is not None:
        return [(override, 0)]
    dirs = [(env.home / "Downloads", 0), (env.home / "Desktop", 0)]
    if env.user:
        media = env.system_path("/run/media") / env.user
        for entry in _sorted_entries(media):
            if entry.is_dir(follow_symlinks=False):
                dirs.append((Path(entry.path), 1))  # the top two levels of each medium
    return dirs


def _sorted_entries(folder: Path) -> List[os.DirEntry]:
    try:
        with _scandir(str(folder)) as iterator:
            return sorted(iterator, key=lambda entry: entry.name)
    except OSError:
        return []


def _is_installer_name(name: str) -> bool:
    folded = name.casefold()
    return folded.startswith(INSTALLER_NAME_PREFIX) and folded.endswith(".exe")


def find_installer_candidates(env: Env, override: Optional[Path]) -> Iterator[Tuple[Path, int]]:
    """Regular files named like the installer, in search order, with their sizes."""
    def scan(folder: Path, depth: int) -> Iterator[Tuple[Path, int]]:
        entries = _sorted_entries(folder)
        entries.sort(key=lambda entry: entry.name != env.pin.name)  # exact name first
        for entry in entries:
            try:
                if entry.is_file(follow_symlinks=False) and _is_installer_name(entry.name):
                    yield Path(entry.path), entry.stat(follow_symlinks=False).st_size
            except OSError:
                continue
        if depth > 0:
            for entry in entries:
                if entry.is_dir(follow_symlinks=False):
                    yield from scan(Path(entry.path), depth - 1)

    for folder, depth in installer_search_dirs(env, override):
        yield from scan(folder, depth)


def _verified(path: Path, pin: InstallerPin, ui: UI) -> bool:
    try:
        info = os.lstat(str(path))
        if not stat.S_ISREG(info.st_mode) or info.st_size != pin.size:
            return False
        ui.stage("Checking %s..." % path.name)
        return _sha256_file(path, lambda done: ui.fraction(done / max(1, pin.size))) == pin.sha256
    except OSError:
        return False


def place_original_installer(env: Env, target: Path, override: Optional[Path], ui: UI,
                             log: Log) -> InstallerOutcome:
    pin = env.pin
    dest = target / pin.name
    outcome = InstallerOutcome("not-found", dest)
    ui.stage("Looking for the original game installer...")
    if os.path.lexists(str(dest)):
        if _verified(dest, pin, ui):
            outcome.status = "already"
            log.write("original installer already beside the game: %s" % dest)
        else:
            outcome.status = "blocked"
            log.write("a different file already uses the installer's name: %s" % dest)
        return outcome
    match: Optional[Path] = None
    for path, size in find_installer_candidates(env, override):
        if size == pin.size and _verified(path, pin, ui):
            match = path
            break
        outcome.rejected.append(path)
        log.write("not the original installer (left alone): %s" % path)
    if match is None:
        log.write("original installer not found")
        return outcome
    outcome.source = match
    try:
        _move_noreplace(match, dest)
        outcome.status = "moved"
        log.write("moved the original installer %s -> %s" % (match, dest))
        return outcome
    except FileExistsError:
        outcome.status = "blocked"
        return outcome
    except OSError as exc:
        log.write("rename not possible (%r); copying instead" % exc)
    return _copy_installer(match, dest, pin, ui, log, outcome)


def _copy_installer(source: Path, dest: Path, pin: InstallerPin, ui: UI, log: Log,
                    outcome: InstallerOutcome) -> InstallerOutcome:
    temp = dest.with_name(".%s.%s.part" % (pin.name, secrets.token_hex(4)))
    ui.stage("Copying the original game installer...")
    try:
        return _copy_installer_to(source, dest, temp, pin, ui, log, outcome)
    finally:
        # Also on Cancelled: never leave a hidden ~1 GB partial copy behind.
        with contextlib.suppress(OSError):
            if os.path.lexists(str(temp)):
                temp.unlink()


def _copy_installer_to(source: Path, dest: Path, temp: Path, pin: InstallerPin, ui: UI,
                       log: Log, outcome: InstallerOutcome) -> InstallerOutcome:
    try:
        digest = hashlib.sha256()
        copied = 0
        with open(str(source), "rb") as reader, open(str(temp), "xb") as writer:
            while True:
                block = reader.read(CHUNK)
                if not block:
                    break
                writer.write(block)
                digest.update(block)
                copied += len(block)
                ui.fraction(copied / max(1, pin.size))
            writer.flush()
            os.fsync(writer.fileno())
        if copied != pin.size or digest.hexdigest() != pin.sha256:
            raise OSError("the copy didn't match the original installer")
        _move_noreplace(temp, dest)
    except FileExistsError:
        outcome.status = "blocked"
    except OSError as exc:
        outcome.status = "copy-failed"
        outcome.detail = str(exc.strerror or exc)
        log.write("copying the original installer failed: %r" % exc)
    else:
        outcome.status = "copied"
        log.write("copied the original installer %s -> %s" % (source, dest))
    return outcome


# --- Steam shortcut -------------------------------------------------------------------


def shortcut_appid(exe_value: str, name: str) -> int:
    """The standard non-Steam shortcut id: crc32(Exe as stored + AppName) | 0x80000000."""
    return (zlib.crc32((exe_value + name).encode("utf-8")) & 0xFFFFFFFF) | 0x80000000


def _normalise_exe(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        value = value[1:-1]
    return os.path.normpath(value) if value else ""


def exe_matches(value: Optional[str], exe: Path) -> bool:
    if not value:
        return False
    normal = _normalise_exe(value)
    if not normal:
        return False
    if normal == os.path.normpath(str(exe)):
        return True
    try:
        return os.path.realpath(normal) == os.path.realpath(str(exe))
    except (OSError, ValueError):
        return False


def find_shortcut(document: vdf.Document, exe: Path) -> Optional[int]:
    shortcuts = vdf.find(document.nodes, "shortcuts")
    if shortcuts is None or shortcuts.type != vdf.TYPE_MAP:
        return None
    for entry in shortcuts.value:
        if entry.type != vdf.TYPE_MAP:
            continue
        exe_node = entry.child("Exe")
        exe_value = exe_node.text() if exe_node is not None else None
        if not exe_matches(exe_value, exe):
            continue
        appid_node = entry.child("appid")
        appid = appid_node.uint32() if appid_node is not None else None
        if appid is None:
            name_node = entry.child("AppName")
            appid = shortcut_appid(exe_value or "", (name_node.text() if name_node else "") or "")
        return appid
    return None


def read_shortcuts(path: Path) -> Optional[vdf.Document]:
    """None when the file doesn't exist; VdfError when it can't be parsed."""
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return None
    return vdf.parse_binary(data)


def steam_running(env: Env) -> bool:
    return any(process.comm == "steam" for process in env.processes() if process.pid != env.pid)


def request_add(env: Env, exe: Path, log: Log) -> Optional[str]:
    """Ask the running Steam to add exe. Returns the route used, or None."""
    wrapper = env.system_path(STEAMOS_ADD_TO_STEAM)
    if wrapper.is_file():
        env.spawn([STEAMOS_ADD_TO_STEAM, str(exe)])
        log.write("asked Steam to add the shortcut via %s" % STEAMOS_ADD_TO_STEAM)
        return "steamos-add-to-steam"
    steam = env.which("steam")
    if not steam:
        log.write("no steamos-add-to-steam and no steam command")
        return None
    sentinel = env.system_path(ADD_SENTINEL)
    try:
        sentinel.touch()  # Steam ignores addnonsteamgame unless this file exists
    except OSError as exc:
        log.write("could not create %s: %r" % (sentinel, exc))
        return None
    url = "steam://addnonsteamgame/" + urllib.parse.quote(str(exe), safe="")
    env.spawn([steam, url])
    log.write("asked Steam to add the shortcut via the addnonsteamgame URL")
    return "steam-addnonsteamgame-url"


def poll_for_shortcut(env: Env, path: Path, exe: Path, seconds: float) -> Optional[int]:
    deadline = env.clock() + seconds
    while True:
        env.sleep(POLL_INTERVAL)
        try:
            document = read_shortcuts(path)
        except (vdf.VdfError, OSError):
            document = None  # Steam may be writing it right now
        if document is not None:
            appid = find_shortcut(document, exe)
            if appid is not None:
                return appid
        if env.clock() >= deadline:
            return None


def shutdown_steam(env: Env, log: Log) -> None:
    steam = env.which("steam") or "steam"
    try:
        env.run([steam, "-shutdown"], 30)
    except OSError as exc:
        log.write("steam -shutdown failed to start: %r" % exc)
    deadline = env.clock() + STEAM_SHUTDOWN_SECONDS
    while steam_running(env):
        if env.clock() >= deadline:
            raise InstallError("Steam didn't close. Exit Steam yourself (Steam menu > Exit), then "
                               "run the installer again.")
        env.sleep(POLL_INTERVAL)
    log.write("Steam closed")


def append_shortcut(env: Env, path: Path, exe: Path, start_dir: Path, log: Log) -> int:
    exe_value = '"%s"' % exe
    appid = shortcut_appid(exe_value, SHORTCUT_NAME)
    unreadable = InstallError(
        "Steam's shortcut list couldn't be read, so it was left alone. Add WoT Reborn yourself: "
        "in Dolphin, right-click %s and choose Add to Steam." % exe)
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        data = None
    if data is not None:
        try:
            document = vdf.parse_binary(data)
        except vdf.VdfError as exc:
            log.write("shortcuts.vdf unreadable: %s" % exc)
            raise unreadable
        stamp = env.now().strftime("%Y%m%dT%H%M%SZ")
        backup = path.with_name(path.name + ".wot-backup-" + stamp)
        _write_atomic(backup, data)
        log.write("backed up %s to %s" % (path, backup))
    else:
        document = vdf.Document([vdf.map_node("shortcuts", [])])
        path.parent.mkdir(parents=True, exist_ok=True)
    shortcuts = vdf.find(document.nodes, "shortcuts")
    if shortcuts is None or shortcuts.type != vdf.TYPE_MAP:
        raise unreadable
    used = [int(node.name) for node in shortcuts.value if node.name.isdigit()]
    index = max(used) + 1 if used else 0
    entry = vdf.map_node(str(index), [
        vdf.int32_node("appid", appid),
        vdf.string_node("AppName", SHORTCUT_NAME),
        vdf.string_node("Exe", exe_value),
        vdf.string_node("StartDir", '"%s/"' % str(start_dir).rstrip("/")),
        vdf.string_node("icon", ""),
        vdf.string_node("ShortcutPath", ""),
        vdf.string_node("LaunchOptions", ""),
        vdf.int32_node("IsHidden", 0),
        vdf.int32_node("AllowDesktopConfig", 1),
        vdf.int32_node("AllowOverlay", 1),
        vdf.int32_node("OpenVR", 0),
        vdf.int32_node("Devkit", 0),
        vdf.string_node("DevkitGameID", ""),
        vdf.int32_node("DevkitOverrideAppID", 0),
        vdf.int32_node("LastPlayTime", 0),
        vdf.string_node("FlatpakAppID", ""),
        vdf.map_node("tags", []),
    ])
    shortcuts.value.append(entry)
    _write_atomic(path, vdf.dump_binary(document))
    log.write("wrote shortcut %d (appid %d) into %s" % (index, appid, path))
    return appid


_CLOSE_STEAM_QUESTION = (
    "WoT Reborn is installed, but Steam didn't add it to your Library by itself.\n\n"
    "To add it, the installer needs to close Steam. Close Steam now?")


def ensure_shortcut(env: Env, ui: UI, account: Account, exe: Path, start_dir: Path,
                    log: Log) -> ShortcutOutcome:
    config = steam_root(env) / "userdata" / str(account.account_id) / "config"
    path = config / "shortcuts.vdf"
    ui.stage("Adding WoT Reborn to Steam...")
    try:
        existing = read_shortcuts(path)
    except vdf.VdfError:
        existing = None
    if existing is not None:
        appid = find_shortcut(existing, exe)
        if appid is not None:
            log.write("shortcut route: existing entry reused (appid %d)" % appid)
            return ShortcutOutcome(appid, "existing")

    route = None
    running = steam_running(env)
    if running:
        route = request_add(env, exe, log)
        if route:
            appid = poll_for_shortcut(env, path, exe, ADD_POLL_SECONDS)
            if appid is not None:
                log.write("shortcut route: %s (appid %d)" % (route, appid))
                return ShortcutOutcome(appid, route)
            log.write("no shortcut appeared within %d s" % ADD_POLL_SECONDS)
    else:
        log.write("Steam isn't running")

    closed = False
    if running:
        if not ui.question(_CLOSE_STEAM_QUESTION, "Close Steam", "Not now"):
            log.write("player declined closing Steam")
            raise InstallError(
                "WoT Reborn is installed in %s, but it isn't in your Steam Library yet.\n\nRun the "
                "installer again and let it close Steam, or add the game yourself: in Dolphin, "
                "right-click %s and choose Add to Steam." % (start_dir, exe.name))
        shutdown_steam(env, log)
        closed = True
        try:
            saved = read_shortcuts(path)
        except vdf.VdfError:
            saved = None
        if saved is not None:
            appid = find_shortcut(saved, exe)
            if appid is not None:
                saved_route = "%s-saved-on-exit" % (route or "steam")
                log.write("shortcut route: %s (appid %d)" % (saved_route, appid))
                return ShortcutOutcome(appid, saved_route, steam_closed=True)
    appid = append_shortcut(env, path, exe, start_dir, log)
    log.write("shortcut route: shortcuts.vdf fallback (appid %d)" % appid)
    return ShortcutOutcome(appid, "shortcuts-vdf-fallback", steam_closed=closed)


def place_artwork(env: Env, account: Account, appid: int, art_dir: Path, log: Log,
                  only_missing: bool = False) -> bool:
    grid = steam_root(env) / "userdata" / str(account.account_id) / "config" / "grid"
    sources = [(art_dir / source, pattern) for source, pattern in ART_FILES]
    missing = [str(source) for source, _ in sources if not source.is_file()]
    if missing:
        log.write("artwork missing: %s" % ", ".join(missing))
        return False
    try:
        grid.mkdir(parents=True, exist_ok=True)
        for source, pattern in sources:
            dest = grid / pattern.format(appid=appid)
            if only_missing and dest.exists():
                continue
            _write_atomic(dest, source.read_bytes())
    except OSError as exc:
        log.write("could not write artwork into %s: %r" % (grid, exc))
        return False
    log.write("artwork written into %s for appid %d" % (grid, appid))
    return True


# --- Run ---------------------------------------------------------------------------


def _installer_summary(outcome: InstallerOutcome) -> List[str]:
    lines = []
    if outcome.status == "already":
        lines.append("The original game installer is already beside the game.")
    elif outcome.status in ("moved", "copied"):
        verb = "Moved" if outcome.status == "moved" else "Copied"
        lines.append("%s the original game installer from %s to %s." % (
            verb, outcome.source.parent if outcome.source else "?", outcome.dest))
    elif outcome.status == "blocked":
        lines.append("A file named %s is already beside the game but isn't the original game "
                     "installer. It was left alone; remove it so the game can find the right "
                     "one." % outcome.dest.name)
    elif outcome.status == "copy-failed":
        lines.append("The original game installer was found at %s but couldn't be copied "
                     "beside the game (%s). Put it in Downloads yourself." % (
                         outcome.source, outcome.detail))
    else:
        lines.append("The original game installer wasn't found on this device. On first launch "
                     "the game copies it from a PC or Mac running WoT Reborn on the same "
                     "network, or finds it later in Downloads.")
    for path in outcome.rejected:
        lines.append("Not used (not the original game installer): %s" % path)
    return lines


def run(env: Env, ui: UI, options: Options, log: Log) -> str:
    target = options.target or (env.home / "Games" / INSTALL_DIR_NAME)
    layout = Layout(target)
    ui.start()
    ui.stage("Getting ready...")
    kind, account = preflight(env, layout, log)

    ui.stage("Looking up the latest release...")
    release = resolve_release(env, log)
    tag_version = release.tag[1:] if release.tag.startswith("v") else release.tag
    installed_version = read_release_version(target) if kind == "install" else None
    version_text = tag_version or "?"
    skipped_for = None
    if installed_is_newer(installed_version, tag_version):
        skipped_for = installed_version
    else:
        zip_path = download(env, layout, release, ui, log)
        staged = extract(zip_path, layout.stage, ui, log)
        downloaded_version = read_release_version(staged) or tag_version
        version_text = downloaded_version or "?"
        if kind == "install" and installed_is_newer(installed_version, downloaded_version):
            skipped_for = installed_version
        else:
            ui.stage("Installing WoT Reborn...")
            kind = install_tree(staged, target, log)
        clean_work(layout)
    if skipped_for:
        version_text = skipped_for
        log.write("kept the newer installed version %s (latest release %s)" % (
            skipped_for, tag_version or "?"))

    installer = place_original_installer(env, target, options.installer_search_dir, ui, log)
    shortcut = ensure_shortcut(env, ui, account, layout.exe, target, log)
    art = place_artwork(env, account, shortcut.appid, ART_DIR, log,
                        only_missing=(shortcut.route == "existing"))
    log.write("done")

    lines = []
    if skipped_for:
        lines.append("WoT Reborn %s is already installed in %s, which is newer than the latest "
                     "release, so the game files were kept." % (skipped_for, target))
    elif kind == "fresh":
        lines.append("WoT Reborn %s is installed in %s." % (version_text, target))
    else:
        lines.append("WoT Reborn %s is repaired in %s. Your settings and data were kept." % (
            version_text, target))
    lines.extend(_installer_summary(installer))
    if shortcut.route == "existing":
        lines.append("WoT Reborn was already in your Steam Library.")
    else:
        lines.append("Added WoT Reborn to your Steam Library.")
    if not art:
        lines.append("The Steam artwork couldn't be added (details in the log).")
    if shortcut.steam_closed:
        lines.append("Steam was closed to add the game.")
    lines.append("")
    lines.append("Next: Return to Gaming Mode, then open WoT Reborn from Library > Non-Steam. "
                 "Steam shows the new artwork after it restarts.")
    lines.append("")
    lines.append("A log is kept at %s." % layout.log)
    return "\n".join(lines)


def parse_args(argv: Optional[Sequence[str]]) -> Options:
    parser = argparse.ArgumentParser(description="Install WoT Reborn on SteamOS.")
    parser.add_argument("--target", type=Path, default=None,
                        help="install folder (test override; default ~/Games/WoT Reborn)")
    parser.add_argument("--installer-search-dir", type=Path, default=None,
                        help="the only folder searched for the original installer (test "
                             "override; replaces Downloads, Desktop and removable media)")
    parser.add_argument("--self-test", action="store_true",
                        help="print a marker and exit (checks the bootstrap)")
    args = parser.parse_args(list(argv) if argv is not None else None)

    def absolute(path: Optional[Path]) -> Optional[Path]:
        return Path(os.path.abspath(os.path.expanduser(str(path)))) if path else None

    return Options(absolute(args.target), absolute(args.installer_search_dir), args.self_test)


def self_test() -> int:
    here = Path(__file__).resolve().parent
    missing = [name for name, _ in ART_FILES if not (ART_DIR / name).is_file()]
    print("%s %s" % (SELF_TEST_MARKER, here))
    if missing:
        print("missing artwork: %s" % ", ".join(missing))
        return 1
    return 0


def main(argv: Optional[Sequence[str]] = None, env: Optional[Env] = None,
         ui: Optional[UI] = None) -> int:
    options = parse_args(argv)
    if options.self_test:
        return self_test()
    env = env or Env(Path.home())
    ui = ui or choose_ui(env)
    log = Log(env.now)

    def where() -> str:
        return "\n\nA log is kept at %s." % log.path if log.path else ""

    try:
        summary = run(env, ui, options, log)
    except Cancelled:
        log.write("cancelled by the player")
        ui.finish()
        ui.info("Installation cancelled. Run the installer again to continue where it stopped.")
        return 1
    except InstallError as exc:
        log.write("stopped: %s" % exc)
        ui.finish()
        ui.error(str(exc) + where())
        return 1
    except Exception:
        if log.path is None:  # failed before the install folder's log existed
            log.attach(env.home / "wot-reborn-install.log")
        log.write("unexpected error:\n" + traceback.format_exc())
        ui.finish()
        ui.error("Something unexpected went wrong while installing WoT Reborn. Run the installer "
                 "again; if it keeps failing, please report it with the log." + where())
        return 2
    ui.finish()
    ui.info(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
