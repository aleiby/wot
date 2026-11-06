# convert_stage.ps1 -- staged BMPs -> named PNGs in exports/reference/shots/<Map>/
# plus enriched manifest. Also flags suspicious runs of near-identical shots.
param(
    [string]$StageRoot = (Join-Path $env:TEMP 'wot-capture-stage'),
    [string]$OutRoot = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\wot_config.ps1')
if (-not $OutRoot) {
    $OutRoot = Join-Path (Get-WotReferenceDirectory 'exports') 'reference\shots'
}
Add-Type -AssemblyName System.Drawing

function Get-TinyHash([System.Drawing.Bitmap]$src) {
    # 8x8 average-luminance grid as the perceptual signature
    $tiny = New-Object System.Drawing.Bitmap(8, 8)
    $g = [System.Drawing.Graphics]::FromImage($tiny)
    $g.DrawImage($src, 0, 0, 8, 8)
    $g.Dispose()
    $vals = New-Object 'System.Collections.Generic.List[double]'
    for ($y = 0; $y -lt 8; $y++) {
        for ($x = 0; $x -lt 8; $x++) {
            $c = $tiny.GetPixel($x, $y)
            $vals.Add(0.299*$c.R + 0.587*$c.G + 0.114*$c.B)
        }
    }
    $tiny.Dispose()
    return ,$vals.ToArray()
}

function Hash-Distance($a, $b) {
    $d = 0.0
    for ($i = 0; $i -lt 64; $i++) { $d += [math]::Abs($a[$i] - $b[$i]) }
    return $d / 64.0
}

$warnings = @()
foreach ($mapDir in Get-ChildItem $StageRoot -Directory | Where-Object { $_.Name -ne '_leftover' }) {
    $map = $mapDir.Name
    $manifestPath = Join-Path $mapDir.FullName 'manifest.json'
    if (-not (Test-Path $manifestPath)) { Write-Host "SKIP $map (no manifest)"; continue }
    $meta = Get-Content $manifestPath -Raw | ConvertFrom-Json

    $outDir = Join-Path $OutRoot $map
    New-Item -ItemType Directory -Force $outDir | Out-Null

    $prevHash = $null
    $dupRun = 0
    $outShots = @()
    foreach ($shot in $meta.shots) {
        $bmpPath = Join-Path $mapDir.FullName ('Shot{0:D4}.bmp' -f [int]$shot.order)
        if (-not (Test-Path $bmpPath)) { $warnings += "$map order $($shot.order): missing BMP"; continue }
        $pngName = "{0}_{1}_{2}.png" -f $map, $shot.source, $shot.view
        $bmp = [System.Drawing.Bitmap]::FromFile($bmpPath)
        $bmp.Save((Join-Path $outDir $pngName), [System.Drawing.Imaging.ImageFormat]::Png)
        $hash = Get-TinyHash $bmp
        $bmp.Dispose()

        if ($null -ne $prevHash) {
            if ((Hash-Distance $prevHash $hash) -lt 1.0) { $dupRun++ } else { $dupRun = 0 }
            if ($dupRun -ge 3) { $warnings += "$map : $dupRun consecutive near-identical shots ending at $pngName" }
        }
        $prevHash = $hash

        $entry = [ordered]@{}
        $shot.PSObject.Properties | ForEach-Object { $entry[$_.Name] = $_.Value }
        $entry['file'] = $pngName
        $outShots += $entry
    }

    $outMeta = [ordered]@{
        map         = $meta.map
        renderer    = $meta.renderer
        brightness  = $meta.brightness
        eye_height  = $meta.eye_height
        fov         = $meta.fov
        camera_note = $meta.camera_note
        shots       = $outShots
    }
    [System.IO.File]::WriteAllText((Join-Path $outDir 'manifest.json'), ($outMeta | ConvertTo-Json -Depth 5))
    Write-Host "$map : $($outShots.Count) shots -> $outDir"
}
Write-Host '--- warnings ---'
if ($warnings.Count) { $warnings | ForEach-Object { Write-Host "WARN: $_" } } else { Write-Host '(none)' }
