# 生成 16x21 RGB565 小说(TXT)文件图标 (与 icon_file/icon_heart 参数一致, 可直接用于列表)
# 运行: C:\Python314\python.exe tools\gen_novel_icon.py [源图路径]
# 默认源图: E:\播放器\手表\代码\图标\文件\IMG_20260502_235932.png
import os
import sys
from PIL import Image

W, H = 16, 21                       # 与 icon_file/icon_heart 每帧一致
BG = (255, 255, 255)                # 白底 (列表也是白底)

SRC_DEFAULT = r"E:\播放器\手表\代码\图标\文件\IMG_20260502_235932.png"

OUT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "main", "resources", "icon_novel.c"))


def render(src_path):
    """把源图等比缩放并居中合成到 16x21 白底; 透明区域填白 (先大图缩放再缩小以获得平滑边缘)"""
    src = Image.open(src_path).convert("RGBA")

    r = min(W / src.width, H / src.height)
    nw = max(1, int(round(src.width * r)))
    nh = max(1, int(round(src.height * r)))
    glyph = src.resize((nw, nh), Image.LANCZOS)

    img = Image.new("RGB", (W, H), BG)
    img.paste(glyph, ((W - nw) // 2, (H - nh) // 2), glyph)
    return img


def rgb_to_rgb565(r, g, b):
    # LV_COLOR_16_SWAP=1: 存储时交换高低字节 (与 icon_file.c 一致), 否则颜色会偏
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return ((v & 0xFF) << 8) | (v >> 8)


def emit_c(img):
    px = img.load()
    vals = []
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y][:3]
            vals.append(rgb_to_rgb565(r, g, b))

    lines = []
    lines.append("/* 自动生成: 16x21 RGB565 小说文件图标 (tools/gen_novel_icon.py) */")
    lines.append("#include \"ui_res.h\"")
    lines.append("")
    lines.append("const uint16_t icon_novel[%d] = {" % (W * H))
    per_row = 10
    for i in range(0, len(vals), per_row):
        chunk = vals[i:i + per_row]
        lines.append("    " + ", ".join("0x%04X" % v for v in chunk) +
                     ("," if i + per_row < len(vals) else ""))
    lines.append("};")
    lines.append("")
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("Saved: %s (%d px)" % (OUT, len(vals)))


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else SRC_DEFAULT
    img = render(src)
    here = os.path.dirname(os.path.abspath(__file__))
    img.save(os.path.join(here, "novel_icon_preview.png"))
    print("Saved preview: tools/novel_icon_preview.png")
    emit_c(img)
