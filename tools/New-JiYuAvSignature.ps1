[CmdletBinding(DefaultParameterSetName = 'Hash')]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Sample,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 4294967295)]
    [uint32]$Id,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 63)]
    [string]$Name,

    [Parameter(ParameterSetName = 'Pattern', Mandatory = $true)]
    [ValidateRange(0, 2147483647)]
    [int]$Offset,

    [Parameter(ParameterSetName = 'Pattern', Mandatory = $true)]
    [ValidateRange(4, 64)]
    [int]$Length,

    [Parameter(ParameterSetName = 'Pattern')]
    [int[]]$Wildcard = @()
)

$ErrorActionPreference = 'Stop'
$samplePath = (Resolve-Path -LiteralPath $Sample).Path
$escapedName = $Name.Replace('"', '\"')

if ($PSCmdlet.ParameterSetName -eq 'Hash') {
    $hash = (Get-FileHash -LiteralPath $samplePath -Algorithm SHA256).Hash
    [pscustomobject]@{
        Type = 'SHA256'
        Id = $Id
        Name = $Name
        Sample = $samplePath
        SHA256 = $hash
        Command = "JiYuAvCtl.exe add-hash $Id `"$escapedName`" $hash"
    }
    return
}

$stream = [System.IO.File]::OpenRead($samplePath)
try {
    if ([int64]$Offset + $Length -gt $stream.Length) {
        throw "Requested range $Offset+$Length exceeds sample length $($stream.Length)"
    }
    $stream.Position = $Offset
    $bytes = [byte[]]::new($Length)
    $read = $stream.Read($bytes, 0, $Length)
    if ($read -ne $Length) {
        throw "Read $read bytes, expected $Length"
    }
}
finally {
    $stream.Dispose()
}

$wildcardSet = [System.Collections.Generic.HashSet[int]]::new()
foreach ($index in $Wildcard) {
    if ($index -lt 0 -or $index -ge $Length) {
        throw "Wildcard index $index is outside 0..$($Length - 1)"
    }
    [void]$wildcardSet.Add($index)
}

$tokens = for ($index = 0; $index -lt $Length; ++$index) {
    if ($wildcardSet.Contains($index)) { '??' } else { $bytes[$index].ToString('X2') }
}
$pattern = $tokens -join ''
[pscustomobject]@{
    Type = 'Pattern'
    Id = $Id
    Name = $Name
    Sample = $samplePath
    Offset = $Offset
    Length = $Length
    Wildcard = $Wildcard -join ','
    Pattern = $pattern
    Command = "JiYuAvCtl.exe add-pattern $Id `"$escapedName`" $pattern"
}
