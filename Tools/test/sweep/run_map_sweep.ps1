[CmdletBinding()]
param(
    # The PRIVATE stage parent: the packaged Windows tree AND the adjacent
    # retail installer (the guarded bootstrap resolves the installer next to
    # the tree), e.g. .stripped-pak/<build>-staged. Never a shared package in use.
    [string]$StageRoot,
    [string]$Executable,
    [string]$EvidenceRoot = "Saved/ReleaseEvidence/map-sweep",
    # A prepared <LocalAppData>/WoT tree whose ContentCache matches the build
    # descriptor (only the generations its active pointers name are copied).
    [string]$SeedInstallRoot,
    [string[]]$Maps = @('Arena_01', 'Arena_02', 'Arena_03', 'Arena_04', 'Arena_05',
        'Arena_06', 'Arena_07', 'Arena_08', 'Arena_09', 'Arena_11', 'Arena_12',
        'Battle2_01', 'Battle2_02', 'Battle2_03', 'Battle2_04', 'Battle2_05',
        'Battle3_01', 'Battle3_02', 'Battle4_01'),
    # Ports rotate per map (index i): game BaseGamePort+i, channels
    # BaseTestPort+2i (host) and +2i+1 (client), so a lingering TIME_WAIT
    # socket from map i-1 can never collide with map i.
    [int]$BaseGamePort = 7977,
    [int]$BaseTestPort = 7887,
    [string]$Token = "wotnet",
    # Optional private loopback announcement endpoint (guarded Test builds
    # honour this override), so a run never reaches the public URL.
    [string]$AnnouncementUrl,
    [int]$ResX = 800,
    [int]$ResY = 450,
    [int]$HoldOnFailureSeconds = 0,
    # Re-seed both role profiles before every map (full first-boot isolation,
    # about 95 s of content-cache rebuild per role per map). By default a role
    # profile is reused across maps and re-seeded only after a failed map.
    [switch]$FreshProfilePerMap,
    # Keep each map's Saved/Logs even when it passed (always kept on failure).
    [switch]$KeepLogs,
    [switch]$StrictSkips,
    # Direct: the client connects to 127.0.0.1:<game port> (ConnectDirect).
    # Session: the client drives the REAL multiplayer browser - Play screen,
    # LAN box, Refresh until the host's row is listed, select it, Join - i.e.
    # Find -> JoinSession -> the resolved connect string -> travel.
    [ValidateSet('Direct', 'Session')][string]$JoinMode = 'Direct',
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
# `powershell -File` passes '-Maps A,B' as ONE string: accept either form.
$Maps = @($Maps | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$expectationsPath = Join-Path $PSScriptRoot 'sweep_expectations.json'
$expectations = Get-Content -LiteralPath $expectationsPath -Raw | ConvertFrom-Json
$mapPlan = @($Maps | ForEach-Object {
    $row = $expectations.maps.$_
    if (-not $row) { throw "$_ is not one of the 19 pinned MP maps" }
    [ordered]@{ map = $_; mode = $row.mode; title = $row.title
        pickup = [bool]$row.pickup; unresolved_slots = @($row.unresolved_slots) }
})
$plan = [ordered]@{
    schema = 'wot-map-sweep-v1'
    process_count_per_map = 2
    topology = 'listen-host-plus-client'
    maps = $mapPlan
    private_stage_copy = $true
    private_stage_copies = @('host', 'client')
    isolated_profiles = @('host', 'client')
    profile_reuse = $(if ($FreshProfilePerMap) { 'fresh-per-map' } else { 'reused-reseeded-after-failure' })
    join_mode = $JoinMode.ToLowerInvariant()
    channel_flow = @(@('wait-front-end-ready', 'host-map', 'wait-listen-world') +
        $(if ($JoinMode -eq 'Session') { @('browser-find-lan', 'browser-row-listed', 'browser-join-row') }
          else { @('join-address') }) +
        @('wait-client-world', 'check-titles', 'screenshot', 'arena-pickup', 'quit'))
    seed_install_root = 'required prepared AppData/Local/WoT tree matching the build descriptor'
    heartbeat_source = '<role-profile>/AppData/Local/WoT/UpdaterState/bootstrap-heartbeat-v1.<pid>.<guid>.jsonl for the launched map process'
    required_outputs = @('summary.json', 'transcript.json', 'host-heartbeat.jsonl',
        'client-heartbeat.jsonl', 'host-frame.png', 'client-frame.png')
    verifier = 'verify_map_sweep.ps1 (sweep_checks.py verify)'
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
$stages = Join-Path $run 'stages'
New-Item -ItemType Directory -Path $run, $profiles, $stages | Out-Null
$relativeExe = $sourceExe.Substring($sourceStage.Length).TrimStart('\', '/')

# Isolation helpers: the same rules as Tools/test/battle/run_seal_match.ps1
# (full byte copies - the installed-session check rejects hard links; seed
# only the active ContentCache generations, never UpdaterState/locks/leases).
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

# A fresh role profile: the whole profile directory is replaced by a new seed.
function Reset-RoleProfile([string]$RoleProfile) {
    if (Test-Path -LiteralPath $RoleProfile) {
        Remove-Item -LiteralPath $RoleProfile -Recurse -Force
    }
    Copy-SeedInstall $seedRoot (Join-Path $RoleProfile 'AppData/Local/WoT')
}

# Detached launch of one packaged instance with an isolated profile. ($host is
# a read-only PowerShell automatic variable; roles are hostRole/clientRole.)
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

function Get-FrameNames([string]$Folder) {
    if (-not (Test-Path -LiteralPath $Folder)) { return @() }
    return @(Get-ChildItem -LiteralPath $Folder -Recurse -Filter '*.png' | ForEach-Object { $_.FullName })
}

$python = (Get-Command python -ErrorAction Stop).Source
$checks = Join-Path $PSScriptRoot 'sweep_checks.py'
$driver = Join-Path $PSScriptRoot 'map_sweep_driver.py'
$common = "-RenderOffscreen -windowed -ForceRes -ResX=$ResX -ResY=$ResY -unattended -nosplash -nocrashreports -nosound -stdout -log -nosteam"
$hostProfile = Join-Path $profiles 'host'
$clientProfile = Join-Path $profiles 'client'
$hostStage = Join-Path $stages 'host'
$clientStage = Join-Path $stages 'client'
$results = @()
$failures = @()
try {
    New-PrivateStage $sourceStage $hostStage
    New-PrivateStage $sourceStage $clientStage
    $hostExe = Join-Path $hostStage $relativeExe
    $clientExe = Join-Path $clientStage $relativeExe
    Reset-RoleProfile $hostProfile
    Reset-RoleProfile $clientProfile
    $index = 0
    foreach ($entry in $mapPlan) {
        $map = $entry.map
        $mapEvidence = Join-Path $run $map
        New-Item -ItemType Directory -Path $mapEvidence | Out-Null
        if ($FreshProfilePerMap -and $index -gt 0) {
            Reset-RoleProfile $hostProfile
            Reset-RoleProfile $clientProfile
        }
        $gamePort = $BaseGamePort + $index
        $hostTestPort = $BaseTestPort + 2 * $index
        $clientTestPort = $BaseTestPort + 2 * $index + 1
        $hostInstall = Join-Path $hostProfile 'AppData/Local/WoT'
        $clientInstall = Join-Path $clientProfile 'AppData/Local/WoT'
        $hostScreens = Join-Path $hostProfile 'Saved/Screenshots'
        $clientScreens = Join-Path $clientProfile 'Saved/Screenshots'
        $framesBefore = @{ host = Get-FrameNames $hostScreens; client = Get-FrameNames $clientScreens }
        $hostArgs = "$common -UserDir=`"$hostProfile`" -port=$gamePort -wottestinstallroot=`"$hostInstall`" -wotnettest -wotnettesttoken=$Token -wotnettestbind=127.0.0.1 -wotnettestport=$hostTestPort"
        $clientArgs = "$common -UserDir=`"$clientProfile`" -wottestinstallroot=`"$clientInstall`" -wotnettest -wotnettesttoken=$Token -wotnettestbind=127.0.0.1 -wotnettestport=$clientTestPort"
        $hostRole = $null
        $clientRole = $null
        $driverExit = 1
        # Startup, launch metadata and the driver share one try so a failure
        # while starting the client (or writing launch.json) still reaps the
        # host that already started.
        try {
            $hostRole = Start-IsolatedGame $hostProfile $hostExe $hostArgs
            $clientRole = Start-IsolatedGame $clientProfile $clientExe $clientArgs
            [ordered]@{ map = $map; host_pid = $hostRole.processId; client_pid = $clientRole.processId
                game_port = $gamePort; host_test_port = $hostTestPort; client_test_port = $clientTestPort
                join_mode = $JoinMode.ToLowerInvariant()
                host_args = $hostArgs; client_args = $clientArgs } |
                ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $mapEvidence 'launch.json') -Encoding UTF8
            & $python $driver --map $map --host-port $hostTestPort --client-port $clientTestPort `
                --game-port $gamePort --token $Token --evidence $mapEvidence `
                --host-screens $hostScreens --client-screens $clientScreens `
                --host-pid $hostRole.processId --client-pid $clientRole.processId `
                --hold-on-failure $HoldOnFailureSeconds --join-mode $JoinMode.ToLowerInvariant()
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
        }
        foreach ($role in @('host', 'client')) {
            $roleProfile = Join-Path $profiles $role
            $roleProcess = $(if ($role -eq 'host') { $hostRole } else { $clientRole })
            if ($null -eq $roleProcess) { continue }
            $heartbeat = Get-ProcessHeartbeat $roleProcess
            if ($heartbeat) {
                # The writer has exited: slice exactly this map's process.
                & $python $checks slice --heartbeat $heartbeat --pid $roleProcess.processId `
                    --out (Join-Path $mapEvidence "$role-heartbeat.jsonl") | Out-Null
                $global:LASTEXITCODE = 0
            }
            $screens = Join-Path $roleProfile 'Saved/Screenshots'
            $fresh = @(Get-FrameNames $screens | Where-Object { $framesBefore[$role] -notcontains $_ } |
                ForEach-Object { Get-Item -LiteralPath $_ } | Sort-Object LastWriteTimeUtc)
            if ($fresh.Count -gt 0) {
                $frameDir = Join-Path $mapEvidence "$role-frames"
                New-Item -ItemType Directory -Path $frameDir -Force | Out-Null
                $frameIndex = 0
                foreach ($frame in $fresh) {
                    Copy-Item -LiteralPath $frame.FullName -Destination (Join-Path $frameDir ('{0:D2}-{1}' -f $frameIndex, $frame.Name))
                    $frameIndex++
                }
                Copy-Item -LiteralPath $fresh[0].FullName -Destination (Join-Path $mapEvidence "$role-frame.png")
                # Screens are per map: leave none behind for the next map.
                $fresh | Remove-Item -Force -ErrorAction SilentlyContinue
            }
            $logs = Join-Path $roleProfile 'Saved/Logs'
            if (($driverExit -ne 0 -or $KeepLogs) -and (Test-Path -LiteralPath $logs)) {
                Copy-Item -LiteralPath $logs -Destination (Join-Path $mapEvidence "$role-logs") -Recurse
            }
            if (Test-Path -LiteralPath $logs) {
                Remove-Item -LiteralPath $logs -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        $summaryPath = Join-Path $mapEvidence 'summary.json'
        if (Test-Path -LiteralPath $summaryPath) {
            $results += Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        }
        if ($driverExit -ne 0) {
            $failures += "$map driver failed with exit $driverExit"
            # A failed map may leave locks/leases or a half-built cache behind:
            # the next map starts from a fresh seed on both roles.
            if ($index + 1 -lt $mapPlan.Count) {
                Reset-RoleProfile $hostProfile
                Reset-RoleProfile $clientProfile
            }
        }
        $index++
    }
}
finally {
    foreach ($role in @('host', 'client')) {
        $heartbeatRoot = Join-Path $profiles "$role/AppData/Local/WoT/UpdaterState"
        foreach ($heartbeat in Get-ChildItem -LiteralPath $heartbeatRoot -Filter 'bootstrap-heartbeat-v1.*.jsonl' -ErrorAction SilentlyContinue) {
            Copy-Item -LiteralPath $heartbeat.FullName -Destination (Join-Path $run "$role-$($heartbeat.Name)")
        }
    }
    # Keep the evidence small: the private stages (GBs), rebuilt content
    # caches (about 1 GB per role) and driver shader caches are disposable.
    foreach ($bulk in @($stages, $profiles)) {
        Remove-Item -LiteralPath $bulk -Recurse -Force -ErrorAction SilentlyContinue
    }
}
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'run-summary.json') -Encoding UTF8
Write-Output $run
$verifyArguments = @{ EvidenceRoot = $run; Maps = $Maps; JoinMode = $JoinMode }
if ($StrictSkips) { $verifyArguments['StrictSkips'] = $true }
& (Join-Path $PSScriptRoot 'verify_map_sweep.ps1') @verifyArguments
$verified = $?
if ($failures.Count -gt 0) { throw ($failures -join '; ') }
if (-not $verified) { throw 'map sweep evidence did not verify' }
