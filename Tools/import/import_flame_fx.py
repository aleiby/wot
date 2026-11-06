"""Import the FireWall flame particle materials.

The FireWallSlab flame bundle (Source/WoT/Traps/WoTFireWall.cpp) resolves its
sprayer materials by soft path. The flame textures are PROCEDURAL (WetTexture/
FireTexture) — bake_flipbooks.py already produced their sheets and
import_flipbooks.py imported them for surfaces; particles need sheet-scrubbing
variants of the PARTICLE masters (camera-facing ISM + per-instance ScaleGlow),
so this builds:

  M_WoT_ParticleAddFB / M_WoT_ParticleModFB — the M_WoT_ParticleAdd/Mod graph
    (gamma-space branches) with the import_flipbooks.py time-scrubbed sheet UV
    (FPS/Frames/Cols/Rows scalar params) feeding the Tex sampler.
  MI_PSAddFB_WoTTraps__<Name> + MI_PSModFB_WoTTraps__<Name> for the three flame
    sheets (FireWallFlameWet, FireWallFlameWet2, FWFlamesSprite), scalars from
    Intermediate/Conv/flipbooks/WoTTraps/_flipbooks.json.
  MI_PSMod_ParticleSystems__Blk64_005 — the (static) smoke texture for
    FireWallSprayer2 (STY_Modulated), per the import_gore_fx.py convention.

Reruns are idempotent.

UnrealEditor-Cmd WoT.uproject -run=pythonscript -script=".../import_flame_fx.py"
"""
import json
import os
import sys
from pathlib import Path

import unreal

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
from payload_binding import ensure_texture_payload_binding

REPO = Path(__file__).resolve().parents[2]
CONV = Path(sys.argv[1] if len(sys.argv) > 1 else
            os.environ.get("WOT_CONV_DIR", REPO / "Intermediate" / "Conv"))
FLIPROOT = CONV / "flipbooks"
SMOKE_PNG = CONV / "tex" / "ParticleSystems" / "Blk64_005.png"
# (package, sheet) pairs needing FB particle MIs: the FireWall flames and
# the weave procedural sprites (Seeker + SoulBarb flare).
SHEETS = [("WoTTraps", "FireWallFlameWet"), ("WoTTraps", "FireWallFlameWet2"),
          ("WoTTraps", "FWFlamesSprite"),
          ("Angreal", "SLightning01"), ("Angreal", "NimbusAnim"),
          ("Angreal", "LSsparkA"), ("WoTPawns", "MyTex2"),
          # Procedural particle/center sheets.
          ("Angreal", "AniBitA"), ("Angreal", "AniBitB"),
          ("Angreal", "TntCntrA"),
          ("Angreal", "RedIce"), ("Angreal", "YeloIce"),
          # The base dart's procedural WetTexture impact.
          ("Angreal", "DartExpl"),
          ("ParticleSystems", "Fire_Torch_001"),
          ("ParticleSystems", "Fire_Torch_003")]
# Static particle textures needing BOTH MI_PSAdd_/MI_PSMod_ pairs (the weave
# trail bundles; translucent style needs both passes).
STATICS = [("ParticleSystems", "Sparks01"), ("ParticleSystems", "particle_fog05"),
           # LegionStompVolcano eruption sprayers (LegionStompSprayerA-D.uc).
           ("ParticleSystems", "LavaSkullA"), ("ParticleSystems", "LavaSmokeA"),
           ("ParticleSystems", "LavaFlamesA"),
           ("ParticleSystems", "Splash01"),
           ("WoTPawns", "SmokePuff02"), ("Angreal", "FBExp503"), ("Angreal", "SnowB"), ("Angreal", "SnowC"),
           ("Angreal", "SnowD"), ("Angreal", "SnowE"),
           ("ParticleSystems", "PF13"), ("ParticleSystems", "Prtcl05"),
           ("ParticleSystems", "Prtcl14"), ("ParticleSystems", "Prtcl20"),
           # AirBurst blast (AirBlastProj.uc / AirBlastBall.uc)
           ("Angreal", "APulseA"), ("ParticleSystems", "CyanCorona"),
           ("ParticleSystems", "PF17"),
           ("ParticleSystems", "Splash02"),
           ("ParticleSystems", "decaydrop"),
           ("ParticleSystems", "decaydrop1"),
           ("ParticleSystems", "decaysplat"),
           ("ParticleSystems", "AWhiteCorona"),
           ("ParticleSystems", "PF14"),
           ("ParticleSystems", "PinkCorona"),
           ("ParticleSystems", "Sparks12"),
           ("ParticleSystems", "Prtcl18"),
           ("ParticleSystems", "Prtcl23"),
           ("ParticleSystems", "PF11"),
           ("ParticleSystems", "YellowCorona"),
           ("ParticleSystems", "Sparks13"),
           ("ParticleSystems", "Sparks15"),
           ("ParticleSystems", "APurpleCorona"),
           ("Angreal", "Spirits03"), ("Angreal", "Spirits04"),
           ("Angreal", "TaintRedcomet3"), ("Angreal", "TaintDarts"),
           ("Angreal", "T2"),
           ("WOT", "PixieStar")]

