#!/usr/bin/env python3
"""Assemble the ``Install-WoT-Reborn.desktop`` release asset (stdlib only, Python >= 3.9).

    python3 Tools/deck/installer/build_desktop_entry.py --out Install-WoT-Reborn.desktop

The asset is a freedesktop Desktop Entry whose ``Exec`` runs a one-line
``python3 -I -c`` bootstrap with ``%k`` (the entry's own path). The rest rides
in comment lines, which are valid anywhere in a desktop entry:

``#WOT-BOOT <python>``
    The second-stage bootstrap, as readable Python source. The ``Exec`` line
    joins these lines and runs them. It checks the payload against
    ``#WOT-SHA256``, extracts it into a fresh ``tempfile.mkdtemp()``, runs
    ``install_wot_reborn.main()`` from there and removes the folder afterwards.
``#WOT-SHA256 <hex>``
    The SHA-256 of the payload zip, so a truncated download is reported in
    plain words instead of failing somewhere inside the program.
``#WOT-DATA <base64>``
    The payload: a zip of ``PAYLOAD_FILES``, 76 base64 characters per line.

The output is deterministic: entries are sorted, stored (not deflated, so zlib
versions cannot change the bytes) with a fixed timestamp and mode, and text
files are normalised to LF so a CRLF checkout builds the same bytes. The file
holds nothing release-specific: the program always installs the latest release.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import sys
import zipfile
from pathlib import Path
from typing import Iterable, List, Optional, Sequence

HERE = Path(__file__).resolve().parent
ASSET_NAME = "Install-WoT-Reborn.desktop"
PAYLOAD_FILES = (
    "install_wot_reborn.py",
    "vdf.py",
    "art/capsule_portrait.jpg",
    "art/capsule_wide.jpg",
    "art/hero.jpg",
    "art/logo.json",
    "art/logo.png",
)
TEXT_SUFFIXES = (".py", ".json")
ZIP_DATE = (1980, 1, 1, 0, 0, 0)
BASE64_WIDTH = 76

BOOT_MARKER = "#WOT-BOOT "
DATA_MARKER = "#WOT-DATA "
SHA_MARKER = "#WOT-SHA256 "

# The Exec value is one quoted argument plus %k. It deliberately contains none
# of the characters the Desktop Entry spec makes special inside a quoted
# argument (" ` $ \) and no % (field codes), so no escaping is needed at either
# level: the string-value unescape (\s \n \t \r \\) and the Exec quoting.
BOOTSTRAP_STAGE1 = (
    "import sys;"
    "from urllib.parse import unquote,urlsplit;"
    "p=sys.argv[1];"
    "p=unquote(urlsplit(p).path) if p.startswith('file:') else p;"
    "exec(''.join(l[" + str(len(BOOT_MARKER)) + ":] for l in open(p,encoding='utf-8') "
    "if l.startswith('" + BOOT_MARKER + "')),"
    "{'__name__':'wot_boot','ENTRY':p,'ARGS':sys.argv[2:]})"
)
EXEC_VALUE = 'python3 -I -c "' + BOOTSTRAP_STAGE1 + '" %k'

BOOTSTRAP_STAGE2 = '''\
import base64, hashlib, io, shutil, subprocess, sys, tempfile, zipfile
sys.dont_write_bytecode = True


def _wot_lines(marker):
    with open(ENTRY, encoding='utf-8') as handle:
        return [line[len(marker):].strip() for line in handle if line.startswith(marker)]


def _wot_fail(text):
    sys.stderr.write(text + '\\n')
    for argv in (['zenity', '--error', '--title=Install WoT Reborn', '--text=' + text],
                 ['kdialog', '--title', 'Install WoT Reborn', '--error', text],
                 ['notify-send', 'Install WoT Reborn', text]):
        try:
            if subprocess.call(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL) == 0:
                break
        except OSError:
            pass
    sys.exit(3)


try:
    _wot_zip = base64.b64decode(''.join(_wot_lines('#WOT-DATA ')), validate=True)
    _wot_ok = hashlib.sha256(_wot_zip).hexdigest() == ''.join(_wot_lines('#WOT-SHA256 '))
except (OSError, ValueError):
    _wot_ok = False
if not _wot_ok:
    _wot_fail('This installer file is incomplete or damaged. '
              'Download Install-WoT-Reborn.desktop again.')
_wot_dir = tempfile.mkdtemp(prefix='wot-reborn-install-')
try:
    with zipfile.ZipFile(io.BytesIO(_wot_zip)) as _wot_archive:
        _wot_archive.extractall(_wot_dir)
    sys.path.insert(0, _wot_dir)
    try:
        import install_wot_reborn
    except Exception:
        _wot_fail('The installer could not start here. It needs Python 3.9 or later.')
    _wot_status = install_wot_reborn.main(ARGS)
finally:
    shutil.rmtree(_wot_dir, ignore_errors=True)
sys.exit(_wot_status)
'''

HEADER = (
    "[Desktop Entry]",
    "Type=Application",
    "Version=1.5",
    "Name=Install WoT Reborn",
    "Comment=Installs WoT Reborn on this device and adds it to Steam",
    "Icon=system-software-install",
    "Exec=" + EXEC_VALUE,
    "Terminal=false",
    "#",
    "# WoT Reborn installer for SteamOS. Open this file in Desktop Mode.",
    "# The lines below hold the installer: a short Python bootstrap (#WOT-BOOT),",
    "# then a zip of the installer program and its Steam artwork (#WOT-DATA,",
    "# base64) with its SHA-256 (#WOT-SHA256).",
    "#",
)


def payload_bytes(source_dir: Path = HERE, files: Sequence[str] = PAYLOAD_FILES) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_STORED) as archive:
        for name in sorted(files):
            data = (source_dir / name).read_bytes()
            if name.endswith(TEXT_SUFFIXES):
                data = data.replace(b"\r\n", b"\n")
            info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_STORED
            archive.writestr(info, data)
    return buffer.getvalue()


def _chunks(text: str, width: int) -> Iterable[str]:
    for start in range(0, len(text), width):
        yield text[start:start + width]


def build_entry(source_dir: Path = HERE) -> bytes:
    payload = payload_bytes(source_dir)
    lines: List[str] = list(HEADER)
    lines.append(SHA_MARKER + hashlib.sha256(payload).hexdigest())
    for line in BOOTSTRAP_STAGE2.splitlines():
        if line.strip():
            lines.append(BOOT_MARKER + line.rstrip())
    lines.extend(DATA_MARKER + chunk
                 for chunk in _chunks(base64.b64encode(payload).decode("ascii"), BASE64_WIDTH))
    return ("\n".join(lines) + "\n").encode("utf-8")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Build " + ASSET_NAME)
    parser.add_argument("--out", type=Path, required=True, help="output file")
    parser.add_argument("--source", type=Path, default=HERE,
                        help="folder holding the program and art/ (default: this folder)")
    args = parser.parse_args(argv)
    data = build_entry(args.source)
    args.out.write_bytes(data)
    print("%s %d bytes sha256=%s" % (args.out, len(data), hashlib.sha256(data).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
