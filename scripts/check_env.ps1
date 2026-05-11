<#
.SYNOPSIS
    Validates the local Windows toolchain required to build WinStellar.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ok = $true

function Test-Step($label, $script) {
    Write-Host "[ .. ] $label" -NoNewline
    try {
        $result = & $script
        if ($result) {
            Write-Host "`r[ OK ] $label : $result"
        } else {
            Write-Host "`r[FAIL] $label" -ForegroundColor Red
            $script:ok = $false
        }
    } catch {
        Write-Host "`r[FAIL] $label : $($_.Exception.Message)" -ForegroundColor Red
        $script:ok = $false
    }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

Test-Step "vswhere.exe" {
    if (Test-Path $vswhere) { return $vswhere } else { return $null }
}

$vsInstall = $null
Test-Step "VS install with C++ workload" {
    if (-not (Test-Path $vswhere)) { return $null }
    $script:vsInstall = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($script:vsInstall)) { return $null }
    return $script:vsInstall
}

Test-Step "MSVC cl.exe" {
    if (-not $vsInstall) { return $null }
    $cl = & $vswhere -latest -find 'VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe' | Select-Object -First 1
    if ($cl) { return $cl } else { return $null }
}

Test-Step "Windows SDK" {
    $inc = "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
    if (-not (Test-Path $inc)) { return $null }
    $sdk = Get-ChildItem $inc -Directory | Where-Object {
        Test-Path (Join-Path $_.FullName 'um\Windows.h')
    } | Select-Object -First 1
    if ($sdk) { return $sdk.Name } else { return $null }
}

Test-Step "CMake (in VS)" {
    if (-not $vsInstall) { return $null }
    $cmake = & $vswhere -latest -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
    if ($cmake) { return $cmake } else { return $null }
}

Test-Step "Ninja (in VS)" {
    if (-not $vsInstall) { return $null }
    $ninja = & $vswhere -latest -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' | Select-Object -First 1
    if ($ninja) { return $ninja } else { return $null }
}

Test-Step "VCPKG_ROOT" {
    if ([string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) { return $null }
    if (-not (Test-Path (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) { return $null }
    return $env:VCPKG_ROOT
}

Write-Host ""
if ($ok) {
    Write-Host "Environment is ready." -ForegroundColor Green
    exit 0
} else {
    Write-Host "Environment is NOT ready. Install the missing components and re-run." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Quick fixes:"
    Write-Host "  - Install VS workload 'Desktop development with C++' (~7-10 GB)"
    Write-Host "  - Clone vcpkg to C:\dev\vcpkg, run bootstrap-vcpkg.bat,"
    Write-Host "    then  setx VCPKG_ROOT C:\dev\vcpkg  and reopen the shell."
    exit 1
}