# Plain textures. Keeping this manifest explicit makes every runtime
# soft name produce both its texture payload and particle-compatible MIs; mesh
# and overlay consumers use the texture asset directly.
STATICS += (
    [("Angreal", f"Corona_{c}") for c in ("blue", "red", "green", "yellow", "purple")]
    + [("Angreal", f"Expl_{c}") for c in ("blue", "red", "green", "yellow", "purple")]
    + [("Angreal", f"Comet_{c}") for c in ("blue", "red", "green", "yellow", "purple")]
    + [("Angreal", f"Ref{m}{c}")
       for m in ("", "M") for c in ("Green", "Purple", "Red", "Blue", "Yellow", "White")]
    + [("Angreal", f"{element}_A{i:02d}")
       for element in ("Air", "Earth", "Fire", "Spirit", "Water") for i in range(1, 6)]
    + [("Angreal", "JFrozen1")]
    + [("Angreal", "Glass")]
    + [("Angreal", f"LSE_A{i:02d}") for i in range(1, 23)]
    + [("Angreal", f"Limpact_A{i:02d}") for i in range(1, 20)]
    + [("Angreal", f"LB{i}") for i in range(1, 11)]
    + [("Angreal", "LightningSpark04"), ("Angreal", "LightningSpark05")]
    + [("ParticleSystems", n) for n in (
        "PurpleCorona", "PurpleSpark", "WhiteCorona", "BlueSpark",
        "PF03", "PF04", "PF15",
        # Whirlwind, EarthTremor/AMA, TrapDetect, WOTSparks.
        "particle_fog01", "particle_fog02", "particle_fog03",
        "AOrangeCorona", "APinkCorona", "AYellowCorona", "Flame06",
        "Prtcl04", "Prtcl08",
        # AppearEffect: the remaining corona palette (rarity pairs, SetColors
        # Red/Green, Champion sparkle).
        "ABlueCorona", "RedCorona", "GreenCorona", "ZGreenCorona")]
    + [("Angreal", n) for n in (
        "TrapWallParts", "TrapPitParts", "TrapPortculParts",
        "TrapSpearParts", "TrapStairParts", "TrapExpWardPartsX")]
    # Light Sphere: the PixieSprayer color sparkle palette (32x32, four
    # slots x five colors) + the pickup's LightGlobeStar glow sprite.
    + [("Angreal", f"Spark0{i}{c}")
       for i in range(1, 5) for c in ("Blue", "Green", "Purple", "Red", "Yellow")]
    + [("Angreal", "LightGlobeGlow1")]
    # Seal carry: the CarrySealSprayer color fountains (128x128; the
    # CarryingSeal .utx group folds into the WOT package).
    + [("WOT", f"CSseal{c}") for c in ("Blue", "Green", "Purple", "Red", "Yellow")]
)
DECALS = [("ParticleSystems", "decaydrop"),
          ("ParticleSystems", "decaydrop1"),
          ("ParticleSystems", "decaysplat")]
MI_DEST = "/Game/WoT/Materials/Instances"

EAL = unreal.EditorAssetLibrary
ATT = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary


def neutral_texture():
    """The 0.5-display-gray linear default the particle masters' samplers need
    (same asset import_map.py's _import_neutral uses)."""
    full = "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral"
    if not EAL.does_asset_exist(full):
        task = unreal.AssetImportTask()
        task.filename = str(CONV / "tex" / "WOTDetail" / "T_WoT_Neutral.png")
        task.destination_path = "/Game/WoT/Textures/WOTDetail"
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    t = unreal.load_asset(full)
    if t.get_editor_property("srgb"):
        t.set_editor_property("srgb", False)
        EAL.save_asset(full)
    return t


def _material_has_scalar_param(m, pname):
    """True when the material declares the scalar param (the graph expression
    list is protected from python in UE 5.8; probe via the parameter query —
    a declared legacy FPS returns its authored 32 default, an absent one 0)."""
    return unreal.MaterialEditingLibrary.get_material_default_scalar_parameter_value(
        m, pname) != 0.0


