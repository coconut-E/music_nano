#requires -Version 5.0
<#
  lyrics_match.ps1 - match Kugou lyrics against local MP3s (DRY RUN)

  - reads ID3v2 TIT2 / TPE1 / USLT (BOM and terminator safe)
  - computes the real duration (Xing frame count, else CBR estimate)
  - flags files whose embedded lyrics look wrong:
        MISSING : no timestamped USLT, or fewer than -MinLines lines
        LONG    : last timestamp > duration + 2s
        SHORT   : last timestamp < duration - max(20s, 10%)
  - searches Kugou for "title artist", then title only
  - accepts a candidate ONLY if
        * its catalog duration is within -DeltaSec of the local duration
        * its song name is similar to ours
        * (when both known) the artist matches
  - downloads the LRC and validates the new timeline

  Writes nothing but the CSV report (-Out).

  Examples:
    powershell -ExecutionPolicy Bypass -File tools\lyrics_match.ps1
    powershell -ExecutionPolicy Bypass -File tools\lyrics_match.ps1 -WhatIf ALL
#>
param(
    [string]$Root   = (Join-Path $env:USERPROFILE 'Music\mp3'),
    [string]$Out    = (Join-Path $PSScriptRoot '..\build\lyrics_match.csv'),
    [int]   $DeltaSec = 2,
    [int]   $MinLines = 10,
    [ValidateSet('BAD','ALL')]
    [string]$WhatIf = 'BAD',
    [int]   $SleepMs  = 200
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$br1 = @(0,32,40,48,56,64,80,96,112,128,160,192,224,256,320)
$br2 = @(0,8,16,24,32,40,48,56,64,80,96,112,128,144,160)
$sr1 = @(44100,48000,32000); $sr2 = @(22050,24000,16000); $sr25 = @(11025,12000,8000)
$tsRe = [regex]'\[(\d+):(\d+(?:[.:]\d+)?)\]'

# ---------- text helpers ----------

function Clean-Text([string]$s) {
    if (-not $s) { return '' }
    $i = $s.IndexOf([char]0)
    if ($i -ge 0) { $s = $s.Substring(0, $i) }
    $s = $s.Replace([string][char]0xFEFF, '')
    return $s.Trim()
}

function Norm-Text([string]$s) {
    if (-not $s) { return '' }
    $s = $s.ToLowerInvariant()
    $s = $s -replace '\.mp3$', ' '
    # drop bracketed qualifiers: () [] and full width variants
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
    $min = [Math]::Min($ta.Count, $tb.Count)
    return (($common / $min) -ge 0.7)
}

function Clean-FileTitle([string]$t) {
    $s = $t -replace '\.mp3$', ''
    $s = $s -replace '\s*[\(\uFF08\[\u3010][^\)\uFF09\]\u3011]*[\)\uFF09\]\u3011]', ''
    $s = $s -replace '\s*[-\uFF0D]\s*[^-\uFF0D]*$', ''
    return $s.Trim()
}

# ---------- ID3 / MP3 ----------

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

function Read-Mp3([string]$path) {
    $len = (Get-Item -LiteralPath $path).Length
    $fs  = [System.IO.File]::OpenRead($path)
    try {
        $hdr = New-Object byte[] 10
        $n   = $fs.Read($hdr, 0, 10)
        $tagEnd = 0; $ver = 0; $flags = 0; $tag = $null
        if ($n -eq 10 -and $hdr[0] -eq 0x49 -and $hdr[1] -eq 0x44 -and $hdr[2] -eq 0x33) {
            $ver   = $hdr[3]
            $flags = $hdr[5]
            $tagEnd = 10 + ((($hdr[6] -band 0x7f) -shl 21) -bor (($hdr[7] -band 0x7f) -shl 14) -bor (($hdr[8] -band 0x7f) -shl 7) -bor ($hdr[9] -band 0x7f))
            if (($flags -band 0x10) -ne 0) { $tagEnd += 10 }
            if ($tagEnd -gt $len) { $tagEnd = 0 }
        }
        if ($tagEnd -gt 0) {
            $fs.Position = 0
            $tag = New-Object byte[] $tagEnd
            [void]$fs.Read($tag, 0, $tagEnd)
        }
        $title = ''; $artist = ''; $uslt = ''
        if ($null -ne $tag) {
            $pos = 10
            if (($flags -band 0x40) -ne 0) {
                if ($ver -eq 4) { $ext = (($tag[10] -band 0x7f) -shl 21) -bor (($tag[11] -band 0x7f) -shl 14) -bor (($tag[12] -band 0x7f) -shl 7) -bor ($tag[13] -band 0x7f); $pos = 10 + $ext }
                else { $ext = ([int]$tag[10] -shl 24) -bor ([int]$tag[11] -shl 16) -bor ([int]$tag[12] -shl 8) -bor [int]$tag[13]; $pos = 10 + 4 + $ext }
            }
            while ($pos + 10 -le $tag.Length) {
                if ($tag[$pos] -eq 0) { break }
                $id = [Text.Encoding]::ASCII.GetString($tag, $pos, 4)
                if ($ver -eq 4) { $fsz = (($tag[$pos+4] -band 0x7f) -shl 21) -bor (($tag[$pos+5] -band 0x7f) -shl 14) -bor (($tag[$pos+6] -band 0x7f) -shl 7) -bor ($tag[$pos+7] -band 0x7f) }
                else { $fsz = ([int]$tag[$pos+4] -shl 24) -bor ([int]$tag[$pos+5] -shl 16) -bor ([int]$tag[$pos+6] -shl 8) -bor [int]$tag[$pos+7] }
                if ($fsz -le 0 -or $pos + 10 + $fsz -gt $tag.Length) { break }
                $p = $pos + 10
                if ($id -eq 'TIT2' -or $id -eq 'TPE1') {
                    $enc = $tag[$p]
                    $t = Decode-Id3 $tag ($p + 1) ($fsz - 1) $enc
                    if ($id -eq 'TIT2') { $title = $t } else { $artist = $t }
                }
                elseif ($id -eq 'USLT') {
                    $enc = $tag[$p]
                    $s = $p + 4
                    $e = $p + $fsz
                    if ($enc -eq 1 -or $enc -eq 2) { while ($s+1 -lt $e) { if ($tag[$s] -eq 0 -and $tag[$s+1] -eq 0) { $s += 2; break }; $s += 2 } }
                    else { while ($s -lt $e) { if ($tag[$s] -eq 0) { $s++; break }; $s++ } }
                    if ($s -lt $e) { $uslt = Decode-Id3 $tag $s ($e - $s) $enc }
                }
                $pos += 10 + $fsz
            }
        }
        $fs.Position = $tagEnd
        $win = New-Object byte[] 8192
        $rn  = $fs.Read($win, 0, 8192)
        $fo = -1
        for ($i = 0; $i + 4 -lt $rn; $i++) {
            if ($win[$i] -eq 0xFF -and ($win[$i+1] -band 0xE0) -eq 0xE0) {
                if ($null -ne (Parse-Mp3Header $win $i)) { $fo = $i; break }
            }
        }
        $dur = -1.0; $kbps = 0; $src = 'none'
        if ($fo -ge 0) {
            $h = Parse-Mp3Header $win $fo
            $kbps = $h.Bitrate
            if ($h.Mpeg1) { if ($h.Mono) { $so = 17 } else { $so = 32 } } else { if ($h.Mono) { $so = 9 } else { $so = 17 } }
            $xi = $fo + 4 + $so
            $tagX = ''
            if ($xi + 12 -lt $rn) { $tagX = [Text.Encoding]::ASCII.GetString($win, $xi, 4) }
            if ($tagX -eq 'Xing' -or $tagX -eq 'Info') {
                $fl = ([int]$win[$xi+4] -shl 24) -bor ([int]$win[$xi+5] -shl 16) -bor ([int]$win[$xi+6] -shl 8) -bor [int]$win[$xi+7]
                if (($fl -band 1) -ne 0) {
                    $fr = ([int64]$win[$xi+8] -shl 24) -bor ([int64]$win[$xi+9] -shl 16) -bor ([int64]$win[$xi+10] -shl 8) -bor [int64]$win[$xi+11]
                    if ($fr -gt 0) { $dur = $fr * $h.Spf / $h.Samp; $src = 'xing' }
                }
            }
            if ($dur -lt 0) { $dur = ($len - $tagEnd) * 8 / ($kbps * 1000); $src = 'cbr' }
        }
        $times = @()
        if ($uslt) { $times = Get-LyricTimes $uslt }
        $last = if ($times.Count -gt 0) { ($times | Measure-Object -Maximum).Maximum } else { -1.0 }
        [pscustomobject]@{
            Path=$path; Name=(Split-Path $path -Leaf)
            Dur=[math]::Round($dur,1); Src=$src; Kbps=$kbps
            Title=$title; Artist=$artist
            CurLast=[math]::Round($last,1); CurLines=$times.Count
        }
    } finally { $fs.Dispose() }
}

# ---------- Kugou ----------

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

# ---------- main ----------

$files = @(Get-ChildItem -LiteralPath $Root -Filter *.mp3 -File | Sort-Object Name)
Write-Host ("Scanning {0} files under {1}" -f $files.Count, $Root)

$rows = New-Object System.Collections.ArrayList
$i = 0
foreach ($f in $files) {
    $i++
    $info = $null
    try { $info = Read-Mp3 $f.FullName } catch { Write-Warning ("read fail: " + $f.Name); continue }
    if ($null -eq $info) { continue }

    $kind = 'OK'
    if     ($info.CurLines -lt $MinLines)                        { $kind = 'MISSING' }
    elseif ($info.CurLast -gt $info.Dur + 2)                     { $kind = 'LONG' }
    elseif ($info.CurLast -lt $info.Dur - [math]::Max(20, $info.Dur * 0.10)) { $kind = 'SHORT' }

    if ($kind -eq 'OK' -and $WhatIf -eq 'BAD') { continue }

    $title  = Clean-Text $info.Title
    if (-not $title) { $title = Clean-FileTitle $info.Name }
    $artist = Clean-Text $info.Artist
    $nt = Norm-Text $title

    $queries = @()
    if ($artist) { $queries += ("$title $artist") }
    $queries += $title
    $ct = Clean-FileTitle $title
    if ($ct -and $ct -ne $title) { $queries += $ct }
    $queries = @($queries | Where-Object { $_ } | Select-Object -Unique)

    $cands = @()
    foreach ($q in $queries) {
        try { $cs = Kugou-Search $q } catch { continue }
        Start-Sleep -Milliseconds $SleepMs
        foreach ($c in $cs) {
            if ($null -eq $c.duration) { continue }
            $delta = [math]::Abs([double]$c.duration - $info.Dur)
            if ($delta -gt $DeltaSec) { continue }
            $ns = Norm-Text $c.songname
            $titleOk = Similar-Text $nt $ns
            $artistOk = $false
            if ($artist) {
                $na = Norm-Text $artist
                $ng = Norm-Text $c.singername
                $artistOk = Similar-Text $na $ng
            }
            $cands += [pscustomobject]@{
                Song=$c.songname; Singer=$c.singername; Hash=$c.hash
                Dur=[double]$c.duration; Delta=[math]::Round($delta,1)
                TitleOk=$titleOk; ArtistOk=$artistOk; Query=$q
            }
        }
        if (@($cands | Where-Object { $_.TitleOk }).Count -gt 0) { break }
    }

    $row = [pscustomobject]@{
        Name=$info.Name; Kind=$kind; LocalDur=$info.Dur
        CurLast=$info.CurLast; CurLines=$info.CurLines
        Title=$title; Artist=$artist
        MatchSong=''; MatchArtist=''; MatchDur=''; Delta=''; TitleOk=''; ArtistOk=''
        NewLast=''; NewLines=''; Cov=''; Action='SKIP'; Note=''
    }

    $good = @($cands | Where-Object { $_.TitleOk -and ($_.ArtistOk -or -not $artist) } | Sort-Object Delta)
    if ($good.Count -eq 0) {
        $row.Note = if ($cands.Count -eq 0) { 'no duration match' }
                    elseif (-not $artist) { 'title mismatch' }
                    else { 'artist mismatch' }
        if ($cands.Count -gt 0) {
            $row.MatchSong = $cands[0].Song; $row.MatchDur = [int]$cands[0].Dur; $row.Delta = $cands[0].Delta
        }
    } else {
        # try several candidates; keep the one whose lyric covers the recording best
        $bestRow = $null; $bestCov = 0.0
        foreach ($c in ($good | Select-Object -First 5)) {
            $lrc = $null
            try {
                $lc = Kugou-LyricCandidates $c.Hash ([int]$info.Dur)
                Start-Sleep -Milliseconds $SleepMs
                $picks = @($lc | Sort-Object { [math]::Abs(([double]$_.duration / 1000) - $info.Dur) } | Select-Object -First 2)
                foreach ($pick in $picks) {
                    for ($try = 0; $try -lt 2 -and -not $lrc; $try++) {
                        $lrc = Kugou-LyricText $pick.id $pick.accesskey
                        if (-not $lrc) { Start-Sleep -Milliseconds 700 }
                    }
                    Start-Sleep -Milliseconds $SleepMs
                    if ($lrc) { break }
                }
            } catch { }
            if (-not $lrc) { $row.Note = 'no lyric file'; continue }
            $t = Get-LyricTimes $lrc
            if ($t.Count -lt 15) { $row.Note = 'lyric too short'; continue }
            $newLast = ($t | Measure-Object -Maximum).Maximum
            $cov = $newLast / $info.Dur
            if ($cov -le 1.02 -and $cov -gt $bestCov) {
                $bestCov = $cov
                $bestRow = [pscustomobject]@{
                    Song=$c.Song; Singer=$c.Singer; Dur=$c.Dur; Delta=$c.Delta
                    TitleOk=$c.TitleOk; ArtistOk=$c.ArtistOk
                    NewLast=[math]::Round($newLast,1); NewLines=$t.Count; Cov=[math]::Round($cov,3)
                }
            }
            if ($bestCov -ge 0.97) { break }
        }
        if ($null -ne $bestRow) {
            $row.MatchSong  = $bestRow.Song
            $row.MatchArtist= $bestRow.Singer
            $row.MatchDur   = [int]$bestRow.Dur
            $row.Delta      = $bestRow.Delta
            $row.TitleOk    = $bestRow.TitleOk
            $row.ArtistOk   = $bestRow.ArtistOk
            $row.NewLast    = $bestRow.NewLast
            $row.NewLines   = $bestRow.NewLines
            $row.Cov        = $bestRow.Cov
            if ($bestCov -ge 0.85) {
                $row.Action = 'REPLACE'; $row.Note = ''
            } else {
                $row.Action = 'CHECK';   $row.Note = 'low coverage'
            }
        }
    }

    $null = $rows.Add($row)
    if (($i % 10) -eq 0) { Write-Host ("  {0}/{1}" -f $i, $files.Count) }
}

$outDir = Split-Path $Out -Parent
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$rows | Export-Csv -Path $Out -NoTypeInformation -Encoding UTF8

Write-Host ''
Write-Host ("Report: {0}  ({1} rows)" -f (Resolve-Path $Out).Path, $rows.Count)
foreach ($x in ($rows | Group-Object Action | Sort-Object Name)) {
    Write-Host ("  {0,-8} {1}" -f $x.Name, $x.Count)
}
