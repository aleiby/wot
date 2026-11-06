#!/usr/bin/env python3
"""Stdlib checks for the payload-texture finalizer's mutation gates."""
import unittest

from strip_payload_textures import (
    PAYLOAD_FINALIZE_ALREADY,
    PAYLOAD_FINALIZE_FLIPBOOK,
    PAYLOAD_FINALIZE_STRIP,
    PAYLOAD_FINALIZE_UNBOUND,
    payload_finalize_decision,
    payload_finalize_is_flipbook_path,
)


class StripPayloadTexturesTests(unittest.TestCase):
    def test_placeholder_currency_selects_already_or_strip(self):
        path = "/Game/WoT/Textures/WoTPawns/AesSedai"
        self.assertEqual(
            payload_finalize_decision(path, True, True),
            PAYLOAD_FINALIZE_ALREADY)
        self.assertEqual(
            payload_finalize_decision(path, True, False),
            PAYLOAD_FINALIZE_STRIP)

    def test_flipbook_tree_is_defensively_excluded(self):
        path = "/Game/WoT/Textures/Flipbooks/Fire/Flame_sheet"
        self.assertTrue(payload_finalize_is_flipbook_path(path))
        self.assertEqual(
            payload_finalize_decision(path, True, False),
            PAYLOAD_FINALIZE_FLIPBOOK)
        self.assertFalse(payload_finalize_is_flipbook_path(
            "/Game/WoT/Textures/Angreal/Flame_sheet"))

    def test_unbound_texture_is_skipped(self):
        self.assertEqual(payload_finalize_decision(
            "/Game/WoT/UI/Authored", False, False),
            PAYLOAD_FINALIZE_UNBOUND)


if __name__ == "__main__":
    unittest.main()
