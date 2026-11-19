#!/usr/bin/env python3
"""Converge existing content's texture sampling to the importers' own claims.

UnrealEditor-Cmd WoT.uproject -run=pythonscript -unattended -nullrhi
  -script="<abs>/Tools/import/converge_texture_sampling.py
           [--conv-dir DIR] [--maps MAP ...] [--dry-run] [--report PATH]
           [--require-clean]"

Every importer that acquires textures declares the assets it owns, from its
own manifest, as ``texture_sampling_claims`` (CLAIM_PROVIDERS below; the map
importer once per map). This step resolves those claims (texture_sampling.py:
a named claim outranks the code-package skin pass's blanket one, so the HUD
inputs beside world textures keep their UI policy), sets each existing owned
asset's srgb flag to its family's setting and saves only the assets it
changed. It never imports, creates, deletes or moves anything, and changes no
other property. It then scans every Texture2D under /Game/WoT and writes a
JSON report: the changed assets, each texture's family, srgb and status, and
the totals. --dry-run reports what would change without writing.

--conv-dir is the converted tree the importers read (WOT_CONV_DIR, else the
machine-local conversion_root). import_map.py reads the script exports when it
loads, so WOT_REFERENCE_ROOT (or WOT_EXPORTS_DIR) must name them. Use forward
slashes inside -script="..." (the commandlet unescapes backslashes). Run it
only while no other editor has the project's content open.
"""
from __future__ import annotations

import argparse
import importlib
import json
import os
import re
import sys
from pathlib import Path

IMPORT_DIR = Path(__file__).resolve().parent
REPO = IMPORT_DIR.parents[1]

# importer script -> module whose texture_sampling_claims(context) declares
# what it acquires. import_mesh.py needs a mesh argument to import, so its
# manifest (slot census + texture resolution) lives in import_mesh_policy.py.
CLAIM_PROVIDERS = {
    "import_first_person_hands": "import_first_person_hands",
    "import_flame_fx": "import_flame_fx",
    "import_flipbooks": "import_flipbooks",
    "import_gore_fx": "import_gore_fx",
    "import_hud_fonts": "import_hud_fonts",
    "import_hud_textures": "import_hud_textures",
    "import_map": "import_map",
    "import_mesh": "import_mesh_policy",
    "import_pickups": "import_pickups",
    "import_player_skins": "import_player_skins",
    "import_seal_skins": "import_seal_skins",
    "import_trap_skins": "import_trap_skins",
    "import_uwindow_textures": "import_uwindow_textures",
    "import_weave_sprites": "import_weave_sprites",
}
# Providers whose claims depend on module-level per-map state (argv-driven).
PER_MAP_PROVIDERS = {"import_map"}

DECAY_TEXTURE = re.compile(r"^(decay\w*|DC_A\d\d)$", re.IGNORECASE)


def _parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--conv-dir", type=Path)
    parser.add_argument("--maps", nargs="+")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--require-clean", action="store_true",
                        help="fail when any owned texture still mismatches")
    return parser


def _default_conv_dir():
    configured = os.environ.get("WOT_CONV_DIR", "").strip()
    if configured:
        return Path(configured)
    sys.path.insert(0, str(IMPORT_DIR.parent))
    from wot_config import local_path  # noqa: E402
    return local_path("conversion_root", required=False,
                      default=REPO / "Intermediate" / "Conv")


def _import_provider(name, argv):
    saved = sys.argv
    sys.argv = list(argv)
    try:
        if name in sys.modules:
            return importlib.reload(sys.modules[name])
        return importlib.import_module(name)
    finally:
        sys.argv = saved


def collect_claims(texture_sampling, context, log=print):
    """Every provider's claims (import_map once per map), deduplicated."""
    claims = []
    for importer, provider in sorted(CLAIM_PROVIDERS.items()):
        script = str(IMPORT_DIR / f"{provider}.py")
        if provider in PER_MAP_PROVIDERS:
            count = 0
            for map_name in context.maps:
                module = _import_provider(
                    provider, [script, map_name, str(context.conv_root),
                               "preview"])
                items = module.texture_sampling_claims(context)
                count += len(items)
                claims.extend(items)
            log(f"TEXTURE-SAMPLING-CLAIMS {importer}: {count} claim(s) over "
                f"{len(context.maps)} map(s)")
            continue
        module = _import_provider(provider, [script])
        items = module.texture_sampling_claims(context)
        log(f"TEXTURE-SAMPLING-CLAIMS {importer}: {len(items)} claim(s)")
        claims.extend(items)
    return texture_sampling.unique_claims(claims)


