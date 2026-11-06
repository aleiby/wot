from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_innoextract_link as checker


MAC_MAP = """# Object files:
[  1] linker synthesized
[ 12] {expected_archive}(adapter.cpp.o)
# Symbols:
0x1000 0x20 [ 12] _wot_innoextract_open
0x1020 0x20 [ 12] _wot_innoextract_entry_count
0x1040 0x20 [ 12] _wot_innoextract_entry_at
0x1060 0x20 [ 12] _wot_innoextract_extract_entry
0x1080 0x20 [ 12] _wot_innoextract_description
0x10a0 0x20 [ 12] _wot_innoextract_close
0x10c0 0x20 [ 12] _wot_innoextract_last_error
"""


class CheckInnoextractLinkTests(unittest.TestCase):
    def _files(self, map_text=MAC_MAP, binary=b"MachO", archive=b"!<arch>\nobjects"):
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        expected = root / "libwot_innoextract.a"
        executable = root / "runner"
        link_map = root / "runner.map"
        expected.write_bytes(archive)
        executable.write_bytes(binary)
        link_map.write_text(
            map_text.format(expected_archive=expected.resolve().as_posix()),
            encoding="utf-8",
        )
        return temporary, executable, link_map, expected

    def _check(self, *, nm_stdout="", nm_returncode=0, **arguments):
        completed = SimpleNamespace(
            returncode=nm_returncode, stdout=nm_stdout,
            stderr="mock nm failure" if nm_returncode else "",
        )
        with mock.patch.object(checker.subprocess, "run", return_value=completed):
            checker.check_link(nm_tool="llvm-nm", **arguments)

    def test_accepts_fixed_static_archive_and_complete_adapter_surface(self):
        temporary, binary, link_map, expected = self._files()
        with temporary:
            self._check(
                platform="Mac",
                binary=binary,
                link_map=link_map,
                expected_archive=expected,
                forbid_runtime_extractor=True,
                forbid_shell=True,
            )

    def test_rejects_missing_symbol_and_alternate_archive(self):
        altered = MAC_MAP.replace("_wot_innoextract_close", "_not_the_close_symbol")
        altered += "[ 13] /tmp/libwot_innoextract_debug.a(adapter.o)\n"
        temporary, binary, link_map, expected = self._files(altered)
        with temporary, self.assertRaises(checker.LinkCheckError):
            self._check(
                platform="Mac",
                binary=binary,
                link_map=link_map,
                expected_archive=expected,
                forbid_runtime_extractor=True,
                forbid_shell=True,
            )

    def test_rejects_expected_basename_from_the_wrong_path(self):
        altered = MAC_MAP.replace(
            "{expected_archive}", "/tmp/imposter/libwot_innoextract.a"
        )
        temporary, binary, link_map, expected = self._files(altered)
        with temporary, self.assertRaises(checker.LinkCheckError):
            self._check(
                platform="Mac",
                binary=binary,
                link_map=link_map,
                expected_archive=expected,
                forbid_runtime_extractor=True,
                forbid_shell=True,
            )

    def test_rejects_runtime_extractor_or_shell_evidence(self):
        temporary, binary, link_map, expected = self._files(
            binary=b"load libinnoextract.dylib", archive=b"!<arch>\n/bin/sh"
        )
        with temporary, self.assertRaises(checker.LinkCheckError):
            self._check(
                platform="Mac",
                binary=binary,
                link_map=link_map,
                expected_archive=expected,
                forbid_runtime_extractor=True,
                forbid_shell=True,
            )

    def test_accepts_windows_map_spelling(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "runner.exe"
            link_map = root / "runner.map"
            expected = root / "wot_innoextract.lib"
            binary.write_bytes(b"PE")
            expected.write_bytes(b"archive")
            windows_archive = str(expected.resolve()).replace("/", "\\")
            windows_map = windows_archive + "\n" + "\n".join(checker.ADAPTER_SYMBOLS)
            link_map.write_text(windows_map, encoding="utf-8")
            self._check(
                platform="Win64",
                binary=binary,
                link_map=link_map,
                expected_archive=expected,
                forbid_runtime_extractor=True,
                forbid_shell=True,
            )

    def test_accepts_real_msvc_member_spelling_and_rejects_a_foreign_one(self):
        # A real MSVC /MAP names members "<library-stem>:<object>.obj" with no
        # path and no .lib suffix, so the path-based spelling never appears.
        def member_map(stem):
            rows = [
                f" 0001:00ce80b0       ?open@@YAXXZ 0000000140ce90b0 f i {stem}:adapter.cpp.obj",
                " 0001:00add2f0       ?other@@YAXXZ 0000000140ade2f0 f i InnoExtractor.cpp.obj",
            ]
            return "\n".join(rows + list(checker.ADAPTER_SYMBOLS))

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "runner.exe"
            expected = root / "wot_innoextract.lib"
            binary.write_bytes(b"PE")
            expected.write_bytes(b"archive")

            accepted = root / "accepted.map"
            accepted.write_text(member_map("wot_innoextract"), encoding="utf-8")
            self._check(
                platform="Win64", binary=binary, link_map=accepted,
                expected_archive=expected, forbid_runtime_extractor=True,
                forbid_shell=True,
            )

            foreign = root / "foreign.map"
            foreign.write_text(member_map("vendor_innoextract"), encoding="utf-8")
            with self.assertRaises(checker.LinkCheckError):
                self._check(
                    platform="Win64", binary=binary, link_map=foreign,
                    expected_archive=expected, forbid_runtime_extractor=True,
                    forbid_shell=True,
                )

    def test_rejects_exact_undefined_shell_import(self):
        temporary, binary, link_map, expected = self._files()
        with temporary, self.assertRaises(checker.LinkCheckError):
            self._check(
                platform="Mac", binary=binary, link_map=link_map,
                expected_archive=expected, forbid_runtime_extractor=True,
                forbid_shell=True, nm_stdout="_system\n",
            )

    def test_accepts_defined_or_mangled_system_name(self):
        # Defined names are absent from --undefined-only output. A mangled undefined
        # name is not an exact process API import and must not be substring-matched.
        temporary, binary, link_map, expected = self._files(
            archive=b"!<arch>\nclang-module-name:_system\0"
        )
        with temporary:
            self._check(
                platform="Mac", binary=binary, link_map=link_map,
                expected_archive=expected, forbid_runtime_extractor=True,
                forbid_shell=True, nm_stdout="__ZN3foo6systemEv\n",
            )

    def test_rejects_undefined_symbol_tool_failure(self):
        temporary, binary, link_map, expected = self._files()
        with temporary, self.assertRaises(checker.LinkCheckError):
            self._check(
                platform="Mac", binary=binary, link_map=link_map,
                expected_archive=expected, forbid_runtime_extractor=True,
                forbid_shell=True, nm_returncode=2,
            )

    def test_shell_string_scan_ignores_identifiers_but_catches_commands(self):
        # Boost.WinAPI's FACILITY_POWERSHELL_ enumerator reaches the archive's
        # debug info on every Win64 build; a command spelling must still fail.
        identifier = b"?FACILITY_POWERSHELL_@winapi@boost@@3KB"
        temporary, binary, link_map, expected = self._files(archive=identifier)
        with temporary:
            self._check(
                platform="Mac", binary=binary, link_map=link_map,
                expected_archive=expected, forbid_runtime_extractor=True,
                forbid_shell=True,
            )

        temporary, binary, link_map, expected = self._files(
            archive=b"start powershell.exe -NoProfile")
        with temporary, self.assertRaises(checker.LinkCheckError):
            self._check(
                platform="Mac", binary=binary, link_map=link_map,
                expected_archive=expected, forbid_runtime_extractor=True,
                forbid_shell=True,
            )

    def test_dumpbin_is_driven_and_parsed_in_its_own_format(self):
        # The only undefined-symbol tool a stock MSVC install provides.
        symbols_output = (
            "COFF SYMBOL TABLE\n"
            "008 00000000 SECT3  notype ()  External  | ?open@@YAXXZ (void __cdecl open(void))\n"
            "010 00000000 UNDEF  notype     External  | __imp_calloc\n"
            "011 00000000 UNDEF  notype ()  External  | ?_Xbad@std@@YAXXZ (void __cdecl std::_Xbad(void))\n"
            "012 00000000 UNDEF  notype     External  | __imp_CreateFileW\n"
        )
        temporary, _binary, _link_map, expected = self._files()
        with temporary:
            captured = {}

            def fake_run(command, **_kwargs):
                captured["command"] = command
                return SimpleNamespace(returncode=0, stdout=symbols_output, stderr="")

            with mock.patch.object(checker.subprocess, "run", side_effect=fake_run):
                symbols = checker._undefined_symbols(
                    expected, "Win64", r"C:\VC\bin\dumpbin.exe"
                )
            self.assertIn("/SYMBOLS", captured["command"])
            # UNDEF rows only, __imp_ stripped, the trailing demangling dropped,
            # and the defined SECT3 row ignored.
            self.assertEqual(symbols, {"calloc", "?_Xbad@std@@YAXXZ", "CreateFileW"})


if __name__ == "__main__":
    unittest.main()
