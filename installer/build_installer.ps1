<#
.SYNOPSIS
    End-to-end installer build: compiles the project (Release) and packages
    everything into a single WinStellarSetup-<version>.exe via Inno Setup.

    Runs from any cwd as long as the script lives at <repo>\installer\.
#>

[CmdletBinding()]
param(
    [switch]$SkipBuild,

    # SHA1 thumbprint of a code-signing certificate from the Windows
    # Personal cert store (Cert:\CurrentUser\My or Cert:\LocalMachine\My).
    # When provided, every binary in the installer AND the final setup.exe
    # are signed + timestamped. Without it the installer is unsigned and
    # SmartScreen will warn the user on first run.
    [string]$SigningCert = "",

    # RFC-3161 timestamp authority. Free public TSAs: DigiCert, Sectigo.
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = 'Stop'

# Resolve repo paths (use ProviderPath to escape PowerShell's
# Microsoft.PowerShell.Core\FileSystem:: prefix on UNC paths).
$repoRoot = [System.IO.Path]::GetFullPath(
    (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath)
$buildBin   = Join-Path $repoRoot 'build\bin\Release'
$outDir     = Join-Path $repoRoot 'build\installer'

# Single source of truth: the project VERSION line in the top-level CMakeLists.txt.
$cmakeText = Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\([^)]*VERSION\s+(\d+\.\d+\.\d+)') {
    throw "Could not parse VERSION from CMakeLists.txt"
}
$version = $Matches[1]
Write-Host "Project version: $version"

# Find Inno Setup compiler (winget puts it under per-user Programs).
$isccCandidates = @(
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
$iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    throw @"
Inno Setup 6 not found. Install it via:
    winget install --id JRSoftware.InnoSetup
or download from https://jrsoftware.org/isdl.php
"@
}
Write-Host "ISCC: $iscc"

# Build (Release) unless skipped.
if (-not $SkipBuild) {
    & (Join-Path $repoRoot 'scripts\build.ps1') -Config Release
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}

# Sanity-check expected outputs are present.
$required = @(
    'WinStellar.exe', 'WinStellarShellExt.dll', 'FitsProps.propdesc',
    'cfitsio.dll', 'sqlite3.dll', 'pugixml.dll', 'z.dll'
)
foreach ($f in $required) {
    $p = Join-Path $buildBin $f
    if (-not (Test-Path $p)) { throw "Missing build artifact: $p" }
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$iss = Join-Path $PSScriptRoot 'setup.iss'
Write-Host "Compiling installer from $iss"

# We invoke ISCC through a generated batch file to dodge PowerShell's habit
# of mangling external-command arguments that contain spaces and quotes
# (the /Ssigntool=... value is a multi-word command line).
$bat = New-TemporaryFile
$bat = Rename-Item -Path $bat -NewName ($bat.BaseName + '.bat') -PassThru
$lines = @('@echo off', 'setlocal')

if ($SigningCert) {
    $signtool = Get-ChildItem `
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" `
        -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signtool) {
        throw "signtool.exe not found - install the Windows 10/11 SDK."
    }
    Write-Host "Signing with certificate $SigningCert"
    Write-Host "  signtool: $($signtool.FullName)"
    Write-Host "  timestamp: $TimestampUrl"

    # Inside a .bat file, double-quotes around the ISCC arg are LITERAL
    # quotes when ISCC parses the command line. Inside the /Ssigntool= value
    # we need quotes around the signtool path (it has spaces). Escape inner
    # quotes by doubling them (cmd convention).
    $signCmd = '""' + $signtool.FullName + '"" sign /fd SHA256 /td SHA256 ' +
               "/tr $TimestampUrl /sha1 $SigningCert `$f"
    $lines += "`"$iscc`" /DMyAppVersion=$version /DSign=1 `"/Ssigntool=$signCmd`" `"$iss`""
} else {
    $lines += "`"$iscc`" /DMyAppVersion=$version `"$iss`""
}
$lines += 'exit /b %errorlevel%'
[System.IO.File]::WriteAllLines($bat.FullName, $lines, [System.Text.Encoding]::ASCII)
& cmd.exe /c $bat.FullName
$rc = $LASTEXITCODE
Remove-Item $bat.FullName -Force -ErrorAction SilentlyContinue
if ($rc -ne 0) { throw "Inno Setup compilation failed (exit $rc)" }

Write-Host ""
Write-Host "Installer built:" -ForegroundColor Green
Get-ChildItem $outDir -Filter '*.exe' | Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 | ForEach-Object {
        Write-Host "  $($_.FullName)"
        Write-Host "  size: $([math]::Round($_.Length / 1MB, 1)) MB"
    }
