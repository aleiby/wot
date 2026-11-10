"""Unit tests for the SteamOS installer program.

No network, no Steam, no Deck: every side effect goes through Env's seams
(home and filesystem root, runner, spawner, HTTP opener, clock, process list).
"""
from __future__ import annotations

import binascii
import datetime
import errno
import hashlib
import io
import json
import os
import shutil
import stat
import struct
import subprocess
import tempfile
import unittest
import urllib.error
import urllib.parse
import zipfile
import zlib
from pathlib import Path
from unittest import mock

import install_wot_reborn as iw
import vdf

DOWNLOAD_URL = "https://github.com/aleiby/wot/releases/download/v0.1.3/WoT-Reborn-Windows.zip"
FIXED_NOW = datetime.datetime(2026, 11, 9, 12, 30, 0, tzinfo=datetime.timezone.utc)
FAKE_INSTALLER = b"fake original installer bytes " * 37


def fake_pin() -> iw.InstallerPin:
    return iw.InstallerPin(iw.ORIGINAL_INSTALLER_PIN.name, len(FAKE_INSTALLER),
                           hashlib.sha256(FAKE_INSTALLER).hexdigest())


def release_version_json(version: str) -> bytes:
    return json.dumps({"format": "wot-release-version-v1", "human_version": version,
                       "version": 1}, separators=(",", ":"), sort_keys=True).encode()


def player_zip(version: str = "0.1.3", extra=None) -> bytes:
    files = {
        "WoT Reborn/": None,
        "WoT Reborn/WoT Reborn.exe": b"launcher " + version.encode(),
        "WoT Reborn/Game/": None,
        "WoT Reborn/Game/WoT/Binaries/Win64/WoT-Win64-Shipping.exe": b"game " + version.encode(),
        "WoT Reborn/Game/WoT/Content/Paks/WoT.pak": b"pak " + version.encode() * 50,
        "WoT Reborn/Game/WoT/Build/WoTRelease/release-version.json": release_version_json(version),
    }
    files.update(extra or {})
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            if data is None:
                archive.writestr(zipfile.ZipInfo(name), b"")
            else:
                archive.writestr(name, data)
    return buffer.getvalue()


def api_document(payload: bytes, tag: str = "v0.1.3", digest=None, url: str = DOWNLOAD_URL):
    asset = {"name": "WoT-Reborn-Windows.zip", "browser_download_url": url,
             "size": len(payload)}
    if digest is not False:
        asset["digest"] = digest or "sha256:" + hashlib.sha256(payload).hexdigest()
    return {"tag_name": tag, "assets": [
        {"name": "WoT-Reborn-Mac.dmg", "browser_download_url": url + ".dmg", "size": 5,
         "digest": "sha256:" + "0" * 64},
        asset]}


class FakeResponse:
    def __init__(self, status: int, body: bytes, headers=None) -> None:
        self.status = status
        self._body = io.BytesIO(body)
        self.headers = headers or {}

    def read(self, size: int = -1) -> bytes:
        return self._body.read(size)

    def getcode(self) -> int:
        return self.status

    def close(self) -> None:
        pass


class FakeHttp:
    """The GitHub API plus the asset download, with optional Range support."""

    def __init__(self, payload: bytes, document=None, honour_range: bool = True,
                 served: bytes = None) -> None:
        self.payload = payload
        self.served = payload if served is None else served
        self.document = api_document(payload) if document is None else document
        self.honour_range = honour_range
        self.requests = []

    def __call__(self, request, timeout):
        self.requests.append((request.full_url, request.get_header("Range")))
        if request.full_url == iw.RELEASE_API_URL:
            return FakeResponse(200, json.dumps(self.document).encode())
        if request.full_url == DOWNLOAD_URL:
            ranged = request.get_header("Range")
            if ranged and self.honour_range:
                start = int(ranged[len("bytes="):-1])
                if start >= len(self.served):
                    raise urllib.error.HTTPError(DOWNLOAD_URL, 416, "Range Not Satisfiable",
                                                 {}, None)
                return FakeResponse(206, self.served[start:], {
                    "Content-Range": "bytes %d-%d/%d" % (start, len(self.served) - 1,
                                                         len(self.served))})
            return FakeResponse(200, self.served)
        raise urllib.error.URLError("no network in tests")

    @property
    def download_requests(self):
        return [entry for entry in self.requests if entry[0] == DOWNLOAD_URL]


