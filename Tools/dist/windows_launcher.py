"""Build the stable Win64 first-install launcher with the staged game's icon."""

from __future__ import annotations

import ctypes
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


class LauncherBuildError(RuntimeError):
    pass


SOURCE_ROOT = Path(__file__).with_name("windows_launcher")


def _vcvars64() -> Path:
    candidates = []
    for base in (Path(os.environ.get("ProgramFiles", r"C:\Program Files")),
                 Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))):
        candidates.extend((base / "Microsoft Visual Studio").glob(
            "*/*/VC/Auxiliary/Build/vcvars64.bat"))
    if not candidates:
        raise LauncherBuildError("Visual Studio x64 build tools are not installed")
    return sorted(candidates, reverse=True)[0]


def _run_toolchain(arguments: list[str], cwd: Path) -> None:
    command = (f'cmd.exe /d /s /c ""{_vcvars64()}" >nul && '
               + subprocess.list2cmdline(arguments) + '"')
    result = subprocess.run(command, cwd=cwd,
                            capture_output=True, text=True)
    if result.returncode:
        raise LauncherBuildError(
            f"launcher toolchain failed ({result.returncode}):\n"
            f"{result.stdout}{result.stderr}")


def _resource_bytes(module, resource_type: int, name) -> bytes:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_void_p]
    kernel32.FindResourceW.restype = ctypes.c_void_p
    kernel32.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.SizeofResource.restype = ctypes.c_uint32
    kernel32.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.LoadResource.restype = ctypes.c_void_p
    kernel32.LockResource.argtypes = [ctypes.c_void_p]
    kernel32.LockResource.restype = ctypes.c_void_p
    resource = kernel32.FindResourceW(
        module, ctypes.c_void_p(int(name)), ctypes.c_void_p(resource_type))
    if not resource:
        raise LauncherBuildError("the staged game has no usable icon resource")
    size = kernel32.SizeofResource(module, resource)
    loaded = kernel32.LoadResource(module, resource)
    address = kernel32.LockResource(loaded)
    if not size or not address:
        raise LauncherBuildError("the staged game icon resource is unreadable")
    return ctypes.string_at(address, size)


def _copy_game_icon(game_executable: Path, launcher: Path) -> None:
    if os.name != "nt":
        raise LauncherBuildError("the Windows launcher must be built on Windows")
    game_executable = Path(game_executable).resolve()
    launcher = Path(launcher).resolve()
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p,
                                        ctypes.c_uint32]
    kernel32.LoadLibraryExW.restype = ctypes.c_void_p
    kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
    module = kernel32.LoadLibraryExW(str(game_executable), None, 0x22)
    if not module:
        raise LauncherBuildError(f"cannot open staged game resources: {game_executable}")
    names = []
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                      ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_void_p)

    @callback_type
    def collect(_module, _kind, name, _parameter):
        names.append(name)
        return 0

    try:
        kernel32.EnumResourceNamesW.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                                callback_type, ctypes.c_void_p]
        kernel32.EnumResourceNamesW(module, ctypes.c_void_p(14), collect, None)
        if not names:
            raise LauncherBuildError("the staged game executable has no icon")
        group_name = int(names[0])
        if group_name > 0xffff:
            raise LauncherBuildError("the staged game uses a named icon resource")
        group = _resource_bytes(module, 14, group_name)
        if len(group) < 6:
            raise LauncherBuildError("the staged game icon group is malformed")
        count = struct.unpack_from("<H", group, 4)[0]
        if len(group) != 6 + count * 14:
            raise LauncherBuildError("the staged game icon group is malformed")
        icons = [(struct.unpack_from("<H", group, 6 + index * 14 + 12)[0],
                  None) for index in range(count)]
        icons = [(identifier, _resource_bytes(module, 3, identifier))
                 for identifier, _ in icons]
    finally:
        kernel32.FreeLibrary(module)

    kernel32.BeginUpdateResourceW.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
    kernel32.BeginUpdateResourceW.restype = ctypes.c_void_p
    kernel32.UpdateResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.c_void_p, ctypes.c_ushort,
                                         ctypes.c_void_p, ctypes.c_uint32]
    kernel32.UpdateResourceW.restype = ctypes.c_int
    kernel32.EndUpdateResourceW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    kernel32.EndUpdateResourceW.restype = ctypes.c_int
    update = kernel32.BeginUpdateResourceW(str(launcher), False)
    if not update:
        raise LauncherBuildError("cannot begin launcher icon update")
    group_buffer = ctypes.create_string_buffer(group)
    ok = bool(kernel32.UpdateResourceW(
        update, ctypes.c_void_p(14), ctypes.c_void_p(group_name), 0,
        group_buffer, len(group)))
    for identifier, data in icons:
        icon_buffer = ctypes.create_string_buffer(data)
        ok = ok and bool(kernel32.UpdateResourceW(
            update, ctypes.c_void_p(3), ctypes.c_void_p(identifier), 0,
            icon_buffer, len(data)))
    if not kernel32.EndUpdateResourceW(update, not ok) or not ok:
        raise LauncherBuildError("cannot copy the staged game icon to the launcher")


