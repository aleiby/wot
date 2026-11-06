# feel_record.ps1 -- Deliverable 2: 60fps feel recordings from the original game.
# Sections: run (straight-line acceleration), jumps (in place), runjumps,
# angreal (one video per specified ter'angreal fired at a wall).
# Requires: game NOT running; ffmpeg on PATH; nGlide aspect set (or use -CaptureFull).
param(
    [Parameter(Mandatory=$true)][ValidateSet('run','jumps','runjumps','angreal','static')][string]$Section,
    [string]$Label = 'static',
    [int]$Pitch = 0,
    [switch]$Clean,   # hide HUD + ghost (clean plates for flipbook calibration)
    [string]$Map = 'Arena_01',
    [int]$JumpToX = -3973, [int]$JumpToY = 3754, [int]$JumpToZ = -452,
    [int]$Yaw = 24096,
    [string]$AngrealClasses = 'AngrealInvFireball',   # comma-separated, all in one session
    [int]$DurationSec = 14,
    [string]$OutDir = '',
    [string]$SystemDir = '',
    [string]$CaptureRegion = '1600x1200+0+0'   # game mode-switches primary to 1600x1200 at origin
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\wot_config.ps1')
$sysDir = if ($SystemDir) { $SystemDir } else {
    Join-Path (Get-WotReferenceDirectory 'original') 'System'
}
if (-not $OutDir) {
    $OutDir = Join-Path (Get-WotReferenceDirectory 'exports') 'reference\feel'
}

# --- input helper (full INPUT union incl. mouse; see Tools/reference/README.md) ---
if (-not ([System.Management.Automation.PSTypeName]'WotFeelInput').Type) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WotFeelInput
{
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr hWnd, bool fAltTab);
    [DllImport("user32.dll")] public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public InputUnion U; }
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
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    static void Inject(INPUT[] batch)
    {
        if (SendInput(1, batch, Marshal.SizeOf(typeof(INPUT))) == 0)
            throw new InvalidOperationException("SendInput injected 0 events");
    }
    public static void KeyDown(ushort vk) { INPUT[] d = new INPUT[1]; d[0].type = 1; d[0].U.ki.wVk = vk; Inject(d); }
    public static void KeyUp(ushort vk)   { INPUT[] u = new INPUT[1]; u[0].type = 1; u[0].U.ki.wVk = vk; u[0].U.ki.dwFlags = KEYEVENTF_KEYUP; Inject(u); }
    public static void KeyPress(ushort vk, int holdMs) { KeyDown(vk); System.Threading.Thread.Sleep(holdMs); KeyUp(vk); }
    public static void CharPress(char c, int holdMs)
    {
        INPUT[] d = new INPUT[1]; d[0].type = 1; d[0].U.ki.wScan = (ushort)c; d[0].U.ki.dwFlags = KEYEVENTF_UNICODE;
        Inject(d);
        System.Threading.Thread.Sleep(holdMs);
        INPUT[] u = new INPUT[1]; u[0].type = 1; u[0].U.ki.wScan = (ushort)c; u[0].U.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        Inject(u);
    }
    public static void MouseClick(int holdMs)
    {
        INPUT[] d = new INPUT[1]; d[0].type = 0; d[0].U.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; Inject(d);
        System.Threading.Thread.Sleep(holdMs);
        INPUT[] u = new INPUT[1]; u[0].type = 0; u[0].U.mi.dwFlags = MOUSEEVENTF_LEFTUP; Inject(u);
    }
}
'@
}

function Ensure-Focus {
    $p = Get-Process WoT -ErrorAction SilentlyContinue
    if (-not $p) { throw 'game process died' }
    $hwnd = $p.MainWindowHandle
    if ([WotFeelInput]::GetForegroundWindow() -ne $hwnd) {
        [WotFeelInput]::SwitchToThisWindow($hwnd, $true)
        Start-Sleep -Milliseconds 800
        if ([WotFeelInput]::GetForegroundWindow() -ne $hwnd) { throw 'cannot focus game window' }
    }
}

function Send-Command([string]$cmd) {
    Ensure-Focus
    [WotFeelInput]::KeyPress(0x09, 40)
    Start-Sleep -Milliseconds 300
    foreach ($c in $cmd.ToCharArray()) { [WotFeelInput]::CharPress($c, 12); Start-Sleep -Milliseconds 12 }
    Start-Sleep -Milliseconds 150
    [WotFeelInput]::KeyPress(0x0D, 40)
    Start-Sleep -Milliseconds 250
}

function Count-Bmp { @(Get-ChildItem (Join-Path $sysDir 'Shot*.bmp') -ErrorAction SilentlyContinue).Count }

