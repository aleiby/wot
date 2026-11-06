# Sky gate: capture every sky pose of runtime-map-t5-2-sky.json for one
# build (dev editor -game, or the guarded package with -Hosted) into ONE output
# directory, one map per process (pose entries carry "map" and run only on it),
# then optionally gate against a second build's directory.
#
#   dev:     run_sky_gallery.ps1 -Out <output>\sky-dev
#   guarded: run_sky_gallery.ps1 -Out <output>\sky-guarded -Hosted `
#              -Executable <staged WoT.exe> -UserDir <dir> -ProfileRoot <dir> `
#              -CompareTo <output>\sky-dev
param(
    [Parameter(Mandatory = $true)]
    [string]$Out,
    [string]$Config = '',
    [string]$Executable = '',
    [string]$Project = '',
    [string]$UserDir = '',
    [string]$ProfileRoot = '',
    [string]$AnnouncementUrl = '',
    [string]$CompareTo = '',
    [string]$Python = 'python',
    [string[]]$Maps = @(),
    [switch]$Hosted,
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'
if (-not $Config) {
    $Config = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'runtime-map-t5-2-sky.json'
}
$configPath = (Resolve-Path $Config).Path
$ledger = Get-Content -Raw -Path $configPath | ConvertFrom-Json
# `powershell -File` hands a comma list over as ONE string; accept both forms.
$Maps = @($Maps | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Maps.Count -eq 0) {
    $Maps = @($ledger.entries | ForEach-Object { $_.map } | Where-Object { $_ } | Sort-Object -Unique)
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$outPath = (Resolve-Path $Out).Path
$runner = Join-Path $PSScriptRoot 'run_gallery.ps1'
foreach ($map in $Maps) {
    $arguments = @{
        Map = $map
        Out = $outPath
        Config = $configPath
        TimeoutSeconds = $TimeoutSeconds
    }
    if ($Executable) { $arguments.Executable = $Executable }
    if ($Project) { $arguments.Project = $Project }
    if ($Hosted) {
        $arguments.Hosted = $true
        $arguments.UserDir = $UserDir
        $arguments.ProfileRoot = $ProfileRoot
        if ($AnnouncementUrl) { $arguments.AnnouncementUrl = $AnnouncementUrl }
    }
    # run_gallery.ps1 only sets an exit code on failure; clear any stale one.
    $global:LASTEXITCODE = 0
    & $runner @arguments
    if ($LASTEXITCODE -ne 0) {
        Write-Error "$map sky pose capture failed"
        exit $LASTEXITCODE
    }
    $log = Join-Path $outPath 'gallery.log'
    if (Test-Path $log) {
        Copy-Item -Force $log (Join-Path $outPath "gallery-$map.log")
    }
}
$expected = @($ledger.entries | Where-Object { $Maps -contains $_.map } | ForEach-Object { "$($_.name)_y0.png" })
$missing = @($expected | Where-Object { -not (Test-Path (Join-Path $outPath $_)) })
if ($missing.Count -gt 0) {
    Write-Error ("missing sky pose frames: " + ($missing -join ', '))
    exit 1
}
Write-Host "sky poses complete: $($expected.Count) frames in $outPath"
if ($CompareTo) {
    $comparePath = (Resolve-Path $CompareTo).Path
    & $Python (Join-Path $PSScriptRoot 'gallery_diff.py') $comparePath $outPath --config $configPath
    if ($LASTEXITCODE -ne 0) {
        Write-Error 'sky pose comparison gate failed'
        exit $LASTEXITCODE
    }
}