def build_launcher(destination: Path, game_executable: Path, configuration: str,
                   *, test_mode: bool = False, copy_icon: bool = True) -> Path:
    """Build a deterministic, fixed-contract x64 launcher at *destination*."""
    if configuration not in {"Test", "Shipping"}:
        raise LauncherBuildError(
            f"unsupported guarded launcher configuration: {configuration}")
    destination = Path(destination)
    game_executable = Path(game_executable)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wot-launcher-",
                                     dir=destination.parent) as temporary:
        temporary_path = Path(temporary)
        resource = temporary_path / "launcher.res"
        output = temporary_path / "WoT Reborn.exe"
        _run_toolchain(["rc.exe", "/nologo", f"/fo{resource}",
                        str(SOURCE_ROOT / "WoTRebornLauncher.rc")], SOURCE_ROOT)
        defines = [
            "/DWOT_LAUNCHER_CONFIGURATION_" + configuration.upper() + "=1"]
        if test_mode:
            defines.append("/DWOT_LAUNCHER_TEST=1")
        _run_toolchain([
            "cl.exe", "/nologo", "/O1", "/GS-", "/GR-", "/EHs-c-",
            "/DUNICODE", "/D_UNICODE", *defines,
            str(SOURCE_ROOT / "WoTRebornLauncher.cpp"), str(resource),
            f"/Fe:{output}", "/link", "/NOLOGO", "/NODEFAULTLIB",
            "/ENTRY:wWinMainCRTStartup", "/SUBSYSTEM:WINDOWS,6.02", "/MACHINE:X64",
            "/DYNAMICBASE", "/NXCOMPAT", "/HIGHENTROPYVA", "/BREPRO",
            "kernel32.lib", "user32.lib",
        ], temporary_path)
        if copy_icon:
            _copy_game_icon(game_executable, output)
        os.replace(output, destination)
    return destination


def build_test_stub(destination: Path) -> Path:
    """Build the no-runtime child used only by the launcher contract tests."""
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    _run_toolchain([
        "cl.exe", "/nologo", "/O1", "/GS-", "/GR-", "/EHs-c-",
        "/DUNICODE", "/D_UNICODE",
        str(SOURCE_ROOT / "WoTLauncherTestStub.cpp"),
        f"/Fe:{destination}", "/link", "/NOLOGO", "/NODEFAULTLIB",
        "/ENTRY:wWinMainCRTStartup", "/SUBSYSTEM:WINDOWS,6.02", "/MACHINE:X64",
        "/DYNAMICBASE", "/NXCOMPAT", "/HIGHENTROPYVA", "/BREPRO",
        "kernel32.lib",
    ], destination.parent)
    return destination
