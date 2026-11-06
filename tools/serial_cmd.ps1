# 串口助手: 给设备控制台发一条命令并收集输出
#
#   .\tools\serial_cmd.ps1                       只听
#   .\tools\serial_cmd.ps1 -Cmd "ota"            发一条命令
#   .\tools\serial_cmd.ps1 -Port COM5 -Cmd "ota boot 1"
#   .\tools\serial_cmd.ps1 -Cmd "stats" -WaitMs 5000
#
# 不传 -Port 时: 系统里只有一个串口就用它, 有多个则要求显式指定。
# 日志是 UTF-8, 必须设 Encoding, 否则中文会变成 '?'。
param(
    [string]$Cmd = "",
    [int]$WaitMs = 2500,
    [string]$Port = "",
    [int]$Baud = 921600
)

$ErrorActionPreference = 'Stop'

if (-not $Port) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports.Count -eq 0) { Write-Host "没有可用串口" -ForegroundColor Red; exit 2 }
    if ($ports.Count -gt 1) {
        Write-Host ("有多个串口: " + ($ports -join ', ') + " —— 用 -Port 指定") -ForegroundColor Yellow
        exit 2
    }
    $Port = $ports[0]
}

$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, 'None', 8, 'One')
$sp.ReadTimeout = 100
$sp.Encoding = [System.Text.Encoding]::UTF8
try { $sp.DtrEnable = $false; $sp.RtsEnable = $false } catch { }
try {
    $sp.Open()
} catch {
    Write-Host "打开 $Port 失败: $($_.Exception.Message)" -ForegroundColor Red
    exit 2
}

Start-Sleep -Milliseconds 150
$sp.DiscardInBuffer()

if ($Cmd -ne "") { $sp.Write($Cmd + "`r") }

$sb = New-Object System.Text.StringBuilder
$end = (Get-Date).AddMilliseconds($WaitMs)
while ((Get-Date) -lt $end) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk) { [void]$sb.Append($chunk) }
    } catch { }
    Start-Sleep -Milliseconds 40
}
$sp.Close()

$out = $sb.ToString()
if ($out.Trim().Length -eq 0) {
    Write-Host "(无输出)" -ForegroundColor DarkGray
} else {
    Write-Output $out
}
