param(
    [ValidateSet('check','desktop','native','config','ida')][string]$Action = 'check',
    [switch]$NoPause
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exitCode = 1
try {
    $python = ''; $prefix = @()
    foreach ($name in @('python','py')) {
        $found = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if (!$found) { continue }
        $candidatePrefix = @(); if ($name -eq 'py') { $candidatePrefix = @('-3') }
        & $found.Source @candidatePrefix -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>$null
        if ($LASTEXITCODE -eq 0) { $python = $found.Source; $prefix = $candidatePrefix; break }
    }
    if (!$python) { throw 'Python 3.10+ not found. Install Python 3.11/3.12 x64 from https://www.python.org/downloads/windows/ and enable Add Python to PATH. Then reopen this launcher.' }
    Push-Location -LiteralPath $root
    try {
        switch ($Action) {
            'check'   { & $python @prefix (Join-Path $root 'desktop/doctor.py') }
            'desktop' { & $python @prefix (Join-Path $root 'desktop/launcher.py') }
            'native'  {
                $info = Get-Content -LiteralPath (Join-Path $root 'RELEASE-INFO.json') -Raw | ConvertFrom-Json
                if (!$info.nativeUi) { throw 'This is the headless package. Use 02-START-DESKTOP.cmd, or obtain the full native package for the phone overlay.' }
                & $python @prefix (Join-Path $root 'desktop/launcher.py') --native
            }
            'config'  { & $python @prefix (Join-Path $root 'bridge/generate_config.py') }
            'ida'     { & $python @prefix (Join-Path $root 'desktop/doctor.py') --ida }
        }
        $exitCode = $LASTEXITCODE
    } finally { Pop-Location }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host 'Read 00-START-HERE.html. No dependencies are downloaded or installed automatically.'
    $exitCode = 1
}
if (!$NoPause -and ($Action -ne 'desktop' -or $exitCode -ne 0)) { Read-Host 'Press Enter to close' | Out-Null }
exit $exitCode
