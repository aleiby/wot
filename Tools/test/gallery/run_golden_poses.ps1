# Replay the retail golden poses (exports/reference/shots/<Map>/manifest.json)
# for any of the 19 maps, in the development editor (-game) or in the guarded
# package (-Hosted): the guarded front end hosts the map's neutral shell and
# UWoTShotSubsystem resolves the shell back to the retail identity (Task B3-2
# reverse lookup), so frames carry the golden file names for 1:1 diffing.
#
#   dev:     run_golden_poses.ps1 -Out <output>\golden-dev
#   guarded: run_golden_poses.ps1 -Out <output>\golden-guarded -Hosted `
#              -Executable <staged WoT.exe> -UserDir <dir> -ProfileRoot <dir>
param(
    [Parameter(Mandatory = $true)]
    [string]$Out,
    [string[]]$Maps = @('Arena_01', 'Arena_02', 'Arena_03', 'Arena_04', 'Arena_05',
        'Arena_06', 'Arena_07', 'Arena_08', 'Arena_09', 'Arena_11', 'Arena_12',
        'Battle2_01', 'Battle2_02', 'Battle2_03', 'Battle2_04', 'Battle2_05',
        'Battle3_01', 'Battle3_02', 'Battle4_01'),
    [string]$Manifests = '',
    [string]$Executable = '',
    [string]$Project = '',
    [string]$UserDir = '',
    [string]$ProfileRoot = '',
    [string]$AnnouncementUrl = '',
    [switch]$Hosted,
    [int]$TimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\wot_config.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Manifests) {
    $Manifests = Join-Path (Get-WotReferenceDirectory 'exports') 'reference\shots'
}
$manifestPath = (Resolve-Path $Manifests).Path
if (-not $Executable) {
    $engine = Get-WotLocalPath 'installed_engine_root_win64'
    $Executable = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
}
$Executable = (Resolve-Path $Executable).Path
if (-not $Project) {
    $Project = Join-Path $repo 'WoT.uproject'
}
if ($Hosted -and (-not $UserDir -or -not $ProfileRoot)) {
    throw '-Hosted requires -UserDir and -ProfileRoot for an isolated packaged profile'
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$outRoot = (Resolve-Path $Out).Path
$failed = @()
foreach ($map in $Maps) {
    $mapOut = Join-Path $outRoot $map
    New-Item -ItemType Directory -Force -Path $mapOut | Out-Null
    $arguments = @()
    if (-not $Hosted) {
        $arguments += @("`"$Project`"", "/Game/WoT/Maps/$map", '-game')
    }
    $arguments += @(
        '-RenderOffscreen', '-ForceRes', '-resx=1600', '-resy=1200', '-windowed',
        '-unattended', '-nosound', '-stdout',
        "-wotshot=`"$mapOut`"", "-wotmanifest=`"$manifestPath`""
    )
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = Split-Path -Parent $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    if ($Hosted) {
        $userDirPath = (Resolve-Path $UserDir).Path
        $profileRootPath = (Resolve-Path $ProfileRoot).Path
        $localAppDataPath = Join-Path $profileRootPath 'AppData\Local'
        $appDataPath = Join-Path $profileRootPath 'AppData\Roaming'
        New-Item -ItemType Directory -Force -Path $localAppDataPath, $appDataPath | Out-Null
        $arguments += @(
            '-nosteam', "-UserDir=`"$userDirPath`"",
            "-abslog=`"$(Join-Path $mapOut 'golden.log')`"",
            "-wotgallerymap=$map"
        )
        $startInfo.EnvironmentVariables['USERPROFILE'] = $profileRootPath
        $startInfo.EnvironmentVariables['LOCALAPPDATA'] = $localAppDataPath
        $startInfo.EnvironmentVariables['APPDATA'] = $appDataPath
        if ($AnnouncementUrl) {
            $startInfo.EnvironmentVariables['WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL'] = $AnnouncementUrl
        }
    }
    $startInfo.Arguments = $arguments -join ' '
    $process = [Diagnostics.Process]::Start($startInfo)
    Write-Host "$map golden replay started: pid=$($process.Id) hosted=$Hosted"
    $process.WaitForExit($TimeoutSeconds * 1000) | Out-Null
    $process.Refresh()
    if (-not $process.HasExited) {
        # Only terminate the exact process object started above.
        Stop-Process -Id $process.Id -Force
        Write-Warning "$map golden replay timed out"
        $failed += $map
        continue
    }
    $manifest = Get-Content -Raw -Path (Join-Path $manifestPath "$map\manifest.json") | ConvertFrom-Json
    $missing = @($manifest.shots | Where-Object { -not (Test-Path (Join-Path $mapOut $_.file)) })
    if ($process.ExitCode -ne 0 -or $missing.Count -gt 0) {
        Write-Warning "$map golden replay: exit=$($process.ExitCode) missing=$($missing.Count)"
        $failed += $map
        continue
    }
    Write-Host "$map golden replay complete: $($manifest.shots.Count) frames"
}
if ($failed.Count -gt 0) {
    Write-Error ("golden replay failed for: " + ($failed -join ', '))
    exit 1
}
Write-Host "golden replay complete for $($Maps.Count) maps in $outRoot"
