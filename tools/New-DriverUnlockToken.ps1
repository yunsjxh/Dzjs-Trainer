[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PrivateKeyPath,
    [string]$OutputPath = (Join-Path (Split-Path $PrivateKeyPath -Parent) 'JiYuTrainerDriver.sys.unlock')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$privateBytes = [System.IO.File]::ReadAllBytes($PrivateKeyPath)
$ecdsa = [System.Security.Cryptography.ECDsa]::Create()
$writer = $null
try {
    $read = 0
    $null = $ecdsa.ImportPkcs8PrivateKey($privateBytes, [ref]$read)
    $payload = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($payload)
    $writer.Write([uint32]144)
    $writer.Write([uint32]0x4B4F4C55)
    $writer.Write([uint32]1)
    $writer.Write([uint32]1)
    # Portable authorization: the driver verifies the vendor signature but
    # does not bind the token to MachineGuid.
    $writer.Write([byte[]]::new(32))
    # Time fields remain zero for protocol compatibility; the driver does not
    # associate the unload authorization with wall-clock time.
    $writer.Write([uint64]0)
    $writer.Write([uint64]0)
    $nonce = [byte[]]::new(16)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($nonce)
    $writer.Write($nonce)
    $writer.Flush()
    $payloadBytes = $payload.ToArray()
    $digest = [System.Security.Cryptography.SHA256]::HashData($payloadBytes)
    $signature = $ecdsa.SignHash(
        $digest,
        [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    if ($signature.Length -ne 64) {
        throw "Unexpected ECDSA signature length: $($signature.Length)."
    }
    [System.IO.File]::WriteAllBytes($OutputPath, $payloadBytes + $signature)
    Write-Output "token: $OutputPath"
    Write-Output 'scope: portable (ECDSA signature only)'
}
finally {
    if ($writer) { $writer.Dispose() }
    if ($payload) { $payload.Dispose() }
    $ecdsa.Dispose()
}
