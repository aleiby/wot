"""Unit tests for the Steam VDF readers (fixtures are built in code, no account data)."""
from __future__ import annotations

import struct
import unittest

import vdf


def _key(name: str) -> bytes:
    return name.encode("utf-8") + b"\0"


def s(name: str, value: bytes) -> bytes:
    return b"\x01" + _key(name) + value + b"\0"


def i32(name: str, value: int) -> bytes:
    return b"\x02" + _key(name) + struct.pack("<I", value & 0xFFFFFFFF)


def m(name: str, *children: bytes) -> bytes:
    return b"\x00" + _key(name) + b"".join(children) + b"\x08"


def shortcuts_fixture() -> bytes:
    """A shortcuts.vdf shaped like a real one, with every key type a client may write."""
    first = m(
        "0",
        i32("appid", 0xB1234567),
        s("AppName", b"Some Emulator"),
        s("Exe", b'"/home/player/Applications/emu.AppImage"'),
        s("StartDir", b'"/home/player/Applications/"'),
        s("icon", b""),
        s("ShortcutPath", b""),
        s("LaunchOptions", b"-fullscreen \xe2\x9c\x93"),  # UTF-8 kept as raw bytes
        i32("IsHidden", 0),
        i32("AllowDesktopConfig", 1),
        i32("AllowOverlay", 1),
        i32("OpenVR", 0),
        i32("Devkit", 0),
        s("DevkitGameID", b""),
        i32("DevkitOverrideAppID", 0),
        i32("LastPlayTime", 1730000000),
        s("FlatpakAppID", b"org.example.Emu"),
        m("tags", s("0", b"favorite"), s("1", b"Retro")),
    )
    second = m(
        "1",
        i32("appid", 0x8ABCDEF0),
        s("appname", b"Lower Case Keys"),          # an older client's spelling
        s("exe", b"/usr/bin/flatpak"),
        b"\x07" + _key("SomeFutureU64") + struct.pack("<Q", 2 ** 63 + 5),
        b"\x03" + _key("SomeFloat") + struct.pack("<f", 1.5),
        b"\x05" + _key("WideName") + "Wide".encode("utf-16-le") + b"\0\0",
        b"\x0a" + _key("SomeI64") + struct.pack("<q", -7),
        b"\x06" + _key("Tint") + b"\x10\x20\x30\x40",
        b"\x04" + _key("Ptr") + b"\0\0\0\0",
        m("tags"),
    )
    return m("shortcuts", first, second) + b"\x08"


class BinaryVdfTests(unittest.TestCase):
    def test_round_trip_is_byte_identical_with_unknown_keys(self):
        data = shortcuts_fixture()
        document = vdf.parse_binary(data)
        self.assertEqual(vdf.dump_binary(document), data)

    def test_append_keeps_existing_entries_byte_for_byte(self):
        data = shortcuts_fixture()
        document = vdf.parse_binary(data)
        shortcuts = vdf.find(document.nodes, "shortcuts")
        shortcuts.value.append(vdf.map_node("2", [vdf.string_node("AppName", "New")]))
        out = vdf.dump_binary(document)
        body_end = len(data) - 2  # the old entries end before the two closing 0x08 bytes
        self.assertEqual(out[:body_end], data[:body_end])
        self.assertEqual(out[body_end:], m("2", s("AppName", b"New")) + b"\x08\x08")
        reparsed = vdf.parse_binary(out)
        names = [node.name for node in vdf.find(reparsed.nodes, "shortcuts").value]
        self.assertEqual(names, ["0", "1", "2"])

    def test_accessors_are_case_insensitive_and_unsigned(self):
        document = vdf.parse_binary(shortcuts_fixture())
        entries = vdf.find(document.nodes, "SHORTCUTS").value
        self.assertEqual(entries[0].child("APPID").uint32(), 0xB1234567)
        self.assertEqual(entries[1].child("Exe").text(), "/usr/bin/flatpak")
        self.assertEqual(entries[1].child("AppName").text(), "Lower Case Keys")
        self.assertIsNone(entries[0].child("AppName").uint32())
        self.assertIsNone(entries[0].child("appid").text())

    def test_trailing_bytes_are_preserved(self):
        data = shortcuts_fixture() + b"\x00garbage"
        self.assertEqual(vdf.dump_binary(vdf.parse_binary(data)), data)

    def test_new_nodes_serialize_in_the_wire_format(self):
        document = vdf.Document([vdf.map_node("shortcuts", [
            vdf.map_node("0", [vdf.int32_node("appid", 0x80000001),
                               vdf.string_node("Exe", '"/x y"'),
                               vdf.map_node("tags", [])])])])
        self.assertEqual(vdf.dump_binary(document), m(
            "shortcuts", m("0", i32("appid", 0x80000001), s("Exe", b'"/x y"'), m("tags"))) + b"\x08")

    def test_malformed_files_are_refused(self):
        good = shortcuts_fixture()
        for bad in (good[:-1], good[:20], b"\x00shortcuts\x00\x09key\x00\x08\x08",
                    b"\x01key\x00no-terminator", b"\x02key\x00\x01\x02"):
            with self.assertRaises(vdf.VdfError, msg=repr(bad[:40])):
                vdf.parse_binary(bad)

    def test_nul_in_new_values_is_refused(self):
        with self.assertRaises(vdf.VdfError):
            vdf.string_node("Exe", "a\0b")


LOGINUSERS = '''"users"
{
\t"76561197960265829"
\t{
\t\t"AccountName"\t\t"first"
\t\t"PersonaName"\t\t"First \\"Q\\" Player"
\t\t"RememberPassword"\t\t"1"
\t\t"MostRecent"\t\t"0"
\t\t"Timestamp"\t\t"1700000000"
\t}
\t// a comment line
\t"76561197960266930"
\t{
\t\t"AccountName"\t\t"second"
\t\t"mostrecent"\t\t"1"
\t\t"Timestamp"\t\t"1600000000"
\t}
}
'''


class TextVdfTests(unittest.TestCase):
    def test_parses_nested_maps_escapes_and_comments(self):
        document = vdf.parse_text(LOGINUSERS)
        users = vdf.get_ci(document, "USERS")
        self.assertEqual(list(users), ["76561197960265829", "76561197960266930"])
        first = users["76561197960265829"]
        self.assertEqual(first["PersonaName"], 'First "Q" Player')
        self.assertEqual(vdf.get_ci(users["76561197960266930"], "MostRecent"), "1")
        self.assertIsNone(vdf.get_ci(first, "missing"))

    def test_unquoted_tokens_and_conditionals(self):
        document = vdf.parse_text('root { key value [$LINUX] other "x" }')
        self.assertEqual(document, {"root": {"key": "value", "other": "x"}})

    def test_malformed_text_is_refused(self):
        for bad in ('"users" {', '"users" { "a" }', '}', '"a" "b" "c"', '"unterminated'):
            with self.assertRaises(vdf.VdfError, msg=bad):
                vdf.parse_text(bad)


if __name__ == "__main__":
    unittest.main()
