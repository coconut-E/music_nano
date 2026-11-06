#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 lv_font_conv 生成的巨型字库 .c 拆成「瘦 .c + 独立二进制」。

背景
----
app 要压进 2.19MB 的 OTA 槽, 而字库存量 3.33MB:
    glyph_bitmap 2,803,930 B  (2.80MB)
    glyph_dsc      462,032 B  (28,877 条 x 16B)
占了 app 的 66%。把这两块搬进独立的 font 分区, app 侧只剩
cmaps / kern / unicode_list 这些小表 (二进制约 67KB)。

注意 ESP32 的 DROM 窗口只有 4MB (0x3F400000-0x3F800000), 现在 .rodata
占了 3.96MB。拆完后 .rodata 约 620KB, 窗口剩 3.40MB, 字库 mmap 需要
50 页 = 3.125MB —— 余量只有 4 页, 所以这两块必须原样搬走, 不能在 app
里留任何副本。

产物
----
  main/resources/font16_full.c        原始文件 (输入, 保留)
  main/resources/lv_font_global_16.c  瘦身版 (本脚本生成)
  main/resources/font16.bin           字库分区镜像 (本脚本生成)

font16.bin 布局 (全部小端)
----
  0x00  magic       字节 'F','N','T','1'
  0x04  version     1
  0x08  total_size  整个文件大小
  0x0C  crc32       payload 的 CRC32 (标准 zlib 多项式)
  0x10  dsc_off     64
  0x14  dsc_len     glyph_count * 16
  0x18  bmp_off     64 + dsc_len
  0x1C  bmp_len
  0x20  glyph_count
  0x24  bpp         4
  0x28  reserved[6] 补到 64 字节
  0x40  payload     glyph_dsc[] 紧跟 glyph_bitmap[]

glyph_dsc 每条 16 字节, 与固件里的 struct 一致 (LV_FONT_FMT_TXT_LARGE=1):
  uint32 bitmap_index; uint32 adv_w; uint16 box_w; uint16 box_h;
  int16 ofs_x; int16 ofs_y;                    => '<IIHHhh'