class FakeTime:
    def __init__(self) -> None:
        self.now = 0.0
        self.hooks = []

    def at(self, when: float, action) -> None:
        self.hooks.append([when, action])

    def clock(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds
        for hook in list(self.hooks):
            if hook[0] <= self.now:
                self.hooks.remove(hook)
                hook[1]()


class FakeRunner:
    def __init__(self) -> None:
        self.calls = []
        self.actions = {}

    def on(self, argument: str, action) -> None:
        self.actions[argument] = action

    def __call__(self, argv, timeout):
        self.calls.append(list(argv))
        for argument, action in self.actions.items():
            if argument in argv:
                action()
        return subprocess.CompletedProcess(list(argv), 0, b"", b"")


class FakeUI(iw.UI):
    def __init__(self, answer: bool = True) -> None:
        self.answer = answer
        self.stages, self.questions, self.infos, self.errors = [], [], [], []

    def stage(self, text: str) -> None:
        self.stages.append(text)

    def question(self, text: str, yes: str, no: str) -> bool:
        self.questions.append(text)
        return self.answer

    def info(self, text: str) -> None:
        self.infos.append(text)

    def error(self, text: str) -> None:
        self.errors.append(text)


LOGINUSERS = '''"users"
{
\t"76561197960265829"
\t{
\t\t"AccountName"\t\t"older"
\t\t"MostRecent"\t\t"0"
\t\t"Timestamp"\t\t"1800000000"
\t}
\t"76561197960266930"
\t{
\t\t"AccountName"\t\t"current"
\t\t"MostRecent"\t\t"1"
\t\t"Timestamp"\t\t"1700000000"
\t}
}
'''
ACCOUNT_ID = 76561197960266930 & 0xFFFFFFFF


class Harness:
    """A temp home + filesystem root with Steam signed in, and all seams faked."""

    def __init__(self, tmp: Path, steam_running: bool = False, wrapper: bool = True,
                 steam_binary: bool = True, answer: bool = True) -> None:
        self.home = tmp / "home"
        self.root = tmp / "root"
        self.home.mkdir()
        self.root.mkdir()
        (self.root / "tmp").mkdir()
        self.steam = self.home / ".local" / "share" / "Steam"
        (self.steam / "config").mkdir(parents=True)
        (self.steam / "config" / "loginusers.vdf").write_text(LOGINUSERS)
        self.config = self.steam / "userdata" / str(ACCOUNT_ID) / "config"
        self.config.mkdir(parents=True)
        (self.steam / "userdata" / "0").mkdir()
        if wrapper:
            (self.root / "usr" / "bin").mkdir(parents=True)
            (self.root / "usr" / "bin" / "steamos-add-to-steam").write_text("#!/bin/sh\n")
        self.time = FakeTime()
        self.runner = FakeRunner()
        self.spawned = []
        self.procs = [iw.ProcessInfo(1, "systemd", ("/sbin/init",))]
        if steam_running:
            self.procs.append(iw.ProcessInfo(4242, "steam", (
                "/home/player/.local/share/Steam/ubuntu12_32/steam", "-srt-logger-opened")))
        self.ui = FakeUI(answer)
        self.free = 100 * 1024 ** 3
        self.steam_binary = steam_binary
        self.env = iw.Env(
            home=self.home, root=self.root, run=self.runner, spawn=self.spawned.append,
            which=self.which, urlopen=self.no_network, clock=self.time.clock,
            sleep=self.time.sleep, processes=lambda: list(self.procs),
            disk_free=lambda path: self.free, now=lambda: FIXED_NOW, user="player",
            environ={}, pin=fake_pin(), pid=99999)

    def which(self, name: str):
        if name == "steam" and self.steam_binary:
            return "/usr/bin/steam"
        return None

    @staticmethod
    def no_network(request, timeout):
        raise AssertionError("unexpected network access: %s" % request.full_url)

    @property
    def target(self) -> Path:
        return self.home / "Games" / "WoT Reborn"

    @property
    def exe(self) -> Path:
        return self.target / "WoT Reborn.exe"

    @property
    def shortcuts(self) -> Path:
        return self.config / "shortcuts.vdf"

    def stop_steam(self) -> None:
        self.procs = [p for p in self.procs if p.comm != "steam"]

    def log(self) -> iw.Log:
        return iw.Log(lambda: FIXED_NOW)


def shortcuts_bytes(entries) -> bytes:
    """entries: (index, appid, exe value, extra raw children)."""
    def key(name):
        return name.encode() + b"\0"
    body = b""
    for index, appid, exe, extra in entries:
        body += (b"\x00" + key(str(index))
                 + b"\x02" + key("appid") + struct.pack("<I", appid)
                 + b"\x01" + key("AppName") + b"Other\0"
                 + b"\x01" + key("Exe") + exe.encode() + b"\0"
                 + b"\x02" + key("LastPlayTime") + struct.pack("<I", 1730000000)
                 + b"\x01" + key("FlatpakAppID") + b"\0"
                 + extra
                 + b"\x00" + key("tags") + b"\x01" + key("0") + b"favorite\0" + b"\x08"
                 + b"\x08")
    return b"\x00" + key("shortcuts") + body + b"\x08\x08"


class TempCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()


# --- Preflight ---------------------------------------------------------------


class PreflightTests(TempCase):
    def make_install(self, target: Path) -> None:
        (target / "Game" / "WoT").mkdir(parents=True)
        (target / "WoT Reborn.exe").write_bytes(b"old launcher")

    def test_fresh_install_and_refuse(self):
        target = self.tmp / "WoT Reborn"
        self.assertEqual(iw.target_kind(target), "fresh")
        target.mkdir()
        (target / "notes.txt").write_text("mine")
        with self.assertRaises(iw.InstallError) as caught:
            iw.target_kind(target)
        self.assertIn(str(target), str(caught.exception))
        self.assertIn("Rename or remove", str(caught.exception))
        (target / "WoT Reborn.exe").write_bytes(b"x")
        self.assertEqual(iw.target_kind(target), "install")
        afile = self.tmp / "plainfile"
        afile.write_text("x")
        with self.assertRaises(iw.InstallError):
            iw.target_kind(afile)

    def test_interrupted_swap_with_game_missing_restores_game_old(self):
        target = self.tmp / "WoT Reborn"
        self.make_install(target)
        os.rename(str(target / "Game"), str(target / "Game.old"))
        iw.recover_interrupted_swap(target, iw.Log(lambda: FIXED_NOW))
        self.assertTrue((target / "Game" / "WoT").is_dir())
        self.assertFalse((target / "Game.old").exists())

    def test_interrupted_swap_with_both_present_deletes_game_old(self):
        target = self.tmp / "WoT Reborn"
        self.make_install(target)
        (target / "Game.old" / "stale").mkdir(parents=True)
        (target / ".wot-update.discarded" / "x").mkdir(parents=True)
        iw.recover_interrupted_swap(target, iw.Log(lambda: FIXED_NOW))
        self.assertTrue((target / "Game" / "WoT").is_dir())
        self.assertFalse((target / "Game.old").exists())
        self.assertFalse((target / ".wot-update.discarded").exists())

    def test_running_game_is_found_in_posix_and_proton_forms(self):
        target = "/home/player/Games/WoT Reborn"
        posix = iw.ProcessInfo(10, "WoT Reborn.exe", (
            "/home/player/.local/share/Steam/steamapps/common/Proton 11.0/proton",
            "waitforexitandrun", "/home/player/Games/WoT Reborn/WoT Reborn.exe"))
        proton = iw.ProcessInfo(11, "WoT-Win64-Shipp", (
            r"Z:\home\player\Games\WoT Reborn\Game\WoT\Binaries\Win64\WoT-Win64-Shipping.exe",))
        proton_upper = iw.ProcessInfo(12, "WoT Reborn.exe", (
            r"z:\HOME\PLAYER\GAMES\WOT REBORN\WoT Reborn.exe",))
        other_folder = iw.ProcessInfo(13, "WoT Reborn.exe", (
            "/home/player/Games/WoT Reborn2/WoT Reborn.exe",))
        not_the_game = iw.ProcessInfo(14, "dolphin", ("dolphin", "/home/player/Games/WoT Reborn/"))
        own = iw.ProcessInfo(15, "python3", ("python3", "/home/player/Games/WoT Reborn/WoT Reborn.exe"))
        for process in (posix, proton, proton_upper):
            self.assertIs(iw.running_game([process], target, 15), process)
        self.assertIsNone(iw.running_game([other_folder, not_the_game, own], target, 15))

    def test_process_list_reads_proc_and_skips_zombies(self):
        proc = self.tmp / "root" / "proc"
        for pid, state, comm, cmdline in (
                (101, "S", "steam", b"/home/player/.local/share/Steam/ubuntu12_32/steam\0-silent\0"),
                (102, "Z", "steam", b""),
                (103, "R", "WoT Reborn.exe", b"Z:\\home\\player\\Games\\WoT Reborn\\WoT Reborn.exe\0")):
            folder = proc / str(pid)
            folder.mkdir(parents=True)
            (folder / "stat").write_text("%d (%s) %s 1 2 3" % (pid, comm, state))
            (folder / "comm").write_text(comm + "\n")
            (folder / "cmdline").write_bytes(cmdline)
        (proc / "self").mkdir()
        (proc / "uptime").write_text("1 2")
        env = iw.Env(home=self.tmp, root=self.tmp / "root", pid=1)
        processes = sorted(env.processes(), key=lambda process: process.pid)
        self.assertEqual([process.pid for process in processes], [101, 103])
        self.assertEqual(processes[0].argv, ("/home/player/.local/share/Steam/ubuntu12_32/steam",
                                             "-silent"))
        self.assertTrue(iw.steam_running(env))
        self.assertEqual(env.system_path("/tmp/addnonsteamgamefile"),
                         self.tmp / "root" / "tmp" / "addnonsteamgamefile")

    def test_free_space_threshold_names_the_amount(self):
        harness = Harness(self.tmp)
        harness.free = 4 * 1024 ** 3
        with self.assertRaises(iw.InstallError) as caught:
            iw.check_free_space(harness.env, harness.target)
        self.assertIn("5 GB", str(caught.exception))
        self.assertIn("4.0 GB", str(caught.exception))
        harness.free = 6 * 1024 ** 3
        iw.check_free_space(harness.env, harness.target)

    def test_non_install_folder_is_refused_before_any_download(self):
        harness = Harness(self.tmp)
        harness.target.mkdir(parents=True)
        (harness.target / "save.dat").write_text("keep")
        http = FakeHttp(player_zip())
        harness.env.urlopen = http
        self.assertEqual(iw.main([], env=harness.env, ui=harness.ui), 1)
        self.assertEqual(http.requests, [])
        self.assertIn("doesn't hold WoT Reborn", harness.ui.errors[0])
        self.assertTrue((harness.target / "save.dat").is_file())

    def test_running_game_refuses_through_main(self):
        harness = Harness(self.tmp)
        PreflightTests.make_install(self, harness.target)
        harness.procs.append(iw.ProcessInfo(77, "WoT Reborn.exe", (
            "Z:" + harness.target.as_posix().replace("/", "\\") + "\\WoT Reborn.exe",)))
        harness.env.urlopen = FakeHttp(player_zip())
        self.assertEqual(iw.main([], env=harness.env, ui=harness.ui), 1)
        self.assertIn("Quit the game", harness.ui.errors[0])

    def test_signed_out_steam_fails_plainly(self):
        harness = Harness(self.tmp)
        (harness.steam / "config" / "loginusers.vdf").unlink()
        harness.env.urlopen = FakeHttp(player_zip())
        self.assertEqual(iw.main([], env=harness.env, ui=harness.ui), 1)
        self.assertIn("Sign in to Steam once", harness.ui.errors[0])


class AccountTests(unittest.TestCase):
    def test_most_recent_wins_and_account_id_is_low_32_bits(self):
        account = iw.choose_account(LOGINUSERS)
        self.assertEqual(account.steamid64, 76561197960266930)
        self.assertEqual(account.account_id, 76561197960266930 & 0xFFFFFFFF)
        self.assertEqual(account.account_id, 1202)

    def test_userdata_zero_and_empty_lists_are_skipped(self):
        zero = '"users" { "76561197960265728" { "MostRecent" "1" } }'
        with self.assertRaises(iw.InstallError):
            iw.choose_account(zero)
        with self.assertRaises(iw.InstallError):
            iw.choose_account('"users" { }')

    def test_without_most_recent_the_newest_timestamp_wins(self):
        text = ('"users" { "76561197960265829" { "Timestamp" "5" } '
                '"76561197960266930" { "Timestamp" "9" } }')
        self.assertEqual(iw.choose_account(text).account_id, 1202)


# --- Release and download -------------------------------------------------------------


class ReleaseTests(TempCase):
    def test_digest_parsing_fails_closed(self):
        good = "ab" * 32
        self.assertEqual(iw.parse_digest("sha256:" + good), good)
        self.assertEqual(iw.parse_digest("sha256:" + good.upper()), good)
        for bad in (None, "", good, "sha512:" + good, "sha256:" + good[:-1],
                    "sha256:" + "g" * 64, "sha256:" + good + "00", 42):
            with self.assertRaises(iw.InstallError, msg=repr(bad)):
                iw.parse_digest(bad)

    def test_release_without_digest_or_asset_fails_closed(self):
        payload = b"zip"
        with self.assertRaises(iw.InstallError):
            iw.parse_release(api_document(payload, digest=False))
        with self.assertRaises(iw.InstallError):
            iw.parse_release(api_document(payload, digest="md5:abc"))
        with self.assertRaises(iw.InstallError):
            iw.parse_release(api_document(payload, url="http://github.com/x.zip"))
        with self.assertRaises(iw.InstallError):
            iw.parse_release({"tag_name": "v1", "assets": []})
        release = iw.parse_release(api_document(payload))
        self.assertEqual(release, iw.Release("v0.1.3", DOWNLOAD_URL, 3,
                                             hashlib.sha256(payload).hexdigest()))

    def test_missing_digest_downloads_nothing(self):
        harness = Harness(self.tmp)
        payload = player_zip()
        http = FakeHttp(payload, document=api_document(payload, digest=False))
        harness.env.urlopen = http
        self.assertEqual(iw.main([], env=harness.env, ui=harness.ui), 1)
        self.assertEqual(http.download_requests, [])
        self.assertIn("checksum", harness.ui.errors[0])
        self.assertFalse(harness.target.exists())

    def test_rate_limit_and_offline_messages(self):
        harness = Harness(self.tmp)
        log = harness.log()

        def limited(request, timeout):
            raise urllib.error.HTTPError(request.full_url, 403, "Forbidden", {}, None)

        harness.env.urlopen = limited
        with self.assertRaises(iw.InstallError) as caught:
            iw.resolve_release(harness.env, log)
        self.assertIn("limiting", str(caught.exception))

        def offline(request, timeout):
            raise urllib.error.URLError("no route")

        harness.env.urlopen = offline
        with self.assertRaises(iw.InstallError) as caught:
            iw.resolve_release(harness.env, log)
        self.assertIn("internet", str(caught.exception))


class DownloadTests(TempCase):
    def setUp(self) -> None:
        super().setUp()
        self.harness = Harness(self.tmp)
        self.payload = player_zip()
        self.release = iw.parse_release(api_document(self.payload))
        self.layout = iw.Layout(self.harness.target)
        self.layout.work.mkdir(parents=True)

    def download(self, http):
        self.harness.env.urlopen = http
        return iw.download(self.harness.env, self.layout, self.release, self.harness.ui,
                           self.harness.log())

    def test_fresh_download_verifies_and_renames(self):
        http = FakeHttp(self.payload)
        path = self.download(http)
        self.assertEqual(path.read_bytes(), self.payload)
        self.assertEqual(http.download_requests, [(DOWNLOAD_URL, None)])
        self.assertFalse(self.layout.part.exists())
        self.assertFalse(self.layout.sidecar.exists())

    def test_matching_sidecar_resumes_with_range(self):
        half = len(self.payload) // 2
        self.layout.part.write_bytes(self.payload[:half])
        self.layout.sidecar.write_text("sha256:%s\n" % self.release.sha256)
        http = FakeHttp(self.payload)
        self.assertEqual(self.download(http).read_bytes(), self.payload)
        self.assertEqual(http.download_requests, [(DOWNLOAD_URL, "bytes=%d-" % half)])

    def test_stale_part_for_another_digest_is_discarded(self):
        self.layout.part.write_bytes(b"bytes of an older release")
        self.layout.sidecar.write_text("sha256:%s\n" % ("1" * 64))
        http = FakeHttp(self.payload)
        self.assertEqual(self.download(http).read_bytes(), self.payload)
        self.assertEqual(http.download_requests, [(DOWNLOAD_URL, None)])

    def test_part_without_sidecar_is_discarded(self):
        self.layout.part.write_bytes(self.payload[:10])
        http = FakeHttp(self.payload)
        self.assertEqual(self.download(http).read_bytes(), self.payload)
        self.assertEqual(http.download_requests, [(DOWNLOAD_URL, None)])

    def test_server_ignoring_range_restarts_from_zero(self):
        self.layout.part.write_bytes(self.payload[:10])
        self.layout.sidecar.write_text("sha256:%s\n" % self.release.sha256)
        http = FakeHttp(self.payload, honour_range=False)
        self.assertEqual(self.download(http).read_bytes(), self.payload)

    def test_damaged_download_deletes_the_part(self):
        damaged = bytearray(self.payload)
        damaged[5] ^= 0xFF
        http = FakeHttp(self.payload, served=bytes(damaged))
        with self.assertRaises(iw.InstallError) as caught:
            self.download(http)
        self.assertIn("damaged", str(caught.exception))
        self.assertFalse(self.layout.part.exists())
        self.assertFalse(self.layout.sidecar.exists())
        self.assertFalse(self.layout.zip.exists())

    def test_dropped_connection_resumes_then_gives_up(self):
        payload = self.payload
        calls = []

        def flaky(request, timeout):
            calls.append(request.get_header("Range"))
            if len(calls) == 1:
                return FakeResponse(200, payload[:100])  # connection closed early
            return FakeHttp(payload)(request, timeout)

        self.assertEqual(self.download(flaky).read_bytes(), payload)
        self.assertEqual(calls, [None, "bytes=100-"])

        def dead(request, timeout):
            raise urllib.error.URLError("gone")

        self.layout.zip.unlink()
        with self.assertRaises(iw.InstallError) as caught:
            self.download(dead)
        self.assertIn("continues where it stopped", str(caught.exception))


# --- Extract and install -----------------------------------------------------------------


def zip_info(name: str, mode: int = 0o100644) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name)
    info.filename = name  # ZipInfo turns backslashes into "/" on Windows; keep the raw name
    info.create_system = 3
    info.external_attr = mode << 16
    return info


