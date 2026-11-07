"""Drive a Steam Deck over SSH to test the Windows build under Proton.

The Deck runs SteamOS with Developer Mode on and this machine's SSH key
authorized for the `deck` user. Its address comes from `deck_host` in the
gitignored Tools/local.json. See Tools/deck/README.md.
"""
from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List

STEAM = "$HOME/.local/share/Steam"
COMMON = STEAM + "/steamapps/common"
REPO = Path(__file__).resolve().parents[2]


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


class SubprocessRunner:
    def run(self, argv, input_bytes=None, timeout=120):
        return subprocess.run(argv, input=input_bytes, capture_output=True,
                              timeout=timeout, check=False)


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


def deploy_tree(target: DeckTarget, runner, local_dir: Path, remote_dir: str):
    archive = subprocess.run(["tar", "-C", str(local_dir), "-cf", "-", "."],
                             capture_output=True, check=True).stdout
    script = (f"mkdir -p {shlex.quote(remote_dir)} && "
              f"tar -C {shlex.quote(remote_dir)} -xf -")
    result = runner.run(_ssh_argv(target) + [script], archive, 1800)
    if result.returncode != 0:
        raise DeckError("deploy failed: " + result.stderr.decode(errors="replace"))


def launch_hidden(target: DeckTarget, runner, remote_dir: str, unit: str,
                  app_id: int, proton: str = "Proton 11.0",
                  runtime: str = "SteamLinuxRuntime_4"):
    """Start the game under Proton as a user unit that outlives the SSH session.

    It runs off-screen in Game Mode (gamescope only shows apps Steam launched);
    use show_window to bring it forward.
    """
    prefix = f"{STEAM}/steamapps/compatdata/{app_id}"
    script = (
        f"cd {shlex.quote(remote_dir)} && "
        f"systemd-run --user --unit={unit} --collect "
        f"--setenv=STEAM_COMPAT_DATA_PATH={prefix} "
        f"--setenv=STEAM_COMPAT_CLIENT_INSTALL_PATH={STEAM} "
        f"--setenv=SteamAppId={app_id} --setenv=SteamGameId={app_id} "
        f"--setenv=DISPLAY=:0 -p WorkingDirectory=\"$PWD\" "
        f"timeout 3600 \"{COMMON}/{runtime}/_v2-entry-point\" "
        f"--verb=waitforexitandrun -- "
        f"\"{COMMON}/{proton}/proton\" waitforexitandrun 'WoT Reborn.exe'"
    )
    result = ssh(target, runner, script)
    if result.returncode != 0:
        raise DeckError("launch failed: " + result.stderr.decode(errors="replace"))


def stop(target: DeckTarget, runner, unit: str):
    ssh(target, runner, f"systemctl --user stop {shlex.quote(unit)} || true")


def heartbeat_events(target: DeckTarget, runner, data_dir: str) -> List[dict]:
    script = (f"cat {shlex.quote(data_dir)}/UpdaterState/"
              f"bootstrap-heartbeat-v1*.jsonl 2>/dev/null || true")
    out = ssh(target, runner, script).stdout.decode("utf-8", "replace")
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
    """A Windows batch file that runs the probe with args, output to out.jsonl."""
    for arg in args:
        if '"' in arg:
            raise DeckError(f"probe argument may not contain a quote: {arg!r}")
    quoted = " ".join(f'"{arg}"' for arg in args)
    lines = [
        "@echo off",
        rf"{PROBE_DIR_WINDOWS}\wot_path_probe.exe {quoted} > {PROBE_DIR_WINDOWS}\out.jsonl 2>&1",
        rf"echo done > {PROBE_DIR_WINDOWS}\done.txt",
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


def run_probe(target: DeckTarget, runner, exe: Path, app_id: int, args: List[str],
              proton: str = "Proton 11.0",
              runtime: str = "SteamLinuxRuntime_4") -> List[dict]:
    """Run the Windows path probe under Proton in the shortcut's prefix."""
    batch = probe_batch(args)
    prefix = f"{STEAM}/steamapps/compatdata/{app_id}"
    probe_dir = f"{prefix}/pfx/drive_c/wot-probe"
    copy = runner.run(_ssh_argv(target) + [
        f"mkdir -p {probe_dir} && cat > {probe_dir}/wot_path_probe.exe"],
        Path(exe).read_bytes(), 600)
    if copy.returncode != 0:
        raise DeckError("probe copy failed: " + copy.stderr.decode(errors="replace"))
    script = (
        f"rm -f {probe_dir}/out.jsonl {probe_dir}/done.txt && "
        f"cat > {probe_dir}/run.bat <<'WOT_PROBE_EOF'\n{batch}WOT_PROBE_EOF\n"
        f"STEAM_COMPAT_DATA_PATH={prefix} STEAM_COMPAT_CLIENT_INSTALL_PATH={STEAM} "
        f"SteamAppId={app_id} SteamGameId={app_id} DISPLAY=:0 WINEDEBUG=-all "
        f"timeout 900 \"{COMMON}/{runtime}/_v2-entry-point\" --verb=waitforexitandrun -- "
        f"\"{COMMON}/{proton}/proton\" waitforexitandrun "
        f"'C:\\windows\\system32\\cmd.exe' /c '{PROBE_DIR_WINDOWS}\\run.bat'"
        # Wine's background server inherits this command's stdio; left on the
        # SSH channel, it keeps the session open after the probe exits.
        f" < /dev/null > {probe_dir}/proton.log 2>&1\n"
    )
    ran = ssh(target, runner, script, timeout=960)
    if ran.returncode != 0:
        raise DeckError("probe run failed: " + ran.stderr.decode(errors="replace"))
    fetched = ssh(target, runner, f"cat {probe_dir}/out.jsonl")
    return parse_probe_output(fetched.stdout)


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
    args = parser.parse_args(argv)

    target, runner = load_target(args.local_json), SubprocessRunner()
    if args.command == "deploy":
        deploy_tree(target, runner, args.local_dir, args.remote_dir)
    elif args.command == "launch":
        launch_hidden(target, runner, args.remote_dir, args.unit, args.app_id,
                      args.proton)
    elif args.command == "stop":
        stop(target, runner, args.unit)
    elif args.command == "heartbeat":
        print(_summarize(heartbeat_events(target, runner, args.data_dir)))
    elif args.command == "probe":
        for row in run_probe(target, runner, args.exe, args.app_id, args.probe_args,
                             args.proton):
            print(json.dumps(row))
    elif args.command == "show":
        if not show_window(target, runner, args.class_name):
            print("no matching window", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
