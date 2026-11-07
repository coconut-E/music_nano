# 分步烧录脚本: 把需要的镜像 + flash_args 摆到 build\fw\ 下, 再调一键烧录.exe
#
# 为什么要这样: 一键烧录.exe 会在自己所在目录 (及其子目录) 找 flash_args,
# 按里面的 "地址 文件名" 逐个烧写, 并且找不到带路径的名字时会回退到 basename
# (src/music_flash.c 的 add_part)。所以不用改 exe, 只要给它一份自己的清单。
#
# 用法:
#   .\tools\flash_step.ps1 once           整片擦除 + 全套 (换分区表后必须来一次)
#   .\tools\flash_step.ps1 base           全套但不擦除 (保留 NVS 设置)
#   .\tools\flash_step.ps1 base -NoFont   只刷 bootloader/分区表/app/otadata (省 4 分钟)
#   .\tools\flash_step.ps1 app2 -Slot 1 -App2 build\fw_bins\v0.7.1.bin   只写 ota_1
#   .\tools\flash_step.ps1 font           只写字库分区
#   .\tools\flash_step.ps1 raw -Addr 0x6A0000 -File build\fw\font_b.bin   调试: 单文件单地址
#   .\tools\flash_step.ps1 erase -Addr 0x4D0000 -Len 0x10000              调试: 只擦一段 (T6 用)
#   .\tools\flash_step.ps1 probe          只连设备看信息, 不烧
#
# 注意: 这套脚本只做"首次全量烧录"和测试, 不是升级手段。
#       日常升级走固件内的网络 OTA (app 自己下载 .bin 写进 ota_1/ota_0)。
#
# 实测这颗 Puya 8MB 擦除很慢 (约 14KB/s): 全套 5MB 要 6~8 分钟,
# 单独 app 1.7MB 约 2 分钟, 字库 3.2MB 约 4 分钟。测试时尽量用 -NoFont。

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('once', 'base', 'app2', 'font', 'raw', 'probe', 'erase')]
    [string]$Step,

    [string]$App2,                                                    # app2 镜像 (默认 build\music_nano.bin)
    [ValidateSet(0, 1)]
    [int]$Slot = 1,                                                   # app2 目标槽: 0=ota_0(0x30000) 1=ota_1(0x280000)
    [switch]$NoFont,                                                  # base 时跳过字库
    [string]$Exe = '',                                                  # 烧录器 (默认仓库内 tools\flash_tool)
    [switch]$Probe,                                                   # 只探测不烧
    [int]$Baud = 921600,

    [string]$Addr = '0x4D0000',                                       # raw / erase 的地址
    [string]$Len = '0x10000',                                         # erase 擦除长度
    [string]$File                                                     # raw 步骤的文件
)

$ErrorActionPreference = 'Stop'

$PROJ = Split-Path -Parent $PSScriptRoot
$BUILD = Join-Path $PROJ 'build'
$FW = Join-Path $BUILD 'fw'

# 烧录器默认用仓库内那份 (tools\flash_tool 由 tools\flash_tool\build.ps1 编译)
if (-not $Exe) { $Exe = Join-Path $PROJ 'tools\flash_tool\out\music_flash.exe' }

# ── 镜像源 ──
$SRC = @{
    boot  = Join-Path $BUILD 'bootloader\bootloader.bin'
    table = Join-Path $BUILD 'partition_table\partition-table.bin'
    app   = Join-Path $BUILD 'music_nano.bin'
    ota   = Join-Path $BUILD 'ota_data_initial.bin'
    font  = Join-Path $PROJ 'main\resources\font16.bin'
}

if (-not (Test-Path $Exe)) {
    Write-Host "找不到烧录器: $Exe" -ForegroundColor Red
    Write-Host "用 -Exe <路径> 指定, 或先编译 flash_tool (build.ps1)" -ForegroundColor Yellow
    exit 1
}

# ── 地址工具: 接受 0x4A0000 字符串或整数, 统一输出 0x%06X ──
function AddrNum([object]$v) {
    if ($v -is [string]) { return [uint32]::Parse(($v -replace '^0[xX]', ''), 'HexNumber') }
    return [uint32]$v
}
function AddrStr([object]$v) { return ('0x{0:X6}' -f (AddrNum $v)) }

# ── 工作目录: 每次重建, 避免残留旧镜像 ──
if (Test-Path $FW) { Remove-Item -Recurse -Force $FW }
New-Item -ItemType Directory -Path $FW -Force | Out-Null
Copy-Item $Exe (Join-Path $FW 'music_flash.exe')

function Copy-Mirror($src, $dstName) {
    if (-not (Test-Path $src)) {
        Write-Host "缺镜像: $src" -ForegroundColor Red
        exit 1
    }
    Copy-Item $src (Join-Path $FW $dstName) -Force
    Write-Host ("  {0,-22} {1,10:N0} B" -f $dstName, (Get-Item $src).Length)
}

$toolArgs = @('/nopause', '/baud', "$Baud")

