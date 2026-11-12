# 生成"更新服务器"的证书链, 并拼出设备端要 pin 的 CA 文件
#
# 干三件事:
#   1) 本地 CA (10 年, RSA 2048) —— 只用于局域网调试服务器, 私钥不进仓库
#   2) 服务器证书 (1 年) —— SAN 里写当前局域网 IP + DNS:music-nano.lan;
#      换 IP 只需要重跑本脚本 (设备端 pin 的是 CA, 不用重编固件)
#   3) main/certs/ota_ca.pem = 本地 CA + ISRG Root X1 + ISRG Root X2
#      —— mbedtls 支持一个 PEM buffer 里放多张证书, 所以"局域网调试服务器"和
#         "gh-proxy (Let's Encrypt 系)" 共用一个固件, 换服务器不用改代码
#
# 用法:
#   .\tools\ota_cert.ps1                 # 缺什么补什么 (幂等)
#   .\tools\ota_cert.ps1 -Force          # 服务器证书强制重签 (CA 不动)
#   .\tools\ota_cert.ps1 -Force -NewCa   # CA 也重来 (设备要跟着重新编译)
#
# 依赖: openssl 在 PATH 里 (本机是 E:\msys64\mingw64\bin\openssl.exe)
# 产物:
#   tools/ota_cert/ca.pem  ca.key        (key 已 gitignore)
#   tools/ota_cert/server.crt server.key server.ip
#   tools/ota_cert/isrg.pem              两张 Let's Encrypt 根证书 (已提交, 静态)
#   main/certs/ota_ca.pem                进固件, 需要提交
#
# 注意: 这张 CA 的私钥是明文放在 tools/ota_cert/ 里的, 只用于局域网自签,
# 不要拿它签任何对外服务的东西. 真正上线走 gh-proxy/Let's Encrypt, 用不到它.
#
# isrg.pem 是从 letsencrypt.org 拉下来的公共根证书 (X1 到 2035-06-04,
# X2 到 2040-09-17), 不需要每次重新生成; 丢了就重新下一份放回去:
#   https://letsencrypt.org/certs/isrgrootx1.pem
#   https://letsencrypt.org/certs/isrg-root-x2.pem

[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$NewCa
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot        # 仓库根目录
$dir   = Join-Path $PSScriptRoot 'ota_cert'
$certs = Join-Path $root 'main\certs'
New-Item -ItemType Directory -Force -Path $dir, $certs | Out-Null

function Find-OpenSSL {
    $c = Get-Command openssl -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @('C:\msys64\mingw64\bin\openssl.exe', 'C:\Program Files\Git\usr\bin\openssl.exe')) {
        if (Test-Path $p) { return $p }
    }
    throw '找不到 openssl, 请装一个或把路径加进 PATH'
}

$ossl = Find-OpenSSL
Write-Host "openssl: $ossl" -ForegroundColor DarkGray

# 现在这个网段的 IPv4 (挑有默认网关的那张网卡)
function Get-LanIp {
    try {
        $cfg = Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -ne $null } | Select-Object -First 1
        if ($cfg) { return $cfg.IPv4Address.IPAddress }
    } catch { }
    try {
        $addrs = [System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName())
        $v4 = $addrs | Where-Object { $_.AddressFamily -eq 'InterNetwork' -and $_.IPAddressToString -notlike '127.*' } | Select-Object -First 1
        if ($v4) { return $v4.IPAddressToString }
    } catch { }
    return '127.0.0.1'
}

$lanIp = Get-LanIp

# ── 1) 本地 CA ─────────────────────────────────────────────
$caKey = Join-Path $dir 'ca.key'
$caPem = Join-Path $dir 'ca.pem'