class ExtractTests(TempCase):
    def test_entry_rejection(self):
        bad = {
            "/WoT Reborn/x": "absolute path",
            "C:/WoT Reborn/x": "absolute path",
            "WoT Reborn/../x": "parent reference",
            "WoT Reborn/Game/../../x": "parent reference",
            "../WoT Reborn/x": "parent reference",
            "Other/x": "outside WoT Reborn/",
            "WoT Reborn": "outside WoT Reborn/",
            "WoT Reborn\\..\\x": "bad name",
            "WoT Reborn//x": "bad name",
        }
        for name, reason in bad.items():
            self.assertEqual(iw.zip_entry_problem(zip_info(name)), reason, name)
        self.assertEqual(iw.zip_entry_problem(zip_info("WoT Reborn/link", 0o120777)), "symlink")
        self.assertEqual(iw.zip_entry_problem(zip_info("WoT Reborn/fifo", 0o010644)), "special file")
        for good in ("WoT Reborn/", "WoT Reborn/WoT Reborn.exe", "WoT Reborn/Game/WoT/a.pak"):
            self.assertIsNone(iw.zip_entry_problem(zip_info(good)), good)
        windows_made = zipfile.ZipInfo("WoT Reborn/Game/x")
        windows_made.create_system = 0
        windows_made.external_attr = 0x20
        self.assertIsNone(iw.zip_entry_problem(windows_made))

    def test_a_bad_entry_extracts_nothing(self):
        zip_path = self.tmp / "evil.zip"
        with zipfile.ZipFile(str(zip_path), "w") as archive:
            archive.writestr("WoT Reborn/WoT Reborn.exe", b"x")
            archive.writestr(zip_info("WoT Reborn/evil", 0o120777), "/etc/passwd")
        stage = self.tmp / "stage"
        with self.assertRaises(iw.InstallError):
            iw.extract(zip_path, stage, FakeUI(), iw.Log(lambda: FIXED_NOW))
        self.assertEqual(list(stage.iterdir()), [])

    def test_extract_good_zip(self):
        zip_path = self.tmp / "good.zip"
        zip_path.write_bytes(player_zip("0.2.0"))
        root = iw.extract(zip_path, self.tmp / "stage", FakeUI(), iw.Log(lambda: FIXED_NOW))
        self.assertEqual((root / "WoT Reborn.exe").read_bytes(), b"launcher 0.2.0")
        self.assertEqual(iw.read_release_version(root), "0.2.0")


