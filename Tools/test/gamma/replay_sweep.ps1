# replay_sweep.ps1 -- replay-only sweep (19 maps): the shared boundary
# asset changed, no reimports needed. Renders overwrite ue_gamma_v1.
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot '..\..\wot_config.ps1')
$maps = @('Arena_01','Arena_02','Arena_03','Arena_04','Arena_05','Arena_06','Arena_07','Arena_08','Arena_09','Arena_11','Arena_12','Battle2_01','Battle2_02','Battle2_03','Battle2_04','Battle2_05','Battle3_01','Battle3_02','Battle4_01')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$engine = Get-WotLocalPath 'installed_engine_root_win64'
$editor = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project = Join-Path $repo 'WoT.uproject'
$outRoot = Join-Path (Get-WotLocalPath 'scratch_root') 'ue_gamma_v1'
$manifests = Join-Path (Get-WotReferenceDirectory 'exports') 'reference\shots'
foreach ($map in $maps) {
    if ($map -eq 'Arena_05') { Write-Host 'Arena_05 already re-rendered'; continue }
    $outDir = Join-Path $outRoot $map
    $p = Start-Process -FilePath $editor -PassThru -WindowStyle Hidden -ArgumentList @(
        "`"$project`"", "/Game/WoT/Maps/$map", '-game', '-RenderOffscreen',
        '-ForceRes', '-resx=1600', '-resy=1200', '-windowed', '-unattended', '-nosound',
        '-stdout', "-wotshot=`"$outDir`"", "-wotmanifest=`"$manifests`"")
    $p | Wait-Process -Timeout 1800 -ErrorAction SilentlyContinue
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Host "$map REPLAY TIMEOUT"; continue }
    Write-Host "$map done"
}
Write-Host 'REPLAY-SWEEP COMPLETE'
