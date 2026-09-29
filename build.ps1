$ErrorActionPreference = "Stop"

$env:IDF_PATH           = "C:\Users\123\esp\v5.5.5\esp-idf"
$env:IDF_PYTHON_ENV_PATH = "d:\espressif\python_env\idf5.5_py3.11_env"
$env:IDF_TOOLS_PATH     = "d:\espressif"
$env:ESP_ROM_ELF_DIR    = "d:\espressif\tools\esp-rom-elfs\20241011"

$TOOLS = "d:\espressif\tools"
$env:PATH = "$TOOLS\ccache\4.11.2\ccache-4.11.2-windows-x86_64;$TOOLS\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin;$TOOLS\ninja\1.12.1;$TOOLS\cmake\3.30.2\bin;$TOOLS\idf-git\2.39.2\cmd;$env:IDF_PYTHON_ENV_PATH\Scripts;$env:PATH"

$BUILD_DIR = Join-Path $PSScriptRoot "build"
$PROJ_DIR  = $PSScriptRoot

$regenerate = $false
if (-not (Test-Path "$BUILD_DIR\build.ninja")) { $regenerate = $true }
else {
    $src_mtime = (Get-ChildItem "$PROJ_DIR\CMakeLists.txt", "$PROJ_DIR\main\CMakeLists.txt", "$PROJ_DIR\sdkconfig.defaults" | Sort-Object LastWriteTime -Descending)[0].LastWriteTime
    $build_mtime = (Get-Item "$BUILD_DIR\build.ninja").LastWriteTime
    if ($src_mtime -gt $build_mtime) { $regenerate = $true }
}

$PYTHON_EXE = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"

if ($regenerate) {
    Write-Host "=== CMake 配置中... ===" -ForegroundColor Cyan
    & "$TOOLS\cmake\3.30.2\bin\cmake.exe" -S "$PROJ_DIR" -B "$BUILD_DIR" -G Ninja `
        -DCMAKE_MAKE_PROGRAM="$TOOLS\ninja\1.12.1\ninja.exe" `
        -DPYTHON_DEPS_CHECKED=1 `
        -DPYTHON="$PYTHON_EXE" `
        -DESP_PLATFORM=1 `
        -DCCACHE_ENABLE=0 2>&1 | ForEach-Object {
        if ($_ -match "error|fatal|Error|CMake Error") { Write-Host $_ -ForegroundColor Red }
        elseif ($_ -match "warning|Warning") { Write-Host $_ -ForegroundColor Yellow }
        elseif ($_ -match "-- Configuring done|-- Generating done|-- Build files") { Write-Host $_ -ForegroundColor Green }
    }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# === 开始编译 ===
Write-Host "=== 开始编译... ===" -ForegroundColor Cyan

$err_count = 0
$warn_count = 0
$step = 0
$total = 0

Push-Location "$BUILD_DIR"
try {
    & "$TOOLS\ninja\1.12.1\ninja.exe" 2>&1 | ForEach-Object {
    $line = $_

    if ($line -match '^\[(\d+)/(\d+)\]') {
        $step = [int]$matches[1]
        $total = [int]$matches[2]
    }

    if ($line -match "error:|fatal error|undefined reference|cannot find") {
        Write-Host $line -ForegroundColor Red
        $err_count++
    }
    elseif ($line -match "warning:") {
        Write-Host $line -ForegroundColor Yellow
        $warn_count++
    }
    elseif ($line -match "FAILED|ninja:") {
        Write-Host $line -ForegroundColor Red
    }
    elseif ($line -match "binary size") {
        Write-Host $line -ForegroundColor Green
    }
    }
} finally {
    Pop-Location
}

if ($LASTEXITCODE -eq 0) {
    Write-Host "=== 编译成功 ===" -ForegroundColor Green
} else {
    Write-Host "=== 编译失败 (错误: $err_count, 警告: $warn_count) ===" -ForegroundColor Red
    exit $LASTEXITCODE
}
