param(
    [string]$ProjectDir = $PSScriptRoot,
    [string]$IdfTools = $env:IDF_TOOLS_PATH,
    [string]$Target = "esp32",
    [int]$Last = 30
)

$ErrorActionPreference = "Stop"

if (-not $IdfTools) {
    Write-Error "IDF_TOOLS_PATH 未设置。请先导出 ESP-IDF 环境，或用 -IdfTools 指定工具目录。"
    exit 1
}

$nm = Get-ChildItem (Join-Path $IdfTools "xtensa-esp-elf") -Recurse -Filter "xtensa-$Target-elf-nm.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
$size = Get-ChildItem (Join-Path $IdfTools "xtensa-esp-elf") -Recurse -Filter "xtensa-$Target-elf-size.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $nm -or -not $size) {
    Write-Error "Toolchain not found for target $Target under $IdfTools"
    exit 1
}

$elf = Get-ChildItem (Join-Path $ProjectDir "build") -Filter "*.elf" -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne "bootloader.elf" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $elf) {
    Write-Error "No app ELF found under build/. Build first, e.g. .\build.ps1"
    exit 1
}

Write-Host "ELF: $($elf.FullName)"
Write-Host ""

# ---- section summary: distinguish resident RAM vs flash ----
$sections = & $size -A $elf.FullName | ForEach-Object {
    if ($_ -match '^\.(\S+)\s+(\d+)\s+') {
        [PSCustomObject]@{ Name = $matches[1]; Size = [long]$matches[2] }
    }
} | Where-Object { $_.Name -notmatch '^debug_|^comment$|^xt\.prop$|^xtensa|^xt\.lit$' }

function Sum-Section($pattern) {
    ($sections | Where-Object { $_.Name -match $pattern } | Measure-Object -Property Size -Sum).Sum
}

$dramData = Sum-Section '^dram0\.data$'
$dramBss  = Sum-Section '^dram0\.bss$'
$iramText = Sum-Section '^iram0\.text$'
$ramTotal = $dramData + $dramBss + $iramText
$flashText   = Sum-Section '^flash\.text$'
$flashRodata = Sum-Section '^flash\.rodata$'

function KB($b) { [Math]::Round($b / 1024.0, 1) }

Write-Host "=== RESIDENT STATIC RAM (not flash, not heap) ==="
Write-Host ("  .dram0.data   {0,8} B   ({1} KB)" -f $dramData, (KB $dramData))
Write-Host ("  .dram0.bss    {0,8} B   ({1} KB)" -f $dramBss, (KB $dramBss))
Write-Host ("  .iram0.text   {0,8} B   ({1} KB)" -f $iramText, (KB $iramText))
Write-Host ("  ---- TOTAL    {0,8} B   ({1} KB)" -f $ramTotal, (KB $ramTotal))
Write-Host ""
Write-Host "=== FLASH (mmap-loaded, NOT in RAM) ==="
Write-Host ("  .flash.text   {0,8} B   ({1} KB)" -f $flashText, (KB $flashText))
Write-Host ("  .flash.rodata {0,8} B   ({1} KB)  <- lvgl fonts, glyphs, tables" -f $flashRodata, (KB $flashRodata))
Write-Host ""

# ---- largest symbols in RESIDENT RAM only ----
# DRAM:  0x3FF90000 - 0x40000000 (data/bss)
# IRAM:  0x40080000 - 0x40100000 (text)
$dramLo = [long]0x3FF90000; $dramHi = [long]0x40000000
$iramLo = [long]0x40080000; $iramHi = [long]0x40100000

Write-Host ("=== largest {0} symbols in RESIDENT RAM ===" -f $Last)
$rows = & $nm --size-sort -S $elf.FullName | ForEach-Object {
    if ($_ -match '^\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\S)\s+(.+)$') {
        $addr = [Convert]::ToInt64($matches[1], 16)
        $sz   = [Convert]::ToInt64($matches[2], 16)
        $where = $null
        if ($addr -ge $dramLo -and $addr -lt $dramHi) { $where = "DRAM" }
        elseif ($addr -ge $iramLo -and $addr -lt $iramHi) { $where = "IRAM" }
        if ($where -and $sz -gt 0) {
            [PSCustomObject]@{ Addr = $matches[1]; Size = $sz; Kind = $matches[3]; Name = $matches[4]; Where = $where }
        }
    }
} | Sort-Object Size -Descending | Select-Object -First $Last

if (-not $rows) {
    Write-Host "  (none)"
} else {
    $rows | ForEach-Object {
        Write-Host ("{0,-5} {1,-8} {2,6} B ({3,6:F1} KB)  {4}" -f $_.Where, $_.Addr, $_.Size, ($_.Size/1024.0), $_.Name)
    }
}
