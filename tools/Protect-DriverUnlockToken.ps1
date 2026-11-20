[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$InputPath,[Parameter(Mandatory=$true)][string]$OutputPath)
# Turns a raw 144-byte JDRV_UNLOAD_TOKEN into the container the main EXE embeds.
# Obfuscation only: the AES key below is the same one compiled into
# DzjsTrainer/KernelUtils.cpp and repeated in Test-DriverUnlockToken.ps1.
#
#   token : size(4) magic(4)='ULOK' version(4) action(4) machineHash(32)
#           notBefore(8) notAfter(8) nonce(16) signature(64)
#   output: 'TEN1'(4) version(4) IV(16) cipherLength(4) AES-256-CBC/PKCS7
#
# Called by Complete-DriverSigning.ps1.
$ErrorActionPreference='Stop'
$inputBytes=[IO.File]::ReadAllBytes($InputPath)
if($inputBytes.Length -ne 144){throw "Expected a 144-byte unload token, got $($inputBytes.Length)"}
if([BitConverter]::ToUInt32($inputBytes,0) -ne 144){throw "Token size field does not match its actual length"}
$magic=[BitConverter]::ToUInt32($inputBytes,4)
if($magic -ne 0x4B4F4C55){throw ("Expected a ULOK token, got magic 0x{0:X8}" -f $magic)}
$version=[BitConverter]::ToUInt32($inputBytes,8)
if($version -ne 1){throw "Unsupported token version $version"}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null
$partA = [byte[]](0xD9,0x13,0x89,0x2C,0x4C,0x78,0x7D,0x94,0x3C,0xCA,0x79,0x7B,0x61,0x03,0xA4,0x21,0xCF,0x35,0x39,0x70,0x81,0xBA,0xA9,0x9A,0x6E,0x45,0xD1,0xD8,0x35,0xDD,0x4F,0x9F)
$partB = [byte[]](0x72,0xB0,0x54,0x31,0xC6,0x5E,0xA7,0xE5,0x59,0x69,0x2C,0xFB,0x06,0x62,0x0C,0xFA,0x26,0x5B,0x30,0x41,0x89,0x05,0xEF,0x14,0xD0,0x0F,0x84,0x69,0xA3,0x0C,0x64,0x8C)
$key = [byte[]]::new(32)
for($i=0; $i -lt $key.Length; ++$i) { $key[$i] = $partA[$i] -bxor $partB[$i] }
$aes = [Security.Cryptography.Aes]::Create()
try {
    $aes.KeySize = 256
    $aes.BlockSize = 128
    $aes.Mode = [Security.Cryptography.CipherMode]::CBC
    $aes.Padding = [Security.Cryptography.PaddingMode]::PKCS7
    $aes.Key = $key
    $aes.GenerateIV()
    $encryptor = $aes.CreateEncryptor()
    try { $cipher = $encryptor.TransformFinalBlock($inputBytes, 0, $inputBytes.Length) }
    finally { $encryptor.Dispose() }
    $stream = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint32]0x314E4554)
        $writer.Write([uint32]1)
        $writer.Write($aes.IV)
        $writer.Write([uint32]$cipher.Length)
        $writer.Write($cipher)
        $writer.Flush()
        [IO.File]::WriteAllBytes($OutputPath, $stream.ToArray())
    } finally { $writer.Dispose(); $stream.Dispose() }
} finally {
    [Array]::Clear($key, 0, $key.Length)
    $aes.Dispose()
}
Write-Output "encrypted token: $OutputPath"
Write-Output "ciphertext size: $($cipher.Length)"
