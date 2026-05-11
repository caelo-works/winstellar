<#
.SYNOPSIS
    End-to-end release pipeline: bump version, commit, tag, build, package.

    Runs (in order):
      1. Verifies the working tree is clean (refuses to release dirty)
      2. Bumps version per the chosen flag (Patch / Minor / Major / Set)
         - Updates CMakeLists.txt project(VERSION) and vcpkg.json version-string
      3. git commit -am "Release v<version>"
      4. git tag -a v<version> -m "Release v<version>"
      5. Builds the project (Release config)
      6. Builds the installer (signed if -SigningCert given)
      7. Optionally pushes commit + tag to origin (-Push)

.PARAMETER Patch / Minor / Major / Set
    Version bump mode. Exactly one is required.

.PARAMETER SigningCert
    SHA-1 thumbprint of a code-signing certificate. Forwarded to
    installer/build_installer.ps1. Without it the produced installer is
    unsigned.

.PARAMETER TimestampUrl
    RFC-3161 TSA URL. Default: DigiCert.

.PARAMETER Push
    After tagging, run `git push` and `git push --tags` to publish.

.PARAMETER AllowDirty
    Skip the clean-tree check. Useful only when re-running after a partial
    failure; not recommended for normal releases.

.EXAMPLE
    .\scripts\release.ps1 -Patch
    .\scripts\release.ps1 -Set 0.2.0 -SigningCert 118E76043A...
    .\scripts\release.ps1 -Minor -Push
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
    [string]$Set,

    [string]$SigningCert  = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [switch]$Push,
    [switch]$AllowDirty
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath)

function Step($n, $msg) {
    Write-Host ""
    Write-Host "==[$n]== $msg" -ForegroundColor Cyan
}

# --- 0. Pre-flight ---------------------------------------------------------
Step 0 "Pre-flight checks"
Push-Location $repoRoot
try {
    # We assume the repo is a git checkout. If it's not, bail loud.
    & git rev-parse --is-inside-work-tree *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "Not a git repository: $repoRoot. Initialize git first or run installer/build_installer.cmd directly."
    }

    if (-not $AllowDirty) {
        $dirty = & git status --porcelain
        if ($dirty) {
            Write-Host $dirty
            throw "Working tree has uncommitted changes. Commit/stash first or pass -AllowDirty."
        }
    }

    # --- 1. Bump --------------------------------------------------------------
    Step 1 "Bumping version"
    # Explicit per-arm dispatch -- PS5's splat handling of `& path @args` is
    # finicky on this UNC repo layout (last release attempt failed here with
    # a misleading "parameter -Set not recognized" against release.ps1).
    $bumper = Join-Path $repoRoot 'scripts\bump_version.ps1'
    switch ($PSCmdlet.ParameterSetName) {
        'Patch' { & $bumper -Patch }
        'Minor' { & $bumper -Minor }
        'Major' { & $bumper -Major }
        'Set'   { & $bumper -Set $Set }
    }
    if ($LASTEXITCODE -ne 0) { throw "bump_version.ps1 failed" }

    # Read the version that bump_version just wrote.
    $cmakeText = Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw
    if ($cmakeText -notmatch 'project\([^)]*VERSION\s+(\d+\.\d+\.\d+)') {
        throw "Could not re-read VERSION from CMakeLists.txt"
    }
    $version = $Matches[1]
    $tag     = "v$version"
    Write-Host "Target version: $version (tag $tag)"

    # --- 2. Commit ------------------------------------------------------------
    Step 2 "git commit"
    & git add CMakeLists.txt vcpkg.json
    & git commit -m "Release $tag"
    if ($LASTEXITCODE -ne 0) { throw "git commit failed" }

    # --- 3. Tag ---------------------------------------------------------------
    Step 3 "git tag"
    # Annotated, so the tag carries a message and shows up in `git describe`.
    & git tag -a $tag -m "Release $tag"
    if ($LASTEXITCODE -ne 0) { throw "git tag failed" }

    # --- 4. Build -------------------------------------------------------------
    Step 4 "Build (Release)"
    & (Join-Path $repoRoot 'scripts\build.ps1') -Config Release
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }

    # --- 5. Installer ---------------------------------------------------------
    Step 5 "Build installer"
    # Same explicit-args pattern as step 1 -- PS5 splat dropped these named
    # arguments and PS treated the URL as a positional arg against
    # release.ps1 itself, failing with PositionalParameterNotFound.
    $installer = Join-Path $repoRoot 'installer\build_installer.ps1'
    if     ($SigningCert -and $TimestampUrl) {
        & $installer -SkipBuild -SigningCert $SigningCert -TimestampUrl $TimestampUrl
    } elseif ($SigningCert) {
        & $installer -SkipBuild -SigningCert $SigningCert
    } elseif ($TimestampUrl) {
        & $installer -SkipBuild -TimestampUrl $TimestampUrl
    } else {
        & $installer -SkipBuild
    }
    if ($LASTEXITCODE -ne 0) { throw "Installer build failed" }

    # --- 6. Push (optional) ---------------------------------------------------
    if ($Push) {
        Step 6 "git push (with tag)"
        & git push
        if ($LASTEXITCODE -ne 0) { throw "git push failed" }
        & git push origin $tag
        if ($LASTEXITCODE -ne 0) { throw "git push tag failed" }
    } else {
        Write-Host ""
        Write-Host "(push skipped -- pass -Push to publish commit + tag to origin)" -ForegroundColor Yellow
    }

    # --- Done -----------------------------------------------------------------
    Write-Host ""
    Write-Host "Release $tag complete." -ForegroundColor Green
    $installerExe = Get-ChildItem (Join-Path $repoRoot 'build\installer') -Filter "WinStellarSetup-$version.exe" -ErrorAction SilentlyContinue
    if ($installerExe) {
        Write-Host "Installer: $($installerExe.FullName)"
        Write-Host "Size:      $([math]::Round($installerExe.Length / 1MB, 2)) MB"
    }
} finally {
    Pop-Location
}
