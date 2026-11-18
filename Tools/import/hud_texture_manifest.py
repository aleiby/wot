"""The HUD registry's registry-only texture inputs (pure data).

import_hud_textures.py adds these existing assets to DA_WoTHUDTextures without
importing them: import_map.py's code-package skin pass produces them beside the
world textures under /Game/WoT/Textures. Both importers read this one list, so
the skin pass acquires them under the HUD's UI sampling policy (srgb on, as the
release HUD resolves them) instead of the world policy of their neighbours.
"""

# D13 registry-only inputs. These assets are produced by the existing WOT and
# Angreal texture passes; the HUD importer must neither extract nor import them.
STATUS_ICON_FRAME_NAMES = (
    "M_Absorb", "M_AirBurst", "M_AndilayRoot", "M_AntiMagicAura",
    "M_Balefire", "M_Champion", "M_Dartwh", "M_Decay", "M_Disguise",
    "M_DistantEye", "M_EarthTremor", "M_ElShieldBlue", "M_ElShieldGold",
    "M_ElShieldGreen", "M_ElShieldRed", "M_ElShieldWhite", "M_Fireball",
    "M_Fork", "M_Freeze", "M_Guardian", "M_Healing", "M_LandMine",
    "M_Levitation", "M_LightningStrike", "M_LtGlobe", "M_Minion",
    "M_PersonalIllusion", "M_Reflect", "M_RemoveCurse", "M_Seeker",
    "M_Shield", "M_Shift", "M_SoulBarb", "M_Special", "M_SwapPlaces",
    "M_Taint", "M_Targeting", "M_Tracer", "M_TrapDetection",
    "M_WallOfAir", "M_Whirlwind",
)
REGISTRY_ONLY_TEXTURES = (
    [(f"T32_{index:02d}", f"/Game/WoT/Textures/WOT/T32_{index:02d}")
     for index in range(1, 20)] +
    [(name, f"/Game/WoT/Textures/Angreal/{name}")
     for name in STATUS_ICON_FRAME_NAMES] +
    # MyrddraalSwordAngreal's displayed DecreaseHealthLeech falls back to this
    # non-Angreal source frame (MyrddraalSwordAngreal.uc:20).
    [("M_SMyrddraal", "/Game/WoT/Textures/WoTPawns/M_SMyrddraal")]
)
# Case-folded asset paths, for the skin pass's per-texture family choice.
REGISTRY_ONLY_ASSET_PATHS = frozenset(
    asset_path.casefold() for _, asset_path in REGISTRY_ONLY_TEXTURES)
