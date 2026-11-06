#!/usr/bin/env python3
"""Pure guarded fractal-catalog seed stripping rules."""
from __future__ import annotations


CATALOG_PATH = "/Game/WoT/Data/DA_WoTFractalCatalog"
SEED_PROPERTIES = (
    "palette_rgba",
    "initial_bitmap",
    "source_indices",
    "glass_indices",
)


def record_uses_retail_source(record) -> bool:
    """Match import_flipbooks.fractal_record_sources_retail exactly."""
    source = str(record.get_editor_property("source_texture_ref"))
    return bool(source) and not bool(
        record.get_editor_property("source_is_fractal"))


def strip_retail_source_seeds(records) -> tuple[list, int, int]:
    """Empty only seed arrays that cache-v3 can rehydrate at runtime."""
    result = list(records)
    stripped_records = 0
    stripped_bytes = 0
    for record in result:
        if not record_uses_retail_source(record):
            continue
        stripped_records += 1
        for prop in SEED_PROPERTIES:
            value = record.get_editor_property(prop)
            stripped_bytes += len(value)
            record.set_editor_property(prop, [])
    return result, stripped_records, stripped_bytes


def verify_retail_source_seeds_empty(records) -> None:
    for record in records:
        if not record_uses_retail_source(record):
            continue
        surviving = [
            prop for prop in SEED_PROPERTIES
            if len(record.get_editor_property(prop))
        ]
        if surviving:
            identity = record.get_editor_property("object_path")
            raise RuntimeError(
                f"guarded fractal seed survived for {identity}: "
                + ", ".join(surviving))
