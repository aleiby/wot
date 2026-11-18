"""Regenerate the display-space gamma boundary (MPC_WoTDisplay, M_WoT_GammaBoundary,
MI_GammaBoundary) in place, without a map import.

make_gamma_boundary() rebuilds the graph deterministically (the exact Glide display ramp:
pow(D, RampInvGamma) before the sRGB decode; RampInvGamma defaults to 1/1.4, Brightness
0.6), so this commandlet is idempotent: a second run leaves identical asset hashes. Every
lane must run it after pulling a change to the boundary, because /Content is generated
and not versioned.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script=".../regen_gamma_boundary.py" -unattended -nullrhi -stdout

Writes Saved/regen_gamma_boundary_result.json (commandlet stdout is unreliable; the
result file is the verification surface).
"""

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
REPO = HERE.parents[1]
RESULT_FILE = REPO / "Saved" / "regen_gamma_boundary_result.json"

import unreal  # noqa: E402

import import_map  # noqa: E402  (CLI is __main__-guarded; import is inert)

ASSETS = (
    "/Game/WoT/Materials/MPC_WoTDisplay",
    "/Game/WoT/Materials/M_WoT_GammaBoundary",
    "/Game/WoT/Materials/Instances/MI_GammaBoundary",
)


def main():
    result = {"ok": False, "assets": {}}
    import_map.make_gamma_boundary()
    for path in ASSETS:
        result["assets"][path] = unreal.EditorAssetLibrary.does_asset_exist(path)
    result["ok"] = all(result["assets"].values())
    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULT_FILE.write_text(json.dumps(result, indent=1), encoding="utf-8")
    unreal.log(f"regen_gamma_boundary ok={result['ok']}")


main()
