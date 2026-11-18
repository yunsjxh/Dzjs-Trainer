# Builds a self-contained portable folder (and optionally a zip) for USB use:
# the main executable plus the VC++ runtime DLLs it needs.
#
# The main executable already embeds the driver, both hook DLLs, the 64-bit
# injector helper, the updater and the AV driver, and extracts them next to
# itself at startup, so nothing else has to travel with it. What it does NOT
# embed is the Visual C++ runtime, because the projects link with /MD.
#
# Usage:
#   powershell -File tools\Build-PortableBundle.ps1
#   powershell -File tools\Build-PortableBundle.ps1 -Zip
#   powershell -File tools\Build-PortableBundle.ps1 -SourceExe Release\DzjsTrainer.exe -OutputDir D:\usb\DzjsTrainer

[CmdletBinding()]
param(
    [string]$SourceExe = '',
    [string]$OutputDir = '',
    [switch]$Zip,
    [switch]$KeepExisting
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $SourceExe) { $SourceExe = Join-Path $root 'Release\DzjsTrainer.exe' }
if (-not $OutputDir) { $OutputDir = Join-Path $root 'dist\portable' }

if (-not (Test-Path -LiteralPath $SourceExe)) {
    throw "Main executable not found: $SourceExe (build the solution first, see docs/BUILD.md)"
}
$SourceExe = [IO.Path]::GetFullPath($SourceExe)

if ((Test-Path -LiteralPath $OutputDir) -and -not $KeepExisting) {
    # Only ever remove the directory this script owns; refuse anything wider.
    $resolved = [IO.Path]::GetFullPath($OutputDir)
    if ($resolved -eq $root -or $resolved -eq [IO.Path]::GetPathRoot($resolved)) {
        throw "Refusing to clean a dangerous output directory: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

Copy-Item -LiteralPath $SourceExe -Destination $OutputDir -Force

# --- VC++ runtime -----------------------------------------------------------
# Find the redist that belongs to the installed toolset, not whatever happens
# to sit in System32.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$runtimeFiles = @()
if (Test-Path -LiteralPath $vswhere) {
    $installs = & $vswhere -all -products * -property installationPath 2>$null
    foreach ($install in $installs) {
        $redistRoot = Join-Path $install 'VC\Redist\MSVC'
        if (-not (Test-Path -LiteralPath $redistRoot)) { continue }
        $crt = Get-ChildItem -LiteralPath $redistRoot -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'x86\Microsoft.VC143.CRT' } |
            Where-Object { Test-Path -LiteralPath $_ } |
            Select-Object -First 1
        if ($crt) { $runtimeFiles += Get-ChildItem -LiteralPath $crt -Filter '*.dll' -File }
    }
}

$required = @('msvcp140.dll', 'vcruntime140.dll')
$copied = @()
foreach ($dll in ($runtimeFiles | Sort-Object Name -Unique)) {
    if ($dll.Name -notmatch '^(msvcp140|vcruntime140|concrt140)') { continue }
    Copy-Item -LiteralPath $dll.FullName -Destination $OutputDir -Force
    $copied += $dll.Name
}
foreach ($name in $required) {
    if ($copied -notcontains $name) {
        Write-Warning "Missing runtime DLL: $name (the target machine needs the VC++ 2015-2022 x86 redistributable)"
    }
}

# --- Default configuration --------------------------------------------------
$iniPath = Join-Path $OutputDir 'DzjsTrainer.ini'
if (-not (Test-Path -LiteralPath $iniPath)) {
    $ini = @(
        '[JTArgeement]',
        'FirstUse=TRUE',
        'Argeed=FALSE',
        '',
        '[JTSettings]',
        'CaptureExclusion=1',
        'SelfProtect=TRUE',
        'AllowMonitor=TRUE',
        'EnforceMode=record'
    )
    Set-Content -LiteralPath $iniPath -Value $ini -Encoding UTF8
}

# --- Read me ----------------------------------------------------------------
$version = (Get-Item -LiteralPath $SourceExe).VersionInfo.FileVersion
$readme = @(
    "Dzjs Trainer $version - 便携版",
    '=================================',
    '',
    '用法',
    '----',
    '1. 把整个文件夹放到 U 盘或本机任意目录（路径里不要有奇怪符号）。',
    '2. 右键 DzjsTrainer.exe -> 以管理员身份运行。加载内核驱动、向极域学生端注入',
    '   兼容层都需要管理员权限。',
    '3. 首次运行会弹免责声明，同意后进入主界面。设置写在本目录的 DzjsTrainer.ini。',
    '',
    '目录里为什么有这些 dll',
    '----------------------',
    'msvcp140.dll / vcruntime140.dll 等是 Visual C++ 运行库。程序用 /MD 编译，',
    '目标机器没装 VC++ 2015-2022 x86 运行库时必须有它们才能启动。',
    '官方下载包只放了 exe，这就是"只有本体"的原因。',
    '',
    '驱动',
    '----',
    '主程序把驱动、两份 Hook DLL、64 位注入助手、更新器和 AV 驱动都内嵌在 exe 里，',
    '运行时释放到本目录（隐藏属性），所以不需要额外文件。',
    '',
    '注意：自行编译出来的驱动没有微软认可的签名，普通系统会拒绝加载，日志里会出现',
    '驱动加载失败，但窗口化/注入等主要功能仍然可用。要在自己机器上启用驱动，需要',
    '测试签名（bcdedit /set testsigning on 后重启）或用自己的证书签名。',
    '',
    '不想让程序尝试加载驱动，可以在 DzjsTrainer.ini 里设：',
    '  [JTSettings]',
    '  DisableDriver=TRUE',
    '',
    '卸载',
    '----',
    '删除本目录即可。如果加载过驱动，先在界面里卸载驱动（需要合法卸载令牌），',
    '或在服务管理器里停止对应服务后重启。',
    '',
    '许可',
    '----',
    'MIT。原作者：快乐的梦鱼（JiYu Trainer）；维护：云散皆星河。',
    '请仅在你拥有、管理或获得明确授权的设备上使用。'
)
Set-Content -LiteralPath (Join-Path $OutputDir 'README.txt') -Value $readme -Encoding UTF8

if ($Zip) {
    $zipPath = "$OutputDir.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path (Join-Path $OutputDir '*') -DestinationPath $zipPath -Force
    Write-Output "ZIP=$zipPath"
}

$files = Get-ChildItem -LiteralPath $OutputDir -File | Sort-Object Name
$total = ($files | Measure-Object Length -Sum).Sum
Write-Output "OUTPUT=$([IO.Path]::GetFullPath($OutputDir))"
foreach ($file in $files) {
    Write-Output ('  {0,-32} {1,10:N0} bytes' -f $file.Name, $file.Length)
}
Write-Output ('TOTAL={0:N1} MB' -f ($total / 1MB))
