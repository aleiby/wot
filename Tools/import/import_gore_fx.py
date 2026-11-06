"""Import the gore blood textures + their modulate particle material instances.

UnrealEditor-Cmd WoT.uproject -run=pythonscript -script=".../import_gore_fx.py"

The 4d blood configs (Source/WoT/WoTBloodFX.cpp) resolve materials by soft path
/Game/WoT/Materials/Instances/MI_PSMod_ParticleSystems__<Tex>; this builds those:
PNG (Intermediate/Conv/tex/ParticleSystems) → /Game/WoT/Textures/ParticleSystems/<Tex>
+ a MaterialInstanceConstant parented to the existing M_WoT_ParticleMod master (all
blood is STY_Modulated — no additive instances). Reruns are idempotent.
"""
import os
import sys
from pathlib import Path

import unreal

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
from payload_binding import ensure_texture_payload_binding

REPO = Path(__file__).resolve().parents[2]
CONV_ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else
                 os.environ.get("WOT_CONV_DIR", REPO / "Intermediate" / "Conv"))
CONV = CONV_ROOT / "tex" / "ParticleSystems"
TEX_DEST = "/Game/WoT/Textures/ParticleSystems"
MI_DEST = "/Game/WoT/Materials/Instances"
MASTER = "/Game/WoT/Materials/M_WoT_ParticleMod"

# Every texture the 7 blood configs reference (WoTBloodFX.cpp registry).
NAMES = [
    "Blood07", "Blood08", "Blood11", "Blood16", "Blood17",
    "RoundBlood01", "RoundBlood02", "RoundBlood03", "RoundBlood04",
    "BloodBlack07", "BloodBlack08", "BloodBlack11", "BloodBlack16", "BloodBlack17",
    "RoundBloodBlack01", "RoundBloodBlack02", "RoundBloodBlack03", "RoundBloodBlack04",
]

EAL = unreal.EditorAssetLibrary
ATT = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary

master = unreal.load_asset(MASTER)
if not master:
    raise RuntimeError(f"missing particle master {MASTER} — run a map import first")

made = 0
for name in NAMES:
    tex_path = f"{TEX_DEST}/{name}"
    if not EAL.does_asset_exist(tex_path):
        png = CONV / f"{name}.png"
        if not png.exists():
            raise RuntimeError(f"missing {png} — run utx2png on ParticleSystems.u")
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = TEX_DEST
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    ensure_texture_payload_binding(
        unreal, tex_path, "ParticleSystems", name)
    mi_name = f"MI_PSMod_ParticleSystems__{name}"
    mi_path = f"{MI_DEST}/{mi_name}"
    if not EAL.does_asset_exist(mi_path):
        mi = ATT.create_asset(mi_name, MI_DEST, unreal.MaterialInstanceConstant,
                              unreal.MaterialInstanceConstantFactoryNew())
        mi.set_editor_property("parent", master)
        MEL.set_material_instance_texture_parameter_value(
            mi, "Tex", unreal.load_asset(tex_path))
        EAL.save_asset(mi_path)
        made += 1


# ---- 4e decals: the decal textures (+ the retired deferred-decal master). ----
# RETIRED 2026-09-30 (PORT_NOTES "Decals as UE1 meshes"): AWoTDecal now draws
# the retail Legend.DecalTRI mesh with M_WoT_MeshModulate / M_WoT_MeshTranslucent
# and binds these textures directly; nothing resolves M_WoT_Decal or MI_Decal_*.
# The master is still generated only because it is a pinned neutral-closure
# root (Tools/dist/neutral-support-assets-v1.json); retire this block together
# with that root. The texture imports below remain live.
DECAL_NAMES = (
    [f"Blood{i:02d}" for i in list(range(4, 14)) + list(range(15, 20))]
    + [f"BloodBlack{i:02d}" for i in list(range(4, 14)) + list(range(15, 20))]
    + ["Crack01", "Crack02", "decaysplat"]
    + [f"scorch{i}" for i in list(range(1, 8)) + [10, 11, 12]]
)