class InstallTests(TempCase):
    def stage(self, version: str = "0.1.3") -> Path:
        zip_path = self.tmp / ("p-%s.zip" % version)
        zip_path.write_bytes(player_zip(version))
        stage = self.tmp / ("stage-" + version)
        return iw.extract(zip_path, stage, FakeUI(), iw.Log(lambda: FIXED_NOW))

    def existing(self) -> Path:
        target = self.tmp / "Games" / "WoT Reborn"
        (target / "Game" / "WoT").mkdir(parents=True)
        (target / "Game" / "WoT" / "old-only.txt").write_text("old")
        (target / "WoT Reborn.exe").write_bytes(b"old launcher")
        (target / "Data" / "Preferences").mkdir(parents=True)
        (target / "Data" / "Preferences" / "prefs").write_text("mine")
        (target / iw.ORIGINAL_INSTALLER_PIN.name).write_bytes(b"installer")
        (target / ".wot-update" / "staged" / "Game").mkdir(parents=True)
        (target / ".wot-update" / "trial").write_text("{}")
        return target

    def test_fresh_install(self):
        target = self.tmp / "Games" / "WoT Reborn"
        self.assertEqual(iw.install_tree(self.stage(), target, iw.Log(lambda: FIXED_NOW)), "fresh")
        self.assertEqual((target / "WoT Reborn.exe").read_bytes(), b"launcher 0.1.3")
        self.assertTrue((target / "Game" / "WoT" / "Content" / "Paks" / "WoT.pak").is_file())

    def test_repair_swap_keeps_data_and_removes_update_workspace(self):
        target = self.existing()
        self.assertEqual(iw.install_tree(self.stage(), target, iw.Log(lambda: FIXED_NOW)),
                         "repaired")
        self.assertEqual((target / "WoT Reborn.exe").read_bytes(), b"launcher 0.1.3")
        self.assertFalse((target / "Game" / "WoT" / "old-only.txt").exists())
        self.assertTrue((target / "Game" / "WoT" / "Content" / "Paks" / "WoT.pak").is_file())
        self.assertEqual((target / "Data" / "Preferences" / "prefs").read_text(), "mine")
        self.assertEqual((target / iw.ORIGINAL_INSTALLER_PIN.name).read_bytes(), b"installer")
        self.assertFalse((target / "Game.old").exists())
        self.assertFalse((target / ".wot-update").exists())
        self.assertFalse((target / ".wot-update.discarded").exists())

    def test_repair_swap_rolls_back_when_the_new_game_cannot_move_in(self):
        target = self.existing()
        staged = self.stage()
        real_rename = iw._rename

        def failing(src, dst):
            if Path(dst) == target / "Game" and Path(src) == staged / "Game":
                raise OSError(errno.EACCES, "Permission denied")
            real_rename(src, dst)

        with mock.patch.object(iw, "_rename", side_effect=failing):
            with self.assertRaises(iw.InstallError) as caught:
                iw.install_tree(staged, target, iw.Log(lambda: FIXED_NOW))
        self.assertIn("previous version was kept", str(caught.exception))
        self.assertEqual((target / "Game" / "WoT" / "old-only.txt").read_text(), "old")
        self.assertFalse((target / "Game.old").exists())
        self.assertEqual((target / "WoT Reborn.exe").read_bytes(), b"old launcher")
        self.assertTrue((target / ".wot-update" / "trial").exists())

    def test_repair_swap_rolls_back_when_the_launcher_cannot_be_replaced(self):
        target = self.existing()
        staged = self.stage()
        with mock.patch.object(iw, "_replace", side_effect=OSError(errno.EBUSY, "busy")):
            with self.assertRaises(iw.InstallError):
                iw.install_tree(staged, target, iw.Log(lambda: FIXED_NOW))
        self.assertEqual((target / "Game" / "WoT" / "old-only.txt").read_text(), "old")
        self.assertFalse((target / "Game.old").exists())
        self.assertTrue((staged / "Game" / "WoT" / "Content" / "Paks" / "WoT.pak").is_file())
        self.assertEqual((target / "WoT Reborn.exe").read_bytes(), b"old launcher")

    def test_interrupted_swap_then_rerun_completes(self):
        target = self.existing()
        os.rename(str(target / "Game"), str(target / "Game.old"))  # stopped after step 1
        iw.recover_interrupted_swap(target, iw.Log(lambda: FIXED_NOW))
        self.assertEqual(iw.install_tree(self.stage(), target, iw.Log(lambda: FIXED_NOW)),
                         "repaired")
        self.assertFalse((target / "Game.old").exists())

    def test_version_order(self):
        cases = [
            ("0.1.4", "0.1.3", 1), ("0.1.3", "0.1.3", 0), ("0.1.3", "0.1.10", -1),
            ("0.1.4-rc1", "0.1.3", 1), ("0.1.4-rc1", "0.1.4", -1), ("1.0.0-rc.2", "1.0.0-rc.10", -1),
            ("1.0.0-alpha", "1.0.0-alpha.1", -1), ("1.0.0-alpha.beta", "1.0.0-beta", -1),
            ("1.0.0-1", "1.0.0-alpha", -1), ("v0.2.0", "0.1.9", 1), ("0.1.3+build5", "0.1.3", 0),
        ]
        for a, b, expected in cases:
            self.assertEqual(iw.compare_versions(a, b), expected, (a, b))
        self.assertIsNone(iw.compare_versions("dev", "0.1.3"))
        self.assertIsNone(iw.compare_versions(None, "0.1.3"))
        self.assertFalse(iw.installed_is_newer("garbage", "0.1.3"))
        self.assertTrue(iw.installed_is_newer("0.1.4-rc1", "0.1.3"))


