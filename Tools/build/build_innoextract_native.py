#!/usr/bin/env python3
"""Build the vendored innoextract static library from source and place it.

One command for the vendor recipe:

1. resolve the pinned native inputs (Win64: vcpkg in manifest mode from
   ThirdParty/vcpkg, triplet x64-windows-static-md; Mac: Homebrew boost + xz),
2. configure ThirdParty/innoextract/1.9/cmake, which verifies the in-tree
   upstream archive, patch, recipe and toolchains before defining its target,
3. build, install into a fresh staging prefix, and
4. promote through stage_innoextract_native.py into lib/<Platform>/.

Two modes:

* maintainer (--record): the resolved dependency BYTES must match the
  vendor-manifest pins, and the new library digest is recorded in the manifest.
  Only the box that recorded the pins can pass this, because MSVC and vcpkg
  builds are not byte-identical across machines.
* contributor (default): the dependency VERSIONS must match, the library is
  placed, and the manifest is left untouched. The result is a working local
  library, not the reviewed artifact; `stage_innoextract_native.py
  verify-library` says which one is in place.

Most contributors want neither and should run fetch_innoextract_libs.py, which
downloads the reviewed library and checks it against the manifest pin.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform as host
import shutil
import subprocess
import sys
from typing import Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_innoextract_native as stage  # noqa: E402


REPO = Path(__file__).resolve().parents[2]
VENDOR = REPO / "ThirdParty" / "innoextract" / "1.9"
VCPKG_MANIFEST_ROOT = REPO / "ThirdParty" / "vcpkg"
VCPKG_TRIPLET = "x64-windows-static-md"
# Shadows the stock triplet of the same name to pin the v143 toolset.
VCPKG_OVERLAY_TRIPLETS = VCPKG_MANIFEST_ROOT / "triplets"
PLATFORMS = ("Win64", "Mac")
LIBRARY_NAMES = {"Win64": "wot_innoextract.lib", "Mac": "libwot_innoextract.a"}
TOOLCHAINS = {"Win64": "msvc-v143.cmake", "Mac": "apple-clang.cmake"}
VISUAL_STUDIO_CMAKE = Path(
    "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")


class BuildError(RuntimeError):
    """A missing tool or a failed build step."""


def host_platform() -> str:
    system = host.system()
    if system == "Windows":
        return "Win64"
    if system == "Darwin":
        return "Mac"
    raise BuildError(f"no innoextract recipe for {system}; only Win64 and Mac")


def _visual_studio_cmake() -> Optional[Path]:
    program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        return None
    result = subprocess.run(
        [str(vswhere), "-latest", "-products", "*", "-version", "[17.0,18.0)",
         "-property", "installationPath"],
        capture_output=True, text=True, check=False)
    for line in result.stdout.splitlines():
        candidate = Path(line.strip()) / VISUAL_STUDIO_CMAKE
        if line.strip() and candidate.is_file():
            return candidate
    return None


def find_cmake(platform: str, explicit: Optional[Path]) -> Path:
    if explicit is not None:
        if not explicit.is_file():
            raise BuildError(f"--cmake does not exist: {explicit}")
        return explicit
    on_path = shutil.which("cmake")
    if on_path:
        return Path(on_path)
    if platform == "Win64":
        bundled = _visual_studio_cmake()
        if bundled is not None:
            return bundled
    raise BuildError(
        "cmake was not found: put CMake 3.24+ on PATH, pass --cmake, or (Win64) "
        "install Visual Studio 2022's 'C++ CMake tools for Windows' component")


def find_vcpkg_root(explicit: Optional[Path]) -> Path:
    root = explicit or (Path(os.environ["VCPKG_ROOT"]) if os.environ.get("VCPKG_ROOT")
                        else None)
    if root is None:
        raise BuildError(
            "vcpkg was not found: clone https://github.com/microsoft/vcpkg, run its "
            "bootstrap-vcpkg.bat, then pass --vcpkg-root or set VCPKG_ROOT")
    if not (root / "vcpkg.exe").is_file():
        raise BuildError(f"{root} has no vcpkg.exe; run bootstrap-vcpkg.bat there first")
    return root


def plan(platform: str, *, cmake: Path, work: Path, stage_dir: Path,
         vcpkg_root: Optional[Path] = None,
         vcpkg_install_root: Optional[Path] = None) -> list[list[str]]:
    """The external commands, in order (promotion runs in-process after them)."""
    if platform not in PLATFORMS:
        raise BuildError(f"unknown platform {platform}")
    build = work
    toolchain = VENDOR / "cmake" / "toolchains" / TOOLCHAINS[platform]
    recipe_inputs = [
        f"-DWOT_INNOEXTRACT_ARCHIVE={VENDOR / 'upstream' / 'innoextract-1.9.tar.gz'}",
        f"-DWOT_INNOEXTRACT_PATCH={VENDOR / 'patches' / '0001-wot-library-adapter.patch'}",
        f"-DWOT_VENDOR_MANIFEST={VENDOR / 'vendor-manifest.json'}",
        f"-DCMAKE_INSTALL_PREFIX={stage_dir}",
    ]
    commands: list[list[str]] = []
    if platform == "Win64":
        if vcpkg_root is None:
            raise BuildError("Win64 needs a vcpkg root")
        installed = vcpkg_install_root or (vcpkg_root / "installed")
        commands.append([
            str(vcpkg_root / "vcpkg.exe"), "install",
            f"--x-manifest-root={VCPKG_MANIFEST_ROOT}",
            f"--x-install-root={installed}",
            f"--overlay-triplets={VCPKG_OVERLAY_TRIPLETS}",
            f"--triplet={VCPKG_TRIPLET}",
        ])
        commands.append([
            str(cmake), "-S", str(VENDOR / "cmake"), "-B", str(build),
            "-G", "Visual Studio 17 2022", "-A", "x64", "-T", "v143",
            # Single-configuration: the recipe's resolved-dependencies.json
            # names the linked library files, which differ per configuration.
            "-DCMAKE_CONFIGURATION_TYPES=Release",
            f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_root / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}",
            f"-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE={toolchain}",
            f"-DVCPKG_TARGET_TRIPLET={VCPKG_TRIPLET}",
            f"-DVCPKG_OVERLAY_TRIPLETS={VCPKG_OVERLAY_TRIPLETS}",
            "-DVCPKG_MANIFEST_MODE=OFF",
            f"-DVCPKG_INSTALLED_DIR={installed}",
            *recipe_inputs,
        ])
    else:
        commands.append([
            str(cmake), "-S", str(VENDOR / "cmake"), "-B", str(build), "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
            *recipe_inputs,
        ])
    commands.append([
        str(cmake), "--build", str(build), "--target", "wot_innoextract",
        "--config", "Release", "--clean-first", "--parallel",
    ])
    commands.append([
        str(cmake), "--install", str(build), "--config", "Release",
        "--prefix", str(stage_dir),
    ])
    return commands


def _run(command: Sequence[str]) -> None:
    print("+ " + subprocess.list2cmdline(list(command)), flush=True)
    result = subprocess.run(list(command), check=False)
    if result.returncode != 0:
        raise BuildError(f"step failed with exit {result.returncode}: {command[0]}")


def build(platform: str, *, record: bool, cmake: Path, work: Path, stage_dir: Path,
          vcpkg_root: Optional[Path], vcpkg_install_root: Optional[Path]) -> Path:
    # The stage must hold exactly the header and the library, so start empty.
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    for command in plan(platform, cmake=cmake, work=work, stage_dir=stage_dir,
                        vcpkg_root=vcpkg_root, vcpkg_install_root=vcpkg_install_root):
        _run(command)
    destination = VENDOR / "lib" / platform / LIBRARY_NAMES[platform]
    stage.promote(
        platform=platform,
        stage=stage_dir,
        header_destination=VENDOR / "include" / "wot" / "innoextract_adapter.h",
        library_destination=destination,
        manifest_path=VENDOR / "vendor-manifest.json",
        resolved_dependencies=work / "resolved-dependencies.json",
        check_input_content=record,
        record_library_digest=record,
    )
    return destination


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", choices=PLATFORMS, default=None,
                        help="default: this host")
    parser.add_argument("--record", action="store_true",
                        help="maintainer: require the pinned dependency bytes and "
                             "re-pin the library digest in vendor-manifest.json")
    parser.add_argument("--cmake", type=Path, default=None)
    parser.add_argument("--vcpkg-root", type=Path, default=None,
                        help="Win64: a vcpkg clone (default: $VCPKG_ROOT)")
    parser.add_argument("--vcpkg-install-root", type=Path, default=None,
                        help="Win64: where vcpkg installs the inputs "
                             "(default: <vcpkg-root>/installed)")
    parser.add_argument("--work", type=Path, default=None,
                        help="build directory (default: Build/ThirdPartyBuild/...)")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        platform = arguments.platform or host_platform()
        if platform != host_platform():
            raise BuildError(f"build {platform} on a {platform} host")
        cmake = find_cmake(platform, arguments.cmake)
        vcpkg_root = find_vcpkg_root(arguments.vcpkg_root) if platform == "Win64" else None
        work = arguments.work or (REPO / "Build" / "ThirdPartyBuild" / "innoextract"
                                  / "1.9" / platform)
        stage_dir = REPO / "Build" / "ThirdPartyStage" / "innoextract" / "1.9" / platform
        destination = build(platform, record=arguments.record, cmake=cmake, work=work,
                            stage_dir=stage_dir, vcpkg_root=vcpkg_root,
                            vcpkg_install_root=arguments.vcpkg_install_root)
    except (BuildError, stage.VendorError, OSError) as exc:
        print(f"build_innoextract_native: {exc}", file=sys.stderr)
        return 2
    if arguments.record:
        print(f"placed and re-pinned {destination}")
    else:
        print(f"placed {destination} (contributor build: vendor-manifest.json "
              "is unchanged, so this is not the reviewed artifact)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
