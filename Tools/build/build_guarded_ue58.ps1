# Build a target with the PINNED SOURCE engine, or refuse. (Win64 counterpart of
# build_guarded_ue58.sh — see that file for why the wrapper exists at all.)
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Engine,
    [Parameter(Mandatory = $true)][string]$Project,
    [Parameter(Mandatory = $true)][string]$Target,
    [string]$Platform = "Win64",
    [string]$Configuration = "Development",
    [switch]$Clean,
    [switch]$Incremental,
    [string]$Sanitizer = "",
    [switch]$LinkMap,
    [string]$Receipt = "",
    [switch]$SerialLocalBuild
)

$ErrorActionPreference = "Stop"

if ($SerialLocalBuild -and $Platform -ine "Win64") {
    throw "-SerialLocalBuild is supported only for Win64"
}

if ($Clean -and $Incremental) {
    throw "-Clean and -Incremental are mutually exclusive"
}

if ($Sanitizer -and $Sanitizer -ine "Address") {
    throw "unsupported sanitizer '$Sanitizer' (expected: Address)"
}

$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Manifest = Join-Path $Repo "EnginePatches\UE-5.8\source-toolchain.json"
$PatchManifest = Join-Path $Repo "EnginePatches\UE-5.8\manifest.json"
$PatchFile = Join-Path $Repo "EnginePatches\UE-5.8\guarded-bootstrap.patch"
$ApplyPatch = Join-Path $Repo "Tools\dist\apply_engine_patch.py"
$Receipts = Join-Path $Repo "Saved\ReleaseEvidence\toolchain"
New-Item -ItemType Directory -Force -Path $Receipts | Out-Null

if ($Clean -and -not (Test-Path -PathType Leaf $PatchFile)) {
    throw "guarded-bootstrap.patch is absent; -Clean requires the patch from the licensee-only EpicGames fork linked under 'Building from source' in README.md"
}

