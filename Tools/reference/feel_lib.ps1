# feel_lib.ps1 -- shared game-driving helpers (dot-source me)
. (Join-Path $PSScriptRoot '..\wot_config.ps1')
$sysDir = Join-Path (Get-WotReferenceDirectory 'original') 'System'

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
