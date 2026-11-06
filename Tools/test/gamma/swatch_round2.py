"""Make the swatch master two-sided; optionally (argv[1]=='ppv') add the
gamma-boundary PPV to the GammaSwatch level for the net-transform run."""
import sys

import unreal

EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

m = unreal.load_asset("/Game/WoT/Materials/M_WoT_Swatch")
m.set_editor_property("two_sided", True)
MEL.recompile_material(m)
EAL.save_asset("/Game/WoT/Materials/M_WoT_Swatch")
unreal.log("SWATCH2 two-sided ok")

if len(sys.argv) > 1 and sys.argv[1] == "ppv":
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level("/Game/WoT/Maps/GammaSwatch"):
        raise RuntimeError("load failed")
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not any(a.get_actor_label() == "GammaBoundary"
               for a in eas.get_all_level_actors()):
        ppv = eas.spawn_actor_from_class(unreal.PostProcessVolume,
                                         unreal.Vector(0, 0, 0))
        ppv.set_actor_label("GammaBoundary")
        ppv.set_editor_property("unbound", True)
        pset = ppv.get_editor_property("settings")
        pset.set_editor_property(
            "weighted_blendables",
            unreal.WeightedBlendables(array=[unreal.WeightedBlendable(
                1.0, unreal.load_asset(
                    "/Game/WoT/Materials/Instances/MI_GammaBoundary"))]))
        ppv.set_editor_property("settings", pset)
        les.save_current_level()
        unreal.log("SWATCH2 ppv added")
unreal.log("SWATCH2-DONE")
