[CmdletBinding()]
param(
    [string]$CertificateThumbprint,
    [string]$TimestampUrl,
    [string]$DriverPath
)

$ErrorActionPreference = 'Stop'

$solutionRoot = Split-Path $PSScriptRoot -Parent
$driverBuildRoot = Join-Path $solutionRoot 'JiYuTrainerDriver\Release'
$defaultDriverOutput = Join-Path $driverBuildRoot 'JiYuTrainerDriver.sys'
$fallbackDriverOutput = Join-Path $driverBuildRoot 'JiYuTrainerDriver\JiYuTrainerDriver.sys'
$packagedDriverOutput = Join-Path $solutionRoot 'Release\JiYuTrainerDriver\JiYuTrainerDriver.sys'
if ($DriverPath) {
    $driverOutput = [IO.Path]::GetFullPath($DriverPath)
}
elseif (Test-Path -LiteralPath $defaultDriverOutput) {
    $driverOutput = $defaultDriverOutput
}
elseif (Test-Path -LiteralPath $packagedDriverOutput) {
    $driverOutput = $packagedDriverOutput
}
else {
    $driverOutput = $fallbackDriverOutput
}
$driverPackage = Split-Path $driverOutput -Parent
$resourcePackage = Join-Path $solutionRoot 'Release\JiYuTrainerDriver'
$unlockTokenSource = Join-Path $solutionRoot 'JiYuTrainerDriver.unlock.enc.sample'
$unlockTokenDestination = Join-Path $resourcePackage 'JiYuTrainerDriver.unlock.enc'
$mainProject = Join-Path $solutionRoot 'JiYuTrainer\DzjsTrainer.vcxproj'
$mainExecutable = Join-Path $solutionRoot 'Release\DzjsTrainer.exe'

foreach ($path in @($driverOutput, $mainProject)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing required file: $path"
    }
}
if (-not (Test-Path -LiteralPath $unlockTokenSource)) {
    throw "Missing public unload-token placeholder: $unlockTokenSource"
}

$driverSignature = Get-AuthenticodeSignature -LiteralPath $driverOutput
if ($null -eq $driverSignature.SignerCertificate) {
    throw "The driver has no embedded Authenticode signature: $driverOutput"
}

$inf2cat = (Get-Command Inf2Cat.exe -ErrorAction Stop).Source
$signtool = (Get-Command signtool.exe -ErrorAction Stop).Source
$msbuild = (Get-Command msbuild.exe -ErrorAction Stop).Source

& $inf2cat "/driver:$driverPackage" '/os:10_X64'
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE"
}

$catalog = Join-Path $driverPackage 'jiyutrainerdriver.cat'
if (-not (Test-Path -LiteralPath $catalog)) {
    throw "Inf2Cat did not create the catalog: $catalog"
}

if ($CertificateThumbprint) {
    $signArguments = @('sign', '/sha1', $CertificateThumbprint, '/fd', 'SHA256')
    if ($TimestampUrl) {
        $signArguments += @('/tr', $TimestampUrl, '/td', 'SHA256')
    }
    $signArguments += $catalog
    & $signtool @signArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Catalog signing failed with exit code $LASTEXITCODE"
    }
}

foreach ($destination in @($resourcePackage)) {
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    $driverDestination = Join-Path $destination 'JiYuTrainerDriver.sys'
    $infSource = Join-Path $driverPackage 'JiYuTrainerDriver.inf'
    $infDestination = Join-Path $destination 'JiYuTrainerDriver.inf'
    $catalogDestination = Join-Path $destination 'jiyutrainerdriver.cat'
    foreach ($copy in @(
        @{ Source = $driverOutput; Destination = $driverDestination },
        @{ Source = $infSource; Destination = $infDestination },
        @{ Source = $catalog; Destination = $catalogDestination },
        @{ Source = $unlockTokenSource; Destination = $unlockTokenDestination }
    )) {
        if ([IO.Path]::GetFullPath($copy.Source) -ne [IO.Path]::GetFullPath($copy.Destination)) {
            Copy-Item -LiteralPath $copy.Source -Destination $copy.Destination -Force
        }
    }
}

& $msbuild $mainProject '/t:Rebuild' '/p:Configuration=Release' '/p:Platform=Win32' "/p:SolutionDir=$solutionRoot\" '/p:BuildProjectReferences=false' '/m' '/v:minimal'
if ($LASTEXITCODE -ne 0) {
    throw "Main executable build failed with exit code $LASTEXITCODE"
}

$readerSource = @'
using System;
using System.Runtime.InteropServices;
public static class DriverResourceReader {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryExW(string file, IntPtr handle, uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, EntryPoint="FindResourceW", SetLastError=true)]
    public static extern IntPtr FindResourceW(IntPtr module, IntPtr name, string type);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")]
    public static extern IntPtr LockResource(IntPtr resourceData);
    [DllImport("kernel32.dll")]
    public static extern bool FreeLibrary(IntPtr module);
}
'@

if (-not ('DriverResourceReader' -as [type])) {
    Add-Type -TypeDefinition $readerSource
}

$module = [DriverResourceReader]::LoadLibraryExW($mainExecutable, [IntPtr]::Zero, 2)
if ($module -eq [IntPtr]::Zero) {
    throw "Unable to load executable resources: $mainExecutable"
}

try {
    $resource = [DriverResourceReader]::FindResourceW($module, [IntPtr]130, 'BIN')
    if ($resource -eq [IntPtr]::Zero) {
        throw 'IDR_DLL_DRIVER (130, BIN) is missing from the executable'
    }
    $size = [DriverResourceReader]::SizeofResource($module, $resource)
    $resourceData = [DriverResourceReader]::LoadResource($module, $resource)
    $resourcePointer = [DriverResourceReader]::LockResource($resourceData)
    $bytes = New-Object byte[] $size
    [Runtime.InteropServices.Marshal]::Copy($resourcePointer, $bytes, 0, $size)
    $embeddedHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
}
finally {
    [void][DriverResourceReader]::FreeLibrary($module)
}

$driverHash = (Get-FileHash -LiteralPath $driverOutput -Algorithm SHA256).Hash
if ($embeddedHash -ne $driverHash) {
    throw "Embedded driver hash mismatch: embedded=$embeddedHash driver=$driverHash"
}

$catalogSignature = Get-AuthenticodeSignature -LiteralPath $catalog
Write-Output "Driver signer: $($driverSignature.SignerCertificate.Subject)"
Write-Output "Driver SHA256: $driverHash"
Write-Output "Embedded driver SHA256: $embeddedHash"
Write-Output "Catalog signer: $($catalogSignature.SignerCertificate.Subject)"
Write-Output "Catalog path: $catalog"
if ($null -eq $catalogSignature.SignerCertificate) {
    Write-Warning 'The catalog contains the current driver hash but is not signed yet.'
}
