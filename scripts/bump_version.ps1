<#
.SYNOPSIS
    Bumps the project version (single source of truth: CMakeLists.txt).
    Everything else (binary VERSIONINFO, installer filename, Apps & Features,
    DLL FileVersion) follows from that.

.EXAMPLE
    .\scripts\bump_version.ps1 -Patch          # 0.1.1 -> 0.1.2
    .\scripts\bump_version.ps1 -Minor          # 0.1.x -> 0.2.0
    .\scripts\bump_version.ps1 -Major          # x.y.z -> (x+1).0.0
    .\scripts\bump_version.ps1 -Set 1.0.0      # exact version
#>

[CmdletBinding(DefaultParameterSetName = 'Patch')]
param(
    [Parameter(ParameterSetName = 'Patch')]
    [switch]$Patch,
    [Parameter(ParameterSetName = 'Minor')]
    [switch]$Minor,
    [Parameter(ParameterSetName = 'Major')]
    [switch]$Major,
    [Parameter(ParameterSetName = 'Set', Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Set
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath)
$cmake = Join-Path $repoRoot 'CMakeLists.txt'
$text = Get-Content $cmake -Raw
if ($text -notmatch '(project\([^)]*VERSION\s+)(\d+)\.(\d+)\.(\d+)') {
    throw "Could not find VERSION in $cmake"
}
$prefix = $Matches[1]
[int]$maj = $Matches[2]; [int]$min = $Matches[3]; [int]$pat = $Matches[4]
$current = "$maj.$min.$pat"

switch ($PSCmdlet.ParameterSetName) {
    'Patch' { $pat++ }
    'Minor' { $min++; $pat = 0 }
    'Major' { $maj++; $min = 0; $pat = 0 }
    'Set'   {
        $parts = $Set -split '\.'
        $maj = [int]$parts[0]; $min = [int]$parts[1]; $pat = [int]$parts[2]
    }
}
$next = "$maj.$min.$pat"

$newText = [regex]::Replace($text,
    'project\(([^)]*VERSION\s+)\d+\.\d+\.\d+',
    "project(`$1$next")
Set-Content -Path $cmake -Value $newText -NoNewline

# Also sync vcpkg.json's version-string so the manifest stays consistent.
$vcpkg = Join-Path $repoRoot 'vcpkg.json'
if (Test-Path $vcpkg) {
    $vtext = Get-Content $vcpkg -Raw
    $vnew  = [regex]::Replace($vtext,
        '("version-string"\s*:\s*")\d+\.\d+\.\d+(")',
        "`${1}$next`${2}")
    if ($vnew -ne $vtext) {
        Set-Content -Path $vcpkg -Value $vnew -NoNewline
        Write-Host "Synced vcpkg.json"
    }
}

Write-Host "Version: $current -> $next" -ForegroundColor Green
Write-Host "Updated $cmake"
Write-Host ""
Write-Host "Next:"
Write-Host "  .\scripts\release.cmd               # commit + tag + build + installer"
Write-Host "  .\installer\build_installer.cmd     # build installer only (no commit/tag)"
