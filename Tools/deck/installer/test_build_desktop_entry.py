"""Tests for the Install-WoT-Reborn.desktop builder and its bootstrap.

The Exec test parses the built entry's Exec value with an implementation of
the Desktop Entry Specification's rules (written here, independent of the
builder), runs the resulting argv with this Python, and checks that the
bootstrap starts the program from a temp folder it removes afterwards.
"""
from __future__ import annotations

import base64
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.parse
import zipfile
from pathlib import Path
from typing import List, Tuple
from unittest import mock

import build_desktop_entry as bde
import install_wot_reborn

# --- Desktop Entry Specification 1.5 -----------------------------------------
# "Possible value types": the escape sequences \s, \n, \t, \r and \\ are
# supported for values of type string. "The Exec key": arguments are separated
# by a space; an argument containing a reserved character must be quoted in
# whole with double quotes, inside which ", `, $ and \ are escaped with a
# backslash; the string unescape applies before the quoting rule. Field codes
# are % plus a letter, %% is a literal %, codes not in the spec make the line
# invalid, and field codes must not be used inside a quoted argument.

RESERVED = set(" \t\n\"'\\><~|&;$*?#()`")
QUOTE_ESCAPABLE = '"`$\\'
STRING_ESCAPES = {"s": " ", "n": "\n", "t": "\t", "r": "\r", "\\": "\\"}
FIELD_CODES = set("fFuUick")
DEPRECATED_FIELD_CODES = set("dDnNvm")


class ExecError(ValueError):
    pass


def unescape_string_value(value: str) -> str:
    out = []
    i = 0
    while i < len(value):
        char = value[i]
        if char == "\\":
            if i + 1 >= len(value):
                raise ExecError("trailing backslash")
            following = value[i + 1]
            if following in STRING_ESCAPES:
                out.append(STRING_ESCAPES[following])
            elif following in QUOTE_ESCAPABLE:
                out.append("\\" + following)  # left for the quoting rule
            else:
                raise ExecError("invalid escape \\%s" % following)
            i += 2
            continue
        out.append(char)
        i += 1
    return "".join(out)


def split_exec(command: str) -> List[Tuple[bool, str]]:
    """(was_quoted, argument) pairs, with quoting undone."""
    args: List[Tuple[bool, str]] = []
    i, n = 0, len(command)
    while i < n:
        if command[i] == " ":
            i += 1
            continue
        if command[i] == '"':
            i += 1
            buf = []
            while True:
                if i >= n:
                    raise ExecError("unterminated quoted argument")
                char = command[i]
                if char == '"':
                    i += 1
                    break
                if char == "\\":
                    if i + 1 < n and command[i + 1] in QUOTE_ESCAPABLE:
                        buf.append(command[i + 1])
                        i += 2
                        continue
                    raise ExecError("backslash not followed by an escapable character")
                if char in "`$":
                    raise ExecError("unescaped %r inside quotes" % char)
                if char == "%":
                    raise ExecError("field code inside a quoted argument")
                buf.append(char)
                i += 1
            if i < n and command[i] != " ":
                raise ExecError("a quote must enclose the whole argument")
            args.append((True, "".join(buf)))
        else:
            start = i
            while i < n and command[i] != " ":
                if command[i] in RESERVED:
                    raise ExecError("reserved character %r in an unquoted argument" % command[i])
                i += 1
            args.append((False, command[start:i]))
    return args


def expand_exec(value: str, entry_path: str) -> List[str]:
    args = split_exec(unescape_string_value(value))
    if not args:
        raise ExecError("empty Exec")
    if "=" in args[0][1]:
        raise ExecError("the program may not contain '='")
    argv: List[str] = []
    for quoted, arg in args:
        if quoted:
            argv.append(arg)
            continue
        if arg == "%k":
            argv.append(entry_path)
            continue
        out = []
        i = 0
        dropped = False
        while i < len(arg):
            if arg[i] == "%":
                if i + 1 >= len(arg):
                    raise ExecError("lone %")
                code = arg[i + 1]
                if code == "%":
                    out.append("%")
                elif code in DEPRECATED_FIELD_CODES or code in "fFuUic":
                    dropped = True  # nothing to substitute in this launch
                elif code == "k":
                    out.append(entry_path)
                else:
                    raise ExecError("unknown field code %%%s" % code)
                i += 2
                continue
            out.append(arg[i])
            i += 1
        if out or not dropped:
            argv.append("".join(out))
    return argv