function Start-GameMap([string]$m) {
    Stop-Process -Name WoT -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2
    Remove-Item (Join-Path $sysDir 'Running.ini') -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $sysDir 'WoT.log') -Force -ErrorAction SilentlyContinue
    Start-Process -FilePath (Join-Path $sysDir 'WoT.exe') -ArgumentList "$m.wot" -WorkingDirectory $sysDir
    Start-Sleep -Seconds 12
    for ($probe = 0; $probe -lt 15; $probe++) {
        $n = Count-Bmp
        Ensure-Focus
        [WotFeelInput]::KeyPress(0x78, 40)
        Start-Sleep -Milliseconds 1500
        if ((Count-Bmp) -gt $n) { Remove-Item (Join-Path $sysDir 'Shot*.bmp') -Force; return }
        Start-Sleep -Milliseconds 3500
    }
    throw 'game never became interactive'
}

function Start-Recording([string]$outFile, [int]$sec) {
    $args = @('-y','-f','gdigrab','-framerate','60','-draw_mouse','0')
    if ($CaptureRegion -match '^(\d+)x(\d+)\+(\d+)\+(\d+)$') {
        $args += @('-offset_x',$Matches[3],'-offset_y',$Matches[4],'-video_size',"$($Matches[1])x$($Matches[2])")
    }
    $args += @('-i','desktop','-t',"$sec",'-c:v','libx264','-preset','veryfast','-crf','18','-pix_fmt','yuv420p',$outFile)
    return Start-Process ffmpeg -ArgumentList $args -PassThru -WindowStyle Hidden
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$VK_W = 0x57; $VK_SPACE = 0x20

Start-GameMap $Map
if ($Clean) { Send-Command 'ToggleUI'; Send-Command 'ghost'; Send-Command 'fly' }
# normal physics unless -Clean; place at pose (JumpTo works regardless)
Send-Command "JumpTo $JumpToX $JumpToY $JumpToZ"
Send-Command "set WOTPlayer ViewRotation (Pitch=$Pitch,Yaw=$Yaw,Roll=0)"
Start-Sleep -Seconds 5   # messages fade, pawn settles on floor

$stamp = "{0:yyyyMMdd_HHmm}" -f (Get-Date)
switch ($Section) {
    'run' {
        $out = Join-Path $OutDir "$($Map)_run_$stamp.mp4"
        $ff = Start-Recording $out $DurationSec
        Start-Sleep -Seconds 2
        Ensure-Focus
        [WotFeelInput]::KeyDown($VK_W); Start-Sleep -Seconds ($DurationSec - 4); [WotFeelInput]::KeyUp($VK_W)
        $ff.WaitForExit()
    }
    'jumps' {
        Send-Command 'behindview 1'
        Start-Sleep -Seconds 4
        $out = Join-Path $OutDir "$($Map)_jumps_$stamp.mp4"
        $ff = Start-Recording $out $DurationSec
        Start-Sleep -Seconds 2
        Ensure-Focus
        for ($j = 0; $j -lt 8; $j++) { [WotFeelInput]::KeyPress($VK_SPACE, 60); Start-Sleep -Milliseconds 1400 }
        $ff.WaitForExit()
    }
    'runjumps' {
        Send-Command 'behindview 1'
        Start-Sleep -Seconds 4
        $out = Join-Path $OutDir "$($Map)_runjumps_$stamp.mp4"
        $ff = Start-Recording $out $DurationSec
        Start-Sleep -Seconds 2
        Ensure-Focus
        [WotFeelInput]::KeyDown($VK_W)
        for ($j = 0; $j -lt 6; $j++) { Start-Sleep -Milliseconds 1100; [WotFeelInput]::KeyPress($VK_SPACE, 60) }
        [WotFeelInput]::KeyUp($VK_W)
        $ff.WaitForExit()
    }
    'angreal' {
        Send-Command 'AllAngreal true'   # every artifact, infinite charges
        Start-Sleep -Seconds 2
        foreach ($cls in ($AngrealClasses -split ',')) {
            $cls = $cls.Trim()
            if (-not $cls) { continue }
            Send-Command "Select $cls"
            Start-Sleep -Seconds 5   # selection anim + console messages fade
            $shortName = $cls -replace '^AngrealInv',''
            $out = Join-Path $OutDir "$($Map)_angreal_$($shortName)_$stamp.mp4"
            $ff = Start-Recording $out $DurationSec
            Start-Sleep -Seconds 2
            Ensure-Focus
            for ($shot = 0; $shot -lt 5; $shot++) { [WotFeelInput]::MouseClick(80); Start-Sleep -Milliseconds 1800 }
            $ff.WaitForExit()
            Write-Host "recorded: $out"
        }
    }
    'static' {
        $out = Join-Path $OutDir "$($Map)_$($Label)_$stamp.mp4"
        $ff = Start-Recording $out $DurationSec
        $ff.WaitForExit()
    }
}
Send-Command 'exit'
Start-Sleep -Seconds 3
Stop-Process -Name WoT -Force -ErrorAction SilentlyContinue
Write-Host "recorded: $out"
