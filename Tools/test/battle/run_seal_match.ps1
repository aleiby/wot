[CmdletBinding()]
param(
    # The PRIVATE stage parent: it holds the packaged Windows tree AND the
    # adjacent retail installer (the guarded bootstrap resolves the installer
    # next to the tree), e.g. .stripped-pak/b3-2-staged.
    [string]$StageRoot,
    [string]$Executable,
    [string]$EvidenceRoot = "Saved/ReleaseEvidence/t6-2",
    [int]$GamePort = 7877,
    [int]$HostTestPort = 7787,
    [int]$ClientTestPort = 7788,
    [string]$Token = "wotnet",
    # A prepared <LocalAppData>/WoT tree whose ContentCache matches the build
    # descriptor (only the generations its active pointers name are copied).
    [string]$SeedInstallRoot,
    # Optional private loopback announcement endpoint (guarded Test builds
    # honour this override), so a run never reaches the public URL.
    [string]$AnnouncementUrl,
    [string[]]$Maps = @('Battle2_01', 'Battle3_01'),
    [int]$ResX = 960,
    [int]$ResY = 540,
    [int]$HoldOnFailureSeconds = 0,
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
$modes = @{ 'Battle2_01' = 'full-seal-objective'; 'Battle3_01' = 'projection-smoke' }
$mapPlan = @($Maps | ForEach-Object {
    $mode = $modes[$_]
    if (-not $mode) { $mode = 'projection-smoke' }
    @{ map = $_; mode = $mode }
})
$plan = [ordered]@{
    schema = 'wot-two-instance-seal-match-v1'
    process_count = 2
    topology = 'listen-host-plus-client'
    maps = $mapPlan
    private_stage_copy = $true
    private_stage_copies_per_map = 2
    isolated_profiles = @('host', 'client')
    launch_url = $null
    channel_flow = @('wait-front-end-ready', 'host-map', 'wait-listen-world',
        'join-address', 'wait-client-world', 'drive-match')
    seed_install_root = 'required prepared AppData/Local/WoT tree matching the build descriptor'
    heartbeat_source = '<role-profile>/AppData/Local/WoT/UpdaterState/bootstrap-heartbeat-v1.<pid>.<guid>.jsonl'
    required_outputs = @('summary.json', 'transcript.json', 'host-heartbeat.jsonl',
        'client-heartbeat.jsonl', 'host-frame.png', 'client-frame.png')
}
if ($PlanOnly) {
    $plan | ConvertTo-Json -Depth 6
    exit 0
}
if ([string]::IsNullOrWhiteSpace($StageRoot) -or
    [string]::IsNullOrWhiteSpace($Executable) -or
    [string]::IsNullOrWhiteSpace($SeedInstallRoot)) {
    throw '-StageRoot, -Executable, and -SeedInstallRoot are required unless -PlanOnly is used'
}
$sourceStage = (Resolve-Path -LiteralPath $StageRoot).Path.TrimEnd('\', '/')
$sourceExe = (Resolve-Path -LiteralPath $Executable).Path
$seedRoot = (Resolve-Path -LiteralPath $SeedInstallRoot).Path.TrimEnd('\', '/')
if (-not $sourceExe.StartsWith($sourceStage + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw '-Executable must be inside -StageRoot so each run uses its private staged copy'
}
$root = [IO.Path]::GetFullPath((Join-Path (Get-Location) $EvidenceRoot))
$run = Join-Path $root ((Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ'))
$profiles = Join-Path $run 'profiles'
New-Item -ItemType Directory -Path $run, $profiles | Out-Null
$relativeExe = $sourceExe.Substring($sourceStage.Length).TrimStart('\', '/')

# A private per-role stage tree: a full byte copy. (Hard links are NOT an
# option: the guarded installed-session check rejects an installed file that
# has another hard-link name - CorruptInstallation, seen on the first run.)
function New-PrivateStage([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    & robocopy $Source $Destination /E /COPY:DAT /R:1 /W:1 /MT:8 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy of the private stage failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

# Seed a role's install root: no UpdaterState (each role establishes its own
# identity and writes its own heartbeat), no locks or leases, and only the
# ContentCache generations named by the active pointers.
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

# Detached launch of one packaged instance with an isolated profile. ($host is
# a read-only PowerShell automatic variable; roles are named hostRole/clientRole.)
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
    return @{ process = $process; profile = $RoleProfile; pid = $process.Id; processId = $process.Id
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
$common = "-RenderOffscreen -windowed -ForceRes -ResX=$ResX -ResY=$ResY -unattended -nosplash -nocrashreports -nosound -stdout -log -nosteam"
$results = @()
$failures = @()
foreach ($entry in $mapPlan) {
    $map = $entry.map
    $mapEvidence = Join-Path $run $map
    New-Item -ItemType Directory -Path $mapEvidence | Out-Null
    $hostStage = Join-Path $mapEvidence 'host-stage'
    $clientStage = Join-Path $mapEvidence 'client-stage'
    New-PrivateStage $sourceStage $hostStage
    New-PrivateStage $sourceStage $clientStage
    $hostExe = Join-Path $hostStage $relativeExe
    $clientExe = Join-Path $clientStage $relativeExe
    $hostProfile = Join-Path $profiles "$map-host"
    $clientProfile = Join-Path $profiles "$map-client"
    $hostInstall = Join-Path $hostProfile 'AppData/Local/WoT'
    $clientInstall = Join-Path $clientProfile 'AppData/Local/WoT'
    Copy-SeedInstall $seedRoot $hostInstall
    Copy-SeedInstall $seedRoot $clientInstall
    $hostArgs = "$common -UserDir=`"$hostProfile`" -port=$GamePort -wottestinstallroot=`"$hostInstall`" -wotnettest -wotnettesttoken=$Token -wotnettestbind=127.0.0.1 -wotnettestport=$HostTestPort"
    $clientArgs = "$common -UserDir=`"$clientProfile`" -wottestinstallroot=`"$clientInstall`" -wotnettest -wotnettesttoken=$Token -wotnettestbind=127.0.0.1 -wotnettestport=$ClientTestPort"
    $hostRole = Start-IsolatedGame $hostProfile $hostExe $hostArgs
    $clientRole = Start-IsolatedGame $clientProfile $clientExe $clientArgs
    [ordered]@{ map = $map; host_pid = $hostRole.pid; client_pid = $clientRole.pid
        host_args = $hostArgs; client_args = $clientArgs
        host_exe = $hostExe; client_exe = $clientExe } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $mapEvidence 'launch.json') -Encoding UTF8
    $driverExit = 1
    try {
        $driver = Join-Path $PSScriptRoot 'seal_match_driver.py'
        & $python $driver --map $map --host-port $HostTestPort --client-port $ClientTestPort `
            --game-port $GamePort --token $Token --evidence $mapEvidence `
            --host-screens (Join-Path $hostProfile 'Saved/Screenshots') `
            --client-screens (Join-Path $clientProfile 'Saved/Screenshots') `
            --host-pid $hostRole.pid --client-pid $clientRole.pid `
            --hold-on-failure $HoldOnFailureSeconds `
            --host-heartbeat $hostRole.heartbeatRoot `
            --client-heartbeat $clientRole.heartbeatRoot
        $driverExit = $LASTEXITCODE
    }
    finally {
        foreach ($owned in @($clientRole, $hostRole)) {
            if (-not $owned.process.WaitForExit(20000)) {
                # Only the exact process objects started above; never by image name.
                $owned.process.Kill()
                $owned.process.WaitForExit(10000) | Out-Null
            }
        }
    }
    foreach ($role in @('host', 'client')) {
        $roleProfile = Join-Path $profiles "$map-$role"
        $roleProcess = $(if ($role -eq 'host') { $hostRole } else { $clientRole })
        $heartbeat = Get-ProcessHeartbeat $roleProcess
        if ($heartbeat) {
            Copy-Item -LiteralPath $heartbeat -Destination (Join-Path $mapEvidence "$role-heartbeat.jsonl")
        }
        $frames = @(Get-ChildItem -LiteralPath $roleProfile -Recurse -Filter '*.png' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc)
        if ($frames.Count -gt 0) {
            $frameDir = Join-Path $mapEvidence "$role-frames"
            New-Item -ItemType Directory -Path $frameDir -Force | Out-Null
            $index = 0
            foreach ($frame in $frames) {
                Copy-Item -LiteralPath $frame.FullName -Destination (Join-Path $frameDir ('{0:D2}-{1}' -f $index, $frame.Name))
                $index++
            }
            Copy-Item -LiteralPath $frames[-1].FullName -Destination (Join-Path $mapEvidence "$role-frame.png")
        }
        $logs = Join-Path $roleProfile 'Saved/Logs'
        if (Test-Path -LiteralPath $logs) {
            Copy-Item -LiteralPath $logs -Destination (Join-Path $mapEvidence "$role-logs") -Recurse
        }
        # The rebuilt content cache (about 0.5-1 GB per role) and driver shader
        # caches are disposable once the run record has been collected.
        foreach ($bulk in @('AppData/Local/WoT/ContentCache', 'AppData/Local/NVIDIA',
                'AppData/Local/AMD', 'AppData/Local/D3DSCache')) {
            Remove-Item -LiteralPath (Join-Path $roleProfile $bulk) -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    # The private stage trees are disposable (hard links + small copies).
    Remove-Item -LiteralPath $hostStage, $clientStage -Recurse -Force -ErrorAction SilentlyContinue
    $summaryPath = Join-Path $mapEvidence 'summary.json'
    if (Test-Path -LiteralPath $summaryPath) {
        $results += Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    }
    if ($driverExit -ne 0) { $failures += "$map driver failed with exit $driverExit" }
}
$results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'run-summary.json') -Encoding UTF8
Write-Output $run
if ($failures.Count -gt 0) { throw ($failures -join '; ') }
& (Join-Path $PSScriptRoot 'verify_seal_match.ps1') -EvidenceRoot $run -Maps $Maps