if ($NewCa -or -not (Test-Path $caKey) -or -not (Test-Path $caPem)) {
    Write-Host '生成本地 CA (10 年)...' -ForegroundColor Cyan
    & $ossl genrsa -out $caKey 2048
    if ($LASTEXITCODE -ne 0) { throw 'genrsa 失败 (CA)' }
    & $ossl req -x509 -new -key $caKey -sha256 -days 3650 -out $caPem -subj '/CN=music_nano dev CA/O=music_nano' -addext 'basicConstraints=critical,CA:TRUE,pathlen:0' -addext 'keyUsage=critical,keyCertSign,cRLSign'
    if ($LASTEXITCODE -ne 0) { throw '自签 CA 失败' }
}

# ── 2) 服务器证书 (SAN 带当前 IP) ──────────────────────────
$srvKey = Join-Path $dir 'server.key'
$srvCrt = Join-Path $dir 'server.crt'
$srvIp  = Join-Path $dir 'server.ip'       # 上次签发时用的 IP (比解析 SAN 简单可靠)
$san    = "subjectAltName=DNS:music-nano.lan,IP:$lanIp,IP:127.0.0.1"

$needSign = $Force
if (-not (Test-Path $srvKey) -or -not (Test-Path $srvCrt) -or -not (Test-Path $srvIp)) {
    $needSign = $true
} elseif (((Get-Content $srvIp -Raw).Trim()) -ne $lanIp) {
    Write-Host "局域网 IP 变了 ($((Get-Content $srvIp -Raw).Trim()) → $lanIp), 重签服务器证书" -ForegroundColor Yellow
    $needSign = $true
}

if ($needSign) {
    Write-Host "签发服务器证书 (IP=$lanIp)..." -ForegroundColor Cyan
    $csr = Join-Path $dir 'server.csr'
    $cnf = Join-Path $dir 'server.ext'
    & $ossl genrsa -out $srvKey 2048
    if ($LASTEXITCODE -ne 0) { throw 'genrsa 失败 (server)' }
    & $ossl req -new -key $srvKey -out $csr -subj '/CN=music-nano.lan/O=music_nano'
    if ($LASTEXITCODE -ne 0) { throw '生成 CSR 失败' }
    @(
        $san
        'basicConstraints=critical,CA:FALSE'
        'keyUsage=critical,digitalSignature,keyEncipherment'
        'extendedKeyUsage=serverAuth'
    ) | Set-Content -Encoding ascii $cnf
    & $ossl x509 -req -in $csr -CA $caPem -CAkey $caKey -CAcreateserial -out $srvCrt -days 365 -sha256 -extfile $cnf
    if ($LASTEXITCODE -ne 0) { throw '签发服务器证书失败' }
    Remove-Item $csr, $cnf -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText($srvIp, $lanIp)
} else {
    Write-Host "服务器证书已包含 IP $lanIp, 复用" -ForegroundColor DarkGray
}

# ── 3) 拼 ota_ca.pem: 本地 CA + ISRG Root X1/X2 ────────────
# ISRG 那两张是静态的公共根证书 (已提交在 tools/ota_cert/isrg.pem),
# 脚本不联网也不依赖 node —— 沙箱里 node 起不来, 而这是构建依赖, 必须离线可跑
$isrg = Join-Path $dir 'isrg.pem'
if (-not (Test-Path $isrg)) {
    Write-Host "缺少 $isrg —— 只 pin 本地 CA, 这样连不上 gh-proxy" -ForegroundColor Yellow
}

$out = Join-Path $certs 'ota_ca.pem'
$parts = @((Get-Content $caPem -Raw).Trim())
if (Test-Path $isrg) { $parts += ((Get-Content $isrg -Raw).Trim()) }
[System.IO.File]::WriteAllText($out, (($parts -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))

$n = ([regex]::Matches((Get-Content $out -Raw), 'BEGIN CERTIFICATE')).Count
Write-Host "已写入 $out ($n 张证书)" -ForegroundColor Green
Write-Host "局域网调试服务器: https://${lanIp}:8443" -ForegroundColor Green
