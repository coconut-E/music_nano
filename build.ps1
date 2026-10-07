param(
    [switch]$Clean,
    [string]$IdfPath,        # 可选: 手动指定 ESP-IDF 根目录 (含 export.ps1)
    [string]$IdfToolsPath    # 可选: 手动指定 IDF 工具安装目录 (IDF_TOOLS_PATH)
)

$ErrorActionPreference = "Stop"

# 通过 idf.py 构建，并过滤掉逐文件编译进度等噪声，只保留警告/错误/链接结果。
# 一步完成：若当前 shell 未导出 ESP-IDF 环境，脚本会自动探测并导入（见下方）,
# 因此无需手动执行 export.ps1。探测优先使用系统里已有的 ESP-IDF 安装记录，
# 不在脚本中硬编码任何本机路径。

# ───────────────────────── 环境探测 ─────────────────────────

# 环境是否已就绪: IDF_PATH 指向有效的 esp-idf, 或 idf.py 已在 PATH 中
function Test-IdfReady {
    $byPath    = $env:IDF_PATH -and (Test-Path -LiteralPath (Join-Path $env:IDF_PATH "tools/idf.py"))
    $byCommand = [bool](Get-Command idf.py -ErrorAction SilentlyContinue)
    return ($byPath -or $byCommand)
}

function Read-JsonFile([string]$path) {
    try { return (Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json) } catch { return $null }
}

# 收集可能的 ESP-IDF 安装记录 (esp_idf.json) 位置
function Get-ConfigCandidates {
    $list = New-Object System.Collections.Generic.List[string]

    if ($env:IDF_TOOLS_PATH) { $list.Add((Join-Path $env:IDF_TOOLS_PATH "esp_idf.json")) }
    if ($env:USERPROFILE)    { $list.Add((Join-Path $env:USERPROFILE ".espressif/esp_idf.json")) }
    if ($env:APPDATA)        { $list.Add((Join-Path $env:APPDATA "Code/User/globalStorage/espressif.esp-idf-extension/esp_idf.json")) }
    if ($env:LOCALAPPDATA)   { $list.Add((Join-Path $env:LOCALAPPDATA "espressif/esp_idf.json")) }

    # 统一安装器的常见根目录 (Espressif 默认名, 非本机专属路径)
    foreach ($drv in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        if ($drv.Root -and (Test-Path -LiteralPath $drv.Root)) {
            $list.Add((Join-Path $drv.Root "Espressif/esp_idf.json"))
            $list.Add((Join-Path $drv.Root "esp_idf.json"))
        }
    }
    return $list
}

# 从 esp_idf.json 安装记录中解析出 IDF 根目录 / 工具目录 / 虚拟环境 python
function Resolve-IdfFromConfig {
    foreach ($cfgPath in (Get-ConfigCandidates)) {
        if (-not (Test-Path -LiteralPath $cfgPath)) { continue }
        $cfg = Read-JsonFile $cfgPath
        if (-not $cfg -or -not $cfg.idfInstalled) { continue }

        $installed = $cfg.idfInstalled
        $sel = $null

        # 1) 优先使用记录里选中的那一个
        if ($cfg.idfSelectedId -and ($installed.PSObject.Properties.Name -contains $cfg.idfSelectedId)) {
            $sel = $installed.$($cfg.idfSelectedId)
        }
        # 2) 否则取第一个路径存在的
        if (-not $sel) {
            foreach ($prop in $installed.PSObject.Properties) {
                if ($prop.Value.path -and (Test-Path -LiteralPath $prop.Value.path)) { $sel = $prop.Value; break }
            }
        }
        if (-not $sel -or -not $sel.path) { continue }

        if (Test-Path -LiteralPath (Join-Path $sel.path "export.ps1")) {
            return [pscustomobject]@{
                Idf    = $sel.path
                Tools  = $cfg.idfToolsPath
                Python = $sel.python
            }
        }
    }
    return $null
}

# 未导出环境时: 探测 → 设置 IDF_TOOLS_PATH / IDF_PYTHON_ENV_PATH → 导入 export.ps1
function Import-IdfEnvironment {
    $resolved = $null

    if ($IdfPath) {
        $resolved = [pscustomobject]@{ Idf = $IdfPath; Tools = $IdfToolsPath; Python = $null }
    } else {
        $resolved = Resolve-IdfFromConfig
    }

    if (-not $resolved) { return $false }

    if ($resolved.Tools) { $env:IDF_TOOLS_PATH = $resolved.Tools }
    if ($resolved.Python) {
        # <venv>\Scripts\python.exe → <venv>
        $venv = Split-Path -Parent (Split-Path -Parent $resolved.Python)
        if (Test-Path -LiteralPath $venv) { $env:IDF_PYTHON_ENV_PATH = $venv }
    }

    $exportScript = Join-Path $resolved.Idf "export.ps1"
    Write-Host "== 导入 ESP-IDF 环境: $($resolved.Idf) ==" -ForegroundColor Cyan
    . $exportScript
    return (Test-IdfReady)
}

if (-not (Test-IdfReady)) {
    if (-not (Import-IdfEnvironment)) {
        Write-Host "未找到可用的 ESP-IDF 环境。" -ForegroundColor Yellow
        Write-Host "请先安装 ESP-IDF (>=5.5.5), 或手动导出环境后再运行:" -ForegroundColor Yellow
        Write-Host "  . <IDF_DIR>\export.ps1" -ForegroundColor Yellow
        Write-Host "也可直接指定路径:  .\build.ps1 -IdfPath <IDF_DIR> [-IdfToolsPath <TOOLS_DIR>]" -ForegroundColor Yellow
        exit 1
    }
}

# ───────────────────────── 构建 ─────────────────────────

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