switch ($Step) {

    'probe' {
        # 探测模式也要摆一份"本地分区表": 烧录器拿它和设备里的分区表逐字节比对,
        # 才能在 /probe 里报"一致 / 不同 (需要先擦除)"。只放这一个文件, 不碰 flash。
        # (以前这里什么都不摆, 脚本后面读 flash_args 时会直接抛 PathNotFound)
        # 这里直接覆盖整个数组, 别漏掉 /nopause —— 漏了 probe 会卡在"按任意键结束"
        $toolArgs = @('/nopause', '/probe', '/baud', "$Baud")
        Write-Host "=== 只探测设备 ===" -ForegroundColor Cyan
        Copy-Mirror $SRC.table 'partition-table.bin'
        @('0x008000 partition-table.bin') | Set-Content (Join-Path $FW 'flash_args') -Encoding ASCII
    }

    'once' {
        Write-Host "=== 整片擦除 + 全套烧录 (约 6-8 分钟) ===" -ForegroundColor Cyan
        Copy-Mirror $SRC.boot  'bootloader.bin'
        Copy-Mirror $SRC.table 'partition-table.bin'
        Copy-Mirror $SRC.app   'app.bin'
        Copy-Mirror $SRC.ota   'ota_data_initial.bin'
        $lines = @(
            '0x001000 bootloader.bin',
            '0x008000 partition-table.bin',
            '0x02E000 ota_data_initial.bin',
            '0x030000 app.bin'
        )
        Copy-Mirror $SRC.font 'font.bin'
        $lines += '0x4D0000 font.bin'
        $lines | Set-Content (Join-Path $FW 'flash_args') -Encoding ASCII
        $toolArgs += '/erase'
    }

    'base' {
        Write-Host "=== 烧 base (不擦除, 保留 NVS) ===" -ForegroundColor Cyan
        Copy-Mirror $SRC.boot  'bootloader.bin'
        Copy-Mirror $SRC.table 'partition-table.bin'
        Copy-Mirror $SRC.app   'app.bin'
        Copy-Mirror $SRC.ota   'ota_data_initial.bin'
        $lines = @(
            '0x001000 bootloader.bin',
            '0x008000 partition-table.bin',
            '0x02E000 ota_data_initial.bin',
            '0x030000 app.bin'
        )
        if (-not $NoFont) {
            Copy-Mirror $SRC.font 'font.bin'
        $lines += '0x4D0000 font.bin'
        } else {
            Write-Host "  (跳过字库分区)" -ForegroundColor DarkGray
        }
        $lines | Set-Content (Join-Path $FW 'flash_args') -Encoding ASCII
        # /noerase: 分区表即便不同也不整片擦, 但会清 0x8000-0x9000 旧表 + 0x1F000-0x30000
        # 的旧 otadata/phy_init, 0x9000-0x1F000 的 nvs 用户数据保留
        $toolArgs += '/noerase'
    }

    'app2' {
        # 注意: 变量名不能叫 $src —— PowerShell 变量名不区分大小写, 会和上面的
        # 镜像源哈希表 $SRC 撞名, 把整张表覆盖掉 (然后 $SRC.app 就是 null)
        $appImg = $App2
        if (-not $appImg) { $appImg = $SRC.app }
        $target = if ($Slot -eq 0) { '0x030000' } else { '0x280000' }
        Write-Host "=== 只写 ota_$Slot ($target) ===" -ForegroundColor Cyan
        Write-Host "  !! 这是测试用通道, 不是升级手段 (升级走网络 OTA)" -ForegroundColor Yellow
        Write-Host "  !! 目标槽若正是设备当前运行的槽, 会覆盖正在执行的代码, 当场崩溃" -ForegroundColor Yellow
        Write-Host "  !! 先用串口命令 ota 看 '运行槽', 确认不是它再往下走" -ForegroundColor Yellow
        Copy-Mirror $appImg 'app2.bin'
        @("$target app2.bin") | Set-Content (Join-Path $FW 'flash_args') -Encoding ASCII
        $toolArgs += '/noerase'
    }

    'erase' {
        # 只擦一段, 不写任何东西 (T6 擦字库分区这类调试用)。
        # 仍然要摆一份合法清单: 烧录器连机前会校验清单里的地址。
        Write-Host "=== 只擦除 $(AddrStr $Addr) 长度 $(AddrStr $Len) ===" -ForegroundColor Cyan
        Copy-Mirror $SRC.boot 'bootloader.bin'
        @('0x001000 bootloader.bin') | Set-Content (Join-Path $FW 'flash_args') -Encoding ASCII
        $toolArgs += '/erase-region', (AddrStr $Addr), (AddrStr $Len)
    }

    'font' {
        Write-Host "=== 只写字库分区 (0x4D0000) ===" -ForegroundColor Cyan
        Copy-Mirror $SRC.font 'font.bin'
        $lines = @('0x4D0000 font.bin')
        $lines | Set-Content (Join-Path $FW 'flash_args') -Encoding ASCII
        $toolArgs += '/noerase'
    }

    'raw' {
        if (-not $File) {
            Write-Host "raw 需要 -File <镜像> [-Addr 0x...]" -ForegroundColor Red
            exit 1
        }
        Write-Host "=== 单文件烧录: $File -> $(AddrStr $Addr) ===" -ForegroundColor Cyan
        Copy-Mirror $File 'raw.bin'
        @("$(AddrStr $Addr) raw.bin") | Set-Content (Join-Path $FW 'flash_args') -Encoding ASCII
        $toolArgs += '/noerase'
    }
}

if ($Probe) { $toolArgs += '/probe' }

Write-Host ""
Write-Host "  flash_args:" -ForegroundColor DarkGray
Get-Content (Join-Path $FW 'flash_args') | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
Write-Host ""

Push-Location $FW
try {
    # 从 stdin 送一个换行: /nopause 已经能跳过按键, 但万一走到提问分支也不会把
    # 自动化流程卡死 (_getch 在无控制台时会一直阻塞)
    '' | & '.\music_flash.exe' @toolArgs
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -eq 0) {
    Write-Host "=== 烧录完成 ===" -ForegroundColor Green
} else {
    Write-Host "=== 烧录失败 (exit $code) ===" -ForegroundColor Red
}
exit $code
