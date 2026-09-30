param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

# 通过 idf.py 构建，并过滤掉逐文件编译进度等噪声，只保留警告/错误/链接结果。
# 前置条件：已导出 ESP-IDF 环境（IDF_PATH 及工具链已在 PATH 中），例如：
#   . $env:IDF_PATH\export.ps1
if (-not $env:IDF_PATH) {
    Write-Host "未检测到 IDF_PATH。请先导出 ESP-IDF 环境，例如：" -ForegroundColor Yellow
    Write-Host '  . $env:IDF_PATH\export.ps1   # 或 . <你的 esp-idf 路径>\export.ps1'
    exit 1
}

$PROJ_DIR  = $PSScriptRoot
$BUILD_DIR = Join-Path $PROJ_DIR "build"

if ($Clean -and (Test-Path $BUILD_DIR)) {
    Write-Host "=== 清空 build/ ===" -ForegroundColor Cyan
    Remove-Item -Recurse -Force $BUILD_DIR
}

Write-Host "=== 开始编译 ($PROJ_DIR) ===" -ForegroundColor Cyan

$code = 1
Push-Location $PROJ_DIR
try {
    & idf.py build 2>&1 | ForEach-Object {
        $line = $_
        if ($line -match "error:|fatal error|undefined reference|cannot find|FAILED|ninja: build stopped") {
            Write-Host $line -ForegroundColor Red
        }
        elseif ($line -match "warning:") {
            Write-Host $line -ForegroundColor Yellow
        }
        elseif ($line -match "binary size|Project build complete|Successfully created|To flash|ELF file") {
            Write-Host $line -ForegroundColor Green
        }
    }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -eq 0) {
    Write-Host "=== 编译成功 ===" -ForegroundColor Green
} else {
    Write-Host "=== 编译失败 (exit $code) ===" -ForegroundColor Red
    exit $code
}