# --- Original installer -------------------------------------------------------------------


class OriginalInstallerTests(TempCase):
    def setUp(self) -> None:
        super().setUp()
        self.harness = Harness(self.tmp)
        self.target = self.harness.target
        self.target.mkdir(parents=True)
        (self.target / "WoT Reborn.exe").write_bytes(b"x")
        self.downloads = self.harness.home / "Downloads"
        self.downloads.mkdir()

    def place(self, override=None) -> iw.InstallerOutcome:
        return iw.place_original_installer(self.harness.env, self.target, override,
                                           self.harness.ui, self.harness.log())

    def test_numbered_download_lands_under_the_pinned_name(self):
        source = self.downloads / "setup_the_wheel_of_time_333b_(54846) (1).exe"
        source.write_bytes(FAKE_INSTALLER)
        outcome = self.place()
        self.assertEqual(outcome.status, "moved")
        self.assertEqual(outcome.source, source)
        self.assertEqual((self.target / iw.ORIGINAL_INSTALLER_PIN.name).read_bytes(), FAKE_INSTALLER)
        self.assertFalse(source.exists())

    def test_non_matching_files_are_left_alone_and_named(self):
        same_size = self.downloads / "setup_the_wheel_of_time_333b_(54846).exe"
        same_size.write_bytes(b"X" * len(FAKE_INSTALLER))
        other_size = self.downloads / "setup_the_wheel_of_time_patch.exe"
        other_size.write_bytes(b"short")
        unrelated = self.downloads / "something_else.exe"
        unrelated.write_bytes(FAKE_INSTALLER)
        outcome = self.place()
        self.assertEqual(outcome.status, "not-found")
        self.assertEqual(sorted(outcome.rejected), sorted([same_size, other_size]))
        for path in (same_size, other_size, unrelated):
            self.assertTrue(path.exists())
        summary = "\n".join(iw._installer_summary(outcome))
        self.assertIn(str(same_size), summary)
        self.assertIn("PC or Mac", summary)

    def test_existing_verified_copy_is_kept_and_a_wrong_one_blocks(self):
        dest = self.target / iw.ORIGINAL_INSTALLER_PIN.name
        dest.write_bytes(FAKE_INSTALLER)
        (self.downloads / "setup_the_wheel_of_time_333b_(54846).exe").write_bytes(FAKE_INSTALLER)
        self.assertEqual(self.place().status, "already")
        dest.write_bytes(b"something else")
        outcome = self.place()
        self.assertEqual(outcome.status, "blocked")
        self.assertEqual(dest.read_bytes(), b"something else")
        self.assertTrue((self.downloads / "setup_the_wheel_of_time_333b_(54846).exe").exists())

    def test_removable_media_top_two_levels_and_cross_device_copy(self):
        media = self.harness.root / "run" / "media" / "player" / "SDCARD"
        (media / "installers" / "deeper").mkdir(parents=True)
        too_deep = media / "installers" / "deeper" / "setup_the_wheel_of_time_333b_(54846).exe"
        too_deep.write_bytes(FAKE_INSTALLER)
        outcome = self.place()
        self.assertEqual(outcome.status, "not-found")
        level_two = media / "installers" / "setup_the_wheel_of_time_333b_(54846).exe"
        level_two.write_bytes(FAKE_INSTALLER)
        real_link = os.link

        def cross_device(src, dst, *args, **kwargs):
            if Path(src) == level_two:
                raise OSError(errno.EXDEV, "Invalid cross-device link")
            return real_link(src, dst, *args, **kwargs)

        with mock.patch.object(iw.os, "link", side_effect=cross_device):
            outcome = self.place()
        self.assertEqual(outcome.status, "copied")
        self.assertEqual((self.target / iw.ORIGINAL_INSTALLER_PIN.name).read_bytes(), FAKE_INSTALLER)
        self.assertTrue(level_two.exists())  # copied, never removed from the card
        self.assertEqual([p.name for p in self.target.iterdir() if p.name.endswith(".part")], [])

    def test_search_override_never_reads_the_default_locations(self):
        in_downloads = self.downloads / "setup_the_wheel_of_time_333b_(54846).exe"
        in_downloads.write_bytes(FAKE_INSTALLER)
        desktop = self.harness.home / "Desktop"
        desktop.mkdir()
        media = self.harness.root / "run" / "media" / "player" / "USB"
        media.mkdir(parents=True)
        scratch = self.tmp / "scratch-search"
        scratch.mkdir()
        (scratch / "setup_the_wheel_of_time_333b_(54846) (2).exe").write_bytes(FAKE_INSTALLER)
        read = []
        real_scandir = iw._scandir

        def recording(path):
            read.append(Path(path))
            return real_scandir(path)

        with mock.patch.object(iw, "_scandir", side_effect=recording):
            self.assertEqual(iw.installer_search_dirs(self.harness.env, scratch), [(scratch, 0)])
            outcome = self.place(override=scratch)
        self.assertEqual(outcome.status, "moved")
        self.assertTrue(in_downloads.exists())
        self.assertTrue(read)
        for path in read:
            self.assertEqual(path, scratch)
        defaults = [folder for folder, _ in iw.installer_search_dirs(self.harness.env, None)]
        self.assertEqual(defaults, [self.downloads, desktop, media])

    def test_nothing_found_is_not_an_error(self):
        outcome = self.place()
        self.assertEqual(outcome.status, "not-found")
        self.assertEqual(outcome.rejected, [])


