# 复位设备并抓启动日志 (用于看 bootloader / 应用日志)
#
#   .\tools\reset_log.ps1                      复位并抓 9 秒
#   .\tools\reset_log.ps1 -Port COM5 -WaitMs 15000
#
# 本板串口转接: RTS -> EN, DTR -> IO0。
#   RTS=1 拉低 EN = 复位;  DTR=1 拉低 IO0 = 下载模式。
# 所以"正常启动复位"必须 DTR=0 (IO0 高) 只给 RTS 打脉冲;
# 两个一起动会进下载模式, 看起来像"设备没起来"。
#
# 注意: 设备默认 2 分钟无操作就深睡, 深睡时串口没输出 —— 复位一下即可。
param(
    [string]$Port = "",
    [int]$Baud = 921600,
    [int]$WaitMs = 9000
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
$sp.Open()
Start-Sleep -Milliseconds 150
$sp.DiscardInBuffer()

$sp.DtrEnable = $false          # IO0 = HIGH
$sp.RtsEnable = $true           # EN  = LOW  (复位)
Start-Sleep -Milliseconds 100
$sp.RtsEnable = $false          # EN  = HIGH (正常启动)
Start-Sleep -Milliseconds 50

$sb = New-Object System.Text.StringBuilder
$end = (Get-Date).AddMilliseconds($WaitMs)
while ((Get-Date) -lt $end) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk) { [void]$sb.Append($chunk) }
    } catch { }
    Start-Sleep -Milliseconds 30
}
$sp.Close()

$out = $sb.ToString()
if ($out.Trim().Length -eq 0) { Write-Host "(无输出 —— 多半在深睡, 或 IO0 被拉低了)" } else { Write-Output $out }
