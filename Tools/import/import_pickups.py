"""Generate pickup leaf BPs: one Blueprint per ported inventory artifact
whose inherited Mesh (UWoTVertexAnimComponent) carries the UE1 PickupViewMesh, so a
pool-spawned pickup renders. This is the ground-rule-2 "generated leaf BP binds the
asset" path — the artifact C++ stays asset-free; the mesh lives on the BP CDO.

UnrealEditor-Cmd WoT.uproject -run=pythonscript -script="...import_pickups.py"

The pickup mesh + DrawScale per artifact are the leaf's defaultproperties Mesh= /
DrawScale= (exports/script/Angreal/*.uc, resolved through the parent chain). Each
imported vertex-anim mesh lands at Meshes/<Name>/<Name>/SkeletalMeshes/<Name> (the
deterministic GLB-Interchange nesting import_mesh.py produces). BPs are written to
/Game/WoT/Blueprints/Pickups/BP_<Class>; load-or-create keeps re-runs idempotent
(delete+recreate in one editor session intermittently returns a null asset).
"""
import sys

import unreal

ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary
PKG = "/Game/WoT/Blueprints/Pickups"

# ue5 artifact class -> (UE1 PickupViewMesh name, DrawScale). Values verbatim from
# the leaf .uc (parent-resolved), except that bMeshCurvy selects the generated
# _Curvy presentation variant. DrawScale defaults to 1.0 where the leaf is silent.
PICKUPS = {
    "WoTAngrealInvFireball":     ("AngrealFireballPickup",       1.3),
    "WoTAngrealInvFireworks":    ("AngrealFireballPickup",       1.3),   # shares Fireball's mesh
    "WoTAngrealInvSeeker":       ("AngrealSeekerPickup",         1.0),
    "WoTAngrealInvTaint":        ("AngrealTaintPickup",          1.0),
    "WoTAngrealInvDart":         ("AngrealInventoryDart",        0.6),
    "WoTAngrealInvIce":          ("Freeze",                      0.3),
    "WoTAngrealInvDecay":        ("AngrealDecayPickup",          0.75),
    "WoTAngrealInvSoulBarb":     ("AngrealSoulBarbPickup",       1.0),
    "WoTAngrealInvHeal":         ("AngrealHealPickup",           1.0),
    "WoTAngrealInvLevitate":     ("AngrealLevitatePickup",       1.0),
    "WoTAngrealInvLightGlobe":   ("LightGlobe",                  1.0),
    "WoTSeal":                   ("Seal",                        0.5),
    "WoTAngrealInvWhirlwind":    ("AngrealWhirlwindPickup",      1.0),
    "WoTAngrealInvShift":        ("AngrealShiftPickup",          0.8),
    "WoTAngrealInvEarthTremor":  ("AngrealEarthTremorPickup",    1.0),
    "WoTAngrealInvBalefire":     ("Balefire",                    0.7),
    "WoTAngrealInvSwapPlaces":   ("AngrealSwapPlacesPickup",     0.8),
    "WoTAngrealInvReflect":      ("AngrealReflectPickup",        1.2),
    "WoTAngrealInvFork":         ("AngrealForkPickup",           1.0),
    "WoTAngrealInvAbsorb":       ("AngrealAbsorbPickup",         1.0),
    "WoTAngrealInvAirBurst":     ("AngrealAirBurst",             0.3),
    "WoTAngrealInvRemoveCurse":  ("AngrealRemoveCursePickup",    0.8),
    "WoTAngrealInvAMA":          ("AngrealAntiMagicAuraPickup",  1.0),
    "WoTAngrealInvExpWard":      ("AngrealLandMinePickup",       0.5),
    "WoTAngrealInvSpecial":      ("AngrealSpecial",              0.7),
    "WoTAngrealInvTarget":       ("AngrealTargetingPickup",      0.8),
    "WoTAngrealInvTrapDetect":   ("AngrealTrapDetectionPickup",  1.0),
    "WoTAngrealInvWallOfAir":    ("WallAir",                     0.5),
    "WoTAngrealInvTracer":       ("AngrealTracerPickup",         1.0),
    "WoTAngrealInvLightning":    ("AngrealLightningStrikePickup", 1.0),
    "WoTAngrealInvChampion":     ("AngrealChampionPickup",       0.5),
    "WoTAngrealInvGuardian":     ("AngrealGuardianPickup",       0.4),
    "WoTAngrealInvMinion":       ("AngrealMinionPickup",         0.8),
    "WoTAngrealInvIllusion":     ("AngrealPersonalIllusionPickup", 1.0),
    "WoTAngrealInvDistantEye":   ("AngrealDistantEyePickup",      0.7),
    "WoTAngrealInvDisguise":     ("AngrealDisguisePickup",       0.7),
    "WoTAngrealInvShield":       ("AngrealShieldPickup_Curvy",   0.8),
    "WoTAngrealInvFireShield":   ("AngrealElementalShield",      0.3),
    "WoTAngrealInvAirShield":    ("AngrealElementalShield",      0.3),
    "WoTAngrealInvWaterShield":  ("AngrealElementalShield",      0.3),
    "WoTAngrealInvEarthShield":  ("AngrealElementalShield",      0.3),
    "WoTAngrealInvSpiritShield": ("AngrealElementalShield",      0.3),
    "WoTAndilayRoot":            ("AndilayRoot",                 1.0),
}

