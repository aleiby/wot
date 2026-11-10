"""Unit tests for the SteamOS installer's Deck cell (SSH is faked; no Deck needed)."""
import inspect
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import deck
import install_cell as cell

sys.path.insert(0, str(cell.INSTALLER_DIR))
import build_desktop_entry  # noqa: E402
import install_wot_reborn as iw  # noqa: E402

SSH_LENGTH = len(deck._ssh_argv(deck.DeckTarget("h")))
ACCOUNT = "/home/player/.local/share/Steam/userdata/42"
APPID = 3000001234
ENTRY = b"[Desktop Entry]\nName=Install WoT Reborn\n# stand-in\n"


def install_log(route, appid=APPID, account=ACCOUNT, done=True, stopped=None):
    lines = ["2026-11-09T10:00:00Z installer started; target=/home/player/x/WoT Reborn",
             f"2026-11-09T10:00:00Z steam account folder: {account}",
             f"2026-11-09T10:00:05Z shortcut route: {route} (appid {appid})"]
    if stopped:
        lines.append(f"2026-11-09T10:00:06Z stopped: {stopped}")
    if done:
        lines.append("2026-11-09T10:00:06Z done")
    return "\n".join(lines) + "\n"


def snapshot(appid=APPID, **overrides):
    base = {"exe": True, "game": True, "installer": cell.PINNED_INSTALLER_SIZE,
            "art": {leaf.format(appid=appid): True for leaf in cell.ART_LEAVES},
            "probe": None, "marker": False, "steam": True}
    base.update(overrides)
    return base


class FakeDeck:
    """Answers each SSH script by its marker line and keeps the state it changes."""

    def __init__(self, logs=(), snapshots=(), **options):
        self.calls = []
        self.downloads = True
        self.size = cell.PINNED_INSTALLER_SIZE
        self.inode = 4242
        self.steam = "running"
        self.link = None
        self.run_status = 0
        self.lose_downloads_on_run = False
        self.restore = "restored"
        self.repair_prep = (0, "deleted 17 Game/WoT/Content/a b.pak")
        self.logs, self.snapshots = list(logs), list(snapshots)
        for name, value in options.items():
            setattr(self, name, value)

    def markers(self):
        return [marker for marker, _, _ in self.calls]

    def script(self, marker):
        return next(script for name, script, _ in self.calls if name == marker)

    def run(self, argv, input_bytes=None, timeout=120):
        def done(status=0, out="", err=""):
            return subprocess.CompletedProcess(argv, status, out.encode(), err.encode())

        if len(argv) > SSH_LENGTH:  # a remote command with data on stdin: the upload
            self.calls.append(("UPLOAD", argv[-1], input_bytes))
            return done()
        script = input_bytes.decode()
        marker = script.splitlines()[0][len("# "):]
        self.calls.append((marker, script, None))
        if marker == "WOT-CELL-GUARD":
            return done(out=f"present {self.size} {self.inode}\n" if self.downloads
                        else "missing\n")
        if marker == "WOT-CELL-STEAM":
            return done(out=self.steam + "\n")
        if marker == "WOT-CELL-STEAM-SHUTDOWN":
            self.steam = "stopped"
            return done(out="stopped\n")
        if marker == "WOT-CELL-PREP":
            return done(out="ready\n")
        if marker == "WOT-CELL-REPAIR-PREP":
            return done(self.repair_prep[0], self.repair_prep[1] + "\n")
        if marker == "WOT-CELL-LINK":
            return done(out=(self.link or f"linked {self.inode} 2") + "\n")
        if marker == "WOT-CELL-RUN":
            if self.lose_downloads_on_run:
                self.downloads = False
            return done(self.run_status, err="installer console text")
        if marker == "WOT-CELL-LOG":
            return done(out=self.logs.pop(0))
        if marker == "WOT-CELL-SNAPSHOT":
            return done(out=json.dumps(self.snapshots.pop(0)) + "\n")
        if marker == "WOT-CELL-RESTORE":
            if self.restore == "restored":
                self.downloads = True
                return done(out="restored /home/player/wot-install-cell/WoT Reborn/"
                                "setup_the_wheel_of_time_333b_(54846).exe\n")
            return done(1, out="lost\n")
        if marker == "WOT-CELL-CLEANUP":
            return done(out="cleaned\n")
        raise AssertionError(f"unscripted Deck call: {script[:80]!r}")


