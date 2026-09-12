# -*- coding: utf-8 -*-
"""重绘 MiderHive 品牌图标：全出血圆角贴片（角部真透明、无白边距）+ 蜂巢三六边形 + 入口蓝点。

几何与 docs/assets/logo.svg 完全一致；产物：
  docs/assets/logo.png  256px 全出血透明 PNG（GUI 窗口图标）
  src/gui/icon.ico      多尺寸 ICO（exe 资源图标，app.rc 引用；MSI 快捷方式同用此文件）

为什么必须是多尺寸：桌面/开始菜单快捷方式按 32/48px 取图，资源管理器小图标视图取 16px。
单帧 256px 的 ICO 只能让 shell 自己缩放——小尺寸发虚，个别 shell 路径还会回退成通用图标。
这里对每个尺寸**按几何重绘**（不是从 256 缩），16~64 用 BMP(DIB) 帧、128/256 用 PNG 帧
（BMP 帧是所有 Windows 版本都认的基座，PNG 帧省体积且 Vista+ 支持）。

不要用无头浏览器截图 SVG 生成图标——透明边距会被渲染成白底，任务栏里出现白边。

用法：在仓库根目录执行  python scripts/gen_icon.py   （需要 Pillow）
"""
import struct
from pathlib import Path

from PIL import Image, ImageDraw

if not Path("src/gui/app.rc").is_file():
    raise SystemExit("请在仓库根目录执行：python scripts/gen_icon.py")

PNG_PATH = Path("docs/assets/logo.png")
ICO_PATH = Path("src/gui/icon.ico")

S = 256  # 设计基准尺寸（logo.svg 的 viewBox）
# 每个尺寸的输出格式：BMP 基座覆盖 shell 的小图标路径，大尺寸用 PNG 省体积
SIZES = [(16, "bmp"), (24, "bmp"), (32, "bmp"), (48, "bmp"), (64, "bmp"),
         (128, "png"), (256, "png")]

PATCH = (0x1E, 0x1E, 0x1E, 255)   # 贴片底色
AMBER = (0xF5, 0x9E, 0x0B, 255)   # 蜂巢描边
BLUE = (0x0E, 0xA5, 0xE9, 255)    # 蜂巢入口点
HEXES = [
    [(128, 56), (162.6, 76), (162.6, 116), (128, 136), (93.4, 116), (93.4, 76)],
    [(93.4, 116), (128, 136), (128, 176), (93.4, 196), (58.8, 176), (58.8, 136)],
    [(162.6, 116), (197.2, 136), (197.2, 176), (162.6, 196), (128, 176), (128, 136)],
]


def render(size):
    """按几何在目标尺寸重绘（非缩放），保证小尺寸边缘锐利。"""
    k = size / float(S)
    # 小尺寸补偿：16~32px 时按比例描边只有 0.6~1.2px，发灰读不出形状，
    # 因此小尺寸适度加粗（1.0px 下限 + 轻微放大），大尺寸严格按品牌比例。
    boost = 1.0 if size >= 48 else (1.35 if size <= 24 else 1.2)
    stroke = max(1, int(round(10 * k * boost)))
    dot_r = max(1, int(round(12 * k * (1.0 if size >= 48 else 1.25))))
    corner = max(2, int(round(52 * k)))

    big = size * 4  # 4x 超采样后缩小：多边形斜边与圆点的锯齿在这里消掉
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, big - 1, big - 1], radius=corner * 4, fill=PATCH)

    # 16/24px 用简化字形：三个六边形在该尺寸互相糊成一团，只留顶部六边形
    # （放大 1.5 倍居中）+ 入口蓝点，保证"看得清"优先于"细节全"。
    if size <= 24:
        poly = [(128 + (x - 128) * 1.5, 96 + (y - 96) * 1.5) for (x, y) in HEXES[0]]
        pts = [(x * k * 4, y * k * 4) for (x, y) in poly]
        d.line(pts + [pts[0]], fill=AMBER, width=stroke * 4, joint="curve")
        for (x, y) in pts:
            r = stroke * 2
            d.ellipse([x - r, y - r, x + r, y + r], fill=AMBER)
        cx, cy = 128 * k * 4, 96 * k * 4
        r = dot_r * 4
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=BLUE)
        return img.resize((size, size), Image.LANCZOS)

    for poly in HEXES:
        pts = [(x * k * 4, y * k * 4) for (x, y) in poly]
        # 每个六边形独立绘制（跨多边形连线会出现假对角线）；
        # 顶点补圆点等效 SVG 的 stroke-linejoin/linecap="round"
        d.line(pts + [pts[0]], fill=AMBER, width=stroke * 4, joint="curve")
        for (x, y) in pts:
            r = stroke * 2
            d.ellipse([x - r, y - r, x + r, y + r], fill=AMBER)

    cx, cy = 128 * k * 4, 96 * k * 4
    r = dot_r * 4
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=BLUE)
    return img.resize((size, size), Image.LANCZOS)


# ---- 256px 主图（GUI 窗口图标 / 品牌 PNG）----
master = render(S)
master.save(PNG_PATH)


def self_check(img, label):
    px = img.load()
    n = img.size[0]
    # 圆角处的抗锯齿必然让角像素带一点 alpha（16/24/32px 尤其明显），所以只要求
    # "基本透明"；真正要守住的是"贴片内不得有近白不透明像素"——白边就是它。
    corner_alpha = max(px[x, y][3] for x, y in
                       [(0, 0), (n - 1, 0), (0, n - 1), (n - 1, n - 1)])
    corners_ok = corner_alpha <= 64
    near_white = sum(1 for y in range(n) for x in range(n)
                     if px[x, y][3] > 200 and min(px[x, y][:3]) > 230)
    print("  %-8s corner_alpha=%-4d near_white=%d" % (label, corner_alpha, near_white))
    return corners_ok and near_white == 0


print("self-check:")
ok = self_check(master, "256px")
frames = []
for size, kind in SIZES:
    img = master if size == S else render(size)
    ok = self_check(img, "%dpx" % size) and ok
    frames.append((size, kind, img))
if not ok:
    raise SystemExit("icon self-check FAILED: white edge or opaque corners")


def bmp_frame(img):
    """32bpp BGRA DIB 帧（BITMAPINFOHEADER + 自下而上像素 + 全零 AND 掩码）。"""
    n = img.size[0]
    px = img.load()
    rows = []
    for y in range(n - 1, -1, -1):  # DIB 自下而上
        row = bytearray()
        for x in range(n):
            r, g, b, a = px[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    pixels = b"".join(rows)
    mask_row = ((n + 31) // 32) * 4  # 1bpp 掩码按 4 字节对齐
    mask = b"\x00" * (mask_row * n)
    header = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0,
                         len(pixels) + len(mask), 0, 0, 0, 0)
    return header + pixels + mask


entries, blobs, offset = [], [], 6 + 16 * len(frames)
for size, kind, img in frames:
    if kind == "bmp":
        data = bmp_frame(img)
    else:
        import io
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        data = buf.getvalue()
    entries.append(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32,
                               len(data), offset))
    blobs.append(data)
    offset += len(data)

ICO_PATH.write_bytes(struct.pack("<HHH", 0, 1, len(frames)) + b"".join(entries) + b"".join(blobs))
print("icon.ico: %d frames, %d bytes -> %s" % (len(frames), ICO_PATH.stat().st_size, ICO_PATH))
