# 生成 16x21 RGB565 粉色爱心图标 (与 icon_file 参数一致, 可直接替换)
# 运行: C:\Python314\python.exe tools\gen_heart_icon.py
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 16, 21                       # 与 icon_file 每帧一致
BG = (255, 255, 255)                # 白底 (列表也是白底)
PINK = (255, 92, 138)               # 粉色爱心
FONT = r"D:\Fonts\seguisym.ttf"     # 含 U+2665 的字体
CH = "\u2665"

OUT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "main", "resources", "icon_heart.c"))


def render_glyph():
    """把爱心按最大尺寸等比缩放进 16x21, 居中; 先大字号渲染再缩小以获得平滑边缘"""
    font = ImageFont.truetype(FONT, 48)
    tmp = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
    d = ImageDraw.Draw(tmp)
    d.text((48, 48), CH, font=font, fill=PINK + (255,), anchor="mm")
    bbox = tmp.getbbox()
    if not bbox:
        raise RuntimeError("heart glyph not rendered")
    glyph = tmp.crop(bbox)

    r = min(W / glyph.width, H / glyph.height)
    nw, nh = max(1, int(round(glyph.width * r))), max(1, int(round(glyph.height * r)))
    glyph = glyph.resize((nw, nh), Image.LANCZOS)

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
    lines.append("/* 自动生成: 16x21 RGB565 粉色爱心 (tools/gen_heart_icon.py) */")
    lines.append("#include \"ui_res.h\"")
    lines.append("")
    lines.append("const uint16_t icon_heart[%d] = {" % (W * H))
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


img = render_glyph()
here = os.path.dirname(os.path.abspath(__file__))
img.save(os.path.join(here, "heart_icon_preview.png"))
print("Saved preview: tools/heart_icon_preview.png")
emit_c(img)
