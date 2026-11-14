[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EncryptedPath,
    [Parameter(Mandatory = $true)][string]$PlainPath
)
$ErrorActionPreference = 'Stop'
$enc = [IO.File]::ReadAllBytes($EncryptedPath)
$raw = [IO.File]::ReadAllBytes($PlainPath)
$partA = [byte[]](0xD9,0x13,0x89,0x2C,0x4C,0x78,0x7D,0x94,0x3C,0xCA,0x79,0x7B,0x61,0x03,0xA4,0x21,0xCF,0x35,0x39,0x70,0x81,0xBA,0xA9,0x9A,0x6E,0x45,0xD1,0xD8,0x35,0xDD,0x4F,0x9F)
$partB = [byte[]](0x72,0xB0,0x54,0x31,0xC6,0x5E,0xA7,0xE5,0x59,0x69,0x2C,0xFB,0x06,0x62,0x0C,0xFA,0x26,0x5B,0x30,0x41,0x89,0x05,0xEF,0x14,0xD0,0x0F,0x84,0x69,0xA3,0x0C,0x64,0x8C)
$key = [byte[]]::new(32)
for ($i = 0; $i -lt 32; ++$i) { $key[$i] = $partA[$i] -bxor $partB[$i] }
$aes = [Security.Cryptography.Aes]::Create()
try {
    $aes.Key = $key
    $aes.IV = $enc[8..23]
    $aes.Mode = [Security.Cryptography.CipherMode]::CBC
    $aes.Padding = [Security.Cryptography.PaddingMode]::PKCS7
    $decryptor = $aes.CreateDecryptor()
    try { $plain = $decryptor.TransformFinalBlock($enc, 28, $enc.Length - 28) }
    finally { $decryptor.Dispose() }
}
finally { $aes.Dispose() }
$match = [Linq.Enumerable]::SequenceEqual([byte[]]$plain, [byte[]]$raw)
Write-Output "ENCRYPTED_SIZE=$($enc.Length)"
Write-Output "DECRYPT_MATCH=$match"
Write-Output "RAW_SHA256=$((Get-FileHash $PlainPath -Algorithm SHA256).Hash)"
$temp = [IO.Path]::GetTempFileName()
try {
    [IO.File]::WriteAllBytes($temp, $plain)
    Write-Output "DECRYPT_SHA256=$((Get-FileHash $temp -Algorithm SHA256).Hash)"
}
finally { Remove-Item -LiteralPath $temp -Force }
if (-not $match) { exit 1 }
