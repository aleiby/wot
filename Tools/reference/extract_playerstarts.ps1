# Extract PlayerStart poses from exports/t3d/<Map>.t3d into a poses JSON.
# Run from the repo root on the Windows capture box.
param(
    [string]$T3dDir = '',
    [string]$OutJson = '',
    [string[]]$Maps = @('Arena_01','Arena_02','Arena_03','Arena_04','Arena_05','Arena_06','Arena_07','Arena_08','Arena_09','Arena_11','Arena_12','Battle2_01','Battle2_02','Battle2_03','Battle2_04','Battle2_05','Battle3_01','Battle3_02','Battle4_01')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\wot_config.ps1')
if (-not $T3dDir) { $T3dDir = Join-Path (Get-WotReferenceDirectory 'exports') 't3d' }
if (-not $OutJson) {
    $OutJson = Join-Path (Get-WotReferenceDirectory 'exports') 'reference\shots\poses.json'
}

$all = [ordered]@{}
foreach ($map in $Maps) {
    $t3d = Join-Path $T3dDir "$map.t3d"
    if (-not (Test-Path $t3d)) { Write-Host "MISSING: $t3d"; continue }
    $starts = @()
    $inStart = $false
    $cur = $null
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $t3d))) {
        if ($line -match '^Begin Actor Class=PlayerStart Name=(\S+)') {
            $inStart = $true
            $cur = [ordered]@{ name = $Matches[1]; x = $null; y = $null; z = $null; yaw = 0; pitch = 0; roll = 0 }
        }
        elseif ($inStart) {
            if ($line -match '^\s+Location=\(X=([-0-9.]+),Y=([-0-9.]+),Z=([-0-9.]+)\)') {
                $cur.x = [double]$Matches[1]; $cur.y = [double]$Matches[2]; $cur.z = [double]$Matches[3]
            }
            elseif ($line -match '^\s+Rotation=\(') {
                if ($line -match 'Pitch=(-?\d+)') { $cur.pitch = [int]$Matches[1] }
                if ($line -match 'Yaw=(-?\d+)')   { $cur.yaw   = [int]$Matches[1] }
                if ($line -match 'Roll=(-?\d+)')  { $cur.roll  = [int]$Matches[1] }
            }
            elseif ($line -match '^End Actor') {
                if ($null -ne $cur.x) { $starts += ,$cur } else { Write-Host "WARN: $map $($cur.name) has no Location" }
                $inStart = $false
            }
        }
    }
    $all[$map] = $starts
    Write-Host ("{0}: {1} PlayerStarts" -f $map, $starts.Count)
}

New-Item -ItemType Directory -Force (Split-Path $OutJson) | Out-Null
[System.IO.File]::WriteAllText((Resolve-Path (Split-Path $OutJson)).Path + '\' + (Split-Path $OutJson -Leaf), ($all | ConvertTo-Json -Depth 5))
Write-Host "wrote $OutJson"
