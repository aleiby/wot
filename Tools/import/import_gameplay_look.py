"""Gameplay post-process look — match the retail golden in real play + PIE.

The offscreen metric capture (WoTShotSubsystem) matches the retail golden by
forcing ToneCurveAmount=0 + ExpandGamut=0 on its camera: r.Tonemapper.Quality=0
does NOT remove the filmic S-curve in UE 5.8 (measured via emissive swatches),
so the DEFAULT player view — real gameplay AND PIE — renders WITH the S-curve +
gamut expansion, i.e. crushed shadows / darker vs the flat-sRGB golden. This
patcher places ONE UNBOUND PostProcessVolume per map carrying those same two
overrides, so gameplay / PIE / packaged all render the intended period look
identical to the capture rig (and to each other). Exposure stays on the
r.DefaultFeature.AutoExposure=False cvars, exactly as the capture rig relied on.

Idempotent IN PLACE: an existing labeled PPV is mutated, not delete+respawned,
so no-op re-runs don't churn actor GUIDs in the umap. import_map.py imports
apply_gameplay_look() as its final import stage (so a map re-import can never
silently lose the volume); this CLI patches ALREADY-IMPORTED levels in place —
no BSP/lighting re-import:

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_gameplay_look.py [<Map> ...]"   (default: the 19 MP maps)
"""
import sys
import unreal

MP_MAPS = [f"Arena_{i:02d}" for i in (1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12)] + \
          [f"Battle2_{i:02d}" for i in range(1, 6)] + \
          ["Battle3_01", "Battle3_02", "Battle4_01"]

LABEL = "WoTGameplayLook"


def apply_gameplay_look(eas):
    """Ensure the CURRENT level carries the WoTGameplayLook PPV (unbound,
    priority 1.0, ToneCurveAmount=0 + ExpandGamut=0). Mutates an existing
    labeled volume in place (stray duplicates/mislabels are dropped); spawns
    one otherwise. Returns 'spawned' or 'updated'. Caller saves the level."""
    ppv = None
    for act in eas.get_all_level_actors():
        if act.get_actor_label() != LABEL:
            continue
        if ppv is None and isinstance(act, unreal.PostProcessVolume):
            ppv = act
        else:
            eas.destroy_actor(act)
    spawned = ppv is None
    if spawned:
        ppv = eas.spawn_actor_from_class(unreal.PostProcessVolume,
                                         unreal.Vector(0, 0, 0))
        ppv.set_actor_label(LABEL)
    ppv.set_editor_property("unbound", True)      # applies everywhere, extent-free
    ppv.set_editor_property("priority", 1.0)
    # World Partition: an unbound global-look volume must never stream out.
    try:
        ppv.set_editor_property("is_spatially_loaded", False)
    except Exception:
        pass
    s = ppv.get_editor_property("settings")
    s.set_editor_property("override_tone_curve_amount", True)
    s.set_editor_property("tone_curve_amount", 0.0)
    s.set_editor_property("override_expand_gamut", True)
    s.set_editor_property("expand_gamut", 0.0)
    ppv.set_editor_property("settings", s)
    return "spawned" if spawned else "updated"


def main():
    maps = [m for m in sys.argv[1:] if not m.startswith("-")] or MP_MAPS
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for map_name in maps:
        asset = f"/Game/WoT/Maps/{map_name}"
        if not unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.log(f"GAMEPLAYLOOK-SKIP {map_name}: no imported map")
            continue
        if not les.load_level(asset):
            # An unchecked failure here would patch whatever level happens to
            # be current and log success against the wrong map.
            unreal.log_error(f"GAMEPLAYLOOK-FAIL {map_name}: load_level failed")
            continue
        res = apply_gameplay_look(eas)
        les.save_current_level()
        unreal.log(f"GAMEPLAYLOOK-OK {map_name}: unbound PPV "
                   f"(ToneCurveAmount=0, ExpandGamut=0, {res})")


if __name__ == "__main__":
    main()
