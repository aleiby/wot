"""Pipeline smoke test: proves UnrealEditor-Cmd -run=pythonscript works headless."""
import unreal

unreal.log(f"WOT-PIPELINE-OK engine={unreal.SystemLibrary.get_engine_version()}")