def run(fake, cases=cell.DEFAULT_CASES, **options):
    printed = []
    verdicts = cell.run_install_cells(deck.DeckTarget("h"), fake, cases, entry=ENTRY,
                                      out=printed.append, **options)
    return dict(verdicts), printed


class LayoutTests(unittest.TestCase):
    def test_scratch_and_installer_names_are_plain(self):
        for scratch in ("Games", "Downloads", "../wot-install-cell", "wot-install-cell/x",
                        'wot-install-cell"', "wot-install-cell$HOME", ""):
            with self.subTest(scratch=scratch), self.assertRaises(deck.DeckError):
                cell.Layout(scratch)
        for name in ('setup_the_wheel_of_time".exe', "setup_the_wheel_of_time$(x).exe",
                     "other.exe", "setup_the_wheel_of_time_333b_(54846).zip"):
            with self.subTest(name=name), self.assertRaises(deck.DeckError):
                cell.Layout(installer_name=name)
        layout = cell.Layout("wot-install-cell2", "setup_the_wheel_of_time_333b_(54846) (1).exe")
        self.assertEqual(layout.downloads,
                         '"$HOME/Downloads/setup_the_wheel_of_time_333b_(54846) (1).exe"')
        self.assertEqual(layout.target("fresh"), '"$HOME/wot-install-cell2/WoT Reborn"')
        self.assertEqual(layout.target("fallback"),
                         '"$HOME/wot-install-cell2/fallback/WoT Reborn"')

    def test_the_pin_matches_the_installer_program(self):
        pin = iw.ORIGINAL_INSTALLER_PIN
        self.assertEqual((cell.PINNED_INSTALLER_NAME, cell.PINNED_INSTALLER_SIZE),
                         (pin.name, pin.size))
        self.assertEqual(len(cell.ART_LEAVES), len(iw.ART_FILES))
        self.assertEqual(sorted(cell.ART_LEAVES),
                         sorted(pattern for _, pattern in iw.ART_FILES))

    def test_the_log_lines_the_cell_reads_are_the_programs_own(self):
        source = inspect.getsource(iw)
        for fragment in ('"shortcut route: %s (appid %d)"',
                         '"shortcut route: existing entry reused (appid %d)"',
                         '"shortcut route: shortcuts.vdf fallback (appid %d)"',
                         '"steam account folder: %s"', 'log.write("done")',
                         'log.write("Steam closed")', '"stopped: %s"'):
            self.assertIn(fragment, source)
        self.assertEqual(cell.STEP3_ROUTES, ("steamos-add-to-steam",
                                             "steam-addnonsteamgame-url"))


class OverrideTests(unittest.TestCase):
    def test_the_override_excludes_the_default_search_locations(self):
        layout = cell.Layout()
        words = shlex.split(cell.run_script(layout, "fresh", 1800).splitlines()[1])
        entry = words.index(layout.entry.strip('"'))
        with tempfile.TemporaryDirectory() as temporary:
            home, root = Path(temporary) / "home", Path(temporary) / "root"
            arguments = [word.replace("$HOME", str(home)) for word in words[entry + 1:-2]]
            self.assertEqual(words[-2:], ["<", "/dev/null"])
            options = iw.parse_args(arguments)
            dl = home / layout.scratch / "dl"
            self.assertEqual(options.target, home / layout.scratch / "WoT Reborn")
            self.assertEqual(options.installer_search_dir, dl)
            name = cell.PINNED_INSTALLER_NAME
            for folder in (home / "Downloads", home / "Desktop", dl,
                           root / "run" / "media" / "player" / "SD"):
                folder.mkdir(parents=True)
                (folder / name).write_bytes(b"x")
            env = iw.Env(home, root=root, user="player")
            self.assertEqual(iw.installer_search_dirs(env, options.installer_search_dir),
                             [(dl, 0)])
            self.assertEqual([path for path, _ in iw.find_installer_candidates(
                env, options.installer_search_dir)], [dl / name])
            # Without the override the same Deck would offer every default copy.
            self.assertEqual(len(list(iw.find_installer_candidates(env, None))), 3)

    def test_the_run_is_the_assets_own_bootstrap_without_a_display(self):
        layout = cell.Layout()
        script = cell.run_script(layout, "repair", 1800)
        self.assertIn("env -u DISPLAY -u WAYLAND_DISPLAY timeout --kill-after=30 1800 "
                      "python3 -I -c ", script)
        words = shlex.split(script.splitlines()[1])
        self.assertEqual(words[words.index("-c") + 1], build_desktop_entry.BOOTSTRAP_STAGE1)
        self.assertEqual(words[words.index("-c") + 2],
                         "$HOME/wot-install-cell/Install-WoT-Reborn.desktop")
        self.assertNotIn("Downloads", script)

    def test_the_link_is_a_hard_link_never_a_copy_or_symlink(self):
        script = cell.link_script(cell.Layout())
        self.assertIn('ln -- "$HOME/Downloads/setup_the_wheel_of_time_333b_(54846).exe" '
                      '"$l"', script)
        self.assertNotIn("ln -s", script)
        self.assertNotIn("cp ", script)
        self.assertIn('[ ! -L "$l" ]', script)

    def test_the_built_entry_is_the_release_asset(self):
        self.assertEqual(cell.build_entry(), build_desktop_entry.build_entry())


