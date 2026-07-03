<#
.SYNOPSIS
    Stages WinStellarShellExt + dependencies into %ProgramFiles%\WinStellar and
    registers the shell extension with Explorer.

    NOTE: a system-wide (HKLM) COM registration must point at a directory that
    ordinary users cannot write to, otherwise anyone could swap the DLL and have
    Explorer load their code. %ProgramFiles% is admin-only (and matches where the
    real installer puts it); %LOCALAPPDATA% -- which this used to use -- is
    user-writable and was a local privilege-escalation vector.

.PARAMETER Config
    Build configuration whose binaries to register. Default: Release.

.NOTES
    Requires elevation (admin) for regsvr32 + HKLM writes; auto-relaunches.
    Uses progressive escalation when copying files: tries plain copy first,
    then kills only specific handler hosts, and only stops Windows Search as
    a last resort. (Aggressive kills + WSearch restarts have been observed to
    momentarily disrupt the WSL2 9p interop on this machine.)
#>

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',

    # Optional repo root override, used when the elevated relaunch can't
    # resolve $PSScriptRoot against the original UNC path.
    [string]$RepoRoot = ''
)

# Try several candidate log paths; whichever one we can actually write to wins.
# Order: user profile (always writable), then Public (in case profile is weird),
# then TEMP. Wrapped in try/catch because $ErrorActionPreference='Stop' is set
# AFTER and we don't want a missing path to silently kill the script.
$logCandidates = @(
    (Join-Path $env:USERPROFILE 'winstellar_register.log'),
    'C:\Users\Public\winstellar_register.log',
    (Join-Path $env:TEMP 'winstellar_register.log')
)
$logPath = $null
foreach ($p in $logCandidates) {
    try {
        New-Item -ItemType File -Path $p -Force -ErrorAction Stop | Out-Null
        $logPath = $p
        break
    } catch { }
}
if (-not $logPath) {
    Write-Host "WARNING: could not open any log file; tracing to console only" -ForegroundColor Yellow
}

function Log($msg) {
    Write-Host $msg
    if ($logPath) {
        try { $msg | Out-File -FilePath $logPath -Append -Encoding UTF8 } catch { }
    }
}
function LogError($msg) {
    Write-Host $msg -ForegroundColor Red
    if ($logPath) {
        try { $msg | Out-File -FilePath $logPath -Append -Encoding UTF8 } catch { }
    }
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Log "=== register.ps1 started $(Get-Date) | admin=$isAdmin ==="
Log "Log path: $logPath"

$ErrorActionPreference = 'Stop'

try {

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $repoRoot = [System.IO.Path]::GetFullPath(
        (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath)
} else {
    $repoRoot = $RepoRoot
}
$srcDir = Join-Path $repoRoot "build\bin\$Config"
Log "Repo root: $repoRoot"
Log "Source dir: $srcDir"

$dllSrc = Join-Path $srcDir 'WinStellarShellExt.dll'
Log "DLL source: $dllSrc"
Log "DLL source exists: $(Test-Path $dllSrc)"
if (-not (Test-Path $dllSrc)) {
    throw "DLL not found: $dllSrc. Run scripts\build.cmd -Config $Config first."
}

# Protected, admin-only location (see .SYNOPSIS): a user-writable install dir
# under a system-wide COM registration is a DLL-hijack / privilege-escalation
# vector. Requires the elevation this script already demands.
$installDir = Join-Path $env:ProgramFiles 'WinStellar'
$dllDst = Join-Path $installDir 'WinStellarShellExt.dll'
Log "Install dir: $installDir"

if (-not $isAdmin) {
    throw "Not running as administrator. Run scripts\register.cmd which self-elevates."
}

function Stop-Targets($names) {
    foreach ($n in $names) {
        Get-Process -Name $n -ErrorAction SilentlyContinue |
            Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

# Three escalation tiers. We start with the smallest hammer and only reach for
# the bigger one if the previous attempt still finds a file lock.
$tiers = @(
    @{ Name = 'soft (just retry)';                action = $null },
    @{ Name = 'kill explorer + prevhost';         action = { Stop-Targets @('explorer','prevhost') } },
    @{ Name = 'stop WSearch service';             action = {
        Stop-Targets @('explorer','prevhost')
        $w = Get-Service -Name WSearch -ErrorAction SilentlyContinue
        if ($w -and $w.Status -eq 'Running') {
            Write-Host "  stopping WSearch service"
            Stop-Service -Name WSearch -ErrorAction SilentlyContinue
        }
    } }
)

function Try-Copy-Files($files, $dest) {
    foreach ($f in $files) {
        Copy-Item -Path $f.FullName -Destination $dest -Force -ErrorAction Stop
    }
}

# Unregister the old DLL (if any) before replacing it.
if (Test-Path $dllDst) {
    Write-Host "Unregistering previous version at $dllDst"
    Start-Process -FilePath 'regsvr32.exe' -ArgumentList @('/s', '/u', $dllDst) `
                  -Wait -NoNewWindow | Out-Null
}

Write-Host "Staging binaries in $installDir"
New-Item -ItemType Directory -Force -Path $installDir | Out-Null

$patterns = @('WinStellarShellExt.dll', '*.dll', 'FitsProps.propdesc', 'WinStellar.exe')
$files = @()
foreach ($pat in $patterns) {
    Get-ChildItem -Path $srcDir -Filter $pat -ErrorAction SilentlyContinue | ForEach-Object {
        $files += $_
    }
}

$copied = $false
for ($i = 0; $i -lt $tiers.Count -and -not $copied; $i++) {
    $tier = $tiers[$i]
    if ($tier.action) {
        Write-Host "  copy lock - escalating: $($tier.Name)"
        & $tier.action
        Start-Sleep -Milliseconds 700
    }
    try {
        Try-Copy-Files $files $installDir
        $copied = $true
    } catch {
        if ($i -eq $tiers.Count - 1) { throw }
    }
}

Write-Host "Registering $dllDst"
$proc = Start-Process -FilePath 'regsvr32.exe' -ArgumentList @($dllDst) `
                       -Wait -PassThru -NoNewWindow
$rc = $proc.ExitCode
Write-Host "regsvr32 exit code: $rc"
if ($rc -ne 0) {
    Write-Host ""
    Write-Host "regsvr32 failed. Common reasons:" -ForegroundColor Yellow
    Write-Host "  3 = LoadLibrary failed (missing dependency, or DLL not 64-bit)"
    Write-Host "  4 = DllRegisterServer export not found"
    Write-Host "  5 = DllRegisterServer returned an error"
    throw "regsvr32 failed (exit code $rc)"
}

# Restart only what we actually stopped.
Get-Process -Name explorer -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Process explorer.exe

$wsearch = Get-Service -Name WSearch -ErrorAction SilentlyContinue
if ($wsearch -and $wsearch.Status -ne 'Running') {
    Write-Host "Restarting Windows Search service"
    Start-Service -Name WSearch -ErrorAction SilentlyContinue
}

Log ""
Log "Done."
Log "Installed at: $installDir"
Log "To uninstall: scripts\unregister.cmd"

} catch {
    LogError ""
    LogError "ERROR: $($_.Exception.Message)"
    LogError ($_ | Out-String)
    LogError ($_.ScriptStackTrace | Out-String)
} finally {
    Write-Host ""
    if ($logPath) {
        Write-Host "Log saved to: $logPath" -ForegroundColor Cyan
    } else {
        Write-Host "(log file not available)" -ForegroundColor Yellow
    }
    Read-Host "Press Enter to close"
}
