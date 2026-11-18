#!/bin/sh
# Build a target with the PINNED SOURCE engine, or refuse.
#
# Every guarded, packaged, candidate, and release build goes through this wrapper
# rather than calling UBT directly, for one reason: UBT will happily build with
# whatever engine it is pointed at, and "which engine produced this binary?" must
# have exactly one checkable answer. The verifier runs FIRST, so a wrong or drifted
# engine costs seconds instead of an hour of compilation.
#
# Usage:
#   build_guarded_ue58.sh --engine <dir> --project <uproject> --target <name>
#                         --platform Mac --configuration Development
#                         [--clean | --incremental]
#                         [--sanitizer address] [--link-map]
set -eu

ENGINE=""
PROJECT=""
TARGET=""
PLATFORM="Mac"
CONFIGURATION="Development"
CLEAN=0
INCREMENTAL=0
RECEIPT=""
SANITIZER=""
LINK_MAP=0

while [ $# -gt 0 ]; do
  case "$1" in
    --engine)        ENGINE="$2"; shift 2 ;;
    --project)       PROJECT="$2"; shift 2 ;;
    --target)        TARGET="$2"; shift 2 ;;
    --platform)      PLATFORM="$2"; shift 2 ;;
    --configuration) CONFIGURATION="$2"; shift 2 ;;
    --clean)         CLEAN=1; shift ;;
    --incremental)   INCREMENTAL=1; shift ;;
    --receipt)       RECEIPT="$2"; shift 2 ;;
    --sanitizer)     SANITIZER="$2"; shift 2 ;;
    --link-map)      LINK_MAP=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$ENGINE" ] || [ -z "$PROJECT" ] || [ -z "$TARGET" ]; then
  echo "usage: $0 --engine <dir> --project <uproject> --target <name> [--platform Mac] [--configuration Development] [--clean | --incremental] [--receipt <path>] [--sanitizer address] [--link-map]" >&2
  exit 2
fi

if [ -n "$SANITIZER" ] && [ "$SANITIZER" != "address" ]; then
  echo "unsupported sanitizer: $SANITIZER (expected: address)" >&2
  exit 2
fi

if [ "$CLEAN" -eq 1 ] && [ "$INCREMENTAL" -eq 1 ]; then
  echo "--clean and --incremental are mutually exclusive" >&2
  exit 2
fi

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
MANIFEST="$REPO/EnginePatches/UE-5.8/source-toolchain.json"
PATCH_MANIFEST="$REPO/EnginePatches/UE-5.8/manifest.json"
PATCH_FILE="$REPO/EnginePatches/UE-5.8/guarded-bootstrap.patch"
RECEIPTS="$REPO/Saved/ReleaseEvidence/toolchain"

if [ "$CLEAN" -eq 1 ] && [ ! -f "$PATCH_FILE" ]; then
  echo "guarded-bootstrap.patch is absent; --clean must restore and apply the patch." >&2
  echo "Obtain it from the licensee-only EpicGames fork linked under 'Building from source' in README.md." >&2
  exit 1
fi

# The gate. --require-source also enforces pristine patch inputs and the pinned
# compiler identity, because the same source with a different toolchain is a
# different binary.
# An explicit --receipt names the release-evidence file for this build; without
# one the receipt lands in the per-toolchain default path.
if [ -z "$RECEIPT" ]; then
  RECEIPT="$RECEIPTS/$PLATFORM-$TARGET-$CONFIGURATION.json"
fi
mkdir -p "$(dirname "$RECEIPT")"
mkdir -p "$RECEIPTS"
# --patch-manifest: a guarded engine is legitimately PATCHED, not pristine. Without
# it the verifier rejects the engine it just correctly patched — which is exactly
# what happened the first time this ran.
if [ -f "$PATCH_FILE" ]; then
  python3 "$REPO/Tools/build/verify_ue58_source.py" \
    --engine "$ENGINE" --manifest "$MANIFEST" --require-source \
    --patch-manifest "$PATCH_MANIFEST" --receipt "$RECEIPT"
else
  # Public clones carry the complete hash manifest, but never Epic's patch.
  # A no-patch build is safe only when every owned file is already patched.
  python3 "$REPO/Tools/build/verify_ue58_source.py" \
    --engine "$ENGINE" --manifest "$MANIFEST" --require-source \
    --patch-manifest "$PATCH_MANIFEST" --require-patched \
    --receipt "$RECEIPT"
fi

