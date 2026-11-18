param(
    [string]$Map = 'Arena_01',
    [Parameter(Mandatory = $true)]
    [string]$Out,
    # Default resolved in the body: $PSScriptRoot is empty inside param()
    # defaults under Windows PowerShell 5.1 `-File` invocation.
    [string]$Config = '',
    [string]$Executable = '',
    [string]$Project = '',
    [string]$UserDir = '',
    [string]$ProfileRoot = '',
    [string]$AnnouncementUrl = '',
    [string]$CompareTo = '',
    [string]$DiffConfig = '',
    [string]$Python = 'python',
    [switch]$Hosted,
    [int]$TimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\wot_config.ps1')
if (-not $Config) {
    $Config = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'gallery.json'
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Executable) {
    $engine = Get-WotLocalPath 'installed_engine_root_win64'
    $Executable = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
}
$Executable = (Resolve-Path $Executable).Path
if (-not $Project) {
    $Project = Join-Path $repo 'WoT.uproject'
}
$configPath = (Resolve-Path $Config).Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$outPath = (Resolve-Path $Out).Path

$arguments = @()
if (-not $Hosted) {
    $arguments += @("`"$Project`"", "/Game/WoT/Maps/$Map", '-game')
}
$arguments += @(
    '-RenderOffscreen',
    '-ForceRes', '-resx=1600', '-resy=1200', '-windowed', '-unattended', '-nosound',
    '-stdout', "-wotshot=`"$outPath`"", "-wotgallery=`"$configPath`""
)
if ($Hosted) {
    if (-not $UserDir -or -not $ProfileRoot) {
        throw '-Hosted requires -UserDir and -ProfileRoot for an isolated packaged profile'
    }
    $userDirPath = (Resolve-Path $UserDir).Path
    $profileRootPath = (Resolve-Path $ProfileRoot).Path
    $localAppDataPath = Join-Path $profileRootPath 'AppData\Local'
    $appDataPath = Join-Path $profileRootPath 'AppData\Roaming'
    New-Item -ItemType Directory -Force -Path $localAppDataPath, $appDataPath | Out-Null
    $arguments += @(
        '-nosteam',
        "-UserDir=`"$userDirPath`"",
        "-abslog=`"$(Join-Path $outPath 'gallery.log')`"",
        "-wotgallerymap=$Map"
    )
}

$startInfo = New-Object Diagnostics.ProcessStartInfo
$startInfo.FileName = $Executable
$startInfo.WorkingDirectory = Split-Path -Parent $Executable
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.Arguments = $arguments -join ' '
if ($Hosted) {
    $startInfo.EnvironmentVariables['USERPROFILE'] = $profileRootPath
    $startInfo.EnvironmentVariables['LOCALAPPDATA'] = $localAppDataPath
    $startInfo.EnvironmentVariables['APPDATA'] = $appDataPath
    if ($AnnouncementUrl) {
        $startInfo.EnvironmentVariables['WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL'] = $AnnouncementUrl
    }
}
$process = [Diagnostics.Process]::Start($startInfo)
Write-Host "gallery process started: pid=$($process.Id) executable=$Executable hosted=$Hosted"
$process.WaitForExit($TimeoutSeconds * 1000) | Out-Null
$process.Refresh()
if (-not $process.HasExited) {
    # Only terminate the exact process object started above. Never match by image name.
    Stop-Process -Id $process.Id -Force
    Write-Error "$Map gallery timed out"
    exit 1
}
if ($process.ExitCode -ne 0) {
    Write-Error "$Map gallery failed with exit code $($process.ExitCode)"
    exit $process.ExitCode
}
Write-Host "$Map gallery complete: $outPath"
if ($CompareTo) {
    $comparePath = (Resolve-Path $CompareTo).Path
    if (-not $DiffConfig) {
        $DiffConfig = $configPath
    }
    $diffConfigPath = (Resolve-Path $DiffConfig).Path
    $diffTool = Join-Path $PSScriptRoot 'gallery_diff.py'
    & $Python $diffTool $comparePath $outPath --config $diffConfigPath
    if ($LASTEXITCODE -ne 0) {
        Write-Error "$Map gallery comparison gate failed"
        exit $LASTEXITCODE
    }
}
