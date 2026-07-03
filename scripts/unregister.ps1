<#
.SYNOPSIS
    Unregisters WinStellarShellExt.dll from Windows Explorer and removes the
    %ProgramFiles%\WinStellar install directory (plus any legacy
    %LOCALAPPDATA%\WinStellar left by older register.ps1 versions).
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

$installDir = Join-Path $env:ProgramFiles 'WinStellar'
$dll = Join-Path $installDir 'WinStellarShellExt.dll'
# Older register.ps1 staged into user-writable LOCALAPPDATA; clean it up too.
$legacyDir = Join-Path $env:LOCALAPPDATA 'WinStellar'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw "Not running as administrator. Run scripts\unregister.cmd which self-elevates."
}

# Unregister whichever copy is present -- regsvr32 /u calls DllUnregisterServer,
# which removes the (path-independent) CLSID keys either way.
$legacyDll = Join-Path $legacyDir 'WinStellarShellExt.dll'
$unregDll = if (Test-Path $dll) { $dll } elseif (Test-Path $legacyDll) { $legacyDll } else { $null }
if ($unregDll) {
    Write-Host "Unregistering $unregDll"
    $proc = Start-Process -FilePath 'regsvr32.exe' -ArgumentList @('/s', '/u', $unregDll) `
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
    Write-Warning "Install DLL not found at $dll or $legacyDll. Continuing with cleanup."
}

Write-Host "Restarting explorer.exe"
Start-Process explorer.exe

foreach ($dir in @($installDir, $legacyDir)) {
    if (Test-Path $dir) {
        Write-Host "Removing $dir"
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    }
}

$wsearch = Get-Service -Name WSearch -ErrorAction SilentlyContinue
if ($wsearch -and $wsearch.Status -ne 'Running') {
    Write-Host "Restarting Windows Search service"
    Start-Service -Name WSearch -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Read-Host "Press Enter to close"
