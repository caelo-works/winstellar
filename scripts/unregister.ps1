<#
.SYNOPSIS
    Unregisters WinStellarShellExt.dll from Windows Explorer and removes the
    %LOCALAPPDATA%\WinStellar install directory.
#>

[CmdletBinding()]
param()

trap {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host ($_ | Out-String) -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}
$ErrorActionPreference = 'Stop'

$installDir = Join-Path $env:LOCALAPPDATA 'WinStellar'
$dll = Join-Path $installDir 'WinStellarShellExt.dll'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw "Not running as administrator. Run scripts\unregister.cmd which self-elevates."
}

if (Test-Path $dll) {
    Write-Host "Unregistering $dll"
    $proc = Start-Process -FilePath 'regsvr32.exe' -ArgumentList @('/s', '/u', $dll) `
                          -Wait -PassThru -NoNewWindow
    if ($proc.ExitCode -ne 0) {
        Write-Warning "regsvr32 /u returned $($proc.ExitCode) (this is sometimes harmless)."
    }
    # Just bounce explorer; WSearch will release the handle on its own once
    # the registry key is gone.
    Get-Process -Name explorer -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
} else {
    Write-Warning "Install DLL not found at $dll. Continuing with cleanup."
}

Write-Host "Restarting explorer.exe"
Start-Process explorer.exe

if (Test-Path $installDir) {
    Write-Host "Removing $installDir"
    Remove-Item -Recurse -Force $installDir -ErrorAction SilentlyContinue
}

$wsearch = Get-Service -Name WSearch -ErrorAction SilentlyContinue
if ($wsearch -and $wsearch.Status -ne 'Running') {
    Write-Host "Restarting Windows Search service"
    Start-Service -Name WSearch -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Read-Host "Press Enter to close"
