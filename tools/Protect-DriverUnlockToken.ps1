# Wraps a raw 144-byte unload token into the container the driver expects.
#
# This is obfuscation, not protection: the AES key is derived from two constants
# that ship in DzjsTrainer/KernelUtils.cpp and in Test-DriverUnlockToken.ps1.
# Real secrecy lives in unlock-key-material/ (the ECDSA private key that signs
# the token), which is git-ignored and must never be committed.
#
# Container layout (little endian), see DecryptEmbeddedUnloadToken():
#
#   0   "TEN1" magic            4 bytes
#   4   version = 1             4 bytes
#   8   IV                     16 bytes
#   24  ciphertext length       4 bytes
#   28  AES-256-CBC (PKCS7)    exactly 160 bytes for a 144-byte token
#
# Usage (called by Complete-DriverSigning.ps1):
#   powershell -File tools\Protect-DriverUnlockToken.ps1 `
#       -InputPath tools\unlock-key-material\DzjsTrainerDriver.sys.unlock `
#       -OutputPath Release\DzjsTrainerDriver\DzjsTrainerDriver.unlock.enc

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$IvHex = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$tokenSize = 144
$tokenMagic = 0x4B4F4C55   # "ULOK"
$containerMagic = 0x314E4554  # "TEN1"
$containerVersion = 1

$partA = [byte[]](0xD9,0x13,0x89,0x2C,0x4C,0x78,0x7D,0x94,0x3C,0xCA,0x79,0x7B,0x61,0x03,0xA4,0x21,0xCF,0x35,0x39,0x70,0x81,0xBA,0xA9,0x9A,0x6E,0x45,0xD1,0xD8,0x35,0xDD,0x4F,0x9F)
$partB = [byte[]](0x72,0xB0,0x54,0x31,0xC6,0x5E,0xA7,0xE5,0x59,0x69,0x2C,0xFB,0x06,0x62,0x0C,0xFA,0x26,0x5B,0x30,0x41,0x89,0x05,0xEF,0x14,0xD0,0x0F,0x84,0x69,0xA3,0x0C,0x64,0x8C)
$key = [byte[]]::new(32)
for ($i = 0; $i -lt 32; ++$i) { $key[$i] = $partA[$i] -bxor $partB[$i] }

if (-not (Test-Path -LiteralPath $InputPath)) {
    throw "Unload token not found: $InputPath"
}
if ((Test-Path -LiteralPath $OutputPath) -and -not $Force) {
    throw "Output already exists (pass -Force to replace): $OutputPath"
}

$plain = [IO.File]::ReadAllBytes($InputPath)
if ($plain.Length -ne $tokenSize) {
    throw "Token must be exactly $tokenSize bytes, got $($plain.Length): $InputPath"
}
$sizeField = [BitConverter]::ToUInt32($plain, 0)
if ($sizeField -ne $tokenSize) {
    throw "Token size field is $sizeField, expected $tokenSize"
}
$magic = [BitConverter]::ToUInt32($plain, 4)
if ($magic -ne $tokenMagic) {
    throw ('Token magic is 0x{0:X8}, expected 0x{1:X8} ("ULOK")' -f $magic, $tokenMagic)
}

if ($IvHex) {
    if ($IvHex -notmatch '^[0-9a-fA-F]{32}$') {
        throw '-IvHex must be 32 hex characters (16 bytes)'
    }
    $iv = [byte[]]::new(16)
    for ($i = 0; $i -lt 16; ++$i) {
        $iv[$i] = [Convert]::ToByte($IvHex.Substring($i * 2, 2), 16)
    }
}
else {
    $iv = [byte[]]::new(16)
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($iv)
}

$aes = [Security.Cryptography.Aes]::Create()
try {
    $aes.Key = $key
    $aes.IV = $iv
    $aes.Mode = [Security.Cryptography.CipherMode]::CBC
    $aes.Padding = [Security.Cryptography.PaddingMode]::PKCS7
    $encryptor = $aes.CreateEncryptor()
    try { $cipher = $encryptor.TransformFinalBlock($plain, 0, $plain.Length) }
    finally { $encryptor.Dispose() }
}
finally { $aes.Dispose() }

$container = [byte[]]::new(28 + $cipher.Length)
[BitConverter]::GetBytes([uint32]$containerMagic).CopyTo($container, 0)
[BitConverter]::GetBytes([uint32]$containerVersion).CopyTo($container, 4)
$iv.CopyTo($container, 8)
[BitConverter]::GetBytes([uint32]$cipher.Length).CopyTo($container, 24)
$cipher.CopyTo($container, 28)

$directory = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($OutputPath))
if ($directory -and -not (Test-Path -LiteralPath $directory)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
[IO.File]::WriteAllBytes($OutputPath, $container)

# Round-trip check: decrypt exactly the way DecryptEmbeddedUnloadToken() does.
$aes = [Security.Cryptography.Aes]::Create()
try {
    $aes.Key = $key
    $aes.IV = $container[8..23]
    $aes.Mode = [Security.Cryptography.CipherMode]::CBC
    $aes.Padding = [Security.Cryptography.PaddingMode]::PKCS7
    $decryptor = $aes.CreateDecryptor()
    try { $roundTrip = $decryptor.TransformFinalBlock($container, 28, $container.Length - 28) }
    finally { $decryptor.Dispose() }
}
finally { $aes.Dispose() }

if (-not [Linq.Enumerable]::SequenceEqual([byte[]]$roundTrip, [byte[]]$plain)) {
    Remove-Item -LiteralPath $OutputPath -Force
    throw 'Round-trip verification failed; output removed'
}

Write-Output "TOKEN_SIZE=$($plain.Length)"
Write-Output "CONTAINER_SIZE=$($container.Length)"
Write-Output "IV=$([BitConverter]::ToString($iv).Replace('-', '').ToLowerInvariant())"
Write-Output "PLAIN_SHA256=$((Get-FileHash -LiteralPath $InputPath -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "OUTPUT=$([IO.Path]::GetFullPath($OutputPath))"
