[CmdletBinding()]
param(
    # A PRIVATE stage parent (the packaged Windows tree + the adjacent retail
    # installer), e.g. a byte copy of a .stripped-pak sweep stage. Never a
    # shared package in use, never another worktree's live stage.
    [string]$StageRoot,
    [string]$Executable,
    [string]$EvidenceRoot = "Saved/ReleaseEvidence/angreal-matrix",
    # A prepared <LocalAppData>/WoT tree (only the generations its active
    # pointers name are copied; never UpdaterState, locks or leases).
    [string]$SeedInstallRoot,
    [string]$Map = 'Arena_02',
    # Phases the driver runs, in order: matrix (per-class host->client and
    # client->host casts), rules (frag limit, restart-fire, rotation), timelimit.
    [string[]]$Phases = @('matrix', 'rules'),
    # Optional class filter (checklist short names, e.g. Fireball,Heal).
    [string[]]$Classes = @(),
    [ValidateSet('both', 'h2c', 'c2h')] [string]$Directions = 'both',
    # Unique ports for this harness (game 8177, test channels 8187+).
    [int]$GamePort = 8177,
    [int]$HostTestPort = 8187,
    [int]$ClientTestPort = 8188,
    [string]$Token = "wotangreal",
    # Private dead loopback endpoint: the guarded Test build honours this
    # override, so a run can never reach the public announcement URL.
    [string]$AnnouncementUrl = 'http://127.0.0.1:8199/announcement.json',
    [int]$ResX = 800,
    [int]$ResY = 450,
    # Probe mode: boot, host, join, then hold both instances this long (the
    # channel accepts extra connections: Tools/test/angreal/probe.py).
    [int]$ProbeSeconds = 0,
    # Pending-build `host` verb field: host with ?TimeLimit=<minutes> through the
    # production host request (the shipped binary ignores it).
    [int]$HostTimeLimit = -1,
    [int]$HoldOnFailureSeconds = 0,
    # Extra command-line switches for BOTH instances (e.g. -wotnetdiag).
    [string]$ExtraArgs = '',
    [switch]$KeepLogs,
    [switch]$KeepStages,
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
$Phases = @($Phases | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Classes = @($Classes | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$plan = [ordered]@{
    schema = 'wot-angreal-matrix-v1'
    topology = 'listen-host-plus-client'
    map = $Map
    phases = $Phases
    classes = $(if ($Classes.Count) { $Classes } else { 'all obtainable (angreal_expectations.json)' })
    directions = $Directions
    ports = [ordered]@{ game = $GamePort; host_test = $HostTestPort; client_test = $ClientTestPort }
    private_stage_copies = @('host', 'client')
    isolated_profiles = @('host', 'client')
    expectations = 'angreal_expectations.json (every observable cites exports/script)'
    verifier = 'verify_angreal_matrix.py'
}
if ($PlanOnly) {
    $plan | ConvertTo-Json -Depth 6
    exit 0
}
if ([string]::IsNullOrWhiteSpace($StageRoot) -or [string]::IsNullOrWhiteSpace($Executable) -or
    [string]::IsNullOrWhiteSpace($SeedInstallRoot)) {
    throw '-StageRoot, -Executable, and -SeedInstallRoot are required unless -PlanOnly is used'
}
$sourceStage = (Resolve-Path -LiteralPath $StageRoot).Path.TrimEnd('\', '/')
$sourceExe = (Resolve-Path -LiteralPath $Executable).Path
$seedRoot = (Resolve-Path -LiteralPath $SeedInstallRoot).Path.TrimEnd('\', '/')
if (-not $sourceExe.StartsWith($sourceStage + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw '-Executable must be inside -StageRoot'
}
if ($sourceStage -match '(?i)black-front-end-diagnostic') {
    throw 'refusing to run from another lane''s worktree; copy the stage privately first'
}
$root = [IO.Path]::GetFullPath((Join-Path (Get-Location) $EvidenceRoot))
$run = Join-Path $root ((Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ'))
# Bulky per-role stages and profiles live beside (not inside) the evidence and
# are deleted afterwards; the evidence stays small.
$bulk = Join-Path ([IO.Path]::GetDirectoryName($sourceStage)) ("run-" + [IO.Path]::GetFileName($run))
$profiles = Join-Path $bulk 'profiles'
$stages = Join-Path $bulk 'stages'
New-Item -ItemType Directory -Path $run, $profiles, $stages | Out-Null
$relativeExe = $sourceExe.Substring($sourceStage.Length).TrimStart('\', '/')

function New-PrivateStage([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    & robocopy $Source $Destination /E /COPY:DAT /R:1 /W:1 /MT:8 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy of the private stage failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

function Copy-SeedInstall([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $sourcePrefix = $Source + [IO.Path]::DirectorySeparatorChar
    $activeGenerations = @()
    foreach ($pointer in Get-ChildItem -LiteralPath (Join-Path $Source 'ContentCache') `
            -Filter 'active-*.ptr' -ErrorAction SilentlyContinue) {
        foreach ($line in Get-Content -LiteralPath $pointer.FullName) {
            if ($line -match '^generation=(.+)$') { $activeGenerations += $Matches[1].Trim() }
        }
    }
    Get-ChildItem -LiteralPath $Source -Force -Recurse | ForEach-Object {
        $relative = $_.FullName.Substring($sourcePrefix.Length)
        $parts = $relative -split '[\\/]'
        if ($parts -contains 'UpdaterState' -or $_.Name -like '*.lock' -or
            $_.Name -match '(?i)lease') { return }
        if ($parts[0] -eq 'ContentCache' -and $parts.Count -ge 2 -and
            $parts[1] -like 'gen-*' -and $activeGenerations -notcontains $parts[1]) { return }
        if ($parts[0] -eq 'ContentCache' -and $parts.Count -ge 2 -and
            $parts[1] -eq 'quarantine') { return }
        $target = Join-Path $Destination $relative
        if ($_.PSIsContainer) {
            New-Item -ItemType Directory -Path $target -Force | Out-Null
        } else {
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $target -Force
        }
    }
}

# Detached launch with an isolated profile ($host is a read-only automatic
# variable, so the roles are hostRole/clientRole).
function Start-IsolatedGame([string]$RoleProfile, [string]$PrivateExe, [string]$Arguments) {
    $local = Join-Path $RoleProfile 'AppData/Local'
    $roaming = Join-Path $RoleProfile 'AppData/Roaming'
    New-Item -ItemType Directory -Path $local, $roaming -Force | Out-Null
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $PrivateExe
    $info.Arguments = $Arguments
    $info.WorkingDirectory = Split-Path -Parent $PrivateExe
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.EnvironmentVariables['USERPROFILE'] = $RoleProfile
    $info.EnvironmentVariables['LOCALAPPDATA'] = $local
    $info.EnvironmentVariables['APPDATA'] = $roaming
    if ($AnnouncementUrl) { $info.EnvironmentVariables['WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL'] = $AnnouncementUrl }
    $heartbeatRoot = Join-Path $RoleProfile 'AppData/Local/WoT/UpdaterState'
    $heartbeatBefore = @(Get-ChildItem -LiteralPath $heartbeatRoot -Filter 'bootstrap-heartbeat-v1.*.jsonl' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    $process = [Diagnostics.Process]::Start($info)
    return @{ process = $process; profile = $RoleProfile; processId = $process.Id
        heartbeatRoot = $heartbeatRoot; heartbeatBefore = $heartbeatBefore }
}

function Get-ProcessHeartbeat([hashtable]$RoleProcess) {
    if ($null -eq $RoleProcess) { return $null }
    return Get-ChildItem -LiteralPath $RoleProcess.heartbeatRoot `
        -Filter "bootstrap-heartbeat-v1.$($RoleProcess.processId).*.jsonl" -ErrorAction SilentlyContinue |
        Where-Object { $RoleProcess.heartbeatBefore -notcontains $_.FullName } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1 -ExpandProperty FullName
}

$python = (Get-Command python -ErrorAction Stop).Source
$driver = Join-Path $PSScriptRoot 'angreal_matrix_driver.py'
$verifier = Join-Path $PSScriptRoot 'verify_angreal_matrix.py'
$slicer = Join-Path $PSScriptRoot '..\sweep\sweep_checks.py'
$common = "-RenderOffscreen -windowed -ForceRes -ResX=$ResX -ResY=$ResY -unattended -nosplash -nocrashreports -nosound -stdout -log -nosteam"
if ($ExtraArgs) { $common = "$common $ExtraArgs" }
$hostProfile = Join-Path $profiles 'host'
$clientProfile = Join-Path $profiles 'client'
$hostRole = $null
$clientRole = $null
$driverExit = 1
try {
    New-PrivateStage $sourceStage (Join-Path $stages 'host')
    New-PrivateStage $sourceStage (Join-Path $stages 'client')
    $hostExe = Join-Path (Join-Path $stages 'host') $relativeExe
    $clientExe = Join-Path (Join-Path $stages 'client') $relativeExe
    Copy-SeedInstall $seedRoot (Join-Path $hostProfile 'AppData/Local/WoT')
    Copy-SeedInstall $seedRoot (Join-Path $clientProfile 'AppData/Local/WoT')
    $hostInstall = Join-Path $hostProfile 'AppData/Local/WoT'
    $clientInstall = Join-Path $clientProfile 'AppData/Local/WoT'
    $hostArgs = "$common -UserDir=`"$hostProfile`" -port=$GamePort -wottestinstallroot=`"$hostInstall`" -wotnettest -wotnettesttoken=$Token -wotnettestbind=127.0.0.1 -wotnettestport=$HostTestPort"
    $clientArgs = "$common -UserDir=`"$clientProfile`" -wottestinstallroot=`"$clientInstall`" -wotnettest -wotnettesttoken=$Token -wotnettestbind=127.0.0.1 -wotnettestport=$ClientTestPort"
    $hostRole = Start-IsolatedGame $hostProfile $hostExe $hostArgs
    $clientRole = Start-IsolatedGame $clientProfile $clientExe $clientArgs
    [ordered]@{ map = $Map; host_pid = $hostRole.processId; client_pid = $clientRole.processId
        game_port = $GamePort; host_test_port = $HostTestPort; client_test_port = $ClientTestPort
        stage = $sourceStage; exe_sha256 = (Get-FileHash -LiteralPath $sourceExe -Algorithm SHA256).Hash
        host_args = $hostArgs; client_args = $clientArgs; phases = $Phases; classes = $Classes
        directions = $Directions } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'launch.json') -Encoding UTF8
    $driverArgs = @($driver, '--map', $Map, '--host-port', $HostTestPort, '--client-port', $ClientTestPort,
        '--game-port', $GamePort, '--token', $Token, '--evidence', $run,
        '--host-screens', (Join-Path $hostProfile 'Saved/Screenshots'),
        '--client-screens', (Join-Path $clientProfile 'Saved/Screenshots'),
        '--host-pid', $hostRole.processId, '--client-pid', $clientRole.processId,
        '--phases', ($Phases -join ','), '--directions', $Directions,
        '--hold-on-failure', $HoldOnFailureSeconds, '--probe-seconds', $ProbeSeconds)
    if ($Classes.Count) { $driverArgs += @('--classes', ($Classes -join ',')) }
    if ($HostTimeLimit -ge 0) { $driverArgs += @('--host-timelimit', $HostTimeLimit) }
    & $python @driverArgs
    $driverExit = $LASTEXITCODE
}
finally {
    foreach ($owned in @($clientRole, $hostRole)) {
        if ($null -eq $owned) { continue }
        if (-not $owned.process.WaitForExit(30000)) {
            # Only the exact process objects started above; never by image name.
            $owned.process.Kill()
            $owned.process.WaitForExit(10000) | Out-Null
        }
    }
    foreach ($role in @('host', 'client')) {
        $roleProfile = Join-Path $profiles $role
        $roleProcess = $(if ($role -eq 'host') { $hostRole } else { $clientRole })
        $heartbeat = Get-ProcessHeartbeat $roleProcess
        if ($heartbeat) {
            & $python $slicer slice --heartbeat $heartbeat --pid $roleProcess.processId `
                --out (Join-Path $run "$role-heartbeat.jsonl") | Out-Null
            $global:LASTEXITCODE = 0
        }
        $screens = Join-Path $roleProfile 'Saved/Screenshots'
        if (Test-Path -LiteralPath $screens) {
            $frameDir = Join-Path $run "$role-frames"
            New-Item -ItemType Directory -Path $frameDir -Force | Out-Null
            Get-ChildItem -LiteralPath $screens -Recurse -Filter '*.png' | Sort-Object LastWriteTimeUtc |
                ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $frameDir $_.Name) }
        }
        $logs = Join-Path $roleProfile 'Saved/Logs'
        if (($driverExit -ne 0 -or $KeepLogs) -and (Test-Path -LiteralPath $logs)) {
            Copy-Item -LiteralPath $logs -Destination (Join-Path $run "$role-logs") -Recurse
        }
    }
    if (-not $KeepStages) {
        Remove-Item -LiteralPath $bulk -Recurse -Force -ErrorAction SilentlyContinue
    }
}
Write-Output $run
if ($ProbeSeconds -le 0) {
    & $python $verifier --evidence $run
    $verified = ($LASTEXITCODE -eq 0)
    if (-not $verified) { Write-Warning 'angreal matrix evidence did not verify (see verdicts.json)' }
}
if ($driverExit -ne 0) { throw "driver failed with exit $driverExit" }