# The gate, before UBT: a wrong or drifted engine costs seconds rather than an
# hour of compilation.
# An explicit -Receipt names the release-evidence file for this build; without
# one the receipt lands in the per-toolchain default path.
if ($Receipt) {
    $ReceiptPath = $Receipt
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ReceiptPath) | Out-Null
} else {
    $ReceiptPath = Join-Path $Receipts "$Platform-$Target-$Configuration.json"
}
# A public clone has the hash manifest but not Epic's patch. In that case the
# wrapper must prove that every manifest-owned file is already patched. When
# the private patch is present, preserve the existing pristine-or-patched gate.
$VerifyPatchState = @()
if (-not (Test-Path -PathType Leaf $PatchFile)) {
    $VerifyPatchState = @("--require-patched")
}
& python (Join-Path $Repo "Tools\build\verify_ue58_source.py") `
    --engine $Engine --manifest $Manifest --require-source `
    --patch-manifest $PatchManifest @VerifyPatchState `
    --receipt $ReceiptPath
if ($LASTEXITCODE -ne 0) {
    throw "the engine at $Engine was rejected; refusing to build"
}

if ($Clean) {
    # Only manifest-owned project intermediates. The engine's Setup-downloaded
    # dependencies are hash-pinned and cost an hour to refetch, so removing them
    # would be destructive rather than clean.
    Write-Host "cleaning project intermediates for $Target/$Platform/$Configuration"
    $ProjectDir = Split-Path -Parent $Project
    # Scoped to THIS target. Wiping Intermediate\Build\$Platform and
    # Binaries\$Platform wholesale also destroyed other targets' outputs --
    # UnrealEditor-WoT.dll and WoTBootstrapCoreTests.exe among them -- which is
    # actively hostile in a shared tree.
    Get-ChildItem -Path (Join-Path $ProjectDir "Intermediate\Build\$Platform") `
        -Filter $Target -Recurse -Directory -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Get-ChildItem -Path (Join-Path $ProjectDir "Binaries\$Platform") `
        -Filter "$Target*" -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

    # A clean build must also start from known ENGINE bytes, not just clean
    # intermediates. Restore every manifest-owned file to its pinned pristine
    # hash, then re-apply the reviewed patch; apply mode re-verifies all
    # three patched hashes afterwards. Without this, --clean silently
    # accepted whatever the engine tree happened to contain, which is the one
    # thing this wrapper exists to prevent.
    Write-Host "restoring manifest-owned engine files to pristine, then re-applying"
    & python $ApplyPatch --engine $Engine --manifest $PatchManifest `
        --patch $PatchFile --restore
    if ($LASTEXITCODE -ne 0) {
        throw "could not restore the engine to pristine; refusing to build"
    }
    & python $ApplyPatch --engine $Engine --manifest $PatchManifest `
        --patch $PatchFile
    if ($LASTEXITCODE -ne 0) {
        throw "could not re-apply the guarded patch; refusing to build"
    }
}

if ($Incremental) {
    # A persistent build root (Tools/dist/guarded_build_root.py): UBT's own
    # dependency graph decides what is stale, so project intermediates are
    # kept. The engine is NOT restored and re-applied: that rewrites every
    # owned engine source, forcing those files, every engine binary linking
    # them and the engine BuildId to rebuild on every run. Apply mode proves
    # the same end state -- pinned commit, no unowned drift, the reviewed
    # patch identity, every owned file at its patched hash -- and writes
    # nothing when the engine is already patched.
    Write-Host "verifying the engine patch state for an incremental build"
    if (Test-Path -PathType Leaf $PatchFile) {
        & python $ApplyPatch --engine $Engine --manifest $PatchManifest `
            --patch $PatchFile
    } else {
        & python $ApplyPatch --engine $Engine --manifest $PatchManifest `
            --verify-only --require-patched
    }
    if ($LASTEXITCODE -ne 0) {
        throw "the engine patch state was rejected; refusing to build"
    }
}

$Build = Join-Path $Engine "Engine\Build\BatchFiles\Build.bat"
if (-not (Test-Path $Build)) {
    throw "the source engine has no $Build; run provision_ue58_source.py first"
}

# A source engine has no prebuilt UnrealBuildTool: it is compiled by
# GenerateProjectFiles. Without this precondition the build fails deep inside a
# dotnet usage dump that never says "UBT is missing".
$Ubt = Join-Path $Engine "Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll"
if (-not (Test-Path $Ubt)) {
    throw "the source engine has no UnrealBuildTool at $Ubt. Run GenerateProjectFiles.bat in $Engine first (Setup.bat does not build UBT)."
}

Write-Host "building $Target $Platform $Configuration with the pinned source engine"
$BuildArguments = @($Target, $Platform, $Configuration, "-project=$Project", "-NoHotReload")
if ($SerialLocalBuild) {
    # This is a resource-safety boundary, not a performance preference. Keep
    # all compiler work local and prove one concurrent UBT action in the log.
    $BuildArguments += @(
        "-MaxParallelActions=1", "-NoXGE", "-NoFASTBuild",
        "-NoSNDBS", "-NoUBA", "-UBADisableRemote")
}
if ($Sanitizer -ieq "Address") {
    $BuildArguments += "-EnableASan"
}
if ($LinkMap) {
    $LinkMapDirectory = Join-Path $Repo "Saved\LinkMaps\$Platform"
    New-Item -ItemType Directory -Force -Path $LinkMapDirectory | Out-Null
    $LinkMapPath = Join-Path $LinkMapDirectory "$Target.map"
    if ($Platform -eq "Win64") {
        $BuildArguments += "-WoTLinkMap=$LinkMapPath"
    } else {
        $BuildArguments += "-WoTLinkMap=$LinkMapPath"
    }
}
& $Build $BuildArguments
if ($LASTEXITCODE -ne 0) {
    throw "the guarded build failed with exit code $LASTEXITCODE"
}

if ($Incremental) {
    # record_build_receipt reads every Definitions.h under this target and
    # configuration; a module an EARLIER build compiled but this one no longer
    # links must not be evidence. The link response file UBT just wrote names
    # exactly the linked module directories; everything else is removed.
    & python (Join-Path $Repo "Tools\build\prune_stale_module_intermediates.py") `
        --project $Project --target $Target --platform $Platform `
        --configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "stale module intermediates could not be pruned; this build is not valid evidence"
    }
}

# The source receipt says WHICH ENGINE. This says WHAT WAS COMPILED, read back
# from UBT's own generated Definitions.h, and REFUSES a guarded configuration
# that lost its guard or a Shipping build carrying a test-only definition.
& python (Join-Path $Repo "Tools\build\record_build_receipt.py") `
    --project $Project --target $Target --platform $Platform `
    --configuration $Configuration --engine-receipt $ReceiptPath `
    --out ($ReceiptPath -replace '\.json$', '-compiled.json')
if ($LASTEXITCODE -ne 0) {
    throw "the build receipt was rejected; this build is not valid evidence"
}

Write-Host "guarded build succeeded: $Target $Platform $Configuration"
