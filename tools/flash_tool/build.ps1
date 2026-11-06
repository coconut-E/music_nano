param(
    [string]$Out = "out\music_flash.exe"
)

$ErrorActionPreference = 'Stop'

# 注意: PowerShell 的 Set-Location 不会同步进程工作目录, 而 gcc 是原生进程,
# 它用的是进程 CWD。所以这里统一用绝对路径 + 显式设置 [Environment]::CurrentDirectory,
# 避免"在 tools\flash_tool 下运行却找不到 src\music_flash.c"。
$root = $PSScriptRoot
Set-Location $root
[Environment]::CurrentDirectory = $root

# 找 gcc: 环境变量 $env:GCC -> PATH -> MSYS2 常见安装位置
$gcc = $env:GCC
if (-not ($gcc -and (Test-Path $gcc))) {
    $cmd = Get-Command gcc -ErrorAction SilentlyContinue
    if ($cmd) { $gcc = $cmd.Source }
}
if (-not ($gcc -and (Test-Path $gcc))) {
    foreach ($c in @('C:\msys64\mingw64\bin\gcc.exe', 'C:\msys32\mingw32\bin\gcc.exe')) {
        if (Test-Path $c) { $gcc = $c; break }
    }
}
if (-not ($gcc -and (Test-Path $gcc))) {
    throw "找不到 gcc: 装 MSYS2 MinGW-w64, 或用 `$env:GCC 指定完整路径"
}

# cc1.exe 依赖 mingw64\bin 下的 DLL, 必须把该目录放到 PATH 最前
$gccDir = Split-Path -Parent $gcc
$env:PATH = $gccDir + ';' + $env:PATH

# 临时文件放在本目录, 避免受系统 TEMP 权限影响
$tmpDir = Join-Path $root 'tmp'
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$env:TMP = $tmpDir
$env:TEMP = $tmpDir

$outAbs = Join-Path $root $Out
New-Item -ItemType Directory -Force -Path (Split-Path $outAbs) | Out-Null

$src = @((Join-Path $root 'src\music_flash.c'), (Join-Path $root 'src\md5.c'))

# 用管道接住 gcc 的 stderr: PS 5.1 下 stderr 直连控制台时, 配合
# $ErrorActionPreference='Stop' 会把警告当成终止错误, $LASTEXITCODE 失真
# gcc 把警告写到 stderr, 而 $ErrorActionPreference='Stop' 会把它当成终止错误,
# 所以在调用期间临时放宽
$oldEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $gcc -std=gnu11 -Os -s -static -Wall -Wextra -Wno-unused-parameter `
    -o $outAbs @src -ladvapi32 2>&1 | ForEach-Object { Write-Host $_ }
$rc = $LASTEXITCODE
$ErrorActionPreference = $oldEap
if ($rc -ne 0) { throw "编译失败 (exit $rc)" }

$fi = Get-Item $outAbs
Write-Host ("OK  {0}  {1:N0} B" -f $fi.FullName, $fi.Length)
