# gamma_sweep.ps1 -- 19-map gamma-space sweep: reimport (GAMMA_SPACE=True,
# mesh lighting bakes, skin overrides) + golden replay per map. Atlases are
# untouched (composition unchanged). Renders go below configured scratch_root.
# DO NOT edit Tools/import|convert or build C++ while this runs.
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot '..\..\wot_config.ps1')
$maps = @('Arena_01','Arena_02','Arena_03','Arena_04','Arena_05','Arena_06','Arena_07','Arena_08','Arena_09','Arena_11','Arena_12','Battle2_01','Battle2_02','Battle2_03','Battle2_04','Battle2_05','Battle3_01','Battle3_02','Battle4_01')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$engine = Get-WotLocalPath 'installed_engine_root_win64'
$editor = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$proj = Join-Path $repo 'WoT.uproject'
$conv = Get-WotLocalPath 'conversion_root'
$outRoot = Join-Path (Get-WotLocalPath 'scratch_root') 'ue_gamma_v1'
$golden = Join-Path (Get-WotReferenceDirectory 'exports') 'reference\shots'
$importMap = Join-Path $repo 'Tools\import\import_map.py'

foreach ($map in $maps) {
    $p = Start-Process -FilePath $editor -PassThru -WindowStyle Hidden -ArgumentList @(
        "`"$proj`"", '-run=pythonscript',
        "-script=`"$importMap $map $conv baked`"",
        '-unattended', '-nullrhi', '-nosplash', '-stdout')
    $p | Wait-Process -Timeout 1800 -ErrorAction SilentlyContinue
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Host "$map IMPORT TIMEOUT"; continue }
    $outDir = Join-Path $outRoot $map
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $p = Start-Process -FilePath $editor -PassThru -WindowStyle Hidden -ArgumentList @(
        "`"$proj`"", "/Game/WoT/Maps/$map", '-game', '-RenderOffscreen', '-ForceRes',
        '-resx=1600', '-resy=1200', '-windowed', '-unattended', '-nosound', '-stdout',
        "-wotshot=`"$outDir`"", "-wotmanifest=`"$golden`"")
    $p | Wait-Process -Timeout 1800 -ErrorAction SilentlyContinue
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Host "$map REPLAY TIMEOUT"; continue }
    Write-Host ("$map done: " + (Get-ChildItem $outDir -Filter '*.png' | Measure-Object).Count + ' shots')
}
Write-Host 'GAMMA-SWEEP COMPLETE'
