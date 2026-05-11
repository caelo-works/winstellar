<#
.SYNOPSIS
    Creates a SELF-SIGNED code-signing certificate in the current user's
    Personal store and prints its thumbprint.

    Self-signed certs are only useful to verify that the signing pipeline
    works end-to-end. They DO NOT remove SmartScreen warnings for
    distributed users (their machines won't trust the cert). For real
    distribution you need:
      - a standard code-signing cert (Sectigo, DigiCert, ~80 EUR/yr,
        warnings persist until reputation builds)
      - an EV code-signing cert (~400 EUR/yr + USB token, instant trust)
      - Microsoft Trusted Signing (~10 EUR/month, cloud-based, instant trust)

.EXAMPLE
    PS> .\scripts\create_dev_cert.ps1
    Created cert with thumbprint ABC123DEF456...
    Use it like:
       .\installer\build_installer.cmd -SigningCert ABC123DEF456...
#>

[CmdletBinding()]
param(
    [string]$Subject = "CN=WinStellar Dev Self-Signed",
    [int]$YearsValid = 5
)

$ErrorActionPreference = 'Stop'

$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -KeyUsage DigitalSignature `
    -CertStoreLocation Cert:\CurrentUser\My `
    -NotAfter (Get-Date).AddYears($YearsValid) `
    -KeyAlgorithm RSA -KeyLength 3072 `
    -HashAlgorithm SHA256

Write-Host ""
Write-Host "Self-signed code-signing cert created." -ForegroundColor Green
Write-Host "  Subject:    $($cert.Subject)"
Write-Host "  Thumbprint: $($cert.Thumbprint)"
Write-Host "  Valid until: $($cert.NotAfter)"
Write-Host ""
Write-Host "Build a signed installer with:" -ForegroundColor Cyan
Write-Host "    .\installer\build_installer.cmd -SigningCert $($cert.Thumbprint)"
Write-Host ""
Write-Host "Note: this cert only proves the pipeline works locally." -ForegroundColor Yellow
Write-Host "Other Windows machines will still show SmartScreen warnings."
