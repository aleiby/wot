"""Shared private/reference paths for the standalone gamma diagnostics."""
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from wot_config import exports_dir, local_path, original_dir

ORIGINAL = original_dir()
EXPORTS = exports_dir()
GOLD = EXPORTS / "reference" / "shots"
SCRATCH = local_path("scratch_root")
# The identity-conversion output these diagnostics read (texture PNGs and
# sky JSON), by default conv_identity under scratch_root. It has its own optional
# key: conversion_root is build_stripped_pak's --conv-dir (by default
# <repo>/Intermediate/Conv), a different directory, and configuring it for
# these tools would redirect packaging.
CONVERSION = local_path("identity_conversion_root", required=False,
                        default=SCRATCH / "conv_identity")
