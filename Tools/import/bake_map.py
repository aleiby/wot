"""Drive a GPU Lightmass bake of Arena_01 from an offscreen editor session.

Launch:
UnrealEditor-Cmd WoT.uproject -dx12 -RenderOffscreen -unattended -nosplash -nosound -stdout
    -ExecCmds="py .../bake_gpulm.py"

-dx12 is required: Windows defaults to D3D11 (Config/Windows/WindowsEngine.ini)
and GPU Lightmass needs the D3D12 SM6/DXR shaders. Since 2026-10-13 the
project cooks D3D12 at SM5 with r.RayTracing=False (download size; the decoded
UE1 lightmaps superseded this bake), so restore both settings locally first.

UGPULightmassSubsystem is a WORLD subsystem (GPULightmassSettings.h):
reach it via SubsystemBlueprintLibrary with the editor world as context.
"""
import sys

import unreal

MAP_PATH = f"/Game/WoT/Maps/{sys.argv[1]}" if len(sys.argv) > 1 else "/Game/WoT/Maps/Arena_01"
state = {"phase": "init", "ticks": 0, "handle": None, "settle_time": 0.0, "elapsed": 0.0}


def get_sub():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    # route 1: blueprint subsystem library (name varies across versions)
    for lib_name in ("SubsystemBlueprintLibrary", "SubsystemLibrary"):
        lib = getattr(unreal, lib_name, None)
        if lib and hasattr(lib, "get_world_subsystem"):
            sub = lib.get_world_subsystem(world, unreal.GPULightmassSubsystem)
            if sub:
                unreal.log(f"BAKE subsystem via {lib_name}")
                return sub
    # route 2: method on the world object
    for m in ("get_subsystem", "get_world_subsystem"):
        fn = getattr(world, m, None)
        if fn:
            try:
                sub = fn(unreal.GPULightmassSubsystem)
                if sub:
                    unreal.log(f"BAKE subsystem via world.{m}")
                    return sub
            except Exception:
                pass
    # route 3: module-level helper
    fn = getattr(unreal, "get_world_subsystem", None)
    if fn:
        try:
            sub = fn(world, unreal.GPULightmassSubsystem)
            if sub:
                unreal.log("BAKE subsystem via unreal.get_world_subsystem")
                return sub
        except Exception:
            pass
    # route 4: object iterator over live instances, match this world
    it = getattr(unreal, "ObjectIterator", None)
    if it:
        for o in it(unreal.GPULightmassSubsystem):
            outer = o.get_outer()
            if outer == world or (outer and outer.get_outer() == world):
                unreal.log("BAKE subsystem via ObjectIterator")
                return o
        for o in it(unreal.GPULightmassSubsystem):
            unreal.log(f"BAKE candidate instance outer={o.get_outer()}")
    unreal.log(f"BAKE module subsystem helpers: "
               f"{[n for n in dir(unreal) if 'ubsystem' in n]}")
    return None


def finish(ok, msg):
    unreal.log(f"BAKE-{'OK' if ok else 'FAIL'} {msg}")
    if state["handle"] is not None:
        unreal.unregister_slate_post_tick_callback(state["handle"])
        state["handle"] = None
    unreal.SystemLibrary.quit_editor()


def drive():
    if state["phase"] == "init":
        if state["ticks"] < 30:
            return
        les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not les.load_level(MAP_PATH):
            finish(False, "load_level failed")
            return
        unreal.log("BAKE level loaded")
        state["phase"] = "level_settle"
        state["settle_time"] = state["elapsed"]
        return

    if state["phase"] == "level_settle":
        # time-based warm-up: the offscreen viewport renders the level each
        # tick, forcing on-demand material/RT shader compiles. Launching while
        # those are in flight trips the GeometrySegmentIndex assert in
        # GPULightmass' cached RT command build.
        if state["elapsed"] - state["settle_time"] < 150.0:
            if state["ticks"] % 600 == 0:
                unreal.log(f"BAKE warming shaders {state['elapsed'] - state['settle_time']:.0f}s")
            return
        sub = get_sub()
        if sub is None:
            finish(False, "GPULightmassSubsystem unavailable for editor world")
            return
        settings = sub.get_settings()
        settings.set_editor_property("mode", unreal.GPULightmassMode.FULL_BAKE)
        settings.set_editor_property("show_progress_bars", False)
        unreal.log(f"BAKE settings: mode={settings.get_editor_property('mode')} "
                   f"gi_samples={settings.get_editor_property('gi_samples')} "
                   f"denoiser={settings.get_editor_property('denoising_options')}")
        sub.set_realtime(False)
        # phase advances BEFORE launch: launch() pumps slate internally and this
        # callback re-enters â€” a second launch() asserts in GPULightmassModule
        state["phase"] = "baking"
        state["seen_running"] = False
        state["baking_since"] = state["ticks"]
        sub.launch()
        unreal.log("BAKE launched")
        return

    if state["phase"] == "baking":
        sub = get_sub()
        running = sub.is_running()
        if running:
            state["seen_running"] = True
        if state["ticks"] % 600 == 0:
            unreal.log(f"BAKE progress running={running} pct={sub.get_percentage()}")
        if state["seen_running"] and not running:
            unreal.log(f"BAKE build ended (pct={sub.get_percentage()})")
            state["phase"] = "save"
        elif not state["seen_running"] and state["ticks"] - state["baking_since"] > 6000:
            finish(False, "build never started within tick budget")
        return

    if state["phase"] == "save":
        try:
            get_sub().save()
        except Exception as e:
            unreal.log(f"BAKE subsystem save failed (continuing): {e}")
        les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        saved = les.save_current_level()
        unreal.EditorAssetLibrary.save_directory("/Game/WoT/Maps", recursive=False)
        finish(True, f"level saved={saved}")


def tick(dt):
    state["ticks"] += 1
    state["elapsed"] += dt
    if state.get("busy"):
        return
    state["busy"] = True
    try:
        drive()
    except Exception as e:
        finish(False, f"exception: {e}")
    finally:
        state["busy"] = False


state["handle"] = unreal.register_slate_post_tick_callback(tick)
unreal.log("BAKE tick registered")