"""

import os
import re
import struct
import sys
import zlib

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RES = os.path.join(ROOT, "main", "resources")

FULL_C = os.path.join(RES, "font16_full.c")
SLIM_C = os.path.join(RES, "lv_font_global_16.c")
BIN = os.path.join(RES, "font16.bin")

MAGIC_BYTES = b"FNT1"
VERSION = 1
HDR_SIZE = 64
DSC_ENTRY_SIZE = 16
BPP = 4


# --------------------------------------------------------------------------
# CRC32: 标准 zlib 多项式 (反射 0xEDB88320)。
# 固件侧用同一套查表实现, 保证两边算出来的值一致。
# --------------------------------------------------------------------------
def _make_table():
    tbl = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        tbl.append(c)
    return tbl


_CRC_TABLE = _make_table()


def crc32_std(data):
    """等价于 zlib.crc32(data) & 0xFFFFFFFF。"""
    crc = 0xFFFFFFFF
    for b in data:
        crc = _CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def die(msg):
    print("错误: " + msg, file=sys.stderr)
    sys.exit(1)


# --------------------------------------------------------------------------
# 定位
# --------------------------------------------------------------------------
RE_BITMAP = re.compile(r"^static\s+LV_ATTRIBUTE_LARGE_CONST\s+const\s+uint8_t\s+glyph_bitmap\[\]")
RE_DSC = re.compile(r"^static\s+const\s+lv_font_fmt_txt_glyph_dsc_t\s+glyph_dsc\[\]")
RE_FONT_DSC = re.compile(r"^\s*(static\s+)?(const\s+)?lv_font_fmt_txt_dsc_t\s+font_dsc\s*=")
RE_IF_LVGL8 = re.compile(r"^#if\s+LVGL_VERSION_MAJOR\s*>=\s*8")
RE_ACC_END = re.compile(r"^#endif\s*/\*#if LV_FONT_GLOBAL_16\*/")


def find_lines(lines, regex, what):
    for i, ln in enumerate(lines):
        if regex.match(ln):
            return i
    die("找不到 %s (正则 %s)" % (what, regex.pattern))


def find_close_brace(lines, start):
    for i in range(start + 1, len(lines)):
        if lines[i].strip() == "};":
            return i
    die("第 %d 行开始的花括号没有闭合" % (start + 1))


# --------------------------------------------------------------------------
# 生成瘦 .c
# --------------------------------------------------------------------------
def build_slim(lines, i_bmp, i_dsc_end, i_fdsc, i_fdsc_end):
    blk = "".join(lines[i_fdsc:i_fdsc_end + 1])

    def gv(name):
        m = re.search(r"\." + name + r"\s*=\s*(-?\d+)", blk)
        if not m:
            die("font_dsc 里找不到 .%s" % name)
        return m.group(1)

    out = []

    # 1) 文件头 -> 常量数组之前 (原文件自带的 BITMAPS 注释块保留)
    out.extend(lines[:i_bmp])
    out.append("/* glyph_bitmap[] 与 glyph_dsc[] 已移到独立分区 (main/resources/font16.bin),\n")
    out.append(" * 开机由 font_part_init() mmap 进来后填进 font_dsc。\n")
    out.append(" * 这里留空, 不要试图把数据补回来: ESP32 的 DROM 窗口只有 4MB,\n")
    out.append(" * 装不下 3.2MB 字库 + app 自身 rodata。*/\n")
    out.append("\n")

    # 2) CHARACTER MAPPING 段 (cmaps / kern / unicode_list ...) 原样保留
    out.extend(lines[i_dsc_end + 1:i_fdsc])

    # 3) font_dsc: 去 const/static, 两个指针留空, 标量字段从原文件取
    out.append("lv_font_fmt_txt_dsc_t font_dsc = {\n")
    out.append("    .glyph_bitmap = NULL,          /* font_part_init() 填 */\n")
    out.append("    .glyph_dsc    = NULL,          /* font_part_init() 填 */\n")
    out.append("    .cmaps = cmaps,\n")
    out.append("    .kern_dsc = &kern_classes,\n")
    out.append("    .kern_scale = %s,\n" % gv("kern_scale"))
    out.append("    .cmap_num = %s,\n" % gv("cmap_num"))
    out.append("    .bpp = %s,\n" % gv("bpp"))
    out.append("    .kern_classes = %s,\n" % gv("kern_classes"))
    out.append("    .bitmap_format = %s,\n" % gv("bitmap_format"))
    out.append("#if LVGL_VERSION_MAJOR == 8\n")
    out.append("    .cache = &cache\n")
    out.append("#endif\n")
    out.append("};\n")

    # 4) 公有字体 lv_font_global_16 + 尾部检查
    out.extend(lines[i_fdsc_end + 1:])

    # 5) 在最后一个 #endif 之前插入取址函数
    tail_idx = len(out)
    for j in range(len(out) - 1, -1, -1):
        if RE_ACC_END.match(out[j]):
            tail_idx = j
            break
    out[tail_idx:tail_idx] = [
        "\n",
        "/* 取 font_dsc 的地址, 供 main/resources/font_part.c 在开机时填指针。\n",
        " * 只导出这个函数, 不把 font_dsc 变成全局符号, 避免重名。*/\n",
        "lv_font_fmt_txt_dsc_t *font_global_16_dsc(void)\n",
        "{\n",
        "    return &font_dsc;\n",
        "}\n",
        "\n",
    ]
    return "".join(out)


# --------------------------------------------------------------------------
# 生成 font16.bin
# --------------------------------------------------------------------------
RE_HEXBYTE = re.compile(r"0x([0-9A-Fa-f]{2})")
RE_DSC_ITEM = re.compile(
    r"\{\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),"
    r"\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\}"
)


def build_bin(lines, i_bmp, i_bmp_end, i_dsc, i_dsc_end):
    bitmap_text = "".join(lines[i_bmp + 1:i_bmp_end])
    if "/*" in bitmap_text:
        bitmap_text = re.sub(r"/\*.*?\*/", "", bitmap_text, flags=re.S)
    bitmap = bytes(int(h, 16) for h in RE_HEXBYTE.findall(bitmap_text))
    if not bitmap:
        die("glyph_bitmap 解析出 0 字节")

    dsc_items = RE_DSC_ITEM.findall("".join(lines[i_dsc + 1:i_dsc_end]))
    if not dsc_items:
        die("glyph_dsc 解析出 0 条")

    dsc = bytearray()
    last_idx = -1
    for it in dsc_items:
        bi, aw, bw, bh, ox, oy = (int(x) for x in it)
        if bi < last_idx:
            die("bitmap_index 不是递增的: %d -> %d" % (last_idx, bi))
        if bi > len(bitmap):
            die("bitmap_index %d 超出 bitmap 长度 %d" % (bi, len(bitmap)))
        last_idx = bi
        dsc += struct.pack("<IIHHhh", bi, aw, bw, bh, ox, oy)

    payload = bytes(dsc) + bitmap
    total = HDR_SIZE + len(payload)
    crc = crc32_std(payload)
    hdr = struct.pack(
        "<10I6I", int.from_bytes(MAGIC_BYTES, "little"), VERSION, total, crc,
        HDR_SIZE, len(dsc), HDR_SIZE + len(dsc), len(bitmap),
        len(dsc_items), BPP, 0, 0, 0, 0, 0, 0,
    )
    if len(hdr) != HDR_SIZE:
        die("header 长度 %d != %d" % (len(hdr), HDR_SIZE))
    return hdr + payload, bitmap, dsc, len(dsc_items)


# --------------------------------------------------------------------------
def main():
    if not os.path.exists(FULL_C):
        die("找不到输入 %s\n(应该是原来那份 22MB 的生成文件)" % FULL_C)

    print("读 %s ..." % os.path.relpath(FULL_C, ROOT))
    with open(FULL_C, "r", encoding="utf-8", newline="") as f:
        lines = f.readlines()
    print("  %d 行, %.1f MB" % (len(lines), os.path.getsize(FULL_C) / 1048576.0))

    i_bmp = find_lines(lines, RE_BITMAP, "glyph_bitmap[]")
    i_bmp_end = find_close_brace(lines, i_bmp)
    i_dsc = find_lines(lines, RE_DSC, "glyph_dsc[]")
    i_dsc_end = find_close_brace(lines, i_dsc)

    i_fdsc = find_lines(lines, RE_FONT_DSC, "font_dsc")
    if i_fdsc > 0 and RE_IF_LVGL8.match(lines[i_fdsc - 1]):
        i_fdsc -= 1
    i_fdsc_end = find_close_brace(lines, i_fdsc)

    print("  glyph_bitmap  %6d .. %6d" % (i_bmp + 1, i_bmp_end + 1))
    print("  glyph_dsc     %6d .. %6d" % (i_dsc + 1, i_dsc_end + 1))
    print("  font_dsc      %6d .. %6d" % (i_fdsc + 1, i_fdsc_end + 1))

    slim = build_slim(lines, i_bmp, i_dsc_end, i_fdsc, i_fdsc_end)
    binary, bitmap, dsc, ncount = build_bin(lines, i_bmp, i_bmp_end, i_dsc, i_dsc_end)

    if ncount * DSC_ENTRY_SIZE != len(dsc):
        die("glyph_dsc 长度不符")

    with open(SLIM_C, "w", encoding="utf-8", newline="") as f:
        f.write(slim)
    with open(BIN, "wb") as f:
        f.write(binary)

    # ---- 自检 ----
    ok = True

    def chk(cond, msg):
        nonlocal ok
        if not cond:
            print("  自检失败: " + msg)
            ok = False

    chk("glyph_bitmap[] = {" not in slim, "瘦 .c 里还残留 glyph_bitmap[]")
    chk("glyph_dsc[] = {" not in slim, "瘦 .c 里还残留 glyph_dsc[]")
    chk(".glyph_bitmap = NULL" in slim and ".glyph_dsc    = NULL" in slim,
        "font_dsc 的指针没有置空")
    chk("lv_font_fmt_txt_dsc_t font_dsc = {" in slim, "font_dsc 不是可写定义")
    chk("font_global_16_dsc" in slim, "没有导出取址函数")
    chk("const lv_font_t lv_font_global_16 = {" in slim, "公有字体被改动")
    chk(binary[:4] == MAGIC_BYTES, "magic 不对: %r" % binary[:4])
    chk(struct.unpack_from("<I", binary, 0x0C)[0] == crc32_std(binary[HDR_SIZE:]),
        "CRC 回读不一致")
    got = struct.unpack_from("<6I", binary, 0x10)
    want = (HDR_SIZE, len(dsc), HDR_SIZE + len(dsc), len(bitmap), ncount, BPP)
    chk(got == want, "头字段 %s != %s" % (got, want))
    first = struct.unpack_from("<IIHHhh", binary, HDR_SIZE)
    last = struct.unpack_from("<IIHHhh", binary, HDR_SIZE + (ncount - 1) * DSC_ENTRY_SIZE)
    chk(first[0] == 0, "首条 bitmap_index != 0")
    chk(last[0] < len(bitmap), "末条 bitmap_index 越界")
    chk(slim.count("\n") < 12000, "瘦 .c 行数异常: %d" % slim.count("\n"))

    print()
    print("  %-32s %10d B  %7.2f MB  %6d 行" % (
        "main/resources/lv_font_global_16.c",
        len(slim.encode("utf-8")), len(slim.encode("utf-8")) / 1048576.0,
        slim.count("\n")))
    print("  %-32s %10d B  %7.2f MB" % (
        "main/resources/font16.bin", len(binary), len(binary) / 1048576.0))
    print("      header    %d B" % HDR_SIZE)
    print("      glyph_dsc %d 条 x %d B = %d B" % (ncount, DSC_ENTRY_SIZE, len(dsc)))
    print("      bitmap    %d B" % len(bitmap))
    print("      crc32     0x%08X" % struct.unpack_from("<I", binary, 0x0C)[0])
    print("      bin 占分区 3.25MB 的 %.1f%%" % (len(binary) / (0x340000 / 100.0)))
    print()
    if not ok:
        die("自检未通过")
    print("自检通过。")


if __name__ == "__main__":
    main()
