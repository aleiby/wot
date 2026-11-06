"""One-shot sheet-scrub retirement patch for COMMITTED content.

Run AFTER import_flame_fx.py (regenerates the FB masters in place, restamps
the FB particle/mesh MICs) and import_flipbooks.py (restamps the shared
MI_<Name> instances onto the FractalLive masters, rebuilds the catalog
without sheet refs). This pass then:

1. Sweeps every MIC under /Game/WoT/Materials/Instances (recursive — the
   per-map subfolders): reparents *Flip-blend children onto their plain
   siblings, drops texture overrides that point into
   /Game/WoT/Textures/Flipbooks (rebinding a dropped Tex to T_WoT_Neutral),
   and drops the sheet-grid scalar overrides (FPS/Frames/Cols/Rows) from
   any instance it touched.
2. Audits the asset registry: NOTHING may still reference the sheet packages
   or the retired masters. Fails loudly (no deletes) otherwise.
3. Deletes the retired assets: /Game/WoT/Textures/Flipbooks/*, the
   M_WoT_Flipbook{Lit,Add} masters, and the four M_WoT_World*Flip blend
   masters.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script=".../retire_flipbook_sheets.py"
"""
import sys

import unreal

EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

RETIRED_MASTERS = [
    "/Game/WoT/Materials/M_WoT_FlipbookLit",
    "/Game/WoT/Materials/M_WoT_FlipbookAdd",
    "/Game/WoT/Materials/M_WoT_WorldTranslucentFlip",
    "/Game/WoT/Materials/M_WoT_WorldModulatedFlip",
    "/Game/WoT/Materials/M_WoT_WorldWaveTranslucentFlip",
    "/Game/WoT/Materials/M_WoT_WorldWaveModulatedFlip",
]
FLIP_SIBLINGS = {
    "M_WoT_FlipbookLit": "/Game/WoT/Materials/M_WoT_FractalLiveLit",
    "M_WoT_FlipbookAdd": "/Game/WoT/Materials/M_WoT_FractalLiveAdd",
    "M_WoT_WorldTranslucentFlip": "/Game/WoT/Materials/M_WoT_WorldTranslucent",
    "M_WoT_WorldModulatedFlip": "/Game/WoT/Materials/M_WoT_WorldModulated",
    "M_WoT_WorldWaveTranslucentFlip":
        "/Game/WoT/Materials/M_WoT_WorldWaveTranslucent",
    "M_WoT_WorldWaveModulatedFlip":
        "/Game/WoT/Materials/M_WoT_WorldWaveModulated",
}
SHEET_ROOT = "/Game/WoT/Textures/Flipbooks"
GRID_PARAMS = ("FPS", "Frames", "Cols", "Rows")
NEUTRAL = "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral"


def patch_instances():
    neutral = unreal.load_asset(NEUTRAL)
    if not neutral:
        raise RuntimeError(f"missing {NEUTRAL}")
    # A commandlet's registry may not have scanned yet — without this,
    # list_assets returns [] and the sweep silently no-ops.
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(
        ["/Game/WoT/Materials"], force_rescan=True)
    candidates = EAL.list_assets("/Game/WoT/Materials/Instances", recursive=True)
    if len(candidates) < 100:
        raise RuntimeError(
            f"implausibly small instance census ({len(candidates)}) — registry scan failed?")
    unreal.log_warning(f"RETIRE-PATCH sweeping {len(candidates)} instance assets")
    touched = reparented = 0
    for asset in candidates:
        path = asset.split(".")[0]
        mi = unreal.load_asset(path)
        if not isinstance(mi, unreal.MaterialInstanceConstant):
            continue
        dirty = False
        parent = mi.get_editor_property("parent")
        pname = parent.get_name() if parent else ""
        if pname in FLIP_SIBLINGS:
            sibling = unreal.load_asset(FLIP_SIBLINGS[pname])
            if not sibling:
                raise RuntimeError(f"missing sibling for {pname}")
            mi.set_editor_property("parent", sibling)
            reparented += 1
            dirty = True
        tex_values = list(mi.get_editor_property("texture_parameter_values"))
        kept = []
        dropped_tex = False
        for v in tex_values:
            value = v.get_editor_property("parameter_value")
            vpath = value.get_path_name() if value else ""
            if vpath.startswith(SHEET_ROOT):
                info = v.get_editor_property("parameter_info")
                if str(info.get_editor_property("name")) == "Tex":
                    dropped_tex = True
                dirty = True
                continue
            kept.append(v)
        if dirty or dropped_tex:
            mi.set_editor_property("texture_parameter_values", kept)
            if dropped_tex:
                MEL.set_material_instance_texture_parameter_value(
                    mi, "Tex", neutral)
            scalars = [v for v in mi.get_editor_property("scalar_parameter_values")
                       if str(v.get_editor_property("parameter_info")
                              .get_editor_property("name")) not in GRID_PARAMS]
            mi.set_editor_property("scalar_parameter_values", scalars)
            EAL.save_asset(path)
            touched += 1
    unreal.log_warning(f"RETIRE-PATCH instances touched={touched} reparented={reparented}")


def audit():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    offenders = []
    sheet_assets = EAL.list_assets(SHEET_ROOT, recursive=True)
    targets = [a.split(".")[0] for a in sheet_assets] + RETIRED_MASTERS
    for target in targets:
        package = target.rsplit(".", 1)[0]
        deps = registry.get_referencers(
            unreal.Name(package),
            unreal.AssetRegistryDependencyOptions(include_soft_package_references=False,
                                                  include_hard_package_references=True))
        for d in deps or []:
            ds = str(d)
            if ds.startswith("/Game/WoT/Textures/Flipbooks"):
                continue   # sheets referencing their own package neighbors
            if any(ds == m.rsplit("/", 1)[0] or ds == m for m in RETIRED_MASTERS):
                continue   # a retired master referencing another retired master
            offenders.append((target, ds))
    if offenders:
        for target, ref in offenders[:40]:
            unreal.log_error(f"RETIRE-AUDIT {ref} still references {target}")
        raise RuntimeError(
            f"{len(offenders)} live references to retired assets — NOT deleting")
    unreal.log_warning(f"RETIRE-AUDIT clean: {len(targets)} retired assets unreferenced")


def delete_retired():
    deleted = 0
    failed = []
    for asset in EAL.list_assets(SHEET_ROOT, recursive=True):
        path = asset.split(".")[0]
        if EAL.delete_asset(path):
            deleted += 1
        else:
            failed.append(path)
    for master in RETIRED_MASTERS:
        if EAL.does_asset_exist(master):
            if EAL.delete_asset(master):
                deleted += 1
            else:
                failed.append(master)
    if failed:
        for path in failed[:20]:
            unreal.log_error(f"RETIRE-DELETE failed on {path}")
        raise RuntimeError(f"{len(failed)} deletes failed")
    # The emptied folder is cosmetic; a refusal here is not a failure.
    if EAL.does_directory_exist(SHEET_ROOT):
        EAL.delete_directory(SHEET_ROOT)
    unreal.log_warning(f"RETIRE-DELETE done ({deleted} assets removed)")


patch_instances()
audit()
delete_retired()
unreal.log_warning("RETIRE-OK sheet lane fully retired")
