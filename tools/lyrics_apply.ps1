#requires -Version 5.0
<#
  lyrics_apply.ps1 - write matched Kugou lyrics into MP3 USLT frames

  Reads the report produced by lyrics_match.ps1, re-fetches the accepted lyric,
  replaces ONLY the USLT frame (title/artist/cover and every other frame are
  copied byte for byte), rebuilds the ID3v2.3 tag and replaces the file.

  Safety:
    - every original USLT text is dumped to -Backup (JSON) before writing
    - the new file is written next to the original, re-parsed and compared, and
      only then moved over the original
    - files with ID3 unsynchronisation / extended header are skipped

  Dry run (default) only prints what would happen:
    powershell -ExecutionPolicy Bypass -File tools\lyrics_apply.ps1

  Apply:
    powershell -ExecutionPolicy Bypass -File tools\lyrics_apply.ps1 -Apply
    powershell -ExecutionPolicy Bypass -File tools\lyrics_apply.ps1 -Apply -Only "water.mp3"
#>
param(
    [string]$Root   = '',
    [string]$Csv    = '',
    [string]$Backup = '',
    [switch]$Apply,
    [switch]$IncludeCheck,
    [switch]$Restore,
    [string]$Only = '',
    [int]   $Limit = 0,
    [int]   $DeltaSec = 2,
    [int]   $SleepMs  = 200
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$base = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
if (-not $Root)   { $Root   = Join-Path $env:USERPROFILE 'Music\mp3' }
if (-not $Csv)    { $Csv    = Join-Path $base '..\build\lyrics_match.csv' }
if (-not $Backup) { $Backup = Join-Path $base '..\build\lyrics_backup.json' }

$br1 = @(0,32,40,48,56,64,80,96,112,128,160,192,224,256,320)
$br2 = @(0,8,16,24,32,40,48,56,64,80,96,112,128,144,160)
$sr1 = @(44100,48000,32000); $sr2 = @(22050,24000,16000); $sr25 = @(11025,12000,8000)
$tsRe = [regex]'\[(\d+):(\d+(?:[.:]\d+)?)\]'

function Clean-Text([string]$s) {
    if (-not $s) { return '' }
    $i = $s.IndexOf([char]0)
    if ($i -ge 0) { $s = $s.Substring(0, $i) }
    return ($s.Replace([string][char]0xFEFF, '')).Trim()
}

function Norm-Text([string]$s) {
    if (-not $s) { return '' }
    $s = $s.ToLowerInvariant()
    $s = $s -replace '\.mp3$', ' '
    $s = $s -replace '[\(\uFF08\[\u3010][^\)\uFF09\]\u3011]*[\)\uFF09\]\u3011]', ' '
    $s = $s -replace '[^0-9a-z\u00c0-\u024f\u0400-\u04ff\u3000-\u9fff\uff00-\uffef]+', ' '
    return $s.Trim()
}

function Similar-Text([string]$a, [string]$b) {
    if (-not $a -or -not $b) { return $false }
    if ($a -eq $b) { return $true }
    if ($a.Contains($b) -or $b.Contains($a)) { return $true }
    $ta = @($a.Split(' ') | Where-Object { $_.Length -ge 2 })
    $tb = @($b.Split(' ') | Where-Object { $_.Length -ge 2 })
    if ($ta.Count -eq 0 -or $tb.Count -eq 0) { return $false }
    $common = @($ta | Where-Object { $tb -contains $_ }).Count
    return (($common / [Math]::Min($ta.Count, $tb.Count)) -ge 0.7)
}

function Parse-Mp3Header([byte[]]$b, [int]$i) {
    if ($i + 4 -ge $b.Length) { return $null }
    if ($b[$i] -ne 0xFF -or (($b[$i+1] -band 0xE0) -ne 0xE0)) { return $null }
    $vb = ($b[$i+1] -shr 3) -band 0x03
    $lb = ($b[$i+1] -shr 1) -band 0x03
    if ($vb -eq 1 -or $lb -eq 0) { return $null }
    $bi = ($b[$i+2] -shr 4) -band 0x0F
    $si = ($b[$i+2] -shr 2) -band 0x03
    if ($bi -eq 0 -or $bi -eq 15 -or $si -eq 3) { return $null }
    $pad = ($b[$i+2] -shr 1) -band 0x01
    $ch  = ($b[$i+3] -shr 6) -band 0x03
    $m1  = ($vb -eq 3)
    if ($m1) { $br = $br1[$bi] } else { $br = $br2[$bi] }
    if ($vb -eq 3) { $sr = $sr1[$si] } elseif ($vb -eq 2) { $sr = $sr2[$si] } else { $sr = $sr25[$si] }
    if ($m1) { $spf = 1152 } else { $spf = 576 }
    [pscustomobject]@{ Bitrate=$br; Samp=$sr; Spf=$spf; Mono=($ch -eq 3); Mpeg1=$m1 }
}

function Decode-Id3([byte[]]$b, [int]$start, [int]$len, [int]$enc) {
    if ($len -le 0) { return '' }
    $s = ''
    if ($enc -eq 0)      { $s = [Text.Encoding]::GetEncoding(28591).GetString($b, $start, $len) }
    elseif ($enc -eq 3)  { $s = [Text.Encoding]::UTF8.GetString($b, $start, $len) }
    elseif ($enc -eq 2)  { if ($len -ge 2) { $s = [Text.Encoding]::BigEndianUnicode.GetString($b, $start, $len - ($len % 2)) } }
    else                 { if ($len -ge 2) { $s = [Text.Encoding]::Unicode.GetString($b, $start, $len - ($len % 2)) } }
    return (Clean-Text $s)
}

function Get-LyricTimes([string]$text) {
    $times = @()
    foreach ($m in $tsRe.Matches($text)) {
        $mm  = [int]$m.Groups[1].Value
        $ss  = $m.Groups[2].Value
        $sec = 0.0
        if ($ss.Contains(':'))     { $q = $ss.Split(':'); $sec = [double]$q[0] + [double]("0." + $q[1]) }
        elseif ($ss.Contains('.')) { $q = $ss.Split('.'); $sec = [double]$q[0] + [double]("0." + $q[1]) }
        else                       { $sec = [double]$ss }
        $times += ($mm * 60 + $sec)
    }
    return ,$times
}

# returns tag info: version, flags, tagEnd, frames(list), title, artist, uslt, duration...
function Read-Mp3([string]$path, [switch]$WithBytes) {
    $all = [System.IO.File]::ReadAllBytes($path)
    $len = $all.Length
    $tagEnd = 0; $ver = 0; $flags = 0
    if ($len -ge 10 -and $all[0] -eq 0x49 -and $all[1] -eq 0x44 -and $all[2] -eq 0x33) {
        $ver   = $all[3]
        $flags = $all[5]
        $tagEnd = 10 + ((($all[6] -band 0x7f) -shl 21) -bor (($all[7] -band 0x7f) -shl 14) -bor (($all[8] -band 0x7f) -shl 7) -bor ($all[9] -band 0x7f))
        if (($flags -band 0x10) -ne 0) { $tagEnd += 10 }
        if ($tagEnd -gt $len) { $tagEnd = 0 }
    }
    $title = ''; $artist = ''; $uslt = ''
    $frames = New-Object System.Collections.ArrayList
    if ($tagEnd -gt 0) {
        $pos = 10
        if (($flags -band 0x40) -ne 0) {
            if ($ver -eq 4) { $ext = (($all[10] -band 0x7f) -shl 21) -bor (($all[11] -band 0x7f) -shl 14) -bor (($all[12] -band 0x7f) -shl 7) -bor ($all[13] -band 0x7f); $pos = 10 + $ext }
            else { $ext = ([int]$all[10] -shl 24) -bor ([int]$all[11] -shl 16) -bor ([int]$all[12] -shl 8) -bor [int]$all[13]; $pos = 10 + 4 + $ext }
        }
        while ($pos + 10 -le $tagEnd) {
            if ($all[$pos] -eq 0) { break }
            $id = [Text.Encoding]::ASCII.GetString($all, $pos, 4)
            if ($ver -eq 4) { $fsz = (($all[$pos+4] -band 0x7f) -shl 21) -bor (($all[$pos+5] -band 0x7f) -shl 14) -bor (($all[$pos+6] -band 0x7f) -shl 7) -bor ($all[$pos+7] -band 0x7f) }
            else { $fsz = ([int]$all[$pos+4] -shl 24) -bor ([int]$all[$pos+5] -shl 16) -bor ([int]$all[$pos+6] -shl 8) -bor [int]$all[$pos+7] }
            if ($fsz -le 0 -or $pos + 10 + $fsz -gt $tagEnd) { break }
            $p = $pos + 10
            if ($id -eq 'TIT2' -or $id -eq 'TPE1') {
                $enc = $all[$p]
                $t = Decode-Id3 $all ($p + 1) ($fsz - 1) $enc
                if ($id -eq 'TIT2') { $title = $t } else { $artist = $t }
            }
            elseif ($id -eq 'USLT') {
                $enc = $all[$p]
                $s = $p + 4; $e = $p + $fsz
                if ($enc -eq 1 -or $enc -eq 2) { while ($s+1 -lt $e) { if ($all[$s] -eq 0 -and $all[$s+1] -eq 0) { $s += 2; break }; $s += 2 } }
                else { while ($s -lt $e) { if ($all[$s] -eq 0) { $s++; break }; $s++ } }
                if ($s -lt $e) { $uslt = Decode-Id3 $all $s ($e - $s) $enc }
            }
            if ($id -ne 'USLT') {
                $payload = New-Object byte[] $fsz
                [Array]::Copy($all, $p, $payload, 0, $fsz)
                $flags2 = @([byte]$all[$pos+8], [byte]$all[$pos+9])
                $null = $frames.Add([pscustomobject]@{ Id=$id; Flags=$flags2; Payload=$payload })
            }
            $pos += 10 + $fsz
        }
    }
    # duration
    $win = New-Object byte[] 8192
    $rn = 0
    if ($len -gt $tagEnd) {
        $rn = [Math]::Min(8192, $len - $tagEnd)
        [Array]::Copy($all, $tagEnd, $win, 0, $rn)
    }
    $fo = -1
    for ($i = 0; $i + 4 -lt $rn; $i++) {
        if ($win[$i] -eq 0xFF -and ($win[$i+1] -band 0xE0) -eq 0xE0) {
            if ($null -ne (Parse-Mp3Header $win $i)) { $fo = $i; break }
        }
    }
    $dur = -1.0
    if ($fo -ge 0) {
        $h = Parse-Mp3Header $win $fo
        if ($h.Mpeg1) { if ($h.Mono) { $so = 17 } else { $so = 32 } } else { if ($h.Mono) { $so = 9 } else { $so = 17 } }
        $xi = $fo + 4 + $so
        $tagX = ''
        if ($xi + 12 -lt $rn) { $tagX = [Text.Encoding]::ASCII.GetString($win, $xi, 4) }
        if ($tagX -eq 'Xing' -or $tagX -eq 'Info') {
            $fl = ([int]$win[$xi+4] -shl 24) -bor ([int]$win[$xi+5] -shl 16) -bor ([int]$win[$xi+6] -shl 8) -bor [int]$win[$xi+7]
            if (($fl -band 1) -ne 0) {
                $fr = ([int64]$win[$xi+8] -shl 24) -bor ([int64]$win[$xi+9] -shl 16) -bor ([int64]$win[$xi+10] -shl 8) -bor [int64]$win[$xi+11]
                if ($fr -gt 0) { $dur = $fr * $h.Spf / $h.Samp }
            }
        }
        if ($dur -lt 0) { $dur = ($len - $tagEnd) * 8 / ($h.Bitrate * 1000) }
    }
    $times = @()
    if ($uslt) { $times = Get-LyricTimes $uslt }
    $last = if ($times.Count -gt 0) { ($times | Measure-Object -Maximum).Maximum } else { -1.0 }
    $res = [pscustomobject]@{
        Path=$path; Name=(Split-Path $path -Leaf); Len=$len; Ver=$ver; Flags=$flags; TagEnd=$tagEnd
        Title=$title; Artist=$artist; Uslt=$uslt
        Dur=[math]::Round($dur,1); CurLast=[math]::Round($last,1); CurLines=$times.Count
        Frames=$frames
    }
    if ($WithBytes) { $res | Add-Member -NotePropertyName Bytes -NotePropertyValue $all }
    return $res
}

function Invoke-Json([string]$url) {
    for ($k = 0; $k -lt 3; $k++) {
        try {
            $r = Invoke-WebRequest $url -UseBasicParsing -TimeoutSec 15
            return ($r.Content | ConvertFrom-Json)
        } catch {
            if ($k -eq 2) { throw }
            Start-Sleep -Milliseconds 600
        }
    }
}

function Kugou-Search([string]$kw) {
    $u = 'http://mobilecdn.kugou.com/api/v3/search/song?format=json&page=1&pagesize=20&keyword=' + [uri]::EscapeDataString($kw)
    $j = Invoke-Json $u
    if ($null -eq $j -or $null -eq $j.data) { return @() }
    return @($j.data.info)
}

function Kugou-LyricCandidates([string]$hash, [int]$durSec) {
    $u = "http://krcs.kugou.com/search?ver=1&man=yes&client=mobi&keyword=&duration=$durSec&hash=$hash"
    $j = Invoke-Json $u
    if ($null -eq $j -or $null -eq $j.candidates) { return @() }
    return @($j.candidates)
}

function Kugou-LyricText([string]$id, [string]$key) {
    $u = "http://lyrics.kugou.com/download?ver=1&client=pc&id=$id&accesskey=$key&fmt=lrc&charset=utf8"
    $j = Invoke-Json $u
    if ($null -eq $j -or [int]$j.status -ne 200 -or -not $j.content) { return $null }
    return [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($j.content))
}

function SyncSafe4([int]$v) {
    return @([byte](($v -shr 21) -band 0x7f), [byte](($v -shr 14) -band 0x7f), [byte](($v -shr 7) -band 0x7f), [byte]($v -band 0x7f))
}

function Build-UsltFrame([string]$text) {
    $txt = $text.Replace("`r`n", "`n").Replace("`r", "`n").TrimEnd("`n")
    $body = [Text.Encoding]::Unicode.GetBytes($txt)
    $sz = 1 + 3 + 2 + 2 + $body.Length + 2      # enc + lang + desc term + BOM + text + term
    $f = New-Object byte[] (10 + $sz)
    $id = [Text.Encoding]::ASCII.GetBytes('USLT')
    [Array]::Copy($id, 0, $f, 0, 4)
    $f[4] = [byte](($sz -shr 24) -band 0xFF)
    $f[5] = [byte](($sz -shr 16) -band 0xFF)
    $f[6] = [byte](($sz -shr 8) -band 0xFF)
    $f[7] = [byte]($sz -band 0xFF)
    $f[8] = 0; $f[9] = 0
    $p = 10
    $f[$p] = 1; $p++                                   # UTF-16 with BOM
    $f[$p] = 0x65; $f[$p+1] = 0x6E; $f[$p+2] = 0x67; $p += 3   # 'eng'
    $p += 2                                            # empty descriptor terminator
    $f[$p] = 0xFF; $f[$p+1] = 0xFE; $p += 2            # BOM
    [Array]::Copy($body, 0, $f, $p, $body.Length)
    return ,$f
}

function Write-Uslt([string]$path, [string]$text) {
    $info = Read-Mp3 $path -WithBytes
    if (($info.Flags -band 0xC0) -ne 0) { throw ("unsupported tag flags 0x{0:x2}" -f $info.Flags) }
    if ($info.Ver -ne 3) { throw ("unsupported id3v2.{0}" -f $info.Ver) }

    $uslt = Build-UsltFrame $text
    $ms = New-Object System.IO.MemoryStream
    $bodyLen = 0
    foreach ($fr in $info.Frames) { $bodyLen += 10 + $fr.Payload.Length }
    $bodyLen += $uslt.Length
    $pad = 1024
    $tagSize = $bodyLen + $pad

    $hdr = New-Object byte[] 10
    $hdr[0] = 0x49; $hdr[1] = 0x44; $hdr[2] = 0x33
    $hdr[3] = 3; $hdr[4] = 0; $hdr[5] = 0
    $ss = SyncSafe4 $tagSize
    $hdr[6] = $ss[0]; $hdr[7] = $ss[1]; $hdr[8] = $ss[2]; $hdr[9] = $ss[3]
    $ms.Write($hdr, 0, 10)

    foreach ($fr in $info.Frames) {
        $h = New-Object byte[] 10
        $id = [Text.Encoding]::ASCII.GetBytes($fr.Id)
        [Array]::Copy($id, 0, $h, 0, 4)
        $n = $fr.Payload.Length
        $h[4] = [byte](($n -shr 24) -band 0xFF)
        $h[5] = [byte](($n -shr 16) -band 0xFF)
        $h[6] = [byte](($n -shr 8) -band 0xFF)
        $h[7] = [byte]($n -band 0xFF)
        $h[8] = $fr.Flags[0]; $h[9] = $fr.Flags[1]
        $ms.Write($h, 0, 10)
        $ms.Write($fr.Payload, 0, $n)
    }
    $ms.Write($uslt, 0, $uslt.Length)
    $zero = New-Object byte[] $pad
    $ms.Write($zero, 0, $pad)
    $ms.Write($info.Bytes, $info.TagEnd, $info.Bytes.Length - $info.TagEnd)

    $bytes = $ms.ToArray()
    $ms.Dispose()

    $tmp = $path + '.new'
    [System.IO.File]::WriteAllBytes($tmp, $bytes)
    return $tmp
}

# ---------- main ----------

if (-not (Test-Path -LiteralPath $Csv)) { throw "report not found: $Csv" }

if ($Restore) {
    if (-not (Test-Path -LiteralPath $Backup)) { throw "backup not found: $Backup" }
    $bak = @()
    $parsed = Get-Content -LiteralPath $Backup -Raw | ConvertFrom-Json
    foreach ($e in $parsed) { $bak += $e }   # ConvertFrom-Json yields the whole array as one item
    Write-Host ("Restore from {0}  ({1} entries)" -f $Backup, $bak.Count)
    foreach ($e in $bak) {
        if ($Only -and $e.Name -ne $Only) { continue }
        $p = Join-Path $Root $e.Name
        if (-not (Test-Path -LiteralPath $p)) { Write-Warning ("missing: " + $e.Name); continue }
        if (-not $e.OldLyric) { Write-Host ("  SKIP  {0}  (no backup lyric)" -f $e.Name); continue }
        try {
            $tmp = Write-Uslt $p $e.OldLyric
            Move-Item -LiteralPath $tmp -Destination $p -Force
            Write-Host ("  RESTORED {0}" -f $e.Name)
        } catch {
            Write-Warning ("  FAIL {0}: {1}" -f $e.Name, $_.Exception.Message)
            if (Test-Path -LiteralPath ($p + '.new')) { Remove-Item -LiteralPath ($p + '.new') -Force }
        }
    }
    return
}

$rows = @(Import-Csv -LiteralPath $Csv)
$want = @($rows | Where-Object { $_.Action -eq 'REPLACE' -or ($IncludeCheck -and $_.Action -eq 'CHECK') })
if ($Only) { $want = @($want | Where-Object { $_.Name -eq $Only }) }
if ($Limit -gt 0) { $want = @($want | Select-Object -First $Limit) }

Write-Host ("Targets: {0}   mode: {1}" -f $want.Count, ($(if ($Apply) { 'APPLY' } else { 'DRY-RUN' })))

$backupList = New-Object System.Collections.ArrayList
$done = 0; $fail = 0; $skip = 0

foreach ($w in $want) {
    $path = Join-Path $Root $w.Name
    if (-not (Test-Path -LiteralPath $path)) { Write-Warning ("missing: " + $w.Name); $fail++; continue }

    $info = $null
    try { $info = Read-Mp3 $path } catch { Write-Warning ("read fail: " + $w.Name); $fail++; continue }
    $dur = $info.Dur

    # re-find the accepted lyric (same rules as lyrics_match.ps1)
    $title = Clean-Text $info.Title
    if (-not $title) { $title = ($w.Name -replace '\.mp3$', '') }
    $artist = Clean-Text $info.Artist
    $nt = Norm-Text $title
    $queries = @()
    if ($artist) { $queries += ("$title $artist") }
    $queries += $title
    $queries = @($queries | Where-Object { $_ } | Select-Object -Unique)

    $bestText = $null; $bestCov = 0.0; $bestSong = ''; $bestLines = 0; $bestLast = 0.0
    foreach ($q in $queries) {
        try { $cs = Kugou-Search $q } catch { continue }
        Start-Sleep -Milliseconds $SleepMs
        $cands = @()
        foreach ($c in $cs) {
            if ($null -eq $c.duration) { continue }
            if ([math]::Abs([double]$c.duration - $dur) -gt $DeltaSec) { continue }
            if (-not (Similar-Text $nt (Norm-Text $c.songname))) { continue }
            # must match the candidate approved in the report (song + artist), so a
            # different song that happens to share the title can never be written
            if ($w.MatchSong -and -not (Similar-Text (Norm-Text $w.MatchSong) (Norm-Text $c.songname))) { continue }
            if ($w.MatchArtist -and -not (Similar-Text (Norm-Text $w.MatchArtist) (Norm-Text $c.singername))) { continue }
            $cands += $c
        }
        foreach ($c in $cands) {
            $lrc = $null
            try {
                $lc = Kugou-LyricCandidates $c.hash ([int]$dur)
                Start-Sleep -Milliseconds $SleepMs
                $picks = @($lc | Sort-Object { [math]::Abs(([double]$_.duration / 1000) - $dur) } | Select-Object -First 2)
                foreach ($pick in $picks) {
                    for ($t = 0; $t -lt 2 -and -not $lrc; $t++) {
                        $lrc = Kugou-LyricText $pick.id $pick.accesskey
                        if (-not $lrc) { Start-Sleep -Milliseconds 700 }
                    }
                    Start-Sleep -Milliseconds $SleepMs
                    if ($lrc) { break }
                }
            } catch { }
            if (-not $lrc) { continue }
            $times = Get-LyricTimes $lrc
            if ($times.Count -lt 15) { continue }
            $last = ($times | Measure-Object -Maximum).Maximum
            $cov = $last / $dur
            if ($cov -le 1.02 -and $cov -gt $bestCov) {
                $bestCov = $cov; $bestText = $lrc; $bestSong = $c.songname; $bestLines = $times.Count; $bestLast = $last
            }
        }
        if ($bestCov -ge 0.97) { break }
    }

    if (-not $bestText -or $bestCov -lt 0.85) {
        Write-Host ("  SKIP  {0}  (no lyric re-found)" -f $w.Name)
        $skip++; continue
    }

    if ($bestText.Trim() -eq $info.Uslt.Trim()) {
        Write-Host ("  SAME  {0}  (already up to date)" -f $w.Name)
        $skip++; continue
    }

    $null = $backupList.Add([pscustomobject]@{
        Name=$w.Name; OldLyric=$info.Uslt; NewFirstLine=($bestText -split "`n" | Where-Object { $_ -match '^\[' } | Select-Object -First 1)
    })

    if (-not $Apply) {
        Write-Host ("  WOULD {0,-44} cov={1:P0} lines={2} last={3:N0}s" -f $w.Name, $bestCov, $bestLines, $bestLast)
        continue
    }

    try {
        $tmp = Write-Uslt $path $bestText
        $chk = Read-Mp3 $tmp
        if ($chk.CurLines -lt 15 -or [math]::Abs($chk.CurLast - $bestLast) -gt 2) { throw "verify: lyric mismatch" }
        if ($chk.Title -ne $info.Title -or $chk.Artist -ne $info.Artist) { throw "verify: title/artist changed" }
        if ($chk.Dur -lt $info.Dur - 1 -or $chk.Dur -gt $info.Dur + 1) { throw "verify: duration changed" }
        Move-Item -LiteralPath $tmp -Destination $path -Force
        $done++
        Write-Host ("  OK    {0,-44} cov={1:P0} lines={2}" -f $w.Name, $bestCov, $bestLines)
    } catch {
        Write-Warning ("  FAIL  {0}: {1}" -f $w.Name, $_.Exception.Message)
        if (Test-Path -LiteralPath ($path + '.new')) { Remove-Item -LiteralPath ($path + '.new') -Force }
        $fail++
    }
}

if ($backupList.Count -gt 0) {
    $bakDir = Split-Path $Backup -Parent
    if (-not (Test-Path $bakDir)) { New-Item -ItemType Directory -Path $bakDir | Out-Null }
    $old = @()
    if (Test-Path -LiteralPath $Backup) {
        $parsedOld = Get-Content -LiteralPath $Backup -Raw | ConvertFrom-Json
        foreach ($e in $parsedOld) { $old += $e }
    }
    $merged = @($old) + @($backupList)
    $merged | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Backup -Encoding UTF8
    Write-Host ("Backup: {0}  ({1} entries)" -f $Backup, $merged.Count)
}

Write-Host ("Done: applied={0} skipped={1} failed={2}" -f $done, $skip, $fail)