if [ "$CLEAN" -eq 1 ]; then
  # Only manifest-owned intermediates and receipts. Deliberately NOT a blanket
  # wipe: the engine's own Setup-downloaded dependencies cost an hour to refetch
  # and are already hash-pinned, so removing them would be destructive rather
  # than clean.
  echo "cleaning project intermediates for $TARGET/$PLATFORM/$CONFIGURATION"
  PROJECT_DIR="$(dirname "$PROJECT")"
  # Scoped to THIS target. Wiping Intermediate/Build/$PLATFORM and
  # Binaries/$PLATFORM wholesale also destroyed other targets' outputs --
  # UnrealEditor-WoT.dll and WoTBootstrapCoreTests among them -- which is
  # actively hostile in a shared tree.
  rm -rf "$PROJECT_DIR/Intermediate/Build/$PLATFORM"/*/"$TARGET"
  rm -rf "$PROJECT_DIR/Intermediate/Build/$PLATFORM/$TARGET"
  find "$PROJECT_DIR/Binaries/$PLATFORM" -maxdepth 1 -name "$TARGET" \
    -o -maxdepth 1 -name "$TARGET-*" -o -maxdepth 1 -name "$TARGET.*" 2>/dev/null \
    | while read -r stale; do rm -rf "$stale"; done

  # A clean build must also start from known ENGINE bytes, not just clean
  # intermediates. Restore every manifest-owned file to its pinned pristine
  # hash, then re-apply the reviewed patch; apply mode re-verifies all three
  # patched hashes afterwards. Without this, --clean silently accepted whatever
  # the engine tree happened to contain, which is the one thing this wrapper
  # exists to prevent.
  echo "restoring manifest-owned engine files to pristine, then re-applying"
  python3 "$REPO/Tools/dist/apply_engine_patch.py" \
    --engine "$ENGINE" \
    --manifest "$PATCH_MANIFEST" --patch "$PATCH_FILE" --restore
  python3 "$REPO/Tools/dist/apply_engine_patch.py" \
    --engine "$ENGINE" \
    --manifest "$PATCH_MANIFEST" --patch "$PATCH_FILE"
fi

if [ "$INCREMENTAL" -eq 1 ]; then
  # A persistent build root (Tools/dist/guarded_build_root.py): UBT's own
  # dependency graph decides what is stale, so project intermediates are kept.
  # The engine is NOT restored and re-applied: that rewrites every owned engine
  # source, forcing those files, every engine binary linking them and the
  # engine BuildId to rebuild on every run. Apply mode proves the same end
  # state -- pinned commit, no unowned drift, the reviewed patch identity,
  # every owned file at its patched hash -- and writes nothing when the engine
  # is already patched.
  echo "verifying the engine patch state for an incremental build"
  if [ -f "$PATCH_FILE" ]; then
    python3 "$REPO/Tools/dist/apply_engine_patch.py" \
      --engine "$ENGINE" --manifest "$PATCH_MANIFEST" --patch "$PATCH_FILE"
  else
    python3 "$REPO/Tools/dist/apply_engine_patch.py" \
      --engine "$ENGINE" --manifest "$PATCH_MANIFEST" \
      --verify-only --require-patched
  fi
fi

BUILD="$ENGINE/Engine/Build/BatchFiles/Mac/Build.sh"
if [ ! -x "$BUILD" ]; then
  echo "the source engine has no $BUILD; run provision_ue58_source.py first" >&2
  exit 1
fi

# A source engine has no prebuilt UnrealBuildTool: it is compiled by
# GenerateProjectFiles. Without this precondition the build fails forty lines into
# a dotnet usage dump that never says "UBT is missing" — checked here so the
# failure names its own cause.
UBT="$ENGINE/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll"
if [ ! -f "$UBT" ]; then
  echo "the source engine has no UnrealBuildTool at $UBT." >&2
  echo "Run GenerateProjectFiles.sh in $ENGINE first (Setup.sh does not build UBT)." >&2
  exit 1
fi

echo "building $TARGET $PLATFORM $CONFIGURATION with the pinned source engine"
# NOT exec, and never piped: the exit code is the only evidence the build ran, and
# a pipeline would report the last stage's status instead of the compiler's.
set -- "$TARGET" "$PLATFORM" "$CONFIGURATION" -project="$PROJECT"
if [ "$SANITIZER" = "address" ]; then
  set -- "$@" -EnableASan
fi
if [ "$LINK_MAP" -eq 1 ]; then
  LINK_MAP_DIR="$REPO/Saved/LinkMaps/$PLATFORM"
  LINK_MAP_PATH="$LINK_MAP_DIR/$TARGET.map"
  mkdir -p "$LINK_MAP_DIR"
  # -WoTLinkMap is a custom argument the WoT target rules read from
  # Target.Arguments; UBT itself silently ignores unknown -Name=Value args,
  # which is why a bare -AdditionalLinkerArguments= did nothing.
  set -- "$@" "-WoTLinkMap=$LINK_MAP_PATH"
fi
"$BUILD" "$@"
STATUS=$?
if [ "$STATUS" -ne 0 ]; then
  echo "guarded build FAILED with exit code $STATUS" >&2
  exit "$STATUS"
fi
if [ "$INCREMENTAL" -eq 1 ]; then
  # record_build_receipt reads every Definitions.h under this target and
  # configuration; a module an EARLIER build compiled but this one no longer
  # links must not be evidence. The link response file UBT just wrote names
  # exactly the linked module directories; everything else is removed.
  python3 "$REPO/Tools/build/prune_stale_module_intermediates.py" \
    --project "$PROJECT" --target "$TARGET" --platform "$PLATFORM" \
    --configuration "$CONFIGURATION"
fi
# The source receipt says WHICH ENGINE. This says WHAT WAS COMPILED, read back
# from UBT's own generated Definitions.h, and REFUSES a guarded configuration
# that lost its guard or a Shipping build carrying a test-only definition.
python3 "$REPO/Tools/build/record_build_receipt.py" \
  --project "$PROJECT" --target "$TARGET" --platform "$PLATFORM" \
  --configuration "$CONFIGURATION" --engine-receipt "$RECEIPT" \
  --out "${RECEIPT%.json}-compiled.json"

echo "guarded build succeeded: $TARGET $PLATFORM $CONFIGURATION"
