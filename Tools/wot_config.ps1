# PowerShell bridge to the stdlib-only shared configuration helper.
function Invoke-WotConfig([string[]]$Arguments) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
    if (-not $python) { $python = Get-Command python -ErrorAction SilentlyContinue }
    if (-not $python) { throw 'python3/python is required to read Tools/local.json' }
    $helper = Join-Path $PSScriptRoot 'wot_config.py'
    $value = & $python.Source $helper @Arguments
    if ($LASTEXITCODE -ne 0) { throw "wot_config.py failed for: $Arguments" }
    return $value.Trim()
}

function Get-WotLocalPath([string]$Key) {
    return Invoke-WotConfig @('get', $Key)
}

function Get-WotReferenceDirectory([ValidateSet('original','devtree','exports')][string]$Name) {
    return Invoke-WotConfig @("$Name-dir")
}
