# capture_v3.ps1 -- golden-set capture. One fresh WoT.exe per map; commands sent
# blind; each F9 verified by BMP appearing; graceful console 'exit' at map end
# flushes the whole UE1 log, which is then audited for every expected
# "Console Command:" echo. Any discrepancy discards the map and retries once.
param(
    [Parameter(Mandatory=$true)][string[]]$Maps,
    # Capture from an isolated retail copy when supplied.  The default keeps
    # the long-standing repo-local workflow for deliberate manual use.
    [string]$SystemDir = '',
    [string]$PosesJson = '',
    [string]$StageRoot = (Join-Path $env:TEMP 'wot-capture-stage'),
    [string[]]$ReverseMaps = @('Arena_01','Arena_02','Arena_12','Battle2_02'),
    [string]$DriverLog = (Join-Path $env:TEMP 'wot-capture-driver.log')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\wot_config.ps1')
$sysDir = if ($SystemDir) {
    (Resolve-Path $SystemDir).Path
} else {
    Join-Path (Get-WotReferenceDirectory 'original') 'System'
}
if (-not $PosesJson) {
    $PosesJson = Join-Path (Get-WotReferenceDirectory 'exports') 'reference\shots\poses.json'
}
$gameLog = Join-Path $sysDir 'WoT.log'
$runningIni = Join-Path $sysDir 'Running.ini'
$exe = Join-Path $sysDir 'WoT.exe'
$script:GameProcess = $null

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WotInput
{
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr hWnd, bool fAltTab);
    [DllImport("user32.dll")] public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public InputUnion U; }
    // MOUSEINPUT must stay in the union: it is the largest member and defines
    // sizeof(INPUT)=40 on x64. Without it SendInput rejects every call (cbSize
    // mismatch) and returns 0 -- silently, unless checked.
    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion
    {
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public MOUSEINPUT mi;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const uint KEYEVENTF_UNICODE = 0x0004;
    static void Inject(INPUT[] batch)
    {
        if (SendInput(1, batch, Marshal.SizeOf(typeof(INPUT))) == 0)
            throw new InvalidOperationException("SendInput injected 0 events");
    }
    public static void KeyPress(ushort vk, int holdMs)
    {
        INPUT[] d = new INPUT[1]; d[0].type = 1; d[0].U.ki.wVk = vk;
        Inject(d);
        System.Threading.Thread.Sleep(holdMs);
        INPUT[] u = new INPUT[1]; u[0].type = 1; u[0].U.ki.wVk = vk; u[0].U.ki.dwFlags = KEYEVENTF_KEYUP;
        Inject(u);
    }
    public static void CharPress(char c, int holdMs)
    {
        INPUT[] d = new INPUT[1]; d[0].type = 1; d[0].U.ki.wScan = (ushort)c; d[0].U.ki.dwFlags = KEYEVENTF_UNICODE;
        Inject(d);
        System.Threading.Thread.Sleep(holdMs);
        INPUT[] u = new INPUT[1]; u[0].type = 1; u[0].U.ki.wScan = (ushort)c; u[0].U.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        Inject(u);
    }
}
'@

function Log-Driver([string]$msg) {
    $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $msg
    Add-Content -Path $DriverLog -Value $line
    Write-Host $line
}

function Get-Game {
    if (-not $script:GameProcess) { return $null }
    try {
        $script:GameProcess.Refresh()
        if ($script:GameProcess.HasExited) {
            $script:GameProcess = $null
            return $null
        }
        return $script:GameProcess
    } catch {
        $script:GameProcess = $null
        return $null
    }
}

function Ensure-Focus {
    $p = Get-Game
    if (-not $p) { throw 'game process died' }
    if ($p.MainWindowTitle -match 'Recovery|Configuration') { throw "stuck at dialog: $($p.MainWindowTitle)" }
    $hwnd = $p.MainWindowHandle
    if ([WotInput]::GetForegroundWindow() -ne $hwnd) {
        [WotInput]::SwitchToThisWindow($hwnd, $true)
        Start-Sleep -Milliseconds 800
        if ([WotInput]::GetForegroundWindow() -ne $hwnd) { throw 'cannot focus game window' }
        Log-Driver 'refocused game window'
    }
}

function Send-Command([string]$cmd) {
    Ensure-Focus
    [WotInput]::KeyPress(0x09, 40)   # Tab -> type bar
    Start-Sleep -Milliseconds 300
    foreach ($c in $cmd.ToCharArray()) {
        [WotInput]::CharPress($c, 12)
        Start-Sleep -Milliseconds 12
    }
    Start-Sleep -Milliseconds 150
    [WotInput]::KeyPress(0x0D, 40)   # Enter
    Start-Sleep -Milliseconds 250
}

function Send-F9 { Ensure-Focus; [WotInput]::KeyPress(0x78, 40) }

function Count-Bmp { @(Get-ChildItem (Join-Path $sysDir 'Shot*.bmp') -ErrorAction SilentlyContinue).Count }

