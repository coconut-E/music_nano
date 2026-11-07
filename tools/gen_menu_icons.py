# 生成主菜单三张图标 (88x88 RGB565): 音乐 / 小说 / 设置
#
# 源图是"白底 + 圆形徽章"的方图, 直接转 RGB565 会在纯黑菜单上留下白色方角, 所以:
#   1) 裁到非白内容的包围盒 (圆形徽章的外接方框), 再补成正方形, 避免拉伸变形
#   2) 等比缩放到 88x88
#   3) 套圆形 alpha 蒙版 (4 倍超采样后缩小, 边缘抗锯齿), 顺手把源图白边裁掉 1px
#   4) 合成到菜单底色 0x050505, 方角自然融进背景
#
# 输出: main/resources/icon_menu_{music,novel,settings}.c
# 预览: tools/menu_icon_preview.png (2 倍放大, 三张并排, 背景同菜单底色)
#
# 运行: python tools/gen_menu_icons.py
import os
from PIL import Image, ImageDraw

W = H = 88                          # 图标边长 (与 ui_menu.c 的 ICON_W 一致)
BG = (0x05, 0x05, 0x05)             # 菜单底色 COLOR_BG, 与 ui_core.h 一致
MASK_INSET = 2                      # 圆形蒙版内缩像素 (裁掉源图白边)
SS = 4                              # 蒙版超采样倍数

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
RES  = os.path.join(ROOT, "main", "resources")

# (C 变量名, 源图文件名, 注释)
ICONS = [
    ("icon_menu_music",    "音乐.jpg", "音乐"),
    ("icon_menu_novel",    "小说.png", "小说"),
    ("icon_menu_settings", "设置.png", "设置"),
]

RESAMPLE = getattr(Image, "Resampling", Image).LANCZOS


def content_bbox(img):
    """非白内容的外接方框 (左, 上, 右, 下), 右/下为开区间; 全白则返回整幅"""
    w, h = img.size
    px = img.load()
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y][:3]
            if r < 235 or g < 235 or b < 235:      # 留出 JPEG 白噪声余量
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    if x1 < 0:
        return (0, 0, w, h)
    return (x0, y0, x1 + 1, y1 + 1)


def crop_square(img, box):
    """按外接方框取正方形 (长边为准, 居中), 贴边时向内回推"""
    x0, y0, x1, y1 = box
    side = max(x1 - x0, y1 - y0)
    side = min(side, img.width, img.height)
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    left = min(max(0, cx - side // 2), img.width  - side)
    top  = min(max(0, cy - side // 2), img.height - side)
    return img.crop((left, top, left + side, top + side))


def circle_mask():
    """88x88 圆形蒙版 (255=圆内), 4 倍超采样后缩小得到抗锯齿边"""
    m = Image.new("L", (W * SS, H * SS), 0)
    d = ImageDraw.Draw(m)
    d.ellipse((MASK_INSET * SS, MASK_INSET * SS,
               W * SS - 1 - MASK_INSET * SS, H * SS - 1 - MASK_INSET * SS), fill=255)
    return m.resize((W, H), RESAMPLE)


def render(src_path, mask):
    """源图 → 88x88, 圆形抠图, 合成到菜单底色"""
    img = Image.open(src_path).convert("RGB")
    img = crop_square(img, content_bbox(img))
    img = img.resize((W, H), RESAMPLE)

    out = Image.new("RGB", (W, H), BG)
    out.paste(img, (0, 0), mask)
    return out


def rgb_to_rgb565(r, g, b):
    # 与 icon_file / icon_heart / icon_novel 一致: 输出时交换高低字节,
    # 现有图标上屏颜色正常, 别改成不交换
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return ((v & 0xFF) << 8) | (v >> 8)


def emit_c(name, label, img):
    px = img.load()
    vals = []
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y][:3]
            vals.append(rgb_to_rgb565(r, g, b))

    path = os.path.join(RES, name + ".c")
    lines = []
    lines.append("/* 自动生成: %dx%d RGB565 主菜单%s图标 (tools/gen_menu_icons.py) */"
                 % (W, H, label))
    lines.append("#include \"ui_res.h\"")
    lines.append("")
    lines.append("const uint16_t %s[%d] = {" % (name, W * H))
    per_row = 10
    for i in range(0, len(vals), per_row):
        chunk = vals[i:i + per_row]
        lines.append("    " + ", ".join("0x%04X" % v for v in chunk) +
                     ("," if i + per_row < len(vals) else ""))
    lines.append("};")
    lines.append("")
    # 行尾用 CRLF: 仓库 core.autocrlf=true, 工作区的 .c 都是 CRLF
    with open(path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines))
    print("Saved: %s (%d px, %d bytes)" % (path, len(vals), len(vals) * 2))


def emit_preview(imgs):
    """三张并排 2 倍放大, 底色同菜单, 间隙 8px, 便于目视检查"""
    gap, pad, zoom = 8, 8, 2
    cw, ch = W * zoom, H * zoom
    pw = pad * 2 + cw * len(imgs) + gap * (len(imgs) - 1)
    ph = pad * 2 + ch
    prev = Image.new("RGB", (pw, ph), BG)
    for i, img in enumerate(imgs):
        prev.paste(img.resize((cw, ch), Image.NEAREST),
                   (pad + i * (cw + gap), pad))
    path = os.path.join(HERE, "menu_icon_preview.png")
    prev.save(path)
    print("Saved preview: %s" % path)


def main():
    mask = circle_mask()
    imgs = []
    for name, fname, label in ICONS:
        src = os.path.join(ROOT, "image", fname)
        if not os.path.exists(src):
            raise SystemExit("missing source: %s" % src)
        img = render(src, mask)
        emit_c(name, label, img)
        imgs.append(img)
    emit_preview(imgs)


if __name__ == "__main__":
    main()