# --- Steam shortcut and artwork ----------------------------------------------------------


class ShortcutTests(TempCase):
    def our_entry_bytes(self, harness: Harness, appid: int) -> bytes:
        return shortcuts_bytes([
            (0, 0xB0000001, '"/home/player/Applications/emu.AppImage"', b""),
            (1, appid, '"%s"' % harness.exe, b""),
        ])

    def test_appid_is_crc32_of_exe_and_name_with_the_top_bit(self):
        exe = '"/home/player/Games/WoT Reborn/WoT Reborn.exe"'
        expected = (binascii.crc32((exe + "WoT Reborn").encode()) & 0xFFFFFFFF) | 0x80000000
        appid = iw.shortcut_appid(exe, "WoT Reborn")
        self.assertEqual(appid, expected)
        self.assertGreaterEqual(appid, 0x80000000)
        self.assertLessEqual(appid, 0xFFFFFFFF)
        self.assertEqual(vdf.int32_node("appid", appid).uint32(), appid)

    def test_existing_shortcut_is_reused(self):
        harness = Harness(self.tmp, steam_running=True)
        harness.shortcuts.write_bytes(self.our_entry_bytes(harness, 0x9ABCDEF0))
        before = harness.shortcuts.read_bytes()
        outcome = iw.ensure_shortcut(harness.env, harness.ui, iw.Account(0, ACCOUNT_ID),
                                     harness.exe, harness.target, harness.log())
        self.assertEqual((outcome.appid, outcome.route), (0x9ABCDEF0, "existing"))
        self.assertEqual(harness.spawned, [])
        self.assertEqual(harness.shortcuts.read_bytes(), before)

    def test_add_through_steamos_add_to_steam(self):
        harness = Harness(self.tmp, steam_running=True)
        harness.time.at(3.0, lambda: harness.shortcuts.write_bytes(
            self.our_entry_bytes(harness, 0xC0FFEE01)))
        log = harness.log()
        log.attach(self.tmp / "install.log")
        outcome = iw.ensure_shortcut(harness.env, harness.ui, iw.Account(0, ACCOUNT_ID),
                                     harness.exe, harness.target, log)
        self.assertEqual((outcome.appid, outcome.route), (0xC0FFEE01, "steamos-add-to-steam"))
        self.assertEqual(harness.spawned, [["/usr/bin/steamos-add-to-steam", str(harness.exe)]])
        self.assertEqual(harness.runner.calls, [])
        self.assertEqual(harness.ui.questions, [])
        self.assertIn("shortcut route: steamos-add-to-steam", (self.tmp / "install.log").read_text())

    def test_add_through_the_url_when_the_wrapper_is_missing(self):
        harness = Harness(self.tmp, steam_running=True, wrapper=False)
        harness.time.at(5.0, lambda: harness.shortcuts.write_bytes(
            self.our_entry_bytes(harness, 0xC0FFEE02)))
        outcome = iw.ensure_shortcut(harness.env, harness.ui, iw.Account(0, ACCOUNT_ID),
                                     harness.exe, harness.target, harness.log())
        self.assertEqual(outcome.route, "steam-addnonsteamgame-url")
        self.assertTrue((harness.root / "tmp" / "addnonsteamgamefile").is_file())
        encoded = urllib.parse.quote(str(harness.exe), safe="")
        self.assertNotIn("/", encoded)
        self.assertNotIn(" ", encoded)
        self.assertEqual(harness.spawned, [["/usr/bin/steam", "steam://addnonsteamgame/" + encoded]])

    def test_fallback_closes_steam_and_appends_after_backup(self):
        harness = Harness(self.tmp, steam_running=True)
        original = shortcuts_bytes([(0, 0xB0000001, '"/home/player/emu"', b"\x02Unknown\x00\x07\x00\x00\x00")])
        harness.shortcuts.write_bytes(original)
        harness.runner.on("-shutdown", lambda: harness.time.at(harness.time.now + 4, harness.stop_steam))
        outcome = iw.ensure_shortcut(harness.env, harness.ui, iw.Account(0, ACCOUNT_ID),
                                     harness.exe, harness.target, harness.log())
        self.assertEqual(outcome.route, "shortcuts-vdf-fallback")
        self.assertTrue(outcome.steam_closed)
        self.assertEqual(len(harness.ui.questions), 1)
        self.assertEqual(harness.runner.calls, [["/usr/bin/steam", "-shutdown"]])
        self.assertGreaterEqual(harness.time.now, iw.ADD_POLL_SECONDS)
        backup = harness.config / "shortcuts.vdf.wot-backup-20261109T123000Z"
        self.assertEqual(backup.read_bytes(), original)
        written = harness.shortcuts.read_bytes()
        self.assertEqual(written[:len(original) - 2], original[:-2])
        exe_value = '"%s"' % harness.exe
        self.assertEqual(outcome.appid, iw.shortcut_appid(exe_value, "WoT Reborn"))
        entry = vdf.find(vdf.parse_binary(written).nodes, "shortcuts").value[-1]
        self.assertEqual(entry.name, "1")
        self.assertEqual(entry.child("appid").uint32(), outcome.appid)
        self.assertEqual(entry.child("AppName").text(), "WoT Reborn")
        self.assertEqual(entry.child("Exe").text(), exe_value)
        self.assertEqual(entry.child("StartDir").text(), '"%s/"' % harness.target)
        for key in ("LaunchOptions", "LastPlayTime", "FlatpakAppID", "tags"):
            self.assertIsNotNone(entry.child(key), key)
        self.assertEqual(iw.find_shortcut(vdf.parse_binary(written), harness.exe), outcome.appid)

    def test_fallback_rereads_and_reuses_the_entry_steam_saved_on_exit(self):
        harness = Harness(self.tmp, steam_running=True)

        def steam_saves_and_exits():
            harness.shortcuts.write_bytes(self.our_entry_bytes(harness, 0xD00D0001))
            harness.stop_steam()

        harness.runner.on("-shutdown", steam_saves_and_exits)
        outcome = iw.ensure_shortcut(harness.env, harness.ui, iw.Account(0, ACCOUNT_ID),
                                     harness.exe, harness.target, harness.log())
        self.assertEqual(outcome.appid, 0xD00D0001)
        self.assertEqual(outcome.route, "steamos-add-to-steam-saved-on-exit")
        self.assertEqual(list(harness.config.glob("*.wot-backup-*")), [])

    def test_declining_to_close_steam_fails_plainly(self):
        harness = Harness(self.tmp, steam_running=True, answer=False)
        with self.assertRaises(iw.InstallError) as caught:
            iw.ensure_shortcut(harness.env, harness.ui, iw.Account(0, ACCOUNT_ID),
                               harness.exe, harness.target, harness.log())
        self.assertIn("Add to Steam", str(caught.exception))
        self.assertEqual(harness.runner.calls, [])
        self.assertFalse(harness.shortcuts.exists())

    def test_steam_that_will_not_close_fails_plainly(self):
        harness = Harness(self.tmp, steam_running=True)
        with self.assertRaises(iw.InstallError) as caught:
            iw.ensure_shortcut(harness.env, harness.ui, iw.Account(0, ACCOUNT_ID),
                               harness.exe, harness.target, harness.log())
        self.assertIn("Steam didn't close", str(caught.exception))
        self.assertFalse(harness.shortcuts.exists())

    def test_steam_not_running_appends_without_asking(self):
        harness = Harness(self.tmp, steam_running=False)
        outcome = iw.ensure_shortcut(harness.env, harness.ui, iw.Account(0, ACCOUNT_ID),
                                     harness.exe, harness.target, harness.log())
        self.assertEqual(outcome.route, "shortcuts-vdf-fallback")
        self.assertFalse(outcome.steam_closed)
        self.assertEqual((harness.spawned, harness.runner.calls, harness.ui.questions), ([], [], []))
        document = vdf.parse_binary(harness.shortcuts.read_bytes())
        self.assertEqual([n.name for n in vdf.find(document.nodes, "shortcuts").value], ["0"])

    def test_unreadable_shortcuts_file_is_left_alone(self):
        harness = Harness(self.tmp, steam_running=False)
        harness.shortcuts.write_bytes(b"\x00shortcuts\x00\x09broken")
        with self.assertRaises(iw.InstallError) as caught:
            iw.ensure_shortcut(harness.env, harness.ui, iw.Account(0, ACCOUNT_ID),
                               harness.exe, harness.target, harness.log())
        self.assertIn("left alone", str(caught.exception))
        self.assertEqual(harness.shortcuts.read_bytes(), b"\x00shortcuts\x00\x09broken")

    def test_artwork_goes_to_our_appid_only(self):
        harness = Harness(self.tmp)
        art = self.tmp / "art"
        art.mkdir()
        for name, _ in iw.ART_FILES:
            (art / name).write_bytes(name.encode())
        grid = harness.config / "grid"
        grid.mkdir()
        (grid / "123p.jpg").write_bytes(b"someone else's")
        (grid / "2900000001p.jpg").write_bytes(b"stale")
        self.assertTrue(iw.place_artwork(harness.env, iw.Account(0, ACCOUNT_ID), 2900000001,
                                         art, harness.log()))
        expected = {
            "2900000001p.jpg": b"capsule_portrait.jpg",
            "2900000001.jpg": b"capsule_wide.jpg",
            "2900000001_hero.jpg": b"hero.jpg",
            "2900000001_logo.png": b"logo.png",
            "2900000001.json": b"logo.json",
        }
        for name, data in expected.items():
            self.assertEqual((grid / name).read_bytes(), data, name)
        self.assertEqual((grid / "123p.jpg").read_bytes(), b"someone else's")
        self.assertEqual(sorted(p.name for p in grid.iterdir()), sorted(list(expected) + ["123p.jpg"]))

    def test_shipped_art_set_is_complete(self):
        for name, _ in iw.ART_FILES:
            self.assertTrue((iw.ART_DIR / name).is_file(), name)
        logo = json.loads((iw.ART_DIR / "logo.json").read_text())
        self.assertEqual(logo["nVersion"], 1)


