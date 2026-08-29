[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$machineGuid = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Cryptography').MachineGuid
$normalized = ($machineGuid.ToString().ToUpperInvariant() -replace '[^0-9A-F]', '')
if ($normalized.Length -ne 32) {
    throw 'MachineGuid did not normalize to 32 hexadecimal characters.'
}
$bytes = [System.Text.Encoding]::ASCII.GetBytes($normalized)
$hash = [System.Security.Cryptography.SHA256]::HashData($bytes)
([System.BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
