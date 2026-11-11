# 核对 "sdkconfig.defaults 里写的" 和 "这次实际编译用的" 是否一致
#
# 为什么需要这个脚本
# ------------------
# idf.py 只读 sdkconfig. sdkconfig.defaults 的作用是"当 sdkconfig 不存在时,
# 用它生成一份", 一旦 sdkconfig 里已经有这个键, defaults 里的值就被忽略.
# 于是会出现这种极难发现的坑: 在 defaults 里改了配置 → 编译成功 → 固件行为没变,
# 因为实际生效的是 sdkconfig. (本工程真踩过: CONFIG_LWIP_DHCP_DOES_ARP_CHECK
# 一直以为关掉了, 实际每一版固件都是 y, 白查了半天 DHCP.)
#
# 用法
# ----
#   .\tools\check_sdkconfig.ps1          # 在工程根目录或任意目录调用都行
#   build.ps1 在编译成功后会自动调用, 不一致直接返回非 0.
#
# 判据
# ----
# 以 build/config/sdkconfig.h 为准 (它就是编进固件的那些宏):
#   #define CONFIG_X 1       → y
#   #define CONFIG_X 4       → 4
#   #define CONFIG_X "abc"   → "abc"
#   没有该行 / "// #define CONFIG_X" → 未设置 (对应 defaults 里的 n)

$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
$defaults = Join-Path $root 'sdkconfig.defaults'
$header   = Join-Path $root 'build\config\sdkconfig.h'

if (-not (Test-Path -LiteralPath $defaults)) {
    Write-Host "找不到 sdkconfig.defaults: $defaults" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path -LiteralPath $header)) {
    Write-Host "找不到 build/config/sdkconfig.h —— 先编译一次" -ForegroundColor Yellow
    exit 1
}

# 实际编译配置: 宏名 → 值 ("未设置" 记为空串)
$actual = @{}
foreach ($m in Select-String -Path $header -Pattern '^#define\s+(CONFIG_\w+)\s+(.+?)\s*$') {
    $actual[$m.Matches[0].Groups[1].Value] = $m.Matches[0].Groups[2].Value
}
foreach ($m in Select-String -Path $header -Pattern '^//\s*#define\s+(CONFIG_\w+)') {
    $actual[$m.Matches[0].Groups[1].Value] = ''
}

$checked = 0
$bad     = @()

foreach ($line in Get-Content -LiteralPath $defaults) {
    # 只认 "KEY=VALUE", 注释行 (# 开头) 整行跳过
    if ($line -notmatch '^\s*(CONFIG_\w+)\s*=\s*(\S+)\s*$') { continue }
    $name = $Matches[1]
    $want = $Matches[2]
    $checked++

    $got = if ($actual.ContainsKey($name)) { $actual[$name] } else { '' }

    # y/n 在 sdkconfig.h 里是 "有 #define 1 / 没有", 其余按字面比 (去掉引号)
    $ok = $false
    if ($want -eq 'y') {
        $ok = ($got -eq '1')
    } elseif ($want -eq 'n') {
        $ok = ($got -eq '')
    } else {
        $w = $want.Trim('"')
        $g = $got.Trim('"')
        $ok = ($w -eq $g)
    }

    if (-not $ok) {
        $showWant = $want
        $showGot  = if ($got -eq '') { '未设置' } else { $got }
        $bad += [pscustomobject]@{ Name = $name; Want = $showWant; Got = $showGot }
    }
}

if ($bad.Count -eq 0) {
    Write-Host ("配置核对: sdkconfig.defaults 的 $checked 项与实际编译配置一致") -ForegroundColor Green
    exit 0
}

Write-Host ("配置核对: $checked 项里有 $($bad.Count) 项没生效 (defaults 写了, 固件里不是这个值)") -ForegroundColor Red
Write-Host "原因: idf.py 只读 sdkconfig, defaults 只在生成新配置时用. 改 sdkconfig 本身才是有效的." -ForegroundColor Yellow
foreach ($b in $bad) {
    Write-Host ("  {0,-52} 期望={1,-8} 实际={2}" -f $b.Name, $b.Want, $b.Got) -ForegroundColor Red
}
exit 1
