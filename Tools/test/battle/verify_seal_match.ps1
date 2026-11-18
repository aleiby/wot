[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EvidenceRoot,
    [string[]]$Maps = @('Battle2_01', 'Battle3_01')
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $EvidenceRoot).Path
foreach ($map in $Maps) {
    $dir = Join-Path $root $map
    foreach ($name in @('summary.json', 'transcript.json', 'host-heartbeat.jsonl',
            'client-heartbeat.jsonl', 'host-frame.png', 'client-frame.png')) {
        $path = Join-Path $dir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "$map missing $name" }
        if ((Get-Item -LiteralPath $path).Length -eq 0) { throw "$map has empty $name" }
    }
    $summary = Get-Content -LiteralPath (Join-Path $dir 'summary.json') -Raw | ConvertFrom-Json
    if ($summary.status -ne 'passed' -or -not $summary.phase_started) {
        throw "$map did not pass phase validation"
    }
    if ($map -eq 'Battle2_01' -and -not $summary.objective_complete) {
        throw 'Battle2_01 did not complete the seal objective'
    }
    # The guarded run record of each process: it reached Ready, projected THIS
    # map, and gave the materialized actors physics bodies once InitWorld had
    # created the scene (without them no touch/overlap can ever fire).
    foreach ($role in @('host', 'client')) {
        $heartbeatPath = Join-Path $dir "$role-heartbeat.jsonl"
        $records = @(Get-Content -LiteralPath $heartbeatPath | ForEach-Object {
            try { $_ | ConvertFrom-Json -ErrorAction Stop } catch { $null }
        } | Where-Object { $_ })
        $ready = $records | Where-Object { $_.phase -eq 'Ready' } | Select-Object -First 1
        if (-not $ready) { throw "$map $role heartbeat never reached Ready" }
        $projected = $records | Where-Object {
            $_.event -eq 'world-projected' -and "$($_.detail)" -match "(^| )map=$map( |$)"
        } | Select-Object -First 1
        if (-not $projected) { throw "$map $role heartbeat never projected $map" }
        $physics = $records | Where-Object {
            $_.event -eq 'world-physics-completed' -and "$($_.detail)" -match 'created=([1-9][0-9]*)'
        } | Select-Object -First 1
        if (-not $physics) { throw "$map $role heartbeat has no world-physics-completed record" }
    }
    # The driver's own phase record (summary events).
    $events = @($summary.events | ForEach-Object { $_.event })
    foreach ($required in @('host-listening', 'client-joined', 'teams-selected',
            'team-ready', 'battlefield-open')) {
        if ($events -notcontains $required) { throw "$map summary lacks the $required phase" }
    }
    if ($map -eq 'Battle2_01') {
        foreach ($required in @('seal-captured', 'seal-banked', 'objective-complete')) {
            if ($events -notcontains $required) { throw "$map summary lacks the $required phase" }
        }
    }
    $commands = Get-Content -LiteralPath (Join-Path $dir 'transcript.json') -Raw | ConvertFrom-Json
    if (($commands | Measure-Object).Count -lt 8) { throw "$map transcript is implausibly short" }
}
Write-Output 'seal-match evidence verified'
