import unittest

import import_menu_strings


class ImportedMenuVerificationTests(unittest.TestCase):
    def test_verification_uses_parsed_rows_and_exact_counts(self):
        sections = ["menuMain", "menuMultiPlayer", "menuLoad",
                    *[f"menu{index}" for index in range(12)]]
        rows = [
            ("menuMain.MenuList[2]", "menuMain", "MenuList[2]", "fixture alpha"),
            ("menuMain.MenuList[7]", "menuMain", "MenuList[7]", "fixture beta"),
            ("menuMultiPlayer.HelpMessage[1]", "menuMultiPlayer",
             "HelpMessage[1]", "fixture gamma"),
            ("menuLoad.RestartString", "menuLoad", "RestartString", "fixture delta "),
        ]
        actual_strings = {key: text for key, _, _, text in rows}

        ok, pins, counts = import_menu_strings.verify_imported_content(
            actual_strings, sections, sections, rows, str)

        self.assertTrue(ok)
        self.assertTrue(all(pins.values()))
        self.assertEqual(counts, {"keys": 4, "sections": 15})

        changed_rows = [*rows]
        changed_rows[2] = (*changed_rows[2][:3], "updated fixture")
        ok, pins, counts = import_menu_strings.verify_imported_content(
            actual_strings, sections, sections, changed_rows, str)

        self.assertFalse(ok)
        self.assertFalse(pins["menuMultiPlayer.HelpMessage[1]"])
        self.assertEqual(counts, {"keys": 4, "sections": 15})

        ok, _, counts = import_menu_strings.verify_imported_content(
            {**actual_strings, "menuMain.Extra": "extra"},
            [*sections, "menuExtra"], sections, rows, str)

        self.assertFalse(ok)
        self.assertEqual(counts, {"keys": 5, "sections": 16})


if __name__ == "__main__":
    unittest.main()
