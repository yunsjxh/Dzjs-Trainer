# Signs the two kernel drivers and reports what the client policy will see.
#
# Two certificate situations are supported:
#   1. Certificate installed in a Windows store (self-signed test cert, or an
#      EV certificate imported by its vendor tool):
#        -Thumbprint <sha1>            (default: machine store; -CurrentUser to switch)
#   2. .pfx file exported by the CA:
#        -Pfx <path> -PfxPassword <password>
#
# After signing, rebuild the main executable: DzjsTrainer.rc embeds the .sys
# files as resources, so the signed bytes only reach users after a relink.
#
# The client accepts a driver when one of these holds:
#   a) the signature verifies and the publisher is in DriverPublisherAllowlist.h
#      ("Microsoft Windows Hardware Compatibility Publisher" is in that list, so
#      an attestation-signed driver needs no source change), or
#   2. the publisher/thumbprint matches DriverSignerPin.h, or
#   c) the root is untrusted but the publisher is one of the two JiYu test names
#      (test-signing builds only).
#
# Usage:
#   powershell -File scripts\Sign-Drivers.ps1 -Thumbprint <sha1>
#   powershell -File scripts\Sign-Drivers.ps1 -Pfx C:\certs\ev.pfx -PfxPassword ***
#   powershell -File scripts\Sign-Drivers.ps1 -Thumbprint <sha1> -TimestampUrl http://timestamp.digicert.com

[CmdletBinding(DefaultParameterSetName = 'Store')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Store')][string]$Thumbprint,
    [Parameter(ParameterSetName = 'Store')][switch]$CurrentUser,

    [Parameter(Mandatory = $true, ParameterSetName = 'Pfx')][string]$Pfx,
    [Parameter(ParameterSetName = 'Pfx')][string]$PfxPassword,

    [string]$TimestampUrl = '',
    [string]$Root = '',
    [string[]]$Drivers = @(
        'Release\DzjsTrainerDriver\DzjsTrainerDriver.sys',
        'JiYuAvRelease\JiYuAvKernel.sys'
    )
)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }

function Find-SignTool {
    $candidate = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($candidate) { return $candidate.FullName }
    $onPath = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw 'signtool.exe not found (install the Windows SDK signing tools)'
}

$signtool = Find-SignTool
Write-Output "SIGNTOOL=$signtool"

$signArgs = @('sign', '/fd', 'sha256', '/a')
if ($PSCmdlet.ParameterSetName -eq 'Store') {
    $store = if ($CurrentUser) { 'Cert:\CurrentUser\My' } else { 'Cert:\LocalMachine\My' }
    $normalized = $Thumbprint -replace '\s', ''
    $cert = Get-ChildItem $store | Where-Object { $_.Thumbprint -eq $normalized } | Select-Object -First 1
    if (-not $cert) { throw "Certificate not found in $store : $Thumbprint" }
    Write-Output ("CERT={0}  EXPIRES={1:yyyy-MM-dd}" -f $cert.Subject, $cert.NotAfter)
    if (-not $CurrentUser) { $signArgs += '/sm' }
    $signArgs += @('/sha1', $normalized)
} else {
    if (-not (Test-Path -LiteralPath $Pfx)) { throw "PFX not found: $Pfx" }
    $signArgs += @('/f', $Pfx)
    if ($PfxPassword) { $signArgs += @('/p', $PfxPassword) }
}
if ($TimestampUrl) { $signArgs += @('/tr', $TimestampUrl, '/td', 'sha256') }

foreach ($relative in $Drivers) {
    $path = Join-Path $Root $relative
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Warning "Driver not built, skipped: $path"
        continue
    }
    Write-Output "SIGN $relative"
    & $signtool @signArgs $path
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $relative (exit $LASTEXITCODE)" }

    $signature = Get-AuthenticodeSignature -LiteralPath $path
    $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '<none>' }
    $thumb = if ($signature.SignerCertificate) { $signature.SignerCertificate.Thumbprint } else { '<none>' }
    $simple = if ($signature.SignerCertificate) { ($subject -split ',')[0] -replace '^CN=', '' } else { '<none>' }
    Write-Output ("  STATUS={0}" -f $signature.Status)
    Write-Output ("  SUBJECT={0}" -f $subject)
    Write-Output ("  THUMBPRINT={0}" -f $thumb)
    Write-Output ("  PUBLISHER={0}" -f $simple)
}

Write-Output ''
Write-Output 'Next steps:'
Write-Output '  1. If PUBLISHER is "Microsoft Windows Hardware Compatibility Publisher"'
Write-Output '     (attestation signing) the client already accepts it: just relink.'
Write-Output '  2. Otherwise add the publisher to DzjsTrainer/DriverPublisherAllowlist.h,'
Write-Output '     or set kJiYuTestSignerSubject/kJiYuTestSignerThumbprint in'
Write-Output '     DzjsTrainer/DriverSignerPin.h to the values printed above.'
Write-Output '  3. Relink the main program so the signed drivers are embedded:'
Write-Output '     msbuild DzjsTrainer\DzjsTrainer.vcxproj /p:Configuration=Release /p:Platform=Win32'
Write-Output '  4. Repackage: tools\Build-PortableBundle.ps1 -Zip -TeacherGui <path>'
