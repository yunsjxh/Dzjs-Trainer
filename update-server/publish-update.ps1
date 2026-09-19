param(
    [Parameter(Mandatory = $true)] [string]$Version,
    [Parameter(Mandatory = $true)] [string]$ReleaseExe,
    [string]$Notes = "",
    [string]$ServerRoot = ''
)

$ErrorActionPreference = 'Stop'
$scriptRoot = if ([string]::IsNullOrWhiteSpace($ServerRoot)) { $PSScriptRoot } else { (Resolve-Path -LiteralPath $ServerRoot).Path }
$source = (Resolve-Path -LiteralPath $ReleaseExe).Path
$releaseDir = Join-Path $scriptRoot 'releases'
$target = Join-Path $releaseDir 'DzjsTrainerUpdater.exe'
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
Copy-Item -LiteralPath $source -Destination $target -Force
$sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = [ordered]@{
    version = $Version
    notes = $Notes
    url = 'releases/DzjsTrainerUpdater.exe'
    sha256 = $sha256
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $scriptRoot 'manifest.json') -Encoding UTF8
Write-Host "Published: $target"
Write-Host "SHA256:    $sha256"
Write-Host "Manifest:  $(Join-Path $scriptRoot 'manifest.json')"
