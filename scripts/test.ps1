<#
.SYNOPSIS
    Runs the WinStellar test suite via ctest, mirroring build.ps1's env
    setup (MSVC vcvars + UNC->drive-letter mapping) so it works from a
    WSL share.

.EXAMPLE
    .\scripts\test.ps1                  # run all tests
    .\scripts\test.ps1 -Filter unit     # run only unit_*
#>

[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [string]$Filter = ''
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath)

# Same UNC -> drive-letter mapping as build.ps1: ctest spawns child test
# processes that wouldn't tolerate a UNC cwd either.
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
    & net use "${letter}:" $share /persistent:no | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "net use $share failed" }
    $mappedDrive = $letter
    $repoRoot = "${letter}:\$relative"
}

try {
    $buildDir = Join-Path $repoRoot 'build'
    if (-not (Test-Path $buildDir)) {
        throw "build/ directory missing -- run scripts\build.ps1 first"
    }

    # Source vcvars so ctest.exe (shipped with VS) is on PATH.
    $vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found at $vcvars"
    }
    # Capture env from the .bat one-shot -- same trick as build.ps1.
    cmd /c "`"$vcvars`" && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }

    Push-Location $buildDir
    try {
        $args = @('-C', $Config, '--output-on-failure', '--label-regex', 'winstellar')
        if ($Filter) { $args += @('-R', $Filter) }
        & ctest @args
        if ($LASTEXITCODE -ne 0) { throw "ctest reported failures" }
    } finally {
        Pop-Location
    }
} finally {
    if ($mappedDrive) {
        & net use "${mappedDrive}:" /delete /yes | Out-Null
    }
}