class InstallCellTests(unittest.TestCase):
    def test_fresh_then_repair_pass_and_clean_up(self):
        fake = FakeDeck(
            logs=[install_log("steamos-add-to-steam"),
                  install_log("existing entry reused")],
            snapshots=[snapshot(), snapshot(probe=17, marker=True)])
        verdicts, printed = run(fake)
        self.assertTrue(all(v.ok for v in verdicts.values()), verdicts)
        self.assertEqual(list(verdicts), ["install-fresh", "install-repair", "cleanup"])
        self.assertIn("Game/WoT/Content/a b.pak restored", verdicts["install-repair"].reason)
        # The Downloads guard runs at the start, before and after every case,
        # and again right before cleanup; Steam is never shut down.
        guard, case = "WOT-CELL-GUARD", ["WOT-CELL-LINK"]
        self.assertEqual(fake.markers(), [
            guard,
            guard, "WOT-CELL-STEAM", "WOT-CELL-PREP", *case, "UPLOAD", "WOT-CELL-RUN",
            "WOT-CELL-LOG", "WOT-CELL-SNAPSHOT", guard,
            guard, "WOT-CELL-STEAM", "WOT-CELL-REPAIR-PREP", *case, "WOT-CELL-RUN",
            "WOT-CELL-LOG", "WOT-CELL-SNAPSHOT", guard,
            guard, "WOT-CELL-CLEANUP"])
        upload = next(call for call in fake.calls if call[0] == "UPLOAD")
        self.assertEqual(upload[2], ENTRY)
        cleanup = fake.script("WOT-CELL-CLEANUP")
        self.assertIn(f"{ACCOUNT}/config/grid/{APPID}p.jpg", cleanup)
        self.assertIn(f"{ACCOUNT}/config/grid/{APPID}_logo.png", cleanup)
        self.assertIn(f'rm -rf -- "$HOME/.local/share/Steam/steamapps/compatdata/{APPID}"',
                      cleanup)
        self.assertIn('rm -rf -- "$HOME/wot-install-cell"', cleanup)
        self.assertNotIn("Downloads", cleanup)
        self.assertTrue(any(f"appid {APPID}" in line and "Remove non-Steam" in line
                            for line in printed), printed)

    def test_the_hard_link_guard_stops_before_the_program_runs(self):
        for link in ("linked 999 2", "linked 4242 1", "cp: no"):
            with self.subTest(link=link):
                fake = FakeDeck(link=link)
                verdicts, _ = run(fake)
                self.assertFalse(verdicts["install-fresh"].ok)
                self.assertIn("not a hard link", verdicts["install-fresh"].reason)
                self.assertEqual(verdicts["install-repair"].reason,
                                 "not run: an earlier case stopped the cell")
                self.assertNotIn("WOT-CELL-RUN", fake.markers())
                self.assertNotIn("UPLOAD", fake.markers())
                self.assertTrue(verdicts["cleanup"].ok)

    def test_a_downloads_installer_lost_during_a_case_is_restored_before_cleanup(self):
        fake = FakeDeck(logs=[install_log("steamos-add-to-steam")], snapshots=[snapshot()],
                        lose_downloads_on_run=True)
        verdicts, _ = run(fake)
        fresh = verdicts["install-fresh"]
        self.assertFalse(fresh.ok)
        self.assertIn("went missing during the case", fresh.reason)
        self.assertIn("restored /home/player/wot-install-cell/", fresh.reason)
        self.assertEqual(verdicts["install-repair"].reason,
                         "not run: an earlier case stopped the cell")
        markers = fake.markers()
        self.assertLess(markers.index("WOT-CELL-RESTORE"), markers.index("WOT-CELL-CLEANUP"))
        self.assertTrue(verdicts["cleanup"].ok)
        restore = fake.script("WOT-CELL-RESTORE")
        self.assertIn("-inum 4242", restore)
        self.assertIn('find "$HOME/wot-install-cell" -xdev', restore)
        self.assertIn('ln -- "$src" "$f"', restore)

    def test_a_lost_downloads_installer_keeps_the_scratch_folder(self):
        fake = FakeDeck(logs=[install_log("steamos-add-to-steam")], snapshots=[snapshot()],
                        lose_downloads_on_run=True, restore="lost")
        verdicts, _ = run(fake)
        self.assertIn("LOST", verdicts["install-fresh"].reason)
        self.assertFalse(verdicts["cleanup"].ok)
        self.assertIn("NOT cleaned up", verdicts["cleanup"].reason)
        self.assertNotIn("WOT-CELL-CLEANUP", fake.markers())

    def test_a_missing_or_wrong_size_downloads_installer_touches_nothing(self):
        for options in ({"downloads": False}, {"size": 1}):
            with self.subTest(options=options):
                fake = FakeDeck(**options)
                verdicts, _ = run(fake)
                self.assertEqual(list(verdicts), ["downloads-installer"])
                self.assertIn("nothing was touched", verdicts["downloads-installer"].reason)
                self.assertEqual(fake.markers(), ["WOT-CELL-GUARD"])

    def test_fresh_and_repair_need_steam_running(self):
        fake = FakeDeck(steam="stopped")
        verdicts, _ = run(fake)
        self.assertIn("Steam isn't running", verdicts["install-fresh"].reason)
        self.assertNotIn("WOT-CELL-RUN", fake.markers())
        self.assertNotIn("WOT-CELL-STEAM-SHUTDOWN", fake.markers())

    def test_fresh_must_add_through_the_running_steam(self):
        fake = FakeDeck(logs=[install_log(cell.FALLBACK_ROUTE)], snapshots=[snapshot()])
        verdicts, _ = run(fake, cases=("fresh",))
        self.assertFalse(verdicts["install-fresh"].ok)
        self.assertIn("step 3", verdicts["install-fresh"].reason)

    def test_fallback_refuses_without_the_flag(self):
        fake = FakeDeck()
        verdicts, _ = run(fake, cases=("fallback",))
        self.assertFalse(verdicts["install-fallback"].ok)
        self.assertIn("--allow-steam-shutdown", verdicts["install-fallback"].reason)
        self.assertNotIn("WOT-CELL-STEAM-SHUTDOWN", fake.markers())
        self.assertNotIn("WOT-CELL-RUN", fake.markers())

    def test_fallback_with_the_flag_shuts_steam_down_and_judges_the_append(self):
        fake = FakeDeck(logs=[install_log(cell.FALLBACK_ROUTE)],
                        snapshots=[snapshot(steam=False)])
        verdicts, printed = run(fake, cases=("fallback",), allow_steam_shutdown=True)
        self.assertTrue(verdicts["install-fallback"].ok, verdicts)
        markers = fake.markers()
        self.assertLess(markers.index("WOT-CELL-STEAM-SHUTDOWN"), markers.index("WOT-CELL-RUN"))
        self.assertIn('--target "$HOME/wot-install-cell/fallback/WoT Reborn"',
                      fake.script("WOT-CELL-RUN"))
        self.assertTrue(any("Steam was shut down" in line for line in printed))

    def test_cases_run_in_order_with_fallback_last(self):
        fake = FakeDeck(logs=[install_log("steamos-add-to-steam"),
                              install_log(cell.FALLBACK_ROUTE, appid=APPID + 1)],
                        snapshots=[snapshot(), snapshot(appid=APPID + 1, steam=False)])
        verdicts, _ = run(fake, cases=("fallback", "fresh"), allow_steam_shutdown=True)
        self.assertEqual(list(verdicts), ["install-fresh", "install-fallback", "cleanup"])
        self.assertTrue(all(v.ok for v in verdicts.values()), verdicts)
        cleanup = fake.script("WOT-CELL-CLEANUP")
        self.assertIn(f"compatdata/{APPID}\"", cleanup)
        self.assertIn(f"compatdata/{APPID + 1}\"", cleanup)

    def test_keep_skips_cleanup(self):
        fake = FakeDeck(logs=[install_log("steamos-add-to-steam")], snapshots=[snapshot()])
        verdicts, printed = run(fake, cases=("fresh",), keep=True)
        self.assertEqual(list(verdicts), ["install-fresh"])
        self.assertNotIn("WOT-CELL-CLEANUP", fake.markers())
        self.assertTrue(any("--keep" in line for line in printed))

    def test_a_failed_run_is_judged_and_still_cleaned_up(self):
        fake = FakeDeck(logs=[install_log("steamos-add-to-steam", done=False,
                                          stopped="the download was damaged")],
                        snapshots=[snapshot()], run_status=1)
        verdicts, printed = run(fake, cases=("fresh",))
        self.assertFalse(verdicts["install-fresh"].ok)
        self.assertIn("the download was damaged", verdicts["install-fresh"].reason)
        self.assertIn("installer console text", printed)
        self.assertTrue(verdicts["cleanup"].ok)

    def test_the_cli_runs_the_cell(self):
        fake = FakeDeck()
        with tempfile.TemporaryDirectory() as temporary:
            local = Path(temporary) / "local.json"
            local.write_text(json.dumps({"deck_host": "deckhost.local"}))
            with mock.patch.object(deck, "SubprocessRunner", return_value=fake), \
                    mock.patch("sys.stdout"):
                code = deck.main(["--local-json", str(local), "install-cell",
                                  "--case", "fallback"])
        self.assertEqual(code, 1)
        # Refused before anything ran: no shutdown, and a kept scratch install stays.
        self.assertEqual(fake.markers(), ["WOT-CELL-GUARD"])


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"),
                     "the Deck scripts need bash and GNU coreutils")
