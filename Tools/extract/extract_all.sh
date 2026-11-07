#!/usr/bin/env bash
# Regenerates exports/ from original/ using the shipped WoT tools under Wine.
#
# Requirements (ARM64 Linux): Hangover wine (tested 11.9, Ubuntu 24.04 debs from
# github.com/AndreRH/hangover). On x86 Linux/Windows, plain wine / native works.
#
# Quirks discovered (2026-07-01, see PORT_NOTES.md):
#  - UCC.exe MakeDirectory fails on relative output paths under Wine; absolute
#    C:\ paths work. We export into the wineprefix's drive_c and copy out.
#  - Map loading needs the full sibling tree (Textures/, Sounds/) per WoT.ini
#    Paths=..\* entries — run from a complete copy of original/, never in-place.
#  - Core/Editor/Engine/IpDrv/WinDrv shipped with stripped ScriptText (Epic
#    engine code); class export fails for them. All Legend game packages export
#    completely. Stripped packages have no shipped source to export.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
REFERENCE_ROOT="${WOT_REFERENCE_ROOT:-$ROOT}"
if [ ! -d "$REFERENCE_ROOT" ]; then
  echo "missing reference root $REFERENCE_ROOT; set WOT_REFERENCE_ROOT to the private root" >&2
  exit 2
fi
REFERENCE_ROOT="$(cd "$REFERENCE_ROOT" && pwd)"
ORIGINAL="$REFERENCE_ROOT/original"
EXPORTS="$REFERENCE_ROOT/exports"
if [ ! -d "$ORIGINAL" ]; then
  echo "missing retail input $ORIGINAL; set WOT_REFERENCE_ROOT to the private root" >&2
  exit 2
fi
WORK="${WORK:-$(mktemp -d)}"
export WINEPREFIX="$WORK/wineprefix" WINEDEBUG=-all DISPLAY=

echo "work dir: $WORK"
mkdir -p "$WORK/wot"
for d in System Maps Textures Sounds Music; do cp -a "$ORIGINAL/$d" "$WORK/wot/$d"; done
cd "$WORK/wot/System"
EXP="$WINEPREFIX/drive_c/exp"

# 1. UnrealScript: batchexport every game package (skip known-stripped engine pkgs)
PKGS="Angreal Environment Fire IpServer Legend MachinShin ParticleSystems \
UBrowser UWindow WOT WoTBase WoTBrowser WoTDecorations WoTPawns WoTTraps"
for p in $PKGS; do
  wine UCC.exe batchexport "$p.u" class .uc "C:\\exp\\$p" >/dev/null 2>&1 \
    || { echo "FAIL: $p"; exit 1; }
  echo "$p: $(ls "$EXP/$p" | wc -l) classes"
done

# 1.5 Sounds: decode embedded package audio + the standalone .uax packages
# directly in Python (artifact/impact/UI sounds).
# This step does not use UCC/Wine. The exporter writes flat <Name>.wav per
# package; Package.Group.Name → asset-path mapping comes from exports/inventory
# (inventory.py).
export_sound_package() {
  local package_path="$1" package_name="$2" out="$EXP/sounds/$2" count
  python3 "$ROOT/Tools/convert/ue1pkg.py" sounds "$package_path" "$out" >/dev/null
  count=$(find "$out" -maxdepth 1 -type f -name '*.wav' | wc -l)
  if [ "$count" -gt 0 ]; then
    echo "sounds: $package_name $count"
  else
    rmdir "$out"
  fi
}
for p in $PKGS; do
  export_sound_package "$WORK/wot/System/$p.u" "$p"
done
for s in ../Sounds/*.uax; do
  n=$(basename "$s" .uax)
  export_sound_package "$s" "$n"
done

# 2. Maps: headless T3D export (each run writes MyLevel.t3d; rename per map)
mkdir -p "$EXP/t3d"
for m in ../Maps/*.wot; do
  n=$(basename "$m" .wot); rm -f "$EXP/t3d/MyLevel.t3d"
  wine UCC.exe batchexport "$n.wot" level t3d 'C:\exp\t3d' >/dev/null 2>&1 \
    && [ -s "$EXP/t3d/MyLevel.t3d" ] || { echo "FAIL: $n"; exit 1; }
  mv "$EXP/t3d/MyLevel.t3d" "$EXP/t3d/$n.t3d"; echo "t3d: $n"
done

# 3. Copy into repo
mkdir -p "$EXPORTS/script" "$EXPORTS/t3d"
for p in $PKGS; do rm -rf "$EXPORTS/script/$p"; cp -a "$EXP/$p" "$EXPORTS/script/"; done
cp "$EXP/t3d"/*.t3d "$EXPORTS/t3d/"
if [ -d "$EXP/sounds" ]; then
  rm -rf "$EXPORTS/sounds"; cp -a "$EXP/sounds" "$EXPORTS/sounds"
fi

# 4. Inventory + cross-diff + MP scope
python3 "$ROOT/Tools/extract/inventory.py"
echo "done."
