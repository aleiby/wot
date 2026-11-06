"""Adversarial tests for the public-tree leak guard; all fixtures are synthetic."""
from __future__ import annotations

import contextlib
import hashlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import leak_guard  # noqa: E402

PERSON = "jd" + "oe"
POSIX_HOME = "/Users/%s/" % PERSON
PRIVATE_WINDOWS_HOME = r"C:\Users\%s\x" % "fireo"
PRIVATE_WINDOWS_ROOT = r"G:\%s\y" % "wot_scratch"
PRIVATE_UNC = r"\\%s\share\x" % "buildbox"
PRIVATE_ADDRESS = "10." + "20.4.1"
PRIVATE_V6 = "fd00" + "::1"
PRIVATE_EMAIL = "person" + "@corp.test"


class LeakGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, relative: str, content, binary: bool = False) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if binary:
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8")
        return path

    def _rules(self, **kwargs):
        violations, count = leak_guard.check(leak_guard.tree_files(self.root), **kwargs)
        return sorted((v.rule, v.path) for v in violations), count

    def test_clean_tree_passes(self):
        self._write("Source/WoT/A.cpp", "int a;\n")
        self._write("Tools/x.py", "print('hi')\n")
        self._write("docs/network.md",
                    "Examples: 192.0.2.10 and 2001:db8::20.\n")
        self._write("docs/contact.md",
                    "noreply@project.test user@example.invalid\n")
        self._write(
            "docs/windows-paths.md",
            r"C:/Program Files/WoT C:\Windows C:/WoT D:/x "
            r"C:\Users\Public\x C:\Users\Default\x "
            r"C:\Users\<placeholder>\x",
        )
        self.assertEqual(self._rules(), ([], 5))

    def test_private_windows_paths_and_named_unc_hosts_fail(self):
        text = "%s %s %s" % (
            PRIVATE_WINDOWS_HOME, PRIVATE_WINDOWS_ROOT, PRIVATE_UNC,
        )
        self.assertEqual(leak_guard.machine_path_hits(text, []), [
            "windows-home " + PRIVATE_WINDOWS_HOME.rsplit("\\", 1)[0],
            "windows-private-root " + PRIVATE_WINDOWS_ROOT,
            "windows-unc " + PRIVATE_UNC,
        ])

    def test_escaped_source_fragments_are_not_unc_paths(self):
        source = (r'TEXT("\\\\.\\pipe\\name") "D:\\path\\to\\engine" '
                  r'b"{app}\\Data\\Bad\x1fName.txt" r"\\foreign\ddc"')
        self.assertEqual(leak_guard.machine_path_hits(source, []), [])

    def test_path_and_extension_rules_are_case_insensitive(self):
        self._write("Tools/renamed.uc", "x")
        self._write("Tools/Local.json", "{}")
        self._write("Tools/test/net/Nodes.JSON", "{}")
        self._write("Source/config/NODES.json", "{}")
        self._write("Tools/original/readme", "x")
        rules, count = self._rules()
        self.assertEqual(count, 5)
        self.assertIn(("retail-extension", "Tools/renamed.uc"), rules)
        self.assertIn(("local-config", "Tools/Local.json"), rules)
        self.assertIn(("local-config", "Tools/test/net/Nodes.JSON"), rules)
        self.assertIn(("local-config", "Source/config/NODES.json"), rules)
        self.assertIn(("private-root", "Tools/original/readme"), rules)

    def test_content_is_scanned_without_a_text_suffix(self):
        self._write("Tools/release.pem",
                    "-----BEGIN " + "PRIVATE KEY-----\nsynthetic\n")
        rules, _ = self._rules()
        self.assertIn(("key-material", "Tools/release.pem"), rules)

    def test_utf16_bom_text_is_scanned(self):
        self._write("Tools/renamed.dat",
                    ("checkout=" + POSIX_HOME + "project\n").encode("utf-16"),
                    binary=True)
        rules, _ = self._rules()
        self.assertIn(("machine-path", "Tools/renamed.dat"), rules)
        self.assertNotIn(("unapproved-binary", "Tools/renamed.dat"), rules)

    def test_binary_requires_exact_path_and_hash(self):
        data = b"\x89PNG\r\n\x1a\n"
        self._write("Tools/frame.png", data, binary=True)
        rules, _ = self._rules()
        self.assertIn(("unapproved-binary", "Tools/frame.png"), rules)

        approved = {"Tools/frame.png": hashlib.sha256(data).hexdigest()}
        self.assertEqual(self._rules(binary_allowlist=approved), ([], 1))
        wrong_path = {"Tools/other.png": hashlib.sha256(data).hexdigest()}
        rules, _ = self._rules(binary_allowlist=wrong_path)
        self.assertIn(("unapproved-binary", "Tools/frame.png"), rules)
        self.assertIn(("stale-binary-allowlist", "Tools/other.png"), rules)

    def test_key_container_magic_overrides_binary_allowlist(self):
        data = b"openssh-" + b"key-v1\x00synthetic"
        self._write("Tools/blob.bin", data, binary=True)
        approved = {"Tools/blob.bin": hashlib.sha256(data).hexdigest()}
        rules, _ = self._rules(binary_allowlist=approved)
        self.assertIn(("key-material", "Tools/blob.bin"), rules)

    def test_email_private_marker_ip_and_windows_path_rules(self):
        self._write(
            "Tools/notes.any",
            "owner=%s user=secret-user host=buildbox "
            "ip=%s v6=%s checkout=%s\n"
            % (PRIVATE_EMAIL, PRIVATE_ADDRESS, PRIVATE_V6, PRIVATE_WINDOWS_ROOT),
        )
        rules, _ = self._rules(private_users=["secret-user"],
                               private_hosts=["buildbox"])
        for rule in ("email", "private-user", "private-host", "private-ip",
                     "machine-path"):
            self.assertIn((rule, "Tools/notes.any"), rules)

    def test_documented_email_allowlist_and_url_userinfo(self):
        text = ("Daniel Scharrer <daniel@constexpr.org> "
                "noreply@project.test person@example.com user@example.invalid "
                "https://user@github.com/object\n")
        self.assertEqual(leak_guard.email_hits(text), [])

    def test_scale_suffixed_asset_names_are_not_email(self):
        self.assertEqual(
            leak_guard.email_hits("background@2x.png icon_512x512@2x.tiff\n"), [])
        address = "dev" + "@2x.org"
        self.assertEqual(leak_guard.email_hits(address + "\n"), [address])

    def test_player_is_a_placeholder_home(self):
        self._write("Tools/paths.txt",
                    "C:/Users/player/AppData and /Users/player/Library\n")
        self.assertEqual(self._rules(), ([], 1))

    def test_guard_source_does_not_trigger_its_own_rules(self):
        violations = leak_guard.check_file(
            "Tools/public/leak_guard.py", Path(leak_guard.__file__),
            leak_guard.DEFAULT_MAX_BYTES, [],
        )
        self.assertEqual([(v.rule, v.detail) for v in violations], [])

    def test_staged_mode_reads_index_blob_not_worktree(self):
        repo = self.root / "repo"
        repo.mkdir()

        def git(*args):
            return subprocess.run(["git", "-C", str(repo), *args], check=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        git("init", "-q")
        path = repo / "Tools" / "notes.md"
        path.parent.mkdir()
        path.write_text("private=" + POSIX_HOME + "project\n")
        git("add", "-A")
        path.write_text("safe worktree replacement\n")

        files = leak_guard.staged_files(repo)
        violations, count = leak_guard.check(files)
        self.assertEqual(count, 1)
        self.assertIn(("machine-path", "Tools/notes.md"),
                      [(v.rule, v.path) for v in violations])

    def test_staged_mode_rejects_index_symlink(self):
        repo = self.root / "repo"
        repo.mkdir()
        subprocess.run(["git", "-C", str(repo), "init", "-q"], check=True)
        oid = subprocess.run(
            ["git", "-C", str(repo), "hash-object", "-w", "--stdin"],
            input=b"target\n", check=True, stdout=subprocess.PIPE,
        ).stdout.decode().strip()
        subprocess.run([
            "git", "-C", str(repo), "update-index", "--add", "--cacheinfo",
            "120000,%s,Tools/link" % oid,
        ], check=True)
        with self.assertRaises(leak_guard.GuardError):
            leak_guard.staged_files(repo)

    @unittest.skipIf(os.name == "nt", "POSIX permissions fixture")
    def test_unreadable_directory_is_fatal(self):
        directory = self.root / "private"
        directory.mkdir()
        (directory / "secret.txt").write_text("x")
        directory.chmod(0)
        try:
            with self.assertRaises(leak_guard.GuardError):
                list(leak_guard.tree_files(self.root))
        finally:
            directory.chmod(0o700)

    @unittest.skipIf(not hasattr(os, "symlink"), "symlinks unavailable")
    def test_symlink_is_fatal(self):
        target = self._write("safe.txt", "safe\n")
        link = self.root / "linked.txt"
        try:
            link.symlink_to(target)
        except OSError as exc:
            self.skipTest("cannot create symlink: %s" % exc)
        with self.assertRaises(leak_guard.GuardError):
            list(leak_guard.tree_files(self.root))

    def test_cli_exit_codes(self):
        self._write("Tools/x.py", "ok\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(leak_guard.run(["--tree", str(self.root)]), 0)
            self._write("Tools/y.uc", "x")
            self.assertEqual(leak_guard.run(["--tree", str(self.root)]), 1)
            self.assertEqual(leak_guard.run(["--tree", str(self.root / "missing")]), 2)
        self.assertIn("retail-extension: Tools/y.uc", out.getvalue())

    def test_cli_explains_how_to_clean_generated_python_bytecode(self):
        self._write("Tools/__pycache__/helper.pyc", b"\x00synthetic", binary=True)
        stderr = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(stderr):
            self.assertEqual(leak_guard.run(["--tree", str(self.root)]), 1)
        self.assertIn("Remove generated __pycache__", stderr.getvalue())
        self.assertIn("PYTHONDONTWRITEBYTECODE=1", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