# Classes whose leaf .uc supplies the pickup texture via Skin= with
# Style=STY_Masked (exports/script/Angreal/AngrealInv{Dart,*Shield}.uc). UE1's
# mesh GetTexture fills texture-less slots (and slot 0) from actor Skin, so
# the imported mesh's slot MIC (MI_tex0_2S / MI_tex1) is deliberately neutral
# and the skin must ride the leaf BP as a slot-0 override material parented to
# the masked mesh master (STY_Masked ExtraFlags; retail masked mesh tris are
# two-sided, PORT_NOTES C02). KeyColor is each texture's palette[0], probed
# offline from original/System/Angreal.u with Tools/convert/ue1pkg (the
# import_seal_skins.py precedent). AngrealInvTracer also authors Skin= but has
# no Style override and the ordinary mesh path never inherits texture flags
# (PORT_NOTES C02): retail drew its embedded slot texture — the same asset the
# Skin= names — through the opaque reconstruct path, so it takes no override.
# ue5 class -> (skin texture under /Game/WoT/Textures/Angreal, palette[0] RGB)
MASKED_SKINS = {
    "WoTAngrealInvDart":         ("DartAesSedai",  (0, 0, 0)),
    "WoTAngrealInvFireShield":   ("ElShieldRED",   (0, 0, 0)),
    "WoTAngrealInvAirShield":    ("ElShieldWHITE", (0, 0, 0)),
    "WoTAngrealInvWaterShield":  ("ElShieldBLUE",  (0, 0, 0)),
    "WoTAngrealInvEarthShield":  ("ElShieldGREEN", (0, 0, 0)),
    "WoTAngrealInvSpiritShield": ("ElShieldGOLD",  (0, 0, 0)),
}

MASKED_MASTER = "/Game/WoT/Materials/M_WoT_MeshLitMasked"
MI_DEST = "/Game/WoT/Materials/Instances"


