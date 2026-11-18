[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EvidenceRoot,
    [string[]]$Maps = @('Arena_01', 'Arena_02', 'Arena_03', 'Arena_04', 'Arena_05',
        'Arena_06', 'Arena_07', 'Arena_08', 'Arena_09', 'Arena_11', 'Arena_12',
        'Battle2_01', 'Battle2_02', 'Battle2_03', 'Battle2_04', 'Battle2_05',
        'Battle3_01', 'Battle3_02', 'Battle4_01'),
    [switch]$StrictSkips,
    # When given, every map's summary must have joined this way.
    [ValidateSet('', 'Direct', 'Session')][string]$JoinMode = ''
)
$ErrorActionPreference = 'Stop'
# `powershell -File` passes '-Maps A,B' as ONE string: accept either form.
$Maps = @($Maps | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$root = (Resolve-Path -LiteralPath $EvidenceRoot).Path
# Every map's record must exist and be non-empty before any judgement.
foreach ($map in $Maps) {
    $dir = Join-Path $root $map
    foreach ($name in @('summary.json', 'transcript.json', 'host-heartbeat.jsonl',
            'client-heartbeat.jsonl', 'host-frame.png', 'client-frame.png')) {
        $path = Join-Path $dir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "$map missing $name" }
        if ((Get-Item -LiteralPath $path).Length -eq 0) { throw "$map has empty $name" }
    }
}
# The per-map judgement (titles, pickups, heartbeat slices vs the pinned
# per-map allowlists) lives in sweep_checks.py so it is unit-tested offline.
$python = (Get-Command python -ErrorAction Stop).Source
$checkArguments = @((Join-Path $PSScriptRoot 'sweep_checks.py'), 'verify',
    '--evidence', $root, '--maps') + $Maps
if ($StrictSkips) { $checkArguments += '--strict-skips' }
if ($JoinMode) { $checkArguments += @('--join-mode', $JoinMode.ToLowerInvariant()) }
# stdout only: under Windows PowerShell 5.1 a redirected native stderr line
# becomes a terminating NativeCommandError; the checker reports on stdout.
$output = @(& $python @checkArguments)
$exitCode = $LASTEXITCODE
$output | ForEach-Object { Write-Host "$_" }
if ($exitCode -ne 0) {
    $detail = ($output | Where-Object { "$_" -match '^\s+- ' } | ForEach-Object { "$_".Trim() }) -join '; '
    throw "map sweep evidence rejected: $detail"
}
Write-Output 'map-sweep evidence verified'
