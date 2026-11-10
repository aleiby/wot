"""Drive a Steam Deck over SSH to test the Windows build under Proton.

The Deck runs SteamOS with Developer Mode on and this machine's SSH key
authorized for the `deck` user. Its address comes from `deck_host` in the
gitignored Tools/local.json. See Tools/deck/README.md.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import re
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

STEAM = "$HOME/.local/share/Steam"
COMMON = STEAM + "/steamapps/common"
REPO = Path(__file__).resolve().parents[2]
SSH_TRANSPORT_FAILURE = 255
# GNU timeout only sends TERM; KILL after this grace so nothing outlives a run.
KILL_AFTER_SECONDS = 30
UNIT_NAME = re.compile(r"^[A-Za-z0-9_.@-]+$")
# Read only by Test builds (never compiled into Shipping): with it set, the
# game's LAN installer receiver never starts, so a sharer elsewhere on the
# network (the owner's PC, say) cannot hand the installer to a cell that
# judges where the installer came from.
LAN_RECEIVER_OFF_ENV = "WOT_TEST_LAN_RECEIVER_OFF"


def _receiver_off_setenv(lan_receiver_off: bool) -> str:
    return f"--setenv={LAN_RECEIVER_OFF_ENV}=1 " if lan_receiver_off else ""


class DeckError(RuntimeError):
    pass


class DeckTimeout(DeckError):
    def __init__(self, message: str, events: List[dict]):
        super().__init__(message)
        self.events = events


@dataclass(frozen=True)
class DeckTarget:
    host: str
    user: str = "deck"


@dataclass(frozen=True)
class PipeResult:
    """Both ends of a `producer | consumer` pipeline."""
    producer_returncode: int
    consumer_returncode: int
    producer_stderr: bytes
    consumer_stdout: bytes
    consumer_stderr: bytes


class SubprocessRunner:
    def run(self, argv, input_bytes=None, timeout=120):
        return subprocess.run(argv, input=input_bytes, capture_output=True,
                              timeout=timeout, check=False)

    def pipe(self, producer, consumer, timeout=None) -> PipeResult:
        """Stream producer's stdout into consumer's stdin; nothing is buffered here."""
        with tempfile.TemporaryFile() as producer_stderr:
            source = subprocess.Popen(producer, stdout=subprocess.PIPE,
                                      stderr=producer_stderr)
            try:
                sink = subprocess.Popen(consumer, stdin=source.stdout,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            except BaseException:
                source.kill()
                source.wait()
                raise
            finally:
                # The consumer holds its own end; closing ours lets the producer
                # see a broken pipe if the consumer exits early.
                source.stdout.close()
            try:
                out, err = sink.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                sink.kill()
                source.kill()
                sink.communicate()
                source.wait()
                raise
            source_status = source.wait()
            producer_stderr.seek(0)
            return PipeResult(source_status, sink.returncode, producer_stderr.read(),
                              out, err)


def load_target(local_json: Path) -> DeckTarget:
    data = json.loads(Path(local_json).read_text(encoding="utf-8"))
    host = data.get("deck_host")
    if not host:
        raise DeckError(f"{local_json}: set deck_host to the Deck's address")
    return DeckTarget(host, data.get("deck_user", "deck"))


def _ssh_argv(target: DeckTarget) -> List[str]:
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
            f"{target.user}@{target.host}"]


def ssh(target: DeckTarget, runner, script: str, timeout: float = 120):
    """Run a bash script on the Deck; the script travels on stdin."""
    return runner.run(_ssh_argv(target), script.encode("utf-8"), timeout)


def _require_reached(result, what: str):
    """SSH exits 255 only when it could not run the script on the Deck."""
    if result.returncode == SSH_TRANSPORT_FAILURE:
        raise DeckError(f"{what}: could not reach the Deck: "
                        + result.stderr.decode(errors="replace"))


def _proton_command(runtime: str, proton: str) -> str:
    """The Steam Linux Runtime + Proton launcher; names are shell-quoted."""
    return (f"\"{COMMON}\"/{shlex.quote(runtime)}/_v2-entry-point "
            f"--verb=waitforexitandrun -- "
            f"\"{COMMON}\"/{shlex.quote(proton)}/proton waitforexitandrun")


def local_tree_files(local_dir: Path) -> Dict[str, int]:
    """{path relative to local_dir, POSIX form: size} for every regular file."""
    files = {}
    for directory, names, filenames in os.walk(local_dir):
        names.sort()
        for name in sorted(filenames):
            path = Path(directory) / name
            if path.is_symlink() or not path.is_file():
                continue
            files[path.relative_to(local_dir).as_posix()] = path.stat().st_size
    return files


def remote_tree_files(target: DeckTarget, runner, remote_dir: str) -> Dict[str, int]:
    """{path relative to remote_dir: size} for every regular file on the Deck."""
    result = ssh(target, runner,
                 f"cd {shlex.quote(remote_dir)} && find . -type f -printf '%s %P\\0'", 600)
    if result.returncode != 0:
        raise DeckError(f"could not list {remote_dir} on the Deck: "
                        + result.stderr.decode(errors="replace"))
    files = {}
    for record in result.stdout.split(b"\0"):
        if record:
            size, _, path = record.decode("utf-8", "surrogateescape").partition(" ")
            files[path] = int(size)
    return files


def deploy_tree(target: DeckTarget, runner, local_dir: Path, remote_dir: str,
                timeout: float = 3600) -> Tuple[int, int]:
    """Stream local_dir to remote_dir as `tar -cf - . | ssh ... tar -xf -`.

    Nothing is held in memory (a 2.3 GB game tree used to vanish silently when
    the whole archive was buffered first). Either end failing raises, and the
    Deck's copy is then listed and every local file must be there with its
    size. Returns (file count, total bytes).
    """
    local_dir = Path(local_dir)
    if not local_dir.is_dir():
        raise DeckError(f"deploy: {local_dir} is not a folder")
    expected = local_tree_files(local_dir)
    if not expected:
        raise DeckError(f"deploy: {local_dir} holds no files")
    script = (f"mkdir -p {shlex.quote(remote_dir)} && "
              f"tar -C {shlex.quote(remote_dir)} -xf -")
    try:
        result = runner.pipe(["tar", "-C", str(local_dir), "-cf", "-", "."],
                             _ssh_argv(target) + [script], timeout)
    except subprocess.TimeoutExpired:
        raise DeckError(f"deploy: no finish within {timeout:.0f}s; the Deck's copy is "
                        "incomplete")
    except OSError as exc:
        raise DeckError(f"deploy: could not start tar or ssh: {exc}")
    if result.producer_returncode != 0:
        raise DeckError(f"deploy: local tar failed (exit {result.producer_returncode}): "
                        + result.producer_stderr.decode(errors="replace"))
    if result.consumer_returncode != 0:
        raise DeckError(f"deploy failed on the Deck (exit {result.consumer_returncode}): "
                        + result.consumer_stderr.decode(errors="replace"))
    deployed = remote_tree_files(target, runner, remote_dir)
    missing = [path for path in expected if path not in deployed]
    wrong = [path for path, size in expected.items()
             if path in deployed and deployed[path] != size]
    if missing or wrong:
        raise DeckError(f"deploy: the Deck's copy of {remote_dir} is incomplete: "
                        f"{len(missing)} file(s) missing, {len(wrong)} with another size "
                        f"(e.g. {(missing + wrong)[:3]})")
    return len(expected), sum(expected.values())


def launch_hidden(target: DeckTarget, runner, remote_dir: str, unit: str,
                  app_id: int, proton: str = "Proton 11.0",
                  runtime: str = "SteamLinuxRuntime_4",
                  lan_receiver_off: bool = False):
    """Start the game under Proton as a user unit that outlives the SSH session.

    It runs off-screen in Game Mode (gamescope only shows apps Steam launched);
    use show_window to bring it forward. lan_receiver_off sets
    LAN_RECEIVER_OFF_ENV for the game (Test builds honour it).
    """
    if not UNIT_NAME.match(unit):
        raise DeckError(f"unit name must match {UNIT_NAME.pattern}: {unit!r}")
    prefix = f"{STEAM}/steamapps/compatdata/{int(app_id)}"
    script = (
        f"cd {shlex.quote(remote_dir)} && "
        f"systemd-run --user --unit={unit} --collect "
        f"--setenv=STEAM_COMPAT_DATA_PATH={prefix} "
        f"--setenv=STEAM_COMPAT_CLIENT_INSTALL_PATH={STEAM} "
        f"--setenv=SteamAppId={int(app_id)} --setenv=SteamGameId={int(app_id)} "
        f"{_receiver_off_setenv(lan_receiver_off)}"
        f"--setenv=DISPLAY=:0 -p WorkingDirectory=\"$PWD\" "
        f"timeout --kill-after={KILL_AFTER_SECONDS} 3600 "
        f"{_proton_command(runtime, proton)} 'WoT Reborn.exe'"
    )
    result = ssh(target, runner, script)
    if result.returncode != 0:
        raise DeckError("launch failed: " + result.stderr.decode(errors="replace"))


def launch_game_direct(target: DeckTarget, runner, remote_dir: str, unit: str,
                       app_id: int, proton: str = "Proton 11.0",
                       runtime: str = "SteamLinuxRuntime_4",
                       lan_receiver_off: bool = False):
    """Start the game executable itself, bypassing the launcher.

    The game takes its shared session lock without the launcher handling any
    staged update first, so a later launcher run meets a running game.
    lan_receiver_off is as for launch_hidden.
    """
    if not UNIT_NAME.match(unit):
        raise DeckError(f"unit name must match {UNIT_NAME.pattern}: {unit!r}")
    prefix = f"{STEAM}/steamapps/compatdata/{int(app_id)}"
    script = (
        f"cd {shlex.quote(remote_dir)}/Game && "
        "exe=$(ls \"$PWD\"/WoT/Binaries/Win64/WoT-Win64-*.exe | head -1) && "
        "[ -n \"$exe\" ] && "
        f"systemd-run --user --unit={unit} --collect "
        f"--setenv=STEAM_COMPAT_DATA_PATH={prefix} "
        f"--setenv=STEAM_COMPAT_CLIENT_INSTALL_PATH={STEAM} "
        f"--setenv=SteamAppId={int(app_id)} --setenv=SteamGameId={int(app_id)} "
        f"{_receiver_off_setenv(lan_receiver_off)}"
        f"--setenv=DISPLAY=:0 -p WorkingDirectory=\"$PWD\" "
        f"timeout --kill-after={KILL_AFTER_SECONDS} 3600 "
        f"{_proton_command(runtime, proton)} \"$exe\""
    )
    result = ssh(target, runner, script)
    if result.returncode != 0:
        raise DeckError("direct game launch failed: "
                        + result.stderr.decode(errors="replace"))


def stop(target: DeckTarget, runner, unit: str):
    """Stop a launched unit; an already-stopped unit is not an error."""
    result = ssh(target, runner, f"systemctl --user stop {shlex.quote(unit)} || true")
    _require_reached(result, "stop")


def heartbeat_events(target: DeckTarget, runner, data_dir: str) -> List[dict]:
    script = (f"cat {shlex.quote(data_dir)}/UpdaterState/"
              f"bootstrap-heartbeat-v1*.jsonl 2>/dev/null || true")
    result = ssh(target, runner, script)
    _require_reached(result, "heartbeat")
    out = result.stdout.decode("utf-8", "replace")
    events = []
    for line in out.splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            continue
    return events


def wait_for_event(target: DeckTarget, runner, data_dir: str, event: str,
                   timeout: float, poll: float = 5.0) -> List[dict]:
    deadline = time.monotonic() + timeout
    while True:
        events = heartbeat_events(target, runner, data_dir)
        if any(e.get("event") == event for e in events):
            return events
        if time.monotonic() >= deadline:
            raise DeckTimeout(f"no {event} within {timeout:.0f}s", events)
        time.sleep(poll)


def show_window(target: DeckTarget, runner, class_name: str) -> bool:
    """Ask gamescope to show the window whose class contains class_name."""
    script = (
        "export DISPLAY=:0; "
        f"w=$(xwininfo -root -tree | grep -F {shlex.quote(class_name)} "
        "| head -1 | awk '{print $1}'); "
        "[ -n \"$w\" ] || exit 3; "
        "xprop -root -f GAMESCOPECTRL_BASELAYER_WINDOW 32c "
        "-set GAMESCOPECTRL_BASELAYER_WINDOW \"$w\""
    )
    return ssh(target, runner, script).returncode == 0


PROBE_DIR_WINDOWS = r"C:\wot-probe"


def probe_batch(args: List[str]) -> str:
    """A Windows batch file that runs the probe with args, output to out.jsonl.

    The batch exits with the probe's own status, after writing done.txt.
    """
    for arg in args:
        if '"' in arg or "\r" in arg or "\n" in arg:
            raise DeckError(f"probe argument may not contain a quote or line break: {arg!r}")
    # cmd.exe expands %NAME% even inside quotes; %% is a literal percent.
    quoted = " ".join('"' + arg.replace("%", "%%") + '"' for arg in args)
    lines = [
        "@echo off",
        rf"{PROBE_DIR_WINDOWS}\wot_path_probe.exe {quoted} > {PROBE_DIR_WINDOWS}\out.jsonl 2>&1",
        "set WOT_PROBE_STATUS=%ERRORLEVEL%",
        rf"echo done > {PROBE_DIR_WINDOWS}\done.txt",
        "exit /b %WOT_PROBE_STATUS%",
    ]
    return "".join(line + "\r\n" for line in lines)


def parse_probe_output(raw: bytes) -> List[dict]:
    """One dict per probe line; a line that is not JSON is kept as {"raw": line}."""
    rows = []
    for line in raw.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except ValueError:
            rows.append({"raw": line})
    return rows


@dataclass(frozen=True)
class ProbeResult:
    rows: List[dict]
    # The probe's own exit status: nonzero when one of its checks hit an error.
    exit_status: int


def run_probe(target: DeckTarget, runner, exe: Path, app_id: int, args: List[str],
              proton: str = "Proton 11.0",
              runtime: str = "SteamLinuxRuntime_4") -> ProbeResult:
    """Run the Windows path probe under Proton in the shortcut's prefix.

    Raises DeckError when the probe could not be copied, run or read back; a
    probe that ran and reported failing checks returns its rows and status.
    """
    batch = probe_batch(args)
    prefix = f"{STEAM}/steamapps/compatdata/{int(app_id)}"
    probe_dir = f"{prefix}/pfx/drive_c/wot-probe"
    copy = runner.run(_ssh_argv(target) + [
        f"mkdir -p {probe_dir} && cat > {probe_dir}/wot_path_probe.exe"],
        Path(exe).read_bytes(), 600)
    if copy.returncode != 0:
        raise DeckError("probe copy failed: " + copy.stderr.decode(errors="replace"))
    script = (
        "set -e\n"
        f"rm -f {probe_dir}/out.jsonl {probe_dir}/done.txt\n"
        f"cat > {probe_dir}/run.bat <<'WOT_PROBE_EOF'\n{batch}WOT_PROBE_EOF\n"
        f"STEAM_COMPAT_DATA_PATH={prefix} STEAM_COMPAT_CLIENT_INSTALL_PATH={STEAM} "
        f"SteamAppId={int(app_id)} SteamGameId={int(app_id)} "
        # Proton stalls during Wine startup without a display.
        f"DISPLAY=:0 WINEDEBUG=-all "
        f"timeout --kill-after={KILL_AFTER_SECONDS} 900 {_proton_command(runtime, proton)} "
        f"'C:\\windows\\system32\\cmd.exe' /c '{PROBE_DIR_WINDOWS}\\run.bat'"
        # Wine's background server inherits this command's stdio; left on the
        # SSH channel, it keeps the session open after the probe exits.
        f" < /dev/null > {probe_dir}/proton.log 2>&1\n"
    )
    ran = ssh(target, runner, script, timeout=960)
    _require_reached(ran, "probe run")
    fetched = ssh(target, runner, f"cat {probe_dir}/out.jsonl")
    if fetched.returncode != 0:
        raise DeckError("probe output could not be read: "
                        + fetched.stderr.decode(errors="replace"))
    return ProbeResult(parse_probe_output(fetched.stdout), ran.returncode)


def _summarize(events: List[dict]) -> str:
    seen, lines = set(), []
    for e in events:
        key = (e.get("event"), e.get("phase"), str(e.get("detail", ""))[:120])
        if key in seen:
            continue
        seen.add(key)
        lines.append(" ".join(str(part) for part in key if part))
    return "\n".join(lines)


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--local-json", type=Path, default=REPO / "Tools/local.json")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("deploy", help="copy a local game tree to the Deck")
    p.add_argument("local_dir", type=Path)
    p.add_argument("remote_dir")
    p = sub.add_parser("launch", help="start the game hidden under Proton")
    p.add_argument("remote_dir")
    p.add_argument("--unit", default="wotdeck")
    p.add_argument("--app-id", type=int, required=True)
    p.add_argument("--proton", default="Proton 11.0")
    p.add_argument("--lan-receiver-off", action="store_true",
                   help="keep a Test build's LAN installer receiver off")
    p = sub.add_parser("stop", help="stop a launched unit")
    p.add_argument("--unit", default="wotdeck")
    p = sub.add_parser("heartbeat", help="summarize the game's heartbeat")
    p.add_argument("data_dir")
    p = sub.add_parser("show", help="bring the game window forward")
    p.add_argument("--class-name", default="steam_app_")
    p = sub.add_parser("probe", help="run the Windows path probe under Proton")
    p.add_argument("exe", type=Path)
    p.add_argument("--app-id", type=int, required=True)
    p.add_argument("--proton", default="Proton 11.0")
    p.add_argument("probe_args", nargs="+")
    p = sub.add_parser("cells", help="run the first-run cells on a deployed install")
    p.add_argument("remote_dir")
    p.add_argument("--app-id", type=int, required=True)
    p.add_argument("--timeout", type=float, default=900.0)
    p = sub.add_parser("lock-cell",
                       help="a second launcher must not touch a staged update while the game runs")
    p.add_argument("remote_dir")
    p.add_argument("--app-id", type=int, required=True)
    p = sub.add_parser("update-cell",
                       help="stage a newer release zip (on the Deck) and judge the next launch")
    p.add_argument("remote_dir")
    p.add_argument("candidate_zip")
    p.add_argument("--app-id", type=int, required=True)
    p = sub.add_parser("install-cell",
                       help="run the SteamOS installer into a scratch folder on the Deck")
    p.add_argument("--case", action="append", default=[],
                   choices=("fresh", "repair", "fallback"),
                   help="repeatable; default: fresh, then repair. fallback shuts Steam "
                        "down and needs --allow-steam-shutdown")
    p.add_argument("--scratch", default="wot-install-cell",
                   help="folder under the Deck's home (name starts with wot-install-cell)")
    p.add_argument("--installer-name", default=None,
                   help="the original installer's file name in ~/Downloads "
                        "(default: its pinned name)")
    p.add_argument("--allow-steam-shutdown", action="store_true",
                   help="allow the fallback case to shut Steam down (the owner's OK)")
    p.add_argument("--keep", action="store_true",
                   help="leave the scratch install and art in place (to repair later)")
    p.add_argument("--timeout", type=float, default=1800.0)
    args = parser.parse_args(argv)

    target, runner = load_target(args.local_json), SubprocessRunner()
    if args.command == "deploy":
        count, size = deploy_tree(target, runner, args.local_dir, args.remote_dir)
        print(f"deployed {count} files ({size} bytes) to {args.remote_dir}, "
              "all present with their sizes")
    elif args.command == "launch":
        launch_hidden(target, runner, args.remote_dir, args.unit, args.app_id,
                      args.proton, lan_receiver_off=args.lan_receiver_off)
    elif args.command == "stop":
        stop(target, runner, args.unit)
    elif args.command == "heartbeat":
        print(_summarize(heartbeat_events(target, runner, args.data_dir)))
    elif args.command == "probe":
        result = run_probe(target, runner, args.exe, args.app_id, args.probe_args,
                           args.proton)
        for row in result.rows:
            print(json.dumps(row))
        return result.exit_status
    elif args.command == "cells":
        import cells
        verdicts = cells.run_first_run_cells(target, runner, args.remote_dir,
                                             args.app_id, timeout=args.timeout)
        for name, verdict in verdicts:
            print(f"{'PASS' if verdict.ok else 'FAIL'} {name}: {verdict.reason}")
        return 0 if all(v.ok for _, v in verdicts) else 1
    elif args.command in ("lock-cell", "update-cell"):
        import cells
        if args.command == "lock-cell":
            name = "launcher-skips-while-running"
            verdict = cells.run_launcher_lock_cell(target, runner, args.remote_dir,
                                                   args.app_id)
        else:
            name = "staged-update-applies"
            verdict = cells.run_staged_update_cell(target, runner, args.remote_dir,
                                                   args.app_id, args.candidate_zip)
        print(f"{'PASS' if verdict.ok else 'FAIL'} {name}: {verdict.reason}")
        return 0 if verdict.ok else 1
    elif args.command == "install-cell":
        import install_cell
        layout = install_cell.Layout(
            args.scratch, args.installer_name or install_cell.PINNED_INSTALLER_NAME)
        verdicts = install_cell.run_install_cells(
            target, runner, args.case or install_cell.DEFAULT_CASES, layout,
            allow_steam_shutdown=args.allow_steam_shutdown, keep=args.keep,
            timeout=args.timeout)
        for name, verdict in verdicts:
            print(f"{'PASS' if verdict.ok else 'FAIL'} {name}: {verdict.reason}")
        return 0 if all(v.ok for _, v in verdicts) else 1
    elif args.command == "show":
        if not show_window(target, runner, args.class_name):
            print("no matching window", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
