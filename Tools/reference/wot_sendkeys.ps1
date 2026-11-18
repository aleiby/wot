# wotkeys.ps1 -- send keystrokes / console commands to the running WoT game.
# Usage:
#   wotkeys.ps1 -Key F9                      # press a single key
#   wotkeys.ps1 -Command "ghost"             # Tab, type command, Enter
#   wotkeys.ps1 -CommandList "ghost;fly"     # multiple commands, ; separated
param(
    [string]$Key,
    [string]$Command,
    [string]$CommandList,
    [int]$InterKeyMs = 30
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class WotInput
{
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr hWnd, bool fAltTab);
    [DllImport("user32.dll")] public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT
    {
        public uint type;
        public InputUnion U;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion
    {
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public MOUSEINPUT mi;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT
    {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT
    {
        public int dx; public int dy; public uint mouseData;
        public uint dwFlags; public uint time; public IntPtr dwExtraInfo;
    }

    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const uint KEYEVENTF_UNICODE = 0x0004;

    public static void KeyPress(ushort vk, int holdMs)
    {
        INPUT[] down = new INPUT[1];
        down[0].type = 1; // INPUT_KEYBOARD
        down[0].U.ki.wVk = vk;
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(holdMs);
        INPUT[] up = new INPUT[1];
        up[0].type = 1;
        up[0].U.ki.wVk = vk;
        up[0].U.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void CharPress(char c, int holdMs)
    {
        INPUT[] down = new INPUT[1];
        down[0].type = 1;
        down[0].U.ki.wScan = (ushort)c;
        down[0].U.ki.dwFlags = KEYEVENTF_UNICODE;
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(holdMs);
        INPUT[] up = new INPUT[1];
        up[0].type = 1;
        up[0].U.ki.wScan = (ushort)c;
        up[0].U.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

$vkMap = @{
    'F9' = 0x78; 'F10' = 0x79; 'F11' = 0x7A; 'F12' = 0x7B;
    'TAB' = 0x09; 'ENTER' = 0x0D; 'ESC' = 0x1B; 'SPACE' = 0x20;
    'UP' = 0x26; 'DOWN' = 0x28; 'LEFT' = 0x25; 'RIGHT' = 0x27;
}

$proc = Get-Process WoT -ErrorAction SilentlyContinue
if (-not $proc) { Write-Output 'ERROR: WoT.exe not running'; exit 1 }
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'ERROR: no main window'; exit 1 }

$fg = [WotInput]::GetForegroundWindow()
if ($fg -ne $hwnd) {
    [WotInput]::SwitchToThisWindow($hwnd, $true)
    Start-Sleep -Milliseconds 700
    $fg = [WotInput]::GetForegroundWindow()
    if ($fg -ne $hwnd) { Write-Output 'ERROR: could not focus game window'; exit 1 }
    Write-Output 'refocused game window'
}

function Send-GameKey([string]$name) {
    $vk = $vkMap[$name.ToUpper()]
    if (-not $vk) { throw "unknown key: $name" }
    [WotInput]::KeyPress([uint16]$vk, 40)
    Start-Sleep -Milliseconds $InterKeyMs
}

function Send-GameCommand([string]$cmd) {
    Send-GameKey 'TAB'
    Start-Sleep -Milliseconds 200
    foreach ($c in $cmd.ToCharArray()) {
        [WotInput]::CharPress($c, 15)
        Start-Sleep -Milliseconds 15
    }
    Start-Sleep -Milliseconds 100
    Send-GameKey 'ENTER'
    Start-Sleep -Milliseconds 200
    Write-Output "sent command: $cmd"
}

if ($Key) { Send-GameKey $Key; Write-Output "sent key: $Key" }
if ($Command) { Send-GameCommand $Command }
if ($CommandList) {
    foreach ($cmd in $CommandList -split ';') {
        if ($cmd.Trim()) { Send-GameCommand $cmd.Trim() }
    }
}