def masked_skin_mic(texture_name, key_rgb):
    """Load-or-create the masked skin-override MIC for one Skin= texture.

    Follows the _ml_skin_override asset convention (MI_Skin_<tex><type>) so a
    map-instance Skin= of the same texture converges on the same MIC. The
    base PNG of a bMasked texture already carries index-zero alpha (utx2png),
    so no _key0 sibling is involved."""
    texture = unreal.load_asset(f"/Game/WoT/Textures/Angreal/{texture_name}")
    if texture is None:
        return None
    master = unreal.load_asset(MASKED_MASTER)
    if master is None:
        return None
    mic_path = f"{MI_DEST}/MI_Skin_{texture_name}_M"
    if EAL.does_asset_exist(mic_path):
        mic = unreal.load_asset(mic_path)
    else:
        mic = ATT.create_asset(
            f"MI_Skin_{texture_name}_M", MI_DEST,
            unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew())
    if mic is None:
        return None
    if mic.get_editor_property("parent") != master:
        mic.set_editor_property("parent", master)
    MEL = unreal.MaterialEditingLibrary
    MEL.set_material_instance_texture_parameter_value(mic, "Tex", texture)
    red, green, blue = key_rgb
    MEL.set_material_instance_vector_parameter_value(
        mic, "KeyColor",
        unreal.LinearColor(red / 255.0, green / 255.0, blue / 255.0, 1.0))
    EAL.save_asset(mic_path)
    return unreal.load_asset(mic_path)


def resolve_mesh(name):
    """The imported SkeletalMesh for a UE1 mesh name, or None. Deterministic nesting
    first (Meshes/<Name>/<Name>/SkeletalMeshes/<Name>), asset-registry scan as a
    fallback for any mesh whose GLB scene node was named differently."""
    path = f"/Game/WoT/Meshes/{name}/{name}/SkeletalMeshes/{name}.{name}"
    m = unreal.load_asset(path)
    if isinstance(m, unreal.SkeletalMesh):
        return m
    base = f"/Game/WoT/Meshes/{name}"
    if EAL.does_directory_exist(base):
        for a in EAL.list_assets(base, recursive=True):
            o = unreal.load_asset(a.split(".")[0])
            if isinstance(o, unreal.SkeletalMesh):
                return o
    return None


def bind_pickup(ue5_class, mesh_name, draw_scale):
    parent = getattr(unreal, ue5_class, None)
    if parent is None:
        return "no-class"
    skel = resolve_mesh(mesh_name)
    if skel is None:
        return "no-mesh"
    bp_path = f"{PKG}/BP_{ue5_class}"
    if EAL.does_asset_exist(bp_path):
        bp = unreal.load_asset(bp_path)
    else:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", parent)
        bp = ATT.create_asset(f"BP_{ue5_class}", PKG, unreal.Blueprint, factory)
    if bp is None:
        return "create-failed"
    cdo = unreal.get_default_object(bp.generated_class())
    comp = cdo.get_editor_property("mesh")
    comp.set_skeletal_mesh_asset(skel)
    comp.set_relative_scale3d(unreal.Vector(draw_scale, draw_scale, draw_scale))
    if ue5_class in MASKED_SKINS:
        texture_name, key_rgb = MASKED_SKINS[ue5_class]
        mic = masked_skin_mic(texture_name, key_rgb)
        if mic is None:
            return "no-skin"
        comp.set_editor_property("override_materials", [mic])
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    EAL.save_asset(bp_path)
    return "ok"


def main():
    only = set(sys.argv[1:])   # optional: limit to named artifact classes
    ok = missing = failed = 0
    for ue5_class, (mesh_name, ds) in PICKUPS.items():
        if only and ue5_class not in only:
            continue
        status = bind_pickup(ue5_class, mesh_name, ds)
        if status == "ok":
            ok += 1
        elif status == "no-mesh":
            missing += 1
            unreal.log_warning(f"IMPORT-PICKUP-NOMESH {ue5_class} mesh={mesh_name}")
        else:
            failed += 1
            unreal.log_warning(f"IMPORT-PICKUP-FAIL {ue5_class}: {status}")
    unreal.log(f"IMPORT-PICKUPS-OK bound={ok} missing={missing} failed={failed}")
    if failed:
        raise RuntimeError(f"{failed} pickup BP(s) failed to generate")


main()
