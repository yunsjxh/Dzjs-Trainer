[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'unlock-key-material'),
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ((Test-Path $OutputDirectory) -and -not $Force) {
    throw "Output directory exists. Pass -Force only when rotating the key pair."
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$ecdsa = [System.Security.Cryptography.ECDsa]::Create(
    [System.Security.Cryptography.ECCurve]::CreateFromFriendlyName('nistP256'))
try {
    $privatePath = Join-Path $OutputDirectory 'driver-unload-private.pk8'
    $publicHeaderPath = Join-Path $PSScriptRoot '..\JiYuTrainerDriver\UnlockPublicKey.h'
    $privateBytes = $ecdsa.ExportPkcs8PrivateKey()
    [System.IO.File]::WriteAllBytes($privatePath, $privateBytes)

    $parameters = $ecdsa.ExportParameters($false)
    $blob = [byte[]]::new(72)
    [BitConverter]::GetBytes([uint32]0x31534345).CopyTo($blob, 0)
    [BitConverter]::GetBytes([uint32]32).CopyTo($blob, 4)
    $parameters.Q.X.CopyTo($blob, 8)
    $parameters.Q.Y.CopyTo($blob, 40)
    $bytes = ($blob | ForEach-Object { '0x{0:X2}' -f $_ }) -join ', '
    $header = @"
#pragma once

static const unsigned char g_JdrvUnlockPublicKey[] = {
    $bytes
};
"@
    Set-Content -LiteralPath $publicHeaderPath -Value $header -Encoding ascii
    Write-Output "private key: $privatePath"
    Write-Output "public header: $publicHeaderPath"
}
finally {
    $ecdsa.Dispose()
}
