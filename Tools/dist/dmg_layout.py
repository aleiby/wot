#!/usr/bin/env python3
"""Write the Finder layout for the guarded Mac DMG without opening Finder."""
from __future__ import annotations

import hashlib
import importlib
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, Optional, Sequence

APP_NAME = "WoT.app"
WINDOW = (660, 400)
ICON_POSITIONS = {APP_NAME: (180, 190), "Applications": (480, 190)}

VENDOR_DIR = (Path(__file__).resolve().parents[2]
              / "ThirdParty" / "dmgbuild" / "1.6.5")
WHEELS = (
    ("dmgbuild-1.6.5-py3-none-any.whl",
     "e19ab8c5e8238e6455d9ccb9175817be7fd62b9cdd1eef20f63dd88e0ec469ab"),
    ("ds_store-1.3.1-py3-none-any.whl",
     "fbacbb0bd5193ab3e66e5a47fff63619f15e374ffbec8ae29744251a6c8f05b5"),
    ("mac_alias-2.2.2-py3-none-any.whl",
     "504ab8ac546f35bbd75ad014d6ad977c426660aa721f2cd3acf3dc2f664141bd"),
)

Runner = Callable[..., subprocess.CompletedProcess]


class LayoutError(Exception):
    pass


def _run(command: Sequence[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(list(command), capture_output=True, text=True, **kwargs)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def load_vendored_dmgbuild():
    """Hash-check and zipimport the pinned dmgbuild dependency set."""
    wheel_paths = []
    for name, expected in WHEELS:
        path = VENDOR_DIR / name
        try:
            actual = _sha256(path)
        except OSError as exc:
            raise LayoutError(f"cannot read vendored wheel {path}: {exc}") from exc
        if actual != expected:
            raise LayoutError(
                f"vendored wheel {path} sha256 differs: {actual} != {expected}")
        wheel_paths.append(path)

    for path in reversed(wheel_paths):
        value = str(path)
        if value not in sys.path:
            sys.path.insert(0, value)
    original_mac_ver = platform.mac_ver
    if sys.platform != "darwin":
        # dmgbuild 1.6.5 assumes platform.mac_ver()[0] is non-empty while its
        # module is imported.  The layout tests only use its portable record
        # writers on non-Mac hosts, so provide a neutral version during import.
        platform.mac_ver = lambda: ("0.0.0", ("", "", ""), "")
    try:
        try:
            module = importlib.import_module("dmgbuild")
            importlib.import_module("ds_store")
            importlib.import_module("mac_alias")
        except (ImportError, OSError, ValueError) as exc:
            raise LayoutError(
                f"cannot import the verified dmgbuild wheels: {exc}") from exc
    finally:
        platform.mac_ver = original_mac_ver

    # zipimport spells a member path with the host separator (backslash on
    # Windows, where the layout tests also run).
    allowed = tuple(str(path) + separator for path in wheel_paths
                    for separator in {"/", os.sep})
    for name in ("dmgbuild", "ds_store", "mac_alias"):
        loaded = sys.modules[name]
        origin = str(getattr(loaded, "__file__", ""))
        if not origin.startswith(allowed):
            raise LayoutError(
                f"{name} was not imported from the verified vendored wheels: {origin}")
    return module


def _require_success(result: subprocess.CompletedProcess, what: str) -> None:
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise LayoutError(f"{what} failed ({result.returncode}): {detail}")


def _set_custom_volume_icon(mountpoint: Path, runner: Runner) -> None:
    command = ["SetFile", "-a", "C", str(mountpoint)]
    try:
        result = runner(command)
    except FileNotFoundError:
        result = None
    if result is not None:
        _require_success(result, "SetFile custom volume icon flag")
        return

    finder_info = bytearray(32)
    finder_info[8:10] = b"\x04\x00"
    result = runner(["xattr", "-wx", "com.apple.FinderInfo",
                     bytes(finder_info).hex(), str(mountpoint)])
    _require_success(result, "xattr custom volume icon flag")


def _background_alias_bytes(target: Path) -> bytes:
    """Classic alias record for the background, as Finder reads it.

    The record stores 32-bit catalog node IDs. That holds on the HFS+ image the
    builder writes into, but not on APFS (IDs exceed 32 bits), so unit tests
    that fake the volume with an APFS directory inject a stub instead.
    """
    load_vendored_dmgbuild()
    from mac_alias import Alias
    return Alias.for_file(str(target)).to_bytes()


def write_layout(mountpoint: Path, *, app_name: str = APP_NAME,
                 background: Path, volume_icon: Path,
                 runner: Runner = _run,
                 alias_bytes: Optional[Callable[[Path], bytes]] = None
                 ) -> None:
    """Copy Finder assets and write the volume's deterministic .DS_Store."""
    load_vendored_dmgbuild()
    from ds_store import DSStore

    mountpoint = Path(mountpoint)
    background = Path(background)
    volume_icon = Path(volume_icon)
    background_dir = mountpoint / ".background"
    background_dir.mkdir()
    background_target = background_dir / "background.tiff"
    shutil.copyfile(str(background), str(background_target))
    shutil.copyfile(str(volume_icon), str(mountpoint / ".VolumeIcon.icns"))
    _set_custom_volume_icon(mountpoint, runner)

    bounds = "{{100, 100}, {%d, %d}}" % WINDOW
    bwsp = {
        "ShowStatusBar": False,
        "WindowBounds": bounds,
        "ContainerShowSidebar": False,
        "PreviewPaneVisibility": False,
        "SidebarWidth": 180,
        "ShowTabView": False,
        "ShowToolbar": False,
        "ShowPathbar": False,
        "ShowSidebar": False,
    }
    icvp = {
        "viewOptionsVersion": 1,
        "backgroundType": 2,
        "backgroundColorRed": 1.0,
        "backgroundColorGreen": 1.0,
        "backgroundColorBlue": 1.0,
        "backgroundImageAlias": (alias_bytes or _background_alias_bytes)(
            background_target),
        "gridOffsetX": 0.0,
        "gridOffsetY": 0.0,
        "gridSpacing": 100.0,
        "arrangeBy": "none",
        "showIconPreview": False,
        "showItemInfo": False,
        "labelOnBottom": True,
        "textSize": 13.0,
        "iconSize": 128.0,
        "scrollPositionX": 0.0,
        "scrollPositionY": 0.0,
    }
    positions = {
        app_name: ICON_POSITIONS[APP_NAME],
        "Applications": ICON_POSITIONS["Applications"],
    }
    with DSStore.open(str(mountpoint / ".DS_Store"), "w+") as store:
        store["."]["vSrn"] = ("long", 1)
        store["."]["bwsp"] = bwsp
        store["."]["icvp"] = icvp
        store["."]["icvl"] = ("type", b"icnv")
        for name, position in positions.items():
            store[name]["Iloc"] = position