class ScriptsInARealShellTests(unittest.TestCase):
    """The scripts themselves, run by bash against a scratch $HOME (no Deck)."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.home = Path(self.temporary.name)
        self.layout = cell.Layout()
        self.downloads = self.home / "Downloads" / cell.PINNED_INSTALLER_NAME
        self.downloads.parent.mkdir()
        self.downloads.write_bytes(b"original installer bytes")

    def tearDown(self):
        self.temporary.cleanup()

    def bash(self, script):
        result = subprocess.run(["bash", "-s"], input=script.encode(), capture_output=True,
                                env={**os.environ, "HOME": str(self.home)})
        return result.returncode, result.stdout.decode().strip()

    def test_guard_link_restore_and_cleanup(self):
        inode = self.downloads.stat().st_ino
        status, out = self.bash(cell.guard_script(self.layout))
        self.assertEqual(cell.parse_guard(out.encode()), (True, 24, inode))
        status, out = self.bash(cell.link_script(self.layout))
        self.assertEqual((status, out), (0, f"linked {inode} 2"))
        link = self.home / self.layout.scratch / "dl" / cell.PINNED_INSTALLER_NAME
        self.assertEqual(link.stat().st_ino, inode)
        # The program renames the link beside the game; then Downloads vanishes.
        target = self.home / self.layout.scratch / "WoT Reborn"
        target.mkdir()
        os.link(link, target / cell.PINNED_INSTALLER_NAME)
        link.unlink()
        self.downloads.unlink()
        self.assertEqual(self.bash(cell.guard_script(self.layout))[1], "missing")
        status, out = self.bash(cell.restore_script(self.layout, inode))
        self.assertEqual(status, 0, out)
        self.assertTrue(out.startswith("restored "), out)
        self.assertEqual(self.downloads.stat().st_ino, inode)
        self.assertEqual(self.downloads.read_bytes(), b"original installer bytes")
        # Nothing with that inode left in the scratch folder: restore says so.
        (target / cell.PINNED_INSTALLER_NAME).unlink()
        self.downloads.unlink()
        self.assertEqual(self.bash(cell.restore_script(self.layout, inode)), (1, "lost"))

    def test_cleanup_removes_only_the_scratch_folder_art_and_prefix(self):
        steam = self.home / ".local" / "share" / "Steam"
        grid = steam / "userdata" / "42" / "config" / "grid"
        grid.mkdir(parents=True)
        for leaf in cell.ART_LEAVES:
            (grid / leaf.format(appid=APPID)).write_bytes(b"art")
        (grid / "2524773648p.jpg").write_bytes(b"the owner's art")
        prefix = steam / "steamapps" / "compatdata" / str(APPID)
        (prefix / "pfx").mkdir(parents=True)
        (self.home / self.layout.scratch / "dl").mkdir(parents=True)
        status, out = self.bash(cell.cleanup_script(self.layout, {APPID: str(grid)}))
        self.assertEqual((status, out), (0, "cleaned"))
        self.assertEqual(sorted(path.name for path in grid.iterdir()), ["2524773648p.jpg"])
        self.assertFalse(prefix.exists())
        self.assertFalse((self.home / self.layout.scratch).exists())
        self.assertTrue(self.downloads.is_file())

    def test_repair_prep_deletes_one_game_file_and_marks_data(self):
        target = self.home / self.layout.scratch / "WoT Reborn"
        (target / "Game" / "WoT").mkdir(parents=True)
        (target / "WoT Reborn.exe").write_bytes(b"MZ")
        (target / "Game" / "WoT" / "b file.pak").write_bytes(b"12345")
        (target / "Game" / "WoT" / "c.pak").write_bytes(b"1")
        status, out = self.bash(cell.repair_prep_script(self.layout))
        self.assertEqual((status, out), (0, "deleted 5 Game/WoT/b file.pak"))
        self.assertFalse((target / "Game" / "WoT" / "b file.pak").exists())
        self.assertTrue((target / "Data" / cell.DATA_MARKER).is_file())
        status, out = self.bash(cell.fresh_prep_script(self.layout, "fresh"))
        self.assertEqual((status, out), (3, "the scratch target exists from an earlier run"))
        status, out = self.bash(cell.fresh_prep_script(self.layout, "fallback"))
        self.assertEqual((status, out), (0, "ready"))

    def test_the_snapshot_reads_the_scratch_install(self):
        target = self.home / self.layout.scratch / "WoT Reborn"
        (target / "Game").mkdir(parents=True)
        (target / "WoT Reborn.exe").write_bytes(b"MZ")
        (target / cell.PINNED_INSTALLER_NAME).write_bytes(b"123")
        status, out = self.bash(cell.snapshot_script(self.layout, "fresh", "", None, ""))
        self.assertEqual(status, 0, out)
        found = json.loads(out)
        self.assertEqual((found["exe"], found["game"], found["installer"], found["art"]),
                         (True, True, 3, {}))


class JudgeTests(unittest.TestCase):
    def test_the_owners_shortcut_is_never_a_scratch_shortcut(self):
        owner = next(iter(cell.PROTECTED_APP_IDS))
        log = cell.parse_install_log(install_log("existing entry reused", appid=owner))
        verdict = cell.judge("repair", 0, log, snapshot(appid=owner, probe=17, marker=True),
                             (17, "Game/x"))
        self.assertFalse(verdict.ok)
        self.assertIn("owner's own shortcut", verdict.reason)
        with self.assertRaises(deck.DeckError):
            cell.cleanup_script(cell.Layout(), {owner: ACCOUNT + "/config/grid"})

    def test_art_is_removed_only_from_a_steam_userdata_grid(self):
        fake = FakeDeck(logs=[install_log("steamos-add-to-steam", account="/etc")],
                        snapshots=[snapshot()])
        run(fake, cases=("fresh",))
        cleanup = fake.script("WOT-CELL-CLEANUP")
        self.assertNotIn("/etc", cleanup)
        self.assertIn(f"compatdata/{APPID}", cleanup)

    def test_repair_must_restore_the_file_and_keep_data(self):
        log = cell.parse_install_log(install_log("existing entry reused"))
        good = snapshot(probe=17, marker=True)
        self.assertTrue(cell.judge("repair", 0, log, good, (17, "Game/x")).ok)
        self.assertFalse(cell.judge("repair", 0, log, dict(good, probe=None),
                                    (17, "Game/x")).ok)
        self.assertFalse(cell.judge("repair", 0, log, dict(good, marker=False),
                                    (17, "Game/x")).ok)
        fresh_log = cell.parse_install_log(install_log("steamos-add-to-steam"))
        self.assertFalse(cell.judge("repair", 0, fresh_log, good, (17, "Game/x")).ok)

    def test_missing_art_or_installer_fails(self):
        log = cell.parse_install_log(install_log("steam-addnonsteamgame-url"))
        self.assertTrue(cell.judge("fresh", 0, log, snapshot()).ok)
        art = snapshot()["art"]
        art[f"{APPID}_hero.jpg"] = False
        self.assertIn("grid art missing", cell.judge("fresh", 0, log, snapshot(art=art)).reason)
        self.assertIn("original installer",
                      cell.judge("fresh", 0, log, snapshot(installer=None)).reason)
        self.assertIn("Steam was shut down",
                      cell.judge("fresh", 0, log, snapshot(steam=False)).reason)


if __name__ == "__main__":
    unittest.main()
