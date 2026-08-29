[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$msbuildCandidates = @(
    'D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe',
    'D:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
)
$msbuild = $msbuildCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $msbuild) {
    $msbuild = (Get-Command msbuild.exe -ErrorAction Stop).Source
}

& $msbuild (Join-Path $root 'JiYuAv.sln') `
    '/t:Rebuild' `
    "/p:Configuration=$Configuration" `
    '/p:Platform=x64' `
    '/m' `
    '/v:minimal'
if ($LASTEXITCODE -ne 0) {
    throw "JiYu AV build failed with exit code $LASTEXITCODE"
}

$output = Join-Path $root 'JiYuAvRelease'
$required = @(
    (Join-Path $output 'JiYuAvKernel.sys'),
    (Join-Path $output 'JiYuAvCtl.exe'),
    (Join-Path $output 'JiYuAvPatternTests.exe'),
    (Join-Path $output 'JiYuAvKernel\JiYuAvKernel.inf'),
    (Join-Path $output 'JiYuAvKernel\jiyuavkernel.cat')
)
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Missing build output: $file"
    }
}

& (Join-Path $output 'JiYuAvPatternTests.exe')
if ($LASTEXITCODE -ne 0) {
    throw "JiYu AV pattern tests failed with exit code $LASTEXITCODE"
}
& (Join-Path $output 'JiYuAvCtl.exe') selftest
if ($LASTEXITCODE -ne 0) {
    throw "JiYu AV controller parser tests failed with exit code $LASTEXITCODE"
}

$required | ForEach-Object {
    $item = Get-Item -LiteralPath $_
    $hash = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
    [pscustomobject]@{ Path = $item.FullName; Length = $item.Length; SHA256 = $hash }
}