# --- The whole program ---------------------------------------------------------------------


class EndToEndTests(TempCase):
    def test_fresh_install_then_repair(self):
        harness = Harness(self.tmp, steam_running=False)
        (harness.home / "Downloads").mkdir()
        (harness.home / "Downloads" / "setup_the_wheel_of_time_333b_(54846) (1).exe").write_bytes(
            FAKE_INSTALLER)
        payload = player_zip("0.1.3")
        harness.env.urlopen = FakeHttp(payload)
        self.assertEqual(iw.main([], env=harness.env, ui=harness.ui), 0, harness.ui.errors)
        summary = harness.ui.infos[-1]
        self.assertIn("WoT Reborn 0.1.3 is installed in %s" % harness.target, summary)
        self.assertIn("Moved the original game installer", summary)
        self.assertIn("Return to Gaming Mode", summary)
        self.assertNotIn("wheel of time", summary.casefold())
        self.assertTrue((harness.target / iw.ORIGINAL_INSTALLER_PIN.name).is_file())
        appid = iw.find_shortcut(vdf.parse_binary(harness.shortcuts.read_bytes()), harness.exe)
        self.assertIsNotNone(appid)
        self.assertTrue((harness.config / "grid" / ("%dp.jpg" % appid)).is_file())
        work = harness.home / "Games" / ".wot-reborn-install"
        self.assertEqual(sorted(p.name for p in work.iterdir()), ["install.log"])
        self.assertIn("shortcut route: shortcuts.vdf fallback", (work / "install.log").read_text())

        # Run again: repairs a deleted file, keeps Data/, drops .wot-update/, reuses the shortcut.
        pak = harness.target / "Game" / "WoT" / "Content" / "Paks" / "WoT.pak"
        pak.unlink()
        (harness.target / "Data").mkdir()
        (harness.target / "Data" / "keep.txt").write_text("mine")
        (harness.target / ".wot-update" / "prev").mkdir(parents=True)
        harness.ui = FakeUI()
        self.assertEqual(iw.main([], env=harness.env, ui=harness.ui), 0, harness.ui.errors)
        self.assertTrue(pak.is_file())
        self.assertEqual((harness.target / "Data" / "keep.txt").read_text(), "mine")
        self.assertFalse((harness.target / ".wot-update").exists())
        self.assertIn("repaired", harness.ui.infos[-1])
        self.assertIn("already beside the game", harness.ui.infos[-1])
        self.assertIn("already in your Steam Library", harness.ui.infos[-1])
        self.assertIn("shortcut route: existing entry reused", (work / "install.log").read_text())

    def test_newer_installed_release_is_kept_and_nothing_downloaded(self):
        harness = Harness(self.tmp, steam_running=False)
        version_file = harness.target.joinpath(*iw.RELEASE_VERSION_PARTS)
        version_file.parent.mkdir(parents=True)
        version_file.write_bytes(release_version_json("0.1.4-rc1"))
        (harness.target / "WoT Reborn.exe").write_bytes(b"newer launcher")
        http = FakeHttp(player_zip("0.1.3"))
        harness.env.urlopen = http
        self.assertEqual(iw.main([], env=harness.env, ui=harness.ui), 0, harness.ui.errors)
        self.assertEqual(http.download_requests, [])
        self.assertEqual((harness.target / "WoT Reborn.exe").read_bytes(), b"newer launcher")
        self.assertIn("0.1.4-rc1", harness.ui.infos[-1])
        self.assertTrue(harness.shortcuts.is_file())

    def test_newer_installed_release_than_the_zip_skips_the_swap(self):
        harness = Harness(self.tmp, steam_running=False)
        version_file = harness.target.joinpath(*iw.RELEASE_VERSION_PARTS)
        version_file.parent.mkdir(parents=True)
        version_file.write_bytes(release_version_json("0.1.4-rc1"))
        (harness.target / "WoT Reborn.exe").write_bytes(b"newer launcher")
        payload = player_zip("0.1.3")
        harness.env.urlopen = FakeHttp(payload, document=api_document(payload, tag="latest"))
        self.assertEqual(iw.main([], env=harness.env, ui=harness.ui), 0, harness.ui.errors)
        self.assertEqual((harness.target / "WoT Reborn.exe").read_bytes(), b"newer launcher")
        work = harness.home / "Games" / ".wot-reborn-install"
        self.assertEqual(sorted(p.name for p in work.iterdir()), ["install.log"])

    def test_target_override(self):
        harness = Harness(self.tmp, steam_running=False)
        target = self.tmp / "scratch" / "Cell Target"
        harness.env.urlopen = FakeHttp(player_zip())
        self.assertEqual(iw.main(["--target", str(target), "--installer-search-dir",
                                  str(self.tmp / "nowhere")], env=harness.env, ui=harness.ui), 0,
                         harness.ui.errors)
        self.assertTrue((target / "WoT Reborn.exe").is_file())
        self.assertFalse(harness.target.exists())
        self.assertTrue((target.parent / ".wot-reborn-install" / "install.log").is_file())

    def test_unexpected_errors_show_the_log_location_without_a_traceback(self):
        harness = Harness(self.tmp)

        def broken(path):
            raise RuntimeError("boom")

        harness.env.disk_free = broken
        self.assertEqual(iw.main([], env=harness.env, ui=harness.ui), 2)
        message = harness.ui.errors[0]
        log = harness.home / "Games" / ".wot-reborn-install" / "install.log"
        self.assertIn(str(log), message)
        self.assertNotIn("Traceback", message)
        self.assertNotIn("boom", message)
        self.assertIn("Traceback", log.read_text())

    def test_self_test_prints_the_marker(self):
        with mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            self.assertEqual(iw.main(["--self-test"]), 0)
        self.assertTrue(out.getvalue().startswith(iw.SELF_TEST_MARKER + " "))