def make_decal_master():
    full = "/Game/WoT/Materials/M_WoT_Decal"
    if EAL.does_asset_exist(full):
        return unreal.load_asset(full)
    m = ATT.create_asset("M_WoT_Decal", "/Game/WoT/Materials", unreal.Material,
                         unreal.MaterialFactoryNew())
    m.set_editor_property("material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    def expr(cls, x, y):
        return MEL.create_material_expression(m, cls, x, y)

    tex = expr(unreal.MaterialExpressionTextureSampleParameter2D, -700, 0)
    tex.set_editor_property("parameter_name", "Tex")
    MEL.connect_material_property(tex, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    # Opacity = 1 - max(R, max(G, B)).
    mr = expr(unreal.MaterialExpressionComponentMask, -520, 200)
    mr.set_editor_property("r", True); mr.set_editor_property("g", False)
    mr.set_editor_property("b", False); mr.set_editor_property("a", False)
    mg = expr(unreal.MaterialExpressionComponentMask, -520, 280)
    mg.set_editor_property("r", False); mg.set_editor_property("g", True)
    mg.set_editor_property("b", False); mg.set_editor_property("a", False)
    mb = expr(unreal.MaterialExpressionComponentMask, -520, 360)
    mb.set_editor_property("r", False); mb.set_editor_property("g", False)
    mb.set_editor_property("b", True); mb.set_editor_property("a", False)
    for e in (mr, mg, mb):
        MEL.connect_material_expressions(tex, "RGB", e, "")
    mx1 = expr(unreal.MaterialExpressionMax, -380, 240)
    MEL.connect_material_expressions(mr, "", mx1, "A")
    MEL.connect_material_expressions(mg, "", mx1, "B")
    mx2 = expr(unreal.MaterialExpressionMax, -260, 300)
    MEL.connect_material_expressions(mx1, "", mx2, "A")
    MEL.connect_material_expressions(mb, "", mx2, "B")
    om = expr(unreal.MaterialExpressionOneMinus, -140, 300)
    MEL.connect_material_expressions(mx2, "", om, "")
    MEL.connect_material_property(om, "", unreal.MaterialProperty.MP_OPACITY)
    MEL.recompile_material(m)
    EAL.save_asset(full)
    return m

decal_master = make_decal_master()
decal_made = 0
for name in DECAL_NAMES:
    tex_path = f"{TEX_DEST}/{name}"
    if not EAL.does_asset_exist(tex_path):
        png = CONV / f"{name}.png"
        if not png.exists():
            raise RuntimeError(f"missing {png} — run utx2png on ParticleSystems.u")
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = TEX_DEST
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    ensure_texture_payload_binding(
        unreal, tex_path, "ParticleSystems", name)
    mi_name = f"MI_Decal_ParticleSystems__{name}"
    mi_path = f"{MI_DEST}/{mi_name}"
    if not EAL.does_asset_exist(mi_path):
        mi = ATT.create_asset(mi_name, MI_DEST, unreal.MaterialInstanceConstant,
                              unreal.MaterialInstanceConstantFactoryNew())
        mi.set_editor_property("parent", decal_master)
        MEL.set_material_instance_texture_parameter_value(
            mi, "Tex", unreal.load_asset(tex_path))
        EAL.save_asset(mi_path)
        decal_made += 1


def import_decal_texture(pkg, name):
    tex_path = f"/Game/WoT/Textures/{pkg}/{name}"
    if not EAL.does_asset_exist(tex_path):
        png = CONV_ROOT / "tex" / pkg / f"{name}.png"
        if not png.exists():
            raise RuntimeError(f"missing {png} — run utx2png")
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = f"/Game/WoT/Textures/{pkg}"
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    ensure_texture_payload_binding(unreal, tex_path, pkg, name)
    return tex_path


# The balefire decal textures AWoTDecal binds directly (WoTDecal.cpp registry):
# BalefireDecal.uc AnimTextures BFhole01-10 and BFHole.uc's BalefireHole.
for name in [f"BFhole{i:02d}" for i in range(1, 11)] + ["BalefireHole"]:
    import_decal_texture("Angreal", name)

# 6a-iv: the balefire end-scorch decal (Angreal package — first burn-in frame).
for pkg, name in [("Angreal", "BFhole01"), ("WoTPawns", "MashadarCrack")]:
    tex_path = f"/Game/WoT/Textures/{pkg}/{name}"
    if not EAL.does_asset_exist(tex_path):
        png = CONV_ROOT / "tex" / pkg / f"{name}.png"
        if not png.exists():
            raise RuntimeError(f"missing {png} — run utx2png")
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = f"/Game/WoT/Textures/{pkg}"
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    ensure_texture_payload_binding(unreal, tex_path, pkg, name)
    mi_path = f"{MI_DEST}/MI_Decal_{pkg}__{name}"
    if not EAL.does_asset_exist(mi_path):
        mi = ATT.create_asset(f"MI_Decal_{pkg}__{name}", MI_DEST,
                              unreal.MaterialInstanceConstant,
                              unreal.MaterialInstanceConstantFactoryNew())
        mi.set_editor_property("parent", decal_master)
        MEL.set_material_instance_texture_parameter_value(
            mi, "Tex", unreal.load_asset(tex_path))
        EAL.save_asset(mi_path)
        decal_made += 1

unreal.log(f"IMPORT-GORE-FX-OK textures={len(NAMES)} new_instances={made} "
           f"decal_instances={decal_made}/{len(DECAL_NAMES)}")
