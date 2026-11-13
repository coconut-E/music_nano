#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 font16.bin 里的字渲染成 PNG (按它自己的位深), 用来肉眼检查画质.

用法:
    python tools/font_preview.py                          # 默认 5x, 三行示例
    python tools/font_preview.py out.png "文本" 8          # 指定输出/文本/倍数
    python tools/font_preview.py out.png "文本" 8 2        # 第4个参数: 量化到 2bpp 再画

不加量化参数时按字库自己的 bpp 画; 加 `2` 可以把 4bpp 字库模拟成 2bpp 看效果.
"""
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "main", "resources", "font16.bin")
SLIM_C = os.path.join(ROOT, "main", "resources", "lv_font_global_16.c")

TEXT = ["晴天 - 周杰伦", "雨纷纷 旧故里草木深", "正在播放 03/12"]
ZOOM = 5
MARGIN = 10
GAP = 28
LINE_H = 24


# ── 字符 -> glyph id (解析瘦身 .c 里的 cmaps) ───────────────────────────
def parse_cmaps():
    s = open(SLIM_C, encoding="utf-8", errors="replace").read()
    arrays = {}
    for m in re.finditer(r"static const (uint8_t|uint16_t) (\w+)\[\] = \{(.*?)\};", s, re.S):
        vals = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(3))]
        arrays[m.group(2)] = vals

    i = s.find("cmaps[]")
    blk = s[i:s.find("};", i)]
    ent = re.findall(
        r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*"
        r"\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), \.list_length = (\d+),\s*"
        r"\.type = (\w+)", blk)
    out = []
    for a, b, c, ul, ofs, ln, t in ent:
        out.append(dict(start=int(a), length=int(b), gid=int(c),
                        ul=None if ul == "NULL" else ul,
                        ofs=None if ofs == "NULL" else ofs,
                        n=int(ln), type=t))
    return out, arrays


CMAPS, ARRAYS = parse_cmaps()


def glyph_of(ch):
    u = ord(ch)
    for c in CMAPS:
        if not (c["start"] <= u < c["start"] + max(c["length"], c["n"])):
            continue
        if c["type"].endswith("TINY"):
            if u < c["start"] + c["length"]:
                return c["gid"] + (u - c["start"])
            continue
        if c["ul"]:
            arr = ARRAYS.get(c["ul"], [])
            try:
                k = arr.index(u - c["start"])
            except ValueError:
                return 0
        else:
            k = u - c["start"]
        if c["ofs"]:
            o = ARRAYS.get(c["ofs"], [])
            return c["gid"] + (o[k] if k < len(o) else 0)
        return c["gid"] + k
    return 0


# ── LVGL 的 RLE 解码 (与 lv_font_fmt_txt.c 的 rle_next 一一对应) ──────────
class RLE:
    def __init__(self, d, bpp):
        self.d = d
        self.bpp = bpp
        self.rdp = 0
        self.prev = 0
        self.cnt = 0
        self.state = 0

    def g(self, pos, n):
        bp = pos >> 3
        off = pos & 7
        if off + n >= 8:
            v = (self.d[bp] << 8) + self.d[bp + 1]
            return (v >> (16 - off - n)) & ((1 << n) - 1)
        return (self.d[bp] >> (8 - off - n)) & ((1 << n) - 1)

    def nxt(self):
        bpp = self.bpp
        if self.state == 0:
            ret = self.g(self.rdp, bpp)
            if self.rdp != 0 and self.prev == ret:
                self.cnt = 0
                self.state = 1
            self.prev = ret
            self.rdp += bpp
            return ret
        if self.state == 1:
            v = self.g(self.rdp, 1)
            self.cnt += 1
            self.rdp += 1
            if v == 1:
                ret = self.prev
                if self.cnt == 11:
                    self.cnt = self.g(self.rdp, 6)
                    self.rdp += 6
                    if self.cnt:
                        self.state = 2
                    else:
                        ret = self.g(self.rdp, bpp)
                        self.prev = ret
                        self.rdp += bpp
                        self.state = 0
                return ret
            ret = self.g(self.rdp, bpp)
            self.prev = ret
            self.rdp += bpp
            self.state = 0
            return ret
        ret = self.prev
        self.cnt -= 1
        if self.cnt == 0:
            ret = self.g(self.rdp, bpp)
            self.prev = ret
            self.rdp += bpp
            self.state = 0
        return ret


B = open(BIN, "rb").read()
(_, _, _, _, DSC_OFF, DSC_LEN, BMP_OFF, BMP_LEN, GLYPHS, BPP) = struct.unpack_from("<10I", B, 0)
BM = B[BMP_OFF:BMP_OFF + BMP_LEN]
MAXV = (1 << BPP) - 1


def glyph_bitmap(gid):
    bi, adv, w, h, ox, oy = struct.unpack_from("<IIHHhh", B, DSC_OFF + 16 * gid)
    if w == 0 or h == 0:
        return None
    r = RLE(BM[bi:], BPP)
    prev = [r.nxt() for _ in range(w)]
    px = [prev[:]]
    for _ in range(1, h):
        cur = [r.nxt() for _ in range(w)]
        cur = [cur[x] ^ prev[x] for x in range(w)]
        px.append(cur)
        prev = cur
    return w, h, adv, ox, oy, px


def text_width(s):
    w = 0.0
    for ch in s:
        g = glyph_of(ch)
        bm = glyph_bitmap(g) if g else None
        w += (bm[2] / 16.0) if bm else 16.0
    return w


def draw_text(buf, W, H, s, x0, baseline, quant=None):
    pen = float(x0)
    for ch in s:
        g = glyph_of(ch)
        bm = glyph_bitmap(g) if g else None
        if not bm:
            pen += 16.0
            continue
        w, h, adv, ox, oy, px = bm
        ipen = int(round(pen))
        top = baseline - oy - h
        for yy in range(h):
            for xx in range(w):
                v = px[yy][xx]
                if quant and quant != BPP:
                    m = (1 << quant) - 1
                    v = min(m, (v * m + MAXV // 2) // MAXV)
                    v = v * MAXV // m
                if not v:
                    continue
                X = ipen + ox + xx
                Y = top + yy
                if 0 <= X < W and 0 <= Y < H:
                    new = 255 - v * 255 // MAXV
                    old = buf[Y * W + X]
                    buf[Y * W + X] = new if new < old else old
        pen += adv / 16.0


def main():
    global TEXT, ZOOM
    args = list(sys.argv[1:])
    out = os.path.join(ROOT, "build", "font_preview.png")
    if args and args[0].endswith(".png"):
        out = args.pop(0)
    if args:
        TEXT = [args.pop(0)]
    if args:
        ZOOM = int(args.pop(0))
    quant = int(args.pop(0)) if args else None

    tw = int(max(text_width(s) for s in TEXT) + 0.999)
    col = tw + 12
    W = MARGIN * 2 + col * 2 + GAP
    H = MARGIN * 2 + 18 + LINE_H * len(TEXT)

    buf = [255] * (W * H)
    panel(buf, W, H, TEXT, MARGIN, None)
    if quant:
        panel(buf, W, H, TEXT, MARGIN + col + GAP, quant)
        xs = MARGIN + col + GAP // 2
        for y in range(MARGIN + 16, H - MARGIN):
            buf[y * W + xs] = 210

    draw_text(buf, W, H, "%dbpp" % BPP, MARGIN, MARGIN + 14, None)
    if quant:
        draw_text(buf, W, H, "%dbpp" % quant, MARGIN + col + GAP, MARGIN + 14, quant)

    big = [255] * (W * ZOOM * H * ZOOM)
    for y in range(H * ZOOM):
        row = (y // ZOOM) * W
        for x in range(W * ZOOM):
            big[y * (W * ZOOM) + x] = buf[row + (x // ZOOM)]

    write_png(out, W * ZOOM, H * ZOOM, big)
    missing = {ch for s in TEXT for ch in s if not glyph_of(ch)}
    print("written", out, W * ZOOM, "x", H * ZOOM, "bpp", BPP,
          "quant", quant, "missing:", missing or "none")


def panel(buf, W, H, lines, x0, quant):
    for i, s in enumerate(lines):
        draw_text(buf, W, H, s, x0, MARGIN + 18 + i * LINE_H + 16, quant)


def write_png(path, w, h, gray):
    raw = b"".join(b"\x00" + bytes(gray[y * w:(y + 1) * w]) for y in range(h))

    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d +
                struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


if __name__ == "__main__":
    main()
