"""Import the plain (non-procedural) weave sprite series.

The DT_Sprite weave projectiles cycle authored frame SERIES: EarthTremor
ETL_A01-30, Decay DC_A01-33, SwapPlaces SPP_A00-04, plus SoulBarb's static
MODdiskC. Procedural companions (Seeker SLightning01, SoulBarb NimbusAnim)
ride the flipbook pipeline (bake_flipbooks.py + import_flipbooks.py), not
this script. PNG (Intermediate/Conv/tex/Angreal) →
/Game/WoT/Textures/Angreal/<Name>. MIs land with the billboard consumer.
Every sprite samples raw (the effect family), existing assets included.
Reruns are idempotent.

UnrealEditor-Cmd WoT.uproject -run=pythonscript -script=".../import_weave_sprites.py"
"""
import os
import sys
from pathlib import Path

import unreal

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
from payload_binding import ensure_texture_payload_binding
import texture_sampling
from texture_sampling import EFFECT, import_texture

REPO = Path(__file__).resolve().parents[2]
CONV_ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else
                 os.environ.get("WOT_CONV_DIR", REPO / "Intermediate" / "Conv"))
CONV = CONV_ROOT / "tex" / "Angreal"
TEX_DEST = "/Game/WoT/Textures/Angreal"

NAMES = (
    [f"ETL_A{i:02d}" for i in range(1, 31)]
    + [f"DC_A{i:02d}" for i in range(1, 34)]
    + [f"SPP_A{i:02d}" for i in range(0, 5)]
    + ["MODdiskC"]
    # The Explosion frame series (impact bursts).
    + [f"FBExp1{i:02d}" for i in range(0, 18)]
    + [f"FBExp2{i:02d}" for i in range(0, 18)]
    + [f"FBExp4{i:02d}" for i in range(0, 17)]
    + [f"FBExp5{i:02d}" for i in range(0, 17)]
    + [f"DecayExplosion{i:02d}" for i in range(0, 10)]
    + ["SB_A00", "SG_A00"]
    + [f"FBFiz1{i:02d}" for i in range(0, 19)]
    # Balefire gather hand + BFExplodeA one-shot.
    + ["BFHand_A01"]
    + [f"BFimpact1{i:02d}" for i in range(1, 22)]
)

EAL = unreal.EditorAssetLibrary
ATT = unreal.AssetToolsHelpers.get_asset_tools()


def texture_sampling_claims(_context=None):
    """Every sprite frame this importer acquires: effect textures, raw."""
    return [texture_sampling.claim(f"{TEX_DEST}/{name}", EFFECT,
                                   "import_weave_sprites")
            for name in NAMES]


def main():
    made = 0
    for name in NAMES:
        tex_path = f"{TEX_DEST}/{name}"
        png = CONV / f"{name}.png"
        if not EAL.does_asset_exist(tex_path):
            if not png.exists():
                raise RuntimeError(f"missing {png} — run utx2png on Angreal.u")
            made += 1
        import_texture(unreal, png, tex_path, EFFECT)
        ensure_texture_payload_binding(unreal, tex_path, "Angreal", name)
    unreal.log(f"WEAVE-SPRITES-OK imported={made} of {len(NAMES)}")


if __name__ == "__main__":
    main()
