"""Create the tiny code-driven front-end map if it does not already exist.

The map intentionally contains no authored actors. Its WorldSettings select
AWoTFrontEndGameMode, whose local controller owns the Slate root.

A normal world save is not byte-deterministic (DateModified and generated GUIDs
can change), so skip-if-exists is the default and keeps routine reruns churn-free.
Pass --force only when the existing map intentionally needs to be regenerated.

Headless, idempotent invocation:
  UnrealEditor-Cmd WoT.uproject -run=pythonscript \
    -script=".../Tools/import/import_frontend_map.py"

Forced regeneration:
  UnrealEditor-Cmd WoT.uproject -run=pythonscript \
    -script=".../Tools/import/import_frontend_map.py --force"

Build the C++ module first so /Script/WoT.WoTFrontEndGameMode is registered.
"""
import sys

import unreal


MAP_PATH = "/Game/WoT/Maps/WoTFrontEnd"
GAME_MODE_PATH = "/Script/WoT.WoTFrontEndGameMode"


def main(force=False):
    assets = unreal.EditorAssetLibrary
    if assets.does_asset_exist(MAP_PATH) and not force:
        unreal.log(f"FRONTEND-MAP-SKIP {MAP_PATH} already exists (use --force)")
        return

    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if assets.does_asset_exist(MAP_PATH):
        if not level_editor.load_level(MAP_PATH):
            raise RuntimeError(f"failed to load existing {MAP_PATH}")
        # Forced regeneration removes anything an older generator left behind.
        # WorldSettings itself is not returned here.
        actor_editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        for actor in actor_editor.get_all_level_actors():
            actor_editor.destroy_actor(actor)
    elif not level_editor.new_level(MAP_PATH):
        raise RuntimeError(f"failed to create {MAP_PATH}")

    game_mode = unreal.load_class(None, GAME_MODE_PATH)
    if game_mode is None:
        raise RuntimeError(
            f"missing {GAME_MODE_PATH}; build the WoT module before generation")

    world = unreal.get_editor_subsystem(
        unreal.UnrealEditorSubsystem).get_editor_world()
    if world is None:
        raise RuntimeError("front-end map has no editor world")
    world_settings = world.get_world_settings()
    world_settings.set_editor_property("default_game_mode", game_mode)

    if not level_editor.save_current_level():
        raise RuntimeError(f"failed to save {MAP_PATH}")
    if not assets.does_asset_exist(MAP_PATH):
        raise RuntimeError(f"saved front-end map is not registered at {MAP_PATH}")
    unreal.log(f"FRONTEND-MAP-OK {MAP_PATH} game_mode={GAME_MODE_PATH}")


if __name__ == "__main__":
    main(force="--force" in sys.argv[1:])
