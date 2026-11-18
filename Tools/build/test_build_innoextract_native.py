from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_innoextract_native as builder


class BuildInnoextractNativeTests(unittest.TestCase):
    def test_win64_resolves_pinned_vcpkg_inputs_before_configuring(self):
        vcpkg = Path("C:/vcpkg")
        commands = builder.plan(
            "Win64", cmake=Path("cmake.exe"), work=Path("work"), stage_dir=Path("stage"),
            vcpkg_root=vcpkg)
        self.assertEqual([command[1] for command in commands],
                         ["install", "-S", "--build", "--install"])
        install, configure, build, install_stage = commands
        self.assertIn(f"--x-manifest-root={builder.VCPKG_MANIFEST_ROOT}", install)
        self.assertIn(f"--x-install-root={vcpkg / 'installed'}", install)
        self.assertIn("--triplet=x64-windows-static-md", install)
        # The in-repo overlay shadows the stock triplet to pin the v143 toolset.
        overlay = builder.VCPKG_MANIFEST_ROOT / "triplets"
        self.assertTrue((overlay / "x64-windows-static-md.cmake").is_file())
        self.assertIn(f"--overlay-triplets={overlay}", install)
        self.assertIn(f"-DVCPKG_OVERLAY_TRIPLETS={overlay}", configure)
        self.assertIn("-DCMAKE_CONFIGURATION_TYPES=Release", configure)
        self.assertIn("-DVCPKG_TARGET_TRIPLET=x64-windows-static-md", configure)
        self.assertIn(
            f"-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="
            f"{builder.VENDOR / 'cmake/toolchains/msvc-v143.cmake'}", configure)
        self.assertIn("-DVCPKG_MANIFEST_MODE=OFF", configure)
        self.assertIn(f"-DVCPKG_INSTALLED_DIR={vcpkg / 'installed'}", configure)
        self.assertIn("v143", configure)
        self.assertIn(
            f"-DWOT_VENDOR_MANIFEST={builder.VENDOR / 'vendor-manifest.json'}", configure)
        self.assertEqual(build[build.index("--target") + 1], "wot_innoextract")
        self.assertEqual(install_stage[install_stage.index("--prefix") + 1], "stage")

    def test_win64_install_root_can_be_isolated(self):
        commands = builder.plan(
            "Win64", cmake=Path("cmake.exe"), work=Path("work"), stage_dir=Path("stage"),
            vcpkg_root=Path("C:/vcpkg"), vcpkg_install_root=Path("D:/inputs"))
        self.assertIn(f"--x-install-root={Path('D:/inputs')}", commands[0])
        self.assertIn(f"-DVCPKG_INSTALLED_DIR={Path('D:/inputs')}", commands[1])

    def test_mac_configures_the_apple_toolchain_without_vcpkg(self):
        commands = builder.plan(
            "Mac", cmake=Path("cmake"), work=Path("work"), stage_dir=Path("stage"))
        configure = commands[0]
        self.assertEqual(configure[configure.index("-G") + 1], "Ninja")
        self.assertIn(
            f"-DCMAKE_TOOLCHAIN_FILE={builder.VENDOR / 'cmake/toolchains/apple-clang.cmake'}",
            configure)
        self.assertFalse(any("vcpkg" in argument.lower()
                             for command in commands for argument in command))

    def test_win64_requires_a_vcpkg_root(self):
        with self.assertRaisesRegex(builder.BuildError, "vcpkg root"):
            builder.plan("Win64", cmake=Path("cmake.exe"), work=Path("work"),
                         stage_dir=Path("stage"))

    def test_missing_tools_name_the_fix(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(builder.BuildError, "bootstrap-vcpkg"):
                builder.find_vcpkg_root(Path(temporary))
            with self.assertRaisesRegex(builder.BuildError, "--cmake does not exist"):
                builder.find_cmake("Win64", Path(temporary) / "cmake.exe")


if __name__ == "__main__":
    unittest.main()
