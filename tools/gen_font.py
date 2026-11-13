#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用 lv_font_conv 重新生成字库 C 文件: 参数直接从 font16_full.c 头部的 Opts 行抄.

为什么要有这个脚本
------------------
字库是 28,877 字形、十几 MB 的 C 文件, 生成命令行有 4,400 多字符 (一堆 -r 和
--symbols). 每次都手敲不现实, 而且很容易和当初那份参数不一致 —— 那样生成的
字库字形集合就变了, 排查起来非常费劲.

Opts 行是 lv_font_conv 自己写进输出文件头的, 所以"照着上一份产物重跑"是唯一
可靠的复现方式。

用法
----
    # 只打印解析出来的参数
    python tools/gen_font.py --list

    # 按原参数重跑一遍 4bpp (用来验证工具链版本没漂)
    python tools/gen_font.py --bpp 4 --out build\\gen4

    # 出 2bpp
    python tools/gen_font.py --bpp 2 --out build\\gen2
    # 然后把 build\\gen2\\lv_font_global_16.c 覆盖到 main/resources/font16_full.c,
    # 跑 tools/split_font.py 拆出 font16.bin + 瘦身 .c

工具链默认在 E:\\播放器\\取模工具链 (fonts/ 下要有 msyh_face0.ttf 等),
换机器用 --tool-dir 指定。
"""
import argparse
import os
import shlex
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "resources", "font16_full.c")
TOOL_DIR = r"E:\播放器\取模工具链"


def parse_opts(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        s = f.read(20000)
    i = s.find("Opts:")
    if i < 0:
        raise SystemExit("找不到 Opts 行")
    j = s.find("\n", i)
    return shlex.split(s[i + len("Opts:"):j].strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--bpp", type=int)
    ap.add_argument("--out")
    ap.add_argument("--tool-dir", default=TOOL_DIR)
    a = ap.parse_args()

    argv = parse_opts(SRC)

    if a.list:
        for k, v in enumerate(argv):
            # --symbols 那个参数是几万个生僻字, 控制台放不下 (GBK 还会报错)
            print(k, v if len(v) < 32 else v[:24] + "...(%d chars)" % len(v))
        return

    if not a.bpp or not a.out:
        raise SystemExit("需要 --bpp 和 --out")

    if "--bpp" in argv:
        argv[argv.index("--bpp") + 1] = str(a.bpp)

    outdir = os.path.abspath(a.out)
    os.makedirs(outdir, exist_ok=True)
    if "-o" in argv:
        argv[argv.index("-o") + 1] = os.path.join(outdir, "lv_font_global_16.c")

    tool = os.path.join(a.tool_dir, "lv_font_conv", "lv_font_conv.js")
    fonts_dir = os.path.join(a.tool_dir, "fonts")
    print("tool:", tool)
    print("cwd :", fonts_dir)
    print("bpp :", a.bpp, " out:", outdir)
    r = subprocess.run(["node", tool] + argv, cwd=fonts_dir)
    if r.returncode != 0:
        raise SystemExit("lv_font_conv 失败 (%d)" % r.returncode)
    p = os.path.join(outdir, "lv_font_global_16.c")
    print("ok:", p, os.path.getsize(p), "bytes")


if __name__ == "__main__":
    main()
