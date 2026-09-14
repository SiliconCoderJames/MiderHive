#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 GitHub 仓库社交预览图 docs/assets/social-preview.png（1280x640）。

GitHub 的社交预览只能在网页 Settings → Social preview 手动上传，本脚本负责产出图片本身：
品牌名必须与产品一致（旧图仍写着改名前 的 "AgentHive"），视觉沿用应用图标与深色主题——
蜂巢纹理背景 + 琥珀色品牌主色 + 六边形蜂巢标识 + 蓝色节点。

用法：python scripts/make_social_preview.py
"""

import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "assets", "social-preview.png")

W, H = 1280, 640
BG = (10, 14, 21)
AMBER = (245, 158, 11)
BLUE = (56, 160, 240)
WHITE = (245, 247, 250)
MUTED = (154, 164, 178)
FAINT = (107, 114, 128)
HONEY = (245, 158, 11, 22)

FONT_BOLD = r"C:\Windows\Fonts\segoeuib.ttf"
FONT_REG = r"C:\Windows\Fonts\segoeui.ttf"
FONT_MONO = r"C:\Windows\Fonts\consola.ttf"


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.truetype(FONT_REG, size)


def hexagon(cx, cy, r):
    """平顶六边形（左右尖、上下平边），与应用图标一致。"""
    return [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
            for a in (0, 60, 120, 180, 240, 300)]


def honeycomb(draw, r=44, color=HONEY, width=2):
    """铺满画布的蜂巢纹理（交错列平铺）。"""
    dx = 1.5 * r
    dy = math.sqrt(3) * r
    cols = int(W / dx) + 3
    rows = int(H / dy) + 3
    for c in range(-1, cols):
        for row in range(-1, rows):
            cx = c * dx
            cy = row * dy + (dy / 2 if c % 2 else 0)
            draw.polygon(hexagon(cx, cy, r), outline=color, width=width)


def mark(draw, cx, cy, r, width=9):
    """品牌标识：三只共边的蜂巢 + 顶部蓝色节点（几何与应用图标一致）。

    六边形为平顶朝向（顶点在 0°/60°/…），相邻共边中心距：横向 √3·R/2、纵向 1.5·R。
    """
    R = r * 0.66
    top = (cx, cy - 0.66 * r)
    left = (cx - 0.866 * R * 0.97, cy + 0.33 * r)
    right = (cx + 0.866 * R * 0.97, cy + 0.33 * r)
    for hx, hy in (top, left, right):
        draw.polygon(hexagon(hx, hy, R), outline=AMBER, width=width)
    node = R * 0.24
    draw.ellipse([top[0] - node, top[1] - node, top[0] + node, top[1] + node], fill=BLUE)


def main():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img, "RGBA")
    honeycomb(d)

    # 左侧琥珀色竖条（品牌强调）
    d.rectangle([62, 252, 70, 448], fill=AMBER)

    f_title = font(FONT_BOLD, 96)
    f_tag = font(FONT_REG, 31)
    f_tech = font(FONT_MONO, 21)

    d.text((96, 278), "MiderHive", font=f_title, fill=WHITE)
    d.text((100, 392), "Local-first collaboration hub for AI agents", font=f_tag, fill=MUTED)
    d.text((100, 440), "C++20  ·  Qt 6  ·  SQLite  ·  localhost-only  ·  MIT", font=f_tech, fill=FAINT)

    mark(d, 1044, 336, 104)
    img.save(OUT)
    print(f"saved {OUT}  {img.size}  {os.path.getsize(OUT) // 1024}KB")


if __name__ == "__main__":
    sys.exit(main())
