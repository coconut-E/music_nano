#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""字库覆盖率检查: 把曲库里的标签/歌词和 lv_font_global_16.c 的字形表对比, 报缺字.

用法:
    python tools/font_check.py <曲库目录或zip> [更多路径...]
    python tools/font_check.py --font <字体C文件> <路径...>

    python tools/font_check.py "C:\\Users\\123\\Music" "C:\\Users\\123\\Downloads\\music4.2.zip"

退出码: 0 = 没有缺字; 1 = 有缺字(需要补字符源重新取模).

会扫描:
  * mp3 : ID3v2 的 TIT2/TPE1/TALB + USLT 歌词
  * flac: Vorbis comment 的 TITLE/ARTIST/ALBUM + LYRICS/UNSYNCEDLYRICS
  * .lrc 伴奏歌词文件
  * 文件名本身
不可显示字符(控制符/零宽/组合符号/变体选择符)会被忽略, 不计入缺字.
"""

import io
import os
import re
import sys
import zipfile

DEFAULT_FONT = os.path.join("main", "resources", "lv_font_global_16.c")

# 不可显示: 控制符、格式符、零宽、组合用符号、变体选择符
IGNORE_CATS = {"Cc", "Cf", "Cs", "Mn", "Me", "Zl", "Zp"}


# ───────────────────────── 字体 ─────────────────────────

def load_glyphs(path):
    text = open(path, encoding="utf-8", errors="ignore").read()
    return {int(m.group(1), 16) for m in re.finditer(r"/\* U\+([0-9A-Fa-f]+)", text)}


# ───────────────────────── ID3 / FLAC ─────────────────────────

def _syncsafe(b):
    return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]


def _plain32(b):
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]


def _decode(b, enc):
    try:
        return b.decode(["latin-1", "utf-16", "utf-16-be", "utf-8"][enc])
    except Exception:
        return b.decode("utf-8", "replace")


def read_mp3_text(fh):
    """返回 mp3 的 标题/歌手/专辑/歌词 文本列表."""
    out = []
    head = fh.read(10)
    if len(head) < 10 or head[:3] != b"ID3":
        return out
    v4 = head[3] >= 4
    size = _syncsafe(head[6:10])
    data = fh.read(min(size, 2 * 1024 * 1024))
    i = 0
    if head[5] & 0x40:                     # 扩展头
        i = _plain32(data[0:4]) + 4
    while i + 10 <= len(data):
        fid = data[i:i + 4]
        if fid == b"\x00\x00\x00\x00":
            break
        if not all(65 <= c <= 90 or 48 <= c <= 57 for c in fid):
            break
        sz = _syncsafe(data[i + 4:i + 8]) if v4 else _plain32(data[i + 4:i + 8])
        if sz <= 0 or i + 10 + sz > len(data):
            break
        body = data[i + 10:i + 10 + sz]
        if fid in (b"USLT", b"TIT2", b"TPE1", b"TALB") and len(body) > 4:
            enc = body[0]
            if fid == b"USLT":
                rest = body[4:]
                if enc in (1, 2):          # UTF-16: 跳过描述符(双 0 结尾)
                    j = 0
                    while j + 1 < len(rest):
                        if rest[j] == 0 and rest[j + 1] == 0:
                            j += 2
                            break
                        j += 2
                    out.append(_decode(rest[j:], enc))
                else:
                    k = rest.find(b"\x00")
                    out.append(_decode(rest[k + 1:] if k >= 0 else rest, enc))
            else:
                out.append(_decode(body[1:], enc))
        i += 10 + sz
    return out


FLAC_KEYS = (b"TITLE", b"ARTIST", b"ALBUM", b"LYRICS", b"UNSYNCEDLYRICS")


def read_flac_text(fh):
    """返回 flac 的 标题/歌手/专辑/歌词 文本列表."""
    out = []
    head = fh.read(4)
    if head != b"fLaC":
        return out
    while True:
        hdr = fh.read(4)
        if len(hdr) < 4:
            break
        last = hdr[0] & 0x80
        btype = hdr[0] & 0x7F
        blen = (hdr[1] << 16) | (hdr[2] << 8) | hdr[3]
        if btype == 4 and blen < 1024 * 1024:
            body = fh.read(blen)
            if len(body) >= 8:
                vlen = int.from_bytes(body[0:4], "little")
                pos = 4 + vlen
                if pos + 4 <= len(body):
                    n = int.from_bytes(body[pos:pos + 4], "little")
                    pos += 4
                    for _ in range(min(n, 4096)):
                        if pos + 4 > len(body):
                            break
                        elen = int.from_bytes(body[pos:pos + 4], "little")
                        pos += 4
                        if pos + elen > len(body):
                            break
                        entry = body[pos:pos + elen]
                        pos += elen
                        if b"=" not in entry:
                            continue
                        k, v = entry.split(b"=", 1)
                        if k.upper() in FLAC_KEYS:
                            out.append(v.decode("utf-8", "replace"))
        else:
            fh.seek(blen, io.SEEK_CUR)
        if last:
            break
    return out


# ───────────────────────── 扫描 ─────────────────────────

def feed(chars, text):
    chars.update(text)


def scan_stream(name, fh, chars):
    low = name.lower()
    try:
        if low.endswith(".mp3"):
            for t in read_mp3_text(fh):
                feed(chars, t)
        elif low.endswith(".flac"):
            for t in read_flac_text(fh):
                feed(chars, t)
    except Exception:
        pass


def scan_path(path, chars, stat):
    if os.path.isdir(path):
        for root, _dirs, files in os.walk(path):
            for fn in files:
                full = os.path.join(root, fn)
                low = fn.lower()
                if low.endswith((".mp3", ".flac")):
                    stat["audio"] += 1
                    feed(chars, os.path.splitext(fn)[0])
                    try:
                        with open(full, "rb") as fh:
                            scan_stream(fn, fh, chars)
                    except OSError:
                        stat["err"] += 1
                elif low.endswith(".lrc"):
                    stat["lrc"] += 1
                    try:
                        feed(chars, open(full, encoding="utf-8", errors="replace").read())
                    except OSError:
                        stat["err"] += 1
    elif path.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                low = info.filename.lower()
                if not low.endswith((".mp3", ".flac")):
                    continue
                stat["audio"] += 1
                base = os.path.basename(info.filename)
                feed(chars, os.path.splitext(base)[0])
                with z.open(info) as fh:
                    scan_stream(base, fh, chars)
    else:
        raise SystemExit("不认识的路径: %s" % path)


# ───────────────────────── 主流程 ─────────────────────────

def block_of(cp):
    for name, lo, hi in (
        ("CJK 基本区", 0x4E00, 0x9FFF), ("CJK 扩展A", 0x3400, 0x4DBF),
        ("CJK 扩展B+", 0x20000, 0x2FA1F), ("CJK 兼容汉字", 0xF900, 0xFAFF),
        ("CJK 部首", 0x2E80, 0x2FFF), ("CJK 标点", 0x3000, 0x303F),
        ("CJK 兼容单位", 0x3300, 0x33FF), ("全角", 0xFF00, 0xFFEF),
        ("假名", 0x3040, 0x30FF), ("韩文", 0xAC00, 0xD7A3),
        ("拉丁", 0x20, 0x24F), ("拉丁扩展附加", 0x1E00, 0x1EFF),
        ("希腊", 0x370, 0x3FF), ("西里尔", 0x400, 0x52F),
        ("标点/符号", 0x2000, 0x2BFF),
    ):
        if lo <= cp <= hi:
            return name
    return "其它"


def main(argv):
    font = DEFAULT_FONT
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == "--font" and i + 1 < len(argv):
            font = argv[i + 1]
            i += 2
        else:
            paths.append(argv[i])
            i += 1

    if not paths:
        print(__doc__)
        return 2
    if not os.path.exists(font):
        raise SystemExit("找不到字体文件: %s" % font)

    glyphs = load_glyphs(font)
    chars = set()
    stat = {"audio": 0, "lrc": 0, "err": 0}
    for p in paths:
        scan_path(p, chars, stat)

    ignorable = set()
    missing = []
    for c in chars:
        cp = ord(c)
        if cp in glyphs:
            continue
        import unicodedata
        cat = unicodedata.category(c)
        if cat in IGNORE_CATS:
            ignorable.add(cp)
        else:
            missing.append(c)

    missing.sort(key=ord)
    print("字体文件: %s" % font)
    print("字形总数: %d" % len(glyphs))
    print("扫描文件: %d 个音频, %d 个 .lrc%s"
          % (stat["audio"], stat["lrc"], (" (读取失败 %d)" % stat["err"]) if stat["err"] else ""))
    print("不同字符: %d" % len(chars))
    print("不可显示字符: %d %s" % (len(ignorable),
                                   " ".join("U+%04X" % cp for cp in sorted(ignorable)[:12])))

    if not missing:
        print("缺字      : 0  ✅ 全部可显示")
        return 0

    print("缺字      : %d  ❌" % len(missing))
    groups = {}
    for c in missing:
        groups.setdefault(block_of(ord(c)), []).append(c)
    for name in sorted(groups, key=lambda k: -len(groups[k])):
        s = "".join(groups[name])
        print("  %-12s %4d  %s" % (name, len(groups[name]), s[:80]))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
