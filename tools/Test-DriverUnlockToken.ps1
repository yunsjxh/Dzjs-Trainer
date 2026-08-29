[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EncryptedPath,
    [Parameter(Mandatory = $true)][string]$PlainPath
)
$ErrorActionPreference = 'Stop'
$enc = [IO.File]::ReadAllBytes($EncryptedPath)
$raw = [IO.File]::ReadAllBytes($PlainPath)
$partA = [byte[]](0x6D,0x2A,0x91,0x44,0xD7,0x0B,0x3E,0xF2,0x18,0xC4,0x67,0xAA,0x50,0x39,0xE1,0x7C,0x82,0x15,0xB8,0x4F,0x23,0xD0,0x69,0xAC,0xF5,0x31,0x0E,0x76,0xCA,0x94,0x48,0x1B)
$partB = [byte[]](0xB4,0xE8,0x27,0x9A,0x0C,0xD1,0x55,0x68,0xF3,0x2B,0x80,0x16,0x7A,0xC6,0x34,0xE9,0x19,0xAF,0x43,0xD8,0x6E,0x02,0xB5,0x77,0x28,0xCD,0x91,0x0A,0x5F,0xE3,0x36,0xC0)
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