def main(argv=None):
    import unreal

    args = _parser().parse_args(sys.argv[1:] if argv is None else argv)
    if os.environ.get("WOT_PAYLOAD_MODE", "").strip().casefold() == "stripped":
        raise RuntimeError("converge_texture_sampling.py repairs canonical "
                           "development content; unset WOT_PAYLOAD_MODE")
    for path in (str(IMPORT_DIR), str(IMPORT_DIR.parent / "convert"),
                 str(IMPORT_DIR.parent)):
        if path not in sys.path:
            sys.path.insert(0, path)
    import texture_sampling
    from import_all import MP_MAPS
    from wot_config import exports_dir

    conv_root = (args.conv_dir or _default_conv_dir()).resolve()
    if not (conv_root / "tex").is_dir():
        raise RuntimeError(f"no converted texture tree under {conv_root}")
    exports = Path(os.environ.get("WOT_EXPORTS_DIR", "").strip()
                   or exports_dir(required=False))
    if not (exports / "script").is_dir():
        raise RuntimeError(
            f"no script exports under {exports}: import_map.py's claims need "
            "them (set WOT_REFERENCE_ROOT or WOT_EXPORTS_DIR)")
    maps = tuple(args.maps or MP_MAPS)
    context = texture_sampling.ClaimContext(conv_root, unreal, maps)

    def log(message):
        print(message)
        unreal.log(message)

    claims = collect_claims(texture_sampling, context, log)
    owned = texture_sampling.resolve_claims(claims)
    converge = texture_sampling.converge_claims(
        unreal, claims, apply=not args.dry_run)
    rows = texture_sampling.scan_textures(unreal, claims)
    totals = texture_sampling.scan_totals(rows)
    decay = [row for row in rows
             if DECAY_TEXTURE.match(row["asset"].rsplit("/", 1)[1])]
    report = {
        "conv_root": str(conv_root),
        "maps": list(maps),
        "dry_run": bool(args.dry_run),
        "claims": len(claims),
        "owned_assets": len(owned),
        "families": {
            family: sum(1 for item in owned.values() if item.family == family)
            for family in sorted({item.family for item in owned.values()})},
        "converge": converge,
        "scan_totals": totals,
        "decay": decay,
        "scan": rows,
    }
    report_path = args.report or (
        Path(unreal.Paths.convert_relative_path_to_full(
            unreal.Paths.project_saved_dir())) / "TextureSampling"
        / ("converge-dry-run.json" if args.dry_run else "converge.json"))
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=1), encoding="utf-8")

    verb = "would change" if args.dry_run else "changed"
    for item in converge["changed"]:
        log(f"TEXTURE-SAMPLING-CHANGED {item['asset']} srgb "
            f"{item['before']}->{item['after']} ({item['family']}; "
            f"{', '.join(item['owners'])})")
    for item in converge["conflicts"]:
        log(f"TEXTURE-SAMPLING-CONFLICT {item['asset']} {item['claims']}")
    log(f"TEXTURE-SAMPLING-CONVERGE {verb}={len(converge['changed'])} "
        f"unchanged={converge['unchanged']} "
        f"missing={len(converge['missing'])} "
        f"conflicts={len(converge['conflicts'])}")
    log("TEXTURE-SAMPLING-SCAN " + " ".join(
        f"{key}={value}" for key, value in totals.items()))
    log(f"TEXTURE-SAMPLING-REPORT {report_path}")
    if args.require_clean and (totals["mismatch"] or totals["conflict"]):
        raise RuntimeError(
            f"texture sampling is not clean: {totals} (see {report_path})")
    return report


if __name__ == "__main__":
    main()