class UITests(TempCase):
    def test_choose_ui_order(self):
        harness = Harness(self.tmp)
        tools = {}
        harness.env.which = tools.get
        self.assertIsInstance(iw.choose_ui(harness.env), iw.ConsoleUI)
        harness.env.environ = {"WAYLAND_DISPLAY": "wayland-0"}
        self.assertIsInstance(iw.choose_ui(harness.env), iw.NotifyUI)
        tools["kdialog"] = "/usr/bin/kdialog"
        self.assertIsInstance(iw.choose_ui(harness.env), iw.KDialogUI)
        tools["zenity"] = "/usr/bin/zenity"
        self.assertIsInstance(iw.choose_ui(harness.env), iw.ZenityUI)

    def test_zenity_dialogs_escape_markup(self):
        harness = Harness(self.tmp)
        ui = iw.ZenityUI(harness.env, "/usr/bin/zenity")
        self.assertTrue(ui.question("A & B <c>", "Close Steam", "Not now"))
        argv = harness.runner.calls[-1]
        self.assertEqual(argv[:2], ["/usr/bin/zenity", "--question"])
        self.assertIn("--text=A &amp; B &lt;c&gt;", argv)
        self.assertIn("--ok-label=Close Steam", argv)

    def test_player_text_names_no_setting_terms(self):
        source = Path(iw.__file__).read_text(encoding="utf-8").casefold()
        self.assertNotIn("wheel of time", source)


# --- Review fixes (P1-1, P1-2, P1-5, P1-6) -------------------------------------------------


class CancellingUI(FakeUI):
    """Cancels the run the first time any progress is reported."""

    def fraction(self, value: float, detail: str = "") -> None:
        raise iw.Cancelled()


class ReviewFixTests(TempCase):
    def test_second_installer_run_is_refused_while_one_holds_the_lock(self):
        harness = Harness(self.tmp)
        harness.env.lock = lambda path: False
        self.assertNotEqual(iw.main([], env=harness.env, ui=harness.ui), 0)
        self.assertTrue(any("already running" in text for text in harness.ui.errors),
                        harness.ui.errors)

    def test_cancelled_cross_device_copy_leaves_no_partial_file(self):
        harness = Harness(self.tmp)
        target = harness.target
        target.mkdir(parents=True)
        (target / "WoT Reborn.exe").write_bytes(b"x")
        media = harness.root / "run" / "media" / "player" / "SDCARD"
        media.mkdir(parents=True)
        source = media / "setup_the_wheel_of_time_333b_(54846).exe"
        source.write_bytes(FAKE_INSTALLER)
        real_link = os.link

        def cross_device(src, dst, *args, **kwargs):
            if Path(src) == source:
                raise OSError(errno.EXDEV, "Invalid cross-device link")
            return real_link(src, dst, *args, **kwargs)

        with mock.patch.object(iw.os, "link", side_effect=cross_device):
            with self.assertRaises(iw.Cancelled):
                iw.place_original_installer(harness.env, target, None, CancellingUI(),
                                            harness.log())
        self.assertEqual([p.name for p in target.iterdir()], ["WoT Reborn.exe"])
        self.assertTrue(source.exists())

    def test_existing_shortcut_keeps_the_players_own_art(self):
        harness = Harness(self.tmp)
        art = self.tmp / "art"
        art.mkdir()
        for name, _ in iw.ART_FILES:
            (art / name).write_bytes(name.encode())
        grid = harness.config / "grid"
        grid.mkdir()
        (grid / "2900000001p.jpg").write_bytes(b"the player's own capsule")
        self.assertTrue(iw.place_artwork(harness.env, iw.Account(0, ACCOUNT_ID), 2900000001,
                                         art, harness.log(), only_missing=True))
        self.assertEqual((grid / "2900000001p.jpg").read_bytes(), b"the player's own capsule")
        self.assertEqual((grid / "2900000001_hero.jpg").read_bytes(), b"hero.jpg")

    def test_download_retries_back_off_for_about_a_minute(self):
        harness = Harness(self.tmp)
        payload = player_zip()
        release = iw.parse_release(api_document(payload))
        layout = iw.Layout(harness.target)
        layout.work.mkdir(parents=True)

        def offline(request, timeout):
            raise OSError("network is unreachable")

        harness.env.urlopen = offline
        with self.assertRaises(iw.InstallError):
            iw.download(harness.env, layout, release, harness.ui, harness.log())
        self.assertEqual(harness.time.now, 2.0 + 4.0 + 8.0 + 16.0 + 30.0)


if __name__ == "__main__":
    unittest.main()
