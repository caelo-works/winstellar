<#
.SYNOPSIS
    Full dev cycle: unregister, rebuild, re-register, restart Explorer.
    Use this after any change to shell_ext source.

.PARAMETER Config
    Build configuration (Debug/Release).
#>

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

Write-Host "==> Unregister" -ForegroundColor Cyan
& "$here\unregister.ps1" -Config $Config

Write-Host "==> Build" -ForegroundColor Cyan
& "$here\build.ps1" -Config $Config

Write-Host "==> Register" -ForegroundColor Cyan
& "$here\register.ps1" -Config $Config

Write-Host "Dev cycle complete." -ForegroundColor Green
