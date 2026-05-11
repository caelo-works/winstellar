<#
.SYNOPSIS
    Configures and builds WinStellar with MSVC + vcpkg.

.PARAMETER Config
    Debug or Release. Defaults to Release.

.PARAMETER Clean
    Wipe build/ before configuring.
#>

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

# Canonicalize: use ProviderPath (raw filesystem path, no PSProvider prefix) and
# fully resolve '..' so UNC-detection works reliably.
$repoRoot = [System.IO.Path]::GetFullPath(
    (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath)
Write-Host "Repo root: $repoRoot"

# MSVC cl.exe / link.exe do not accept UNC paths as cwd. If the project lives
# on a WSL share (\\wsl.localhost\...), map it to a free drive letter for the
# duration of the build, then unmap on exit.
$mappedDrive = $null
if ($repoRoot.StartsWith('\\')) {
    if ($repoRoot -notmatch '^(\\\\[^\\]+\\[^\\]+)\\(.*)$') {
        throw "Could not parse UNC path: $repoRoot"
    }
    $share = $Matches[1]
    $relative = $Matches[2]

    $used = (Get-PSDrive -PSProvider FileSystem).Name
    $letter = ('Z','Y','X','W','V','U','T','S','R','Q') |
        Where-Object { $used -notcontains $_ } |
        Select-Object -First 1
    if (-not $letter) { throw "No free drive letter to map $share" }

    Write-Host "Mapping $share -> ${letter}:"
    & net use "${letter}:" $share /persistent:no | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "net use $share failed" }
    $mappedDrive = $letter
    $repoRoot = "${letter}:\$relative"
}

Set-Location $repoRoot

try {

if ($Clean -and (Test-Path 'build')) {
    Write-Host "Cleaning build/"
    Remove-Item -Recurse -Force 'build'
}

# Resolve VCPKG_ROOT: session env var, then user-scope registry.
# We re-resolve AFTER vcvars too because vcvars64.bat sets VCPKG_ROOT to the
# VS-bundled vcpkg, which is missing common ports - the user's vcpkg must win.
$userVcpkgRoot = $env:VCPKG_ROOT
if ([string]::IsNullOrWhiteSpace($userVcpkgRoot)) {
    $userVcpkgRoot = [Environment]::GetEnvironmentVariable('VCPKG_ROOT','User')
}
if ([string]::IsNullOrWhiteSpace($userVcpkgRoot) -or
    -not (Test-Path (Join-Path $userVcpkgRoot 'vcpkg.exe'))) {
    throw "VCPKG_ROOT is not set or invalid. Run scripts/check_env.ps1 to diagnose."
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($vsInstall)) {
    throw "Visual Studio with C++ tools not found. Run scripts/check_env.ps1 to diagnose."
}
$vcvars = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

Write-Host "Sourcing $vcvars"
# pushd into a local dir first so cmd doesn't complain about UNC cwd.
& cmd /c "pushd %TEMP% && `"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^(?<name>[^=]+)=(?<value>.*)$') {
        Set-Item -Force -Path "env:$($Matches.name)" -Value $Matches.value
    }
}

# Restore the user's VCPKG_ROOT (vcvars overwrote it with the VS-bundled one).
$env:VCPKG_ROOT = $userVcpkgRoot

# Ensure Ninja is reachable. The VS-bundled Ninja sits next to CMake but is not
# on PATH after vcvars; locate it explicitly and prepend.
$ninjaDir = & $vswhere -latest -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' |
    Select-Object -First 1 |
    Split-Path -Parent
if ($ninjaDir -and (Test-Path (Join-Path $ninjaDir 'ninja.exe'))) {
    $env:PATH = "$ninjaDir;$env:PATH"
}

Write-Host "Configuring (preset x64-windows-msvc)"
& cmake --preset x64-windows-msvc
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "Building $Config"
& cmake --build build --config $Config
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$binDir = Join-Path $repoRoot "build\bin\$Config"
Write-Host ""
Write-Host "Build outputs in $binDir :" -ForegroundColor Green
Get-ChildItem $binDir -ErrorAction SilentlyContinue | Where-Object {
    $_.Extension -in '.dll', '.exe', '.propdesc'
} | ForEach-Object { Write-Host "  $($_.Name)" }

} finally {
    if ($mappedDrive) {
        Set-Location $env:TEMP
        Write-Host "Unmapping ${mappedDrive}:"
        & net use "${mappedDrive}:" /delete /yes 2>&1 | Out-Null
    }
}