def make_particle_fb_master(name, modulated):
    """M_WoT_ParticleAdd/Mod graph at identity UV (gamma-space display-byte
    pass-through, per-instance custom data 0 = ScaleGlow). Historically these
    carried the flipbook sheet scrub (Cols/Rows/Frames/FPS) — the sheet lane is
    RETIRED (PORT_NOTES "Procedural textures animate when recently seen"): the fractal subsystem binds
    the live procedural texture, which samples at plain frac(UV0). A legacy
    scrub graph (detected by its FPS param) is regenerated in place so existing
    MI_PS*FB_ instances keep their parent references."""
    full = f"/Game/WoT/Materials/{name}"
    if EAL.does_asset_exist(full):
        m = unreal.load_asset(full)
        if not _material_has_scalar_param(m, "FPS"):
            return m
        MEL.delete_all_material_expressions(m)
    else:
        m = ATT.create_asset(name, "/Game/WoT/Materials", unreal.Material,
                             unreal.MaterialFactoryNew())

    def expr(cls, x, y):
        return MEL.create_material_expression(m, cls, x, y)

    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property(
        "blend_mode",
        unreal.BlendMode.BLEND_MODULATE if modulated else unreal.BlendMode.BLEND_ADDITIVE)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("used_with_instanced_static_meshes", True)

    tex = expr(unreal.MaterialExpressionTextureSampleParameter2D, -220, 0)
    tex.set_editor_property("parameter_name", "Tex")
    tex.set_editor_property("texture", neutral_texture())
    tex.set_editor_property("sampler_type",
                            unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

    glow = expr(unreal.MaterialExpressionPerInstanceCustomData, -220, 300)
    glow.set_editor_property("data_index", 0)
    if modulated:
        # display space: FB *= 1 - Tex*glow
        srcd = expr(unreal.MaterialExpressionMultiply, -60, 150)
        MEL.connect_material_expressions(tex, "RGB", srcd, "A")
        MEL.connect_material_expressions(glow, "", srcd, "B")
        e = expr(unreal.MaterialExpressionOneMinus, 60, 150)
        MEL.connect_material_expressions(srcd, "", e, "")
    else:
        # display space: FB += Tex*glow
        e = expr(unreal.MaterialExpressionMultiply, -60, 150)
        MEL.connect_material_expressions(tex, "RGB", e, "A")
        MEL.connect_material_expressions(glow, "", e, "B")
    MEL.connect_material_property(e, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(m)
    EAL.save_asset(full)
    return m


def make_meshfb_glow_master():
    """M_WoT_MeshFBAddGlow: the additive identity-UV graph with a 'Glow' scalar
    param in place of PerInstanceCustomData — for MESH components (the Mashadar
    body's ScaleGlow fades ride a MID scalar). The historical sheet
    scrub is RETIRED; a legacy graph regenerates in place (see
    make_particle_fb_master)."""
    full = "/Game/WoT/Materials/M_WoT_MeshFBAddGlow"
    if EAL.does_asset_exist(full):
        m = unreal.load_asset(full)
        if not _material_has_scalar_param(m, "FPS"):
            return m
        MEL.delete_all_material_expressions(m)
    else:
        m = ATT.create_asset("M_WoT_MeshFBAddGlow", "/Game/WoT/Materials",
                             unreal.Material, unreal.MaterialFactoryNew())

    def expr(cls, x, y):
        return MEL.create_material_expression(m, cls, x, y)

    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("used_with_skeletal_mesh", True)
    m.set_editor_property("used_with_morph_targets", True)
    tex = expr(unreal.MaterialExpressionTextureSampleParameter2D, -220, 0)
    tex.set_editor_property("parameter_name", "Tex")
    tex.set_editor_property("texture", neutral_texture())
    tex.set_editor_property("sampler_type",
                            unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    glow = expr(unreal.MaterialExpressionScalarParameter, -220, 300)
    glow.set_editor_property("parameter_name", "Glow")
    glow.set_editor_property("default_value", 1.0)
    e = expr(unreal.MaterialExpressionMultiply, -60, 150)
    MEL.connect_material_expressions(tex, "RGB", e, "A")
    MEL.connect_material_expressions(glow, "", e, "B")
    MEL.connect_material_property(e, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(m)
    EAL.save_asset(full)
    return m


def main():
    add_fb = make_particle_fb_master("M_WoT_ParticleAddFB", False)
    mod_fb = make_particle_fb_master("M_WoT_ParticleModFB", True)
    mesh_fb = make_meshfb_glow_master()
    made = 0
    for pkg, name in SHEETS:
        # Sheet-scrub RETIRED: these MICs are pure identity children of the FB
        # masters — the fractal subsystem binds the live procedural texture at
        # runtime; the parent's neutral default is the (never-rendered)
        # pre-activation fallback. Restamp EXISTING instances too, dropping
        # legacy sheet Tex + grid overrides so nothing references
        # /Game/WoT/Textures/Flipbooks and the assets become deletable.
        for prefix, master in (("MI_PSAddFB_", add_fb), ("MI_PSModFB_", mod_fb),
                               ("MI_MeshFB_", mesh_fb)):
            mi_name = f"{prefix}{pkg}__{name}"
            mi_path = f"{MI_DEST}/{mi_name}"
            if not EAL.does_asset_exist(mi_path):
                mi = ATT.create_asset(mi_name, MI_DEST, unreal.MaterialInstanceConstant,
                                      unreal.MaterialInstanceConstantFactoryNew())
                made += 1
            else:
                mi = unreal.load_asset(mi_path)
            dirty = False
            if mi.get_editor_property("parent") != master:
                mi.set_editor_property("parent", master)
                dirty = True
            if mi.get_editor_property("texture_parameter_values")                     or mi.get_editor_property("scalar_parameter_values"):
                mi.set_editor_property("texture_parameter_values", [])
                mi.set_editor_property("scalar_parameter_values", [])
                dirty = True
            if dirty:
                EAL.save_asset(mi_path)

    # Static weave-trail textures: import + BOTH particle MI passes.
    add_master = unreal.load_asset("/Game/WoT/Materials/M_WoT_ParticleAdd")
    mod_master = unreal.load_asset("/Game/WoT/Materials/M_WoT_ParticleMod")
    if not add_master or not mod_master:
        raise RuntimeError("missing particle masters — run a map import first")
    for pkg, name in STATICS:
        tex_path = f"/Game/WoT/Textures/{pkg}/{name}"
        if not EAL.does_asset_exist(tex_path):
            png = CONV / "tex" / pkg / f"{name}.png"
            if not png.exists():
                raise RuntimeError(f"missing {png} — run utx2png")
            task = unreal.AssetImportTask()
            task.filename = str(png)
            task.destination_path = f"/Game/WoT/Textures/{pkg}"
            task.automated = True
            task.save = True
            ATT.import_asset_tasks([task])
        ensure_texture_payload_binding(unreal, tex_path, pkg, name)
        for prefix, master in (("MI_PSAdd_", add_master), ("MI_PSMod_", mod_master)):
            mi_path = f"{MI_DEST}/{prefix}{pkg}__{name}"
            if not EAL.does_asset_exist(mi_path):
                mi = ATT.create_asset(f"{prefix}{pkg}__{name}", MI_DEST,
                                      unreal.MaterialInstanceConstant,
                                      unreal.MaterialInstanceConstantFactoryNew())
                mi.set_editor_property("parent", master)
                MEL.set_material_instance_texture_parameter_value(
                    mi, "Tex", unreal.load_asset(tex_path))
                EAL.save_asset(mi_path)
                made += 1

    # The static smoke sprite (FireWallSprayer2, STY_Modulated) — the
    # import_gore_fx.py convention verbatim.
    # DecaySplatter's falling drop and landed splat use decal-domain MIs.
    decal_master = unreal.load_asset("/Game/WoT/Materials/M_WoT_Decal")
    if not decal_master:
        raise RuntimeError("missing M_WoT_Decal - run import_gore_fx.py first")
    for pkg, name in DECALS:
        decal_tex_path = f"/Game/WoT/Textures/{pkg}/{name}"
        decal_mi_name = f"MI_Decal_{pkg}__{name}"
        decal_mi_path = f"{MI_DEST}/{decal_mi_name}"
        if not EAL.does_asset_exist(decal_mi_path):
            mi = ATT.create_asset(decal_mi_name, MI_DEST,
                                  unreal.MaterialInstanceConstant,
                                  unreal.MaterialInstanceConstantFactoryNew())
            mi.set_editor_property("parent", decal_master)
            MEL.set_material_instance_texture_parameter_value(
                mi, "Tex", unreal.load_asset(decal_tex_path))
            EAL.save_asset(decal_mi_path)
            made += 1

    tex_path = "/Game/WoT/Textures/ParticleSystems/Blk64_005"
    if not EAL.does_asset_exist(tex_path):
        task = unreal.AssetImportTask()
        task.filename = str(SMOKE_PNG)
        task.destination_path = "/Game/WoT/Textures/ParticleSystems"
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    ensure_texture_payload_binding(
        unreal, tex_path, "ParticleSystems", "Blk64_005")
    mi_path = f"{MI_DEST}/MI_PSMod_ParticleSystems__Blk64_005"
    if not EAL.does_asset_exist(mi_path):
        master = unreal.load_asset("/Game/WoT/Materials/M_WoT_ParticleMod")
        if not master:
            raise RuntimeError("missing M_WoT_ParticleMod — run a map import first")
        mi = ATT.create_asset("MI_PSMod_ParticleSystems__Blk64_005", MI_DEST,
                              unreal.MaterialInstanceConstant,
                              unreal.MaterialInstanceConstantFactoryNew())
        mi.set_editor_property("parent", master)
        MEL.set_material_instance_texture_parameter_value(
            mi, "Tex", unreal.load_asset(tex_path))
        EAL.save_asset(mi_path)
        made += 1
    unreal.log(f"FLAME-FX-OK instances_created={made}")


main()
