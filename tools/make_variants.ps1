# 批量生成 OTA 测试用的版本变体 (每条一个 .bin)
#
#   .\tools\make_variants.ps1                     默认生成 <大版本>.0/.1/.2 与 .99-crash
#   .\tools\make_variants.ps1 -BaseVer 0.7
#   .\tools\make_variants.ps1 -IdfPath <IDF目录> -IdfToolsPath <工具目录>
#
# 用途: 验证切槽 / 首启校验窗口 / 崩溃自动回退时需要"版本号不同、内容相同"的镜像。
# 产物落在 build\fw_bins\ (派生文件, 可随时重建; build 目录被清掉不影响本脚本)。
# 末尾会重新构建一次基础版本, 保证 build\music_nano.bin 回到常规产物。
param(
    [string]$BaseVer = "0.7",
    [string]$IdfPath = "",
    [string]$IdfToolsPath = "",
    [string]$OutDir = ""
)

$ErrorActionPreference = 'Stop'

$PROJ = Split-Path -Parent $PSScriptRoot
Set-Location $PROJ
if (-not $OutDir) { $OutDir = Join-Path $PROJ 'build\fw_bins' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

function Build-One([string]$ver, [switch]$TestCrash) {
    $p = @{ Ver = $ver }
    if ($IdfPath)      { $p.IdfPath      = $IdfPath }
    if ($IdfToolsPath) { $p.IdfToolsPath = $IdfToolsPath }
    if ($TestCrash)    { $p.TestCrash    = $true }
    $tag = if ($TestCrash) { ' (启动即崩)' } else { '' }
    Write-Host "=== 构建 v$ver$tag ===" -ForegroundColor Cyan
    & (Join-Path $PROJ 'build.ps1') @p
    if ($LASTEXITCODE -ne 0) { throw "build failed: $ver" }
    $name = if ($TestCrash) { "v$ver-crash.bin" } else { "v$ver.bin" }
    Copy-Item (Join-Path $PROJ 'build\music_nano.bin') (Join-Path $OutDir $name) -Force
}

Build-One "$BaseVer.0"
Build-One "$BaseVer.1"
Build-One "$BaseVer.2"
Build-One "$BaseVer.99" -TestCrash
Build-One "$BaseVer.0"          # 恢复常规构建

Write-Host ''
Write-Host '=== 归档 ==='
Get-ChildItem $OutDir -Filter '*.bin' | Select-Object Name, Length | Format-Table -AutoSize