def desktop_entry_keys(text: str) -> dict:
    keys = {}
    group = None
    for line in text.split("\n"):
        if not line or line.startswith("#"):
            continue
        if line.startswith("["):
            group = line
            continue
        key, sep, value = line.partition("=")
        if not sep or group != "[Desktop Entry]":
            raise ExecError("not a key line: %r" % line)
        if key.strip() in keys:
            raise ExecError("duplicate key %s" % key)
        keys[key.strip()] = value.lstrip()
    return keys


class ExecParserSelfTests(unittest.TestCase):
    """The spec implementation above, checked on the spec's own examples."""

    def test_spec_rules(self):
        self.assertEqual(expand_exec('prog "a b" %k', "/x y.desktop"), ["prog", "a b", "/x y.desktop"])
        self.assertEqual(expand_exec(r'prog "\\\\" "\\$HOME"', "e"), ["prog", "\\", "$HOME"])
        self.assertEqual(expand_exec("prog 100%%", "e"), ["prog", "100%"])
        self.assertEqual(expand_exec("prog %f", "e"), ["prog"])
        for bad in ('prog a;b', 'prog "unterminated', 'prog "$x"', 'prog "%k"',
                    'prog %z', 'prog "a"b', 'pro=g', 'prog ~/x', r'prog \q'):
            with self.assertRaises(ExecError, msg=bad):
                expand_exec(bad, "e")


class DesktopEntryTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.data = bde.build_entry()
        self.text = self.data.decode("utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write_entry(self, data: bytes = None) -> Path:
        path = self.tmp / "Install-WoT-Reborn.desktop"
        path.write_bytes(self.data if data is None else data)
        return path

    def test_build_is_deterministic_and_line_ending_independent(self):
        self.assertEqual(bde.build_entry(), self.data)
        crlf = self.tmp / "crlf-source"
        shutil.copytree(str(bde.HERE / "art"), str(crlf / "art"))
        for name in bde.PAYLOAD_FILES:
            source = bde.HERE / name
            data = source.read_bytes()
            if name.endswith(bde.TEXT_SUFFIXES):
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            (crlf / name).write_bytes(data)
        self.assertEqual(bde.build_entry(crlf), self.data)

    def test_it_is_a_valid_desktop_entry(self):
        lines = self.text.split("\n")
        self.assertEqual(lines[0], "[Desktop Entry]")
        self.assertEqual(lines[-1], "")
        keys = desktop_entry_keys(self.text)
        self.assertEqual(keys["Type"], "Application")
        self.assertEqual(keys["Name"], "Install WoT Reborn")
        self.assertEqual(keys["Terminal"], "false")
        self.assertIn(keys.get("Icon", "system-software-install"), ("system-software-install",))
        self.assertNotIn("\r", self.text)
        self.assertTrue(self.text.isascii())
        for line in lines[1:-1]:
            self.assertTrue(line.startswith("#") or "=" in line, line)

    def test_payload_lines_are_marked_base64_at_most_76_wide(self):
        data_lines = [line for line in self.text.split("\n") if line.startswith(bde.DATA_MARKER)]
        self.assertTrue(data_lines)
        for line in data_lines:
            chunk = line[len(bde.DATA_MARKER):]
            self.assertLessEqual(len(chunk), 76)
            self.assertRegex(chunk, r"^[A-Za-z0-9+/=]+$")
        payload = base64.b64decode("".join(line[len(bde.DATA_MARKER):] for line in data_lines))
        sha_lines = [line for line in self.text.split("\n") if line.startswith(bde.SHA_MARKER)]
        self.assertEqual(sha_lines, [bde.SHA_MARKER + hashlib.sha256(payload).hexdigest()])
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            infos = archive.infolist()
        self.assertEqual([info.filename for info in infos], sorted(bde.PAYLOAD_FILES))
        for info in infos:
            self.assertEqual(info.date_time, bde.ZIP_DATE)
            self.assertEqual(info.compress_type, zipfile.ZIP_STORED)

    def test_payload_is_the_program_and_exactly_the_art_it_installs(self):
        # art/ also holds wordmark.png, make_steam_art.py's input; it never ships.
        self.assertTrue((bde.HERE / "art" / "wordmark.png").is_file())
        art = sorted("art/" + source for source, _ in install_wot_reborn.ART_FILES)
        self.assertEqual(sorted(bde.PAYLOAD_FILES),
                         sorted(["install_wot_reborn.py", "vdf.py"] + art))
        data_lines = [line for line in self.text.split("\n") if line.startswith(bde.DATA_MARKER)]
        payload = base64.b64decode("".join(line[len(bde.DATA_MARKER):] for line in data_lines))
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            names = archive.namelist()
        self.assertEqual(sorted(names), sorted(["install_wot_reborn.py", "vdf.py"] + art))
        self.assertFalse([name for name in names if "wordmark" in name])

    def test_exec_follows_the_spec_and_starts_the_program(self):
        entry = self.write_entry()
        value = desktop_entry_keys(self.text)["Exec"]
        argv = expand_exec(value, str(entry))
        self.assertEqual(argv[:3], ["python3", "-I", "-c"])
        self.assertEqual(argv[-1], str(entry))
        self.assertEqual(len(argv), 5)
        argv[0] = sys.executable
        result = subprocess.run(argv + ["--self-test"], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=120, cwd=str(self.tmp))
        out = result.stdout.decode("utf-8", "replace")
        self.assertEqual(result.returncode, 0, out + result.stderr.decode("utf-8", "replace"))
        match = re.search(re.escape(install_wot_reborn.SELF_TEST_MARKER) + r" (.+)", out)
        self.assertIsNotNone(match, out)
        program_dir = Path(match.group(1).strip())
        self.assertNotEqual(program_dir.resolve(), bde.HERE.resolve())
        self.assertFalse(program_dir.exists(), "the bootstrap must remove its temp folder")

    @unittest.skipIf(os.name == "nt", "file: URIs of drive paths are POSIX-only here")
    def test_exec_also_accepts_a_file_uri(self):
        entry = self.write_entry()
        value = desktop_entry_keys(self.text)["Exec"]
        uri = "file://" + urllib.parse.quote(str(entry))
        argv = expand_exec(value, uri)
        argv[0] = sys.executable
        result = subprocess.run(argv + ["--self-test"], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=120, cwd=str(self.tmp))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(install_wot_reborn.SELF_TEST_MARKER, result.stdout.decode())

    def test_a_damaged_file_is_reported_not_run(self):
        lines = self.data.split(b"\n")
        data_index = max(i for i, line in enumerate(lines) if line.startswith(b"#WOT-DATA "))
        damaged = b"\n".join(lines[:data_index] + lines[data_index + 1:])  # truncated download
        entry = self.write_entry(damaged)
        argv = expand_exec(desktop_entry_keys(self.text)["Exec"], str(entry))
        argv[0] = sys.executable
        no_tools = self.tmp / "empty-path"
        no_tools.mkdir()
        env = dict(os.environ)
        env["PATH"] = str(no_tools)  # no zenity/kdialog/notify-send: the message goes to stderr
        result = subprocess.run(argv + ["--self-test"], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=120, env=env, cwd=str(self.tmp))
        self.assertEqual(result.returncode, 3)
        self.assertNotIn(install_wot_reborn.SELF_TEST_MARKER, result.stdout.decode())
        self.assertIn("incomplete or damaged", result.stderr.decode())

    def test_cli_writes_the_asset(self):
        out = self.tmp / "out.desktop"
        with mock.patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(bde.main(["--out", str(out)]), 0)
        self.assertEqual(out.read_bytes(), self.data)


if __name__ == "__main__":
    unittest.main()
