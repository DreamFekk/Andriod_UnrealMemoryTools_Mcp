param(
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [string]$BuildDir = '',
    [switch]$Headless,
    [ValidateRange(1,128)][int]$Jobs = 8,
    [switch]$SkipPackage,
    [switch]$SourcePackage,
    [string]$ReleaseDir = '',
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?$')][string]$Version = '1.0.1',
    [string]$Python = ''
)
$ErrorActionPreference = 'Stop'
$pythonCommand = @()
if (!$SkipPackage) {
    foreach ($candidate in @($Python, 'python', 'py')) {
        if (!$candidate) { continue }
        $resolved = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
        if (!$resolved) { continue }
        $prefix = @(); if ($resolved.Name -match '^py(\.exe)?$') { $prefix = @('-3') }
        & $resolved.Source @prefix -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>$null
        if ($LASTEXITCODE -eq 0) { $pythonCommand = @($resolved.Source) + $prefix; break }
    }
    if (!$pythonCommand.Count) { throw 'Release packaging requires Python 3.10+. Install Python, pass -Python <path>, or use -SkipPackage.' }
}
if (!$Ndk) { $Ndk = $env:ANDROID_NDK_ROOT }
if (!$Ndk -or !(Test-Path -LiteralPath "$Ndk/build/cmake/android.toolchain.cmake")) { throw 'Set ANDROID_NDK_HOME or pass -Ndk <Android NDK path>.' }
if (!$BuildDir) { $BuildDir = Join-Path $PSScriptRoot 'build' }
$native = if ($Headless) {'OFF'} else {'ON'}
& cmake -S $PSScriptRoot -B $BuildDir -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$Ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release "-DUMT_NATIVE_UI=$native"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $BuildDir --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
Write-Host "Built: $PSScriptRoot\outputs\arm64-v8a\UnrealMemoryTools"
if (!$SkipPackage) {
    if (!$ReleaseDir) { $ReleaseDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'releases' }
    if (![System.IO.Path]::IsPathRooted($ReleaseDir)) { $ReleaseDir = Join-Path $PSScriptRoot $ReleaseDir }
    $variant = if ($Headless) { 'headless' } else { 'native' }
    $pythonExe = $pythonCommand[0]
    $prefix = @(); if ($pythonCommand.Count -gt 1) { $prefix = $pythonCommand[1..($pythonCommand.Count - 1)] }
    $packageScript = Join-Path $PSScriptRoot 'scripts/package_release.py'
    & $pythonExe @prefix $packageScript --kind user --variant $variant --version $Version --release-dir $ReleaseDir
    if ($LASTEXITCODE -ne 0) { throw 'Build succeeded, but user release packaging failed.' }
    if ($SourcePackage) {
        & $pythonExe @prefix $packageScript --kind source --variant $variant --version $Version --release-dir $ReleaseDir
        if ($LASTEXITCODE -ne 0) { throw 'Build succeeded, but source release packaging failed.' }
    }
    Write-Host "Release ready: $ReleaseDir" -ForegroundColor Green
}