function Kill-Game {
    $owned = Get-Game
    if ($owned) { Stop-Process -Id $owned.Id -Force -ErrorAction SilentlyContinue }
    for ($i = 0; $i -lt 10 -and (Get-Game); $i++) { Start-Sleep -Milliseconds 500 }
    Remove-Item $runningIni -Force -ErrorAction SilentlyContinue
}

# Graceful console-exit. Returns $true when the process ended by itself
# (=> console worked and the log was flushed to disk).
function Exit-Game {
    if (-not (Get-Game)) { return $true }
    try { Send-Command 'exit' } catch { Log-Driver "exit send failed: $_"; Kill-Game; return $false }
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 500
        if (-not (Get-Game)) { Start-Sleep -Seconds 1; return $true }
    }
    Log-Driver 'graceful exit timed out -- force killing'
    Kill-Game
    return $false
}

# Launch into a map; liveness via window title (log may still be buffered).
function Start-GameMap([string]$map) {
    if (Get-Game) { [void](Exit-Game) }
    Remove-Item $gameLog -Force -ErrorAction SilentlyContinue
    Remove-Item $runningIni -Force -ErrorAction SilentlyContinue
    $script:GameProcess = Start-Process -FilePath $exe -ArgumentList "$map.wot" `
        -WorkingDirectory $sysDir -PassThru
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Seconds 2
        $p = Get-Game
        if (-not $p) {
            if ($i -gt 4) { Log-Driver 'game exited during load'; return $false }
            continue
        }
        if ($p.MainWindowTitle -match 'Recovery|Configuration') {
            Log-Driver "dialog appeared: '$($p.MainWindowTitle)' -- clearing"
            Kill-Game
            return $false
        }
        if ($p.MainWindowTitle -eq 'Wheel of Time') {
            Start-Sleep -Seconds 10
            # readiness probe: the game ignores input while precaching; the first
            # F9 that lands a BMP proves the session is interactive
            for ($probe = 0; $probe -lt 15; $probe++) {
                $nBefore = Count-Bmp
                try { Send-F9 } catch { Log-Driver "probe focus error: $_"; return $false }
                Start-Sleep -Milliseconds 1500
                if ((Count-Bmp) -gt $nBefore) {
                    Log-Driver "game interactive after probe $($probe+1)"
                    $probeShots = Get-ChildItem (Join-Path $sysDir 'Shot*.bmp') -ErrorAction SilentlyContinue
                    if ($probeShots) {
                        $lo = Join-Path $StageRoot '_leftover'
                        New-Item -ItemType Directory -Force $lo | Out-Null
                        $probeShots | ForEach-Object { Move-Item $_.FullName (Join-Path $lo ("probe_$(Get-Random)_" + $_.Name)) -Force }
                    }
                    return $true
                }
                Start-Sleep -Milliseconds 3500
            }
            Log-Driver 'game never became interactive (15 probes)'
            return $false
        }
    }
    Log-Driver 'no game window in 40s'
    return $false
}

$poses = Get-Content $PosesJson -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force $StageRoot | Out-Null

foreach ($map in $Maps) {
    $starts = $poses.$map
    if (-not $starts) { Log-Driver "SKIP $map -- no poses"; continue }
    $mapStage = Join-Path $StageRoot $map

    $mapDone = $false
    for ($mapTry = 0; $mapTry -lt 2 -and -not $mapDone; $mapTry++) {
        # park stray shots so numbering starts at Shot0000
        $stray = Get-ChildItem (Join-Path $sysDir 'Shot*.bmp') -ErrorAction SilentlyContinue
        if ($stray) {
            $lo = Join-Path $StageRoot '_leftover'
            New-Item -ItemType Directory -Force $lo | Out-Null
            $stray | ForEach-Object { Move-Item $_.FullName (Join-Path $lo ("$(Get-Random)_" + $_.Name)) -Force }
        }

        Log-Driver "=== $map (attempt $($mapTry+1)): launching"
        if (-not (Start-GameMap $map)) { continue }

        $expectedEchoes = New-Object System.Collections.Generic.List[string]
        $failed = $false
        try {
            foreach ($cmd in @('ToggleUI','ghost','fly')) { Send-Command $cmd; $expectedEchoes.Add($cmd) }
            Start-Sleep -Seconds 6   # console messages fade

            $manifest = @()
            $doReverse = $ReverseMaps -contains $map
            $shotIdx = 0
            foreach ($s in $starts) {
                # Preserve authored diagnostic coordinates (including fractions)
                # and optional pitch/roll.  PlayerStart manifests omit the latter
                # and therefore retain the historical pitch=roll=0 contract.
                $px = [double]$s.x; $py = [double]$s.y; $pz = [double]$s.z
                $jx = [Convert]::ToString($px, [Globalization.CultureInfo]::InvariantCulture)
                $jy = [Convert]::ToString($py, [Globalization.CultureInfo]::InvariantCulture)
                $jz = [Convert]::ToString($pz, [Globalization.CultureInfo]::InvariantCulture)
                $pitch = if ($null -ne $s.pitch) { [double]$s.pitch } else { 0.0 }
                $roll = if ($null -ne $s.roll) { [double]$s.roll } else { 0.0 }
                $views = @(@{ tag = 'fwd'; yaw = [double]$s.yaw; pitch = $pitch; roll = $roll })
                if ($doReverse) {
                    $views += @{ tag = 'rev'; yaw = (($s.yaw + 32768) % 65536); pitch = $pitch; roll = $roll }
                }
                foreach ($v in $views) {
					# Diagnostic poses may deliberately sit inside a volume or brush where
					# the collision-aware JumpTo exec refuses with "Couldn't set location".
					# Opt in per pose to the native property seam; ordinary golden
					# PlayerStart captures keep their historical JumpTo behavior.
					$jumpCmd = if ($s.direct_location) {
						"set WOTPlayer Location (X=$jx,Y=$jy,Z=$jz)"
					} else {
						"JumpTo $jx $jy $jz"
					}
                    $rotCmd  = "set WOTPlayer ViewRotation (Pitch=$($v.pitch),Yaw=$($v.yaw),Roll=$($v.roll))"
                    Send-Command $jumpCmd; $expectedEchoes.Add($jumpCmd)
                    Send-Command $rotCmd;  $expectedEchoes.Add($rotCmd)
                    Start-Sleep -Milliseconds 2800
                    $shotOk = $false
                    for ($st = 0; $st -lt 2 -and -not $shotOk; $st++) {
                        $nBefore = Count-Bmp
                        Send-F9
                        Start-Sleep -Milliseconds 900
                        if ((Count-Bmp) -gt $nBefore) { $shotOk = $true }
                    }
                    if (-not $shotOk) { throw "no BMP for $($s.name) $($v.tag)" }
                    $manifest += [ordered]@{
                        order  = $shotIdx
                        source = $s.name
                        view   = $v.tag
						location_command = if ($s.direct_location) { 'set-property' } else { 'JumpTo' }
                        pawn   = [ordered]@{ x = $px; y = $py; z = $pz }
                        rot    = [ordered]@{ pitch = $v.pitch; yaw = $v.yaw; roll = $v.roll }
                    }
                    $shotIdx++
                }
            }
        }
        catch { Log-Driver "capture aborted: $_"; $failed = $true }

        $flushed = Exit-Game
        if ($failed -or -not $flushed) { Log-Driver "$map attempt failed (failed=$failed flushed=$flushed)"; continue }

        # post-hoc audit: every command must have echoed in the flushed log
        $logTxt = Get-Content $gameLog -Raw -ErrorAction SilentlyContinue
        if (-not $logTxt) { Log-Driver "$map : empty log after exit -- retrying"; continue }
        $missing = @($expectedEchoes | Where-Object { -not $logTxt.Contains("Console Command: $_") })
        if ($missing.Count -gt 0) {
            Log-Driver "$map : $($missing.Count)/$($expectedEchoes.Count) echoes missing (first: '$($missing[0])') -- retrying"
            continue
        }

        $bmps = @(Get-ChildItem (Join-Path $sysDir 'Shot*.bmp') -ErrorAction SilentlyContinue | Sort-Object Name)
        if ($bmps.Count -ne $shotIdx) { Log-Driver "$map : BMP count $($bmps.Count) != manifest $shotIdx -- retrying"; continue }

        New-Item -ItemType Directory -Force $mapStage | Out-Null
        $bmps | Move-Item -Destination $mapStage -Force
        $meta = [ordered]@{
            map         = $map
            renderer    = 'GlideDrv via nGlide (Glide 2.60 reported), 1600x1200, 16-bit'
            brightness  = 0.6
            # 29.4, NOT BaseEyeHeight 42: the ghost/fly capture pawn sits in the
            # jump/fall anim state, EyeHeight = 0.7*BaseEyeHeight (WOTPlayer.uc:2261
            # PlayInAir / :2283 Falling). Measured by per-pose parallax fit
            # (12 poses / 6 maps, 1uu steps -> consensus -12 from 42; 0.7*42 = 29.4).
            eye_height  = 29.4
            fov         = 90
			camera_note = 'camera = pawn location + (0,0,eye_height); ordinary poses use JumpTo, while direct_location diagnostic poses use the exact WOTPlayer Location property; rotation exact via set WOTPlayer ViewRotation. eye_height = 0.7*BaseEyeHeight (fly-mode jump/fall anim state), parallax-fit verified 2026-07-06'
            shots       = $manifest
        }
        [System.IO.File]::WriteAllText((Join-Path $mapStage 'manifest.json'), ($meta | ConvertTo-Json -Depth 5))
        Log-Driver "$map : AUDIT PASSED, staged $($bmps.Count) shots"
        $mapDone = $true
    }
    if (-not $mapDone) { Log-Driver "GIVING UP on $map" }
}
if (Get-Game) { [void](Exit-Game) }
Log-Driver 'RUN COMPLETE'

